/* epoll.hpp -- v1.0
   Wraps an epoll instance */

#pragma once

#include "endpoint.hpp"
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <sys/epoll.h>

namespace fserv::detail {
    //! Implements epoll_ctl().
    //! @param epfd
    //!     Epoll file descriptor
    //! @param opcode
    //!     Operation to be performed
    //! @param sfd
    //!     Socket file descriptor
    //! @param events
    //!     Events to be monitored
    //! @param handler_ptr
    //!     Pointer to the handler
    //! @return
    //!     Result of the epoll_ctl call
    inline int ctl(int epfd,
                   int opcode,
                   int sfd,
                   unsigned events,
                   void* handler_ptr)
    {
        ::epoll_event event = {};
        event.events = events;
        event.data.ptr = handler_ptr;

        int ret = epoll_ctl(epfd, opcode, sfd, &event);
        return ret;
    }
} // namespace fserv::detail

namespace fserv {
    //! @class EpollWaiter
    /*! Encapsulates an epoll instance
     */
    template <typename SinkType, typename HandlerType>
    class EpollWaiter {
    public:
        //! Dtor.
        ~EpollWaiter()
        {
            endpoint_close(epfd_);
            endpoint_close(selfpipe_[0]);
            endpoint_close(selfpipe_[1]);
        }

        //! Ctor.
        //! @param max_events
        //!     Maximum number of epoll events to read before calling event
        //!     handler
        explicit EpollWaiter(int max_events = kDefaultMaxEvents)
            : max_events_(max_events)
        {
            // Generate epoll instance
            epfd_ = epoll_create1(0);
            if (epfd_ == -1) {
                throw std::runtime_error("Failed to create epoll descriptor");
            }

            // Generate the self-pipe used to send control signals; signals
            // close
            if (::socketpair(AF_UNIX, SOCK_STREAM, 0, selfpipe_) == -1) {
                throw std::runtime_error("Failed to create epoll descriptor");
            }

            if (detail::ctl(epfd_,
                            EPOLL_CTL_ADD,
                            selfpipe_[1],
                            EPOLLIN | EPOLLET | EPOLLONESHOT,
                            &selfpipe_[1])
                == -1) {
                throw std::runtime_error("Failed to create epoll descriptor");
            }
        }

        //! Removes managed socket.
        //! @param sfd
        //!     Socket file descriptor
        void remove(int sfd) const
        {
            struct epoll_event event = {};
            epoll_ctl(epfd_, EPOLL_CTL_DEL, sfd, &event);
        }

        //! Adds socket and packet handler.
        //! @param handler
        //!     Pointer to the handler
        //! @param sfd
        //!     Socket file descriptor
        //! @param flags
        //!     Event flags
        //! @return
        //!     True if addition is successful, false otherwise
        bool add(HandlerType* handler, int sfd, unsigned flags)
        {
            return detail::ctl(epfd_, EPOLL_CTL_ADD, sfd, flags, handler) == 0;
        }

        //! Rearms client socket and handler.
        //! @param handler
        //!     Pointer to the handler
        //! @param sfd
        //!     Socket file descriptor
        //! @param flags
        //!     Event flags
        void rearm(HandlerType* handler, int sfd, unsigned flags)
        {
            detail::ctl(epfd_, EPOLL_CTL_MOD, sfd, flags, handler);
        }

        //! Waits on epoll instance.
        //! @param sink
        //!     Downstream event handler
        void wait(SinkType* sink);

        void run()
        {
            std::scoped_lock<std::mutex> l(access_lock_);

            bool running = false;
            running_.compare_exchange_strong(running, true);
        }

        //! Signals shut down by writing to pipe.
        void close(bool block = true)
        {
            {
                std::scoped_lock<std::mutex> l(access_lock_);

                bool running = true;
                if (!running_.compare_exchange_strong(running, false)) {
                    return;
                }

                close_epoll_socket();
            }

            if (!block) {
                return;
            }

            // Busy-wait until all instances closed
            while (true) {
                std::scoped_lock<std::mutex> l(access_lock_);
                if (instance_count_ == 0) {
                    break;
                }
            }
        }

        // Non-copyable object
        explicit EpollWaiter(EpollWaiter&) = delete;
        explicit EpollWaiter(const EpollWaiter&) = delete;
    private:
        void start_close_epoll_socket()
        {
            std::scoped_lock<std::mutex> l(access_lock_);

            char ch;
            endpoint_read(selfpipe_[1], &ch, sizeof(ch));

            // Daisy-chained shutdown using the self-pipe trick.
            // Before escaping the current thread, this block will
            // write to the self-pipe. The next thread to call
            // epoll_wait() will read the pipe and follow the same
            // daisy-chained exit procedure.
            if (--instance_count_ > 0) {
                close_epoll_socket();
            }
        }

        void close_epoll_socket()
        {
            detail::ctl(epfd_,
                        EPOLL_CTL_MOD,
                        selfpipe_[1],
                        EPOLLIN | EPOLLET | EPOLLONESHOT,
                        &selfpipe_[1]);

            char ch = 0;
            endpoint_write(selfpipe_[0], &ch, sizeof(ch));
        }

        static const int kDefaultMaxEvents = 65536;

        std::mutex access_lock_;
        std::atomic<bool> running_ = false;

        // Pipe used to send control signals; signals close
        int selfpipe_[2];

        // Epoll parameter
        int epfd_ = -1;

        // Epoll parameter
        int max_events_ = kDefaultMaxEvents;

        // Count of waiting threads
        // Incremented everytime wait is invoked
        // Decremented for every node closed during the daisy-chained shutdown
        // sequence
        int instance_count_ = 0;
    };

    /*! Waits on epoll instance.
     */
    template <typename SinkType, typename HandlerType>
    void EpollWaiter<SinkType, HandlerType>::wait(SinkType* sink)
    {
        {
            std::scoped_lock<std::mutex> l(access_lock_);
            if (!running_) {
                return;
            }

            ++instance_count_;
        }

        int epfd = epfd_;
        int max_events = max_events_;

        auto* const events
            = new epoll_event[static_cast<std::size_t>(max_events)];

        // Enter epoll wait loop...
        while (true) {
            int nevents = epoll_wait(epfd, events, max_events, 0);
            if (nevents == -1) {
                break; // Encountered error
            }

            for (int i = 0; i != nevents; ++i) {
                auto& event = events[i];
                // If event is from control socket, trigger daisy-changed
                // shutdown sequence and exit the wait loop
                if (event.data.ptr == &selfpipe_[1]) {
                    delete[] events;

                    start_close_epoll_socket();
                    return;
                }

                // Otherwise, have a regular socket, so handle the event
                if (running_.load()) {
                    sink->trigger(static_cast<HandlerType*>(events[i].data.ptr),
                                  event.events);
                }
            }
        }

        delete[] events;

        std::scoped_lock<std::mutex> l(access_lock_);
        --instance_count_;
    }
} // namespace fserv
