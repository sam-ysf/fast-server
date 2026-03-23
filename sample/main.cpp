/* main.cpp -- v1.0 */

#include "config.hpp"
#include "echo_server.hpp"
#include "ssl_echo_server.hpp"
#include <csignal>
#include <filesystem>
#include <ncurses.h>
#include <pwd.h>
#include <thread>

namespace {
    // Global run flag
    std::atomic<bool> g_run = true;

    //! @brief SIGINT handler
    //! @param signo Signal number
    void on_sigint(int signo)
    {
        // Sanity check before toggling flag
        if (signo == SIGINT) {
            g_run.store(false);
        }
    }
} // namespace

namespace {
    // Ncurses
    void print_inf(int i, const char* message)
    {
        mvprintw(i, 0, "[inf] .... %s", message);
        refresh();
    }

    // Ncurses
    void print_inf(int i, const char* message, int n)
    {
        mvprintw(i, 0, "[inf] .... %s: %d", message, n);
        refresh();
    }

    // Ncurses
    void print_inf(int i, const char* message, float n)
    {
        mvprintw(i, 0, "[inf] .... %s: %f", message, n);
        refresh();
    }
} // namespace

namespace {
    // Helper
    inline std::string get_home_path()
    {
        std::size_t bufflen = std::max<std::size_t>(
            static_cast<std::size_t>(sysconf(_SC_GETPW_R_SIZE_MAX)),
            std::numeric_limits<std::uint16_t>::max());
        auto data = std::make_unique<char[]>(bufflen);

        passwd* check = nullptr;
        passwd passwd_out = {};
        if (::getpwuid_r(::getuid(), &passwd_out, data.get(), bufflen, &check)
            || (check != &passwd_out)) {
            return std::filesystem::path();
        }

        return std::filesystem::path(passwd_out.pw_dir);
    }

    //! @brief Loads command line arguments
    bool load_env(std::unordered_map<std::string, std::string>& env,
                  char* const* argv,
                  int argc)
    {
        // Home directory is default directory
        env["config-path"]
            = get_home_path()
              / std::filesystem::path(".config/fserv/server/config.json");

        // Parse arguments
        for (int opt = -1; (opt = getopt(argc, argv, "D:h")) != -1;) {
            switch (opt) {
                case 'D':
                {
                    env["config-path"] = std::string(optarg);
                    break;
                }

                case 'h':
                    [[fallthrough]];
                default:
                {
                    std::fprintf(stderr,
                                 "usage: %s [-D </path/to/conf/file>] [-h]\n",
                                 argv[0]);
                    return false;
                }
            }
        }

        // Done
        return true;
    }

    //! @brief Starts echo server
    //! @return Pointer to the worker thread
    template <typename ServerType>
    inline std::unique_ptr<std::jthread> start_echo_server(
        ServerType* server,
        const app::Config& config)
    {
        const int max_workers = [&config] {
            auto itr = config.global_params.find("max-workers");
            if (itr == config.global_params.end()) {
                print_inf(1, "Max workers: not specified, defaulting to 1");
                return 1;
            }

            const int value = std::atoi(itr->second.c_str());
            if (value <= 0) {
                print_inf(
                    1, "Max workers: invalid value provided, defaulting to 1");
                return 1;
            }

            print_inf(1, "Max workers", value);
            return value;
        }();

        const int max_connections = [&config] {
            constexpr int kMaxConnections = 1024;

            auto itr = config.global_params.find("max-concurrent-connections");
            if (itr == config.global_params.end()) {
                print_inf(2,
                          "Max connections: not specified, using default",
                          kMaxConnections);
                return kMaxConnections;
            }

            const int value = std::atoi(itr->second.c_str());
            if (value <= 0) {
                print_inf(2,
                          "Max connections: invalid value, using default",
                          kMaxConnections);
                return kMaxConnections;
            }

            print_inf(2, "Max connections", value);
            return value;
        }();

        const int timeout_interval = [&config] {
            auto itr = config.global_params.find("timeout-interval");
            if (itr == config.global_params.end()) {
                print_inf(3, "Client timeout interval not specified, skipping");
                return 0;
            }

            const int value = std::atoi(itr->second.c_str());
            if (value < 0) {
                print_inf(3, "Client timeout interval invalid, skipping");
                return 0;
            }

            if (value == 0) {
                print_inf(3, "Client timeout disabled");
                return 0;
            }

            print_inf(3,
                      "Client timeout interval (s)",
                      static_cast<float>(value) / 1000.0F);
            return value;
        }();

        auto worker = std::make_unique<std::jthread>(&ServerType::run,
                                                     server,
                                                     max_workers,
                                                     max_connections,
                                                     timeout_interval);

        print_inf(4, "Server started");
        return worker;
    }

