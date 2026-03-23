/* client_pool.hpp -- v1.0
   Multithreaded wrapper around EpollWaiter instance that registers client
   sockets and handles all triggered client events */

#pragma once

#include "atomic_stack.hpp"
#include "client_generation_token.hpp"
#include "client_session.hpp"
#include "client_session_manager.hpp"
#include "client_state.hpp"
#include "epoll.hpp"
#include "std_memory.hpp"
#include "timeout_timer.hpp"
#include <atomic>
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
        void add_client(int sfd, int thread_id)
        {
            constexpr unsigned kEpollFlags = EPOLLIN | EPOLLET | EPOLLHUP
                                             | EPOLLRDHUP | EPOLLPRI
                                             | EPOLLONESHOT;
            auto [client, uuid] = clients_stack_.pop(sfd, thread_id);
            if (client == nullptr) {
                return;
            }

            generation_tokens_[client->index].store(ClientGenerationToken{
                .uuid = uuid,
                .state = static_cast<std::uint64_t>(ClientState::kReady)});

            if (!epoll_.add(client, sfd, kEpollFlags)) {
                terminate(client, uuid);
                return;
            }

            if (timeout_interval_ > 0) {
                timeout_timer_.set(client, uuid);
            }

            have_client_accepted(client, uuid);
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
        bool run(int worker_count,
                 int max_client_count,
                 int timeout_interval,
                 int server_count)
        {
            std::scoped_lock l(status_check_lock_);

            // Only proceed if not already in running instance
            if (!workers_.empty()) {
                return false;
            }

            // Allocate clients buffer
            if (!std_init(node_pool_,
                          static_cast<std::size_t>(max_client_count))) {
                throw std::bad_alloc();
            }

            generation_tokens_
                = std::vector<std::atomic<ClientGenerationToken>>(
                    node_pool_.capacity);

            // Maybe run timeout timer
            timeout_interval_ = timeout_interval;
            if (timeout_interval_ > 0) {
                timeout_timer_.run(
                    timeout_interval_,
                    &generation_tokens_,
                    [this](const std::vector<
                           std::pair<StackNode<ClientType>*, std::uint64_t>>&
                               timed_out_clients) {
                        on_client_timeout(timed_out_clients);
                    });
            }

            clients_stack_.init(node_pool_, server_count);
            epoll_.run();
            for (int i = 0; i < worker_count; ++i) {
                workers_.emplace_back([this] {
                    epoll_.wait(this);
                });
            }

            return true;
        }

        //! Stops running worker instances.
        void stop()
        {
            std::scoped_lock l(status_check_lock_);

            // Only proceed if already in running instance
            if (workers_.empty()) {
                return;
            }

            timeout_timer_.stop();

            // Master thread initiates the shutdown daisy-chain
            epoll_.close();

            for (auto& thread: workers_) {
                thread.join();
            }

            workers_ = std::vector<std::jthread>();

            // Reset active clients...
            for (std::size_t i = 0; i != node_pool_.capacity; ++i) {
                auto* client = &node_pool_.ptr_to_mem_slab[i];
                ClientGenerationToken generation_token
                    = generation_tokens_[client->index].load();
                // Close socket descriptor
                if (client->sfd > 0) {
                    epoll_.remove(client->sfd);
                    endpoint_close(client->sfd);
                }
            }

            // Clean up
            destroy(node_pool_);
        }

        bool is_client_ready(StackNode<ClientType>* client,
                             std::uint64_t tested_uuid) const
        {
            const auto& generation_token = generation_tokens_[client->index];

            const auto [uuid, state] = generation_token.load();
            if (uuid != tested_uuid) {
                return false;
            }

            return state == static_cast<std::uint64_t>(ClientState::kReady);
        }

        bool is_client_working(StackNode<ClientType>* client,
                               std::uint64_t tested_uuid) const
        {
            using enum ClientState;

            const auto& generation_token = generation_tokens_[client->index];

            const auto [uuid, state] = generation_token.load();
            if (uuid != tested_uuid) {
                return false;
            }

            return state == static_cast<std::uint64_t>(kWorking);
        }

        bool set_client_closing(StackNode<ClientType>* client,
                                std::uint64_t tested_uuid)
        {
            auto& generation_token = generation_tokens_[client->index];

            while (true) {
                using enum fserv::ClientState;

                switch (ClientGenerationToken current = generation_token.load();
                        static_cast<ClientState>(current.state)) {
                    case kReady:
                    case kStopped:
                    case kWorking:
                    {
                        break;
                    }

                    case kClosing:
                    case kClosed:
                    {
                        return false;
                    }
                }

                ClientGenerationToken ready
                    = {.uuid = tested_uuid,
                       .state = static_cast<std::uint64_t>(kReady)};

                ClientGenerationToken stopped
                    = {.uuid = tested_uuid,
                       .state = static_cast<std::uint64_t>(kStopped)};

                ClientGenerationToken working
                    = {.uuid = tested_uuid,
                       .state = static_cast<std::uint64_t>(kWorking)};

                ClientGenerationToken next = {
                    .uuid = 0, .state = static_cast<std::uint64_t>(kClosing)};

                if (generation_token.compare_exchange_strong(ready, next)) {
                    return true;
                }

                if (generation_token.compare_exchange_strong(stopped, next)) {
                    return true;
                }

                if (generation_token.compare_exchange_strong(working, next)) {
                    return true;
                }
            }

            return false;
        }

        bool set_client_ready(StackNode<ClientType>* client,
                              std::uint64_t tested_uuid)
        {
            auto& generation_token = generation_tokens_[client->index];

            while (true) {
                using enum fserv::ClientState;

                switch (ClientGenerationToken current = generation_token.load();
                        static_cast<ClientState>(current.state)) {
                    case kReady:
                    case kWorking:
                    {
                        break;
                    }

                    case kStopped:
                    case kClosing:
                    case kClosed:
                    {
                        return false;
                    }
                }

                ClientGenerationToken ready = {
                    .uuid = tested_uuid,
                    .state = static_cast<std::uint64_t>(ClientState::kWorking)};

                ClientGenerationToken working = {
                    .uuid = tested_uuid,
                    .state = static_cast<std::uint64_t>(ClientState::kWorking)};

                ClientGenerationToken next = {
                    .uuid = tested_uuid,
                    .state = static_cast<std::uint64_t>(ClientState::kReady)};

                if (generation_token.compare_exchange_strong(ready, next)) {
                    break;
                }

                if (generation_token.compare_exchange_strong(working, next)) {
                    break;
                }
            }

            return true;
        }

        bool set_client_working(StackNode<ClientType>* client,
                                std::uint64_t tested_uuid)
        {
            auto& generation_token = generation_tokens_[client->index];

            ClientGenerationToken expected
                = {.uuid = tested_uuid,
                   .state = static_cast<std::uint64_t>(ClientState::kReady)};

            ClientGenerationToken next
                = {.uuid = tested_uuid,
                   .state = static_cast<std::uint64_t>(ClientState::kWorking)};

            return generation_token.compare_exchange_strong(expected, next);
        }

        bool commit_client_working(StackNode<ClientType>* client,
                                   std::uint64_t tested_uuid)
        {
            auto& generation_token = generation_tokens_[client->index];

            ClientGenerationToken expected
                = {.uuid = tested_uuid,
                   .state = static_cast<std::uint64_t>(ClientState::kWorking)};

            ClientGenerationToken next
                = {.uuid = tested_uuid,
                   .state = static_cast<std::uint64_t>(ClientState::kReady)};

            return generation_token.compare_exchange_strong(expected, next);
        }

        //! Reactivates client for next read.
        //! @param client
        //!     Client to rearm
        bool rearm(StackNode<ClientType>* client, std::uint64_t uuid) override;

        //! Closes socket and pushes client to free stack.
        //! @param client
        //!     Client to terminate
        bool terminate(StackNode<ClientType>* client,
                       std::uint64_t uuid) override;

        void pin(StackNode<ClientType>* client, std::uint64_t uuid) override;

        void unpin(StackNode<ClientType>* client, std::uint64_t uuid) override;

        //! Called on triggered event.
        //! @param client
        //!     Triggered client
        //! @param flags
        //!     Epoll event flags
        void trigger(StackNode<ClientType>* client, std::uint32_t flags);
    private:
        int timeout_interval_ = 0;

        // Epoll instance that handles all triggered client events
        EpollWaiter<ClientPool<PacketSinkType, ClientType>,
                    StackNode<ClientType>>
            epoll_;

        // Pre-allocated slab, contains current monotonically increasing
        // generation number (field is 0 for inactive clients)
        std::vector<std::atomic<ClientGenerationToken>> generation_tokens_;
        // Pre-allocated slab, contains client data
        StdMemory<StackNode<ClientType>> node_pool_;
        // Pre-allocated stack of inactive/ready clients
        AtomicStack<ClientType, StdMemory<StackNode<ClientType>>>
            clients_stack_;

        // Points to downstream packet handler / data sink
        PacketSinkType* packet_sink_;
        // Current running worker threads
        std::vector<std::jthread> workers_;
        // Runs in background to check for inactive clients
        TimeoutTimer<ClientType> timeout_timer_;

        std::atomic<std::uint64_t> triggered_clients_ = 0;

        mutable std::mutex status_check_lock_;

        //! @param client
        //!     Triggered client
        template <typename Type = PacketSinkType>
        void have_client_accepted(StackNode<ClientType>* client,
                                  std::uint64_t uuid)
        {
            if (!std::is_base_of_v<
                    enable_client_accepted<PacketSinkType, ClientType>,
                    Type>) {
                return;
            }

            ClientSession<ClientType> session(
                static_cast<ClientSessionManager<ClientType>*>(this),
                client,
                uuid);
            packet_sink_->client_accepted(session);
        }

        //! @param client
        //!     Triggered client
        template <typename Type = PacketSinkType>
        void have_client_closed(StackNode<ClientType>* client,
                                std::uint64_t uuid)
        {
            if (!std::is_base_of_v<
                    enable_client_closed<PacketSinkType, ClientType>,
                    Type>) {
                return;
            }

            ClientSession<ClientType> session(
                static_cast<ClientSessionManager<ClientType>*>(this),
                client,
                uuid);
            packet_sink_->client_closed(session);
        }

        //! @param client
        //!     Triggered client
        template <typename Type = PacketSinkType>
        void have_client_error(StackNode<ClientType>* client,
                               std::uint64_t uuid)
        {
            if (!std::is_base_of_v<
                    enable_client_error<PacketSinkType, ClientType>,
                    Type>) {
                return;
            }

            ClientSession<ClientType> session(
                static_cast<ClientSessionManager<ClientType>*>(this),
                client,
                uuid);
            packet_sink_->client_error(session);
        }

        //! @param client
        //!     Triggered client
        //! @param oobdata
        //!     Out-of-band byte
        template <typename Type = PacketSinkType>
        void have_client_oob_received(StackNode<ClientType>* client,
                                      std::uint64_t uuid,
                                      char oobdata)
        {
            if (!std::is_base_of_v<
                    enable_client_oob_received<PacketSinkType, ClientType>,
                    Type>) {
                return;
            }

            ClientSession<ClientType> session(
                static_cast<ClientSessionManager<ClientType>*>(this),
                client,
                uuid);
            packet_sink_->client_oob_received(session, oobdata);
        }

        //! @param client
        //!     Triggered client
        //! @param data
        //!     Received message data
        //! @param size
        //!     Received message data size
        template <typename Type = PacketSinkType>
        void have_client_data_received(StackNode<ClientType>* client,
                                       std::uint64_t uuid,
                                       const char* data,
                                       std::int32_t size)
        {
            if constexpr (!std::is_base_of_v<
                              enable_client_data_received<PacketSinkType,
                                                          ClientType>,
                              Type>) {
                return;
            }

            ClientSession<ClientType> session(
                static_cast<ClientSessionManager<ClientType>*>(this),
                client,
                uuid);
            packet_sink_->client_data_received(session, data, size);
        }

        void on_client_timeout(
            const std::vector<std::pair<StackNode<ClientType>*, std::uint64_t>>&
                clients)
        {
            for (auto& [client, uuid]: clients) {
                using enum ClientState;

                ClientGenerationToken token
                    = generation_tokens_[client->index].load();

                if (token.uuid != uuid) {
                    continue;
                }

                switch (static_cast<ClientState>(token.state)) {
                    case kReady:
                    case kStopped:
                    {
                        terminate_on_close(client, uuid);
                        break;
                    }

                    case kWorking:
                    case kClosing:
                    case kClosed:
                    {
                        break;
                    }
                }
            }
        }

        //! EPOLLPRI event handler
        void pri_read_ready_triggered(StackNode<ClientType>* client,
                                      std::uint64_t uuid);

        //! EPOLLIN event handler
        void read_ready_triggered(StackNode<ClientType>* client,
                                  std::uint64_t uuid);

        //! Called on triggered event.
        //! @param client
        //!     Triggered client
        //! @param flags
        //!     Epoll event flags
        void trigger(StackNode<ClientType>* client,
                     std::uint64_t uuid,
                     std::uint32_t flags);

        //! Closes socket and returns client to unused queue.
        //! @param client
        //!     To be terminated
        bool terminate_on_close(StackNode<ClientType>* client,
                                std::uint64_t uuid);

        //! Closes socket and returns client to unused queue.
        //! @param client
        //!     To be terminated
        bool terminate_on_error(StackNode<ClientType>* client,
                                std::uint64_t uuid);

        bool terminate_impl(
            StackNode<ClientType>* client,
            std::uint64_t uuid,
            const std::function<void(StackNode<ClientType>*)>& cleanup_call);
    };

    /*! Reactivates client for next read.
     */
    template <typename PacketSinkType, typename ClientType>
    bool ClientPool<PacketSinkType, ClientType>::rearm(
        StackNode<ClientType>* client,
        std::uint64_t uuid)
    {
        if (!set_client_ready(client, uuid)) {
            return false;
        }

        return epoll_.rearm(client,
                            client->sfd,
                            EPOLLIN | EPOLLET | EPOLLHUP | EPOLLRDHUP | EPOLLPRI
                                | EPOLLONESHOT);
    }

    /*! Closes socket and pushes client to free stack.
     */
    template <typename PacketSinkType, typename ClientType>
    bool ClientPool<PacketSinkType, ClientType>::terminate(
        StackNode<ClientType>* client,
        std::uint64_t uuid)
    {
        return terminate_impl(client, uuid, [](StackNode<ClientType>*) {
            /* No extra cleanup */
        });
    }

    template <typename PacketSinkType, typename ClientType>
    void ClientPool<PacketSinkType, ClientType>::pin(
        StackNode<ClientType>* client,
        std::uint64_t uuid)
    {
        if (timeout_interval_ == 0) {
            return;
        }

        while (true) {
            using enum ClientState;

            switch (ClientGenerationToken token
                    = generation_tokens_[client->index].load();
                    static_cast<ClientState>(token.state)) {
                case kReady:
                case kWorking:
                {
                    timeout_timer_.unset(client, uuid);
                    return;
                }

                case kStopped:
                case kClosing:
                case kClosed:
                {
                    return;
                }
            }
        }
    }

    template <typename PacketSinkType, typename ClientType>
    void ClientPool<PacketSinkType, ClientType>::unpin(
        StackNode<ClientType>* client,
        std::uint64_t uuid)
    {
        if (timeout_interval_ == 0) {
            return;
        }

        while (true) {
            using enum ClientState;

            switch (ClientGenerationToken token
                    = generation_tokens_[client->index].load();
                    static_cast<ClientState>(token.state)) {
                case kReady:
                case kWorking:
                {
                    timeout_timer_.set(client, uuid);
                    return;
                }

                case kStopped:
                case kClosing:
                case kClosed:
                {
                    return;
                }
            }
        }
    }

    /*! Closes socket and pushes client to free stack.
     */
    template <typename PacketSinkType, typename ClientType>
    bool ClientPool<PacketSinkType, ClientType>::terminate_on_close(
        StackNode<ClientType>* client,
        std::uint64_t uuid)
    {
        return terminate_impl(
            client, uuid, [this, uuid](StackNode<ClientType>* value) {
                have_client_closed(value, uuid);
            });
    }

    /*! Closes socket and pushes client to free stack.
     */
    template <typename PacketSinkType, typename ClientType>
    bool ClientPool<PacketSinkType, ClientType>::terminate_on_error(
        StackNode<ClientType>* client,
        std::uint64_t uuid)
    {
        return terminate_impl(
            client, uuid, [this, uuid](StackNode<ClientType>* value) {
                have_client_error(value, uuid);
            });
    }

    template <typename PacketSinkType, typename ClientType>
    bool ClientPool<PacketSinkType, ClientType>::terminate_impl(
        StackNode<ClientType>* client,
        std::uint64_t uuid,
        const std::function<void(StackNode<ClientType>*)>& extra_cleanup_call)
    {
        if (!set_client_closing(client, uuid)) {
            return false;
        }

        if (timeout_interval_ > 0) {
            timeout_timer_.unset(client, uuid);
        }

        // Close socket descriptor
        epoll_.remove(client->sfd);
        endpoint_close(client->sfd);

        extra_cleanup_call(client);

        // Push back to stack of ready clients
        clients_stack_.push(client);
        return true;
    }

    /*! Called on triggered event.
     */
    template <typename PacketSinkType, typename ClientType>
    void ClientPool<PacketSinkType, ClientType>::trigger(
        StackNode<ClientType>* client,
        std::uint32_t flags)
    {
        trigger(client, client->uuid, flags);
    }

    /*! Called on triggered event.
     */
    template <typename PacketSinkType, typename ClientType>
    void ClientPool<PacketSinkType, ClientType>::trigger(
        StackNode<ClientType>* client,
        std::uint64_t uuid,
        std::uint32_t flags)
    {
        if (!set_client_working(client, uuid)) {
            return;
        }

        if (flags & EPOLLERR) {
            terminate_on_error(client, uuid);
            return;
        }

        if ((flags & EPOLLHUP) || (flags & EPOLLRDHUP)) {
            terminate_on_close(client, uuid);
            return;
        }

        if (flags & EPOLLPRI) {
            pri_read_ready_triggered(client, uuid);
        }

        if (flags & EPOLLIN) {
            read_ready_triggered(client, uuid);
        }

        commit_client_working(client, uuid);
    }

    /*! EPOLLIN event handler
     */
    template <typename PacketSinkType, typename ClientType>
    void ClientPool<PacketSinkType, ClientType>::read_ready_triggered(
        StackNode<ClientType>* const client,
        std::uint64_t uuid)
    {
        if (!is_client_working(client, uuid)) {
            return;
        }

        while (true) {
            // Read incoming message
            int nbytes = -1;
            ClientType& sink = client->value;
            const char* data = sink.read(client->sfd, &nbytes);

            if (nbytes == -1 && errno == EAGAIN) {
                break;
            }

            // Handle error case
            if (nbytes == -1) {
                terminate_on_error(client, uuid);
                return;
            }

            // Handle client close
            if (nbytes == 0) {
                terminate_on_close(client, uuid);
                return;
            }

            // Have actual data
            // Process it...
            have_client_data_received(client, uuid, data, nbytes);
        }

        if (timeout_interval_ > 0) {
            timeout_timer_.reset(client, uuid);
        }
    }

    /*! EPOLLPRI event handler
     */
    template <typename PacketSinkType, typename ClientType>
    void ClientPool<PacketSinkType, ClientType>::pri_read_ready_triggered(
        StackNode<ClientType>* const client,
        std::uint64_t uuid)
    {
        if (!is_client_working(client, uuid)) {
            return;
        }

        while (true) {
            int nbytes = 0;
            int mark = 0;
            const char* oobdata
                = (client->value).read_oob(client->sfd, &nbytes, &mark);

            if (mark == -1 && errno == EAGAIN) {
                break;
            }

            // If have actual error - terminate client
            if (mark == -1) {
                terminate_on_error(client, uuid);
                return;
            }

            if (mark == 0) {
                // Nothing to do
                break;
            }

            if (nbytes > 0) {
                // Have actual data
                // Process it...
                have_client_oob_received(client, uuid, *oobdata);
            }
        }

        if (timeout_interval_ > 0) {
            timeout_timer_.reset(client, uuid);
        }
    }
} // namespace fserv
