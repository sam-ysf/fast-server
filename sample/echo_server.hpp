/* echo_server.hpp -- v1.0
   Stateless server that echoes back received messages */

#include <memory>

namespace app {
    //! @class EchoServer
    /*! Sample server that echoes-back client messages
     */
    class EchoServer {
    public:
        /*! @brief Ctor.
         */
        EchoServer();

        /*! @brief Initializes server
         */
        bool init(int port);

        /*! @brief Runs server instance
         */
        void run(int max_workers, int max_connections, int timeout_interval);

        /*! @brief Stops running server
         */
        void stop();
    private:
        //! @class Impl
        /*! @brief Pimpl. idiom
         */
        class Impl;

        std::shared_ptr<Impl> impl_;
    };
} // namespace app
