/*! client_session.hpp -- v1.0
    Exposes necessary client methods to downstream client event handlers */

#pragma once

#include "atomic_stack.hpp"
#include "fserv/client_session_manager.hpp"
#include <atomic>
#include <cstdint>
#include <memory>

namespace fserv {

    //! @class ClientSession
    /*! Encapsulates a client, used for exposing session-related methods to
     *  downstream event handlers
     */
    template <typename ClientType>
    class ClientSession {
    public:
        //! Ctor.
        //! @param client
        //!     Encapsulated client
        //! @param uuid
        //!     Encapsulated client unique id
        ClientSession(
            std::shared_ptr<ClientSessionManager<ClientType>> session_manager,
            StackNode<ClientType>* client,
            std::uint64_t uuid,
            std::uint64_t pool_instance)
            : session_manager_(session_manager)
            , client_ptr_(
                  std::make_shared<std::atomic<StackNode<ClientType>*>>(client))
            , uuid_(uuid)
            , pool_instance_(pool_instance)
        {}

        //! @return
        //!     Client uuid.
        std::uint64_t uuid() const
        {
            return uuid_;
        }

        bool init(ClientType&& sink)
        {
            StackNode<ClientType>* client_ptr = hazard_lock();
            if (!client_ptr) {
                hazard_unlock();
                return false;
            }

            try {
                session_manager_->init(
                    client_ptr, uuid_, pool_instance_, std::move(sink));
                hazard_unlock();
            } catch (std::exception&) {
                hazard_unlock();
                throw;
            }

            return true;
        }

        //! Writes data to client socket
        //! @param buff
        //!     Message buffer
        //! @param size
        //!     Message buffer size (bytes)
        //! @param[out] nbytes_written
        //!     Number of bytes written
        bool write(const char* buff,
                   std::uint64_t size,
                   std::uint64_t* nbytes_written)
        {
            StackNode<ClientType>* client_ptr = hazard_lock();
            if (!client_ptr) {
                hazard_unlock();
                return false;
            }

            try {
                bool ret = session_manager_->write(client_ptr,
                                                   uuid_,
                                                   pool_instance_,
                                                   buff,
                                                   size,
                                                   nbytes_written);
                hazard_unlock();
                return ret;
            } catch (std::exception&) {
                hazard_unlock();
                throw;
            }
        }

        //! Reactivates the client for next read
        bool rearm()
        {
            StackNode<ClientType>* client_ptr = hazard_lock();
            if (!client_ptr) {
                hazard_unlock();
                return false;
            }

            std::uint32_t additional_flags = 0;

            bool ret = session_manager_->rearm(
                client_ptr, uuid_, pool_instance_, additional_flags);
            hazard_unlock();

            return ret;
        }

        //! Terminates the client
        bool terminate()
        {
            StackNode<ClientType>* client_ptr = hazard_lock();
            if (!client_ptr) {
                hazard_unlock();
                return false;
            }

            bool ret = session_manager_->terminate(
                client_ptr, uuid_, pool_instance_);
            if (ret) {
                client_ptr_->compare_exchange_strong(client_ptr, nullptr);
            }

            hazard_unlock();
            return ret;
        }
    private:
        StackNode<ClientType>* hazard_lock() const
        {
            StackNode<ClientType>* client_ptr = nullptr;
            while (true) {
                client_ptr = client_ptr_->load();

                StackNode<ClientType>* expected = nullptr;
                if (hazard_ptr_->compare_exchange_strong(expected,
                                                         client_ptr)) {
                    break;
                }
            }

            return client_ptr;
        }

        void hazard_unlock() const
        {
            hazard_ptr_->store(nullptr);
        }

        std::shared_ptr<ClientSessionManager<ClientType>> session_manager_;
        // Encapsulated client
        std::shared_ptr<std::atomic<StackNode<ClientType>*>> client_ptr_;
        // Hazard pointer to client
        std::shared_ptr<std::atomic<StackNode<ClientType>*>> hazard_ptr_
            = std::make_shared<std::atomic<StackNode<ClientType>*>>(nullptr);
        // Unique session identifier
        std::uint64_t uuid_ = 0;
        // Unique pool runtime identifier
        std::uint64_t pool_instance_ = 0;
    };
} // namespace fserv
