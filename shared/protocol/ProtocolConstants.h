#pragma once

#include <cstdint>
#include <string_view>

namespace tabletdisplay::protocol
{
    inline constexpr std::string_view kLoopbackAddress = "127.0.0.1";
    inline constexpr std::uint16_t kPort = 27183;
    inline constexpr std::uint16_t kProtocolVersion = 1;
}
