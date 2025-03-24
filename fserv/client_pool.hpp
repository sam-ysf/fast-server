/* client_pool.hpp -- v1.0
   Multithreaded wrapper around EpollWaiter instance that registers client
   sockets and handles all triggered client events */

#pragma once

#include "atomic_stack.hpp"
#include "client_session.hpp"
#include "client_session_manager.hpp"
#include "endpoint.hpp"
#include "epoll.hpp"
#include "std_memory.hpp"
#include "timeout_timer.hpp"
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace fserv {

    template <typename DerivedType, typename ClientType>
    struct enable_client_error {
        //! SFINAE
        void client_error(ClientSession<ClientType>& client)
        {
            static_cast<DerivedType*>(this)->client_error(client);
        }
    };

    template <typename DerivedType, typename ClientType>
    struct enable_client_accepted {
        //! SFINAE
        void client_accepted(ClientSession<ClientType>& client)
        {
            static_cast<DerivedType*>(this)->client_accepted(client);
        }
    };

    template <typename DerivedType, typename ClientType>
    struct enable_client_closed {
        //! SFINAE
        void client_closed(ClientSession<ClientType>& client)
        {
            static_cast<DerivedType*>(this)->client_closed(client);
        }
    };

    template <typename DerivedType, typename ClientType>
    struct enable_client_data_received {
        //! SFINAE
        void client_data_received(ClientSession<ClientType>& client,
                                  const char* data,
                                  const int size)
        {
            static_cast<DerivedType*>(this)->client_data_received(
                client, data, size);
        }
    };

    template <typename DerivedType, typename ClientType>
    struct enable_client_oob_received {
        //! SFINAE
        void client_oob_received(ClientSession<ClientType>& client,
                                 char oobdata)
        {
            static_cast<DerivedType*>(this)->client_oob_received(client,
                                                                 oobdata);
        }
    };

    //! @class ClientPool
    /*! Encapsulates event handling of multiple clients
     */
    template <typename PacketSinkType, typename ClientType>
    class ClientPool : public ClientSessionManager<ClientType> {
    public:
        //! Ctor.
        //! @param packet_sink
        //!     Downstream event handler
        explicit ClientPool(PacketSinkType* packet_sink)
            : packet_sink_(packet_sink)
        {}

        //! Adds a new client.
        //! @param sfd
        //!     Socket file descriptor
        //! @return
        //!     The newly-allocated client
        ClientType* add_client(int sfd)
        {
            auto* node = clients_stack_.pop();
            if (node == nullptr) {
                return nullptr;
            }

            auto* client = new (node) ClientType(sfd, this);
            static_cast<util::StackNode<ClientType>*>(client)->sfd = sfd;
            have_client_accepted(client);

            constexpr int kFlags = EPOLLIN | EPOLLET | EPOLLHUP | EPOLLRDHUP
                                   | EPOLLPRI | EPOLLONESHOT;
            if (!epoll_.add(client, sfd, kFlags)) {
                return terminate(client), nullptr;
            }

            timeout_timer_.set(client);

            return client;
        }

        //! Initializes and starts the pool.
        //! @param worker_count
        //!     Client handler thread count
        //! @param max_client_count
        //!     Maximum number of clients
        //! @param timeout_interval
        //!     Client inactivity timeout interval
        //! @return
        //!     True if the pool is successfully started, false otherwise
        bool run(int worker_count, int max_client_count, int timeout_interval)
        {
            std::lock_guard<std::mutex> l(status_check_lock_);

            // Only proceed if not already in running instance
            if (!threads_.empty()) {
                return false;
            }

            // Allocate clients buffer
            if (!init(mem_pool_, max_client_count)) {
                throw std::bad_alloc();
            }

            if (timeout_interval > 0) {
                const auto fn =
                    [this](const std::vector<ClientType*>& timed_out_clients) {
                        for (auto* client: timed_out_clients) {
                            terminate_on_close(client);
                        }
                    };

                timeout_timer_.run(timeout_interval, fn);
            }

            clients_stack_.init(mem_pool_);
            for (int i = 0; i != worker_count; ++i) {
                threads_.emplace_back([&] {
                    epoll_.wait(this);
                });
            }

            return true;
        }

        //! Stops running worker instances.
        void stop()
        {
            std::lock_guard<std::mutex> l(status_check_lock_);

            if (threads_.empty()) {
                return;
            }

            timeout_timer_.stop();

            // Master thread initiates the shutdown daisy-chain
            epoll_.close();
            for (auto& thread: threads_) {
                thread.join();
            }

            threads_.clear();

            // Reset active clients...
            for (int i = 0; i != mem_pool_.capacity; ++i) {
                auto* node = &mem_pool_.ptr_to_mem_slab[i];
                auto* client = static_cast<ClientType*>(node);
                terminate(client);
            }

            // Clean up
            destroy(mem_pool_);
        }

        //! Reactivates client for next read.
        //! @param client
        //!     Client to rearm
        void rearm(ClientType* client) override;

        //! Closes socket and pushes client to free stack.
        //! @param client
        //!     Client to terminate
        void terminate(ClientType* client) override;

        //! Called on triggered event.
        //! @param client
        //!     Triggered client
        //! @param flags
        //!     Epoll event flags
        void trigger(ClientType* client, int flags);
    private:
        // Epoll instance that handles all triggered client events
        EpollWaiter<ClientPool<PacketSinkType, ClientType>, ClientType> epoll_;
        // Pre-allocated slab of memory, to be passed to client stack
        util::StdMemory<util::StackNode<ClientType>> mem_pool_;
        // Pre-allocated stack of inactive/ready clients
        util::AtomicStack<ClientType,
                          util::StdMemory<util::StackNode<ClientType>>>
            clients_stack_;
        // Points to downstream packet handler / data sink
        PacketSinkType* packet_sink_;
        // Current running worker threads
        std::vector<std::thread> threads_;
        // Runs in background to check for inactive clients
        TimeoutTimer<ClientType> timeout_timer_;

        mutable std::mutex status_check_lock_;

        //! Closes socket and returns client to unused queue.
        //! @param client
        //!     To be terminated
        void terminate_on_close(ClientType* client);

        //! Closes socket and returns client to unused queue.
        //! @param client
        //!     To be terminated
        void terminate_on_error(ClientType* client);

        template <typename q_t = PacketSinkType>
        typename std::enable_if<
            !std::is_base_of_v<
                enable_client_accepted<PacketSinkType, ClientType>,
                q_t>,
            void>::type
        have_client_accepted(ClientType&)
        {
            /* Not implemented in packet sink */
        }

        //! @param client
        //!     Triggered client
        template <typename q_t = PacketSinkType>
        typename std::enable_if<
            std::is_base_of_v<
                enable_client_accepted<PacketSinkType, ClientType>,
                q_t>,
            void>::type
        have_client_accepted(ClientType* client)
        {
            const int uuid
                = static_cast<util::StackNode<ClientType>*>(client)->uuid;

            ClientSession<ClientType> session(client, uuid);
            packet_sink_->client_accepted(session);
        }

        template <typename q_t = PacketSinkType>
        typename std::enable_if<
            !std::is_base_of_v<enable_client_closed<PacketSinkType, ClientType>,
                               q_t>,
            void>::type
        have_client_closed(ClientType*)
        {
            /* Not implemented in packet sink */
        }

        //! @param client
        //!     Triggered client
        template <typename q_t = PacketSinkType>
        typename std::enable_if<
            std::is_base_of_v<enable_client_closed<PacketSinkType, ClientType>,
                              q_t>,
            void>::type
        have_client_closed(ClientType* client)
        {
            const int uuid
                = static_cast<util::StackNode<ClientType>*>(client)->uuid;

            ClientSession<ClientType> session(client, uuid);
            packet_sink_->client_closed(session);
        }

        template <typename q_t = PacketSinkType>
        typename std::enable_if<
            !std::is_base_of_v<enable_client_error<PacketSinkType, ClientType>,
                               q_t>,
            void>::type
        have_client_error(ClientType*)
        {
            /* Not implemented in packet sink */
        }

        //! @param client
        //!     Triggered client
        template <typename q_t = PacketSinkType>
        typename std::enable_if<
            std::is_base_of_v<enable_client_error<PacketSinkType, ClientType>,
                              q_t>,
            void>::type
        have_client_error(ClientType* client)
        {
            const int uuid
                = static_cast<util::StackNode<ClientType>*>(client)->uuid;

            ClientSession<ClientType> session(client, uuid);
            packet_sink_->client_error(session);
        }

        template <typename q_t = PacketSinkType>
        typename std::enable_if<
            !std::is_base_of_v<
                enable_client_oob_received<PacketSinkType, ClientType>,
                q_t>,
            void>::type
        have_client_oob_received(ClientType*, char)
        {
            /* Not implemented in packet sink */
        }

        //! @param client
        //!     Triggered client
        //! @param oobdata
        //!     Out-of-band byte
        template <typename q_t = PacketSinkType>
        typename std::enable_if<
            std::is_base_of_v<
                enable_client_oob_received<PacketSinkType, ClientType>,
                q_t>,
            void>::type
        have_client_oob_received(ClientType* client, char oobdata)
        {
            const int uuid
                = static_cast<util::StackNode<ClientType>*>(client)->uuid;

            ClientSession<ClientType> session(client, uuid);
            packet_sink_->client_oob_received(session, oobdata);
        }

        template <typename q_t = PacketSinkType>
        typename std::enable_if<
            !std::is_base_of_v<
                enable_client_data_received<PacketSinkType, ClientType>,
                q_t>,
            void>::type
        have_client_data_received(ClientType*, const char*, const int)
        {
            /* Not implemented in packet sink */
        }

        //! @param client
        //!     Triggered client
        //! @param data
        //!     Received message data
        //! @param size
        //!     Received message data size
        template <typename q_t = PacketSinkType>
        typename std::enable_if<
            std::is_base_of_v<
                enable_client_data_received<PacketSinkType, ClientType>,
                q_t>,
            void>::type
        have_client_data_received(ClientType* client,
                                  const char* data,
                                  const int size)
        {
            const int uuid
                = static_cast<util::StackNode<ClientType>*>(client)->uuid;

            ClientSession<ClientType> session(client, uuid);
            packet_sink_->client_data_received(session, data, size);
        }

        //! EPOLLPRI event handler
        inline void pri_read_ready_triggered(ClientType*);

        //! EPOLLIN event handler
        inline void read_ready_triggered(ClientType*);
    };

    /*! Reactivates client for next read.
     */
    template <typename PacketSinkType, typename ClientType>
    void ClientPool<PacketSinkType, ClientType>::rearm(ClientType* client)
    {
        const int sfd = static_cast<util::StackNode<ClientType>*>(client)->sfd;

        constexpr int kFlags = EPOLLIN | EPOLLET | EPOLLHUP | EPOLLRDHUP
                               | EPOLLPRI | EPOLLONESHOT;
        epoll_.rearm(client, sfd, kFlags);
    }

    /*! Closes socket and pushes client to free stack.
     */
    template <typename PacketSinkType, typename ClientType>
    void ClientPool<PacketSinkType, ClientType>::terminate(ClientType* client)
    {
        const int sfd = static_cast<util::StackNode<ClientType>*>(client)->sfd;

        if (sfd == 0) {
            return;
        }

        // Close socket descriptor
        util::endpoint_close(sfd);
        epoll_.remove(sfd);
        // Clear
        static_cast<util::StackNode<ClientType>*>(client)->sfd = 0;

        /* No callback */

        // Push back to stack of ready clients
        clients_stack_.push(client);
    }

    /*! Closes socket and pushes client to free stack.
     */
    template <typename PacketSinkType, typename ClientType>
    void ClientPool<PacketSinkType, ClientType>::terminate_on_close(
        ClientType* client)
    {
        const int sfd = static_cast<util::StackNode<ClientType>*>(client)->sfd;

        if (sfd == 0) {
            return;
        }

        // Close socket descriptor
        util::endpoint_close(sfd);
        epoll_.remove(sfd);
        // Clear
        static_cast<util::StackNode<ClientType>*>(client)->sfd = 0;

        // Trigger callback
        have_client_closed(client);

        // Push back to stack of ready clients
        clients_stack_.push(client);
    }

    /*! Closes socket and pushes client to free stack.
     */
    template <typename PacketSinkType, typename ClientType>
    void ClientPool<PacketSinkType, ClientType>::terminate_on_error(
        ClientType* client)
    {
        const int sfd = static_cast<util::StackNode<ClientType>*>(client)->sfd;

        if (sfd == 0) {
            return;
        }

        // Close socket descriptor
        util::endpoint_close(sfd);
        epoll_.remove(sfd);
        // Clear
        static_cast<util::StackNode<ClientType>*>(client)->sfd = 0;

        // Trigger callback
        have_client_error(client);

        // Push back to stack of ready clients
        clients_stack_.push(client);
    }

    /*! Called on triggered event.
     */
    template <typename PacketSinkType, typename ClientType>
    void ClientPool<PacketSinkType, ClientType>::trigger(ClientType* client,
                                                         int flags)
    {
        if (flags & EPOLLERR) {
            terminate_on_error(client);
            return;
        }

        if ((flags & EPOLLHUP) || (flags & EPOLLRDHUP)) {
            have_client_closed(client);
            terminate_on_close(client);
            return;
        }

        if (flags & EPOLLPRI) {
            timeout_timer_.set(client);
            pri_read_ready_triggered(client);
        }

        if (flags & EPOLLIN) {
            timeout_timer_.set(client);
            read_ready_triggered(client);
        }
    }

    /*! EPOLLIN event handler
     */
    template <typename PacketSinkType, typename ClientType>
    void ClientPool<PacketSinkType, ClientType>::read_ready_triggered(
        ClientType* const client)
    {
        while (true) {
            // Read incoming message
            int nbytes = -1;
            const char* data = client->read(&nbytes);

            if (nbytes == -1 && errno == EAGAIN) {
                break;
            }

            // Handle error case
            if (nbytes == -1) {
                terminate_on_error(client);
                break;
            }

            // Handle client close
            if (nbytes == 0) {
                terminate_on_close(client);
                break;
            }

            // Have actual data
            // Process it...
            have_client_data_received(client, data, nbytes);
        }
    }

    /*! EPOLLPRI event handler
     */
    template <typename PacketSinkType, typename ClientType>
    void ClientPool<PacketSinkType, ClientType>::pri_read_ready_triggered(
        ClientType* const client)
    {
        while (true) {
            char oobdata = 0;
            int mark = client->read_oob(&oobdata);

            if (mark == -1 && errno == EAGAIN) {
                break;
            }

            // If have actual error - terminate client
            if (mark == -1) {
                terminate_on_error(client);
                break;
            }

            if (mark == 0) {
                // Nothing to do
                break;
            }

            // Have actual data
            // Process it...
            have_client_oob_received(client, oobdata);
        }
    }
} // namespace fserv
