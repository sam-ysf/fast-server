#pragma once

#include "client_state.hpp"
#include <atomic>
#include <cstdint>

namespace fserv {

    struct ClientGenerationToken {
        std::uint64_t uuid = 0;
        std::uint8_t state = static_cast<std::uint8_t>(ClientState::kClosed);
    };

    struct ClientGenerationState {
        std::atomic<ClientGenerationToken> token;
        std::atomic<std::uint64_t> n_writers = 0;
    };
} // namespace fserv
