/* config.cpp -- v1.0 */

#include "config.hpp"
#include "json.hpp"
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

namespace {

    template <typename T>
    inline void maybe_load_field(T&, const std::string&, const nlohmann::json&);

    template <>
    inline void maybe_load_field<std::string>(std::string& value,
                                              const std::string& key,
                                              const nlohmann::json& source)
    {
        auto j = source.find(key);
        if (j != source.end()) {
            const auto& v = j.value();
            if (v.is_string()) {
                value = static_cast<std::string>(v);
            }
        }
    }

    template <>
    inline void maybe_load_field<std::vector<std::string>>(
        std::vector<std::string>& value,
        const std::string& key,
        const nlohmann::json& source)
    {
        const auto j = source.find(key);
        if (j != source.end()) {
            const auto& v = j.value();
            if (v.is_array()) {
                value.clear();
                const auto& vec = v;
                for (auto itr = vec.begin(); itr != vec.end(); ++itr) {
                    value.push_back(itr.value());
                }
            }
        }
    }

    inline void save_config(const char* path, const app::Config& config)
    {
        nlohmann::json json_config;
        json_config["server-port"] = config.global_params.at("server-port");
        json_config["max-concurrent-connections"]
            = config.global_params.at("max-concurrent-connections");

        std::ofstream ofs(path);
        if (ofs.is_open()) {
            ofs << json_config.dump(2);
        }
    }
} // namespace

app::Config app::load_config(const char* path)
{
    static const std::string kDefaultPort = "60007";
    static const std::string kDefaultMaxConcurrentConnections = "50000";
    static const std::string kDefaultMaxWorkers = "2";

    Config config;

    // Load config defaults
    config.global_params["server-port"] = kDefaultPort;
    config.global_params["max-concurrent-connections"]
        = kDefaultMaxConcurrentConnections;
    config.global_params["max-workers"] = kDefaultMaxWorkers;

    // Create configuration directory (if not already exists)
    std::filesystem::path config_file_path(path);
    std::filesystem::path parent_dir_path = config_file_path.parent_path();
    if (!std::filesystem::exists(parent_dir_path)) {
        std::filesystem::create_directories(parent_dir_path);
    }

    // Load config from file
    std::ifstream ifs(config_file_path);
    if (!ifs.is_open()) {
        return save_config(config_file_path.c_str(), config), config;
    }

    nlohmann::json loaded_config;

    try {
        loaded_config = nlohmann::json::parse(ifs);
        ifs.close();
    } catch (nlohmann::detail::parse_error& e) {
        ifs.close();
        save_config(config_file_path.c_str(), config);
        return config;
    }

    maybe_load_field(
        config.global_params["server-port"], "server-port", loaded_config);

    maybe_load_field(config.global_params["max-concurrent-connections"],
                     "max-concurrent-connections",
                     loaded_config);

    maybe_load_field(
        config.global_params["max-workers"], "max-workers", loaded_config);

    maybe_load_field(config.global_params["timeout-interval"],
                     "timeout-interval",
                     loaded_config);
    // Done
    return config;
}
