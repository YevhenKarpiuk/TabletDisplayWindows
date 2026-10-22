#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "DesktopCapture.h"
#include "display/DisplayModes.h"
#include "IddFrameCapture.h"
#include "MediaFoundationH264Encoder.h"
#include "protocol/Protocol.h"
#include "video/SyntheticH264.h"
#include "video/VideoProcessing.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    using namespace tabletdisplay::protocol;

    constexpr std::uint64_t kCapabilities = 0xFF;
    constexpr std::uint64_t kRequiredCapabilities = 0xE1;
    constexpr std::uint64_t kAvcProfiles = 0x0E;
    constexpr std::uint16_t kHeartbeatIntervalMs = 1'000;
    constexpr std::uint16_t kIdleTimeoutMs = 5'000;
    constexpr std::wstring_view kTabletDisplayDeviceString = L"TabletDisplay Virtual Display";

    // ERROR codes and severities from doc/PROTOCOL.md section 5.9.
    constexpr std::uint16_t kErrorMalformedHeader = 1;
    constexpr std::uint16_t kErrorUnsupportedVersion = 2;
    constexpr std::uint16_t kErrorUnknownCriticalType = 3;
    constexpr std::uint16_t kErrorPayloadTooLarge = 4;
    constexpr std::uint16_t kErrorInvalidPayload = 5;
    constexpr std::uint16_t kErrorUnexpectedMessage = 6;
    constexpr std::uint16_t kErrorInternal = 8;
    constexpr std::uint16_t kErrorTimeout = 9;
    constexpr std::uint16_t kErrorSequenceGap = 10;
    constexpr std::uint8_t kSeverityFatal = 3;

    enum class RunMode
    {
        Connect,
        Synthetic,
        EncoderDemo,
        Desktop,
        VirtualDisplay
    };

    struct ProcessResult
    {
        DWORD exitCode{};
        std::string output;
    };

    struct Device
    {
        std::string serial;
        std::string state;
        std::string description;
    };

    // The peer violated the protocol; reported to Android with the given ERROR code.
    class ProtocolFailure final : public std::runtime_error
    {
    public:
        ProtocolFailure(std::uint16_t code, const std::string& message)
            : std::runtime_error(message), m_code(code)
        {
        }

        [[nodiscard]] std::uint16_t code() const noexcept { return m_code; }

    private:
        std::uint16_t m_code;
    };

    // The TCP connection is gone, so there is nobody to report an ERROR to.
    class TransportFailure final : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    class PeerReportedFailure final : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    class NoDeviceFailure final : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    HANDLE g_stopEvent{};
    std::atomic<std::uint64_t> g_lastProgressMs{ GetTickCount64() };

    [[nodiscard]] bool StopRequested() noexcept
    {
        return g_stopEvent != nullptr && WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0;
    }

    // Returns true when the tray asked the Host to stop during the wait.
    bool WaitUnlessStopped(std::chrono::milliseconds duration)
    {
        if (g_stopEvent == nullptr)
        {
            std::this_thread::sleep_for(duration);
            return false;
        }
        return WaitForSingleObject(g_stopEvent, static_cast<DWORD>(duration.count())) == WAIT_OBJECT_0;
    }

    void ReportProgress() noexcept
    {
        g_lastProgressMs = GetTickCount64();
    }

    // A hung adb, driver IOCTL or Media Foundation call cannot be cancelled from inside the
    // process. The tray restarts a Host that exits, so a stalled Host terminates itself.
    class ProgressWatchdog
    {
    public:
        explicit ProgressWatchdog(std::chrono::seconds limit)
            : m_thread([limit](std::stop_token stopToken)
            {
                std::mutex mutex;
                std::condition_variable_any wake;
                ReportProgress();
                std::unique_lock lock(mutex);
                while (!wake.wait_for(lock, stopToken, std::chrono::seconds(1), [] { return false; }))
                {
                    if (stopToken.stop_requested()) return;
                    const auto stalledMs = GetTickCount64() - g_lastProgressMs.load();
                    if (stalledMs > static_cast<std::uint64_t>(limit.count()) * 1'000U)
                    {
                        std::cerr << "TabletDisplayHost: no progress for " << stalledMs / 1'000U
                            << " s; terminating so the tray can restart the Host.\n";
                        std::cerr.flush();
                        TerminateProcess(GetCurrentProcess(), 3);
                    }
                }
            })
        {
        }

    private:
        std::jthread m_thread;
    };

    class SocketHandle
    {
    public:
        explicit SocketHandle(SOCKET value = INVALID_SOCKET) noexcept : m_value(value) {}
        ~SocketHandle() { reset(); }
        SocketHandle(const SocketHandle&) = delete;
        SocketHandle& operator=(const SocketHandle&) = delete;
        SocketHandle(SocketHandle&& other) noexcept : m_value(other.release()) {}
        SocketHandle& operator=(SocketHandle&& other) noexcept
        {
            if (this != &other)
            {
                reset(other.release());
            }
            return *this;
        }
        [[nodiscard]] SOCKET get() const noexcept { return m_value; }
        [[nodiscard]] SOCKET release() noexcept
        {
            const auto value = m_value;
            m_value = INVALID_SOCKET;
            return value;
        }
        void reset(SOCKET value = INVALID_SOCKET) noexcept
        {
            if (m_value != INVALID_SOCKET)
            {
                closesocket(m_value);
            }
            m_value = value;
        }

    private:
        SOCKET m_value;
    };

    class WinsockSession
    {
    public:
        WinsockSession()
        {
            WSADATA data{};
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            {
                throw std::runtime_error("WSAStartup failed");
            }
        }
        ~WinsockSession() { WSACleanup(); }
        WinsockSession(const WinsockSession&) = delete;
        WinsockSession& operator=(const WinsockSession&) = delete;
    };

    template<typename T>
    void AppendLe(std::vector<std::uint8_t>& output, T value)
    {
        static_assert(std::is_unsigned_v<T>);
        for (std::size_t index = 0; index < sizeof(T); ++index)
        {
            output.push_back(static_cast<std::uint8_t>(value & static_cast<T>(0xFF)));
            value >>= 8U;
        }
    }

    template<typename T>
    [[nodiscard]] T ReadLe(std::span<const std::uint8_t> bytes, std::size_t offset)
    {
        static_assert(std::is_unsigned_v<T>);
        if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
        {
            throw ProtocolFailure(kErrorInvalidPayload, "Truncated protocol payload");
        }
        T value{};
        for (std::size_t index = 0; index < sizeof(T); ++index)
        {
            value |= static_cast<T>(bytes[offset + index]) << (index * 8U);
        }
        return value;
    }

    [[nodiscard]] std::uint64_t NowUs()
    {
        const auto value = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return static_cast<std::uint64_t>(std::max<std::int64_t>(1, value));
    }

    [[nodiscard]] std::uint64_t RandomNonce()
    {
        std::random_device source;
        const auto high = static_cast<std::uint64_t>(source()) << 32U;
        const auto low = static_cast<std::uint64_t>(source());
        return std::max<std::uint64_t>(1, (high | low) &
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
    }

    [[nodiscard]] std::wstring Widen(std::string_view value)
    {
        return std::wstring(value.begin(), value.end());
    }

    [[nodiscard]] std::string Narrow(std::wstring_view value)
    {
        if (value.empty()) return {};
        const auto size = WideCharToMultiByte(
            CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (size <= 0) throw std::runtime_error("UTF-8 conversion failed");
        std::string output(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(
            CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
            output.data(), size, nullptr, nullptr);
        return output;
    }

    struct TabletDisplayMode
    {
        std::wstring deviceName;
        DEVMODEW mode{};
    };

    [[nodiscard]] std::optional<TabletDisplayMode> FindTabletDisplayMode()
    {
        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        for (DWORD index = 0; EnumDisplayDevicesW(nullptr, index, &device, 0); ++index)
        {
            if (std::wstring_view(device.DeviceString) == kTabletDisplayDeviceString &&
                (device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0)
            {
                TabletDisplayMode result{ device.DeviceName, {} };
                result.mode.dmSize = sizeof(result.mode);
                if (!EnumDisplaySettingsExW(device.DeviceName, ENUM_CURRENT_SETTINGS, &result.mode, 0))
                    throw std::runtime_error("Cannot query the TabletDisplay mode");
                return result;
            }
            device = {};
            device.cb = sizeof(device);
        }
        return std::nullopt;
    }

    [[nodiscard]] TabletDisplayMode RequireTabletDisplayMode()
    {
        auto display = FindTabletDisplayMode();
        if (!display) throw std::runtime_error("TabletDisplay virtual display was not found by Windows");
        return std::move(*display);
    }

    void ConfigureTabletDisplayMode(
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t frequency)
    {
        auto display = RequireTabletDisplayMode();
        auto& mode = display.mode;
        if (mode.dmPelsWidth == width && mode.dmPelsHeight == height &&
            mode.dmDisplayFrequency == frequency)
            return;

        mode.dmPelsWidth = width;
        mode.dmPelsHeight = height;
        mode.dmDisplayFrequency = frequency;
        mode.dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
        const auto testResult = ChangeDisplaySettingsExW(
            display.deviceName.c_str(), &mode, nullptr, CDS_TEST, nullptr);
        if (testResult != DISP_CHANGE_SUCCESSFUL)
            throw std::runtime_error("Windows rejected the requested TabletDisplay mode");
        const auto applyResult = ChangeDisplaySettingsExW(
            display.deviceName.c_str(), &mode, nullptr, CDS_UPDATEREGISTRY, nullptr);
        if (applyResult != DISP_CHANGE_SUCCESSFUL)
            throw std::runtime_error("Windows could not apply the requested TabletDisplay mode");
        std::cout << "Windows display mode: " << width << 'x' << height
            << " @ " << frequency << " Hz\n";
    }

    [[nodiscard]] DWORD TabletDisplayOrientation()
    {
        const auto display = RequireTabletDisplayMode();
        if (display.mode.dmDisplayOrientation > DMDO_270)
            throw std::runtime_error("Windows reported an invalid TabletDisplay orientation");
        return display.mode.dmDisplayOrientation;
    }

    [[nodiscard]] DWORD WaitForTabletDisplayOrientation()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline)
        {
            try
            {
                return TabletDisplayOrientation();
            }
            catch (const std::runtime_error&)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        throw std::runtime_error(
            "TabletDisplay did not appear in the Windows desktop after the monitor connection request");
    }

    struct DisplayBounds
    {
        LONG left{};
        LONG top{};
        DWORD width{};
        DWORD height{};
    };

    [[nodiscard]] DisplayBounds FindTabletDisplayBounds()
    {
        const auto display = RequireTabletDisplayMode();
        return {
            display.mode.dmPosition.x,
            display.mode.dmPosition.y,
            display.mode.dmPelsWidth,
            display.mode.dmPelsHeight
        };
    }

    void ReleaseTabletMouseButton(std::atomic_bool& primaryButtonDown) noexcept
    {
        if (!primaryButtonDown.exchange(false)) return;
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
        (void)SendInput(1, &input, sizeof(input));
    }

    class TabletMouseButtonGuard
    {
    public:
        explicit TabletMouseButtonGuard(std::atomic_bool& primaryButtonDown) noexcept
            : primaryButtonDown_(primaryButtonDown)
        {
        }

        TabletMouseButtonGuard(const TabletMouseButtonGuard&) = delete;
        TabletMouseButtonGuard& operator=(const TabletMouseButtonGuard&) = delete;

        ~TabletMouseButtonGuard()
        {
            ReleaseTabletMouseButton(primaryButtonDown_);
        }

    private:
        std::atomic_bool& primaryButtonDown_;
    };

    void InjectTouch(
        std::span<const std::uint8_t> payload,
        DisplayBounds& display,
        std::atomic_bool& primaryButtonDown)
    {
        if (ReadLe<std::uint32_t>(payload, 0) != 1 || ReadLe<std::uint32_t>(payload, 4) != 0)
            throw ProtocolFailure(kErrorInvalidPayload, "TOUCH targets an unexpected stream or pointer");

        const auto action = payload[8];
        if (action < 1 || action > 4) throw ProtocolFailure(kErrorInvalidPayload, "TOUCH has an unknown action");
        if (action == 1)
        {
            // The user may have rearranged monitors since the stream started.
            try
            {
                display = FindTabletDisplayBounds();
            }
            catch (const std::runtime_error&)
            {
            }
        }

        const auto normalizedX = ReadLe<std::uint16_t>(payload, 10);
        const auto normalizedY = ReadLe<std::uint16_t>(payload, 12);
        const auto virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const auto virtualTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const auto virtualWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const auto virtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (display.width < 2 || display.height < 2 || virtualWidth < 2 || virtualHeight < 2)
            throw std::runtime_error("Windows reported invalid virtual desktop bounds");

        const auto pixelX = static_cast<std::int64_t>(display.left) +
            (static_cast<std::int64_t>(normalizedX) * (display.width - 1U) + 32'767) / 65'535;
        const auto pixelY = static_cast<std::int64_t>(display.top) +
            (static_cast<std::int64_t>(normalizedY) * (display.height - 1U) + 32'767) / 65'535;
        const auto absoluteX = (pixelX - virtualLeft) * 65'535 / (virtualWidth - 1);
        const auto absoluteY = (pixelY - virtualTop) * 65'535 / (virtualHeight - 1);

        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dx = static_cast<LONG>(std::clamp<std::int64_t>(absoluteX, 0, 65'535));
        input.mi.dy = static_cast<LONG>(std::clamp<std::int64_t>(absoluteY, 0, 65'535));
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        if (action == 1) input.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
        else if (action == 3 || action == 4) input.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
        // UIPI blocks input to elevated windows and the secure desktop (UAC, lock screen).
        // Losing such an event must not drop the whole video session.
        if (SendInput(1, &input, sizeof(input)) != 1) return;
        if (action == 1) primaryButtonDown = true;
        else if (action == 3 || action == 4) primaryButtonDown = false;
    }

    [[nodiscard]] std::wstring EnvironmentValue(const wchar_t* name)
    {
        const auto size = GetEnvironmentVariableW(name, nullptr, 0);
        if (size == 0) return {};
        std::wstring value(size, L'\0');
        const auto written = GetEnvironmentVariableW(name, value.data(), size);
        value.resize(written);
        return value;
    }

    [[nodiscard]] std::filesystem::path FindAdb()
    {
        std::vector<std::filesystem::path> candidates;
        const auto explicitPath = EnvironmentValue(L"TABLETDISPLAY_ADB");
        if (!explicitPath.empty()) candidates.emplace_back(explicitPath);
        const auto androidHome = EnvironmentValue(L"ANDROID_HOME");
        if (!androidHome.empty())
        {
            candidates.emplace_back(std::filesystem::path(androidHome) / L"platform-tools" / L"adb.exe");
        }
        candidates.emplace_back(L"C:\\tools\\android\\sdk\\platform-tools\\adb.exe");

        wchar_t found[MAX_PATH]{};
        if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, found, nullptr) != 0)
        {
            candidates.emplace_back(found);
        }
        const auto match = std::ranges::find_if(candidates, [](const auto& candidate)
        {
            return std::filesystem::is_regular_file(candidate);
        });
        if (match == candidates.end())
        {
            throw std::runtime_error("adb.exe was not found");
        }
        return *match;
    }

    [[nodiscard]] ProcessResult RunProcess(
        const std::filesystem::path& executable,
        const std::wstring& arguments,
        std::chrono::milliseconds timeout = std::chrono::seconds(20))
    {
        SECURITY_ATTRIBUTES security{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        HANDLE readPipe{};
        HANDLE writePipe{};
        if (!CreatePipe(&readPipe, &writePipe, &security, 0) ||
            !SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0))
        {
            throw std::runtime_error("Cannot create adb output pipe");
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = writePipe;
        startup.hStdError = writePipe;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION process{};
        auto command = L"\"" + executable.wstring() + L"\" " + arguments;
        const auto created = CreateProcessW(
            executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
        CloseHandle(writePipe);
        if (!created)
        {
            CloseHandle(readPipe);
            throw std::runtime_error("Cannot start adb.exe");
        }

        // The adb server started by the first command inherits the pipe and keeps it open,
        // so the output is read only while the adb client process itself is running.
        std::string output;
        std::array<char, 4096> buffer{};
        const auto drain = [&]
        {
            DWORD available{};
            while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available != 0)
            {
                DWORD count{};
                const auto request = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
                if (!ReadFile(readPipe, buffer.data(), request, &count, nullptr) || count == 0) return;
                output.append(buffer.data(), count);
            }
        };
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        bool finished{};
        while (!finished)
        {
            finished = WaitForSingleObject(process.hProcess, 50) == WAIT_OBJECT_0;
            drain();
            if (!finished && std::chrono::steady_clock::now() >= deadline) break;
        }
        if (!finished)
        {
            (void)TerminateProcess(process.hProcess, 1);
            (void)WaitForSingleObject(process.hProcess, 1'000);
        }
        DWORD exitCode{};
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(readPipe);
        if (!finished) throw std::runtime_error("adb.exe did not finish in time");
        ReportProgress();
        return { exitCode, std::move(output) };
    }

    [[nodiscard]] std::vector<Device> ParseDevices(std::string_view output)
    {
        std::vector<Device> devices;
        std::istringstream lines{ std::string(output) };
        std::string line;
        while (std::getline(lines, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line.starts_with("List of devices") || line.front() == '*') continue;
            std::istringstream fields(line);
            Device device;
            fields >> device.serial >> device.state;
            std::getline(fields, device.description);
            if (!device.serial.empty() && !device.state.empty()) devices.push_back(std::move(device));
        }
        return devices;
    }

    [[nodiscard]] std::string SelectDevice(
        const std::vector<Device>& devices,
        const std::string& requested)
    {
        if (!requested.empty())
        {
            const auto match = std::ranges::find_if(devices, [&](const Device& device)
            {
                return device.serial == requested;
            });
            if (match == devices.end()) throw NoDeviceFailure("Requested ADB device was not found");
            if (match->state != "device") throw NoDeviceFailure("Requested ADB device is " + match->state);
            return match->serial;
        }
        std::vector<std::string> ready;
        for (const auto& device : devices)
        {
            if (device.state == "device") ready.push_back(device.serial);
        }
        if (ready.empty()) throw NoDeviceFailure("No authorized ADB device is connected");
        if (ready.size() != 1) throw std::runtime_error("Multiple ADB devices are ready; use --serial");
        return ready.front();
    }

    void RequireAdbSuccess(const ProcessResult& result, std::string_view operation)
    {
        if (result.exitCode != 0)
        {
            throw std::runtime_error(std::string(operation) + " failed: " + result.output);
        }
    }

    [[nodiscard]] bool ForwardAlreadyExists(const std::string& output, const std::string& serial)
    {
        std::istringstream lines(output);
        std::string line;
        const auto expected = serial + " tcp:27183 tcp:27183";
        while (std::getline(lines, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line == expected) return true;
        }
        return false;
    }

    [[nodiscard]] SocketHandle ConnectLoopback()
    {
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_port = htons(kPort);
        if (inet_pton(AF_INET, kLoopbackAddress.data(), &endpoint.sin_addr) != 1)
        {
            throw std::runtime_error("Invalid loopback address");
        }
        for (int attempt = 1; attempt <= 20; ++attempt)
        {
            SocketHandle socket(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            if (socket.get() == INVALID_SOCKET) throw std::runtime_error("socket() failed");
            const DWORD timeout = 5'000;
            setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO,
                reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            setsockopt(socket.get(), SOL_SOCKET, SO_SNDTIMEO,
                reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            const BOOL noDelay = TRUE;
            setsockopt(socket.get(), IPPROTO_TCP, TCP_NODELAY,
                reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
            if (connect(socket.get(), reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) == 0)
            {
                return socket;
            }
            if (WaitUnlessStopped(std::chrono::milliseconds(250))) break;
        }
        throw std::runtime_error("Cannot connect to Android on 127.0.0.1:27183");
    }

    void SendAll(SOCKET socket, std::span<const std::uint8_t> bytes)
    {
        std::size_t offset{};
        while (offset < bytes.size())
        {
            const auto remaining = std::min<std::size_t>(
                bytes.size() - offset,
                static_cast<std::size_t>(std::numeric_limits<int>::max()));
            const auto sent = send(
                socket,
                reinterpret_cast<const char*>(bytes.data() + offset),
                static_cast<int>(remaining),
                0);
            if (sent == SOCKET_ERROR || sent == 0) throw TransportFailure("TCP send failed");
            offset += static_cast<std::size_t>(sent);
        }
    }

    [[nodiscard]] Message MakeMessage(
        MessageType type,
        std::uint64_t sequence,
        std::vector<std::uint8_t> payload,
        std::uint16_t flags = Critical)
    {
        return {
            { static_cast<std::uint16_t>(type), flags,
              static_cast<std::uint32_t>(payload.size()), sequence, NowUs() },
            std::move(payload)
        };
    }

    [[nodiscard]] Message MakeHello(std::uint64_t nonce)
    {
        std::vector<std::uint8_t> payload;
        payload.reserve(32);
        payload.insert(payload.end(), { 1, 0, 1, 1, 0, 0 });
        AppendLe(payload, static_cast<std::uint16_t>(0));
        AppendLe(payload, nonce);
        AppendLe(payload, kCapabilities);
        AppendLe(payload, kMaxPayloadSize);
        AppendLe(payload, kHeartbeatIntervalMs);
        AppendLe(payload, kIdleTimeoutMs);
        return MakeMessage(MessageType::Hello, 1, std::move(payload));
    }

    [[nodiscard]] Message MakePing(std::uint64_t sequence, std::uint64_t token)
    {
        std::vector<std::uint8_t> payload;
        payload.reserve(16);
        AppendLe(payload, token);
        AppendLe(payload, NowUs());
        return MakeMessage(MessageType::Ping, sequence, std::move(payload));
    }

    [[nodiscard]] Message MakePong(std::uint64_t sequence, std::span<const std::uint8_t> ping)
    {
        std::vector<std::uint8_t> payload;
        payload.reserve(24);
        AppendLe(payload, ReadLe<std::uint64_t>(ping, 0));
        AppendLe(payload, ReadLe<std::uint64_t>(ping, 8));
        AppendLe(payload, NowUs());
        return MakeMessage(MessageType::Pong, sequence, std::move(payload));
    }

    [[nodiscard]] Message MakeError(
        std::uint64_t sequence,
        std::uint16_t code,
        std::string_view text)
    {
        // Diagnostic text is plain ASCII, which is always valid UTF-8.
        std::string ascii;
        for (const auto character : text.substr(0, kMaxErrorTextSize))
        {
            ascii.push_back(character >= 0x20 && character < 0x7F ? character : '?');
        }
        std::vector<std::uint8_t> payload;
        payload.reserve(20U + ascii.size());
        AppendLe(payload, code);
        payload.push_back(kSeverityFatal);
        payload.push_back(0);
        AppendLe(payload, static_cast<std::uint16_t>(0)); // offending message type.
        AppendLe(payload, static_cast<std::uint16_t>(0));
        AppendLe(payload, static_cast<std::uint64_t>(0)); // offending sequence.
        AppendLe(payload, static_cast<std::uint16_t>(ascii.size()));
        AppendLe(payload, static_cast<std::uint16_t>(0));
        payload.insert(payload.end(), ascii.begin(), ascii.end());
        return MakeMessage(MessageType::Error, sequence, std::move(payload));
    }

    [[nodiscard]] std::string ErrorText(std::span<const std::uint8_t> payload)
    {
        const auto length = ReadLe<std::uint16_t>(payload, 16);
        if (payload.size() < 20U + length) return {};
        return std::string(payload.begin() + 20, payload.begin() + 20 + length);
    }

    [[nodiscard]] Message MakeVideoConfig(
        std::uint64_t sequence,
        std::uint16_t width,
        std::uint16_t height,
        std::uint16_t fps,
        std::uint8_t profileIdc,
        std::uint8_t levelIdc,
        std::uint32_t bitrate,
        std::span<const std::uint8_t> codecSpecificData)
    {
        std::vector<std::uint8_t> payload;
        payload.reserve(28U + codecSpecificData.size());
        AppendLe(payload, static_cast<std::uint32_t>(1)); // stream_id.
        AppendLe(payload, static_cast<std::uint32_t>(1)); // config_id.
        AppendLe(payload, width);
        AppendLe(payload, height);
        AppendLe(payload, fps);
        AppendLe(payload, static_cast<std::uint16_t>(1));
        payload.push_back(profileIdc);
        payload.push_back(levelIdc);
        AppendLe(payload, static_cast<std::uint16_t>(1)); // ANNEX_B_CSD.
        AppendLe(payload, bitrate);
        AppendLe(payload, static_cast<std::uint32_t>(codecSpecificData.size()));
        payload.insert(payload.end(), codecSpecificData.begin(), codecSpecificData.end());
        return MakeMessage(MessageType::VideoConfig, sequence, std::move(payload));
    }

    [[nodiscard]] Message MakeVideoFrame(
        std::uint64_t sequence,
        std::uint64_t frameId,
        std::uint64_t presentationTimeUs,
        std::span<const std::uint8_t> accessUnit,
        bool keyFrame,
        bool discontinuity,
        bool endOfStream = false)
    {
        std::vector<std::uint8_t> payload;
        payload.reserve(32U + accessUnit.size());
        AppendLe(payload, static_cast<std::uint32_t>(1)); // stream_id.
        AppendLe(payload, static_cast<std::uint32_t>(1)); // config_id.
        AppendLe(payload, frameId);
        AppendLe(payload, presentationTimeUs);
        AppendLe(payload, static_cast<std::uint32_t>(accessUnit.size()));
        AppendLe(payload, static_cast<std::uint32_t>(0));
        payload.insert(payload.end(), accessUnit.begin(), accessUnit.end());
        auto flags = static_cast<std::uint16_t>(Critical);
        if (keyFrame) flags = static_cast<std::uint16_t>(flags | KeyFrame);
        if (discontinuity) flags = static_cast<std::uint16_t>(flags | Discontinuity);
        if (endOfStream) flags = static_cast<std::uint16_t>(flags | EndOfStream);
        return MakeMessage(MessageType::VideoFrame, sequence, std::move(payload), flags);
    }

    void SendMessage(SOCKET socket, const Message& message)
    {
        SendAll(socket, EncodeMessage(message.header, message.payload));
    }

    // Serializes Windows-to-Android messages from the streaming and receiving threads and
    // assigns the direction's sequence numbers.
    class ProtocolSender
    {
    public:
        ProtocolSender(SOCKET socket, std::uint64_t nextSequence) noexcept
            : m_socket(socket), m_nextSequence(nextSequence)
        {
        }

        template<typename MakeMessageForSequence>
        void Send(MakeMessageForSequence&& make)
        {
            std::lock_guard lock(m_mutex);
            SendMessage(m_socket, make(m_nextSequence++));
        }

    private:
        SOCKET m_socket;
        std::mutex m_mutex;
        std::uint64_t m_nextSequence;
    };

    [[nodiscard]] std::uint16_t ErrorCodeFor(const std::exception& exception) noexcept
    {
        if (const auto* failure = dynamic_cast<const ProtocolFailure*>(&exception)) return failure->code();
        if (const auto* failure = dynamic_cast<const ProtocolException*>(&exception))
        {
            switch (failure->kind())
            {
            case ProtocolErrorKind::BadMagic:
            case ProtocolErrorKind::InvalidHeader:
                return 0; // The header could not be parsed safely: close without ERROR.
            case ProtocolErrorKind::InvalidFlags: return kErrorMalformedHeader;
            case ProtocolErrorKind::UnsupportedVersion: return kErrorUnsupportedVersion;
            case ProtocolErrorKind::UnknownCriticalType: return kErrorUnknownCriticalType;
            case ProtocolErrorKind::PayloadTooLarge: return kErrorPayloadTooLarge;
            case ProtocolErrorKind::InvalidPayload: return kErrorInvalidPayload;
            case ProtocolErrorKind::SequenceGap: return kErrorSequenceGap;
            }
        }
        if (dynamic_cast<const TransportFailure*>(&exception) != nullptr ||
            dynamic_cast<const PeerReportedFailure*>(&exception) != nullptr)
            return 0;
        return kErrorInternal;
    }

    // Best effort: tells Android why the session ends, as protocol section 5.9 requires.
    void SendFatalError(ProtocolSender& sender, std::exception_ptr failure) noexcept
    {
        try
        {
            std::rethrow_exception(failure);
        }
        catch (const std::exception& exception)
        {
            const auto code = ErrorCodeFor(exception);
            if (code == 0) return;
            try
            {
                sender.Send([&](std::uint64_t sequence)
                {
                    return MakeError(sequence, code, exception.what());
                });
            }
            catch (...)
            {
            }
        }
        catch (...)
        {
        }
    }

    // DISPLAY_INFO: the panel resolution in the current tablet orientation and the
    // largest AVC frame the tablet decoder accepts.
    struct AndroidDisplay
    {
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint32_t maxAvcWidth{};
        std::uint32_t maxAvcHeight{};
    };

    struct HandshakeResult
    {
        std::uint64_t nextSequence{};
        IncrementalDecoder decoder;
        AndroidDisplay display;
    };

    [[nodiscard]] HandshakeResult PerformHandshake(SOCKET socket)
    {
        const auto nonce = RandomNonce();
        SendMessage(socket, MakeHello(nonce));
        IncrementalDecoder decoder;
        std::array<std::uint8_t, 16 * 1024> buffer{};
        bool ackReceived{};
        bool displayReceived{};
        bool pingSent{};
        AndroidDisplay display;
        const auto pingToken = RandomNonce();

        while (true)
        {
            const auto count = recv(socket, reinterpret_cast<char*>(buffer.data()),
                static_cast<int>(buffer.size()), 0);
            if (count == 0) throw TransportFailure("Android closed the handshake connection");
            if (count == SOCKET_ERROR) throw TransportFailure("TCP receive timed out or failed");
            for (const auto& message : decoder.Feed(
                std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(count))))
            {
                const auto type = static_cast<MessageType>(message.header.messageType);
                if (type == MessageType::HelloAck && !ackReceived)
                {
                    const auto status = ReadLe<std::uint16_t>(message.payload, 0);
                    if (status != 0) throw PeerReportedFailure("Android rejected HELLO, status " + std::to_string(status));
                    if (ReadLe<std::uint64_t>(message.payload, 8) != nonce)
                        throw ProtocolFailure(kErrorInvalidPayload, "HELLO_ACK nonce mismatch");
                    const auto capabilities = ReadLe<std::uint64_t>(message.payload, 24);
                    if ((capabilities & kRequiredCapabilities) != kRequiredCapabilities ||
                        (capabilities & kAvcProfiles) == 0)
                        throw ProtocolFailure(kErrorInvalidPayload, "HELLO_ACK is missing an MVP capability");
                    ackReceived = true;
                }
                else if (type == MessageType::DisplayInfo && ackReceived && !displayReceived)
                {
                    display = {
                        ReadLe<std::uint32_t>(message.payload, 0),
                        ReadLe<std::uint32_t>(message.payload, 4),
                        ReadLe<std::uint32_t>(message.payload, 16),
                        ReadLe<std::uint32_t>(message.payload, 20)
                    };
                    std::cout
                        << "Android display: " << display.width << 'x' << display.height
                        << ", decoder up to " << display.maxAvcWidth << 'x' << display.maxAvcHeight
                        << " @ " << ReadLe<std::uint32_t>(message.payload, 24) << " FPS\n";
                    displayReceived = true;
                }
                else if (type == MessageType::Pong && pingSent)
                {
                    if (ReadLe<std::uint64_t>(message.payload, 0) != pingToken)
                        throw ProtocolFailure(kErrorInvalidPayload, "PONG token mismatch");
                    std::cout << "Protocol handshake and PING/PONG succeeded.\n";
                    ReportProgress();
                    return { 3, std::move(decoder), display };
                }
                else if (type == MessageType::Error)
                {
                    throw PeerReportedFailure("Android reported an error: " + ErrorText(message.payload));
                }
                else
                {
                    throw ProtocolFailure(
                        kErrorUnexpectedMessage,
                        "Unexpected Android-to-Windows message during handshake");
                }
            }
            if (ackReceived && displayReceived && !pingSent)
            {
                SendMessage(socket, MakePing(2, pingToken));
                pingSent = true;
            }
        }
    }

    // Sends VIDEO_CONFIG before the first access unit and numbers VIDEO_FRAME messages.
    class VideoStreamWriter
    {
    public:
        VideoStreamWriter(
            ProtocolSender& sender,
            const tabletdisplay::host::MediaFoundationH264Encoder& encoder,
            std::uint16_t width,
            std::uint16_t height,
            std::uint16_t fps,
            std::uint32_t bitrate) noexcept
            : m_sender(sender),
              m_encoder(encoder),
              m_width(width),
              m_height(height),
              m_fps(fps),
              m_bitrate(bitrate)
        {
        }

        void Send(const tabletdisplay::host::EncodedAccessUnit& encoded)
        {
            if (!m_configSent)
            {
                const auto* config = m_encoder.CodecConfig();
                if (config == nullptr) throw std::runtime_error("H.264 encoder produced no SPS/PPS");
                m_sender.Send([&](std::uint64_t sequence)
                {
                    return MakeVideoConfig(
                        sequence, m_width, m_height, m_fps,
                        config->profileIdc == 0 ? 66 : config->profileIdc,
                        config->levelIdc == 0 ? 40 : config->levelIdc,
                        m_bitrate,
                        config->annexB);
                });
                m_configSent = true;
            }
            if (m_nextFrameId == 1 && !encoded.keyFrame)
                throw std::runtime_error("H.264 encoder did not produce an initial IDR");
            // Protocol section 5.5: the first IDR after VIDEO_CONFIG or REQUEST_KEYFRAME
            // carries DISCONTINUITY so Android flushes its decoder.
            const bool discontinuity = encoded.keyFrame && (m_nextFrameId == 1 || m_discontinuityPending);
            if (encoded.keyFrame) m_discontinuityPending = false;
            m_sender.Send([&](std::uint64_t sequence)
            {
                return MakeVideoFrame(
                    sequence,
                    m_nextFrameId,
                    encoded.presentationTimeUs,
                    encoded.bytes,
                    encoded.keyFrame,
                    discontinuity);
            });
            ++m_nextFrameId;
            m_bytesSent += encoded.bytes.size();
            ReportProgress();
        }

        void RequestDiscontinuity() noexcept
        {
            m_discontinuityPending = true;
        }

        void SendEndOfStream(std::uint64_t presentationTimeUs)
        {
            if (!m_configSent) throw std::runtime_error("H.264 encoder produced no frames");
            m_sender.Send([&](std::uint64_t sequence)
            {
                return MakeVideoFrame(sequence, m_nextFrameId, presentationTimeUs, {}, false, false, true);
            });
        }

        [[nodiscard]] std::uint64_t FramesSent() const noexcept { return m_nextFrameId - 1; }
        [[nodiscard]] std::uint64_t BytesSent() const noexcept { return m_bytesSent; }

    private:
        ProtocolSender& m_sender;
        const tabletdisplay::host::MediaFoundationH264Encoder& m_encoder;
        std::uint16_t m_width;
        std::uint16_t m_height;
        std::uint16_t m_fps;
        std::uint32_t m_bitrate;
        std::uint64_t m_nextFrameId{ 1 };
        std::uint64_t m_bytesSent{};
        bool m_configSent{};
        bool m_discontinuityPending{};
    };

    void RunSyntheticDemo(ProtocolSender& sender)
    {
        const auto stream = tabletdisplay::video::BuildSyntheticColorBars();
        std::cout
            << "Synthetic H.264: " << stream.width << 'x' << stream.height
            << ", IDR " << stream.idrAccessUnit.size() << " bytes\n";
        sender.Send([&](std::uint64_t sequence)
        {
            return MakeVideoConfig(
                sequence, stream.width, stream.height, stream.fps,
                stream.profileIdc, stream.levelIdc, 5'000'000, stream.codecSpecificData);
        });
        sender.Send([&](std::uint64_t sequence)
        {
            return MakeVideoFrame(sequence, 1, 0, stream.idrAccessUnit, true, true);
        });
        std::cout << "Color bars sent; keeping the demo session alive for 6 seconds.\n";
        for (unsigned second = 0; second < 6; ++second)
        {
            if (WaitUnlessStopped(std::chrono::seconds(1))) break;
            sender.Send([](std::uint64_t sequence) { return MakePing(sequence, RandomNonce()); });
        }
        std::cout << "Synthetic video demo completed.\n";
    }

    void RunEncoderDemo(ProtocolSender& sender, unsigned seconds)
    {
        constexpr std::uint16_t width = 1280;
        constexpr std::uint16_t height = 800;
        constexpr std::uint16_t fps = 30;
        constexpr std::uint32_t bitrate = 5'000'000;
        tabletdisplay::host::MediaFoundationH264Encoder encoder({
            width, height, fps, bitrate, false });
        std::cout << "Media Foundation encoder demo: " << encoder.Name() << '\n';
        VideoStreamWriter writer(sender, encoder, width, height, fps, bitrate);

        std::vector<std::uint8_t> nv12(
            static_cast<std::size_t>(width) * height * 3U / 2U,
            128);
        const auto frameCount = static_cast<std::uint64_t>(seconds) * fps;
        auto nextFrameTime = std::chrono::steady_clock::now();
        const auto frameDuration = std::chrono::nanoseconds(1'000'000'000ULL / fps);
        std::uint64_t frameIndex = 0;
        for (; frameIndex < frameCount && !StopRequested(); ++frameIndex)
        {
            auto* yPlane = nv12.data();
            for (std::uint32_t y = 0; y < height; ++y)
            {
                for (std::uint32_t x = 0; x < width; ++x)
                {
                    const auto band = ((x + frameIndex * 12U) / 160U) % 8U;
                    yPlane[static_cast<std::size_t>(y) * width + x] =
                        static_cast<std::uint8_t>(32U + band * 27U);
                }
            }
            std::fill(
                nv12.begin() + static_cast<std::size_t>(width) * height,
                nv12.end(),
                static_cast<std::uint8_t>(128));
            for (const auto& encoded : encoder.Encode(
                    nv12,
                    frameIndex * 1'000'000ULL / fps,
                    frameIndex == 0))
                writer.Send(encoded);
            nextFrameTime += frameDuration;
            std::this_thread::sleep_until(nextFrameTime);
        }
        for (const auto& encoded : encoder.Drain()) writer.Send(encoded);
        writer.SendEndOfStream(frameIndex * 1'000'000ULL / fps);
        std::cout
            << "Encoder demo completed: " << writer.FramesSent() << " frames, "
            << writer.BytesSent() << " encoded bytes.\n";
    }

    void RunDesktopStream(ProtocolSender& sender, unsigned seconds)
    {
        constexpr std::uint16_t width = 1280;
        constexpr std::uint16_t height = 800;
        constexpr std::uint16_t fps = 30;
        constexpr std::uint32_t bitrate = 5'000'000;
        tabletdisplay::host::DesktopCapture capture(width, height);
        tabletdisplay::host::MediaFoundationH264Encoder encoder({
            width, height, fps, bitrate, false });
        std::cout
            << "Desktop capture: primary display -> " << width << 'x' << height << " @ " << fps << " FPS\n"
            << "Media Foundation encoder: " << encoder.Name()
            << (encoder.IsHardware() ? " (hardware)" : " (software fallback)") << '\n';
        VideoStreamWriter writer(sender, encoder, width, height, fps, bitrate);

        const auto frameDuration = std::chrono::nanoseconds(1'000'000'000ULL / fps);
        auto nextFrameTime = std::chrono::steady_clock::now();
        const auto frameCount = static_cast<std::uint64_t>(seconds) * fps;
        std::vector<std::uint8_t> nv12;
        std::uint64_t frameIndex = 0;
        for (; frameIndex < frameCount && !StopRequested(); ++frameIndex)
        {
            std::this_thread::sleep_until(nextFrameTime);
            const auto frame = capture.Capture();
            tabletdisplay::video::BgraToNv12(
                frame.bgra, frame.width, frame.height, frame.stride, nv12);
            const auto ptsUs = frameIndex * 1'000'000ULL / fps;
            for (const auto& encoded : encoder.Encode(nv12, ptsUs, frameIndex == 0))
                writer.Send(encoded);
            nextFrameTime += frameDuration;
        }
        for (const auto& encoded : encoder.Drain()) writer.Send(encoded);
        writer.SendEndOfStream(frameIndex * 1'000'000ULL / fps);
        std::cout
            << "Desktop stream completed: " << writer.FramesSent() << " frames, "
            << writer.BytesSent() << " encoded bytes.\n";
    }

    [[nodiscard]] bool DecoderAccepts(tabletdisplay::display::Mode mode, const AndroidDisplay& display) noexcept
    {
        // Decoder limits are reported per axis; compare long and short sides so the
        // portrait variant of a landscape mode is judged the same way.
        const auto decoderLong = std::max(display.maxAvcWidth, display.maxAvcHeight);
        const auto decoderShort = std::min(display.maxAvcWidth, display.maxAvcHeight);
        return std::max(mode.width, mode.height) <= decoderLong &&
            std::min(mode.width, mode.height) <= decoderShort;
    }

    // About 0.13 bit per pixel: 8 Mbit/s for 1280x800 at 60 FPS, which the tablet handles well.
    [[nodiscard]] std::uint32_t BitrateFor(std::uint32_t width, std::uint32_t height, std::uint32_t fps) noexcept
    {
        const auto pixelsPerSecond = static_cast<std::uint64_t>(width) * height * fps;
        return static_cast<std::uint32_t>(
            std::clamp<std::uint64_t>(pixelsPerSecond * 13U / 100U, 2'000'000U, 25'000'000U));
    }

    // seconds == 0 streams until the tray stops the Host or the connection fails.
    // The virtual monitor uses the tablet resolution divided by resolutionDivisor (1..4).
    void RunVirtualDisplayStream(
        SOCKET socket,
        ProtocolSender& sender,
        IncrementalDecoder receiveDecoder,
        const AndroidDisplay& display,
        unsigned resolutionDivisor,
        unsigned seconds)
    {
        namespace modes = tabletdisplay::display;
        const auto native = modes::Landscape(display.width, display.height);
        if (!modes::IsSupportedMode(modes::ScaledMode(native.width, native.height, 1)))
        {
            throw std::runtime_error(
                "The tablet resolution " + std::to_string(native.width) + 'x' +
                std::to_string(native.height) + " is outside the supported virtual display range");
        }
        // A smaller mode keeps the aspect ratio when the tablet decoder cannot play the requested one.
        auto divisor = resolutionDivisor;
        auto landscape = modes::ScaledMode(native.width, native.height, divisor);
        while (divisor <= modes::kMaxResolutionDivisor &&
            (!modes::IsSupportedMode(landscape) || !DecoderAccepts(landscape, display)))
        {
            ++divisor;
            landscape = modes::ScaledMode(native.width, native.height, divisor);
        }
        if (divisor > modes::kMaxResolutionDivisor)
            throw std::runtime_error("The tablet decoder cannot play any TabletDisplay mode");
        if (divisor != resolutionDivisor)
        {
            std::cout << "The tablet decoder is limited to " << display.maxAvcWidth << 'x'
                << display.maxAvcHeight << "; using 1/" << divisor << " of the tablet resolution.\n";
        }

        tabletdisplay::host::IddFrameCapture capture;
        capture.SetMonitorConnected(true, {
            static_cast<std::uint16_t>(native.width),
            static_cast<std::uint16_t>(native.height),
            static_cast<std::uint16_t>(divisor) });
        ReportProgress();
        const auto orientation = WaitForTabletDisplayOrientation();
        const bool portrait = orientation == DMDO_90 || orientation == DMDO_270;
        const auto rawWidth = static_cast<std::uint16_t>(landscape.width);
        const auto rawHeight = static_cast<std::uint16_t>(landscape.height);
        const std::uint16_t width = portrait ? rawHeight : rawWidth;
        const std::uint16_t height = portrait ? rawWidth : rawHeight;
        constexpr std::uint16_t fps = 60;
        const auto bitrate = BitrateFor(width, height, fps);
        std::cout << "Tablet " << native.width << 'x' << native.height << ", resolution 1/" << divisor
            << " -> " << width << 'x' << height << ", " << bitrate / 1'000U << " kbit/s\n";
        ConfigureTabletDisplayMode(width, height, fps);
        tabletdisplay::host::CapturedFrame frame{};
        const auto waitForFrame = [&]()
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (frame.width == 0 && std::chrono::steady_clock::now() < deadline)
            {
                (void)capture.Capture(frame, 500);
            }
            return frame.width != 0;
        };
        if (!waitForFrame())
        {
            throw std::runtime_error(
                "No IddCx frame is available in the requested TabletDisplay orientation. "
                "Select 'Extend these displays' in Windows display settings");
        }
        ReportProgress();
        std::cout << "IddCx raw frame: " << frame.width << 'x' << frame.height << '\n';
        if (frame.width != rawWidth || frame.height != rawHeight)
        {
            throw std::runtime_error(
                "IddCx frame size does not match the Windows display orientation");
        }
        tabletdisplay::host::SharedCapturedFrame sharedProbe{};
        bool sharedTransport = capture.ProbeShared(sharedProbe, 500) &&
            sharedProbe.width == rawWidth && sharedProbe.height == rawHeight;
        tabletdisplay::host::MediaFoundationH264Encoder encoder({
            width,
            height,
            fps,
            bitrate,
            true,
            sharedTransport ? sharedProbe.adapterLuid : 0 });
        tabletdisplay::host::SharedCapturedFrame initialSharedFrame{};
        if (sharedTransport && encoder.IsHardware())
        {
            try
            {
                encoder.PrepareSharedBgra(
                    sharedProbe.resourceName,
                    sharedProbe.generation,
                    sharedProbe.width,
                    sharedProbe.height,
                    orientation);
                capture.SetSharedTransport(true, sharedProbe.generation);
                if (!capture.CaptureShared(initialSharedFrame, 500))
                    throw std::runtime_error("No initial shared D3D11 frame is available");
            }
            catch (const std::exception& exception)
            {
                if (capture.UsesSharedTransport()) capture.SetSharedTransport(false);
                sharedTransport = false;
                std::cerr << "Shared D3D11 transport unavailable: " << exception.what()
                    << "; using CPU frame IPC fallback.\n";
            }
        }
        else
        {
            sharedTransport = false;
        }
        ReportProgress();
        std::cout
            << "IddCx capture: virtual display -> " << width << 'x' << height
            << " @ " << fps << " FPS\n"
            << "Media Foundation encoder: " << encoder.Name()
            << (encoder.IsHardware() ? " (hardware)" : " (software fallback)") << '\n'
            << "Driver/Host frame transport: "
            << (sharedTransport ? "shared D3D11 texture (zero-copy)" : "CPU BGRA IOCTL fallback")
            << '\n';

        const DWORD receiveTimeoutMs = 250;
        if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                reinterpret_cast<const char*>(&receiveTimeoutMs), sizeof(receiveTimeoutMs)) != 0)
            throw std::runtime_error("Cannot configure the touch receiver timeout");
        std::atomic_bool forceKeyFrame{};
        std::atomic_uint64_t receivedTouches{};
        std::atomic_uint64_t lastReceivedMs{ GetTickCount64() };
        std::atomic_bool primaryButtonDown{};
        [[maybe_unused]] TabletMouseButtonGuard mouseButtonGuard(primaryButtonDown);
        std::mutex receiverFailureMutex;
        std::exception_ptr receiverFailure;
        std::jthread receiver([&](std::stop_token stopToken)
        {
            try
            {
                auto displayBounds = FindTabletDisplayBounds();
                std::array<std::uint8_t, 16 * 1024> buffer{};
                while (!stopToken.stop_requested())
                {
                    const auto count = recv(socket, reinterpret_cast<char*>(buffer.data()),
                        static_cast<int>(buffer.size()), 0);
                    if (count == SOCKET_ERROR)
                    {
                        if (WSAGetLastError() == WSAETIMEDOUT) continue;
                        if (stopToken.stop_requested()) return;
                        throw TransportFailure("Android-to-Windows receive failed");
                    }
                    if (count == 0)
                    {
                        if (stopToken.stop_requested()) return;
                        throw TransportFailure("Android closed the streaming connection");
                    }
                    // The handshake decoder continues, so sequence numbers stay verified.
                    for (const auto& message : receiveDecoder.Feed(
                            std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(count))))
                    {
                        lastReceivedMs = GetTickCount64();
                        const auto type = static_cast<MessageType>(message.header.messageType);
                        if (type == MessageType::Touch)
                        {
                            InjectTouch(message.payload, displayBounds, primaryButtonDown);
                            ++receivedTouches;
                        }
                        else if (type == MessageType::RequestKeyframe)
                        {
                            forceKeyFrame = true;
                        }
                        else if (type == MessageType::Ping)
                        {
                            sender.Send([&](std::uint64_t sequence)
                            {
                                return MakePong(sequence, message.payload);
                            });
                        }
                        else if (type == MessageType::Pong)
                        {
                            // Heartbeat answer; receiving it is all that matters.
                        }
                        else if (type == MessageType::Error)
                        {
                            throw PeerReportedFailure(
                                "Android reported a streaming error: " + ErrorText(message.payload));
                        }
                        else
                        {
                            throw ProtocolFailure(
                                kErrorUnexpectedMessage,
                                "Unexpected Android-to-Windows streaming message");
                        }
                    }
                }
            }
            catch (...)
            {
                if (!stopToken.stop_requested())
                {
                    std::lock_guard lock(receiverFailureMutex);
                    receiverFailure = std::current_exception();
                }
            }
        });
        const auto checkReceiver = [&]
        {
            std::lock_guard lock(receiverFailureMutex);
            if (receiverFailure) std::rethrow_exception(receiverFailure);
        };

        VideoStreamWriter writer(sender, encoder, width, height, fps, bitrate);
        const auto frameDuration = std::chrono::nanoseconds(1'000'000'000ULL / fps);
        using Clock = std::chrono::steady_clock;
        const auto streamStarted = Clock::now();
        const auto frameCount = seconds == 0
            ? std::numeric_limits<std::uint64_t>::max()
            : static_cast<std::uint64_t>(seconds) * fps;

        struct PreparedFrameSlot
        {
            std::mutex mutex;
            std::condition_variable ready;
            std::condition_variable empty;
            tabletdisplay::host::CapturedFrame frame;
            tabletdisplay::host::SharedCapturedFrame sharedFrame;
            bool newSharedFrame{};
            bool occupied{};
            bool finished{};
            bool stop{};
            std::exception_ptr failure;
        } slot;
        const auto stopProducer = [&]
        {
            {
                std::lock_guard lock(slot.mutex);
                slot.stop = true;
                slot.occupied = false;
            }
            slot.empty.notify_all();
            slot.ready.notify_all();
        };

        std::chrono::nanoseconds captureTime{};
        std::chrono::nanoseconds encodingTime{};
        std::chrono::nanoseconds sendTime{};
        std::chrono::nanoseconds maximumProducerWork{};
        std::chrono::nanoseconds maximumConsumerWork{};
        std::chrono::nanoseconds maximumOutputInterval{};
        std::uint64_t delayedOutputIntervals{};
        Clock::time_point previousFrameFinished{};

        std::thread producer([&]
        {
            try
            {
                auto prepared = std::move(frame);
                auto preparedShared = initialSharedFrame;
                auto nextCaptureTime = streamStarted;
                for (std::uint64_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
                {
                    {
                        std::unique_lock lock(slot.mutex);
                        slot.empty.wait(lock, [&] { return !slot.occupied || slot.stop; });
                        if (slot.stop) return;
                    }

                    if (frameIndex != 0)
                    {
                        nextCaptureTime += frameDuration;
                        std::this_thread::sleep_until(nextCaptureTime);
                    }
                    if (frameIndex != 0 && frameIndex % fps == 0 &&
                        TabletDisplayOrientation() != orientation)
                    {
                        throw std::runtime_error(
                            "The virtual display orientation changed; reconnecting the stream");
                    }

                    const auto producerWorkStarted = Clock::now();
                    bool newSharedFrame = sharedTransport;
                    if (frameIndex != 0)
                    {
                        const auto captureStarted = Clock::now();
                        if (sharedTransport)
                        {
                            newSharedFrame = capture.CaptureShared(preparedShared, 0);
                            if (newSharedFrame &&
                                (preparedShared.width != rawWidth ||
                                 preparedShared.height != rawHeight))
                            {
                                throw std::runtime_error(
                                    "The shared virtual display mode changed during streaming");
                            }
                        }
                        else if (capture.Capture(prepared, 0))
                        {
                            if (prepared.width != rawWidth || prepared.height != rawHeight)
                            {
                                throw std::runtime_error(
                                    "The virtual display mode changed during streaming; restart virtual-stream.cmd");
                            }
                        }
                        captureTime += Clock::now() - captureStarted;
                    }
                    maximumProducerWork = std::max(
                        maximumProducerWork,
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            Clock::now() - producerWorkStarted));

                    {
                        std::lock_guard lock(slot.mutex);
                        if (slot.stop) return;
                        slot.frame = prepared;
                        slot.sharedFrame = preparedShared;
                        slot.newSharedFrame = newSharedFrame;
                        slot.occupied = true;
                    }
                    slot.ready.notify_one();
                }
            }
            catch (...)
            {
                std::lock_guard lock(slot.mutex);
                slot.failure = std::current_exception();
            }

            {
                std::lock_guard lock(slot.mutex);
                slot.finished = true;
            }
            slot.ready.notify_one();
        });

        tabletdisplay::host::CapturedFrame bgraFrame;
        tabletdisplay::host::SharedCapturedFrame sharedFrame;
        bool newSharedFrame{};
        auto activeSharedGeneration = initialSharedFrame.generation;
        std::uint64_t submittedFrames{};
        bool conversionReported{};
        bool stopped{};
        const auto pingToken = RandomNonce();
        auto lastPingSent = Clock::now();
        lastReceivedMs = GetTickCount64();
        try
        {
            for (std::uint64_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
            {
                checkReceiver();
                if (StopRequested())
                {
                    stopped = true;
                    break;
                }
                // Heartbeat: Android answers each PING with PONG, so silence means it is gone.
                if (Clock::now() - lastPingSent >= std::chrono::milliseconds(kHeartbeatIntervalMs))
                {
                    sender.Send([&](std::uint64_t sequence) { return MakePing(sequence, pingToken); });
                    lastPingSent = Clock::now();
                }
                if (GetTickCount64() - lastReceivedMs.load() > kIdleTimeoutMs)
                    throw ProtocolFailure(kErrorTimeout, "Android did not answer the heartbeat");

                {
                    std::unique_lock lock(slot.mutex);
                    slot.ready.wait(lock, [&]
                    {
                        return slot.occupied || slot.finished || slot.failure;
                    });
                    if (slot.failure) std::rethrow_exception(slot.failure);
                    if (!slot.occupied) break;
                    bgraFrame = slot.frame;
                    sharedFrame = slot.sharedFrame;
                    newSharedFrame = slot.newSharedFrame;
                }

                const auto consumerWorkStarted = Clock::now();
                const auto ptsUs = frameIndex * 1'000'000ULL / fps;
                const auto encodingStarted = Clock::now();
                const auto keyFrameRequested = forceKeyFrame.exchange(false);
                if (keyFrameRequested) writer.RequestDiscontinuity();
                const auto requestKeyFrame = frameIndex == 0 || keyFrameRequested;
                if (sharedTransport && newSharedFrame &&
                    sharedFrame.generation != activeSharedGeneration)
                {
                    encoder.PrepareSharedBgra(
                        sharedFrame.resourceName,
                        sharedFrame.generation,
                        sharedFrame.width,
                        sharedFrame.height,
                        orientation);
                    capture.SetSharedTransport(true, sharedFrame.generation);
                    activeSharedGeneration = sharedFrame.generation;
                }
                const auto encodedFrames = sharedTransport
                    ? (newSharedFrame
                        ? encoder.EncodeSharedBgra(
                            sharedFrame.resourceName,
                            sharedFrame.generation,
                            sharedFrame.width,
                            sharedFrame.height,
                            orientation,
                            ptsUs,
                            requestKeyFrame)
                        : encoder.EncodeRepeatedFrame(ptsUs, requestKeyFrame))
                    : encoder.EncodeBgra(
                        bgraFrame.bgra,
                        bgraFrame.width,
                        bgraFrame.height,
                        bgraFrame.stride,
                        orientation,
                        ptsUs,
                        requestKeyFrame);
                encodingTime += Clock::now() - encodingStarted;
                {
                    std::lock_guard lock(slot.mutex);
                    slot.occupied = false;
                }
                slot.empty.notify_one();
                if (!conversionReported)
                {
                    std::cout << "BGRA conversion: " << encoder.BgraConversionName() << '\n';
                    conversionReported = true;
                }
                const auto sendStarted = Clock::now();
                for (const auto& encoded : encodedFrames)
                    writer.Send(encoded);
                sendTime += Clock::now() - sendStarted;
                ++submittedFrames;

                const auto frameFinished = Clock::now();
                maximumConsumerWork = std::max(
                    maximumConsumerWork,
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        frameFinished - consumerWorkStarted));
                if (frameIndex != 0)
                {
                    const auto outputInterval = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        frameFinished - previousFrameFinished);
                    maximumOutputInterval = std::max(maximumOutputInterval, outputInterval);
                    if (outputInterval > frameDuration + std::chrono::milliseconds(2))
                        ++delayedOutputIntervals;
                }
                previousFrameFinished = frameFinished;
            }
            stopProducer();
            producer.join();
            checkReceiver();
        }
        catch (...)
        {
            stopProducer();
            if (producer.joinable()) producer.join();
            throw;
        }
        if (!stopped && submittedFrames != frameCount)
            throw std::runtime_error("The frame preparation pipeline stopped before completing the stream");

        const auto streamElapsed = Clock::now() - streamStarted;
        for (const auto& encoded : encoder.Drain()) writer.Send(encoded);
        writer.SendEndOfStream(submittedFrames * 1'000'000ULL / fps);
        receiver.request_stop();
        receiver.join();
        checkReceiver();
        std::cout
            << "Virtual-display stream completed: " << writer.FramesSent() << " frames, "
            << writer.BytesSent() << " encoded bytes, " << receivedTouches.load() << " touch events.\n";
        const auto averageMilliseconds = [submittedFrames](std::chrono::nanoseconds duration)
        {
            return std::chrono::duration<double, std::milli>(duration).count() /
                static_cast<double>(std::max<std::uint64_t>(1, submittedFrames));
        };
        std::cout
            << "Pipeline timing: "
            << std::chrono::duration<double>(streamElapsed).count() << " s loop, avg/frame "
            << averageMilliseconds(captureTime) << " ms capture, "
            << averageMilliseconds(encodingTime) << " ms "
            << (encoder.UsesGpuBgraConversion() ? "GPU" : "CPU fallback")
            << " convert/encode, "
            << averageMilliseconds(sendTime) << " ms send; max producer "
            << std::chrono::duration<double, std::milli>(maximumProducerWork).count()
            << " ms, max encode/send "
            << std::chrono::duration<double, std::milli>(maximumConsumerWork).count()
            << " ms; max output interval "
            << std::chrono::duration<double, std::milli>(maximumOutputInterval).count()
            << " ms, delayed intervals " << delayedOutputIntervals << '/'
            << (submittedFrames > 0 ? submittedFrames - 1 : 0) << ".\n";
    }

    void PrintHelp()
    {
        std::cout
            << "TabletDisplayHost\n"
            << "Usage:\n"
            << "  TabletDisplayHost --check\n"
            << "  TabletDisplayHost --connect [--serial SERIAL]\n"
            << "  TabletDisplayHost --demo [--serial SERIAL]\n"
            << "  TabletDisplayHost --encoder-demo [--serial SERIAL] [--seconds N]\n"
            << "  TabletDisplayHost --stream [--serial SERIAL] [--seconds N]\n"
            << "  TabletDisplayHost --virtual-stream [--serial SERIAL] [--seconds N] [--reconnect]\n"
            << "                    [--scale 1|2|3|4] [--stop-event NAME]\n"
            << "  TabletDisplayHost --version\n"
            << "--seconds 0 streams the virtual display until the stop event is signaled.\n"
            << "--scale N divides the tablet resolution by N, keeping its aspect ratio (default 1).\n";
    }

    int ConnectCommand(
        const std::string& requestedSerial,
        RunMode mode,
        unsigned streamSeconds,
        unsigned resolutionDivisor = 1)
    {
        const auto adb = FindAdb();
        std::cout << "ADB: " << Narrow(adb.wstring()) << '\n';
        const auto deviceResult = RunProcess(adb, L"devices -l");
        RequireAdbSuccess(deviceResult, "adb devices");
        const auto serial = SelectDevice(ParseDevices(deviceResult.output), requestedSerial);
        std::cout << "Device: " << serial << '\n';

        const auto forwardList = RunProcess(adb, L"forward --list");
        RequireAdbSuccess(forwardList, "adb forward --list");
        const auto hadForward = ForwardAlreadyExists(forwardList.output, serial);
        const auto selector = L"-s \"" + Widen(serial) + L"\" ";
        bool createdForward{};
        try
        {
            if (!hadForward)
            {
                const auto result = RunProcess(adb, selector + L"forward tcp:27183 tcp:27183");
                RequireAdbSuccess(result, "adb forward");
                createdForward = true;
            }
            WinsockSession winsock;
            auto socket = ConnectLoopback();
            auto handshake = PerformHandshake(socket.get());
            ProtocolSender sender(socket.get(), handshake.nextSequence);
            try
            {
                if (mode == RunMode::Synthetic)
                {
                    RunSyntheticDemo(sender);
                }
                else if (mode == RunMode::EncoderDemo)
                {
                    RunEncoderDemo(sender, streamSeconds);
                }
                else if (mode == RunMode::Desktop)
                {
                    RunDesktopStream(sender, streamSeconds);
                }
                else if (mode == RunMode::VirtualDisplay)
                {
                    RunVirtualDisplayStream(
                        socket.get(),
                        sender,
                        std::move(handshake.decoder),
                        handshake.display,
                        resolutionDivisor,
                        streamSeconds);
                }
            }
            catch (...)
            {
                SendFatalError(sender, std::current_exception());
                throw;
            }
            if (createdForward)
            {
                const auto result = RunProcess(adb, selector + L"forward --remove tcp:27183");
                RequireAdbSuccess(result, "adb forward --remove");
            }
        }
        catch (...)
        {
            if (createdForward)
            {
                try
                {
                    (void)RunProcess(adb, selector + L"forward --remove tcp:27183");
                }
                catch (...)
                {
                }
            }
            throw;
        }
        return 0;
    }

    // totalSeconds == 0 keeps reconnecting until the tray signals the stop event.
    int ReconnectingVirtualDisplayCommand(
        const std::string& requestedSerial,
        unsigned totalSeconds,
        unsigned resolutionDivisor)
    {
        using Clock = std::chrono::steady_clock;
        const bool unlimited = totalSeconds == 0;
        const auto deadline = Clock::now() + std::chrono::seconds(totalSeconds);
        ProgressWatchdog watchdog(std::chrono::seconds(30));
        unsigned reconnectCount{};
        bool reportedMissingDevice{};
        while (!StopRequested() && (unlimited || Clock::now() < deadline))
        {
            ReportProgress();
            unsigned attemptSeconds = 0;
            if (!unlimited)
            {
                const auto remaining = deadline - Clock::now();
                const auto wholeSeconds = std::chrono::duration_cast<std::chrono::seconds>(remaining);
                attemptSeconds = std::max(1U, static_cast<unsigned>(wholeSeconds.count()) +
                    (remaining > wholeSeconds ? 1U : 0U));
            }
            try
            {
                ConnectCommand(requestedSerial, RunMode::VirtualDisplay, attemptSeconds, resolutionDivisor);
                reportedMissingDevice = false;
                if (!unlimited && !StopRequested())
                {
                    std::cout << "Continuous virtual-display stream completed after "
                        << reconnectCount << " reconnect(s).\n";
                    return 0;
                }
            }
            catch (const NoDeviceFailure& exception)
            {
                // Waiting for the tablet: poll adb less often and report the state only once.
                if (!reportedMissingDevice) std::cerr << exception.what() << "; waiting for the tablet.\n";
                reportedMissingDevice = true;
                (void)WaitUnlessStopped(std::chrono::seconds(3));
            }
            catch (const std::exception& exception)
            {
                reportedMissingDevice = false;
                if (!unlimited && Clock::now() >= deadline) break;
                ++reconnectCount;
                std::cerr << "Virtual-display connection lost: " << exception.what()
                    << ". Reconnect attempt " << reconnectCount << " will start in 1 second.\n";
                (void)WaitUnlessStopped(std::chrono::seconds(1));
            }
        }
        if (StopRequested())
        {
            std::cout << "Virtual-display stream stopped after " << reconnectCount << " reconnect(s).\n";
            return 0;
        }
        std::cout << "Continuous virtual-display stream reached its time limit after "
            << reconnectCount << " reconnect(s).\n";
        return 0;
    }
}

int main(int argc, char* argv[])
{
    try
    {
        const std::string_view command = argc > 1 ? argv[1] : "--check";
        if (command == "--help")
        {
            PrintHelp();
            return 0;
        }
        if (command == "--version")
        {
            std::cout << "TabletDisplayHost 0.1.5\n";
            return 0;
        }
        if (command == "--check")
        {
            std::cout
                << "TabletDisplayHost transport is ready.\n"
                << "Protocol version: " << tabletdisplay::protocol::kProtocolVersion << '\n'
                << "ADB: " << Narrow(FindAdb().wstring()) << '\n';
            return 0;
        }
        if (command == "--connect" || command == "--demo" ||
            command == "--encoder-demo" || command == "--stream" ||
            command == "--virtual-stream")
        {
            std::string serial;
            unsigned seconds = 10;
            bool reconnect = false;
            unsigned resolutionDivisor = 1;
            for (int index = 2; index < argc; ++index)
            {
                const std::string_view option = argv[index];
                if (option == "--serial" && index + 1 < argc)
                {
                    serial = argv[++index];
                }
                else if (option == "--seconds" && index + 1 < argc &&
                    (command == "--stream" || command == "--virtual-stream" ||
                     command == "--encoder-demo"))
                {
                    const auto parsed = std::stoul(argv[++index]);
                    const bool unlimitedAllowed = command == "--virtual-stream";
                    if ((parsed == 0 && !unlimitedAllowed) || parsed > 3'600)
                        throw std::runtime_error("--seconds must be in 1..3600 (0 = unlimited for --virtual-stream)");
                    seconds = parsed;
                }
                else if (option == "--scale" && index + 1 < argc && command == "--virtual-stream")
                {
                    const auto parsed = std::stoul(argv[++index]);
                    if (parsed < tabletdisplay::display::kMinResolutionDivisor ||
                        parsed > tabletdisplay::display::kMaxResolutionDivisor)
                        throw std::runtime_error("--scale must be 1, 2, 3 or 4");
                    resolutionDivisor = parsed;
                }
                else if (option == "--reconnect" && command == "--virtual-stream")
                {
                    reconnect = true;
                }
                else if (option == "--stop-event" && index + 1 < argc)
                {
                    const auto name = Widen(argv[++index]);
                    g_stopEvent = OpenEventW(SYNCHRONIZE, FALSE, name.c_str());
                    if (g_stopEvent == nullptr) throw std::runtime_error("Cannot open the stop event");
                }
                else
                {
                    throw std::runtime_error(
                        "Expected [--serial SERIAL] [--seconds N] [--reconnect] [--scale N] [--stop-event NAME]");
                }
            }
            const auto mode = command == "--virtual-stream"
                ? RunMode::VirtualDisplay
                : (command == "--stream"
                    ? RunMode::Desktop
                    : (command == "--encoder-demo"
                        ? RunMode::EncoderDemo
                        : (command == "--demo" ? RunMode::Synthetic : RunMode::Connect)));
            if (reconnect) return ReconnectingVirtualDisplayCommand(serial, seconds, resolutionDivisor);
            return ConnectCommand(serial, mode, seconds, resolutionDivisor);
        }
        throw std::runtime_error("Unknown argument");
    }
    catch (const std::exception& exception)
    {
        std::cerr << "TabletDisplayHost: " << exception.what() << '\n';
        return 1;
    }
}
