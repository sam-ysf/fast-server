#pragma once

#include <cstdint>

namespace fserv {

    enum class ClientState : std::uint8_t {
        kReady,
        kRearming,
        kWorking,
        kWorked,
        kStopped,
        kClosing,
        kClosed
    };
} // namespace fserv