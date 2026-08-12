#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace tabletdisplay::host
{
    struct CapturedFrame
    {
        std::span<const std::uint8_t> bgra;
        std::uint32_t width{};
        std::uint32_t height{};
        std::size_t stride{};
    };

    class DesktopCapture
    {
    public:
        DesktopCapture(std::uint32_t width, std::uint32_t height);
        ~DesktopCapture();
        DesktopCapture(const DesktopCapture&) = delete;
        DesktopCapture& operator=(const DesktopCapture&) = delete;

        [[nodiscard]] CapturedFrame Capture();

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
