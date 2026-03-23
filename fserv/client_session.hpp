/*! client_session.hpp -- v1.0
    Exposes necessary client methods to downstream client event handlers */

#pragma once

#include "atomic_stack.hpp"
#include "fserv/client_session_manager.hpp"
#include <atomic>
#include <cstdint>

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
        ClientSession(ClientSessionManager<ClientType>* session_manager,
                      StackNode<ClientType>* client,
                      std::uint64_t uuid)
            : session_manager_(session_manager)
            , client_ptr_(client)
            , uuid_(uuid)
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
                session_manager_->init(client_ptr, uuid_, std::move(sink));
                hazard_unlock();
            } catch (std::exception&) {
                hazard_unlock();
                throw;
            }

            return true;
        }

        bool init(const ClientType& sink)
        {
            StackNode<ClientType>* client_ptr = hazard_lock();
            if (!client_ptr) {
                hazard_unlock();
                return false;
            }

            try {
                session_manager_->init(client_ptr, uuid_, sink);
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
        //! @return
        //!     Number of bytes written
        int write(const char* buff, int size) const
        {
            return write(buff, static_cast<std::uint32_t>(size));
        }

        //! Writes data to client socket
        //! @param buff
        //!     Message buffer
        //! @param size
        //!     Message buffer size (bytes)
        //! @return
        //!     Number of bytes written
        int write(const char* buff, std::uint32_t size) const
        {
            StackNode<ClientType>* client_ptr = hazard_lock();
            if (!client_ptr) {
                hazard_unlock();
                return -1;
            }

            int nbytes = 0;
            try {
                nbytes = session_manager_->write(client_ptr, uuid_, buff, size);
            } catch (std::exception&) {
                hazard_unlock();
                throw;
            }

            hazard_unlock();

            return nbytes;
        }

        //! Reactivates the client for next read
        bool rearm()
        {
            StackNode<ClientType>* client_ptr = hazard_lock();
            if (!client_ptr) {
                hazard_unlock();
                return false;
            }

            bool ret = session_manager_->rearm(client_ptr, uuid_);
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

            bool ret = session_manager_->terminate(client_ptr, uuid_);

            client_ptr_.compare_exchange_strong(client_ptr, nullptr);
            hazard_unlock();

            return ret;
        }
    private:
        StackNode<ClientType>* hazard_lock() const
        {
            StackNode<ClientType>* client_ptr = nullptr;
            while (true) {
                client_ptr = client_ptr_.load();

                StackNode<ClientType>* expected = nullptr;
                if (hazard_ptr_.compare_exchange_strong(expected, client_ptr)) {
                    break;
                }
            }

            return client_ptr;
        }

        void hazard_unlock() const
        {
            hazard_ptr_.store(nullptr);
        }

        ClientSessionManager<ClientType>* session_manager_ = nullptr;
        // Encapsulated client
        std::atomic<StackNode<ClientType>*> client_ptr_ = nullptr;
        // Hazard pointer to client
        mutable std::atomic<StackNode<ClientType>*> hazard_ptr_ = nullptr;
        // Unique session identifier
        std::uint64_t uuid_ = 0;
    };
} // namespace fserv
