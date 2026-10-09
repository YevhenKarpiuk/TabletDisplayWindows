#pragma once

#include "DesktopCapture.h"

#include <cstdint>
#include <memory>
#include <string>

namespace tabletdisplay::host
{
    struct SharedCapturedFrame
    {
        std::wstring resourceName;
        std::uint32_t width{};
        std::uint32_t height{};
        std::int64_t adapterLuid{};
        std::uint64_t sequence{};
        std::uint64_t timestampUs{};
        std::uint64_t generation{};
    };

    class IddFrameCapture
    {
    public:
        IddFrameCapture();
        ~IddFrameCapture();
        IddFrameCapture(const IddFrameCapture&) = delete;
        IddFrameCapture& operator=(const IddFrameCapture&) = delete;

        [[nodiscard]] bool Capture(CapturedFrame& frame, std::uint32_t timeoutMs);
        [[nodiscard]] bool ProbeShared(SharedCapturedFrame& frame, std::uint32_t timeoutMs);
        [[nodiscard]] bool CaptureShared(SharedCapturedFrame& frame, std::uint32_t timeoutMs);
        void SetMonitorConnected(bool connected);
        void SetSharedTransport(bool enabled, std::uint64_t generation = 0);
        [[nodiscard]] bool UsesSharedTransport() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
