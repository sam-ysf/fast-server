/* client_session_manager.hpp -- v1.0
   Interface for exposing session-related client pool methods */

#pragma once

#include "atomic_stack.hpp"

namespace fserv {

    //! @class ClientSessionManager
    /*! Interface for exposing session-related client pool methods
     */
    template <typename ClientType>
    class ClientSessionManager {
    public:
        //! Dtor.
        //!
        virtual ~ClientSessionManager() = default;

        //! Reactivates socket descriptor
        //! Must be called by read handler in order to register the client to be
        //! triggered on subsequent events.
        //! @param client
        //!     Pointer to the client
        //! @param uuid
        //!     Client generation identifier
        virtual bool rearm(StackNode<ClientType>* client, std::uint64_t uuid)
            = 0;

        //! Closes socket descriptor.
        //! @param client
        //!     Client to close
        //! @param uuid
        //!     Client generation identifier
        virtual bool terminate(StackNode<ClientType>* client,
                               std::uint64_t uuid) = 0;

        virtual void pin(StackNode<ClientType>* client, std::uint64_t uuid) = 0;

        virtual void unpin(StackNode<ClientType>* client, std::uint64_t uuid)
            = 0;
    };
} // namespace fserv
