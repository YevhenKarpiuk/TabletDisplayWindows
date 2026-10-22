#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "TabletMonitors.h"
#include "ipc/FrameTransport.h"

#include <cstdlib>
#include <vector>

namespace tabletdisplay::host
{
    namespace
    {
        // DISPLAYCONFIG reports EDID IDs as 16-bit values; accept either byte order.
        [[nodiscard]] bool SameEdidId(std::uint16_t reported, std::uint16_t expected) noexcept
        {
            return reported == expected || _byteswap_ushort(reported) == expected;
        }

        [[nodiscard]] bool IsTabletDisplayAdapter(const wchar_t* gdiDeviceName) noexcept
        {
            DISPLAY_DEVICEW display{};
            display.cb = sizeof(display);
            for (DWORD index = 0; EnumDisplayDevicesW(nullptr, index, &display, 0); ++index)
            {
                if (wcscmp(display.DeviceName, gdiDeviceName) == 0)
                    return wcscmp(display.DeviceString, L"TabletDisplay Virtual Display") == 0;
                display = {};
                display.cb = sizeof(display);
            }
            return false;
        }
    }

    std::optional<std::wstring> FindTabletDisplayDevice(std::uint32_t slot)
    {
        if (slot >= tabletdisplay::ipc::kMaxMonitors) return std::nullopt;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths;
        std::vector<DISPLAYCONFIG_MODE_INFO> modes;
        for (;;)
        {
            UINT32 pathCount{};
            UINT32 modeCount{};
            if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
                return std::nullopt;
            paths.resize(pathCount);
            modes.resize(modeCount);
            const auto result = QueryDisplayConfig(
                QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
            if (result == ERROR_INSUFFICIENT_BUFFER) continue;
            if (result != ERROR_SUCCESS) return std::nullopt;
            paths.resize(pathCount);
            break;
        }

        const auto productCode = tabletdisplay::ipc::EdidProductCode(slot);
        std::vector<std::wstring> tabletDisplays;
        bool edidIdsMissing{};
        for (const auto& path : paths)
        {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source.header.size = sizeof(source);
            source.header.adapterId = path.sourceInfo.adapterId;
            source.header.id = path.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
                !IsTabletDisplayAdapter(source.viewGdiDeviceName))
                continue;
            tabletDisplays.emplace_back(source.viewGdiDeviceName);

            DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
            target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            target.header.size = sizeof(target);
            target.header.adapterId = path.targetInfo.adapterId;
            target.header.id = path.targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS || !target.flags.edidIdsValid)
            {
                edidIdsMissing = true;
                continue;
            }
            if (SameEdidId(target.edidManufactureId, tabletdisplay::ipc::kEdidManufacturerId) &&
                SameEdidId(target.edidProductCodeId, productCode))
                return tabletDisplays.back();
        }

        // Without EDID IDs only the first monitor of a single tablet can be identified.
        if (edidIdsMissing && slot == 0 && tabletDisplays.size() == 1) return tabletDisplays.front();
        return std::nullopt;
    }
}
