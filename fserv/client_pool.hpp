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
#include "fserv/endpoint.hpp"
#include "std_memory.hpp"
#include "timeout_timer.hpp"
#include <atomic>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace fserv {

    enum class RunState : std::uint8_t {
        kPoolReady,
        kPoolStopped,
    };

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
        bool client_accepted(ClientSession<ClientType>& client)
        {
            return static_cast<DerivedType*>(this)->client_accepted(client);
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

            active_clients_.fetch_add(1);
            if (runstate_.load() != RunState::kPoolReady) {
                endpoint_close(sfd);
                active_clients_.fetch_sub(1);
                return;
            }

            auto [client, uuid] = clients_stack_.pop(sfd, thread_id);
            if (client == nullptr) {
                endpoint_close(sfd);
                active_clients_.fetch_sub(1);
                return;
            }

            generation_tokens_[client->index].store(ClientGenerationToken{
                .writing = 0,
                .uuid = uuid,
                .state = static_cast<std::uint64_t>(ClientState::kReady)});

            if (!have_client_accepted(client, uuid)) {
                terminate(client, uuid);
                active_clients_.fetch_sub(1);
                return;
            }

            if (!epoll_.add(client, sfd, kEpollFlags)) {
                terminate(client, uuid);
                active_clients_.fetch_sub(1);
                return;
            }

            maybe_set_timeout(client, uuid);

            // Done
            // Decrement hazard counter
            active_clients_.fetch_sub(1);
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
        bool run(unsigned worker_count,
                 unsigned max_client_count,
                 unsigned timeout_interval,
                 unsigned server_count)
        {
            std::scoped_lock l(status_check_lock_);

            // Only proceed if not already in running instance
            if (runstate_.load() == RunState::kPoolReady) {
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
                        have_client_timeout(timed_out_clients);
                    });
            }

            clients_stack_.init(node_pool_, server_count);
            epoll_.run();
            for (unsigned i = 0; i < worker_count; ++i) {
                workers_.emplace_back([this] {
                    epoll_.wait(this);
                });
            }

            runstate_.store(RunState::kPoolReady);
            return true;
        }

        //! Stops running worker instances.
        bool stop()
        {
            std::scoped_lock l(status_check_lock_);

            // Only proceed if already in running instance
            if (!set_to_stopped()) {
                return false;
            }

            while (active_clients_.load() > 0) {
                /* Wait for active cients to finish work */
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
                auto token = generation_tokens_[client->index].load();
                // Close socket descriptor
                const auto sup = (client->sup).load();
                if (token.uuid > 0) {
                    // Remove from timeout handler
                    maybe_unset_timeout(client, token.uuid);
                    // Remove socket descriptor
                    epoll_.remove(sup.sfd);
                    endpoint_close(sup.sfd);
                    // Push back to stack of ready clients
                    clients_stack_.push(client);
                }
            }

            destroy(node_pool_);

            return true;
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

        void init(StackNode<ClientType>* client,
                  std::uint64_t uuid,
                  const ClientType& sink) override;

        void init(StackNode<ClientType>* client,
                  std::uint64_t uuid,
                  ClientType&& sink) override;

        int write(StackNode<ClientType>* client,
                  std::uint64_t uuid,
                  const char* buff,
                  std::uint32_t size) override;

        //! Called on triggered event.
        //! @param client
        //!     Triggered client
        //! @param flags
        //!     Epoll event flags
        void trigger(StackNode<ClientType>* client, std::uint32_t flags);
    private:
        bool set_client_closing(StackNode<ClientType>* client,
                                std::uint64_t tested_uuid)
        {
            using enum fserv::ClientState;

            ClientGenerationToken ready
                = {.writing = 0,
                   .uuid = tested_uuid,
                   .state = static_cast<std::uint64_t>(kReady)};

            ClientGenerationToken worked
                = {.writing = 0,
                   .uuid = tested_uuid,
                   .state = static_cast<std::uint64_t>(kWorked)};

            ClientGenerationToken stopped
                = {.writing = 0,
                   .uuid = tested_uuid,
                   .state = static_cast<std::uint64_t>(kStopped)};

            const ClientGenerationToken next
                = {.writing = 0,
                   .uuid = 0,
                   .state = static_cast<std::uint64_t>(kClosing)};

            auto& generation_token = generation_tokens_[client->index];

            while (true) {
                const auto [_writing, uuid, state] = generation_token.load();
                if (uuid != tested_uuid) {
                    return false;
                }

                switch (static_cast<ClientState>(state)) {
                    case kReady:
                    case kStopped:
                    case kWorked:
                    {
                        break;
                    }

                    case kWorking:
                    case kRearming:
                    {
                        continue; // Wait till rearm completion
                    }

                    case kClosing:
                    case kClosed:
                    {
                        return false;
                    }
                }

                if (generation_token.compare_exchange_strong(ready, next)) {
                    return true;
                }

                if (generation_token.compare_exchange_strong(worked, next)) {
                    return true;
                }

                if (generation_token.compare_exchange_strong(stopped, next)) {
                    return true;
                }
            }
        }

        bool set_client_ready(StackNode<ClientType>* client,
                              std::uint64_t tested_uuid)
        {
            using enum fserv::ClientState;

            ClientGenerationToken rearming
                = {.writing = 0,
                   .uuid = tested_uuid,
                   .state = static_cast<std::uint64_t>(kRearming)};

            const ClientGenerationToken next
                = {.writing = 0,
                   .uuid = tested_uuid,
                   .state = static_cast<std::uint64_t>(kReady)};

            ClientGenerationToken rearming_while_writing
                = {.writing = 1,
                   .uuid = tested_uuid,
                   .state = static_cast<std::uint64_t>(kRearming)};

            const ClientGenerationToken next_while_writing
                = {.writing = 1,
                   .uuid = tested_uuid,
                   .state = static_cast<std::uint64_t>(kReady)};

            auto& generation_token = generation_tokens_[client->index];

            while (true) {
                const auto [_writing, uuid, state] = generation_token.load();
                if (uuid != tested_uuid) {
                    return false;
                }

                switch (static_cast<ClientState>(state)) {
                    case kRearming:
                    {
                        break;
                    }

                    case kReady:
                    case kWorking:
                    case kWorked:
                    case kStopped:
                    case kClosing:
                    case kClosed:
                    {
                        return false;
                    }
                }

                if (generation_token.compare_exchange_strong(rearming, next)) {
                    return true;
                }

                if (generation_token.compare_exchange_strong(
                        rearming_while_writing, next_while_writing)) {
                    return true;
                }
            }
        }

        bool set_client_rearming(StackNode<ClientType>* client,
                                 std::uint64_t tested_uuid)
        {
            using enum fserv::ClientState;

            ClientGenerationToken working
                = {.writing = 0,
                   .uuid = tested_uuid,
                   .state = static_cast<std::uint64_t>(kWorked)};

            const ClientGenerationToken next
                = {.writing = 0,
                   .uuid = tested_uuid,
                   .state = static_cast<std::uint64_t>(kRearming)};

            ClientGenerationToken working_while_writing
                = {.writing = 1,
                   .uuid = tested_uuid,
                   .state = static_cast<std::uint64_t>(kWorked)};

            const ClientGenerationToken next_while_writing
                = {.writing = 1,
                   .uuid = tested_uuid,
                   .state = static_cast<std::uint64_t>(kRearming)};

            auto& generation_token = generation_tokens_[client->index];

            while (true) {
                const auto [_writing, uuid, state] = generation_token.load();
                if (uuid != tested_uuid) {
                    return false;
                }

                switch (static_cast<ClientState>(state)) {
                    case kWorked:
                    {
                        break;
                    }

                    case kReady:
                    case kRearming:
                    case kWorking:
                    case kStopped:
                    case kClosing:
                    case kClosed:
                    {
                        return false;
                    }
                }

                if (generation_token.compare_exchange_strong(working, next)) {
                    return true;
                }

                if (generation_token.compare_exchange_strong(
                        working_while_writing, next_while_writing)) {
                    return true;
                }
            }
        }

        bool set_client_working(StackNode<ClientType>* client,
                                std::uint64_t tested_uuid)
        {
            using enum ClientState;

            auto& generation_token = generation_tokens_[client->index];

            ClientGenerationToken ready
                = {.writing = 0,
                   .uuid = tested_uuid,
                   .state = static_cast<std::uint64_t>(kReady)};

            ClientGenerationToken worked
                = {.writing = 0,
                   .uuid = tested_uuid,
                   .state = static_cast<std::uint64_t>(kWorked)};

            const ClientGenerationToken next
                = {.writing = 0,
                   .uuid = tested_uuid,
                   .state = static_cast<std::uint64_t>(kWorking)};

            ClientGenerationToken ready_while_writing
                = {.writing = 1,
                   .uuid = tested_uuid,
                   .state = static_cast<std::uint64_t>(kReady)};

            ClientGenerationToken worked_while_writing
                = {.writing = 1,
                   .uuid = tested_uuid,
                   .state = static_cast<std::uint64_t>(kWorked)};

            const ClientGenerationToken next_while_writing
                = {.writing = 1,
                   .uuid = tested_uuid,
                   .state = static_cast<std::uint64_t>(kWorking)};

            while (true) {
                const auto [_writing, uuid, state] = generation_token.load();
                if (uuid != tested_uuid) {
                    return false;
                }

                switch (static_cast<ClientState>(state)) {
                    case kReady:
                    case kWorked:
                    {
                        break;
                    }

                    case kRearming:
                    case kWorking:
                    case kStopped:
                    case kClosing:
                    case kClosed:
                    {
                        return false;
                    }
                }

                if (generation_token.compare_exchange_strong(ready, next)) {
                    return true;
                }

                if (generation_token.compare_exchange_strong(worked, next)) {
                    return true;
                }

                if (generation_token.compare_exchange_strong(
                        ready_while_writing, next_while_writing)) {
                    return true;
                }

                if (generation_token.compare_exchange_strong(
                        worked_while_writing, next_while_writing)) {
                    return true;
                }
            }
        }

        bool set_client_worked(StackNode<ClientType>* client,
                               std::uint64_t tested_uuid)
        {
            using enum ClientState;

            ClientGenerationToken expected
                = {.writing = 0,
                   .uuid = tested_uuid,
                   .state = static_cast<std::uint64_t>(kWorking)};

            const ClientGenerationToken next
                = {.writing = 0,
                   .uuid = tested_uuid,
                   .state = static_cast<std::uint64_t>(kWorked)};

            ClientGenerationToken expected_while_writing
                = {.writing = 1,
                   .uuid = tested_uuid,
                   .state = static_cast<std::uint64_t>(kWorking)};

            const ClientGenerationToken next_while_writing
                = {.writing = 1,
                   .uuid = tested_uuid,
                   .state = static_cast<std::uint64_t>(kWorked)};

            auto& generation_token = generation_tokens_[client->index];

            while (true) {
                const auto [_writing, uuid, state] = generation_token.load();
                if (uuid != tested_uuid) {
                    return false;
                }

                switch (static_cast<ClientState>(state)) {
                    case kWorking:
                    {
                        break;
                    }

                    case kReady:
                    case kRearming:
                    case kWorked:
                    case kStopped:
                    case kClosing:
                    case kClosed:
                    {
                        return false;
                    }
                }

                if (generation_token.compare_exchange_strong(expected, next)) {
                    return true;
                }

                if (generation_token.compare_exchange_strong(
                        expected_while_writing, next_while_writing)) {
                    return true;
                }
            }
        }

        bool set_client_writing(StackNode<ClientType>* client,
                                std::uint64_t tested_uuid,
                                bool writing)
        {
            using enum ClientState;

            const ClientGenerationToken next
                = {.writing = static_cast<std::uint64_t>(writing),
                   .uuid = tested_uuid,
                   .state = static_cast<std::uint64_t>(kWorked)};

            auto& generation_token = generation_tokens_[client->index];

            while (true) {
                const auto [_writing, uuid, state] = generation_token.load();
                if (uuid != tested_uuid) {
                    return false;
                }

                auto from_state = static_cast<ClientState>(state);
                switch (from_state) {
                    case kReady:
                    case kRearming:
                    case kWorking:
                    case kWorked:
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

                ClientGenerationToken expected
                    = {.writing = 0,
                       .uuid = tested_uuid,
                       .state = static_cast<std::uint64_t>(from_state)};

                ClientGenerationToken expected_while_writing
                    = {.writing = 1,
                       .uuid = tested_uuid,
                       .state = static_cast<std::uint64_t>(from_state)};

                if (generation_token.compare_exchange_strong(expected, next)) {
                    return true;
                }

                if (generation_token.compare_exchange_strong(
                        expected_while_writing, expected)) {
                    return true;
                }
            }
        }

        //! @param client
        //!     Triggered client
        template <typename Type = PacketSinkType>
        bool have_client_accepted(StackNode<ClientType>* client,
                                  std::uint64_t uuid)
        {
            if (!std::is_base_of_v<
                    enable_client_accepted<PacketSinkType, ClientType>,
                    Type>) {
                return true;
            }

            ClientSession<ClientType> session(
                static_cast<ClientSessionManager<ClientType>*>(this),
                client,
                uuid);
            return packet_sink_->client_accepted(session);
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

        void have_client_timeout(
            const std::vector<std::pair<StackNode<ClientType>*, std::uint64_t>>&
                clients)
        {
            using enum ClientState;

            for (auto& [client, uuid]: clients) {
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

                    case kRearming:
                    case kWorking:
                    case kWorked:
                    case kClosing:
                    case kClosed:
                    {
                        break;
                    }
                }
            }
        }

        void maybe_set_timeout(StackNode<ClientType>* client,
                               std::uint64_t uuid)
        {
            if (timeout_interval_ > 0) {
                timeout_timer_.set(client, uuid);
            }
        }

        void maybe_unset_timeout(StackNode<ClientType>* client,
                                 std::uint64_t uuid)
        {
            if (timeout_interval_ > 0) {
                timeout_timer_.unset(client, uuid);
            }
        }

        void maybe_reset_timeout(StackNode<ClientType>* client,
                                 std::uint64_t uuid)
        {
            if (timeout_interval_ > 0) {
                timeout_timer_.reset(client, uuid);
            }
        }

        //! EPOLLPRI event handler
        void pri_read_ready_triggered(StackNode<ClientType>* client,
                                      std::int32_t sfd,
                                      std::uint64_t uuid);

        //! EPOLLIN event handler
        void read_ready_triggered(StackNode<ClientType>* client,
                                  std::int32_t sfd,
                                  std::uint64_t uuid);

        //! Called on triggered event.
        //! @param client
        //!     Triggered client
        //! @param flags
        //!     Epoll event flags
        void trigger_impl(StackNode<ClientType>* client,
                          std::uint64_t uuid,
                          std::int32_t sfd,
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

        unsigned timeout_interval_ = 0;

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

        std::atomic<std::uint64_t> active_clients_ = 0;
        std::atomic<RunState> runstate_ = RunState::kPoolStopped;

        bool set_to_stopped()
        {
            using enum RunState;
            while (true) {
                switch (runstate_.load()) {
                    case kPoolReady:
                    {
                        break;
                    }

                    case kPoolStopped:
                    {
                        return false;
                    }
                }

                RunState expected = kPoolReady;
                if (runstate_.compare_exchange_strong(expected, kPoolStopped)) {
                    return true;
                }
            }
        }

        mutable std::mutex status_check_lock_;
    };

    /*! Reactivates client for next read.
     */
    template <typename PacketSinkType, typename ClientType>
    bool ClientPool<PacketSinkType, ClientType>::rearm(
        StackNode<ClientType>* client,
        std::uint64_t uuid)
    {
        static const unsigned kEpollFlags = EPOLLIN | EPOLLET | EPOLLHUP
                                            | EPOLLRDHUP | EPOLLPRI
                                            | EPOLLONESHOT;

        active_clients_.fetch_add(1);
        if (runstate_.load() != RunState::kPoolReady) {
            active_clients_.fetch_sub(1);
            return false;
        }

        if (!set_client_rearming(client, uuid)) {
            active_clients_.fetch_sub(1);
            return false;
        }

        const auto [sfd, _uuid] = (client->sup).load();

        epoll_.rearm(client, sfd, kEpollFlags);

        maybe_reset_timeout(client, uuid);
        set_client_ready(client, uuid);
        active_clients_.fetch_sub(1);
        return true;
    }

    /*! Closes socket and pushes client to free stack.
     */
    template <typename PacketSinkType, typename ClientType>
    bool ClientPool<PacketSinkType, ClientType>::terminate(
        StackNode<ClientType>* client,
        std::uint64_t uuid)
    {
        active_clients_.fetch_add(1);
        if (runstate_.load() == RunState::kPoolStopped) {
            active_clients_.fetch_sub(1);
            return false;
        }

        bool ret = terminate_impl(client, uuid, [](StackNode<ClientType>*) {
            /* No extra cleanup */
        });

        active_clients_.fetch_sub(1);
        return ret;
    }

    template <typename PacketSinkType, typename ClientType>
    void ClientPool<PacketSinkType, ClientType>::init(
        StackNode<ClientType>* client,
        std::uint64_t uuid,
        const ClientType& sink)
    {
        using enum ClientState;

        active_clients_.fetch_add(1);
        if (runstate_.load() == RunState::kPoolStopped) {
            active_clients_.fetch_sub(1);
            return;
        }

        while (true) {
            ClientGenerationToken token
                = generation_tokens_[client->index].load();
            if (token.uuid != uuid) {
                active_clients_.fetch_sub(1);
                return;
            }

            switch (static_cast<ClientState>(token.state)) {
                case kReady:
                {
                    client->sink = sink;
                    maybe_set_timeout(client, uuid);
                    active_clients_.fetch_sub(1);
                    return;
                }

                case kWorking:
                case kWorked:
                case kRearming:
                case kStopped:
                case kClosing:
                case kClosed:
                {
                    active_clients_.fetch_sub(1);
                    return;
                }
            }
        }
    }

    template <typename PacketSinkType, typename ClientType>
    void ClientPool<PacketSinkType, ClientType>::init(
        StackNode<ClientType>* client,
        std::uint64_t uuid,
        ClientType&& sink)
    {
        using enum ClientState;

        active_clients_.fetch_add(1);
        if (runstate_.load() == RunState::kPoolStopped) {
            active_clients_.fetch_sub(1);
            return;
        }

        while (true) {
            ClientGenerationToken token
                = generation_tokens_[client->index].load();
            if (token.uuid != uuid) {
                active_clients_.fetch_sub(1);
                return;
            }

            switch (static_cast<ClientState>(token.state)) {
                case kReady:
                {
                    client->sink = std::move(sink);
                    maybe_set_timeout(client, uuid);
                    active_clients_.fetch_sub(1);
                    return;
                }

                case kRearming:
                case kWorking:
                case kWorked:
                case kStopped:
                case kClosing:
                case kClosed:
                {
                    active_clients_.fetch_sub(1);
                    return;
                }
            }
        }
    }

    template <typename PacketSinkType, typename ClientType>
    int ClientPool<PacketSinkType, ClientType>::write(
        StackNode<ClientType>* client,
        std::uint64_t uuid,
        const char* buff,
        std::uint32_t size)
    {
        using enum ClientState;

        active_clients_.fetch_add(1);
        if (runstate_.load() == RunState::kPoolStopped) {
            active_clients_.fetch_sub(1);
            return -1;
        }

        if (!set_client_writing(client, uuid, true)) {
            active_clients_.fetch_sub(1);
            return -1;
        }

        const auto [sfd, _uuid] = (client->sup).load();

        ClientType& sink = client->sink;
        int n_bytes = sink.write(sfd, buff, size);

        set_client_writing(client, uuid, false);

        active_clients_.fetch_sub(1);
        return n_bytes;
    }

    /*! Closes socket and pushes client to free stack.
     */
    template <typename PacketSinkType, typename ClientType>
    bool ClientPool<PacketSinkType, ClientType>::terminate_on_close(
        StackNode<ClientType>* client,
        std::uint64_t uuid)
    {
        active_clients_.fetch_add(1);
        if (runstate_.load() == RunState::kPoolStopped) {
            active_clients_.fetch_sub(1);
            return false;
        }

        bool ret = terminate_impl(
            client, uuid, [this, uuid](StackNode<ClientType>* value) {
                have_client_closed(value, uuid);
            });

        active_clients_.fetch_sub(1);
        return ret;
    }

    /*! Closes socket and pushes client to free stack.
     */
    template <typename PacketSinkType, typename ClientType>
    bool ClientPool<PacketSinkType, ClientType>::terminate_on_error(
        StackNode<ClientType>* client,
        std::uint64_t uuid)
    {
        active_clients_.fetch_add(1);
        if (runstate_.load() == RunState::kPoolStopped) {
            active_clients_.fetch_sub(1);
            return false;
        }

        bool ret = terminate_impl(
            client, uuid, [this, uuid](StackNode<ClientType>* value) {
                have_client_error(value, uuid);
            });

        active_clients_.fetch_sub(1);
        return ret;
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

        const auto [sfd, _uuid] = (client->sup).load();

        // Remove from timeout handler
        maybe_unset_timeout(client, uuid);
        // Close socket descriptor
        epoll_.remove(sfd);
        endpoint_close(sfd);

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
        active_clients_.fetch_add(1);
        if (runstate_.load() == RunState::kPoolStopped) {
            active_clients_.fetch_sub(1);
            return;
        }

        const auto [_writing, uuid, _state]
            = generation_tokens_[client->index].load();

        const auto [sfd, _uuid] = (client->sup).load();
        trigger_impl(client, uuid, sfd, flags);

        active_clients_.fetch_sub(1);
    }

    /*! Called on triggered event.
     */
    template <typename PacketSinkType, typename ClientType>
    void ClientPool<PacketSinkType, ClientType>::trigger_impl(
        StackNode<ClientType>* client,
        std::uint64_t uuid,
        std::int32_t sfd,
        std::uint32_t flags)
    {
        if (flags & EPOLLERR) {
            terminate_on_error(client, uuid);
            return;
        }

        if ((flags & EPOLLHUP) || (flags & EPOLLRDHUP)) {
            terminate_on_close(client, uuid);
            return;
        }

        if ((flags & EPOLLPRI) || (flags & EPOLLIN)) {
            maybe_reset_timeout(client, uuid);
        }

        if (flags & EPOLLPRI) {
            pri_read_ready_triggered(client, sfd, uuid);
        }

        if (flags & EPOLLIN) {
            read_ready_triggered(client, sfd, uuid);
        }
    }

    /*! EPOLLPRI event handler
     */
    template <typename PacketSinkType, typename ClientType>
    void ClientPool<PacketSinkType, ClientType>::pri_read_ready_triggered(
        StackNode<ClientType>* const client,
        std::int32_t sfd,
        std::uint64_t uuid)
    {
        while (set_client_working(client, uuid)) {
            int nbytes = 0;
            int mark = 0;

            ClientType& sink = client->sink;
            const char* oobdata = sink.read_oob(sfd, &nbytes, &mark);
            set_client_worked(client, uuid);

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
    }

    /*! EPOLLIN event handler
     */
    template <typename PacketSinkType, typename ClientType>
    void ClientPool<PacketSinkType, ClientType>::read_ready_triggered(
        StackNode<ClientType>* const client,
        std::int32_t sfd,
        std::uint64_t uuid)
    {
        while (set_client_working(client, uuid)) {
            // Read incoming message
            int nbytes = -1;
            ClientType& sink = client->sink;
            const char* data = sink.read(sfd, &nbytes);
            set_client_worked(client, uuid);

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
    }
} // namespace fserv
