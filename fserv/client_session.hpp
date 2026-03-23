/*! client_session.hpp -- v1.0
    Exposes necessary client methods to downstream client event handlers */

#pragma once

#include "atomic_stack.hpp"
#include "fserv/client_session_manager.hpp"
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

        template <typename ArgType>
        bool init(ArgType arg)
        {
            if (!client_ptr_) {
                return false;
            }

            ClientType& client = client_ptr_->value;
            return client.init(arg);
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
            if (!client_ptr_) {
                return 0;
            }

            session_manager_->pin(client_ptr_, uuid_);

            int sfd = client_ptr_->sfd;
            int nbytes = 0;
            try {
                ClientType& client = client_ptr_->value;
                nbytes = client.write(sfd, buff, size);
            } catch (std::exception&) {
                session_manager_->unpin(client_ptr_, uuid_);
                throw;
            }

            session_manager_->unpin(client_ptr_, uuid_);
            return nbytes;
        }

        //! Reactivates the client for next read
        bool rearm()
        {
            if (!client_ptr_) {
                return false;
            }

            return session_manager_->rearm(client_ptr_, uuid_);
        }

        //! Terminates the client
        bool terminate()
        {
            if (!client_ptr_) {
                return false;
            }

            StackNode<ClientType>* client_ptr = client_ptr_;
            client_ptr_ = nullptr;

            // Handle cleanup
            return session_manager_->terminate(client_ptr, uuid_);
        }
    private:
        ClientSessionManager<ClientType>* session_manager_ = nullptr;
        // Encapsulated client
        StackNode<ClientType>* client_ptr_ = nullptr;
        // Unique session identifier
        std::uint64_t uuid_ = 0;
    };
} // namespace fserv
