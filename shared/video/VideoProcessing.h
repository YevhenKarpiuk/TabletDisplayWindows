#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace tabletdisplay::video
{
    struct H264CodecConfig
    {
        std::vector<std::uint8_t> annexB;
        std::uint8_t nalLengthSize{ 4 };
        std::uint8_t profileIdc{};
        std::uint8_t levelIdc{};
    };

    [[nodiscard]] std::vector<std::uint8_t> BgraToNv12(
        std::span<const std::uint8_t> bgra,
        std::uint32_t width,
        std::uint32_t height,
        std::size_t stride);
    void BgraToNv12(
        std::span<const std::uint8_t> bgra,
        std::uint32_t width,
        std::uint32_t height,
        std::size_t stride,
        std::vector<std::uint8_t>& nv12);
    void RotateBgra(
        std::span<const std::uint8_t> bgra,
        std::uint32_t width,
        std::uint32_t height,
        std::size_t stride,
        std::uint32_t clockwiseQuarterTurns,
        std::vector<std::uint8_t>& rotated);

    [[nodiscard]] bool HasAnnexBStartCode(std::span<const std::uint8_t> bytes) noexcept;
    [[nodiscard]] bool AnnexBContainsNalType(
        std::span<const std::uint8_t> bytes,
        std::uint8_t nalType) noexcept;
    [[nodiscard]] std::vector<std::uint8_t> NormalizeH264AccessUnit(
        std::span<const std::uint8_t> bytes,
        std::uint8_t nalLengthSize = 4);
    [[nodiscard]] H264CodecConfig ParseH264CodecConfig(
        std::span<const std::uint8_t> bytes);
    [[nodiscard]] std::vector<std::uint8_t> ExtractAnnexBParameterSets(
        std::span<const std::uint8_t> accessUnit);
}
