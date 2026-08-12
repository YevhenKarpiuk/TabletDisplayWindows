#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tabletdisplay::protocol
{
    inline constexpr std::string_view kLoopbackAddress = "127.0.0.1";
    inline constexpr std::uint16_t kPort = 27183;
    inline constexpr std::array<std::uint8_t, 4> kMagic{ 0x54, 0x44, 0x53, 0x31 };
    inline constexpr std::uint8_t kVersionMajor = 1;
    inline constexpr std::uint8_t kVersionMinor = 0;
    inline constexpr std::uint16_t kHeaderSize = 32;
    inline constexpr std::uint32_t kMaxAccessUnitSize = 8U * 1024U * 1024U;
    inline constexpr std::uint32_t kMaxPayloadSize = kMaxAccessUnitSize + 32U;
    inline constexpr std::uint32_t kMaxCsdSize = 64U * 1024U;
    inline constexpr std::uint16_t kMaxErrorTextSize = 1024;
    inline constexpr std::uint16_t kProtocolVersion = kVersionMajor;
}
