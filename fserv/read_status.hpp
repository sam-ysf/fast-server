#include <cstdint>

namespace fserv {

    enum class ReadStatus : std::uint8_t {
        kOk,
        kOkAndHaveMoreIo,
        kConnectionClosed,
        kWantReadIo,
        kWantWriteIo,
        kError
    };
}