    //! @brief Initializes echo server
    //! @return Pointer to new server
    inline std::unique_ptr<app::EchoServer> generate_echo_server(
        const app::Config& config)
    {
        auto itr_port = config.global_params.find("server-port");
        if (itr_port == config.global_params.end()) {
            std::fprintf(stderr, "[err] ... Port not specified\n");
            return nullptr;
        }

        auto port
            = static_cast<std::uint16_t>(std::atoi(itr_port->second.c_str()));

        auto server = std::make_unique<app::EchoServer>();
        if (!server->init(port)) {
            return nullptr;
        }

        /* Print info */
        print_inf(0, "Server bound to port", port);

        return server;
    }

    //! @brief Initializes echo server
    //! @return Pointer to new server
    inline std::unique_ptr<app::SslEchoServer> generate_ssl_echo_server(
        const app::Config& config)
    {
        auto itr_port = config.global_params.find("server-port");
        if (itr_port == config.global_params.end()) {
            std::fprintf(stderr, "[err] ... Port not specified\n");
            return nullptr;
        }

        auto port
            = static_cast<std::uint16_t>(std::atoi(itr_port->second.c_str()));

        const auto certificate_file_path = config["ssl-certificate"];
        const auto certificate_password = config["ssl-certificate-password"];
        const auto private_key_file_path = config["ssl-private-key"];
        const auto private_key_password = config["ssl-private-key-password"];

        if (!certificate_file_path || !private_key_file_path) {
            return nullptr;
        }

        auto server = std::make_unique<app::SslEchoServer>(
            certificate_file_path.value(),
            private_key_file_path.value(),
            certificate_password.value_or(std::string()),
            private_key_password.value_or(std::string()));

        if (!server->init(port)) {
            return nullptr;
        }

        /* Print info */
        print_inf(0, "Ssl server bound to port", port);

        return server;
    }
} // namespace

int main(int argc, char** argv)
{
    // Init. signal handler
    if (signal(SIGINT, on_sigint) == SIG_ERR) {
        std::fprintf(stderr, "[err] ... Error setting SIGINT handler");
        return 1;
    }

    // Load env. from command line args
    std::unordered_map<std::string, std::string> env;
    if (!load_env(env, argv, argc)) {
        return 1;
    }

    std::unique_ptr<app::EchoServerBase> echo_server;

    // Load configuration from file, otherwise load defaults
    app::Config config = app::load_config(env["config-path"].c_str());

    if (config["network-protocol"]
        && (config["network-protocol"]).value() == "ssl") {
        echo_server = generate_ssl_echo_server(config);
        if (!echo_server) {
            return 1;
        }
    }

    else {
        echo_server = generate_echo_server(config);
        if (!echo_server) {
            return 1;
        }
    }

    auto echo_server_worker = start_echo_server(echo_server.get(), config);
    if (!echo_server_worker) {
        return 1;
    }

    while (g_run.load()) {
        /* Run loop */
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Cleanup and return
    echo_server->stop();
    echo_server_worker->join();

    // Ncurses...
    // End curses mode
    endwin();

    return 0;
}
