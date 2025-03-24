/* epoll.hpp -- v1.0
   Wraps an epoll instance */

#pragma once

#include "endpoint.hpp"
#include <atomic>
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
    inline int ctl(int epfd, int opcode, int sfd, int events, void* handler_ptr)
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
            util::endpoint_close(epfd_);
            util::endpoint_close(selfpipe_[0]);
            util::endpoint_close(selfpipe_[1]);
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
        //! @return
        //!     True if removal is successful, false otherwise
        bool remove(int sfd)
        {
            struct epoll_event event = {};
            return epoll_ctl(epfd_, EPOLL_CTL_DEL, sfd, &event) == 0;
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
        bool add(HandlerType* handler, int sfd, int flags)
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
        //! @return
        //!     True if rearming is successful, false otherwise
        bool rearm(HandlerType* handler, int sfd, int flags)
        {
            return detail::ctl(epfd_, EPOLL_CTL_MOD, sfd, flags, handler) == 0;
        }

        //! Waits on epoll instance.
        //! @param sink
        //!     Downstream event handler
        void wait(SinkType* sink);

        //! Signals shut down by writing to pipe.
        void close()
        {
            if (detail::ctl(epfd_,
                            EPOLL_CTL_MOD,
                            selfpipe_[1],
                            EPOLLIN | EPOLLET | EPOLLONESHOT,
                            &selfpipe_[1])
                == -1) {
                throw std::runtime_error("Failed to rearm epoll descriptor "
                                         "when terminating epoll instance");
            }

            char ch = 0;
            util::endpoint_write(selfpipe_[0], &ch, sizeof(ch));
        }

        // Non-copyable object
        explicit EpollWaiter(EpollWaiter&) = delete;
        explicit EpollWaiter(const EpollWaiter&) = delete;
    private:
        static const int kDefaultMaxEvents = 65536;

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
        std::atomic<int> instance_count_ = 0;
    };

    /*! Waits on epoll instance.
     */
    template <typename SinkType, typename HandlerType>
    void EpollWaiter<SinkType, HandlerType>::wait(SinkType* sink)
    {
        ++instance_count_;

        int epfd = epfd_;
        int max_events = max_events_;

        auto* const events = new epoll_event[max_events];

        // Enter epoll wait loop...
        for (bool run = true; run;) {
            int nevents = epoll_wait(epfd, events, max_events, 0);
            if (nevents == -1) {
                break; // Encountered error
            }

            for (int i = 0; i != nevents; ++i) {
                auto& event = events[i];
                // If event is from control socket, trigger daisy-changed
                // shutdown sequence and exit the wait loop
                if (event.data.ptr == &selfpipe_[1]) {
                    char ch;
                    util::endpoint_read(selfpipe_[1], &ch, sizeof(ch));

                    // Daisy-chained shutdown using the self-pipe trick.
                    // Before escaping the current thread, this block will
                    // write to the self-pipe. The next thread to call
                    // epoll_wait() will read the pipe and follow the same
                    // daisy-chained exit procedure.
                    if (--instance_count_ > 0) {
                        close();
                    }

                    run = false;
                    break;
                }

                // Otherwise, have a regular socket, so handle the event
                sink->trigger(
                    reinterpret_cast<HandlerType*>(events[i].data.ptr),
                    event.events);
            }
        }

        delete[] events;
    }
} // namespace fserv
