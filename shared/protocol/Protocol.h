#pragma once

#include "ProtocolConstants.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace tabletdisplay::protocol
{
    enum class MessageType : std::uint16_t
    {
        Hello = 0x0001,
        HelloAck = 0x0002,
        DisplayInfo = 0x0003,
        VideoConfig = 0x0010,
        VideoFrame = 0x0011,
        RequestKeyframe = 0x0012,
        Touch = 0x0020,
        Ping = 0x0030,
        Pong = 0x0031,
        // Optional (not CRITICAL): peers that do not know it skip it.
        OrientationPolicy = 0x0040,
        Error = 0x00FF,
    };

    enum HeaderFlag : std::uint16_t
    {
        Critical = 0x0001,
        KeyFrame = 0x0002,
        Discontinuity = 0x0004,
        EndOfStream = 0x0008,
    };

    enum class ProtocolErrorKind
    {
        BadMagic,
        UnsupportedVersion,
        InvalidHeader,
        InvalidFlags,
        UnknownCriticalType,
        PayloadTooLarge,
        InvalidPayload,
        SequenceGap,
    };

    class ProtocolException final : public std::runtime_error
    {
    public:
        ProtocolException(ProtocolErrorKind kind, const char* message);

        [[nodiscard]] ProtocolErrorKind kind() const noexcept;

    private:
        ProtocolErrorKind m_kind;
    };

    struct Header
    {
        std::uint16_t messageType{};
        std::uint16_t flags{};
        std::uint32_t payloadLength{};
        std::uint64_t sequence{};
        std::uint64_t timestampUs{};
    };

    struct Message
    {
        Header header;
        std::vector<std::uint8_t> payload;
    };

    [[nodiscard]] bool IsKnownMessageType(std::uint16_t type) noexcept;
    // Even dimension in 128..4096, as protocol section 5.4 requires for VIDEO_CONFIG.
    [[nodiscard]] constexpr bool IsValidVideoDimension(std::uint32_t value) noexcept
    {
        return value >= kMinVideoDimension && value <= kMaxVideoDimension && value % 2U == 0;
    }
    [[nodiscard]] std::string_view ProtocolErrorName(ProtocolErrorKind kind) noexcept;
    [[nodiscard]] Header DecodeHeader(
        std::span<const std::uint8_t> bytes,
        std::uint32_t maxPayload = kMaxPayloadSize);
    void ValidatePayload(const Header& header, std::span<const std::uint8_t> payload);
    [[nodiscard]] std::vector<std::uint8_t> EncodeMessage(
        const Header& header,
        std::span<const std::uint8_t> payload);

    class IncrementalDecoder
    {
    public:
        explicit IncrementalDecoder(
            std::uint32_t maxPayload = kMaxPayloadSize,
            bool enforceSequence = true);

        [[nodiscard]] std::vector<Message> Feed(std::span<const std::uint8_t> bytes);
        void Reset() noexcept;

    private:
        void BeginPayload();
        void CompleteMessage(std::vector<Message>& output);

        std::uint32_t m_maxPayload;
        bool m_enforceSequence;
        std::uint64_t m_expectedSequence{ 1 };
        std::array<std::uint8_t, kHeaderSize> m_headerBytes{};
        std::size_t m_headerFilled{};
        std::optional<Header> m_currentHeader;
        std::vector<std::uint8_t> m_payload;
        std::size_t m_payloadFilled{};
    };
}
