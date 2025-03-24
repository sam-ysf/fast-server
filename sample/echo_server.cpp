/* echo_server.cpp -- v1.0 */

#include "echo_server.hpp"
#include "echo_server_impl.hpp"
#include "fserv/basic_client.hpp"
#include "fserv/basic_server.hpp"
#include <mutex>
#include <set>

class app::EchoServer::Impl {
public:
    Impl()
        : stats_(5)
    {}

    /*! @brief Initializes server, impl.
     */
    bool init(int port);

    /*! @brief Runs server (blocking), impl.
     */
    void run(int max_workers, int max_connections, int timeout_interval)
    {
        server_.run(max_workers, max_connections, timeout_interval);
    }

    /*! @brief Stops running server, impl.
     */
    void stop()
    {
        server_.stop();
    }
private:
    using ClientSessionType = fserv::ClientSession<fserv::BasicClient>;

    /*! @brief Event handler, called on new client
     */
    void handle_new_client(ClientSessionType& client)
    {
        std::lock_guard<std::mutex> l(callback_access_lock_);
        impl::handle_new_echo_client(client, active_sessions_, &stats_);
    }

    /*! @brief Event handler, called on client error
     */
    void handle_client_error(ClientSessionType& client)
    {
        std::lock_guard<std::mutex> l(callback_access_lock_);
        impl::handle_echo_client_error(client, active_sessions_, &stats_);
    }

    /*! @brief Event handler, called when client closed
     */
    void handle_client_closed(ClientSessionType& client)
    {
        std::lock_guard<std::mutex> l(callback_access_lock_);
        impl::handle_echo_client_closed(client, active_sessions_, &stats_);
    }

    /*! @brief Event handler, called when data received
     */
    void handle_client_data_received(ClientSessionType& client,
                                     const char* data,
                                     const int size)
    {
        std::lock_guard<std::mutex> l(callback_access_lock_);
        impl::handle_echo_client_data_received(
            client, data, size, active_sessions_, &stats_);
    }

    /* Records statistics */
    impl::Stats stats_;

    /* Primary access lock */
    std::mutex callback_access_lock_;

    /* Server backend instance */
    fserv::BasicServer<fserv::BasicClient> server_;

    /* Stored set of active sessions */
    std::set<int> active_sessions_;
};

bool app::EchoServer::Impl::init(int port)
{
    constexpr int kQueueLen = 100;
    if (!server_.bind(port, kQueueLen)) {
        std::printf("[err] Error binding echo server to port %d\n", port);
        return false;
    }

    const auto new_client_callback = [this](ClientSessionType& client) -> void {
        handle_new_client(client);
    };

    const auto client_error_callback
        = [this](ClientSessionType& client) -> void {
        handle_client_error(client);
    };

    const auto client_closed_callback
        = [this](ClientSessionType& client) -> void {
        handle_client_closed(client);
    };

    const auto client_data_received_callback = [this](ClientSessionType& client,
                                                      const char* data,
                                                      const int size) -> void {
        handle_client_data_received(client, data, size);
    };

    server_.bind_new_client_callback(new_client_callback);
    server_.bind_client_error_callback(client_error_callback);
    server_.bind_client_closed_callback(client_closed_callback);
    server_.bind_client_data_received_callback(client_data_received_callback);

    // Ncurses...
    // Start curses mode
    initscr();
    use_default_colors();

    return true;
}

app::EchoServer::EchoServer()
    : impl_(std::make_shared<Impl>())
{}

bool app::EchoServer::init(int port)
{
    return impl_->init(port);
}

void app::EchoServer::run(int max_workers,
                          int max_connections,
                          int timeout_interval)
{
    impl_->run(max_workers, max_connections, timeout_interval);
}

void app::EchoServer::stop()
{
    impl_->stop();
}
