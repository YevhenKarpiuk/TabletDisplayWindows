#pragma once

#include <cstdint>
#include <memory>

namespace tabletdisplay::host
{
    enum class TouchAction : std::uint8_t
    {
        Down = 1,
        Move = 2,
        Up = 3,
        Cancel = 4
    };

    // Windows touch input for one tablet. Unlike mouse input it does not move the
    // cursor, so several tablets can be touched at the same time, and Windows touch gestures
    // (panning, press-and-hold, the touch keyboard) work.
    class TouchInjector
    {
    public:
        // Throws when Windows cannot initialize touch injection.
        TouchInjector();
        ~TouchInjector();
        TouchInjector(const TouchInjector&) = delete;
        TouchInjector& operator=(const TouchInjector&) = delete;

        // x and y are physical virtual-desktop pixels; pressure is 0..65535 (0 = unknown).
        void Inject(TouchAction action, long x, long y, std::uint16_t pressure);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
