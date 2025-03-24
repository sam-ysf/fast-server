/* basic_client.hpp -- v1.0
   Encapsulates a client socket */

#pragma once

#include "client_session_manager.hpp"
#include "endpoint.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/ioctl.h>

namespace fserv {

    //! @class BasicClient
    /*! Remote connection endpoint
     */
    class BasicClient {
        static const int kBuffSize = 4096;
    public:
        //! Default constructor.
        BasicClient() = default;

        //! Ctor.
        //! @param sfd
        //!     Socket file descriptor
        //! @param session_manager
        //!     Pointer to the session manager
        BasicClient(const int sfd,
                    ClientSessionManager<BasicClient>* session_manager)
            : sfd_(sfd)
            , session_manager_(session_manager)
        {}

        //! Reads data from the client.
        //! @param nbytes
        //!     Pointer to store the number of bytes read
        //! @return
        //!     Pointer to the buffer containing the data
        const char* read(int* nbytes)
        {
            *nbytes = util::endpoint_read(sfd_, message_buff_, kBuffSize);
            return message_buff_;
        }

        //! Reads out-of-band data from the client.
        //! @param oobdata
        //!     Pointer to store the out-of-band data
        //! @return
        //!     Number of bytes read
        int read_oob(char* oobdata)
        {
            int mark;
            ::ioctl(sfd_, SIOCATMARK, &mark);
            switch (mark) {
                case -1:
                    return -1;
                case 0:
                    return 0;
                default:
                    return util::endpoint_read_oob(sfd_, oobdata);
            }
        }

        //! Writes data to the client.
        //! @param buff
        //!     Buffer containing the data
        //! @param size
        //!     Size of the buffer
        //! @return
        //!     Number of bytes written
        int write(const char* buff, int size) const
        {
            int total_size = size;

            while (size > 0) {
                int n = util::endpoint_write(sfd_, buff, size);
                if (n <= 0) {
                    break;
                }

                size -= n;
                buff += n;
            }

            return total_size - size;
        }

        //! Rearms the client for additional read.
        void rearm()
        {
            session_manager_->rearm(this);
        }

        //! Terminates the client.
        void terminate()
        {
            session_manager_->terminate(this);
        }
    private:
        /*! Socket descriptor */
        int sfd_ = 0;
        /*! Message buffer */
        char message_buff_[kBuffSize + 1] = {};
        /*! Upstream session manager */
        ClientSessionManager<BasicClient>* session_manager_ = nullptr;
    };
} // namespace fserv
