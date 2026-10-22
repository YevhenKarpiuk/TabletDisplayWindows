#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>

#include "IddFrameCapture.h"
#include "TabletMonitors.h"
#include "ipc/FrameTransport.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace tabletdisplay::host
{
    namespace
    {
        class DeviceInfoHandle
        {
        public:
            explicit DeviceInfoHandle(HDEVINFO value) noexcept : m_value(value) {}
            ~DeviceInfoHandle()
            {
                if (m_value != INVALID_HANDLE_VALUE)
                {
                    SetupDiDestroyDeviceInfoList(m_value);
                }
            }
            DeviceInfoHandle(const DeviceInfoHandle&) = delete;
            DeviceInfoHandle& operator=(const DeviceInfoHandle&) = delete;
            [[nodiscard]] HDEVINFO get() const noexcept { return m_value; }

        private:
            HDEVINFO m_value{ INVALID_HANDLE_VALUE };
        };

        class WinHandle
        {
        public:
            explicit WinHandle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : m_value(value) {}
            ~WinHandle()
            {
                if (m_value != INVALID_HANDLE_VALUE && m_value != nullptr)
                {
                    CloseHandle(m_value);
                }
            }
            WinHandle(const WinHandle&) = delete;
            WinHandle& operator=(const WinHandle&) = delete;
            WinHandle(WinHandle&& other) noexcept : m_value(other.release()) {}
            WinHandle& operator=(WinHandle&& other) noexcept
            {
                if (this != &other)
                {
                    reset(other.release());
                }
                return *this;
            }
            [[nodiscard]] HANDLE get() const noexcept { return m_value; }
            [[nodiscard]] bool valid() const noexcept
            {
                return m_value != INVALID_HANDLE_VALUE && m_value != nullptr;
            }
            [[nodiscard]] HANDLE release() noexcept
            {
                const auto value = m_value;
                m_value = INVALID_HANDLE_VALUE;
                return value;
            }
            void reset(HANDLE value = INVALID_HANDLE_VALUE) noexcept
            {
                if (valid())
                {
                    CloseHandle(m_value);
                }
                m_value = value;
            }

        private:
            HANDLE m_value{ INVALID_HANDLE_VALUE };
        };

        [[nodiscard]] WinHandle OpenFrameDevice()
        {
            DeviceInfoHandle deviceInfo(SetupDiGetClassDevsW(
                &tabletdisplay::ipc::kFrameDeviceInterface,
                nullptr,
                nullptr,
                DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
            if (deviceInfo.get() == INVALID_HANDLE_VALUE)
            {
                throw std::runtime_error("Cannot enumerate the TabletDisplay driver interface");
            }

            for (DWORD index = 0;; ++index)
            {
                SP_DEVICE_INTERFACE_DATA interfaceData{};
                interfaceData.cbSize = sizeof(interfaceData);
                if (!SetupDiEnumDeviceInterfaces(
                        deviceInfo.get(),
                        nullptr,
                        &tabletdisplay::ipc::kFrameDeviceInterface,
                        index,
                        &interfaceData))
                {
                    if (GetLastError() == ERROR_NO_MORE_ITEMS)
                    {
                        break;
                    }
                    throw std::runtime_error("Cannot enumerate the TabletDisplay device interface");
                }

                DWORD requiredBytes{};
                (void)SetupDiGetDeviceInterfaceDetailW(
                    deviceInfo.get(),
                    &interfaceData,
                    nullptr,
                    0,
                    &requiredBytes,
                    nullptr);
                if (requiredBytes < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
                {
                    continue;
                }

                std::vector<std::uint8_t> detailStorage(requiredBytes);
                auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailStorage.data());
                detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
                if (!SetupDiGetDeviceInterfaceDetailW(
                        deviceInfo.get(),
                        &interfaceData,
                        detail,
                        requiredBytes,
                        nullptr,
                        nullptr))
                {
                    continue;
                }

                WinHandle device(CreateFileW(
                    detail->DevicePath,
                    GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL,
                    nullptr));
                if (device.valid())
                {
                    return device;
                }
            }

            throw std::runtime_error(
                "TabletDisplay frame interface is unavailable; reinstall the current driver package");
        }

        [[nodiscard]] bool FindAttachedTabletDisplay(
            std::uint32_t slot,
            std::wstring* deviceName = nullptr,
            DWORD* stateFlags = nullptr) noexcept
        {
            try
            {
                const auto name = FindTabletDisplayDevice(slot);
                if (!name) return false;
                DISPLAY_DEVICEW display{};
                display.cb = sizeof(display);
                for (DWORD index = 0; EnumDisplayDevicesW(nullptr, index, &display, 0); ++index)
                {
                    if (*name == display.DeviceName)
                    {
                        if (deviceName != nullptr) *deviceName = display.DeviceName;
                        if (stateFlags != nullptr) *stateFlags = display.StateFlags;
                        return true;
                    }
                    display = {};
                    display.cb = sizeof(display);
                }
            }
            catch (...)
            {
            }
            return false;
        }

        void PrepareTabletDisplayForDeparture(std::uint32_t slot) noexcept
        {
            std::wstring deviceName;
            DWORD stateFlags{};
            if (!FindAttachedTabletDisplay(slot, &deviceName, &stateFlags) ||
                (stateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0)
            {
                return;
            }

            // Detach the display path first, without changing the saved display layout.
            // This gives Explorer and applications a normal WM_DISPLAYCHANGE before the
            // IddCx monitor object disappears, so taskbar/window ownership can migrate.
            DEVMODEW detachedMode{};
            detachedMode.dmSize = sizeof(detachedMode);
            detachedMode.dmFields = DM_POSITION;
            detachedMode.dmPelsWidth = 0;
            detachedMode.dmPelsHeight = 0;
            if (ChangeDisplaySettingsExW(
                    deviceName.c_str(),
                    &detachedMode,
                    nullptr,
                    0,
                    nullptr) != DISP_CHANGE_SUCCESSFUL)
            {
                return;
            }

            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(2);
            while (FindAttachedTabletDisplay(slot) &&
                   std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        }

        void RestoreTaskbarWindowRegistration() noexcept
        {
            // Windows 11 can keep the windows alive but drop their taskbar buttons
            // after a display path disappears. Re-showing already visible top-level
            // windows without activation makes Explorer register the buttons again.
            (void)EnumWindows(
                [](HWND window, LPARAM) -> BOOL
                {
                    if (IsWindowVisible(window) && GetWindow(window, GW_OWNER) == nullptr)
                        (void)ShowWindowAsync(window, SW_SHOWNA);
                    return TRUE;
                },
                0);
        }
    }

    struct IddFrameCapture::Impl
    {
        Impl() : device(OpenFrameDevice())
        {
        }

        ~Impl()
        {
            if (sharedTransport)
            {
                tabletdisplay::ipc::FrameTransportRequest request{};
                DWORD returnedBytes{};
                (void)DeviceIoControl(
                    device.get(),
                    tabletdisplay::ipc::kSetFrameTransportIoctl,
                    &request,
                    static_cast<DWORD>(sizeof(request)),
                    nullptr,
                    0,
                    &returnedBytes,
                    nullptr);
            }
            if (monitorConnected)
            {
                try
                {
                    SetMonitorConnected(false);
                }
                catch (...)
                {
                    // Closing the WDF file object is the final monitor-lease fallback.
                }
            }
        }

        void SetMonitorConnected(bool connected, MonitorModeRequest modes = {})
        {
            const auto wasConnected = monitorConnected;
            if (!connected && wasConnected)
                PrepareTabletDisplayForDeparture(slot);

            tabletdisplay::ipc::MonitorConnectionRequest request{};
            request.connected = connected ? 1U : 0U;
            if (connected)
            {
                request.nativeWidth = modes.nativeWidth;
                request.nativeHeight = modes.nativeHeight;
                request.preferredDivisor = modes.preferredDivisor;
                request.slot = modes.slot;
                // Frames never exceed the tablet resolution, so the CPU fallback buffer can too.
                const auto frameBytes = modes.nativeWidth != 0 && modes.nativeHeight != 0
                    ? static_cast<std::size_t>(modes.nativeWidth) * modes.nativeHeight *
                        tabletdisplay::ipc::kBytesPerPixel
                    : static_cast<std::size_t>(tabletdisplay::ipc::kMaxFrameBytes);
                response.resize(sizeof(tabletdisplay::ipc::FrameResponseHeader) +
                    std::min<std::size_t>(frameBytes, tabletdisplay::ipc::kMaxFrameBytes));
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            DWORD error{};
            for (;;)
            {
                tabletdisplay::ipc::MonitorConnectionResponse connectionResponse{};
                DWORD returnedBytes{};
                if (DeviceIoControl(
                        device.get(),
                        tabletdisplay::ipc::kSetMonitorConnectionIoctl,
                        &request,
                        static_cast<DWORD>(sizeof(request)),
                        &connectionResponse,
                        static_cast<DWORD>(sizeof(connectionResponse)),
                        &returnedBytes,
                        nullptr))
                {
                    // A driver with a single monitor returns no slot.
                    if (connected)
                    {
                        slot = returnedBytes == sizeof(connectionResponse) &&
                                connectionResponse.slot < tabletdisplay::ipc::kMaxMonitors
                            ? connectionResponse.slot
                            : 0;
                    }
                    break;
                }
                error = GetLastError();
                // A driver with a single monitor rejects the slot field; monitor 1 is its only one.
                if (connected && error == ERROR_INVALID_PARAMETER && request.slot == 1)
                {
                    request.slot = 0;
                    continue;
                }
                if (connected && error == ERROR_BUSY)
                {
                    throw std::runtime_error(request.slot == 0
                        ? "All TabletDisplay monitors are used by other tablets"
                        : "TabletDisplay monitor " + std::to_string(request.slot) +
                            " is used by another tablet");
                }
                if (!connected ||
                    (error != ERROR_NOT_READY && error != ERROR_RETRY) ||
                    std::chrono::steady_clock::now() >= deadline)
                {
                    throw std::runtime_error(
                        "Cannot change the TabletDisplay monitor connection, Windows error " +
                        std::to_string(error) +
                        "; reinstall the current driver package");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            monitorConnected = connected;
            if (!connected && wasConnected)
            {
                // IddCx departure and Explorer's taskbar update are asynchronous.
                // A short second pass covers both the path detach and object removal.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                RestoreTaskbarWindowRegistration();
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                RestoreTaskbarWindowRegistration();
            }
            lastSequence = 0;
            lastSharedSequence = 0;
            lastSharedGeneration = 0;
        }

        [[nodiscard]] bool Capture(CapturedFrame& frame, std::uint32_t timeoutMs)
        {
            if (response.empty())
            {
                response.resize(
                    sizeof(tabletdisplay::ipc::FrameResponseHeader) +
                    tabletdisplay::ipc::kMaxFrameBytes);
            }
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(timeoutMs);
            for (;;)
            {
                tabletdisplay::ipc::FrameRequest request{};
                request.afterSequence = lastSequence;
                DWORD returnedBytes{};
                if (DeviceIoControl(
                        device.get(),
                        tabletdisplay::ipc::kGetLatestFrameIoctl,
                        &request,
                        static_cast<DWORD>(sizeof(request)),
                        response.data(),
                        static_cast<DWORD>(response.size()),
                        &returnedBytes,
                        nullptr))
                {
                    AcceptResponse(returnedBytes);
                    frame = {
                        pixels,
                        header.width,
                        header.height,
                        header.stride
                    };
                    return true;
                }

                const auto error = GetLastError();
                if (error != ERROR_NOT_READY && error != ERROR_NO_MORE_ITEMS &&
                    error != ERROR_RETRY && error != ERROR_BUSY)
                {
                    throw std::runtime_error(
                        "TabletDisplay frame IOCTL failed with Windows error " +
                        std::to_string(error));
                }
                if (timeoutMs == 0 || std::chrono::steady_clock::now() >= deadline)
                {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }

        [[nodiscard]] bool CaptureShared(
            SharedCapturedFrame& frame,
            std::uint32_t timeoutMs,
            bool updateSequence)
        {
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(timeoutMs);
            for (;;)
            {
                tabletdisplay::ipc::SharedFrameRequest request{};
                request.afterSequence = updateSequence ? lastSharedSequence : 0;
                request.afterGeneration = updateSequence ? lastSharedGeneration : 0;
                tabletdisplay::ipc::SharedFrameResponse sharedResponse{};
                DWORD returnedBytes{};
                if (DeviceIoControl(
                        device.get(),
                        tabletdisplay::ipc::kGetSharedFrameIoctl,
                        &request,
                        static_cast<DWORD>(sizeof(request)),
                        &sharedResponse,
                        static_cast<DWORD>(sizeof(sharedResponse)),
                        &returnedBytes,
                        nullptr))
                {
                    if (returnedBytes != sizeof(sharedResponse) ||
                        sharedResponse.magic != tabletdisplay::ipc::kSharedFrameMagic ||
                        sharedResponse.versionMajor != tabletdisplay::ipc::kFrameVersionMajor ||
                        sharedResponse.versionMinor != tabletdisplay::ipc::kFrameVersionMinor ||
                        sharedResponse.structureBytes != sizeof(sharedResponse) ||
                        sharedResponse.pixelFormat != tabletdisplay::ipc::kPixelFormatBgra8 ||
                        sharedResponse.width == 0 ||
                        sharedResponse.width > tabletdisplay::ipc::kMaxFrameWidth ||
                        sharedResponse.height == 0 ||
                        sharedResponse.height > tabletdisplay::ipc::kMaxFrameHeight ||
                        sharedResponse.sequence == 0 ||
                        sharedResponse.generation == 0 ||
                        sharedResponse.resourceNameLength == 0 ||
                        sharedResponse.resourceNameLength >= tabletdisplay::ipc::kSharedResourceNameChars ||
                        sharedResponse.resourceName[sharedResponse.resourceNameLength] != L'\0')
                    {
                        throw std::runtime_error(
                            "TabletDisplay driver returned invalid shared-frame metadata");
                    }
                    LARGE_INTEGER adapterLuid{};
                    adapterLuid.LowPart = sharedResponse.adapterLuidLow;
                    adapterLuid.HighPart = sharedResponse.adapterLuidHigh;
                    frame.resourceName.assign(
                        sharedResponse.resourceName,
                        sharedResponse.resourceNameLength);
                    frame.width = sharedResponse.width;
                    frame.height = sharedResponse.height;
                    frame.adapterLuid = adapterLuid.QuadPart;
                    frame.sequence = sharedResponse.sequence;
                    frame.timestampUs = sharedResponse.timestampUs;
                    frame.generation = sharedResponse.generation;
                    if (updateSequence)
                    {
                        lastSharedSequence = sharedResponse.sequence;
                        lastSharedGeneration = sharedResponse.generation;
                    }
                    return true;
                }

                const auto error = GetLastError();
                if (error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED)
                {
                    return false;
                }
                if (error != ERROR_NOT_READY && error != ERROR_NO_MORE_ITEMS &&
                    error != ERROR_RETRY && error != ERROR_BUSY)
                {
                    throw std::runtime_error(
                        "TabletDisplay shared-frame IOCTL failed with Windows error " +
                        std::to_string(error));
                }
                if (timeoutMs == 0 || std::chrono::steady_clock::now() >= deadline)
                {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }

        void SetSharedTransport(bool enabled, std::uint64_t generation)
        {
            tabletdisplay::ipc::FrameTransportRequest request{};
            request.mode = enabled
                ? tabletdisplay::ipc::kFrameTransportSharedTexture
                : tabletdisplay::ipc::kFrameTransportCpu;
            request.generation = enabled ? generation : 0;
            DWORD returnedBytes{};
            if (!DeviceIoControl(
                    device.get(),
                    tabletdisplay::ipc::kSetFrameTransportIoctl,
                    &request,
                    static_cast<DWORD>(sizeof(request)),
                    nullptr,
                    0,
                    &returnedBytes,
                    nullptr))
            {
                throw std::runtime_error(
                    "Cannot switch TabletDisplay frame transport, Windows error " +
                    std::to_string(GetLastError()));
            }
            sharedTransport = enabled;
            lastSharedSequence = 0;
            lastSharedGeneration = 0;
        }

        void AcceptResponse(DWORD returnedBytes)
        {
            if (returnedBytes < sizeof(tabletdisplay::ipc::FrameResponseHeader))
            {
                throw std::runtime_error("TabletDisplay driver returned a truncated frame header");
            }
            std::memcpy(&header, response.data(), sizeof(header));
            if (header.magic != tabletdisplay::ipc::kFrameMagic ||
                header.versionMajor != tabletdisplay::ipc::kFrameVersionMajor ||
                header.versionMinor != tabletdisplay::ipc::kFrameVersionMinor ||
                header.headerBytes != sizeof(header) ||
                header.pixelFormat != tabletdisplay::ipc::kPixelFormatBgra8)
            {
                throw std::runtime_error("TabletDisplay driver returned an incompatible frame header");
            }
            if (header.width == 0 || header.width > tabletdisplay::ipc::kMaxFrameWidth ||
                header.height == 0 || header.height > tabletdisplay::ipc::kMaxFrameHeight ||
                header.stride != header.width * tabletdisplay::ipc::kBytesPerPixel ||
                header.frameBytes != header.stride * header.height ||
                header.frameBytes > tabletdisplay::ipc::kMaxFrameBytes ||
                returnedBytes != sizeof(header) + header.frameBytes ||
                header.sequence <= lastSequence)
            {
                throw std::runtime_error("TabletDisplay driver returned invalid frame metadata");
            }
            const auto* begin = response.data() + sizeof(header);
            pixels.assign(begin, begin + header.frameBytes);
            lastSequence = header.sequence;
        }

        WinHandle device;
        std::vector<std::uint8_t> response;
        std::vector<std::uint8_t> pixels;
        tabletdisplay::ipc::FrameResponseHeader header{};
        std::uint64_t lastSequence{};
        std::uint64_t lastSharedSequence{};
        std::uint64_t lastSharedGeneration{};
        std::uint32_t slot{};
        bool sharedTransport{};
        bool monitorConnected{};
    };

    IddFrameCapture::IddFrameCapture() : m_impl(std::make_unique<Impl>()) {}
    IddFrameCapture::~IddFrameCapture() = default;

    bool IddFrameCapture::Capture(CapturedFrame& frame, std::uint32_t timeoutMs)
    {
        return m_impl->Capture(frame, timeoutMs);
    }

    bool IddFrameCapture::ProbeShared(SharedCapturedFrame& frame, std::uint32_t timeoutMs)
    {
        return m_impl->CaptureShared(frame, timeoutMs, false);
    }

    bool IddFrameCapture::CaptureShared(SharedCapturedFrame& frame, std::uint32_t timeoutMs)
    {
        return m_impl->CaptureShared(frame, timeoutMs, true);
    }

    void IddFrameCapture::SetMonitorConnected(bool connected, MonitorModeRequest modes)
    {
        m_impl->SetMonitorConnected(connected, modes);
    }

    void IddFrameCapture::SetSharedTransport(bool enabled, std::uint64_t generation)
    {
        m_impl->SetSharedTransport(enabled, generation);
    }

    bool IddFrameCapture::UsesSharedTransport() const noexcept
    {
        return m_impl->sharedTransport;
    }

    std::uint32_t IddFrameCapture::MonitorSlot() const noexcept
    {
        return m_impl->slot;
    }
}
