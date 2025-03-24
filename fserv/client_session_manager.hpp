/* client_session_manager.hpp -- v1.0
   Interface for exposing session-related client pool methods */

#pragma once

namespace fserv {

    //! @class ClientSessionManager
    /*! Interface for exposing session-related client pool methods
     */
    template <typename ClientType>
    class ClientSessionManager {
    public:
        //! Reactivates socket descriptor
        //! Must be called by read handler in order to register the client to be
        //! triggered on subsequent events.
        //! @param client
        //!     Pointer to the client
        virtual void rearm(ClientType* client) = 0;

        //! Closes socket descriptor.
        //! @param client
        //!     Client to close
        virtual void terminate(ClientType* client) = 0;
    };
} // namespace fserv
