#include "SyntheticH264.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace tabletdisplay::video
{
    namespace
    {
        // One 1280x800 SMPTE color-bar IDR encoded as AVC Baseline by libx264.
        // FFmpeg is used only to regenerate and validate this compile-time fixture.
        constexpr std::string_view kEncodedAccessUnit =
            "AAAAAWdCwCjcBQBlsBagQECoAAADAAgAAAMB4HjBnAAAAAFozg8sgAAAAQYF//9a3EXpvebZSLeWLNgg2SPu73gyNjQgLSBj"
            "b3JlIDE2NSByMzIyMyAwNDgwY2IwIC0gSC4yNjQvTVBFRy00IEFWQyBjb2RlYyAtIENvcHlsZWZ0IDIwMDMtMjAyNSAtIGh0"
            "dHA6Ly93d3cudmlkZW9sYW4ub3JnL3gyNjQuaHRtbCAtIG9wdGlvbnM6IGNhYmFjPTAgcmVmPTEgZGVibG9jaz0xOjA6MCBh"
            "bmFseXNlPTB4MToweDExMSBtZT1oZXggc3VibWU9NyBwc3k9MSBwc3lfcmQ9MS4wMDowLjAwIG1peGVkX3JlZj0wIG1lX3Jh"
            "bmdlPTE2IGNocm9tYV9tZT0xIHRyZWxsaXM9MSA4eDhkY3Q9MCBjcW09MCBkZWFkem9uZT0yMSwxMSBmYXN0X3Bza2lwPTEg"
            "Y2hyb21hX3FwX29mZnNldD0tMiB0aHJlYWRzPTI1IGxvb2thaGVhZF90aHJlYWRzPTQgc2xpY2VkX3RocmVhZHM9MCBucj0w"
            "IGRlY2ltYXRlPTEgaW50ZXJsYWNlZD0wIGJsdXJheV9jb21wYXQ9MCBjb25zdHJhaW5lZF9pbnRyYT0wIGJmcmFtZXM9MCB3"
            "ZWlnaHRwPTAga2V5aW50PTEga2V5aW50X21pbj0xIHNjZW5lY3V0PTAgaW50cmFfcmVmcmVzaD0wIHJjPWNyZiBtYnRyZWU9"
            "MCBjcmY9MjMuMCBxY29tcD0wLjYwIHFwbWluPTAgcXBtYXg9NjkgcXBzdGVwPTQgaXBfcmF0aW89MS40MCBhcT0xOjEuMDAA"
            "gAAAAWWIhAS8mKAAK00nJycnJycnJycnEQSA4AB4B+IAAIHwAAgRxAAEaSCWTk5OTk5OTk5OTiMUAATUY4AAvrRwABbXk5OT"
            "k5OTk5OTk4iCQHAAEAAAvEAAED4AAQI4gACNJBLJycnJycnJycnJxGKAAJfccAAX1o4AAzrScnJycnJycnJycRBIDgACAAAX"
            "iAACB8AAIEcQABGkglk5OTk5OTk5OTk4jFAAEzWOAAL60cAAW15OTk5OTk5OTk5Ouuuuuuuuuuunrrrrrrrrrrrp66666666"
            "666euuuuuuuuuuunrrrrrrrrrrp666666666666euuuuuuuuuuuuuuuuuuuuunrrrrrrrrrrrp66666666666euuuuuuuuuu"
            "unrrrrrrrrrrp666666666666euuuuuuuuuuuuuuuuuuuuunrrrrrrrrrrrp66666666666euuuuuuuuuuunrrrrrrrrrrp6"
            "66666666666euuuuuuuuuuuuuuuuuuuuunrrrrrrrrrrrp66666666666euuuuuuuuuuunrrrrrrrrrrp666666666666euu"
            "uuuuuuuuuuuuuuuuuuunrrrrrrrrrrrp66666666666euuuuuuuuuuunrrrrrrrrrrp666666666666euuuuuuuuuuuuuuuu"
            "uuuuunrrrrrrrrrrrp66666666666euuuuuuuuuuunrrrrrrrrrrp666666666666euuuuuuuuuuuuuuuuuuuuunrrrrrrrr"
            "rrrp66666666666euuuuuuuuuuunrrrrrrrrrrp666666666666euuuuuuuuuuuuuuuuuuuuunrrrrrrrrrrrp6666666666"
            "6euuuuuuuuuuunrrrrrrrrrrp666666666666euuuuuuuuuuuuuuuuuuuuunrrrrrrrrrrrp66666666666euuuuuuuuuuun"
            "rrrrrrrrrrp666666666666euuuuuuuuuuuuuuuuuuuuunrrrrrrrrrrrp66666666666euuuuuuuuuuunrrrrrrrrrrp666"
            "666666666euuuuuuuuuuuuuuuuuuuuunrrrrrrrrrrrp66666666666euuuuuuuuuuunrrrrrrrrrrp666666666666euuuu"
            "uuuuuuuuuuuuuuuuunrrrrrrrrrrrp66666666666euuuuuuuuuuunrrrrrrrrrrp666666666666euuuuuuuuuuuuuuuuuu"
            "uuunrrrrrrrrrrrp66666666666euuuuuuuuuuunrrrrrrrrrrp666666666666euuuuuuuuuuuuuuuuuuuuunrrrrrrrrrr"
            "rp66666666666euuuuuuuuuuunrrrrrrrrrrp666666666666euuuuuuuuuuuuuuuuuuuuunrrrrrrrrrrrp66666666666e"
            "uuuuuuuuuuunrrrrrrrrrrp666666666666euuuuuuuuuuuuuuuuuuuuunrrrrrrrrrrrp66666666666euuuuuuuuuuunrr"
            "rrrrrrrrp666666666666euuuuuuuuuuuuuuuuuuuuunrrrrrrrrrrrp66666666666euuuuuuuuuuunrrrrrrrrrrp66666"
            "6666666euuuuuuuuuuuuuuuuuuuuunrrrrrrrrrrrp66666666666euuuuuuuuuuunrrrrrrrrrrp666666666666euuuuuu"
            "uuuuuuuuuuuuuuunrrrrrrrrrrrp66666666666euuuuuuuuuuunrrrrrrrrrrp666666666666euuuuuuuuuuuuuuuuuuuu"
            "unrrrrrrrrrrrp66666666666euuuuuuuuuuunrrrrrrrrrrp666666666666euuuuuuuuuuuuuuuuuuuuunrrrrrrrrrrrp"
            "66666666666euuuuuuuuuuunrrrrrrrrrrp666666666666euuuuuuuuuuuuuuuuuuuuunrrrrrrrrrrrp66666666666euu"
            "uuuuuuuuunrrrrrrrrrrp666666666666euuuuuuuuuuuuuuuuuuuuunrrrrrrrrrrrp66666666666euuuuuuuuuuunrrrr"
            "rrrrrrp666666666666euuuuuuuuuuuuuuuuuuuuunrrrrrrrrrrrp66666666666euuuuuuuuuuunrrrrrrrrrrp6666666"
            "66666euuuuuuuuuuuuuuuuuuuuunrrrrrrrrrrrp66666666666euuuuuuuuuuunrrrrrrrrrrp666666666666euuuuuuuu"
            "uuuuuuuuuuuuunrrrrrrrrrrrp66666666666euuuuuuuuuuunrrrrrrrrrrp666666666666euuuuuuuuuuuuuuuuuuuuun"
            "rrrrrrrrrrrp66666666666euuuuuuuuuuunrrrrrrrrrrp666666666666euuuuuuuuuuuuuuuuuuuuunrrrrrrrrrrrp66"
            "666666666euuuuuuuuuuunrrrrrrrrrrp666666666666euuuuuuuuuuuuuuuuuuuuunrrrrrrrrrrrp66666666666euuuu"
            "uuuuuuunrrrrrrrrrrp666666666666euuuuuuuuuuuuuuuuuuuuunrrrrrrrrrrrp66666666666euuuuuuuuuuunrrrrrr"
            "rrrrp666666666666euuuuuuuuuuuuuuuuuuuuunrrrrrrrrrrrp66666666666euuuuuuuuuuunrrrrrrrrrrp666666666"
            "666euuuuuuuuuuuuuuuuuuuuunrrrrrrrrrrrp66666666666euuuuuuuuuuunrrrrrrrrrrp666666666666euuuuuuuuuv"
            "4//wVgqwOACAGBbLEAAEBEAAQFkIEVAcBGLlhwEYuX4VyyXL9ra2tra2tra2trF4NAAJAqhILmOgOAjFyw4CMXL8K5ZLl+1t"
            "bW1tbW1tbW1tcIhgAf4K+B0CrLCAI5wDgOAAIDYBwAoDpHLDpHL8DgAEQEDZYcAAiAgbL86/lra2tra2tra2vjCAeHgs4HEC"
            "ArLCRdhAAiIDpHLDiCFl+BwACICBssOCITL9q7W1tbW1tbW1tbXDDAP+CvgeBiMsIMAQ4ICAAIBgYC4HmUsPMpfgcAAgAoZL"
            "DgAEAFDJftbW1tbW1tbW1tfGGH/BZwOgQrLCZwgACOgPMpYdI5fgcAAgAoZLDgRypftFNjW1tbW1tbW1tbXCIYAH+CsmBwAg"
            "BwQywoAgBFhACOSkDgRypYcCOVL8LMsmZfnVJi1tbW1tbW1tbWoK66666666666euuuuuuuuuuuZ40AAzHA11111111110wT"
            "F6gnrrrrrrrrrrp66666666666YKlUFdddddddddddKKAAbpaWlpaWlpaWlpa66666666666euuuuuuuuuuunrrrrrrrrrrp"
            "666666666666euuuuuuuuuumCp1BXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXT1111111111109ddddddddddPXXXXXXXXXX"
            "XT11111111110wVKoK6666666666666666666665oIgcAAQCwA2CAACBEAAIA6EAAJB0dZUVHWZGR1lRUdZkZHWVFR1mRkdZ"
            "UVHWZGR1lRUdZkZfDjwDhGEF3YIBIQrW1/8cPgtFQOABABwIQcDhjgYGtgeZXPA6R3N9999999+H8AwDhGIAASDMQAAwBG1t"
            "bW1/8fDQLsUAArwgu7BW1weBscQLB0JB7zp11Ovp11Ovvw48A4RhAE5zBEzI60qa2tra2tra//hHsFpwOICqC+EC7MEtLA4E"
            "IZc8DxEubi78P4BgHCIWEAJmIAAIAIDs62tbW1tbW1tbW1/DjwAMEMUfcebH3/+IwHwQYMHQdxN9//iGHggJFD3334fwAIBw"
            "QBQv77777777775oLY4DHAaWlpaWlpaWlpaWlrrmYLLqgtrrrrrrrpaWlpaWZgs9UFtLXS10tdddddddczBBqmCGulpaWlpa"
            "WlpaWlpaho3UN1111111111x0LWOx/Y7H9QvXXXXXXXXXXXXXXXXXXXXXXXT111111111111109dddddddddddddczBB6oIa"
            "6666666666666666666666666666666666666666666666666euuuuuuuuuuuuunrrrrrrrrrrrrrrmYINUwQ11111111111"
            "1111111111111111111111111111111111111109dddddddddddddPXXXXXXXXXXXXXXMwQeqCGuuuuuuuuuuuuuuuuuuuuu"
            "uuuuuuuuuuuuuuuuuuuuuuuuuuuunrrrrrrrrrrrrrp666666666666665mCDVMENddddddddddddddddddddddddddddddd"
            "ddddddddddddddddddPXXXXXXXXXXXXXT11111111111111zMEHqghrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrr"
            "rrrrrrrp66666666666666euuuuuuuuuuuuuuZgg1TBDXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXT11"
            "1111111111109dddddddddddddczBB6oIa6666666666666666666666666666666666666666666666666euuuuuuuuuuuu"
            "unrrrrrrrrrrrrrrmYINUwQ111111111111111111111111111111111111111111111111109dddddddddddddPXXXXXXXX"
            "XXXXXXMwQeqCGuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuunrrrrrrrrrrrrrp666666666666665mCDV"
            "MENdddddddddddddddddddddddddddddddddddddddddddddddddPXXXXXXXXXXXXXT11111111111111zMEHqghrrrrrrrr"
            "rrrrrrrrrrrrrrrrrrrrrrrrrrrw";

        constexpr std::size_t kCodecSpecificDataSize = 40;
        constexpr std::size_t kIdrOffset = 649;

        [[nodiscard]] int Base64Value(char value)
        {
            if (value >= 'A' && value <= 'Z') return value - 'A';
            if (value >= 'a' && value <= 'z') return value - 'a' + 26;
            if (value >= '0' && value <= '9') return value - '0' + 52;
            if (value == '+') return 62;
            if (value == '/') return 63;
            return -1;
        }

        [[nodiscard]] std::vector<std::uint8_t> DecodeBase64(std::string_view encoded)
        {
            std::vector<std::uint8_t> bytes;
            bytes.reserve(encoded.size() * 3U / 4U);
            std::uint32_t accumulator{};
            unsigned bitCount{};
            for (const auto character : encoded)
            {
                if (character == '=') break;
                const auto value = Base64Value(character);
                if (value < 0) throw std::runtime_error("Invalid base64 video fixture");
                accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(value);
                bitCount += 6U;
                if (bitCount >= 8U)
                {
                    bitCount -= 8U;
                    bytes.push_back(static_cast<std::uint8_t>(accumulator >> bitCount));
                    accumulator &= (1U << bitCount) - 1U;
                }
            }
            return bytes;
        }
    }

    SyntheticH264 BuildSyntheticColorBars()
    {
        const auto encoded = DecodeBase64(kEncodedAccessUnit);
        if (encoded.size() <= kIdrOffset)
        {
            throw std::runtime_error("Invalid synthetic video fixture");
        }

        return {
            1280,
            800,
            30,
            66,
            40,
            { encoded.begin(), encoded.begin() + kCodecSpecificDataSize },
            { encoded.begin(), encoded.end() },
        };
    }
}
