#pragma once

#include <cstdint>

namespace fserv {

    enum class ClientState : std::uint8_t {
        kReady,
        kWorking,
        kStopped,
        kClosing,
        kClosed
    };
} // namespace fserv