/* endpoint.hpp -- v1.0
   Network backend implementation */

#pragma once

#include <arpa/inet.h>
#include <cstdint>
#include <fcntl.h>
#include <unistd.h>

namespace fserv::util {

    //! Creates a TCP socket.
    //! @return
    //!     The socket file descriptor
    inline int endpoint_tcp()
    {
        return ::socket(AF_INET, SOCK_STREAM, 0);
    }

    //! Creates a TCP server socket.
    //! @param port
    //!     Port number
    //! @param queuelen
    //!     Backlog queue length for accept()
    //! @return
    //!     The socket file descriptor
    inline int endpoint_tcp_server(int port, int queuelen)
    {
        struct sockaddr_in addr = {};

        addr.sin_family = AF_INET;
        addr.sin_port = ::htons(port);
        addr.sin_addr.s_addr = ::htonl(INADDR_ANY);

        int sfd = endpoint_tcp();
        if (sfd == -1) {
            return -1;
        }

        int flags = 1;
        if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &flags, sizeof(int))
            == -1) {
            return -1;
        }

        // Bind to local socket
        if (::bind(sfd,
                   reinterpret_cast<struct sockaddr*>(&addr),
                   sizeof(struct sockaddr_in))
            == -1) {
            return ::close(sfd), -1;
        }

        // Start listening on socket
        if (::listen(sfd, queuelen) == -1) {
            return ::close(sfd), -1;
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
    inline int endpoint_udp_server(int port)
    {
        struct sockaddr_in addr = {};

        addr.sin_family = AF_INET;
        addr.sin_port = ::htons(port);
        addr.sin_addr.s_addr = ::htonl(INADDR_ANY);

        int sfd = endpoint_udp();
        if (sfd == -1) {
            return -1;
        }

        int flags = 1;
        if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &flags, sizeof(int))
            == -1) {
            return -1;
        }

        // bind to local socket
        if (bind(sfd,
                 reinterpret_cast<struct sockaddr*>(&addr),
                 sizeof(struct sockaddr_in))
            == -1) {
            return ::close(sfd), -1;
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
    inline int endpoint_read(int sfd, void* buff, int bufflen)
    {
        return ::recv(sfd, buff, bufflen, 0);
    }

    //! Reads out-of-band data from a socket.
    //! @param sfd
    //!     Socket file descriptor
    //! @param buff
    //!     Data buffer
    //! @return
    //!     Number of bytes read
    inline int endpoint_read_oob(int sfd, void* const buff)
    {
        return ::recv(sfd, buff, 1, MSG_OOB);
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
    inline int endpoint_write(int sfd, const void* buff, int bufflen)
    {
        return ::send(sfd, buff, bufflen, 0);
    }

    //! Writes data to socket.
    //! @param sfd
    //!     Socket file descriptor
    //! @param ipaddr
    //!     Remote ip address
    //! @param port
    //!     Remote port number
    //! @param buff
    //!     Data buffer
    //! @param bufflen
    //!     Data buffer length
    //! @return
    //!     Number of bytes written
    inline int endpoint_write(int sfd,
                              int ipaddr,
                              int port,
                              const void* buff,
                              int bufflen)
    {
        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = ::htons(port);
        addr.sin_addr.s_addr = ipaddr;

        return ::sendto(sfd,
                        buff,
                        bufflen,
                        0,
                        reinterpret_cast<struct sockaddr*>(&addr),
                        sizeof(sockaddr_in));
    }

    //! Writes data to socket.
    //! @param sfd
    //!     Socket file descriptor
    //! @param ipaddr
    //!     Remote ip address
    //! @param port
    //!     Port number
    //! @param buff
    //!     Data buffer
    //! @param bufflen
    //!     Data buffer length
    //! @return
    //!     Number of bytes written
    inline int endpoint_write(int sfd,
                              const char* const ipaddr,
                              int port,
                              void* buff,
                              int bufflen)
    {
        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = ::htons(port);
        addr.sin_addr.s_addr = ::inet_addr(ipaddr);

        return ::sendto(sfd,
                        buff,
                        bufflen,
                        0,
                        reinterpret_cast<struct sockaddr*>(&addr),
                        sizeof(sockaddr_in));
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
    inline int endpoint_connect(int sfd, const char* const ipaddr, int port)
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

        return ::accept(sfd, reinterpret_cast<struct sockaddr*>(&addr), &size);
    }
} // namespace fserv::util
