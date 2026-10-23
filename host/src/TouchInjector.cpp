#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "TouchInjector.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>

namespace tabletdisplay::host
{
    namespace
    {
        // A held contact is repeated so Windows sees it as alive while the finger rests.
        constexpr auto kHoldRefreshInterval = std::chrono::milliseconds(100);
        constexpr long kContactRadius = 2;
        constexpr UINT32 kMaxPressure = 1'024;
        // Windows moves the cursor to a touch for applications that only understand the mouse.
        // Once the finger lifts the cursor goes back, checked a few times because that move is
        // asynchronous.
        constexpr auto kCursorRestoreDelay = std::chrono::milliseconds(30);
        constexpr int kCursorRestoreChecks = 10;
        constexpr long kCursorTouchTolerance = 3;
    }

    // InjectTouchInput rather than a synthetic pointer device: Windows maps a synthetic touch
    // device to the primary monitor only, so touches on a tablet monitor landed on its edge.

    struct TouchInjector::Impl
    {
        Impl()
        {
            // Each Host serves one tablet, so the process-wide touch injector belongs to it.
            if (!InitializeTouchInjection(1, TOUCH_FEEDBACK_DEFAULT))
            {
                throw std::runtime_error(
                    "Cannot initialize touch injection, Windows error " + std::to_string(GetLastError()));
            }
            refresher = std::jthread([this](std::stop_token stopToken) { RefreshHeldContact(stopToken); });
        }

        ~Impl()
        {
            refresher.request_stop();
            wake.notify_all();
            if (refresher.joinable()) refresher.join();
            {
                std::lock_guard lock(mutex);
                if (inContact) SendLocked(POINTER_FLAG_UP, lastX, lastY, 0);
            }
        }

        void Inject(TouchAction action, long x, long y, std::uint16_t pressure)
        {
            std::lock_guard lock(mutex);
            const auto contactPressure = pressure == 0
                ? kMaxPressure / 2
                : static_cast<UINT32>(pressure) * kMaxPressure / 65'535U;
            switch (action)
            {
            case TouchAction::Down:
                // A DOWN while a contact is held means its UP was lost: lift it first.
                if (inContact) SendLocked(POINTER_FLAG_UP, lastX, lastY, 0);
                if (restoreChecksLeft == 0) (void)GetCursorPos(&savedCursor);
                restoreChecksLeft = 0;
                inContact = SendLocked(
                    POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT, x, y, contactPressure);
                break;
            case TouchAction::Move:
                if (!inContact) return;
                (void)SendLocked(
                    POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT, x, y, contactPressure);
                break;
            case TouchAction::Up:
            case TouchAction::Cancel:
                if (!inContact) return;
                (void)SendLocked(
                    POINTER_FLAG_UP | (action == TouchAction::Cancel ? POINTER_FLAG_CANCELED : 0), x, y, 0);
                inContact = false;
                restoreChecksLeft = kCursorRestoreChecks;
                wake.notify_all();
                break;
            }
            lastX = x;
            lastY = y;
            lastPressure = contactPressure;
            lastInjection = std::chrono::steady_clock::now();
        }

        // UIPI rejects input to elevated windows and the secure desktop; such a contact is
        // simply lost, as with mouse input.
        bool SendLocked(POINTER_FLAGS flags, long x, long y, UINT32 pressure)
        {
            POINTER_TOUCH_INFO touch{};
            touch.pointerInfo.pointerType = PT_TOUCH;
            touch.pointerInfo.pointerId = 0;
            touch.pointerInfo.pointerFlags = flags;
            touch.pointerInfo.ptPixelLocation = { x, y };
            touch.touchFlags = TOUCH_FLAG_NONE;
            touch.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_PRESSURE;
            touch.rcContact = { x - kContactRadius, y - kContactRadius, x + kContactRadius, y + kContactRadius };
            touch.pressure = pressure;
            return InjectTouchInput(1, &touch) != FALSE;
        }

        void RefreshHeldContact(std::stop_token stopToken)
        {
            std::unique_lock lock(mutex);
            while (!stopToken.stop_requested())
            {
                (void)wake.wait_for(
                    lock, stopToken, restoreChecksLeft > 0 ? kCursorRestoreDelay : kHoldRefreshInterval,
                    [] { return false; });
                if (stopToken.stop_requested()) return;
                if (restoreChecksLeft > 0 && !inContact) RestoreCursorLocked();
                if (inContact && std::chrono::steady_clock::now() - lastInjection >= kHoldRefreshInterval)
                {
                    (void)SendLocked(
                        POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT,
                        lastX, lastY, lastPressure);
                    lastInjection = std::chrono::steady_clock::now();
                }
            }
        }

        // Puts the cursor back where it was before the touch, unless the mouse moved it since.
        void RestoreCursorLocked()
        {
            --restoreChecksLeft;
            POINT cursor{};
            if (!GetCursorPos(&cursor)) return;
            const bool atTouch = labs(cursor.x - lastX) <= kCursorTouchTolerance &&
                labs(cursor.y - lastY) <= kCursorTouchTolerance;
            if (atTouch) (void)SetCursorPos(savedCursor.x, savedCursor.y);
            else if (cursor.x != savedCursor.x || cursor.y != savedCursor.y) restoreChecksLeft = 0;
        }

        std::mutex mutex;
        std::condition_variable_any wake;
        bool inContact{};
        long lastX{};
        long lastY{};
        UINT32 lastPressure{};
        POINT savedCursor{};
        int restoreChecksLeft{};
        std::chrono::steady_clock::time_point lastInjection{};
        std::jthread refresher;
    };

    TouchInjector::TouchInjector() : m_impl(std::make_unique<Impl>()) {}
    TouchInjector::~TouchInjector() = default;

    void TouchInjector::Inject(TouchAction action, long x, long y, std::uint16_t pressure)
    {
        m_impl->Inject(action, x, y, pressure);
    }
}
