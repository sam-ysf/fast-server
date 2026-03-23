/* server_pool.hpp -- v1.0
   Wrapper around EpollWaiter instance that registers server sockets and
   handles all triggered server events */

#pragma once

#include "client_pool.hpp"
#include "server_session.hpp"
#include <cerrno>
#include <exception>
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
            : client_pool_(
                  std::make_shared<ClientPool<PacketSinkType, ClientType>>(
                      packet_sink))
        {}

        //! Dtor.
        ~ServerPool()
        {
            try {
                stop();
                std::scoped_lock l(serverlist_lock_);
                for (const std::shared_ptr<ServerSession>& session: servers_) {
                    endpoint_close(session->sfd);
                }
            } catch (std::exception& e) {
                fprintf(stderr, "%s", e.what());
            }
        }

        //! Starts listening on all server sockets.
        //! @param worker_count
        //!     Number of client handler threads
        //! @param max_client_count_hint
        //!     Maximum number of clients
        //! @param timeout_interval
        //!     Timeout interval for client connections
        void run(unsigned worker_count,
                 unsigned max_client_count_hint,
                 unsigned timeout_interval)
        {
            {
                // Maybe start the server (if not already running)
                std::scoped_lock l(run_lock_);

                running_.store(true);

                bool ret = client_pool_->run(
                    worker_count, max_client_count_hint, timeout_interval, 1);
                if (!ret) {
                    return;
                }

                epoll_.run();
            }

            epoll_.wait(this);
        }

        //! Stops listening on all server sockets.
        void stop() noexcept(false)
        {
            // Maybe stop the server (if already running)
            std::scoped_lock l(run_lock_);

            running_.store(false);

            epoll_.close();
            client_pool_->stop();
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
            std::scoped_lock l(run_lock_);
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
            std::scoped_lock l(run_lock_);
            return do_bind_v6(port, queuelen);
        }

        //! Adds an existing listener socket.
        //! @param sfd
        //!     File descriptor
        //! @return
        //!     True if adding is successful, false otherwise
        bool add(int sfd)
        {
            std::scoped_lock l(run_lock_);
            return do_add(sfd);
        }

        //! Called on epoll event to handle connection requests.
        //! @param server
        //!     Pointer to the server session
        //! @param flags
        //!     Event flags
        void trigger(const ServerSession* server, std::uint32_t flags);
    private:
        /* Helper */
        void add_clients(const ServerSession* server)
        {
            while (running_.load()) {
                int cfd = endpoint_accept(server->sfd);
                if (cfd == -1 && errno == EAGAIN) {
                    return;
                }

                if (cfd == -1) {
                    continue;
                }

                if (endpoint_unblock(cfd) != 0) {
                    endpoint_close(cfd);
                    continue;
                }

                client_pool_->add_client(cfd, 1);
            }
        }

        /* Helper */
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

        /* Helper */
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

        /* Helper */
        bool do_add(int sfd)
        {
            std::scoped_lock l(serverlist_lock_);

            // Must be non-blocking socket
            if (int flags = fcntl(sfd, F_GETFL, 0); (flags & O_NONBLOCK) == 0) {
                return false;
            }

            int uuid = 1;
            if (!servers_.empty()) {
                uuid = (servers_.back())->uuid + 1;
            }

            servers_.push_back(std::make_shared<ServerSession>(uuid, sfd));
            ServerSession* server = (servers_.back()).get();

            bool ret = epoll_.add(server, sfd, EPOLLIN | EPOLLET);
            if (!ret) {
                servers_.pop_back();
            }
            return ret;
        }

        std::atomic<bool> running_ = false;

        // Applied when starting and stopping the running instance
        mutable std::mutex run_lock_;

        // Synchronizes access to list of bound servers
        mutable std::mutex serverlist_lock_;

        // Map of bound servers
        std::vector<std::shared_ptr<ServerSession>> servers_;

        // Client connections manager
        std::shared_ptr<ClientPool<PacketSinkType, ClientType>> client_pool_;

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
            std::scoped_lock l(serverlist_lock_);
            auto itr = std::ranges::find_if(
                servers_,
                [server](const std::shared_ptr<ServerSession>& element) {
                    return element->uuid == server->uuid;
                });

            // Just in case
            if (itr == servers_.end()) {
                return;
            }

            endpoint_close(server->sfd);
            servers_.erase(itr);
            return;
        }

        add_clients(server);
    }
} // namespace fserv
