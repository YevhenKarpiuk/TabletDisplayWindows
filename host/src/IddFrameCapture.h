#pragma once

#include "DesktopCapture.h"

#include <cstdint>
#include <memory>

namespace tabletdisplay::host
{
    class IddFrameCapture
    {
    public:
        IddFrameCapture();
        ~IddFrameCapture();
        IddFrameCapture(const IddFrameCapture&) = delete;
        IddFrameCapture& operator=(const IddFrameCapture&) = delete;

        [[nodiscard]] bool Capture(CapturedFrame& frame, std::uint32_t timeoutMs);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
