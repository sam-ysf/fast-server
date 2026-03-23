/* client_pool.hpp -- v1.0
   Wrapper around EpollWaiter instance that registers server sockets and
   handles all triggered server events */

#pragma once

#include "client_pool.hpp"
#include "server_session.hpp"
#include <exception>
#include <map>
#include <mutex>
#include <stdexcept>
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
            try {
                stop();
                for (const auto& [key, server]: servers_) {
                    endpoint_close(server.sfd);
                }
            } catch (std::exception& e) {
                fprintf(stderr, "%s", e.what());
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
            run(static_cast<unsigned>(worker_count),
                static_cast<unsigned>(max_client_count),
                static_cast<unsigned>(timeout_interval));
        }

        //! Starts listening on all server sockets.
        //! @param worker_count
        //!     Number of client handler threads
        //! @param max_client_count
        //!     Maximum number of clients
        //! @param timeout_interval
        //!     Timeout interval for client connections
        void run(unsigned worker_count,
                 unsigned max_client_count,
                 unsigned timeout_interval)
        {
            {
                // Maybe start the server (if not already running)
                std::scoped_lock l(status_check_lock_);
                epoll_.run();
                if (!client_pool_.run(
                        worker_count, max_client_count, timeout_interval, 1)) {
                    return;
                }
            }

            epoll_.wait(this);
        }

        //! Stops listening on all server sockets.
        void stop() noexcept(false)
        {
            // Maybe stop the server (if already running)
            std::scoped_lock l(status_check_lock_);

            epoll_.close();
            client_pool_.stop();
        }

        //! Binds a IpV4 listener socket to a port.
        //! @param port
        //!     Port number
        //! @param queuelen
        //!     Backlog queue length for accept()
        //! @return
        //!     True if binding is successful, false otherwise
        bool bind4(std::uint16_t port, int queuelen)
        {
            std::scoped_lock l(status_check_lock_);
            return do_bind_v4(port, queuelen);
        }

        //! Binds a IpV6 listener socket to a port.
        //! @param port
        //!     Port number
        //! @param queuelen
        //!     Backlog queue length for accept()
        //! @return
        //!     True if binding is successful, false otherwise
        bool bind6(std::uint16_t port, int queuelen)
        {
            std::scoped_lock l(status_check_lock_);
            return do_bind_v6(port, queuelen);
        }

        //! Adds an existing listener socket.
        //! @param sfd
        //!     File descriptor
        //! @return
        //!     True if adding is successful, false otherwise
        bool add(int sfd)
        {
            std::scoped_lock l(status_check_lock_);
            return do_add(sfd);
        }

        //! Called on epoll event to handle connection requests.
        //! @param server
        //!     Pointer to the server session
        //! @param flags
        //!     Event flags
        void trigger(const ServerSession* server, std::uint32_t flags);
    private:
        /* @helper */
        bool do_bind_v4(std::uint16_t port, int queuelen)
        {
            int sfd = endpoint_tcp_v4_server(port, queuelen);
            if (sfd == -1) {
                return false;
            }

            if (endpoint_unblock(sfd)) {
                endpoint_close(sfd);
                return false;
            }

            bool ret = do_add(sfd);
            if (!ret)
                endpoint_close(sfd);
            return ret;
        }

        /* @helper */
        bool do_bind_v6(std::uint16_t port, int queuelen)
        {
            int sfd = endpoint_tcp_v6_server(port, queuelen);
            if (sfd == -1) {
                return false;
            }

            if (endpoint_unblock(sfd)) {
                endpoint_close(sfd);
                return false;
            }

            bool ret = do_add(sfd);
            if (!ret)
                endpoint_close(sfd);
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

            return epoll_.add(server, sfd, EPOLLIN | EPOLLET);
        }

        // Applied when starting and stopping the running instance
        mutable std::mutex status_check_lock_;

        // Synchronizes access to list of bound servers
        mutable std::mutex server_add_lock_;

        // Map of bound servers
        std::map<int, ServerSession, std::greater<>> servers_;

        // Client connections manager
        ClientPool<PacketSinkType, ClientType> client_pool_;

        // Epoll instance
        EpollWaiter<ServerPool<PacketSinkType, ClientType>, ServerSession>
            epoll_;
    };

    /*! Called on epoll event to handle connection requests.
     */
    template <typename PacketSinkType, typename ClientType>
    void ServerPool<PacketSinkType, ClientType>::trigger(
        const ServerSession* server,
        std::uint32_t flags)
    {
        if ((flags & EPOLLHUP) || (flags & EPOLLERR)) {
            endpoint_close(server->sfd);
            return;
        }

        int cfd;
        while ((cfd = endpoint_accept(server->sfd)) != -1) {
            if (endpoint_unblock(cfd) != 0) {
                endpoint_close(cfd);
                continue;
            }

            client_pool_.add_client(cfd, 1);
        }
    }
} // namespace fserv
