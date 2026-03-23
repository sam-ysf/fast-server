/* ssl_echo_server.hpp -- v1.0
   Stateless server with TLS that echoes back received messages */

#include "echo_server_base.hpp"
#include <memory>
#include <string>

namespace app {
    //! @class EchoServer
    /*! Sample server that echoes-back client messages
     */
    class SslEchoServer : public EchoServerBase {
    public:
        /*! @brief Ctor.
         */
        SslEchoServer(const std::string& certificate_file_path,
                      const std::string& private_key_file_path,
                      const std::string& certificate_password = std::string(),
                      const std::string& private_key_password = std::string());

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
