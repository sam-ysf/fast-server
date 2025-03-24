/*! client_session.hpp -- v1.0
    Exposes necessary client methods to downstream client event handlers */

#pragma once

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
        explicit ClientSession(ClientType* client, const int uuid)
            : client_ptr_(client)
            , uuid_(uuid)
        {}

        //! @return
        //!     Client uuid.
        int uuid() const
        {
            return uuid_;
        }

        //! Writes data to client socket.
        //! @param buff
        //!     Message buffer
        //! @param size
        //!     Message buffer size (bytes)
        //! @return
        //!     Number of bytes written
        int write(const char* buff, const int size) const
        {
            return client_ptr_->write(buff, size);
        }

        //! Reactivates the client for next read.
        void rearm()
        {
            client_ptr_->rearm();
        }

        //! Terminates the client.
        void terminate()
        {
            client_ptr_->terminate();
        }
    private:
        // Encapsulated client
        ClientType* client_ptr_ = nullptr;
        // Encapsulated client's unique identifier
        int uuid_ = 0;
    };
} // namespace fserv
