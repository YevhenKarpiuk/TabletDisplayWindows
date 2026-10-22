#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace tabletdisplay::host
{
    // The GDI device name (\\.\DISPLAYn) of the TabletDisplay monitor in the given driver
    // slot while Windows shows it on the desktop. Every slot has its own EDID product code,
    // which tells the monitors of several connected tablets apart.
    [[nodiscard]] std::optional<std::wstring> FindTabletDisplayDevice(std::uint32_t slot);
}
