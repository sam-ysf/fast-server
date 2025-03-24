/* config.hpp -- v1.0
   Loads server configuration from disk (if available) or from defaults */

#pragma once

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
        const std::string& operator[](const std::string& key) const
        {
            const std::string& value = global_params.at(key);
            return value;
        }

        /*! @brief Checks if the configuration has the given key.
         */
        bool has_param(const std::string& key) const
        {
            return global_params.find(key) != global_params.end();
        }
    };

    /*! @brief Returns the configuration found at the specified path.
     */
    Config load_config(const char* path);
} // namespace app
