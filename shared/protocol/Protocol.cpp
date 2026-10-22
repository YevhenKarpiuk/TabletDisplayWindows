#include "Protocol.h"

#include <algorithm>
#include <limits>
#include <string>
#include <type_traits>

namespace tabletdisplay::protocol
{
    namespace
    {
        template<typename T>
        [[nodiscard]] T ReadLe(std::span<const std::uint8_t> bytes, std::size_t offset)
        {
            static_assert(std::is_unsigned_v<T>);
            if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
            {
                throw ProtocolException(ProtocolErrorKind::InvalidPayload, "truncated field");
            }

            T value{};
            for (std::size_t index = 0; index < sizeof(T); ++index)
            {
                value |= static_cast<T>(bytes[offset + index]) << (index * 8U);
            }
            return value;
        }

        template<typename T>
        void AppendLe(std::vector<std::uint8_t>& output, T value)
        {
            static_assert(std::is_unsigned_v<T>);
            for (std::size_t index = 0; index < sizeof(T); ++index)
            {
                output.push_back(static_cast<std::uint8_t>(value & static_cast<T>(0xFF)));
                value >>= 8U;
            }
        }

        void Require(bool condition, const char* message)
        {
            if (!condition)
            {
                throw ProtocolException(ProtocolErrorKind::InvalidPayload, message);
            }
        }

        [[nodiscard]] bool HasAnnexBStartCode(
            std::span<const std::uint8_t> bytes,
            std::size_t offset)
        {
            if (offset + 3U > bytes.size() ||
                bytes[offset] != 0 ||
                bytes[offset + 1U] != 0)
            {
                return false;
            }
            if (bytes[offset + 2U] == 1)
            {
                return true;
            }
            return offset + 4U <= bytes.size() &&
                bytes[offset + 2U] == 0 &&
                bytes[offset + 3U] == 1;
        }

        [[nodiscard]] bool IsStrictUtf8(std::span<const std::uint8_t> bytes)
        {
            std::size_t index = 0;
            while (index < bytes.size())
            {
                const auto first = bytes[index++];
                if (first <= 0x7F)
                {
                    continue;
                }

                std::uint32_t codePoint{};
                std::size_t continuationCount{};
                std::uint32_t minimum{};
                if ((first & 0xE0) == 0xC0)
                {
                    codePoint = first & 0x1F;
                    continuationCount = 1;
                    minimum = 0x80;
                }
                else if ((first & 0xF0) == 0xE0)
                {
                    codePoint = first & 0x0F;
                    continuationCount = 2;
                    minimum = 0x800;
                }
                else if ((first & 0xF8) == 0xF0)
                {
                    codePoint = first & 0x07;
                    continuationCount = 3;
                    minimum = 0x10000;
                }
                else
                {
                    return false;
                }

                if (continuationCount > bytes.size() - index)
                {
                    return false;
                }
                for (std::size_t count = 0; count < continuationCount; ++count)
                {
                    const auto next = bytes[index++];
                    if ((next & 0xC0) != 0x80)
                    {
                        return false;
                    }
                    codePoint = (codePoint << 6U) | (next & 0x3F);
                }
                if (codePoint < minimum ||
                    codePoint > 0x10FFFF ||
                    (codePoint >= 0xD800 && codePoint <= 0xDFFF))
                {
                    return false;
                }
            }
            return true;
        }

        void ValidateFlags(const Header& header)
        {
            const auto known = IsKnownMessageType(header.messageType);
            if (!known)
            {
                if ((header.flags & HeaderFlag::Critical) != 0)
                {
                    throw ProtocolException(
                        ProtocolErrorKind::UnknownCriticalType,
                        "unknown critical message type");
                }
                if (header.flags != 0)
                {
                    throw ProtocolException(
                        ProtocolErrorKind::InvalidFlags,
                        "optional unknown type has flags");
                }
                return;
            }

            if ((header.flags & HeaderFlag::Critical) == 0)
            {
                throw ProtocolException(
                    ProtocolErrorKind::InvalidFlags,
                    "known message is not critical");
            }

            const auto allowed = header.messageType ==
                static_cast<std::uint16_t>(MessageType::VideoFrame)
                ? static_cast<std::uint16_t>(0x000F)
                : static_cast<std::uint16_t>(HeaderFlag::Critical);
            if ((header.flags & ~allowed) != 0)
            {
                throw ProtocolException(
                    ProtocolErrorKind::InvalidFlags,
                    "reserved header flag is set");
            }
        }
    }

    ProtocolException::ProtocolException(ProtocolErrorKind kind, const char* message) :
        std::runtime_error(message),
        m_kind(kind)
    {
    }

    ProtocolErrorKind ProtocolException::kind() const noexcept
    {
        return m_kind;
    }

