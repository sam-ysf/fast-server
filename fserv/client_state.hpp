#pragma once

#include <cstdint>

namespace fserv {

    enum class ClientState : std::uint8_t {
        kAllocated,
        kArming,
        kReady,
        kRearming,
        kWorking,
        kWorked,
        kStopped,
        kClosing,
        kClosed
    };
} // namespace fserv