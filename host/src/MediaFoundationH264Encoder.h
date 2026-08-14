#pragma once

#include "video/VideoProcessing.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace tabletdisplay::host
{
    struct EncoderOptions
    {
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint32_t fps{};
        std::uint32_t bitrate{};
        bool preferHardware{};
    };

    struct EncodedAccessUnit
    {
        std::vector<std::uint8_t> bytes;
        std::uint64_t presentationTimeUs{};
        bool keyFrame{};
    };

    class MediaFoundationH264Encoder
    {
    public:
        explicit MediaFoundationH264Encoder(EncoderOptions options);
        ~MediaFoundationH264Encoder();
        MediaFoundationH264Encoder(const MediaFoundationH264Encoder&) = delete;
        MediaFoundationH264Encoder& operator=(const MediaFoundationH264Encoder&) = delete;

        [[nodiscard]] std::vector<EncodedAccessUnit> Encode(
            std::span<const std::uint8_t> nv12,
            std::uint64_t presentationTimeUs,
            bool forceKeyFrame = false);
        [[nodiscard]] std::vector<EncodedAccessUnit> EncodeBgra(
            std::span<const std::uint8_t> bgra,
            std::uint32_t sourceWidth,
            std::uint32_t sourceHeight,
            std::size_t sourceStride,
            std::uint32_t clockwiseQuarterTurns,
            std::uint64_t presentationTimeUs,
            bool forceKeyFrame = false);
        [[nodiscard]] std::vector<EncodedAccessUnit> Drain();

        [[nodiscard]] const std::string& Name() const noexcept;
        [[nodiscard]] bool IsHardware() const noexcept;
        [[nodiscard]] bool UsesGpuBgraConversion() const noexcept;
        [[nodiscard]] const std::string& BgraConversionName() const noexcept;
        [[nodiscard]] const tabletdisplay::video::H264CodecConfig* CodecConfig() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
