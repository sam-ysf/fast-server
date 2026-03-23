/* basic_client.hpp -- v1.0
   Encapsulates a client socket */

#pragma once

#include "endpoint.hpp"
#include <cstring>
#include <sys/ioctl.h>

namespace fserv {

    //! @class BasicClient
    /*! Remote connection endpoint
     */
    class BasicClient {
        static const int kBuffSize = 4096;
    public:
        virtual ~BasicClient() = default;

        BasicClient() = default;

        BasicClient(const BasicClient& rhs)
        {
            std::memcpy(message_buff_, rhs.message_buff_, kBuffSize);
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
        //! @param nbytes
        //!     Pointer to store the number of bytes read
        //! @return
        //!     Pointer to the buffer containing the data
        const char* read(int sfd, int* nbytes)
        {
            *nbytes = endpoint_read(sfd, message_buff_, kBuffSize);
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
            switch (*mark) {
                case -1:
                    [[fallthrough]];
                case 0:
                    return nullptr;
                default:
                {
                    *nbytes = endpoint_read_oob(sfd, message_buff_);
                    return message_buff_;
                }
            }
        }

        //! Writes data to the client.
        //! @param sfd
        //!     Client socket
        //! @param buff
        //!     Buffer containing the data
        //! @param size
        //!     Size of the buffer
        //! @return
        //!     Number of bytes written
        static int write(int sfd, const char* buff, std::uint32_t size)
        {
            std::uint32_t total_size = size;
            const char* ptr = buff;
            while (size > 0) {
                int n = endpoint_write(sfd, ptr, size);
                if (n == 0) {
                    break;
                }

                if (n < 0) {
                    return n;
                }

                size -= static_cast<std::uint32_t>(n);
                ptr += n;
            }

            return static_cast<int>(total_size - size);
        }
    private:
        /*! Read message buffer */
        char message_buff_[kBuffSize + 1] = {};
    };
} // namespace fserv
