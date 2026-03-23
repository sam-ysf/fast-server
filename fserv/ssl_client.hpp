#pragma once

#include "ssl_impl.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <openssl/ssl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace fserv {
    //! @class SslClient
    /*! Remote SSL connection endpoint
     */
    class SslClient {
        static const int kBuffSize = 4096;
    public:
        virtual ~SslClient()
        {
            --*instance_;
            if (ssl_ && *instance_ == 0) {
                SSL_free(ssl_);
                ssl_ = nullptr;
            }
        }

        SslClient() = default;

        explicit SslClient(SSL* ssl)
            : ssl_(ssl)
        {}

        SslClient(const SslClient& rhs)
            : instance_(rhs.instance_)
            , ssl_(rhs.ssl_)
        {
            ++*instance_;
            std::memcpy(message_buff_, rhs.message_buff_, kBuffSize);
        }

        SslClient& operator=(const SslClient& rhs)
        {
            if (this == &rhs) {
                return *this;
            }

            --*instance_;
            if (ssl_ && *instance_ == 0) {
                SSL_free(ssl_);
            }

            instance_ = rhs.instance_;
            ++*instance_;

            ssl_ = rhs.ssl_;
            std::memcpy(message_buff_, rhs.message_buff_, kBuffSize);

            return *this;
        }

        //! Reads data from the client.
        //! @param sfd
        //!     Socket file descriptor
        //! @param nbytes
        //!     Pointer to store the number of bytes read
        //! @return
        //!     Pointer to the buffer containing the data
        const char* read(int sfd, int* nbytes)
        {
            if (!ssl_) {
                return nullptr;
            }

            if ((SSL_get_fd(ssl_) != sfd) && SSL_set_fd(ssl_, sfd) != 1) {
                return nullptr;
            }

            *nbytes = SSL_read(ssl_, message_buff_, kBuffSize);
            return message_buff_;
        }

        //! Reads out-of-band data from the client.
        //! @param sfd
        //!     Socket file descriptor
        //! @param nbytes
        //!     Pointer to store the number of bytes read
        //! @param mark
        //!     Pointer to store the result of SIOCATMARK socket operation
        //! @return
        //!     Pointer to the buffer containing the out-of-band data
        const char* read_oob(int sfd, int* nbytes, const int* mark)
        {
            if (!ssl_) {
                return nullptr;
            }

            if ((SSL_get_fd(ssl_) != sfd) && SSL_set_fd(ssl_, sfd) != 1) {
                return nullptr;
            }

            switch (*mark) {
                case -1:
                    [[fallthrough]];
                case 0:
                    return nullptr;
                default:
                {
                    *nbytes = SSL_read(ssl_, message_buff_, kBuffSize);
                    return message_buff_;
                }
            }
        }

        //! Writes data to the client.
        //! @param buff
        //!     Buffer containing the data
        //! @param size
        //!     Size of the buffer
        //! @return
        //!     Number of bytes written
        int write(int sfd, const char* buff, std::uint32_t size) const
        {
            if (!ssl_) {
                return 0;
            }

            if ((SSL_get_fd(ssl_) != sfd) && SSL_set_fd(ssl_, sfd) != 1) {
                return 0;
            }

            std::uint32_t total_size = size;

            while (size > 0) {
                int n = SSL_write(ssl_, buff, static_cast<int>(size));
                if (n == 0) {
                    break;
                }

                if (n < 0) {
                    return n;
                }

                size -= static_cast<std::uint32_t>(n);
                buff += n;
            }

            return static_cast<int>(total_size - size);
        }
    private:
        /*! Instance count */
        std::shared_ptr<std::atomic<std::uint64_t>> instance_
            = std::make_shared<std::atomic<std::uint64_t>>(1);
        /*! Ssl object */
        SSL* ssl_ = nullptr;
        /*! Stored buffer */
        char message_buff_[kBuffSize + 1] = {};
    };
} // namespace fserv
