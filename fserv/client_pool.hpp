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
#include "fserv/basic_client.hpp"
#include "fserv/endpoint.hpp"
#include "scoped_atomic_counter.hpp"
#include "std_memory.hpp"
#include "timeout_timer.hpp"
#include <atomic>
#include <exception>
#include <mutex>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <thread>
#include <type_traits>
#include <vector>

namespace fserv {

    enum class ClientPoolRunState : std::uint8_t {
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
        bool client_accepted(ClientSession<ClientType>& client, int sfd)
        {
            return static_cast<DerivedType*>(this)->client_accepted(client,
                                                                    sfd);
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
                                  std::uint64_t size)
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
        static const unsigned kEpollFlags = EPOLLIN | EPOLLET | EPOLLHUP
                                            | EPOLLRDHUP | EPOLLPRI
                                            | EPOLLONESHOT;
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
            ScopedAtomicCounter sc(active_clients_);

            if (runstate_.load() != ClientPoolRunState::kPoolReady) {
                endpoint_close(sfd);
                return;
            }

            auto [client, uuid] = clients_stack_.pop(sfd, thread_id);
            if (client == nullptr) {
                endpoint_close(sfd);
                return;
            }

            const auto rollback = [this, client, sfd]() {
                // Close socket descriptor
                endpoint_close(sfd);
                // Clear generation token
                generation_tokens_[client->index].token.store(
                    {.uuid = 0,
                     .state = static_cast<std::uint8_t>(ClientState::kClosed)});
                // Push back to stack of ready clients
                clients_stack_.push(client);
            };

            generation_tokens_[client->index].token.store(
                {.uuid = uuid,
                 .state = static_cast<std::uint8_t>(ClientState::kClosed)});

            if constexpr (std::is_base_of_v<
                              enable_client_accepted<PacketSinkType,
                                                     ClientType>,
                              PacketSinkType>) {
                try {
                    if (!have_client_accepted(client, uuid, sfd)) {
                        rollback();
                        return;
                    }
                } catch (...) {
                    rollback();
                    return;
                }
            }

            generation_tokens_[client->index].token.store(
                {.uuid = uuid,
                 .state
                 = static_cast<std::uint8_t>(ClientState::kInitialized)});

            maybe_set_timeout(client, uuid);

            if (!epoll_.add(client, sfd, kEpollFlags)) {
                terminate_impl(client, uuid, [](StackNode<ClientType>*) {
                    /* Nothing to do */
                });
                return;
            }

            set_client_ready(client, uuid);
        }

        //! Initializes and starts the pool.
        //! @param worker_count
        //!     Client handler thread count
        //! @param max_client_count_hint
        //!     Maximum number of clients
        //! @param timeout_interval
        //!     Client inactivity timeout interval
        //! @return
        //!     True if the pool is successfully started, false otherwise
        bool run(unsigned worker_count,
                 unsigned max_client_count_hint,
                 unsigned timeout_interval,
                 unsigned server_count)
        {
            std::scoped_lock l(status_check_lock_);

            // Only proceed if not already in running instance
            if (runstate_.load() == ClientPoolRunState::kPoolReady) {
                return false;
            }

            if (worker_count == 0 || max_client_count_hint == 0
                || server_count == 0) {
                return false;
            }

            // Allocate clients buffer
            if (!std_init(node_pool_,
                          static_cast<std::size_t>(max_client_count_hint))) {
                throw std::bad_alloc();
            }

            try {
                clients_stack_.init(node_pool_, server_count);

                std::size_t capacity = node_pool_.capacity;
                generation_tokens_
                    = std::vector<ClientGenerationState>(capacity);

                // Maybe run timeout timer
                if (timeout_interval > 0) {
                    timeout_interval_ = timeout_interval;
                    timeout_timer_.run(
                        &generation_tokens_,
                        timeout_interval_,
                        [this](std::vector<std::pair<StackNode<ClientType>*,
                                                     std::uint64_t>>&
                                   pruned_clients) {
                            have_client_timeouts(pruned_clients);
                        });
                }

                epoll_.run();
                for (unsigned i = 0; i < worker_count; ++i) {
                    workers_.emplace_back([this] {
                        epoll_.wait(this);
                    });
                }
            } catch (...) {
                stop_impl();
                throw;
            }

            runcount_.fetch_add(1);
            runstate_.store(ClientPoolRunState::kPoolReady);
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

            stop_impl();

            return true;
        }

