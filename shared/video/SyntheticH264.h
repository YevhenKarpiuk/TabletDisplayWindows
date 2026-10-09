#pragma once

#include <cstdint>
#include <vector>

namespace tabletdisplay::video
{
    struct SyntheticH264
    {
        std::uint16_t width{};
        std::uint16_t height{};
        std::uint16_t fps{};
        std::uint8_t profileIdc{};
        std::uint8_t levelIdc{};
        std::vector<std::uint8_t> codecSpecificData;
        std::vector<std::uint8_t> idrAccessUnit;
    };

    [[nodiscard]] SyntheticH264 BuildSyntheticColorBars();
}
