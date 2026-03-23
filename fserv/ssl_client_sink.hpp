
#pragma once

#include "client_pool.hpp"
#include "fserv/endpoint.hpp"
#include "fserv/epoll.hpp"
#include "fserv/ssl_client_handler.hpp"
#include "ssl_impl.hpp"
#include <cstdint>
#include <memory>
#include <mutex>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <unordered_map>

namespace fserv {

    // Fwd. decl.
    template <typename ClientType>
    class SslClientSink;

    template <typename ClientType>
    class SslClientSink {
        // Incomplete connection session variables
        struct InProgressConnection {
            int sfd = 0;
            SSL* ssl = nullptr;
        };

        static const unsigned kEpollFlags = EPOLLIN | EPOLLOUT | EPOLLET
                                            | EPOLLHUP | EPOLLRDHUP | EPOLLPRI
                                            | EPOLLONESHOT;
    public:
        //! Dtor.
        //!
        ~SslClientSink()
        {
            {
                std::scoped_lock lock(lock_);
                running_ = false;
                pending_connections_epoll_.close(true);

                for (auto& [uuid, connection]: pending_connections_) {
                    endpoint_close(connection.sfd);
                    SSL_free(connection.ssl);
                }

                if (ssl_ctx_) {
                    SSL_CTX_free(ssl_ctx_);
                }
            }

            if (pending_connections_worker_) {
                pending_connections_worker_->join();
            }
        }

        //! Ctor.
        //!
        SslClientSink(const std::shared_ptr<
                          ClientPool<SslClientHandler<ClientType>, ClientType>>&
                          client_pool,
                      const std::string& certificate_path,
                      const std::string& key_file_path,
                      const std::string& certificate_password = std::string(),
                      const std::string& key_file_password = std::string())
            : client_pool_(client_pool)
            , ssl_ctx_(detail::init_ssl_ctx(certificate_path,
                                            key_file_path,
                                            certificate_password,
                                            key_file_password))
        {
            if (!ssl_ctx_) {
                return;
            }

            running_ = true;

            pending_connections_worker_
                = std::make_unique<std::jthread>([this]() {
                      {
                          std::scoped_lock lock(lock_);
                          if (!running_) {
                              return;
                          }
                          pending_connections_epoll_.run();
                      }

                      pending_connections_epoll_.wait(this);
                  });
        }

        void add_client(int sfd)
        {
            detail::SslConnectionAcceptStatus status
                = try_accept_connection(sfd);
            if (status == detail::SslConnectionAcceptStatus::kError) {
                endpoint_close(sfd);
            }
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

        //! Called on epoll event to handle connection handshake updates
        //! @param uuid
        //!     Unique client id
        //! @param flags
        //!     Event flags
        void trigger(void* uuid, std::uint32_t flags)
        {
            std::scoped_lock lock(lock_);

            if (!running_) {
                return;
            }

            auto itr = pending_connections_.find(uuid);
            // Just in case
            if (itr == pending_connections_.end()) {
                return;
            }

            InProgressConnection connection_vars = itr->second;

            detail::SslConnectionAcceptStatus status
                = detail::SslConnectionAcceptStatus::kError;
            if ((flags & EPOLLIN) || (flags & EPOLLOUT)) {
                status = try_again_accept_connection(uuid, connection_vars);
            }

            if (status == detail::SslConnectionAcceptStatus::kIncomplete) {
                pending_connections_epoll_.rearm(
                    uuid, connection_vars.sfd, kEpollFlags);
                return;
            }

            // Remove session from list of pending sessions
            pending_connections_.erase(pending_connections_.find(uuid));
            pending_connections_epoll_.remove(connection_vars.sfd);

            if (status == detail::SslConnectionAcceptStatus::kError) {
                SSL_free(connection_vars.ssl);
                endpoint_close(connection_vars.sfd);
            }
        }
    private:
        // Initializes the client TLS state.
        detail::SslConnectionAcceptStatus try_accept_connection(int sfd)
        {
            std::scoped_lock lock(lock_);

            // Just in case
            if (ssl_ctx_ == nullptr) {
                return detail::SslConnectionAcceptStatus::kError;
            }

            SSL* ssl = SSL_new(ssl_ctx_);
            if (ssl == nullptr) {
                return detail::SslConnectionAcceptStatus::kError;
            }

            if (SSL_set_fd(ssl, sfd) != 1) {
                SSL_free(ssl);
                return detail::SslConnectionAcceptStatus::kError;
            }

            detail::SslConnectionAcceptStatus status = detail::accept_ssl(ssl);
            if (status == detail::SslConnectionAcceptStatus::kError) {
                SSL_free(ssl);
                return detail::SslConnectionAcceptStatus::kError;
            }

            if (status == detail::SslConnectionAcceptStatus::kSuccess) {
                client_pool_->add_client(ClientType(ssl), sfd);
                return detail::SslConnectionAcceptStatus::kSuccess;
            }

            pending_connections_[reinterpret_cast<void*>(ssl)]
                = {.sfd = sfd, .ssl = ssl};

            // Handshake incomplete
            // Store descriptor and read on next event
            if (!pending_connections_epoll_.add(ssl, sfd, kEpollFlags)) {
                pending_connections_.erase(
                    pending_connections_.find(reinterpret_cast<void*>(ssl)));
                SSL_free(ssl);
                return detail::SslConnectionAcceptStatus::kError;
            }

            return detail::SslConnectionAcceptStatus::kIncomplete;
        }

        detail::SslConnectionAcceptStatus try_again_accept_connection(
            void* uuid,
            InProgressConnection& connections_vars)
        {
            detail::SslConnectionAcceptStatus status
                = detail::accept_ssl(connections_vars.ssl);
            switch (status) {
                // Handshake in progress case
                case detail::SslConnectionAcceptStatus::kIncomplete:
                {
                    return pending_connections_epoll_.rearm(
                               uuid, connections_vars.sfd, kEpollFlags)
                               ? detail::SslConnectionAcceptStatus::kIncomplete
                               : detail::SslConnectionAcceptStatus::kError;
                }

                // Handshake complete case
                case detail::SslConnectionAcceptStatus::kSuccess:
                {
                    client_pool_->add_client(ClientType(connections_vars.ssl),
                                             connections_vars.sfd);
                    return status;
                }

                // Have error or if peer closed connection case
                case detail::SslConnectionAcceptStatus::kError:
                {
                    return status;
                }
            }
        }

        std::unique_ptr<std::jthread> pending_connections_worker_;
        EpollWaiter<SslClientSink<ClientType>, void> pending_connections_epoll_;

        std::shared_ptr<ClientPool<SslClientHandler<ClientType>, ClientType>>
            client_pool_;

        std::mutex lock_;
        std::unordered_map<void*, InProgressConnection> pending_connections_;

        /*! Run flag */
        bool running_ = false;

        /*! Ssl context */
        SSL_CTX* ssl_ctx_ = nullptr;

        /*! Primary access lock */
        std::mutex lock_callback_access_;
    };
} // namespace fserv
