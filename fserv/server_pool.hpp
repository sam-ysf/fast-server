/* client_pool.hpp -- v1.0
   Wrapper around EpollWaiter instance that registers server sockets and
   handles all triggered server events */

#pragma once

#include "client_pool.hpp"
#include "server_session.hpp"
#include <map>
#include <mutex>
#include <sys/epoll.h>

namespace fserv {

    //! @class ServerPool
    /*! Encapsulates event handling for multiple server sockets and their
     *  clients
     */
    template <typename PacketSinkType, typename ClientType>
    class ServerPool {
    public:
        //! Ctor.
        //! @param packet_sink
        //!     Pointer to the packet sink
        explicit ServerPool(PacketSinkType* packet_sink)
            : client_pool_(packet_sink)
        {}

        //! Dtor.
        ~ServerPool()
        {
            stop();
            for (const auto& s: servers_) {
                const auto& server = s.second;
                util::endpoint_close(server.sfd);
            }
        }

        //! Starts listening on all server sockets.
        //! @param worker_count
        //!     Number of client handler threads
        //! @param max_client_count
        //!     Maximum number of clients
        //! @param timeout_interval
        //!     Timeout interval for client connections
        void run(int worker_count, int max_client_count, int timeout_interval)
        {
            {
                // Maybe start the server (if not already running)
                std::lock_guard<std::mutex> l(status_check_lock_);
                if (!client_pool_.run(
                        worker_count, max_client_count, timeout_interval)) {
                    return;
                }

                // Start server
                // Server instance listens on only one thread
                server_count_ = std::make_unique<std::atomic<int>>(1);
            }

            epoll_.wait(this);
        }

        //! Stops listening on all server sockets.
        void stop()
        {
            // Maybe stop the server (if already running)
            std::lock_guard<std::mutex> l(status_check_lock_);

            epoll_.close();
            client_pool_.stop();
        }

        //! Binds a listener socket to a port.
        //! @param port
        //!     Port number
        //! @param queuelen
        //!     Backlog queue length for accept()
        //! @return
        //!     True if binding is successful, false otherwise
        bool bind(int port, int queuelen)
        {
            std::lock_guard<std::mutex> l(status_check_lock_);
            return do_bind(port, queuelen);
        }

        //! Adds an existing listener socket.
        //! @param sfd
        //!     File descriptor
        //! @return
        //!     True if adding is successful, false otherwise
        bool add(int sfd)
        {
            std::lock_guard<std::mutex> l(status_check_lock_);
            return do_add(sfd);
        }

        //! Called on epoll event to handle connection requests.
        //! @param server
        //!     Pointer to the server session
        //! @param flags
        //!     Event flags
        void trigger(ServerSession* server, int flags);
    private:
        /* @helper */
        bool do_bind(int port, int queuelen)
        {
            int sfd = util::endpoint_tcp_server(port, queuelen);
            if (sfd == -1) {
                return false;
            }

            if (util::endpoint_unblock(sfd)) {
                return util::endpoint_close(sfd), false;
            }

            bool ret = do_add(sfd);
            if (!ret)
                util::endpoint_close(sfd);
            return ret;
        }

        /* @helper */
        bool do_add(int sfd)
        {
            int uuid = 1;
            if (!servers_.empty()) {
                auto top = servers_.begin();
                uuid = top->first + 1;
            }

            servers_[uuid] = ServerSession(uuid, sfd);
            ServerSession* server = &servers_[uuid];

            int flags = EPOLLIN | EPOLLET | EPOLLEXCLUSIVE;
            return epoll_.add(server, sfd, flags);
        }

        // Applied when starting and stopping the running instance
        mutable std::mutex status_check_lock_;

        // Synchronizes access to list of bound servers
        mutable std::mutex server_add_lock_;

        // Map of bound servers
        std::map<int, ServerSession, std::greater<>> servers_;

        // Worker count
        std::unique_ptr<std::atomic<int>> server_count_;

        // Client connections manager
        ClientPool<PacketSinkType, ClientType> client_pool_;

        //
        EpollWaiter<ServerPool<PacketSinkType, ClientType>, ServerSession>
            epoll_;
    };

    /*! Called on epoll event to handle connection requests.
     */
    template <typename PacketSinkType, typename ClientType>
    void ServerPool<PacketSinkType, ClientType>::trigger(ServerSession* server,
                                                         int flags)
    {
        switch (flags) {
            case EPOLLHUP:
            case EPOLLERR:
            {
                util::endpoint_close(server->sfd);
                break;
            }

            default:
            {
                int cfd;
                while ((cfd = util::endpoint_accept(server->sfd)) != -1) {
                    if (util::endpoint_unblock(cfd) != 0) {
                        util::endpoint_close(cfd);
                        continue;
                    }

                    client_pool_.add_client(cfd);
                }
            }
        }
    }
} // namespace fserv
