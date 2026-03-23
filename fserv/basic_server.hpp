/* basic_server.hpp -- v1.0
   Facade interface that wraps a server pool and a basic client handler */

#pragma once

#include "basic_client_handler.hpp"
#include "client_session.hpp"
#include "fserv/client_pool.hpp"
#include "server_pool.hpp"
#include <memory>

namespace fserv {

    template <typename ClientType>
    class BasicClientSink {
    public:
        explicit BasicClientSink(
            const std::shared_ptr<ClientPool<BasicClientHandler<ClientType>,
                                             ClientType>>& client_pool)
            : client_pool_(client_pool)
        {}

        void add_client(int sfd)
        {
            client_pool_->add_client(ClientType(), sfd);
        }

        bool run(unsigned worker_count,
                 unsigned max_client_count_hint,
                 unsigned timeout_interval)
        {
            return client_pool_->run(
                worker_count, max_client_count_hint, timeout_interval);
        }

        void stop() noexcept
        {
            client_pool_->stop();
        }
    private:
        std::shared_ptr<ClientPool<BasicClientHandler<ClientType>, ClientType>>
            client_pool_;
    };

    //! class BasicServer
    /*! Wrapper that encapsulates a server pool and a client handler that
     *! implements observer pattern to handle client read/close/disconnect
     *! events
     */
    template <typename ClientType>
    class BasicServer {
        // Default value
        static constexpr int kMaxWorkerCount = 1;
        // Default value
        static constexpr int kMaxClientCount = 100000;
        // Default value
        static constexpr int kQueueLen = 1000;

        using ClientHandler = BasicClientHandler<ClientType>;
        using ClientSink = BasicClientSink<ClientType>;
        using ServerHandler
            = ServerPool<BasicClientSink<ClientType>, ClientType>;
        using ClientSessionType = ClientSession<ClientType>;
    public:
        /*! @brief Forwards event handler assignment
         */
        void bind_new_client_callback(
            const std::function<void(ClientSessionType&)>& fn)
        {
            client_handler_->bind_new_client_callback(fn);
        }

        /*! @brief Forwards event handler assignment
         */
        void bind_client_error_callback(
            const std::function<void(ClientSessionType&)>& fn)
        {
            client_handler_->bind_client_error_callback(fn);
        }

        /*! @brief Forwards event handler assignment
         */
        void bind_client_closed_callback(
            const std::function<void(ClientSessionType&)>& fn)
        {
            client_handler_->bind_client_closed_callback(fn);
        }

        /*! @brief Forwards event handler assignment
         */
        void bind_client_data_received_callback(
            const std::function<
                void(ClientSessionType&, const char*, std::uint64_t)>& fn)
        {
            client_handler_->bind_data_received_callback(fn);
        }

        /*! @brief Forwards event handler assignment
         */
        void bind_oob_received_callback(
            const std::function<void(ClientSessionType&, char)>& fn)
        {
            client_handler_->bind_oob_received_callback(fn);
        }

        /*! @brief Enters run loop
         */
        void run(unsigned worker_count = kMaxWorkerCount,
                 unsigned max_client_count_hint = kMaxClientCount,
                 unsigned timeout_interval = 0)
        {
            server_pool_->run(
                worker_count, max_client_count_hint, timeout_interval);
        }

        /*! @brief Stops run loop
         */
        void stop()
        {
            server_pool_->stop();
        }

        /*! @brief Creates ipv4 socket and listens on port
         */
        bool bind(std::uint16_t port, int queue_len = kQueueLen)
        {
            return bind4(port, queue_len);
        }

        /*! @brief Creates ipv4 socket and listens on port
         */
        bool bind4(std::uint16_t port, int queue_len = kQueueLen)
        {
            std::scoped_lock l(run_access_lock_);
            return server_pool_->bind4(port, queue_len);
        }

        /*! @brief Creates ipv6 socket and listens on port
         */
        bool bind6(std::uint16_t port, int queue_len = kQueueLen)
        {
            std::scoped_lock l(run_access_lock_);
            return server_pool_->bind6(port, queue_len);
        }

        /*! @brief Listens on existing socket
         */
        bool add(int sfd)
        {
            std::scoped_lock l(run_access_lock_);
            return server_pool_->add(sfd);
        }
    private:
        // Primary access lock
        std::mutex run_access_lock_;

        // Client handler backend
        std::shared_ptr<ClientHandler> client_handler_
            = std::make_shared<ClientHandler>();

        std::shared_ptr<ClientPool<ClientHandler, ClientType>> client_pool_
            = std::make_shared<ClientPool<ClientHandler, ClientType>>(
                client_handler_);

        std::shared_ptr<ClientSink> client_sink_
            = std::make_shared<ClientSink>(client_pool_);

        // Server handler backend
        std::unique_ptr<ServerHandler> server_pool_
            = std::make_unique<ServerHandler>(client_sink_);
    };
} // namespace fserv
