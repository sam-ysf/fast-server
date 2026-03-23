#pragma once

#include "client_state.hpp"
#include <cstdint>

namespace fserv {

    struct ClientGenerationToken {
        std::uint64_t writing = 0;
        std::uint64_t uuid = 0;
        std::uint64_t state = static_cast<std::uint64_t>(ClientState::kClosed);
    };
} // namespace fserv
