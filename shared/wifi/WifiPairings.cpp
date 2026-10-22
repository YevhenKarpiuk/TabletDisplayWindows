#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dpapi.h>

#include "WifiPairings.h"

#include <algorithm>
#include <stdexcept>

namespace tabletdisplay::wifi
{
    namespace
    {
        constexpr wchar_t kSettingsKey[] = L"Software\\TabletDisplay";
        constexpr wchar_t kTabletsKey[] = L"Software\\TabletDisplay\\WifiTablets";

        std::optional<Bytes> ReadBinary(const std::wstring& key, const wchar_t* name)
        {
            DWORD size{};
            if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_BINARY, nullptr, nullptr, &size) !=
                ERROR_SUCCESS)
                return std::nullopt;
            Bytes value(size);
            if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_BINARY, nullptr, value.data(), &size) !=
                ERROR_SUCCESS)
                return std::nullopt;
            value.resize(size);
            return value;
        }

        std::wstring ReadString(const std::wstring& key, const wchar_t* name)
        {
            DWORD size{};
            if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_SZ, nullptr, nullptr, &size) !=
                ERROR_SUCCESS || size < sizeof(wchar_t))
                return {};
            std::wstring value(size / sizeof(wchar_t), L'\0');
            if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_SZ, nullptr, value.data(), &size) !=
                ERROR_SUCCESS)
                return {};
            value.resize(wcslen(value.c_str()));
            return value;
        }

        void WriteValue(const std::wstring& key, const wchar_t* name, DWORD type, const void* data, DWORD size)
        {
            if (RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, type, data, size) != ERROR_SUCCESS)
                throw std::runtime_error("Cannot write the TabletDisplay Wi-Fi settings");
        }

        void WriteString(const std::wstring& key, const wchar_t* name, const std::wstring& value)
        {
            WriteValue(key, name, REG_SZ, value.c_str(), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        }

        Bytes Protect(ByteView data)
        {
            DATA_BLOB input{ static_cast<DWORD>(data.size()), const_cast<BYTE*>(data.data()) };
            DATA_BLOB output{};
            if (!CryptProtectData(&input, L"TabletDisplay pairing key", nullptr, nullptr, nullptr,
                    CRYPTPROTECT_UI_FORBIDDEN, &output))
                throw std::runtime_error("Cannot protect the Wi-Fi pairing key");
            Bytes protectedData(output.pbData, output.pbData + output.cbData);
            LocalFree(output.pbData);
            return protectedData;
        }

        std::optional<Bytes> Unprotect(ByteView data)
        {
            DATA_BLOB input{ static_cast<DWORD>(data.size()), const_cast<BYTE*>(data.data()) };
            DATA_BLOB output{};
            if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
                return std::nullopt;
            Bytes plain(output.pbData, output.pbData + output.cbData);
            SecureZeroMemory(output.pbData, output.cbData);
            LocalFree(output.pbData);
            return plain;
        }

        std::wstring TabletKey(ByteView id)
        {
            return std::wstring(kTabletsKey) + L"\\" + ToHex(id);
        }
    }

    std::wstring ToHex(ByteView bytes)
    {
        static constexpr wchar_t digits[] = L"0123456789abcdef";
        std::wstring text;
        for (const auto byte : bytes)
        {
            text.push_back(digits[byte >> 4U]);
            text.push_back(digits[byte & 15U]);
        }
        return text;
    }

    std::optional<Bytes> FromHex(std::wstring_view text)
    {
        if (text.size() % 2 != 0) return std::nullopt;
        const auto nibble = [](wchar_t character) -> int
        {
            if (character >= L'0' && character <= L'9') return character - L'0';
            if (character >= L'a' && character <= L'f') return character - L'a' + 10;
            if (character >= L'A' && character <= L'F') return character - L'A' + 10;
            return -1;
        };
        Bytes bytes;
        for (std::size_t index = 0; index < text.size(); index += 2)
        {
            const auto high = nibble(text[index]);
            const auto low = nibble(text[index + 1]);
            if (high < 0 || low < 0) return std::nullopt;
            bytes.push_back(static_cast<std::uint8_t>(high * 16 + low));
        }
        return bytes;
    }

    Bytes WindowsId()
    {
        if (auto id = ReadBinary(kSettingsKey, L"WindowsId"); id && id->size() == kIdSize) return *id;
        auto id = RandomBytes(kIdSize);
        WriteValue(kSettingsKey, L"WindowsId", REG_BINARY, id.data(), static_cast<DWORD>(id.size()));
        return id;
    }

    std::string WindowsName()
    {
        wchar_t name[MAX_COMPUTERNAME_LENGTH + 1]{};
        DWORD size = ARRAYSIZE(name);
        if (!GetComputerNameW(name, &size)) return "Windows";
        const auto bytes = WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(size), nullptr, 0, nullptr, nullptr);
        std::string output(static_cast<std::size_t>(std::max(bytes, 0)), '\0');
        WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(size), output.data(), bytes, nullptr, nullptr);
        return output;
    }

    void SavePairing(const PairedTablet& tablet)
    {
        const auto key = TabletKey(tablet.id);
        const auto protectedKey = Protect(tablet.key);
        WriteValue(key, L"Key", REG_BINARY, protectedKey.data(), static_cast<DWORD>(protectedKey.size()));
        WriteString(key, L"Name", tablet.name);
        SaveTabletAddress(tablet.id, tablet.address, tablet.port);
    }

    std::optional<PairedTablet> LoadPairing(ByteView id)
    {
        const auto key = TabletKey(id);
        const auto protectedKey = ReadBinary(key, L"Key");
        if (!protectedKey) return std::nullopt;
        auto pairingKey = Unprotect(*protectedKey);
        if (!pairingKey || pairingKey->size() != kKeySize) return std::nullopt;
        PairedTablet tablet;
        tablet.id.assign(id.begin(), id.end());
        tablet.name = ReadString(key, L"Name");
        tablet.key = std::move(*pairingKey);
        tablet.address = ReadString(key, L"Address");
        DWORD port{};
        DWORD size = sizeof(port);
        if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"Port", RRF_RT_REG_DWORD, nullptr, &port, &size) ==
            ERROR_SUCCESS && port > 0 && port <= 65'535)
            tablet.port = static_cast<std::uint16_t>(port);
        return tablet;
    }

    std::vector<PairedTablet> LoadPairings()
    {
        std::vector<PairedTablet> tablets;
        HKEY key{};
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kTabletsKey, 0, KEY_ENUMERATE_SUB_KEYS, &key) != ERROR_SUCCESS)
            return tablets;
        for (DWORD index = 0;; ++index)
        {
            wchar_t name[64]{};
            DWORD nameChars = ARRAYSIZE(name);
            if (RegEnumKeyExW(key, index, name, &nameChars, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                break;
            const auto id = FromHex(name);
            if (!id || id->size() != kIdSize) continue;
            if (auto tablet = LoadPairing(*id)) tablets.push_back(std::move(*tablet));
        }
        RegCloseKey(key);
        return tablets;
    }

    void SaveTabletAddress(ByteView id, const std::wstring& address, std::uint16_t port)
    {
        const auto key = TabletKey(id);
        WriteString(key, L"Address", address);
        const DWORD value = port;
        WriteValue(key, L"Port", REG_DWORD, &value, sizeof(value));
    }

    void RemovePairing(ByteView id)
    {
        (void)RegDeleteTreeW(HKEY_CURRENT_USER, TabletKey(id).c_str());
    }
}
