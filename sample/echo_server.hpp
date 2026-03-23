/* echo_server.hpp -- v1.0
   Stateless server that echoes back received messages */

#include "echo_server_base.hpp"
#include <cstdint>
#include <memory>

namespace app {
    //! @class EchoServer
    /*! Sample server that echoes-back client messages
     */
    class EchoServer : public EchoServerBase {
    public:
        ~EchoServer() override = default;

        /*! @brief Ctor.
         */
        EchoServer();

        /*! @brief Initializes server
         */
        bool init(std::uint16_t port) override;

        /*! @brief Runs server instance
         */
        void run(int max_workers,
                 int max_connections,
                 int timeout_interval) override;

        /*! @brief Stops running server
         */
        void stop() override;
    private:
        //! @class Impl
        /*! @brief Pimpl. idiom
         */
        class Impl;

        std::shared_ptr<Impl> impl_;
    };
} // namespace app
