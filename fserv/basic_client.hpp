/* basic_client.hpp -- v1.0
   Encapsulates a client socket */

#pragma once

#include "endpoint.hpp"
#include "read_status.hpp"
#include <cerrno>
#include <cstring>
#include <functional>
#include <sys/ioctl.h>
#include <sys/socket.h>

namespace fserv {

    //! @class BasicClient
    /*! Remote connection endpoint
     */
    class BasicClient {
        static const std::size_t kBuffSize = 4096;
    public:
        static std::size_t buff_size()
        {
            return kBuffSize;
        }

        virtual ~BasicClient() = default;

        BasicClient() = default;

        BasicClient(BasicClient&& rhs) noexcept
        {
            std::memcpy(message_buff_, rhs.message_buff_, kBuffSize);
        }

        BasicClient(const BasicClient& rhs)
        {
            std::memcpy(message_buff_, rhs.message_buff_, kBuffSize);
        }

        BasicClient& operator=(BasicClient&& rhs) noexcept
        {
            if (this == &rhs) {
                return *this;
            }

            std::memcpy(message_buff_, rhs.message_buff_, kBuffSize);
            return *this;
        }

        BasicClient& operator=(const BasicClient& rhs)
        {
            if (this == &rhs) {
                return *this;
            }

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
            const std::function<void(char, bool)>& handle_oob_message)
        {
            bool at_oob_mark = ::sockatmark(sfd) == 1;

            int nbytes_read = 0;
            if (at_oob_mark) {
                nbytes_read = read_oob_impl(sfd);
            } else {
                nbytes_read = read_message_impl(sfd);
            }

            if (nbytes_read == 0) {
                return ReadStatus::kConnectionClosed;
            }

            // Check if nothing to read
            if (nbytes_read == -1 && errno == EAGAIN) {
                return ReadStatus::kWantReadIo;
            }

            // Check for error
            if (nbytes_read == -1) {
                return ReadStatus::kError;
            }

            int nbytes_remaining = 0;
            ::ioctl(sfd, FIONREAD, &nbytes_remaining);

            bool finished = nbytes_remaining == 0;

            if (at_oob_mark) {
                handle_oob_message(message_buff_[0], finished);
            } else {
                auto nbytes = static_cast<std::uint64_t>(nbytes_read);
                handle_message(message_buff_, nbytes, finished);
            }

            return finished ? ReadStatus::kOk : ReadStatus::kOkAndHaveMoreIo;
        }

        //! Writes data to the client.
        //! @param sfd
        //!     Client socket
        //! @param buff
        //!     Buffer containing the data
        //! @param size
        //!     Size of the buffer
        //! @param[out] nbytes_written
        //!     Number of bytes written
        static bool write(int sfd,
                          const char* buff,
                          std::uint64_t size,
                          std::uint64_t* nbytes_written)
        {
            *nbytes_written = 0;

            while (*nbytes_written < size) {
                std::uint64_t nbytes = *nbytes_written;

                int res = endpoint_write(sfd, buff + nbytes, size - nbytes);
                if (res == -1 && errno == EAGAIN) {
                    return true;
                }

                if (res == -1) {
                    return false;
                }

                *nbytes_written += static_cast<std::uint64_t>(res);
            }

            return true;
        }
    private:
        //! Reads data from the client.
        //! @param sfd
        //!     Socket file descriptor
        //! @return
        //!     Number of bytes read
        int read_message_impl(int sfd)
        {
            return endpoint_read(sfd, message_buff_, kBuffSize);
        }

        //! Reads out-of-band data from the client.
        //! @param sfd
        //!     Socket file descriptor
        //! @return
        //!     Number of bytes read
        int read_oob_impl(int sfd)
        {
            return endpoint_read_oob(sfd, message_buff_);
        }

        /*! Read message buffer */
        char message_buff_[kBuffSize + 1] = {};
    };
} // namespace fserv