        //! Reactivates client for next read.
        //! @param client
        //!     Client to rearm
        bool rearm(StackNode<ClientType>* client,
                   std::uint64_t uuid,
                   std::uint64_t pool_instance,
                   std::uint32_t additional_flags) override;

        //! Closes socket and pushes client to free stack.
        //! @param client
        //!     Client to terminate
        bool terminate(StackNode<ClientType>* client,
                       std::uint64_t uuid,
                       std::uint64_t pool_instance) override;

        void init(StackNode<ClientType>* client,
                  std::uint64_t uuid,
                  std::uint64_t pool_instance,
                  ClientType&& sink) override;

        bool write(StackNode<ClientType>* client,
                   std::uint64_t uuid,
                   std::uint64_t pool_instance,
                   const char* buff,
                   std::uint64_t size,
                   std::uint64_t* nbytes_written) override;

        //! Called on triggered event.
        //! @param client
        //!     Triggered client
        //! @param flags
        //!     Epoll event flags
        void trigger(StackNode<ClientType>* client, std::uint32_t flags);
    private:
        void stop_impl()
        {
            while (active_clients_.load() > 0) {
                /* Wait for active cients to finish work */
            }

            // Stop timeout thread
            timeout_timer_.stop();
            // Master thread initiates the shutdown daisy-chain
            epoll_.close();

            for (auto& thread: workers_) {
                thread.join();
            }

            workers_ = std::vector<std::jthread>();

            // Reset active clients...
            for (std::size_t i = 0; i < generation_tokens_.size(); ++i) {
                auto* client = &node_pool_.ptr_to_mem_slab[i];
                auto& generation_token = generation_tokens_[client->index];

                ClientGenerationToken token = generation_token.token.load();
                if (token.uuid > 0) {
                    // Remove from timeout handler
                    maybe_unset_timeout(client, token.uuid);
                    // Close & remove socket descriptor
                    epoll_.remove(client->sfd);
                    endpoint_close(client->sfd);
                    // Push back to stack of ready clients
                    clients_stack_.push(client);
                }
            }

            destroy(node_pool_);
            generation_tokens_ = std::vector<ClientGenerationState>();
        }

        bool is_valid_client(StackNode<ClientType>* client,
                             std::uint64_t tested_uuid) const
        {
            const auto& token = generation_tokens_[client->index].token;
            return (token.load()).uuid == tested_uuid;
        }

        bool set_client_closing(StackNode<ClientType>* client,
                                std::uint64_t tested_uuid)
        {
            using enum fserv::ClientState;

            ClientGenerationToken initialized
                = {.uuid = tested_uuid,
                   .state = static_cast<std::uint8_t>(kInitialized)};

            ClientGenerationToken ready
                = {.uuid = tested_uuid,
                   .state = static_cast<std::uint8_t>(kReady)};

            ClientGenerationToken working
                = {.uuid = tested_uuid,
                   .state = static_cast<std::uint8_t>(kWorking)};

            ClientGenerationToken worked
                = {.uuid = tested_uuid,
                   .state = static_cast<std::uint8_t>(kWorked)};

            ClientGenerationToken stopped
                = {.uuid = tested_uuid,
                   .state = static_cast<std::uint8_t>(kStopped)};

            const ClientGenerationToken next
                = {.uuid = 0, .state = static_cast<std::uint8_t>(kClosing)};

            auto& token = generation_tokens_[client->index].token;

            while (true) {
                const auto [uuid, state] = token.load();
                if (uuid != tested_uuid) {
                    return false;
                }

                switch (static_cast<ClientState>(state)) {
                    case kInitialized:
                    {
                        if (token.compare_exchange_strong(initialized, next))
                            return true;
                        break;
                    }

                    case kReady:
                    {
                        if (token.compare_exchange_strong(ready, next))
                            return true;
                        break;
                    }

                    case kWorking:
                    {
                        if (token.compare_exchange_strong(working, next))
                            return true;
                        break;
                    }

                    case kWorked:
                    {
                        if (token.compare_exchange_strong(worked, next))
                            return true;
                        break;
                    }

                    case kStopped:
                    {
                        if (token.compare_exchange_strong(stopped, next))
                            return true;
                        break;
                    }

                    case kRearming:
                    {
                        // Treat as spinlock
                        continue;
                    }

                    case kClosing:
                    case kClosed:
                    {
                        return false;
                    }
                }
            }
        }

