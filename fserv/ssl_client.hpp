#pragma once

#include "fserv/basic_client.hpp"
#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <limits>
#include <mutex>
#include <openssl/ssl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace fserv {
    //! @class SslClient
    /*! Remote SSL connection endpoint
     */
    class SslClient {
        static const std::size_t kBuffSize = 4096;
    public:
        static std::size_t buff_size()
        {
            return kBuffSize;
        }

        virtual ~SslClient()
        {
            if (ssl_) {
                SSL_free(ssl_);
                ssl_ = nullptr;
            }
        }

        SslClient() = default;

        explicit SslClient(SSL* ssl)
            : ssl_(ssl)
        {}

        SslClient(SslClient&& rhs) noexcept
            : ssl_(rhs.ssl_)
        {
            std::scoped_lock l(lock_, rhs.lock_);

            rhs.ssl_ = nullptr;
            std::memcpy(message_buff_, rhs.message_buff_, kBuffSize);
        }

        SslClient& operator=(SslClient&& rhs) noexcept
        {
            if (this == &rhs) {
                return *this;
            }

            std::scoped_lock l(lock_, rhs.lock_);

            if (ssl_) {
                SSL_free(ssl_);
            }

            ssl_ = rhs.ssl_;
            rhs.ssl_ = nullptr;

            std::memcpy(message_buff_, rhs.message_buff_, kBuffSize);

            return *this;
        }

        //! Reads data from the client.
        //! @param sfd
        //!     Socket file descriptor
        ReadStatus read(
            int sfd,
            const std::function<void(const char*, std::uint64_t, bool)>&
                handle_message,
            const std::function<void(char, bool)>& /* handle_oob_message */)
        {
            using enum ReadStatus;

            int nbytes_read = 0;
            int nbytes_remaining = 0;
            int ssl_error = 0;

            {
                std::scoped_lock<std::mutex> l(lock_);

                if (!ssl_) {
                    return kError;
                }

                if (SSL_get_fd(ssl_) != sfd) {
                    return kError;
                }

                while (true) {
                    nbytes_read = SSL_read(ssl_, message_buff_, kBuffSize);
                    // Retry if spurious return was caused by interrupt
                    if (nbytes_read != -1 || errno != EINTR) {
                        break;
                    }
                }

                ssl_error = SSL_get_error(ssl_, nbytes_read);
                nbytes_remaining = SSL_pending(ssl_);
            }

            bool finished = nbytes_remaining == 0;

            switch (ssl_error) {
                case SSL_ERROR_NONE:
                {
                    auto nbytes = static_cast<std::uint64_t>(nbytes_read);
                    handle_message(message_buff_, nbytes, finished);
                    return finished ? kOk : kOkAndHaveMoreIo;
                }

                case SSL_ERROR_ZERO_RETURN:
                {
                    return kConnectionClosed;
                }

                case SSL_ERROR_WANT_READ:
                {
                    return kWantReadIo;
                }

                case SSL_ERROR_WANT_WRITE:
                {
                    return kWantWriteIo;
                }

                default:
                {
                    return kError;
                }
            }
        }

        //! Writes data to the client.
        //! @param buff
        //!     Buffer containing the data
        //! @param size
        //!     Size of the buffer
        //! @param nbytes_written
        //!     Number of bytes written
        bool write(int sfd,
                   const char* buff,
                   std::uint64_t size,
                   std::uint64_t* nbytes_written) const
        {
            std::scoped_lock<std::mutex> l(lock_);

            *nbytes_written = 0;

            if (!ssl_) {
                return false;
            }

            if (SSL_get_fd(ssl_) != sfd) {
                return false;
            }

            while (*nbytes_written < size) {
                std::uint64_t nbytes = *nbytes_written;

                // Write must be capped to 32-bit int size before passing to ssl
                // write
                std::uint64_t nbytes_to_write = std::min<std::uint64_t>(
                    size - nbytes, std::numeric_limits<std::int32_t>::max());
                int res = write_impl(
                    buff + nbytes, static_cast<std::int32_t>(nbytes_to_write));

                int ssl_error = SSL_get_error(ssl_, res);
                if (ssl_error == SSL_ERROR_WANT_WRITE
                    || ssl_error == SSL_ERROR_WANT_READ) {
                    return true;
                }

                if (ssl_error != SSL_ERROR_NONE) {
                    return false;
                }

                *nbytes_written += static_cast<std::uint64_t>(res);
            }

            return true;
        }
    private:
        int write_impl(const char* buff, std::int32_t size) const
        {
            int nbytes_written = 0;
            while (true) {
                nbytes_written = SSL_write(ssl_, buff, size);
                if (nbytes_written != -1 || errno != EINTR) {
                    break;
                }
            }
            return nbytes_written;
        }

        /*! Concurrent operation on SSL object are not safe */
        mutable std::mutex lock_;
        /*! Ssl object */
        SSL* ssl_ = nullptr;
        /*! Stored buffer */
        char message_buff_[kBuffSize + 1] = {};
    };
} // namespace fserv
