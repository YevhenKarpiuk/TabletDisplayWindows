#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>

#include "IddFrameCapture.h"
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
    }

    struct IddFrameCapture::Impl
    {
        Impl() : device(OpenFrameDevice())
        {
            response.resize(
                sizeof(tabletdisplay::ipc::FrameResponseHeader) +
                tabletdisplay::ipc::kMaxFrameBytes);
        }

        [[nodiscard]] bool Capture(CapturedFrame& frame, std::uint32_t timeoutMs)
        {
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
    };

    IddFrameCapture::IddFrameCapture() : m_impl(std::make_unique<Impl>()) {}
    IddFrameCapture::~IddFrameCapture() = default;

    bool IddFrameCapture::Capture(CapturedFrame& frame, std::uint32_t timeoutMs)
    {
        return m_impl->Capture(frame, timeoutMs);
    }
}
