/* echo_server_base.hpp -- v1.0
   Echo server abstract interface */

#pragma once

#include <cstdint>

namespace app {
    //! @class EchoServer
    /*! Sample server that echoes-back client messages
     */
    class EchoServerBase {
    public:
        virtual ~EchoServerBase() = default;

        /*! @brief Initializes server
         */
        virtual bool init(std::uint16_t port) = 0;

        /*! @brief Runs server instance
         */
        virtual void run(unsigned max_workers,
                         unsigned max_connections_hint,
                         unsigned timeout_interval) = 0;

        /*! @brief Stops running server
         */
        virtual void stop() = 0;
    };
} // namespace app
