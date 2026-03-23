#pragma once

#include "client_session.hpp"
#include "server_pool.hpp"
#include "ssl_client_handler.hpp"
#include "ssl_impl.hpp"
#include <memory>
#include <optional>
#include <string>

namespace fserv {

    struct SslParams {
        std::string certificate_file_path;
        std::optional<std::string> certificate_password;
        std::string private_key_file_path;
        std::optional<std::string> private_key_password;
    };

    template <typename ClientType>
    class SslServer {
        // Default value
        static constexpr int kMaxWorkerCount = 1;
        // Default value
        static constexpr int kMaxClientCount = 100000;
        // Default value
        static constexpr int kQueueLen = 1000;

        using ClientHandler = SslClientHandler<ClientType>;
        using ServerHandler = ServerPool<ClientHandler, ClientType>;

        using ClientSessionType = ClientSession<ClientType>;
    public:
        //! Ctor.
        //! @param ssl_params
        //!     TLS certificate and key configuration
        explicit SslServer(const SslParams& ssl_params)
            : SslServer(ssl_params.certificate_file_path,
                        ssl_params.private_key_file_path,
                        ssl_params.certificate_password.value_or(std::string()),
                        ssl_params.private_key_password.value_or(std::string()))
        {}

        //! Ctor.
        SslServer(const std::string& certificate_file_path,
                  const std::string& private_key_file_path,
                  const std::string& certificate_password = std::string(),
                  const std::string& private_key_password = std::string())
        {
            if (detail::check_ssl_integrity(certificate_file_path,
                                            private_key_file_path,
                                            certificate_password,
                                            private_key_password)) {
                // Generate client query handler and bind API endpoints
                client_pool_
                    = std::make_unique<ClientHandler>(certificate_file_path,
                                                      private_key_file_path,
                                                      certificate_password,
                                                      private_key_password);
                // Generate the server
                server_pool_
                    = std::make_unique<ServerHandler>(client_pool_.get());
            }
        }

        /*! @brief Returns true if sucessfully initialzed, false otherwise
         */
        bool is_ok() const
        {
            return client_pool_ != nullptr && server_pool_ != nullptr;
        }

        /*! @brief Forwards event handler assignment
         */
        void bind_new_client_callback(
            const std::function<void(ClientSessionType&)>& fn)
        {
            client_pool_->bind_new_client_callback(fn);
        }

        /*! @brief Forwards event handler assignment
         */
        void bind_client_error_callback(
            const std::function<void(ClientSessionType&)>& fn)
        {
            client_pool_->bind_client_error_callback(fn);
        }

        /*! @brief Forwards event handler assignment
         */
        void bind_client_closed_callback(
            const std::function<void(ClientSessionType&)>& fn)
        {
            client_pool_->bind_client_closed_callback(fn);
        }

        /*! @brief Forwards event handler assignment
         */
        void bind_client_data_received_callback(
            const std::function<
                void(ClientSessionType&, const char*, const int)>& fn)
        {
            client_pool_->bind_data_received_callback(fn);
        }

        /*! @brief Forwards event handler assignment
         */
        void bind_oob_received_callback(
            const std::function<void(ClientSessionType&, const int, char)>& fn)
        {
            client_pool_->bind_oob_received_callback(fn);
        }

        /*! @brief Enters run loop
         */
        void run(int worker_count = kMaxWorkerCount,
                 int max_client_count = kMaxClientCount,
                 int timeout_interval = 0)
        {
            server_pool_->run(worker_count, max_client_count, timeout_interval);
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
        std::unique_ptr<ClientHandler> client_pool_;

        // Server handler backend
        std::unique_ptr<ServerHandler> server_pool_;
    };
} // namespace fserv