        bool set_client_ready(StackNode<ClientType>* client,
                              std::uint64_t tested_uuid)
        {
            using enum fserv::ClientState;

            ClientGenerationToken initialized
                = {.uuid = tested_uuid,
                   .state = static_cast<std::uint8_t>(kInitialized)};

            ClientGenerationToken rearming
                = {.uuid = tested_uuid,
                   .state = static_cast<std::uint8_t>(kRearming)};

            const ClientGenerationToken next
                = {.uuid = tested_uuid,
                   .state = static_cast<std::uint8_t>(kReady)};

            auto& token = generation_tokens_[client->index].token;

            while (true) {
                const auto [uuid, state] = token.load();
                if (uuid != tested_uuid) {
                    return false;
                }

                switch (static_cast<ClientState>(state)) {
                    case kInitialized:
                    {
                        if (token.compare_exchange_strong(initialized, next))
                            return true;
                        break;
                    }

                    case kRearming:
                    {
                        if (token.compare_exchange_strong(rearming, next))
                            return true;
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
            }
        }

        bool set_client_rearming(StackNode<ClientType>* client,
                                 std::uint64_t tested_uuid)
        {
            using enum fserv::ClientState;

            ClientGenerationToken working
                = {.uuid = tested_uuid,
                   .state = static_cast<std::uint8_t>(kWorked)};

            const ClientGenerationToken next
                = {.uuid = tested_uuid,
                   .state = static_cast<std::uint8_t>(kRearming)};

            auto& token = generation_tokens_[client->index].token;

            while (true) {
                const auto [uuid, state] = token.load();
                if (uuid != tested_uuid) {
                    return false;
                }

                switch (static_cast<ClientState>(state)) {
                    case kWorked:
                    {
                        if (token.compare_exchange_strong(working, next))
                            return true;
                        break;
                    }

                    case kInitialized:
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
            }
        }

        bool set_client_working(StackNode<ClientType>* client,
                                std::uint64_t tested_uuid)
        {
            using enum ClientState;

            auto& token = generation_tokens_[client->index].token;

            ClientGenerationToken ready
                = {.uuid = tested_uuid,
                   .state = static_cast<std::uint8_t>(kReady)};

            ClientGenerationToken worked
                = {.uuid = tested_uuid,
                   .state = static_cast<std::uint8_t>(kWorked)};

            const ClientGenerationToken next
                = {.uuid = tested_uuid,
                   .state = static_cast<std::uint8_t>(kWorking)};

            while (true) {
                const auto [uuid, state] = token.load();
                if (uuid != tested_uuid) {
                    return false;
                }

                switch (static_cast<ClientState>(state)) {
                    case kReady:
                    {
                        if (token.compare_exchange_strong(ready, next))
                            return true;
                        break;
                    }

                    case kWorked:
                    {
                        if (token.compare_exchange_strong(worked, next))
                            return true;
                        break;
                    }

                    case kInitialized:
                    case kRearming:
                    {
                        // Busy wait until state transition completes in
                        // other thread
                        continue;
                    }

                    case kWorking:
                    case kStopped:
                    case kClosing:
                    case kClosed:
                    {
                        return false;
                    }
                }
            }
        }

        bool set_client_worked(StackNode<ClientType>* client,
                               std::uint64_t tested_uuid)
        {
            using enum ClientState;

            ClientGenerationToken rearming
                = {.uuid = tested_uuid,
                   .state = static_cast<std::uint8_t>(kRearming)};

            ClientGenerationToken working
                = {.uuid = tested_uuid,
                   .state = static_cast<std::uint8_t>(kWorking)};

            const ClientGenerationToken next
                = {.uuid = tested_uuid,
                   .state = static_cast<std::uint8_t>(kWorked)};

            auto& token = generation_tokens_[client->index].token;

            while (true) {
                const auto [uuid, state] = token.load();
                if (uuid != tested_uuid) {
                    return false;
                }

                switch (static_cast<ClientState>(state)) {
                    case kRearming:
                    {
                        // This state transition is an undo operation and
                        // only used if epoll rearm fails
                        if (token.compare_exchange_strong(rearming, next))
                            return true;
                        break;
                    }

                    case kWorking:
                    {
                        if (token.compare_exchange_strong(working, next))
                            return true;
                        break;
                    }

                    case kInitialized:
                    case kReady:
                    case kWorked:
                    case kStopped:
                    case kClosing:
                    case kClosed:
                    {
                        return false;
                    }
                }
            }
        }

        bool can_client_write(StackNode<ClientType>* client,
                              std::uint64_t tested_uuid)
        {
            using enum ClientState;

            auto& token = generation_tokens_[client->index].token;

            const auto [uuid, state] = token.load();

            switch (static_cast<ClientState>(state)) {
                case kReady:
                case kRearming:
                case kWorking:
                case kWorked:
                {
                    return uuid == tested_uuid;
                }

                case kInitialized:
                case kStopped:
                case kClosing:
                case kClosed:
                {
                    return false;
                }
            }
        }

        //! @param client
        //!     Triggered client
        template <typename Type = PacketSinkType>
        bool have_client_accepted(StackNode<ClientType>* client,
                                  std::uint64_t uuid,
                                  int sfd)
        {
            if constexpr (std::is_base_of_v<
                              enable_client_accepted<PacketSinkType,
                                                     ClientType>,
                              Type>) {
                ClientSession<ClientType> session(
                    ClientSessionManager<ClientType>::shared_from_this(),
                    client,
                    uuid,
                    runcount_.load());
                return packet_sink_->client_accepted(session, sfd);
            }
        }

        //! @param client
        //!     Triggered client
        template <typename Type = PacketSinkType>
        void have_client_closed(StackNode<ClientType>* client,
                                std::uint64_t uuid)
        {
            if constexpr (std::is_base_of_v<
                              enable_client_closed<PacketSinkType, ClientType>,
                              Type>) {
                ClientSession<ClientType> session(
                    ClientSessionManager<ClientType>::shared_from_this(),
                    client,
                    uuid,
                    runcount_.load());
                packet_sink_->client_closed(session);
            }
        }

        //! @param client
        //!     Triggered client
        template <typename Type = PacketSinkType>
        void have_client_error(StackNode<ClientType>* client,
                               std::uint64_t uuid)
        {
            if constexpr (std::is_base_of_v<
                              enable_client_error<PacketSinkType, ClientType>,
                              Type>) {
                ClientSession<ClientType> session(
                    ClientSessionManager<ClientType>::shared_from_this(),
                    client,
                    uuid,
                    runcount_.load());
                packet_sink_->client_error(session);
            }
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
            if constexpr (std::is_base_of_v<
                              enable_client_oob_received<PacketSinkType,
                                                         ClientType>,
                              Type>) {
                ClientSession<ClientType> session(
                    ClientSessionManager<ClientType>::shared_from_this(),
                    client,
                    uuid,
                    runcount_.load());
                packet_sink_->client_oob_received(session, oobdata);
            }
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
                                       std::uint64_t size)
        {
            if constexpr (std::is_base_of_v<
                              enable_client_data_received<PacketSinkType,
                                                          ClientType>,
                              Type>) {
                ClientSession<ClientType> session(
                    ClientSessionManager<ClientType>::shared_from_this(),
                    client,
                    uuid,
                    runcount_.load());
                packet_sink_->client_data_received(session, data, size);
            }
        }

        void have_client_timeouts(
            std::vector<std::pair<StackNode<ClientType>*, std::uint64_t>>&
                pruned_clients)
        {
            std::exception_ptr callback_error;

            auto itr = pruned_clients.begin();
            for (; itr < pruned_clients.end(); ++itr) {
                auto& [client, uuid] = *itr;
                try {
                    apply_client_timeout(client, uuid);
                } catch (...) {
                    callback_error = std::current_exception();
                    break;
                }
            }

            for (; itr < pruned_clients.end(); ++itr) {
                auto& [client, uuid] = *itr;
                apply_client_timeout_noreport(client, uuid);
            }

            if (callback_error) {
                std::rethrow_exception(callback_error);
            }
        }

        void apply_client_timeout(StackNode<ClientType>* client,
                                  std::uint64_t uuid)
        {
            using enum ClientState;

            ClientGenerationToken token
                = generation_tokens_[client->index].token.load();

            switch (static_cast<ClientState>(token.state)) {
                case kReady:
                case kStopped:
                {
                    if (token.uuid == uuid)
                        terminate_on_close(client, uuid);
                    break;
                }

                case kInitialized:
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

        void apply_client_timeout_noreport(StackNode<ClientType>* client,
                                           std::uint64_t uuid)
        {
            using enum ClientState;

            ClientGenerationToken token
                = generation_tokens_[client->index].token.load();

            switch (static_cast<ClientState>(token.state)) {
                case kReady:
                case kStopped:
                {
                    if (token.uuid == uuid) {
                        terminate_impl(
                            client, uuid, [](StackNode<ClientType>*) {
                                /* Nothing to do */
                            });
                    }
                    break;
                }

                case kInitialized:
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

        //! EPOLLIN | EPOLLPRI event handler
        void read_ready_triggered(StackNode<ClientType>* client,
                                  int sfd,
                                  std::uint64_t uuid);

        //! Called on triggered event.
        //! @param client
        //!     Triggered client
        //! @param flags
        //!     Epoll event flags
        void trigger_impl(StackNode<ClientType>* client,
                          std::uint64_t uuid,
                          int sfd,
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

        bool rearm_impl(StackNode<ClientType>* client,
                        std::uint64_t uuid,
                        std::uint32_t additional_flags);

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
        std::vector<ClientGenerationState> generation_tokens_;
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
        // Number of currently triggered clients or clients under initialization
        std::atomic<std::uint64_t> active_clients_ = 0;

        std::atomic<ClientPoolRunState> runstate_
            = ClientPoolRunState::kPoolStopped;
        std::atomic<std::uint64_t> runcount_ = 0;

        bool set_to_stopped()
        {
            using enum ClientPoolRunState;
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

                ClientPoolRunState expected = kPoolReady;
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
        std::uint64_t uuid,
        std::uint64_t pool_instance,
        std::uint32_t additional_flags)
    {
        ScopedAtomicCounter sc(active_clients_);

        if ((runstate_.load() != ClientPoolRunState::kPoolReady)
            || (runcount_.load() != pool_instance)) {
            return false;
        }

        return rearm_impl(client, uuid, additional_flags);
    }

    /*! Closes socket and pushes client to free stack.
     */
    template <typename PacketSinkType, typename ClientType>
    bool ClientPool<PacketSinkType, ClientType>::terminate(
        StackNode<ClientType>* client,
        std::uint64_t uuid,
        std::uint64_t pool_instance)
    {
        ScopedAtomicCounter sc(active_clients_);

        if ((runstate_.load() != ClientPoolRunState::kPoolReady)
            || (runcount_.load() != pool_instance)) {
            return false;
        }

        return terminate_impl(client, uuid, [](StackNode<ClientType>*) {
            /* No extra cleanup */
        });
    }

    template <typename PacketSinkType, typename ClientType>
    void ClientPool<PacketSinkType, ClientType>::init(
        StackNode<ClientType>* client,
        std::uint64_t uuid,
        std::uint64_t pool_instance,
        ClientType&& sink)
    {
        using enum ClientState;

        ScopedAtomicCounter sc(active_clients_);

        if ((runstate_.load() != ClientPoolRunState::kPoolReady)
            || (runcount_.load() != pool_instance)) {
            return;
        }

        while (true) {
            ClientGenerationToken token
                = generation_tokens_[client->index].token.load();
            if (token.uuid != uuid
                || token.state != static_cast<std::uint8_t>(kClosed)) {
                return;
            }

            switch (static_cast<ClientState>(token.state)) {
                case kClosed:
                {
                    client->sink = std::move(sink);
                    return;
                }

                case kInitialized:
                case kReady:
                case kRearming:
                case kWorking:
                case kWorked:
                case kStopped:
                case kClosing:
                {
                    return;
                }
            }
        }
    }

    template <typename PacketSinkType, typename ClientType>
    bool ClientPool<PacketSinkType, ClientType>::write(
        StackNode<ClientType>* client,
        std::uint64_t uuid,
        std::uint64_t pool_instance,
        const char* buff,
        std::uint64_t size,
        std::uint64_t* nbytes_written)
    {
        using enum ClientState;

        ScopedAtomicCounter sc_active_clients(active_clients_);

        if ((runstate_.load() != ClientPoolRunState::kPoolReady)
            || (runcount_.load() != pool_instance)) {
            return false;
        }

        if (!can_client_write(client, uuid)) {
            return false;
        }

        ScopedAtomicCounter sc_writers(
            generation_tokens_[client->index].n_writers);

        if (!can_client_write(client, uuid)) {
            return false;
        }

        maybe_reset_timeout(client, uuid);

        return (client->sink).write(client->sfd, buff, size, nbytes_written);
    }

    /*! Closes socket and pushes client to free stack.
     */
    template <typename PacketSinkType, typename ClientType>
    bool ClientPool<PacketSinkType, ClientType>::terminate_on_close(
        StackNode<ClientType>* client,
        std::uint64_t uuid)
    {
        ScopedAtomicCounter sc(active_clients_);

        if (runstate_.load() == ClientPoolRunState::kPoolStopped) {
            return false;
        }

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
        ScopedAtomicCounter sc(active_clients_);

        if (runstate_.load() == ClientPoolRunState::kPoolStopped) {
            return false;
        }

        return terminate_impl(
            client, uuid, [this, uuid](StackNode<ClientType>* value) {
                have_client_error(value, uuid);
            });
    }

    /*! Closes socket and pushes client to free stack.
     */
    template <typename PacketSinkType, typename ClientType>
    bool ClientPool<PacketSinkType, ClientType>::rearm_impl(
        StackNode<ClientType>* client,
        std::uint64_t uuid,
        std::uint32_t additional_flags)
    {
        if (!set_client_rearming(client, uuid)) {
            return false;
        }

        std::int32_t sfd = client->sfd;
        bool ret = epoll_.rearm(client, sfd, kEpollFlags | additional_flags);

        if (ret) {
            // Commit state change to ready
            maybe_reset_timeout(client, uuid);
            set_client_ready(client, uuid);
        } else {
            // Have error,
            // Roll back state change
            set_client_worked(client, uuid);
        }

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

        auto& generation_token = generation_tokens_[client->index];
        while (generation_token.n_writers > 0) {
            /* Wait until all writes finish */
        }

        // Remove from timeout handler
        maybe_unset_timeout(client, uuid);
        // Close socket descriptor
        epoll_.remove(client->sfd);
        endpoint_close(client->sfd);

        // Maybe handle exception
        std::exception_ptr callback_error;
        try {
            extra_cleanup_call(client);
        } catch (...) {
            callback_error = std::current_exception();
        }

        // Zero-out generation token and push client back to stack of ready
        // clients
        generation_token.token.store(
            {.uuid = 0,
             .state = static_cast<std::uint8_t>(ClientState::kClosed)});
        clients_stack_.push(client);

        if (callback_error) {
            std::rethrow_exception(callback_error);
        }

        return true;
    }

    /*! Called on triggered event.
     */
    template <typename PacketSinkType, typename ClientType>
    void ClientPool<PacketSinkType, ClientType>::trigger(
        StackNode<ClientType>* client,
        std::uint32_t flags)
    {
        ScopedAtomicCounter sc(active_clients_);

        if (runstate_.load() == ClientPoolRunState::kPoolStopped) {
            return;
        }

        const auto [uuid, _state]
            = generation_tokens_[client->index].token.load();

        int sfd = client->sfd;
        trigger_impl(client, uuid, sfd, flags);
    }

    /*! Called on triggered event.
     */
    template <typename PacketSinkType, typename ClientType>
    void ClientPool<PacketSinkType, ClientType>::trigger_impl(
        StackNode<ClientType>* client,
        std::uint64_t uuid,
        int sfd,
        std::uint32_t flags)
    {
        maybe_reset_timeout(client, uuid);

        if ((flags & EPOLLIN) || (flags & EPOLLOUT) || (flags & EPOLLPRI)) {
            read_ready_triggered(client, sfd, uuid);
        }

        if (flags & EPOLLERR) {
            terminate_on_error(client, uuid);
        }

        if ((flags & EPOLLHUP) || (flags & EPOLLRDHUP)) {
            terminate_on_close(client, uuid);
        }
    }

    /*! EPOLLIN | EPOLLPRI event handler
     */
    template <typename PacketSinkType, typename ClientType>
    void ClientPool<PacketSinkType, ClientType>::read_ready_triggered(
        StackNode<ClientType>* const client,
        int sfd,
        std::uint64_t uuid)
    {
        const auto message_handler
            = [this, client, uuid](const char* data,
                                   std::uint64_t nbytes,
                                   bool finished_reading) {
                  // Have actual data
                  // Hand it to client for processing...
                  if (finished_reading) {
                      set_client_worked(client, uuid);
                  }
                  have_client_data_received(client, uuid, data, nbytes);
              };

        const auto oob_message_handler
            = [this, client, uuid](char data, bool finished_reading) {
                  // Have actual data
                  // Hand it to client for processing...
                  if (finished_reading) {
                      set_client_worked(client, uuid);
                  }
                  have_client_oob_received(client, uuid, data);
              };

        if (!set_client_working(client, uuid)) {
            return;
        }

        while (is_valid_client(client, uuid)) {
            // Read socket buffer
            ReadStatus ret
                = client->sink.read(sfd, message_handler, oob_message_handler);

            if (!is_valid_client(client, uuid)) {
                break;
            }

            // Handle client close
            if (ret == ReadStatus::kConnectionClosed) {
                terminate_on_close(client, uuid);
                break;
            }

            // Non-blocking socket should rearm and return if no read
            // available
            if (ret == ReadStatus::kWantReadIo) {
                set_client_worked(client, uuid);
                if (!rearm_impl(client, uuid, 0)) {
                    terminate_on_error(client, uuid);
                }
                break;
            }

            // Non-blocking socket should rearm and return if pending write
            if (ret == ReadStatus::kWantWriteIo) {
                set_client_worked(client, uuid);
                if (!rearm_impl(client, uuid, EPOLLOUT)) {
                    terminate_on_error(client, uuid);
                }
                break;
            }

            // Handle error case
            if (ret == ReadStatus::kError) {
                terminate_on_error(client, uuid);
                break;
            }

            // Return if have no more data to read
            if (ret == ReadStatus::kOk) {
                break;
            }
        }
    }
} // namespace fserv
