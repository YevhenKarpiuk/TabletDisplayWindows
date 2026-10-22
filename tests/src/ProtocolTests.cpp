#include "display/DisplayModes.h"
#include "ipc/FrameTransport.h"
#include "protocol/Protocol.h"
#include "video/SyntheticH264.h"
#include "video/VideoProcessing.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using tabletdisplay::protocol::Header;
    using tabletdisplay::protocol::IncrementalDecoder;
    using tabletdisplay::protocol::Message;
    using tabletdisplay::protocol::ProtocolException;

    struct VectorCase
    {
        std::string name;
        std::string file;
        std::string direction;
        std::string expectation;
        std::uint16_t type;
        std::uint16_t flags;
        std::uint32_t payloadLength;
        std::uint64_t sequence;
    };

    void Check(bool condition, std::string_view message)
    {
        if (!condition)
        {
            throw std::runtime_error(std::string(message));
        }
    }

    std::vector<std::string> SplitTabs(const std::string& line)
    {
        std::vector<std::string> fields;
        std::size_t start = 0;
        while (true)
        {
            const auto separator = line.find('\t', start);
            fields.push_back(line.substr(start, separator - start));
            if (separator == std::string::npos)
            {
                break;
            }
            start = separator + 1;
        }
        return fields;
    }

    std::uint64_t ParseUnsigned(const std::string& text)
    {
        return std::stoull(text, nullptr, 0);
    }

    std::vector<VectorCase> LoadManifest(const std::filesystem::path& directory)
    {
        std::ifstream input(directory / "manifest.tsv");
        Check(input.good(), "cannot open golden manifest");

        std::vector<VectorCase> cases;
        std::string line;
        while (std::getline(input, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            if (line.empty() || line.front() == '#')
            {
                continue;
            }

            const auto fields = SplitTabs(line);
            Check(fields.size() == 8, "invalid golden manifest row");
            cases.push_back(VectorCase{
                fields[0],
                fields[1],
                fields[2],
                fields[3],
                static_cast<std::uint16_t>(ParseUnsigned(fields[4])),
                static_cast<std::uint16_t>(ParseUnsigned(fields[5])),
                static_cast<std::uint32_t>(ParseUnsigned(fields[6])),
                ParseUnsigned(fields[7]),
            });
        }
        return cases;
    }

    int HexNibble(char value)
    {
        if (value >= '0' && value <= '9')
        {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f')
        {
            return value - 'a' + 10;
        }
        if (value >= 'A' && value <= 'F')
        {
            return value - 'A' + 10;
        }
        return -1;
    }

    std::vector<std::uint8_t> LoadHex(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        Check(input.good(), "cannot open hex vector");
        const std::string contents{
            std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};

        std::vector<std::uint8_t> bytes;
        int highNibble = -1;
        for (const char value : contents)
        {
            if (std::isspace(static_cast<unsigned char>(value)) != 0)
            {
                continue;
            }
            const auto nibble = HexNibble(value);
            Check(nibble >= 0, "invalid character in hex vector");
            if (highNibble < 0)
            {
                highNibble = nibble;
            }
            else
            {
                bytes.push_back(static_cast<std::uint8_t>((highNibble << 4) | nibble));
                highNibble = -1;
            }
        }
        Check(highNibble < 0, "odd number of hex digits");
        return bytes;
    }

    std::vector<Message> FeedInChunks(
        IncrementalDecoder& decoder,
        std::span<const std::uint8_t> bytes,
        std::size_t firstChunk = 1)
    {
        std::vector<Message> messages;
        std::size_t offset = 0;
        std::size_t chunk = firstChunk;
        while (offset < bytes.size())
        {
            const auto count = std::min(chunk, bytes.size() - offset);
            auto decoded = decoder.Feed(bytes.subspan(offset, count));
            messages.insert(
                messages.end(),
                std::make_move_iterator(decoded.begin()),
                std::make_move_iterator(decoded.end()));
            offset += count;
            chunk = chunk % 11U + 1U;
        }
        return messages;
    }

    void CheckValidVector(
        const std::filesystem::path& directory,
        const VectorCase& vectorCase)
    {
        const auto bytes = LoadHex(directory / vectorCase.file);

        for (std::size_t split = 0; split <= bytes.size(); ++split)
        {
            IncrementalDecoder decoder(tabletdisplay::protocol::kMaxPayloadSize, false);
            auto first = decoder.Feed(std::span(bytes).first(split));
            auto second = decoder.Feed(std::span(bytes).subspan(split));
            first.insert(
                first.end(),
                std::make_move_iterator(second.begin()),
                std::make_move_iterator(second.end()));
            Check(first.size() == 1, vectorCase.name + ": split decode count");
        }

        IncrementalDecoder decoder(tabletdisplay::protocol::kMaxPayloadSize, false);
        auto messages = FeedInChunks(decoder, bytes);
        Check(messages.size() == 1, vectorCase.name + ": chunk decode count");

        const auto& message = messages.front();
        Check(message.header.messageType == vectorCase.type, vectorCase.name + ": type");
        Check(message.header.flags == vectorCase.flags, vectorCase.name + ": flags");
        Check(message.header.payloadLength == vectorCase.payloadLength, vectorCase.name + ": payload length");
        Check(message.header.sequence == vectorCase.sequence, vectorCase.name + ": sequence");

        const auto encoded = tabletdisplay::protocol::EncodeMessage(
            message.header,
            message.payload);
        Check(encoded == bytes, vectorCase.name + ": decode/encode mismatch");
    }

    void CheckRejectedVector(
        const std::filesystem::path& directory,
        const VectorCase& vectorCase)
    {
        const auto bytes = LoadHex(directory / vectorCase.file);
        const bool enforceSequence = vectorCase.expectation == "SEQUENCE_GAP";
        IncrementalDecoder decoder(
            tabletdisplay::protocol::kMaxPayloadSize,
            enforceSequence);

        try
        {
            static_cast<void>(FeedInChunks(decoder, bytes));
        }
        catch (const ProtocolException& exception)
        {
            Check(
                tabletdisplay::protocol::ProtocolErrorName(exception.kind()) ==
                    vectorCase.expectation,
                vectorCase.name + ": wrong rejection");
            return;
        }
        throw std::runtime_error(vectorCase.name + ": rejection expected");
    }

    void CheckDirectionStream(
        const std::filesystem::path& directory,
        const std::vector<VectorCase>& cases,
        std::string_view direction)
    {
        IncrementalDecoder decoder;
        std::size_t decodedCount = 0;
        for (const auto& vectorCase : cases)
        {
            if (vectorCase.expectation != "VALID" ||
                vectorCase.direction != direction)
            {
                continue;
            }
            const auto bytes = LoadHex(directory / vectorCase.file);
            decodedCount += FeedInChunks(decoder, bytes, vectorCase.sequence).size();
        }

        const auto expected = std::count_if(
            cases.begin(),
            cases.end(),
            [direction](const VectorCase& vectorCase)
            {
                return vectorCase.expectation == "VALID" &&
                    vectorCase.direction == direction;
            });
        Check(decodedCount == static_cast<std::size_t>(expected), "direction stream count");
    }
}

int main(int argc, char* argv[])
{
    try
    {
        Check(argc == 2, "usage: TabletDisplayProtocolTests <golden-directory>");
        const std::filesystem::path directory(argv[1]);
        const auto cases = LoadManifest(directory);
        Check(cases.size() == 14, "unexpected vector count");

        for (const auto& vectorCase : cases)
        {
            if (vectorCase.expectation == "VALID")
            {
                CheckValidVector(directory, vectorCase);
            }
            else
            {
                CheckRejectedVector(directory, vectorCase);
            }
        }
        CheckDirectionStream(directory, cases, "W2A");
        CheckDirectionStream(directory, cases, "A2W");

        const auto synthetic = tabletdisplay::video::BuildSyntheticColorBars();
        Check(synthetic.width == 1280 && synthetic.height == 800, "synthetic dimensions");
        Check(synthetic.codecSpecificData.size() >= 16, "synthetic CSD size");
        Check(synthetic.idrAccessUnit.size() > 3'000, "synthetic IDR size");
        Check(synthetic.idrAccessUnit.size() <= tabletdisplay::protocol::kMaxAccessUnitSize,
            "synthetic IDR protocol limit");
        Check(synthetic.codecSpecificData[4] == 0x67, "synthetic SPS");
        Check(synthetic.idrAccessUnit[4] == 0x67, "synthetic access unit SPS");

        const std::vector<std::uint8_t> blackBgra{
            0, 0, 0, 255, 0, 0, 0, 255,
            0, 0, 0, 255, 0, 0, 0, 255
        };
        const auto blackNv12 = tabletdisplay::video::BgraToNv12(blackBgra, 2, 2, 8);
        Check(blackNv12 == std::vector<std::uint8_t>({ 16, 16, 16, 16, 128, 128 }),
            "BGRA black to NV12");

        const std::vector<std::uint8_t> whiteBgra{
            255, 255, 255, 255, 255, 255, 255, 255,
            255, 255, 255, 255, 255, 255, 255, 255
        };
        const auto whiteNv12 = tabletdisplay::video::BgraToNv12(whiteBgra, 2, 2, 8);
        Check(whiteNv12 == std::vector<std::uint8_t>({ 235, 235, 235, 235, 128, 128 }),
            "BGRA white to NV12");

        const std::vector<std::uint8_t> rotationSource{
            1, 0, 0, 255, 2, 0, 0, 255, 3, 0, 0, 255,
            4, 0, 0, 255, 5, 0, 0, 255, 6, 0, 0, 255
        };
        std::vector<std::uint8_t> rotated;
        tabletdisplay::video::RotateBgra(rotationSource, 3, 2, 12, 1, rotated);
        Check(rotated.size() == rotationSource.size() &&
            rotated[0] == 4 && rotated[4] == 1 && rotated[8] == 5 &&
            rotated[12] == 2 && rotated[16] == 6 && rotated[20] == 3,
            "BGRA clockwise rotation");
        tabletdisplay::video::RotateBgra(rotationSource, 3, 2, 12, 3, rotated);
        Check(rotated[0] == 3 && rotated[4] == 6 && rotated[8] == 2 &&
            rotated[12] == 5 && rotated[16] == 1 && rotated[20] == 4,
            "BGRA counter-clockwise rotation");

        const std::vector<std::uint8_t> lengthPrefixed{
            0, 0, 0, 2, 0x65, 0xAA,
            0, 0, 0, 2, 0x41, 0xBB
        };
        const auto annexB = tabletdisplay::video::NormalizeH264AccessUnit(lengthPrefixed);
        Check(annexB == std::vector<std::uint8_t>({
            0, 0, 0, 1, 0x65, 0xAA,
            0, 0, 0, 1, 0x41, 0xBB
        }), "length-prefixed access unit to Annex B");
        Check(tabletdisplay::video::AnnexBContainsNalType(annexB, 5), "Annex B IDR detection");

        // An SPS without VUI gains bitstream_restriction with no reordering and a one-frame DPB;
        // the expected bytes were checked with FFmpeg's trace_headers.
        const std::vector<std::uint8_t> spsWithoutVui{
            0, 0, 0, 1, 0x67, 0x42, 0xC0, 0x28, 0xD9, 0x00, 0x50, 0x06, 0x59,
            0, 0, 0, 1, 0x68, 0xCE
        };
        const auto lowLatencySps = tabletdisplay::video::PatchSpsForLowLatency(spsWithoutVui);
        Check(lowLatencySps == std::vector<std::uint8_t>({
            0, 0, 0, 1, 0x67, 0x42, 0xC0, 0x28, 0xD9, 0x00, 0x50, 0x06, 0x5A, 0x01, 0xB4, 0x11, 0x08, 0xC9,
            0, 0, 0, 1, 0x68, 0xCE
        }), "low-latency SPS rewrite");
        Check(tabletdisplay::video::PatchSpsForLowLatency(lowLatencySps) == lowLatencySps,
            "low-latency SPS rewrite is idempotent");
        const auto patchedSynthetic =
            tabletdisplay::video::PatchSpsForLowLatency(synthetic.idrAccessUnit);
        Check(tabletdisplay::video::AnnexBContainsNalType(patchedSynthetic, 7) &&
            tabletdisplay::video::AnnexBContainsNalType(patchedSynthetic, 8) &&
            tabletdisplay::video::AnnexBContainsNalType(patchedSynthetic, 5),
            "low-latency SPS rewrite keeps the access unit");

        const std::vector<std::uint8_t> avcConfig{
            1, 66, 0, 40, 0xFF, 0xE1,
            0, 4, 0x67, 66, 0, 40,
            1, 0, 2, 0x68, 0xCE
        };
        const auto parsedConfig = tabletdisplay::video::ParseH264CodecConfig(avcConfig);
        Check(parsedConfig.profileIdc == 66 && parsedConfig.levelIdc == 40,
            "AVC configuration profile/level");
        Check(parsedConfig.nalLengthSize == 4, "AVC configuration NAL length");
        Check(tabletdisplay::video::AnnexBContainsNalType(parsedConfig.annexB, 7) &&
            tabletdisplay::video::AnnexBContainsNalType(parsedConfig.annexB, 8),
            "AVC configuration SPS/PPS");

        const tabletdisplay::ipc::FrameRequest frameRequest{};
        const tabletdisplay::ipc::FrameResponseHeader frameHeader{};
        Check(frameRequest.structureBytes == sizeof(frameRequest), "frame IPC request layout");
        Check(frameHeader.magic == tabletdisplay::ipc::kFrameMagic &&
            frameHeader.headerBytes == sizeof(frameHeader), "frame IPC response layout");
        Check(tabletdisplay::ipc::kMaxFrameWidth == 3840U &&
            tabletdisplay::ipc::kMaxFrameHeight == 3840U &&
            tabletdisplay::ipc::kMaxFrameBytes == 3840U * 2400U * 4U,
            "frame IPC byte limit");
        Check(sizeof(tabletdisplay::ipc::MonitorConnectionRequest) == 16, "monitor IPC request layout");

        using tabletdisplay::display::ScaledMode;
        const auto native = ScaledMode(1200, 1920, 1);
        Check(native.width == 1920 && native.height == 1200, "native mode is landscape");
        const auto third = ScaledMode(1920, 1200, 3);
        Check(third.width == 640 && third.height == 400, "one third of 1920x1200");
        const auto quarter = ScaledMode(2560, 1600, 4);
        Check(quarter.width == 640 && quarter.height == 400, "one quarter of 2560x1600");
        const auto oddThird = ScaledMode(2000, 1200, 3);
        Check(oddThird.width == 666 && oddThird.height == 400, "scaled sides round down to even");
        Check(ScaledMode(1920, 1200, 5).width == 0, "divisor above 4 is rejected");

        const auto modeConfig = [](std::uint16_t width, std::uint16_t height)
        {
            std::vector<std::uint8_t> payload(28 + 4, 0);
            payload[0] = 1;
            payload[4] = 1;
            payload[8] = static_cast<std::uint8_t>(width);
            payload[9] = static_cast<std::uint8_t>(width >> 8U);
            payload[10] = static_cast<std::uint8_t>(height);
            payload[11] = static_cast<std::uint8_t>(height >> 8U);
            payload[12] = 60;
            payload[14] = 1;
            payload[16] = 66;
            payload[17] = 40;
            payload[18] = 1;
            payload[24] = 4;
            payload[30] = 0;
            payload[31] = 1;
            const tabletdisplay::protocol::Header header{
                static_cast<std::uint16_t>(tabletdisplay::protocol::MessageType::VideoConfig),
                tabletdisplay::protocol::Critical,
                static_cast<std::uint32_t>(payload.size()), 2, 1 };
            tabletdisplay::protocol::ValidatePayload(header, payload);
        };
        modeConfig(960, 600);
        modeConfig(600, 960);
        bool oddRejected{};
        try
        {
            modeConfig(961, 600);
        }
        catch (const tabletdisplay::protocol::ProtocolException&)
        {
            oddRejected = true;
        }
        Check(oddRejected, "odd VIDEO_CONFIG width is rejected");

        std::cout << "TabletDisplay protocol: 14 golden vectors, H.264, pixel and frame IPC passed\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Protocol test failed: " << exception.what() << '\n';
        return 1;
    }
}
