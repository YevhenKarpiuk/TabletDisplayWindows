#include "protocol/Protocol.h"

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
        Check(cases.size() == 13, "unexpected vector count");

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

        std::cout << "TabletDisplay protocol: 13 golden vectors passed\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Protocol test failed: " << exception.what() << '\n';
        return 1;
    }
}
