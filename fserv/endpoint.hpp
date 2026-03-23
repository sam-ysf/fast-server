/* endpoint.hpp -- v1.0
   Network backend implementation */

#pragma once

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <netinet/in.h>
#include <unistd.h>

namespace fserv {

    //! Creates a TCP socket.
    //! @return
    //!     The socket file descriptor
    inline int endpoint_tcp_v4()
    {
        return ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    }

    //! Creates a TCP socket.
    //! @return
    //!     The socket file descriptor
    inline int endpoint_tcp_v6()
    {
        return ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    }

    //! Creates a TCP server socket.
    //! @param port
    //!     Port number
    //! @param queuelen
    //!     Backlog queue length for accept()
    //! @return
    //!     The socket file descriptor
    inline int endpoint_tcp_v4_server(std::uint16_t port, int queuelen)
    {
        struct sockaddr_in addr = {};

        addr.sin_family = AF_INET;
        addr.sin_port = ::htons(port);
        addr.sin_addr.s_addr = ::htonl(INADDR_ANY);

        int sfd = endpoint_tcp_v4();
        if (sfd == -1) {
            return -1;
        }

        if (int flags = 1;
            ::setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &flags, sizeof(int))
            == -1) {
            ::close(sfd);
            return -1;
        }

        // Bind to local socket
        if (::bind(sfd,
                   reinterpret_cast<sockaddr*>(&addr),
                   sizeof(struct sockaddr_in))
            == -1) {
            ::close(sfd);
            return -1;
        }

        // Start listening on socket
        if (::listen(sfd, queuelen) == -1) {
            ::close(sfd);
            return -1;
        }

        return sfd;
    }

    //! Creates a TCP server socket.
    //! @param port
    //!     Port number
    //! @param queuelen
    //!     Backlog queue length for accept()
    //! @return
    //!     The socket file descriptor
    inline int endpoint_tcp_v6_server(std::uint16_t port, int queuelen)
    {
        struct sockaddr_in6 addr = {};

        addr.sin6_family = AF_INET6;
        addr.sin6_port = ::htons(port);
        addr.sin6_addr = in6addr_any;

        int sfd = endpoint_tcp_v6();
        if (sfd == -1) {
            return -1;
        }

        if (int flags = 1;
            ::setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &flags, sizeof(int))
            == -1) {
            ::close(sfd);
            return -1;
        }

        // Bind to local socket
        if (::bind(sfd,
                   reinterpret_cast<struct sockaddr*>(&addr),
                   sizeof(struct sockaddr_in6))
            == -1) {
            ::close(sfd);
            return -1;
        }

        // Start listening on socket
        if (::listen(sfd, queuelen) == -1) {
            ::close(sfd);
            return -1;
        }

        return sfd;
    }

    //! Creates a UDP socket.
    //! @return
    //!     The socket file descriptor
    inline int endpoint_udp()
    {
        return ::socket(AF_INET, SOCK_DGRAM, 0);
    }

    //! Creates a UDP server socket.
    //! @param port
    //!     Port number
    //! @return
    //!     The socket file descriptor
    inline int endpoint_udp_server(std::uint16_t port)
    {
        struct sockaddr_in addr = {};

        addr.sin_family = AF_INET;
        addr.sin_port = ::htons(port);
        addr.sin_addr.s_addr = ::htonl(INADDR_ANY);

        int sfd = endpoint_udp();
        if (sfd == -1) {
            return -1;
        }

        if (int flags = 1;
            ::setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &flags, sizeof(int))
            == -1) {
            ::close(sfd);
            return -1;
        }

        // bind to local socket
        if (bind(sfd,
                 reinterpret_cast<struct sockaddr*>(&addr),
                 sizeof(struct sockaddr_in))
            == -1) {
            ::close(sfd);
            return -1;
        }

        return sfd;
    }

    //! Reads data from a socket.
    //! @param sfd
    //!     Socket file descriptor
    //! @param buff
    //!     Data buffer
    //! @param bufflen
    //!     Data buffer length
    //! @return
    //!     Number of bytes read
    inline int endpoint_read(int sfd, char* buff, std::uint64_t bufflen)
    {
        ::ssize_t nbytes = 0;
        while (true) {
            nbytes = ::recv(sfd, buff, bufflen, 0);
            if (nbytes != -1 || errno != EINTR) {
                break;
            }
        }

        return static_cast<int>(nbytes);
    }

    //! Reads out-of-band data from a socket.
    //! @param sfd
    //!     Socket file descriptor
    //! @param buff
    //!     Data buffer
    //! @return
    //!     Number of bytes read
    inline int endpoint_read_oob(int sfd, char* const buff)
    {
        ::ssize_t nbytes = 0;
        while (true) {
            nbytes = ::recv(sfd, buff, 1, MSG_OOB);
            if (nbytes != -1 || errno != EINTR) {
                break;
            }
        }

        return static_cast<int>(nbytes);
    }

    //! Writes data to socket.
    //! @param sfd
    //!     Socket file descriptor
    //! @param buff
    //!     Data buffer
    //! @param bufflen
    //!     Data buffer length
    //! @return
    //!     Number of bytes written
    inline int endpoint_write(int sfd,
                              const char* buff,
                              std::uint64_t bufflen,
                              std::int32_t flags = 0)
    {
        ::ssize_t nbytes = 0;
        while (true) {
            nbytes = ::send(sfd, buff, bufflen, flags);
            if (nbytes != -1 || errno != EINTR) {
                break;
            }
        }

        return static_cast<int>(nbytes);
    }

    //! Connects to remote socket.
    //! @param sfd
    //!     Socket file descriptor
    //! @param ipaddr
    //!     Remote ip address
    //! @param port
    //!     Remote port number
    //! @return
    //!     Result of the connect call
    inline int endpoint_connect(int sfd,
                                const char* const ipaddr,
                                std::uint16_t port)
    {
        struct sockaddr_in addr = {};

        addr.sin_port = ::htons(port);
        addr.sin_addr.s_addr = ::inet_addr(ipaddr);
        addr.sin_family = AF_INET;

        return ::connect(sfd,
                         reinterpret_cast<struct sockaddr*>(&addr),
                         sizeof(struct sockaddr_in));
    }

    //! Sets a socket to non-blocking mode.
    //! @param sfd
    //!     Socket file descriptor
    //! @return
    //!     Result of the call to fcntl
    inline int endpoint_unblock(int sfd)
    {
        int flags = ::fcntl(sfd, F_GETFL, 0);
        return ::fcntl(sfd, F_SETFL, flags | O_NONBLOCK);
    }

    //! Closes a socket.
    //! @param sfd
    //!     Socket file descriptor
    //! @return
    //!     Result of the close call
    inline int endpoint_close(int sfd)
    {
        return ::close(sfd);
    }

    //! Accepts a connection on a socket.
    //! @param sfd
    //!     Socket file descriptor
    //! @return
    //!     The accepted socket file descriptor
    inline int endpoint_accept(int sfd)
    {
        struct sockaddr_in addr = {};
        socklen_t size = sizeof(struct sockaddr_in);

        int cfd = 0;
        while (true) {
            cfd = ::accept(
                sfd, reinterpret_cast<struct sockaddr*>(&addr), &size);
            if (cfd != -1 || errno != EINTR) {
                break;
            }
        }

        return cfd;
    }
} // namespace fserv