    bool IsKnownMessageType(std::uint16_t type) noexcept
    {
        switch (static_cast<MessageType>(type))
        {
        case MessageType::Hello:
        case MessageType::HelloAck:
        case MessageType::DisplayInfo:
        case MessageType::VideoConfig:
        case MessageType::VideoFrame:
        case MessageType::RequestKeyframe:
        case MessageType::Touch:
        case MessageType::Ping:
        case MessageType::Pong:
        case MessageType::Error:
            return true;
        default:
            return false;
        }
    }

    std::string_view ProtocolErrorName(ProtocolErrorKind kind) noexcept
    {
        switch (kind)
        {
        case ProtocolErrorKind::BadMagic: return "BAD_MAGIC";
        case ProtocolErrorKind::UnsupportedVersion: return "UNSUPPORTED_VERSION";
        case ProtocolErrorKind::InvalidHeader: return "INVALID_HEADER";
        case ProtocolErrorKind::InvalidFlags: return "INVALID_FLAGS";
        case ProtocolErrorKind::UnknownCriticalType: return "UNKNOWN_CRITICAL_TYPE";
        case ProtocolErrorKind::PayloadTooLarge: return "PAYLOAD_TOO_LARGE";
        case ProtocolErrorKind::InvalidPayload: return "INVALID_PAYLOAD";
        case ProtocolErrorKind::SequenceGap: return "SEQUENCE_GAP";
        }
        return "UNKNOWN";
    }

