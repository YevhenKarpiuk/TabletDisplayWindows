#pragma once

#include "WifiCrypto.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Windows identity and the tablets it is paired with over Wi-Fi, kept per user in
// HKCU\Software\TabletDisplay; pairing keys are protected with DPAPI.
namespace tabletdisplay::wifi
{
    struct PairedTablet
    {
        Bytes id;
        std::wstring name;
        Bytes key;
        std::wstring address;
        std::uint16_t port{ kWifiPort };
    };

    // Stable random identifier of this Windows user, created on first use.
    [[nodiscard]] Bytes WindowsId();
    [[nodiscard]] std::string WindowsName();

    void SavePairing(const PairedTablet& tablet);
    [[nodiscard]] std::optional<PairedTablet> LoadPairing(ByteView id);
    [[nodiscard]] std::vector<PairedTablet> LoadPairings();
    void SaveTabletAddress(ByteView id, const std::wstring& address, std::uint16_t port);
    void RemovePairing(ByteView id);

    [[nodiscard]] std::wstring ToHex(ByteView bytes);
    [[nodiscard]] std::optional<Bytes> FromHex(std::wstring_view text);
}
