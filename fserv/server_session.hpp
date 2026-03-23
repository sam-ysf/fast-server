/* server_session.hpp -- v1.0
   Defines a server socket */

#pragma once

namespace fserv {

    //! @struct ServerSession
    /*! Server session variables
     */
    struct ServerSession {
        int uuid = 0;
        int sfd = 0;

        //! Ctor.
        ServerSession() = default;

        //! Ctor.
        ServerSession(int uuid, int sfd)
            : uuid(uuid)
            , sfd(sfd)
        {}
    };
} // namespace fserv
