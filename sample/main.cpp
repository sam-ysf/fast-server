/* main.cpp -- v1.0 */

#include "config.hpp"
#include "echo_server.hpp"
#include <csignal>
#include <ncurses.h>
#include <pwd.h>
#include <thread>

namespace {
    // Global run flag
    volatile bool g_run = true;

    //! @brief SIGINT handler
    //! @param signo Signal number
    void on_sigint(int signo)
    {
        // Sanity check before toggling flag
        if (signo == SIGINT) {
            g_run = false;
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
    //! @brief Loads command line arguments
    bool load_env(std::unordered_map<std::string, std::string>& env,
                  char* const* argv,
                  int argc)
    {
        // Home directory is default directory
        env["config-path"] = std::string(getpwuid(getuid())->pw_dir)
                             + std::string("/.config/fserv/server/config.json");

        // Parse arguments
        for (int opt = -1; (opt = getopt(argc, argv, "D:h")) != -1;) {
            switch (opt) {
                case 'D':
                {
                    env["config-path"] = std::string(optarg);
                    break;
                }

                case 'h':
                {
                    [[fallthrough]];
                }

                default:
                {
                    std::fprintf(stderr,
                                 "usage: %s [-D </path/to/conf/dir>] [-h]\n",
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
    inline std::unique_ptr<std::thread> start_echo_server(
        const std::unique_ptr<ServerType>& server,
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
            constexpr int kMinValue = 1024;

            auto itr = config.global_params.find("max-concurrent-connections");
            if (itr == config.global_params.end()) {
                print_inf(2,
                          "Max connections: not specified, using default "
                          "padded to page size",
                          kMinValue);
                return kMinValue;
            }

            const int value = std::atoi(itr->second.c_str());
            if (value <= 0) {
                print_inf(2,
                          "Max connections: invalid value, using default "
                          "padded to page size",
                          kMinValue);
                return kMinValue;
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
                      value / static_cast<float>(1e3));
            return value;
        }();

        auto worker = std::make_unique<std::thread>(&ServerType::run,
                                                    server.get(),
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

        int port = std::atoi(itr_port->second.c_str());

        /* Print info */
        print_inf(0, "Server bound to port", port);

        auto server = std::make_unique<app::EchoServer>();
        if (!server->init(port)) {
            return nullptr;
        }

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

    // Load configuration from file, otherwise load defaults
    app::Config config = app::load_config(env["config-path"].c_str());

    auto echo_server = generate_echo_server(config);
    if (!echo_server.get()) {
        return 1;
    }

    auto echo_server_worker = start_echo_server(echo_server, config);
    if (!echo_server_worker.get()) {
        return 1;
    }

    while (g_run) {
        /* Run loop */
    }

    // Cleanup and return
    echo_server->stop();
    echo_server_worker->join();

    endwin();

    return 0;
}
