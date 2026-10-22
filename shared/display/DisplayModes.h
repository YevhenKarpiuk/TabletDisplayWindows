#pragma once

#include <cstdint>

namespace tabletdisplay::display
{
    // The virtual monitor offers the tablet resolution divided by 1, 2, 3 or 4.
    inline constexpr std::uint32_t kMinResolutionDivisor = 1;
    inline constexpr std::uint32_t kMaxResolutionDivisor = 4;
    inline constexpr std::uint32_t kMinModeDimension = 128;
    inline constexpr std::uint32_t kMaxModeDimension = 3840;

    struct Mode
    {
        std::uint32_t width{};
        std::uint32_t height{};
    };

    // IddCx frames are always landscape; Windows applies portrait orientation by rotation.
    [[nodiscard]] constexpr Mode Landscape(std::uint32_t width, std::uint32_t height) noexcept
    {
        return width >= height ? Mode{ width, height } : Mode{ height, width };
    }

    // Divides the landscape tablet resolution and rounds each side down to an even value,
    // which NV12 and H.264 require, so the aspect ratio stays within one pixel of the tablet.
    [[nodiscard]] constexpr Mode ScaledMode(
        std::uint32_t nativeWidth,
        std::uint32_t nativeHeight,
        std::uint32_t divisor) noexcept
    {
        const auto native = Landscape(nativeWidth, nativeHeight);
        if (divisor < kMinResolutionDivisor || divisor > kMaxResolutionDivisor) return {};
        return { (native.width / divisor) & ~1U, (native.height / divisor) & ~1U };
    }

    [[nodiscard]] constexpr bool IsSupportedMode(Mode mode) noexcept
    {
        return mode.width >= kMinModeDimension && mode.width <= kMaxModeDimension &&
            mode.height >= kMinModeDimension && mode.height <= kMaxModeDimension &&
            mode.width % 2U == 0 && mode.height % 2U == 0;
    }
}
