#pragma once

#include "client_pool.hpp"
#include "client_session.hpp"
#include "fserv/ssl_client.hpp"
#include "ssl_impl.hpp"
#include <functional>
#include <memory>
#include <mutex>
#include <openssl/ssl.h>

namespace fserv {

    // Fwd. decl.
    template <typename ClientType>
    class SslClientHandler;

    template <typename ClientType>
    using ssl_client_error
        = enable_client_error<SslClientHandler<ClientType>, ClientType>;

    template <typename ClientType>
    using ssl_client_accepted
        = enable_client_accepted<SslClientHandler<ClientType>, ClientType>;

    template <typename ClientType>
    using ssl_client_closed
        = enable_client_closed<SslClientHandler<ClientType>, ClientType>;

    template <typename ClientType>
    using ssl_client_received
        = enable_client_data_received<SslClientHandler<ClientType>, ClientType>;

    template <typename ClientType>
    using ssl_client_oob_received
        = enable_client_oob_received<SslClientHandler<ClientType>, ClientType>;

    //! @class SslClientHandler
    /*! Wrapper that encapsulates a Ssl server and implements observer pattern
     *! to handle client read/close/disconnect events
     */
    template <typename ClientType>
    class SslClientHandler : public ssl_client_error<ClientType>,
                             public ssl_client_accepted<ClientType>,
                             public ssl_client_closed<ClientType>,
                             public ssl_client_received<ClientType>,
                             public ssl_client_oob_received<ClientType> {
        using ClientSessionType = ClientSession<ClientType>;

        using NewClientCallbackType = std::function<void(ClientSessionType&)>;

        using ClientErrorCallbackType = std::function<void(ClientSessionType&)>;

        using ClientClosedCallbackType
            = std::function<void(ClientSessionType&)>;

        using DataReceivedCallbackType = std::function<
            void(ClientSessionType&, const char*, std::uint64_t)>;

        using OobReceivedCallbackType
            = std::function<void(ClientSessionType&, char)>;
    public:
        //! Dtor.
        //!
        ~SslClientHandler()
        {
            if (ssl_ctx_) {
                SSL_CTX_free(ssl_ctx_);
            }
        }

        //! Ctor.
        //!
        SslClientHandler(const std::string& certificate_path,
                         const std::string& key_file_path,
                         unsigned stall_timeout_interval,
                         const std::string& certificate_password
                         = std::string(),
                         const std::string& key_file_password = std::string())
            : stall_timeout_interval_(stall_timeout_interval)
            , ssl_ctx_(detail::init_ssl_ctx(certificate_path,
                                            key_file_path,
                                            certificate_password,
                                            key_file_password))
        {}

        //! Handles client error.
        //! @param client
        //!     Triggered client
        void client_error(ClientSessionType& client)
        {
            std::shared_ptr<ClientErrorCallbackType> callback;
            {
                std::scoped_lock lock_callback_access(lock_callback_access_);
                callback = on_client_error_;
            }

            if (callback) {
                const auto& ref = *callback;
                ref(client);
            }
        }

        //! Handles client acceptance.
        //! @param client
        //!     Triggered client
        bool client_accepted(ClientSessionType& client, int sfd)
        {
            SSL* ssl = accept_new_ssl_instance(sfd);
            if (!ssl) {
                return false;
            }

            client.init(SslClient(ssl));

            std::shared_ptr<NewClientCallbackType> callback;

            {
                std::scoped_lock lock_callback_access(lock_callback_access_);
                callback = on_new_client_;
            }

            if (callback) {
                const auto& ref = *callback;
                ref(client);
            }

            return true;
        }

        //! Handles client closure.
        //! @param client
        //!     Triggered client
        void client_closed(ClientSessionType& client)
        {
            std::shared_ptr<ClientClosedCallbackType> callback;

            {
                std::scoped_lock lock_callback_access(lock_callback_access_);
                callback = on_client_closed_;
            }

            if (callback) {
                const auto& ref = *callback;
                ref(client);
            }
        }

        //! Handles client data received.
        //! @param client
        //!     Triggered client
        //! @param data
        //!     Message data
        //! @param size
        //!     Message data size
        void client_data_received(ClientSessionType& client,
                                  const char* data,
                                  std::uint64_t size)
        {
            std::shared_ptr<DataReceivedCallbackType> callback;

            {
                std::scoped_lock lock_callback_access(lock_callback_access_);
                callback = on_data_received_;
            }

            if (callback) {
                const auto& ref = *callback;
                ref(client, data, size);
            }
        }

        //! Handles client out-of-band data received.
        //! @param client
        //!     Triggered client
        //! @param oobdata
        //!     Out-of-band data
        void client_oob_received(ClientSessionType& client, char oobdata)
        {
            std::shared_ptr<OobReceivedCallbackType> callback;

            {
                std::scoped_lock lock_callback_access(lock_callback_access_);
                callback = on_oob_received_;
            }

            if (callback) {
                const auto& ref = *callback;
                ref(client, oobdata);
            }
        }

        //! Binds client error callback.
        //! @param fn
        //!     Callback function
        void bind_client_error_callback(
            const std::function<void(ClientSessionType&)>& fn)
        {
            std::scoped_lock lock_callback_access(lock_callback_access_);
            on_client_error_
                = std::make_shared<std::function<void(ClientSessionType&)>>(fn);
        }

        //! Binds new client callback.
        //! @param fn
        //!     Callback function
        void bind_new_client_callback(
            const std::function<void(ClientSessionType&)>& fn)
        {
            std::scoped_lock lock_callback_access(lock_callback_access_);
            on_new_client_
                = std::make_shared<std::function<void(ClientSessionType&)>>(fn);
        }

        //! Binds client closed callback.
        //! @param fn
        //!     Callback function
        void bind_client_closed_callback(
            const std::function<void(ClientSessionType&)>& fn)
        {
            std::scoped_lock lock_callback_access(lock_callback_access_);
            on_client_closed_
                = std::make_shared<std::function<void(ClientSessionType&)>>(fn);
        }

        //! Binds data received callback.
        //! @param fn
        //!     Callback function
        void bind_data_received_callback(
            const std::function<
                void(ClientSessionType&, const char*, std::uint64_t)>& fn)
        {
            std::scoped_lock lock_callback_access(lock_callback_access_);
            on_data_received_ = std::make_shared<std::function<void(
                ClientSessionType&, const char*, std::uint64_t)>>(fn);
        }

        //! Binds out-of-band data received callback.
        //! @param fn
        //!     Callback function
        void bind_oob_received_callback(
            const std::function<void(ClientSessionType&, char)>& fn)
        {
            std::scoped_lock lock_callback_access(lock_callback_access_);
            on_oob_received_ = std::make_shared<
                std::function<void(ClientSessionType&, char)>>(fn);
        }
    private:
        // Initializes the client TLS state.
        SSL* accept_new_ssl_instance(int sfd)
        {
            // Just in case
            if (ssl_ctx_ == nullptr) {
                return nullptr;
            }

            SSL* ssl = SSL_new(ssl_ctx_);
            if (ssl == nullptr) {
                return nullptr;
            }

            if (SSL_set_fd(ssl, sfd) != 1) {
                SSL_free(ssl);
                return nullptr;
            }

            if (!detail::accept_ssl(ssl, stall_timeout_interval_)) {
                SSL_free(ssl);
                return nullptr;
            }

            return ssl;
        }

        /*! Timeout interval before a stalled client handshake is cancelled */
        unsigned stall_timeout_interval_ = 0;

        /*! Ssl context */
        SSL_CTX* ssl_ctx_ = nullptr;

        /*! Primary access lock */
        std::mutex lock_callback_access_;

        /*! Event handler */
        std::shared_ptr<ClientErrorCallbackType> on_client_error_;

        /*! Event handler */
        std::shared_ptr<NewClientCallbackType> on_new_client_;

        /*! Event handler */
        std::shared_ptr<ClientClosedCallbackType> on_client_closed_;

        /*! Event handler */
        std::shared_ptr<DataReceivedCallbackType> on_data_received_;

        /*! Event handler */
        std::shared_ptr<OobReceivedCallbackType> on_oob_received_;
    };
} // namespace fserv
