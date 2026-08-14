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
    inline constexpr std::uint32_t kMaxFrameWidth = 1920;
    inline constexpr std::uint32_t kMaxFrameHeight = 1920;
    inline constexpr std::uint32_t kBytesPerPixel = 4;
    inline constexpr std::uint32_t kMaxFrameBytes =
        1920U * 1200U * kBytesPerPixel;
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

    static_assert(sizeof(FrameRequest) == 16);
    static_assert(sizeof(FrameResponseHeader) == 64);
    static_assert(sizeof(SharedFrameRequest) == 24);
    static_assert(sizeof(SharedFrameResponse) == 256);
    static_assert(sizeof(FrameTransportRequest) == 16);
}
