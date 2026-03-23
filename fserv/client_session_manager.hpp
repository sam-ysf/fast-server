/* client_session_manager.hpp -- v1.0
   Interface for exposing session-related client pool methods */

#pragma once

#include "atomic_stack.hpp"
#include <memory>

namespace fserv {

    //! @class ClientSessionManager
    /*! Interface for exposing session-related client pool methods
     */
    template <typename ClientType>
    class ClientSessionManager : public std::enable_shared_from_this<
                                     ClientSessionManager<ClientType>> {
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
        virtual bool rearm(StackNode<ClientType>* client,
                           std::uint64_t uuid,
                           std::uint64_t pool_instance,
                           std::uint32_t additional_flags) = 0;

        //! Closes socket descriptor.
        //! @param client
        //!     Client to close
        //! @param uuid
        //!     Client generation identifier
        virtual bool terminate(StackNode<ClientType>* client,
                               std::uint64_t uuid,
                               std::uint64_t pool_instance) = 0;

        virtual void init(StackNode<ClientType>* client,
                          std::uint64_t uuid,
                          std::uint64_t pool_instance,
                          ClientType&& sink) = 0;

        virtual bool write(StackNode<ClientType>* client,
                           std::uint64_t uuid,
                           std::uint64_t pool_instance,
                           const char* buff,
                           std::uint64_t size,
                           std::uint64_t* nbytes_written) = 0;
    };
} // namespace fserv