    Header DecodeHeader(std::span<const std::uint8_t> bytes, std::uint32_t maxPayload)
    {
        if (bytes.size() != kHeaderSize)
        {
            throw ProtocolException(ProtocolErrorKind::InvalidHeader, "header must be 32 bytes");
        }
        if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin()))
        {
            throw ProtocolException(ProtocolErrorKind::BadMagic, "bad protocol magic");
        }
        if (bytes[4] != kVersionMajor || bytes[5] != kVersionMinor)
        {
            throw ProtocolException(
                ProtocolErrorKind::UnsupportedVersion,
                "unsupported protocol version");
        }
        if (ReadLe<std::uint16_t>(bytes, 6) != kHeaderSize)
        {
            throw ProtocolException(ProtocolErrorKind::InvalidHeader, "invalid header size");
        }

        Header header{
            ReadLe<std::uint16_t>(bytes, 8),
            ReadLe<std::uint16_t>(bytes, 10),
            ReadLe<std::uint32_t>(bytes, 12),
            ReadLe<std::uint64_t>(bytes, 16),
            ReadLe<std::uint64_t>(bytes, 24),
        };
        if (header.payloadLength > maxPayload || header.payloadLength > kMaxPayloadSize)
        {
            throw ProtocolException(
                ProtocolErrorKind::PayloadTooLarge,
                "payload exceeds negotiated limit");
        }
        constexpr auto maxSigned64 =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        if (header.sequence == 0 || header.sequence > maxSigned64 ||
            header.timestampUs == 0 || header.timestampUs > maxSigned64)
        {
            throw ProtocolException(
                ProtocolErrorKind::InvalidHeader,
                "sequence or timestamp is outside v1 range");
        }
        ValidateFlags(header);
        return header;
    }

    void ValidatePayload(const Header& header, std::span<const std::uint8_t> payload)
    {
        Require(payload.size() == header.payloadLength, "payload length mismatch");
        if (!IsKnownMessageType(header.messageType))
        {
            return;
        }

        switch (static_cast<MessageType>(header.messageType))
        {
        case MessageType::Hello:
        {
            Require(payload.size() == 32, "HELLO size");
            Require(payload[0] == 1 && payload[1] == 0, "HELLO role or reserved");
            Require(payload[2] == 1 && payload[3] == 1 &&
                payload[4] == 0 && payload[5] == 0, "HELLO version range");
            Require(ReadLe<std::uint16_t>(payload, 6) == 0, "HELLO reserved");
            const auto advertisedMax = ReadLe<std::uint32_t>(payload, 24);
            const auto heartbeat = ReadLe<std::uint16_t>(payload, 28);
            const auto idle = ReadLe<std::uint16_t>(payload, 30);
            Require(advertisedMax >= kHeaderSize && advertisedMax <= kMaxPayloadSize, "HELLO max payload");
            Require(heartbeat > 0 && idle >= heartbeat * 3U, "HELLO heartbeat");
            break;
        }
        case MessageType::HelloAck:
            Require(payload.size() == 32, "HELLO_ACK size");
            Require(ReadLe<std::uint16_t>(payload, 0) <= 3, "HELLO_ACK status");
            Require(payload[2] == 1 && payload[3] == 0, "HELLO_ACK version");
            Require(ReadLe<std::uint32_t>(payload, 4) == 0, "HELLO_ACK reserved");
            break;
        case MessageType::DisplayInfo:
        {
            Require(payload.size() == 40, "DISPLAY_INFO size");
            Require(ReadLe<std::uint32_t>(payload, 0) > 0 &&
                ReadLe<std::uint32_t>(payload, 4) > 0 &&
                ReadLe<std::uint32_t>(payload, 16) > 0 &&
                ReadLe<std::uint32_t>(payload, 20) > 0, "DISPLAY_INFO dimensions");
            Require(ReadLe<std::uint32_t>(payload, 28) <= kMaxAccessUnitSize, "DISPLAY_INFO access unit");
            const auto rotation = ReadLe<std::uint16_t>(payload, 36);
            Require(rotation == 0 || rotation == 90 || rotation == 180 || rotation == 270, "DISPLAY_INFO rotation");
            Require(ReadLe<std::uint16_t>(payload, 38) == 0, "DISPLAY_INFO reserved");
            break;
        }
        case MessageType::VideoConfig:
        {
            Require(payload.size() >= 28, "VIDEO_CONFIG size");
            const auto width = ReadLe<std::uint16_t>(payload, 8);
            const auto height = ReadLe<std::uint16_t>(payload, 10);
            const auto fpsNumerator = ReadLe<std::uint16_t>(payload, 12);
            const auto fpsDenominator = ReadLe<std::uint16_t>(payload, 14);
            const auto profile = payload[16];
            const auto configFlags = ReadLe<std::uint16_t>(payload, 18);
            const auto csdLength = ReadLe<std::uint32_t>(payload, 24);
            Require(ReadLe<std::uint32_t>(payload, 0) != 0 &&
                ReadLe<std::uint32_t>(payload, 4) != 0, "VIDEO_CONFIG ids");
            Require(IsValidVideoDimension(width) && IsValidVideoDimension(height), "VIDEO_CONFIG mode");
            Require((fpsNumerator == 30 || fpsNumerator == 60) &&
                fpsDenominator == 1, "VIDEO_CONFIG fps");
            Require(profile == 66 || profile == 77 || profile == 100, "VIDEO_CONFIG profile");
            Require(configFlags == 1, "VIDEO_CONFIG flags");
            Require(csdLength > 0 && csdLength <= kMaxCsdSize &&
                payload.size() == 28U + csdLength, "VIDEO_CONFIG CSD length");
            Require(HasAnnexBStartCode(payload, 28), "VIDEO_CONFIG CSD framing");
            break;
        }
        case MessageType::VideoFrame:
        {
            Require(payload.size() >= 32, "VIDEO_FRAME size");
            const auto dataLength = ReadLe<std::uint32_t>(payload, 24);
            const auto eos = (header.flags & HeaderFlag::EndOfStream) != 0;
            Require(ReadLe<std::uint32_t>(payload, 0) != 0 &&
                ReadLe<std::uint32_t>(payload, 4) != 0 &&
                ReadLe<std::uint64_t>(payload, 8) != 0, "VIDEO_FRAME ids");
            Require(ReadLe<std::uint32_t>(payload, 28) == 0, "VIDEO_FRAME reserved");
            Require(dataLength <= kMaxAccessUnitSize &&
                payload.size() == 32U + dataLength, "VIDEO_FRAME data length");
            Require(eos ? dataLength == 0 : dataLength > 0, "VIDEO_FRAME EOS");
            if (dataLength > 0)
            {
                Require(HasAnnexBStartCode(payload, 32), "VIDEO_FRAME Annex B");
            }
            if ((header.flags & HeaderFlag::Discontinuity) != 0)
            {
                Require((header.flags & HeaderFlag::KeyFrame) != 0, "VIDEO_FRAME discontinuity must be key");
            }
            break;
        }
        case MessageType::RequestKeyframe:
            Require(payload.size() == 24, "REQUEST_KEYFRAME size");
            Require(ReadLe<std::uint32_t>(payload, 0) != 0 &&
                ReadLe<std::uint32_t>(payload, 4) != 0, "REQUEST_KEYFRAME ids");
            Require(ReadLe<std::uint16_t>(payload, 16) >= 1 &&
                ReadLe<std::uint16_t>(payload, 16) <= 5, "REQUEST_KEYFRAME reason");
            Require(ReadLe<std::uint16_t>(payload, 18) == 0 &&
                ReadLe<std::uint32_t>(payload, 20) == 0, "REQUEST_KEYFRAME reserved");
            break;
        case MessageType::Touch:
            Require(payload.size() == 32, "TOUCH size");
            Require(ReadLe<std::uint32_t>(payload, 0) != 0, "TOUCH stream");
            Require(ReadLe<std::uint32_t>(payload, 4) == 0, "TOUCH pointer");
            Require(payload[8] >= 1 && payload[8] <= 4, "TOUCH action");
            Require((payload[9] & ~1U) == 0, "TOUCH buttons");
            Require(ReadLe<std::uint32_t>(payload, 16) != 0 &&
                ReadLe<std::uint64_t>(payload, 20) != 0, "TOUCH sequence or time");
            Require(ReadLe<std::uint32_t>(payload, 28) == 0, "TOUCH reserved");
            break;
        case MessageType::Ping:
            Require(payload.size() == 16, "PING size");
            break;
        case MessageType::Pong:
            Require(payload.size() == 24, "PONG size");
            break;
        case MessageType::Error:
        {
            Require(payload.size() >= 20, "ERROR size");
            const auto code = ReadLe<std::uint16_t>(payload, 0);
            const auto textLength = ReadLe<std::uint16_t>(payload, 16);
            Require(code >= 1 && code <= 10, "ERROR code");
            Require(payload[2] >= 1 && payload[2] <= 3 && payload[3] == 0, "ERROR severity");
            Require(ReadLe<std::uint16_t>(payload, 6) == 0 &&
                ReadLe<std::uint16_t>(payload, 18) == 0, "ERROR reserved");
            Require(textLength <= kMaxErrorTextSize &&
                payload.size() == 20U + textLength, "ERROR text length");
            Require(IsStrictUtf8(payload.subspan(20)), "ERROR UTF-8");
            break;
        }
        }
    }

    std::vector<std::uint8_t> EncodeMessage(
        const Header& header,
        std::span<const std::uint8_t> payload)
    {
        ValidatePayload(header, payload);
        ValidateFlags(header);
        if (header.sequence == 0 || header.timestampUs == 0)
        {
            throw ProtocolException(ProtocolErrorKind::InvalidHeader, "zero sequence or timestamp");
        }

        std::vector<std::uint8_t> output;
        output.reserve(kHeaderSize + payload.size());
        output.insert(output.end(), kMagic.begin(), kMagic.end());
        output.push_back(kVersionMajor);
        output.push_back(kVersionMinor);
        AppendLe(output, kHeaderSize);
        AppendLe(output, header.messageType);
        AppendLe(output, header.flags);
        AppendLe(output, header.payloadLength);
        AppendLe(output, header.sequence);
        AppendLe(output, header.timestampUs);
        output.insert(output.end(), payload.begin(), payload.end());
        return output;
    }

    IncrementalDecoder::IncrementalDecoder(std::uint32_t maxPayload, bool enforceSequence) :
        m_maxPayload(std::min(maxPayload, kMaxPayloadSize)),
        m_enforceSequence(enforceSequence)
    {
    }

    std::vector<Message> IncrementalDecoder::Feed(std::span<const std::uint8_t> bytes)
    {
        std::vector<Message> output;
        std::size_t offset = 0;
        while (offset < bytes.size())
        {
            if (!m_currentHeader)
            {
                const auto count = std::min(
                    bytes.size() - offset,
                    m_headerBytes.size() - m_headerFilled);
                std::copy_n(bytes.begin() + offset, count, m_headerBytes.begin() + m_headerFilled);
                offset += count;
                m_headerFilled += count;
                if (m_headerFilled != m_headerBytes.size())
                {
                    continue;
                }

                m_currentHeader = DecodeHeader(m_headerBytes, m_maxPayload);
                if (m_enforceSequence && m_currentHeader->sequence != m_expectedSequence)
                {
                    throw ProtocolException(
                        ProtocolErrorKind::SequenceGap,
                        "message sequence gap");
                }
                m_expectedSequence = m_currentHeader->sequence + 1U;
                BeginPayload();
                if (m_payload.empty())
                {
                    CompleteMessage(output);
                }
                continue;
            }

            const auto count = std::min(
                bytes.size() - offset,
                m_payload.size() - m_payloadFilled);
            std::copy_n(bytes.begin() + offset, count, m_payload.begin() + m_payloadFilled);
            offset += count;
            m_payloadFilled += count;
            if (m_payloadFilled == m_payload.size())
            {
                CompleteMessage(output);
            }
        }
        return output;
    }

    void IncrementalDecoder::Reset() noexcept
    {
        m_expectedSequence = 1;
        m_headerFilled = 0;
        m_currentHeader.reset();
        m_payload.clear();
        m_payloadFilled = 0;
    }

    void IncrementalDecoder::BeginPayload()
    {
        m_payload.assign(m_currentHeader->payloadLength, 0);
        m_payloadFilled = 0;
    }

    void IncrementalDecoder::CompleteMessage(std::vector<Message>& output)
    {
        ValidatePayload(*m_currentHeader, m_payload);
        if (IsKnownMessageType(m_currentHeader->messageType))
        {
            output.push_back(Message{ *m_currentHeader, std::move(m_payload) });
        }
        m_headerFilled = 0;
        m_currentHeader.reset();
        m_payload.clear();
        m_payloadFilled = 0;
    }
}
