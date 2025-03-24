/* basic_client_handler.hpp -- v1.0
   Default packet sink that is invoked from client pool, implements observer
   pattern to trigger registered callbacks */

#pragma once

#include "client_pool.hpp"
#include "client_session.hpp"
#include <functional>
#include <memory>
#include <mutex>

namespace fserv {

    // Fwd. decl.
    template <typename ClientType>
    class BasicClientHandler;

    template <typename ClientType>
    using client_error
        = fserv::enable_client_error<BasicClientHandler<ClientType>,
                                     ClientType>;

    template <typename ClientType>
    using client_accepted
        = fserv::enable_client_accepted<BasicClientHandler<ClientType>,
                                        ClientType>;

    template <typename ClientType>
    using client_closed
        = fserv::enable_client_closed<BasicClientHandler<ClientType>,
                                      ClientType>;

    template <typename ClientType>
    using client_received
        = fserv::enable_client_data_received<BasicClientHandler<ClientType>,
                                             ClientType>;

    template <typename ClientType>
    using client_oob_received
        = fserv::enable_client_oob_received<BasicClientHandler<ClientType>,
                                            ClientType>;

    //! @class BasicClientHandler
    /*! Wrapper that encapsulates a server and implements observer pattern to
     *  handle client read/close/disconnect events
     */
    template <typename ClientType>
    class BasicClientHandler : public client_error<ClientType>,
                               public client_accepted<ClientType>,
                               public client_closed<ClientType>,
                               public client_received<ClientType>,
                               public client_oob_received<ClientType> {
        using ClientSessionType = ClientSession<ClientType>;

        using NewClientCallbackType = std::function<void(ClientSessionType&)>;

        using ClientErrorCallbackType = std::function<void(ClientSessionType&)>;

        using ClientClosedCallbackType
            = std::function<void(ClientSessionType&)>;

        using DataReceivedCallbackType
            = std::function<void(ClientSessionType&, const char*, const int)>;

        using OobReceivedCallbackType
            = std::function<void(ClientSessionType&, const char)>;
    public:
        //! Handles client error.
        //! @param client
        //!     Triggered client
        void client_error(ClientSessionType& client)
        {
            std::shared_ptr<NewClientCallbackType> callback;

            {
                std::lock_guard<std::mutex> lock_callback_access(
                    lock_callback_access_);
                callback = on_client_error_;
            }

            if (callback.get()) {
                const auto& ref = *on_client_error_;
                ref(client);
            }
        }

        //! Handles client acceptance.
        //! @param client
        //!     Triggered client
        void client_accepted(ClientSessionType& client)
        {
            std::shared_ptr<NewClientCallbackType> callback;

            {
                std::lock_guard<std::mutex> lock_callback_access(
                    lock_callback_access_);
                callback = on_new_client_;
            }

            if (callback.get()) {
                const auto& ref = *on_new_client_;
                ref(client);
            }
        }

        //! Handles client closure.
        //! @param client
        //!     Triggered client
        void client_closed(ClientSessionType& client)
        {
            std::shared_ptr<ClientClosedCallbackType> callback;

            {
                std::lock_guard<std::mutex> lock_callback_access(
                    lock_callback_access_);
                callback = on_client_closed_;
            }

            if (callback.get()) {
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
                                  const int size)
        {
            std::shared_ptr<DataReceivedCallbackType> callback;

            {
                std::lock_guard<std::mutex> lock_callback_access(
                    lock_callback_access_);
                callback = on_data_received_;
            }

            if (callback.get()) {
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
                std::lock_guard<std::mutex> lock_callback_access(
                    lock_callback_access_);
                callback = on_oob_received_;
            }

            if (callback.get()) {
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
            std::lock_guard<std::mutex> lock_callback_access(
                lock_callback_access_);
            on_client_error_
                = std::make_shared<std::function<void(ClientSessionType&)>>(fn);
        }

        //! Binds new client callback.
        //! @param fn
        //!     Callback function
        void bind_new_client_callback(
            const std::function<void(ClientSessionType&)>& fn)
        {
            std::lock_guard<std::mutex> lock_callback_access(
                lock_callback_access_);
            on_new_client_
                = std::make_shared<std::function<void(ClientSessionType&)>>(fn);
        }

        //! Binds client closed callback.
        //! @param fn
        //!     Callback function
        void bind_client_closed_callback(
            const std::function<void(ClientSessionType&)>& fn)
        {
            std::lock_guard<std::mutex> lock_callback_access(
                lock_callback_access_);
            on_client_closed_
                = std::make_shared<std::function<void(ClientSessionType&)>>(fn);
        }

        //! Binds data received callback.
        //! @param fn
        //!     Callback function
        void bind_data_received_callback(
            const std::function<
                void(ClientSessionType&, const char*, const int)>& fn)
        {
            std::lock_guard<std::mutex> lock_callback_access(
                lock_callback_access_);
            on_data_received_ = std::make_shared<std::function<void(
                ClientSessionType&, const char*, const int)>>(fn);
        }

        //! Binds out-of-band data received callback.
        //! @param fn
        //!     Callback function
        void bind_oob_received_callback(
            const std::function<void(ClientSessionType&, char)>& fn)
        {
            std::lock_guard<std::mutex> lock_callback_access(
                lock_callback_access_);
            on_oob_received_ = std::make_shared<
                std::function<void(ClientSessionType&, char)>>(fn);
        }
    private:
        /*! Primary access lock */
        std::mutex lock_callback_access_;

        /*! Event handler */
        std::shared_ptr<ClientClosedCallbackType> on_client_error_;

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
