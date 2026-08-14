#include "video/VideoProcessing.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace tabletdisplay::video
{
    namespace
    {
        [[nodiscard]] std::uint8_t ClampByte(int value) noexcept
        {
            return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
        }

        [[nodiscard]] std::size_t StartCodeLength(
            std::span<const std::uint8_t> bytes,
            std::size_t offset) noexcept
        {
            if (offset + 3 <= bytes.size() && bytes[offset] == 0 && bytes[offset + 1] == 0)
            {
                if (bytes[offset + 2] == 1) return 3;
                if (offset + 4 <= bytes.size() && bytes[offset + 2] == 0 && bytes[offset + 3] == 1)
                    return 4;
            }
            return 0;
        }

        [[nodiscard]] std::vector<std::pair<std::size_t, std::size_t>> AnnexBUnits(
            std::span<const std::uint8_t> bytes)
        {
            std::vector<std::size_t> starts;
            for (std::size_t offset = 0; offset + 3 <= bytes.size();)
            {
                const auto length = StartCodeLength(bytes, offset);
                if (length != 0)
                {
                    starts.push_back(offset);
                    offset += length;
                }
                else
                {
                    ++offset;
                }
            }
            std::vector<std::pair<std::size_t, std::size_t>> units;
            units.reserve(starts.size());
            for (std::size_t index = 0; index < starts.size(); ++index)
            {
                units.emplace_back(starts[index], index + 1 < starts.size() ? starts[index + 1] : bytes.size());
            }
            return units;
        }

        void AppendStartCode(std::vector<std::uint8_t>& output)
        {
            output.insert(output.end(), { 0, 0, 0, 1 });
        }

        [[nodiscard]] std::uint32_t ReadBigEndian(
            std::span<const std::uint8_t> bytes,
            std::size_t offset,
            std::uint8_t size)
        {
            if (size == 0 || size > 4 || offset > bytes.size() || size > bytes.size() - offset)
                throw std::runtime_error("Truncated H.264 NAL length");
            std::uint32_t value{};
            for (std::uint8_t index = 0; index < size; ++index)
                value = (value << 8U) | bytes[offset + index];
            return value;
        }

        void AppendLengthPrefixedNal(
            std::vector<std::uint8_t>& output,
            std::span<const std::uint8_t> bytes,
            std::size_t& offset,
            std::uint8_t lengthSize)
        {
            const auto length = ReadBigEndian(bytes, offset, lengthSize);
            offset += lengthSize;
            if (length == 0 || offset > bytes.size() || length > bytes.size() - offset)
                throw std::runtime_error("Invalid H.264 NAL length");
            AppendStartCode(output);
            output.insert(output.end(), bytes.begin() + offset, bytes.begin() + offset + length);
            offset += length;
        }
    }

    std::vector<std::uint8_t> BgraToNv12(
        std::span<const std::uint8_t> bgra,
        std::uint32_t width,
        std::uint32_t height,
        std::size_t stride)
    {
        std::vector<std::uint8_t> nv12;
        BgraToNv12(bgra, width, height, stride, nv12);
        return nv12;
    }

    void BgraToNv12(
        std::span<const std::uint8_t> bgra,
        std::uint32_t width,
        std::uint32_t height,
        std::size_t stride,
        std::vector<std::uint8_t>& nv12)
    {
        if (width == 0 || height == 0 || (width & 1U) != 0 || (height & 1U) != 0)
            throw std::invalid_argument("NV12 dimensions must be non-zero and even");
        if (stride < static_cast<std::size_t>(width) * 4U ||
            static_cast<std::size_t>(height) > std::numeric_limits<std::size_t>::max() / stride ||
            bgra.size() < stride * height)
            throw std::invalid_argument("BGRA buffer is too small");

        const auto ySize = static_cast<std::size_t>(width) * height;
        nv12.resize(ySize + ySize / 2U);
        auto* uvPlane = nv12.data() + ySize;
        for (std::uint32_t row = 0; row < height; row += 2)
        {
            const auto* source0 = bgra.data() + static_cast<std::size_t>(row) * stride;
            const auto* source1 = source0 + stride;
            auto* yPlane0 = nv12.data() + static_cast<std::size_t>(row) * width;
            auto* yPlane1 = yPlane0 + width;
            auto* uvRow = uvPlane + static_cast<std::size_t>(row / 2U) * width;
            for (std::uint32_t column = 0; column < width; column += 2)
            {
                const auto offset = static_cast<std::size_t>(column) * 4U;
                const int blue00 = source0[offset];
                const int green00 = source0[offset + 1U];
                const int red00 = source0[offset + 2U];
                const int blue01 = source0[offset + 4U];
                const int green01 = source0[offset + 5U];
                const int red01 = source0[offset + 6U];
                const int blue10 = source1[offset];
                const int green10 = source1[offset + 1U];
                const int red10 = source1[offset + 2U];
                const int blue11 = source1[offset + 4U];
                const int green11 = source1[offset + 5U];
                const int red11 = source1[offset + 6U];

                yPlane0[column] = ClampByte(((66 * red00 + 129 * green00 + 25 * blue00 + 128) >> 8) + 16);
                yPlane0[column + 1U] = ClampByte(((66 * red01 + 129 * green01 + 25 * blue01 + 128) >> 8) + 16);
                yPlane1[column] = ClampByte(((66 * red10 + 129 * green10 + 25 * blue10 + 128) >> 8) + 16);
                yPlane1[column + 1U] = ClampByte(((66 * red11 + 129 * green11 + 25 * blue11 + 128) >> 8) + 16);

                const int red = (red00 + red01 + red10 + red11) / 4;
                const int green = (green00 + green01 + green10 + green11) / 4;
                const int blue = (blue00 + blue01 + blue10 + blue11) / 4;
                uvRow[column] = ClampByte(((-38 * red - 74 * green + 112 * blue + 128) >> 8) + 128);
                uvRow[column + 1U] = ClampByte(((112 * red - 94 * green - 18 * blue + 128) >> 8) + 128);
            }
        }
    }

    void RotateBgra(
        std::span<const std::uint8_t> bgra,
        std::uint32_t width,
        std::uint32_t height,
        std::size_t stride,
        std::uint32_t clockwiseQuarterTurns,
        std::vector<std::uint8_t>& rotated)
    {
        if (width == 0 || height == 0 || clockwiseQuarterTurns > 3)
            throw std::invalid_argument("Invalid BGRA rotation");
        if (stride < static_cast<std::size_t>(width) * 4U ||
            static_cast<std::size_t>(height) > std::numeric_limits<std::size_t>::max() / stride ||
            bgra.size() < stride * height)
            throw std::invalid_argument("BGRA buffer is too small");

        const auto outputWidth = (clockwiseQuarterTurns & 1U) != 0 ? height : width;
        const auto outputHeight = (clockwiseQuarterTurns & 1U) != 0 ? width : height;
        rotated.resize(static_cast<std::size_t>(outputWidth) * outputHeight * 4U);
        const auto copyPixel = [&](std::uint32_t sourceX, std::uint32_t sourceY,
                                   std::uint32_t destinationX, std::uint32_t destinationY)
        {
            const auto* sourceRow = bgra.data() + static_cast<std::size_t>(sourceY) * stride;
            auto* destination = rotated.data() +
                (static_cast<std::size_t>(destinationY) * outputWidth + destinationX) * 4U;
            memcpy(destination, sourceRow + static_cast<std::size_t>(sourceX) * 4U, 4U);
        };
        if (clockwiseQuarterTurns == 0)
        {
            const auto rowBytes = static_cast<std::size_t>(width) * 4U;
            for (std::uint32_t row = 0; row < height; ++row)
                memcpy(rotated.data() + static_cast<std::size_t>(row) * rowBytes,
                    bgra.data() + static_cast<std::size_t>(row) * stride, rowBytes);
        }
        else if (clockwiseQuarterTurns == 1)
        {
            for (std::uint32_t sourceY = 0; sourceY < height; ++sourceY)
            for (std::uint32_t sourceX = 0; sourceX < width; ++sourceX)
                copyPixel(sourceX, sourceY, height - 1U - sourceY, sourceX);
        }
        else if (clockwiseQuarterTurns == 2)
        {
            for (std::uint32_t sourceY = 0; sourceY < height; ++sourceY)
            for (std::uint32_t sourceX = 0; sourceX < width; ++sourceX)
                copyPixel(sourceX, sourceY, width - 1U - sourceX, height - 1U - sourceY);
        }
        else
        {
            for (std::uint32_t sourceY = 0; sourceY < height; ++sourceY)
            for (std::uint32_t sourceX = 0; sourceX < width; ++sourceX)
                copyPixel(sourceX, sourceY, sourceY, width - 1U - sourceX);
        }
    }

    bool HasAnnexBStartCode(std::span<const std::uint8_t> bytes) noexcept
    {
        return StartCodeLength(bytes, 0) != 0;
    }

    bool AnnexBContainsNalType(
        std::span<const std::uint8_t> bytes,
        std::uint8_t nalType) noexcept
    {
        for (const auto [start, end] : AnnexBUnits(bytes))
        {
            const auto payload = start + StartCodeLength(bytes, start);
            if (payload < end && (bytes[payload] & 0x1FU) == nalType) return true;
        }
        return false;
    }

    std::vector<std::uint8_t> NormalizeH264AccessUnit(
        std::span<const std::uint8_t> bytes,
        std::uint8_t nalLengthSize)
    {
        if (bytes.empty()) return {};
        if (HasAnnexBStartCode(bytes)) return { bytes.begin(), bytes.end() };
        if (nalLengthSize == 0 || nalLengthSize > 4)
            throw std::invalid_argument("Invalid H.264 NAL length field size");

        std::vector<std::uint8_t> output;
        output.reserve(bytes.size() + 16U);
        std::size_t offset{};
        while (offset < bytes.size()) AppendLengthPrefixedNal(output, bytes, offset, nalLengthSize);
        return output;
    }

    H264CodecConfig ParseH264CodecConfig(std::span<const std::uint8_t> bytes)
    {
        if (bytes.empty()) throw std::invalid_argument("H.264 codec configuration is empty");
        H264CodecConfig result;
        if (HasAnnexBStartCode(bytes))
        {
            result.annexB = ExtractAnnexBParameterSets(bytes);
            const auto units = AnnexBUnits(result.annexB);
            for (const auto [start, end] : units)
            {
                const auto payload = start + StartCodeLength(result.annexB, start);
                if (payload + 3 < end && (result.annexB[payload] & 0x1FU) == 7)
                {
                    result.profileIdc = result.annexB[payload + 1U];
                    result.levelIdc = result.annexB[payload + 3U];
                    break;
                }
            }
            return result;
        }
        if (bytes.size() < 7 || bytes[0] != 1)
            throw std::invalid_argument("Unsupported H.264 codec configuration format");

        result.profileIdc = bytes[1];
        result.levelIdc = bytes[3];
        result.nalLengthSize = static_cast<std::uint8_t>((bytes[4] & 3U) + 1U);
        std::size_t offset = 6;
        const auto spsCount = bytes[5] & 0x1FU;
        for (std::uint8_t index = 0; index < spsCount; ++index)
            AppendLengthPrefixedNal(result.annexB, bytes, offset, 2);
        if (offset >= bytes.size()) throw std::runtime_error("H.264 codec configuration has no PPS count");
        const auto ppsCount = bytes[offset++];
        for (std::uint8_t index = 0; index < ppsCount; ++index)
            AppendLengthPrefixedNal(result.annexB, bytes, offset, 2);
        if (offset != bytes.size()) throw std::runtime_error("H.264 codec configuration has trailing data");
        if (!AnnexBContainsNalType(result.annexB, 7) || !AnnexBContainsNalType(result.annexB, 8))
            throw std::runtime_error("H.264 codec configuration lacks SPS/PPS");
        return result;
    }

    std::vector<std::uint8_t> ExtractAnnexBParameterSets(
        std::span<const std::uint8_t> accessUnit)
    {
        std::vector<std::uint8_t> output;
        for (const auto [start, end] : AnnexBUnits(accessUnit))
        {
            const auto payload = start + StartCodeLength(accessUnit, start);
            if (payload >= end) continue;
            const auto type = accessUnit[payload] & 0x1FU;
            if (type == 7 || type == 8)
            {
                AppendStartCode(output);
                output.insert(output.end(), accessUnit.begin() + payload, accessUnit.begin() + end);
            }
        }
        if (!AnnexBContainsNalType(output, 7) || !AnnexBContainsNalType(output, 8))
            throw std::runtime_error("H.264 access unit lacks SPS/PPS");
        return output;
    }
}
