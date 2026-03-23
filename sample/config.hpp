/* config.hpp -- v1.0
   Loads server configuration from disk (if available) or from defaults */

#pragma once

#include <optional>
#include <string>
#include <unordered_map>

namespace app {
    //! @struct Config
    /*! Global configuration options
     */
    struct Config {
        std::unordered_map<std::string, std::string> global_params;

        /*! @brief Accesses the value associated with the given key.
         */
        std::optional<std::string> operator[](const std::string& key) const
        {
            if (global_params.find(key) == global_params.end()) {
                return std::nullopt;
            }

            return std::optional<std::string>(global_params.at(key));
        }
    };

    /*! @brief Returns the configuration found at the specified path.
     */
    Config load_config(const char* path);
} // namespace app
