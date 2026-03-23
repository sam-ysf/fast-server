#pragma once

#include <cstdint>

namespace fserv {

    enum class ClientState : std::uint8_t {
        kInitialized,
        kReady,
        kRearming,
        kWorking,
        kWorked,
        kStopped,
        kClosing,
        kClosed
    };
} // namespace fserv