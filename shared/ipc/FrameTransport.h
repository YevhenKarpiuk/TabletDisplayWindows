#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

#include <cstdint>

namespace tabletdisplay::ipc
{
    inline constexpr GUID kFrameDeviceInterface =
    { 0xc6e7470b, 0x1371, 0x4f25, { 0x9e, 0xec, 0x4e, 0xf5, 0x8d, 0x3a, 0x84, 0xce } };

    inline constexpr std::uint32_t kFrameMagic = 0x31464454U; // "TDF1"
    inline constexpr std::uint32_t kSharedFrameMagic = 0x31534454U; // "TDS1"
    inline constexpr std::uint16_t kFrameVersionMajor = 1;
    inline constexpr std::uint16_t kFrameVersionMinor = 0;
    inline constexpr std::uint32_t kPixelFormatBgra8 = 1;
    // Landscape IddCx frames up to the largest tablet panels (3840x2400).
    inline constexpr std::uint32_t kMaxFrameWidth = 3840;
    inline constexpr std::uint32_t kMaxFrameHeight = 3840;
    inline constexpr std::uint32_t kBytesPerPixel = 4;
    inline constexpr std::uint32_t kMaxFrameBytes =
        3840U * 2400U * kBytesPerPixel;
    inline constexpr std::uint32_t kSharedResourceNameChars = 96;

    struct FrameRequest
    {
        std::uint32_t structureBytes{ sizeof(FrameRequest) };
        std::uint32_t reserved{};
        std::uint64_t afterSequence{};
    };

    struct FrameResponseHeader
    {
        std::uint32_t magic{ kFrameMagic };
        std::uint16_t versionMajor{ kFrameVersionMajor };
        std::uint16_t versionMinor{ kFrameVersionMinor };
        std::uint32_t headerBytes{ sizeof(FrameResponseHeader) };
        std::uint32_t pixelFormat{ kPixelFormatBgra8 };
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint32_t stride{};
        std::uint32_t frameBytes{};
        std::uint64_t sequence{};
        std::uint64_t timestampUs{};
        std::uint64_t reserved0{};
        std::uint64_t reserved1{};
    };

    struct SharedFrameRequest
    {
        std::uint32_t structureBytes{ sizeof(SharedFrameRequest) };
        std::uint32_t reserved{};
        std::uint64_t afterSequence{};
        std::uint64_t afterGeneration{};
    };

    struct SharedFrameResponse
    {
        std::uint32_t magic{ kSharedFrameMagic };
        std::uint16_t versionMajor{ kFrameVersionMajor };
        std::uint16_t versionMinor{ kFrameVersionMinor };
        std::uint32_t structureBytes{ sizeof(SharedFrameResponse) };
        std::uint32_t pixelFormat{ kPixelFormatBgra8 };
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint32_t resourceNameLength{};
        std::uint32_t adapterLuidLow{};
        std::int32_t adapterLuidHigh{};
        std::uint64_t sequence{};
        std::uint64_t timestampUs{};
        std::uint64_t generation{};
        wchar_t resourceName[kSharedResourceNameChars]{};
    };

    inline constexpr std::uint32_t kFrameTransportCpu = 0;
    inline constexpr std::uint32_t kFrameTransportSharedTexture = 1;

    struct FrameTransportRequest
    {
        std::uint32_t structureBytes{ sizeof(FrameTransportRequest) };
        std::uint32_t mode{ kFrameTransportCpu };
        std::uint64_t generation{};
    };

    // The driver offers one monitor per connected tablet; each Host process (driver file
    // object) leases one monitor slot and receives only that monitor's frames.
    inline constexpr std::uint32_t kMaxMonitors = 4;

    // nativeWidth/nativeHeight describe the tablet panel; the monitor offers that resolution
    // divided by 1..4. Zero keeps the legacy 1920x1200/1280x800 mode list.
    // slot 0 leases the first free monitor; slot N (1..kMaxMonitors) asks for monitor N - 1,
    // so a tablet keeps its Windows display position across reconnects.
    struct MonitorConnectionRequest
    {
        std::uint32_t structureBytes{ sizeof(MonitorConnectionRequest) };
        std::uint32_t connected{};
        std::uint16_t nativeWidth{};
        std::uint16_t nativeHeight{};
        std::uint16_t preferredDivisor{};
        std::uint16_t slot{};
    };

    // Optional output of kSetMonitorConnectionIoctl: the zero-based leased monitor slot.
    struct MonitorConnectionResponse
    {
        std::uint32_t structureBytes{ sizeof(MonitorConnectionResponse) };
        std::uint32_t slot{};
    };

    // Each monitor slot reports its own EDID: the base EDID with the product code and serial
    // number advanced by the slot, so Windows keeps a separate layout for every tablet.
    inline constexpr std::uint16_t kEdidManufacturerId = 0x10AC; // "DEL", big-endian in the EDID.
    inline constexpr std::uint16_t kEdidBaseProductCode = 0xD0E6;

    [[nodiscard]] constexpr std::uint16_t EdidProductCode(std::uint32_t slot) noexcept
    {
        return static_cast<std::uint16_t>(kEdidBaseProductCode + slot);
    }

    // Output of kGetFrameEventIoctl: the name of an auto-reset event that the driver signals
    // after every new frame of the caller's leased monitor, so the Host waits instead of polling.
    struct FrameEventResponse
    {
        std::uint32_t structureBytes{ sizeof(FrameEventResponse) };
        std::uint32_t nameLength{};
        wchar_t name[kSharedResourceNameChars]{};
    };

    inline constexpr ULONG kGetLatestFrameIoctl = CTL_CODE(
        FILE_DEVICE_UNKNOWN,
        0x800,
        METHOD_OUT_DIRECT,
        FILE_READ_ACCESS);
    inline constexpr ULONG kGetSharedFrameIoctl = CTL_CODE(
        FILE_DEVICE_UNKNOWN,
        0x801,
        METHOD_BUFFERED,
        FILE_READ_ACCESS);
    inline constexpr ULONG kSetFrameTransportIoctl = CTL_CODE(
        FILE_DEVICE_UNKNOWN,
        0x802,
        METHOD_BUFFERED,
        FILE_READ_ACCESS);
    inline constexpr ULONG kSetMonitorConnectionIoctl = CTL_CODE(
        FILE_DEVICE_UNKNOWN,
        0x803,
        METHOD_BUFFERED,
        FILE_READ_ACCESS);
    inline constexpr ULONG kGetFrameEventIoctl = CTL_CODE(
        FILE_DEVICE_UNKNOWN,
        0x804,
        METHOD_BUFFERED,
        FILE_READ_ACCESS);

    static_assert(sizeof(FrameRequest) == 16);
    static_assert(sizeof(FrameResponseHeader) == 64);
    static_assert(sizeof(SharedFrameRequest) == 24);
    static_assert(sizeof(SharedFrameResponse) == 256);
    static_assert(sizeof(FrameTransportRequest) == 16);
    static_assert(sizeof(MonitorConnectionRequest) == 16);
    static_assert(sizeof(MonitorConnectionResponse) == 8);
    static_assert(sizeof(FrameEventResponse) == 8 + kSharedResourceNameChars * sizeof(wchar_t));
}
