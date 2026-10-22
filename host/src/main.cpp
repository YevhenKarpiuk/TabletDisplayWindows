#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <timeapi.h>

#include "DesktopCapture.h"
#include "display/DisplayModes.h"
#include "IddFrameCapture.h"
#include "MediaFoundationH264Encoder.h"
#include "TabletMonitors.h"
#include "ipc/FrameTransport.h"
#include "protocol/Protocol.h"
#include "video/SyntheticH264.h"
#include "video/VideoProcessing.h"
#include "wifi/WifiClient.h"
#include "wifi/WifiDiscovery.h"

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
    // The driver monitor this Host leased; every tablet streams its own virtual monitor.
    std::uint32_t g_monitorSlot{};
    // Virtual display refresh and encoding rate (--fps 60|30).
    std::uint16_t g_streamFps = 60;
    // --ignore-decoder-limit streams the chosen resolution even beyond the decoder limits the
    // tablet reports; many decoders play more than they declare.
    bool g_ignoreDecoderLimit{};
    // --wifi-tablet: the paired tablet (32 hex digits) streamed over Wi-Fi instead of adb;
    // --wifi-address overrides the address remembered from discovery.
    std::wstring g_wifiTablet;
    std::wstring g_wifiAddress;
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

    // The same clock and conversion as the driver's frame timestamps.
    [[nodiscard]] std::uint64_t QpcMicroseconds() noexcept
    {
        LARGE_INTEGER counter{};
        LARGE_INTEGER frequency{};
        QueryPerformanceCounter(&counter);
        QueryPerformanceFrequency(&frequency);
        if (frequency.QuadPart <= 0) return 0;
        return static_cast<std::uint64_t>(counter.QuadPart / frequency.QuadPart) * 1'000'000ULL +
            static_cast<std::uint64_t>(counter.QuadPart % frequency.QuadPart) * 1'000'000ULL /
                static_cast<std::uint64_t>(frequency.QuadPart);
    }

    // Frame polling sleeps for 1 ms, which needs a 1 ms system timer. Windows 11 ignores the
    // request of a process without a visible window unless it opts out of that throttling.
    void UseHighResolutionTimer() noexcept
    {
        // Each reconnect asks again; timeBeginPeriod requests would otherwise pile up.
        static bool enabled{};
        if (enabled) return;
        enabled = true;
        PROCESS_POWER_THROTTLING_STATE throttling{};
        throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        throttling.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        throttling.StateMask = 0;
        (void)SetProcessInformation(
            GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));
        (void)timeBeginPeriod(1);
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
        auto deviceName = tabletdisplay::host::FindTabletDisplayDevice(g_monitorSlot);
        if (!deviceName) return std::nullopt;
        TabletDisplayMode result{ std::move(*deviceName), {} };
        result.mode.dmSize = sizeof(result.mode);
        if (!EnumDisplaySettingsExW(result.deviceName.c_str(), ENUM_CURRENT_SETTINGS, &result.mode, 0))
            throw std::runtime_error("Cannot query the TabletDisplay mode");
        return result;
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

    [[nodiscard]] bool ForwardAlreadyExists(
        const std::string& output,
        const std::string& serial,
        std::uint16_t localPort)
    {
        std::istringstream lines(output);
        std::string line;
        const auto expected = serial + " tcp:" + std::to_string(localPort) + " tcp:" + std::to_string(kPort);
        while (std::getline(lines, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line == expected) return true;
        }
        return false;
    }

    [[nodiscard]] SocketHandle ConnectLoopback(std::uint16_t localPort)
    {
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_port = htons(localPort);
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
        throw std::runtime_error("Cannot connect to Android on 127.0.0.1:" + std::to_string(localPort));
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

    void SendMessage(tabletdisplay::wifi::ByteStream& stream, const Message& message)
    {
        try
        {
            stream.Send(EncodeMessage(message.header, message.payload));
        }
        catch (const TransportFailure&)
        {
            throw;
        }
        catch (const std::runtime_error& exception)
        {
            throw TransportFailure(exception.what());
        }
    }

    // Receive with the transport's conventions mapped to Host failures; -1 means timeout.
    [[nodiscard]] int ReceiveSome(tabletdisplay::wifi::ByteStream& stream, std::span<std::uint8_t> buffer)
    {
        try
        {
            return stream.Receive(buffer);
        }
        catch (const std::runtime_error& exception)
        {
            throw TransportFailure(exception.what());
        }
    }

    // Serializes Windows-to-Android messages from the streaming and receiving threads and
    // assigns the direction's sequence numbers.
    class ProtocolSender
    {
    public:
        ProtocolSender(tabletdisplay::wifi::ByteStream& stream, std::uint64_t nextSequence) noexcept
            : m_stream(stream), m_nextSequence(nextSequence)
        {
        }

        template<typename MakeMessageForSequence>
        void Send(MakeMessageForSequence&& make)
        {
            std::lock_guard lock(m_mutex);
            SendMessage(m_stream, make(m_nextSequence++));
        }

    private:
        tabletdisplay::wifi::ByteStream& m_stream;
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

    [[nodiscard]] HandshakeResult PerformHandshake(tabletdisplay::wifi::ByteStream& stream)
    {
        const auto nonce = RandomNonce();
        SendMessage(stream, MakeHello(nonce));
        IncrementalDecoder decoder;
        std::array<std::uint8_t, 16 * 1024> buffer{};
        bool ackReceived{};
        bool displayReceived{};
        bool pingSent{};
        AndroidDisplay display;
        const auto pingToken = RandomNonce();

        while (true)
        {
            const auto count = ReceiveSome(stream, buffer);
            if (count == 0) throw TransportFailure("Android closed the handshake connection");
            if (count < 0) throw TransportFailure("Android did not answer the handshake in time");
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
                SendMessage(stream, MakePing(2, pingToken));
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

    // The tray lists every tablet's resolution choices from what its Host last saw: the panel,
    // the divisor actually streamed (which may exceed the chosen one when the decoder cannot
    // play it) and the smallest divisor the tablet decoder accepts.
    void SaveTabletDisplay(
        const std::string& serial,
        std::uint32_t nativeWidth,
        std::uint32_t nativeHeight,
        bool portrait,
        std::uint32_t activeDivisor,
        std::uint32_t minimumDivisor,
        std::uint32_t fps) noexcept
    {
        const auto key = L"Software\\TabletDisplay\\Devices\\" + Widen(serial);
        const DWORD values[] = {
            nativeWidth, nativeHeight, portrait ? 1UL : 0UL, activeDivisor, minimumDivisor, fps };
        const wchar_t* const names[] = {
            L"NativeWidth", L"NativeHeight", L"Portrait", L"ActiveDivisor", L"MinimumDivisor", L"ActiveFps" };
        for (std::size_t index = 0; index < std::size(values); ++index)
        {
            (void)RegSetKeyValueW(
                HKEY_CURRENT_USER, key.c_str(), names[index], REG_DWORD, &values[index], sizeof(DWORD));
        }
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
    // monitor 0 leases the first free driver monitor, N (1..4) leases monitor N.
    void RunVirtualDisplayStream(
        SOCKET socket,
        tabletdisplay::wifi::ByteStream& stream,
        ProtocolSender& sender,
        IncrementalDecoder receiveDecoder,
        const AndroidDisplay& display,
        const std::string& serial,
        unsigned resolutionDivisor,
        unsigned monitor,
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
            (!modes::IsSupportedMode(landscape) ||
             (!g_ignoreDecoderLimit && !DecoderAccepts(landscape, display))))
        {
            ++divisor;
            landscape = modes::ScaledMode(native.width, native.height, divisor);
        }
        if (divisor > modes::kMaxResolutionDivisor)
            throw std::runtime_error("The tablet decoder cannot play any TabletDisplay mode");
        auto minimumDivisor = modes::kMinResolutionDivisor;
        while (minimumDivisor < divisor)
        {
            const auto candidate = modes::ScaledMode(native.width, native.height, minimumDivisor);
            if (modes::IsSupportedMode(candidate) && DecoderAccepts(candidate, display)) break;
            ++minimumDivisor;
        }
        if (divisor != resolutionDivisor)
        {
            std::cout << "The tablet decoder is limited to " << display.maxAvcWidth << 'x'
                << display.maxAvcHeight << "; using 1/" << divisor << " of the tablet resolution.\n";
        }

        tabletdisplay::host::IddFrameCapture capture;
        capture.SetMonitorConnected(true, {
            static_cast<std::uint16_t>(native.width),
            static_cast<std::uint16_t>(native.height),
            static_cast<std::uint16_t>(divisor),
            static_cast<std::uint16_t>(monitor) });
        g_monitorSlot = capture.MonitorSlot();
        std::cout << "TabletDisplay monitor " << g_monitorSlot + 1 << '\n';
        ReportProgress();
        const auto orientation = WaitForTabletDisplayOrientation();
        const bool portrait = orientation == DMDO_90 || orientation == DMDO_270;
        SaveTabletDisplay(serial, native.width, native.height, portrait, divisor, minimumDivisor, g_streamFps);
        const auto rawWidth = static_cast<std::uint16_t>(landscape.width);
        const auto rawHeight = static_cast<std::uint16_t>(landscape.height);
        const std::uint16_t width = portrait ? rawHeight : rawWidth;
        const std::uint16_t height = portrait ? rawWidth : rawHeight;
        const std::uint16_t fps = g_streamFps;
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
                    int count{};
                    try
                    {
                        count = ReceiveSome(stream, buffer);
                    }
                    catch (const TransportFailure&)
                    {
                        if (stopToken.stop_requested()) return;
                        throw;
                    }
                    if (count < 0) continue;
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
        const auto streamDeadline = seconds == 0
            ? Clock::time_point::max()
            : streamStarted + std::chrono::seconds(seconds);
        // Windows presents a new frame only when the picture changes. A few repeats after the
        // last change let the encoder refine that picture and push it out of a decoder that holds
        // frames; after that nothing is encoded until the picture changes again.
        constexpr unsigned kRefinementFrames = 3;
        // The driver signals an event for every new frame. Without it (older driver) frames are
        // polled; a new frame then waits at most this long instead of up to a whole 60 Hz tick.
        constexpr auto kCapturePollInterval = std::chrono::milliseconds(1);
        // Longest wait for the frame event, which bounds how late a keyframe request, the
        // orientation check and the driver's shared-transport heartbeat run while nothing changes.
        constexpr auto kMaximumFrameWait = std::chrono::milliseconds(20);
        const HANDLE frameEvent = capture.FrameEvent();
        if (frameEvent == nullptr) UseHighResolutionTimer();
        std::cout << "Frame delivery: " << (frameEvent != nullptr ? "driver event" : "1 ms polling") << '\n';

        struct PreparedFrameSlot
        {
            std::mutex mutex;
            std::condition_variable ready;
            std::condition_variable empty;
            tabletdisplay::host::CapturedFrame frame;
            tabletdisplay::host::SharedCapturedFrame sharedFrame;
            bool newFrame{};
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
        std::atomic<std::uint64_t> producerAliveMs{ GetTickCount64() };

        std::chrono::nanoseconds captureTime{};
        std::chrono::nanoseconds encodingTime{};
        std::chrono::nanoseconds sendTime{};
        std::chrono::nanoseconds maximumConsumerWork{};
        std::uint64_t frameAgeTotalUs{};
        std::uint64_t maximumFrameAgeUs{};
        std::uint64_t agedFrames{};
        std::uint64_t newFrames{};

        std::thread producer([&]
        {
            try
            {
                auto prepared = std::move(frame);
                auto preparedShared = initialSharedFrame;
                unsigned refinementsLeft = kRefinementFrames;
                auto lastSubmitted = streamStarted;
                auto nextOrientationCheck = streamStarted + std::chrono::seconds(1);
                // The first slot carries the frame captured while the stream was set up.
                for (bool firstFrame = true;; firstFrame = false)
                {
                    {
                        std::unique_lock lock(slot.mutex);
                        slot.empty.wait(lock, [&] { return !slot.occupied || slot.stop; });
                        if (slot.stop) return;
                    }

                    bool newFrame = firstFrame;
                    while (!newFrame)
                    {
                        producerAliveMs = GetTickCount64();
                        {
                            std::lock_guard lock(slot.mutex);
                            if (slot.stop) return;
                        }
                        const auto now = Clock::now();
                        if (now >= nextOrientationCheck)
                        {
                            if (TabletDisplayOrientation() != orientation)
                            {
                                throw std::runtime_error(
                                    "The virtual display orientation changed; reconnecting the stream");
                            }
                            nextOrientationCheck = now + std::chrono::seconds(1);
                        }

                        const auto captureStarted = Clock::now();
                        if (sharedTransport)
                        {
                            newFrame = capture.CaptureShared(preparedShared, 0);
                            if (newFrame &&
                                (preparedShared.width != rawWidth ||
                                 preparedShared.height != rawHeight))
                            {
                                throw std::runtime_error(
                                    "The shared virtual display mode changed during streaming");
                            }
                        }
                        else if (capture.Capture(prepared, 0))
                        {
                            newFrame = true;
                            if (prepared.width != rawWidth || prepared.height != rawHeight)
                            {
                                throw std::runtime_error(
                                    "The virtual display mode changed during streaming; restart virtual-stream.cmd");
                            }
                        }
                        if (newFrame)
                        {
                            captureTime += Clock::now() - captureStarted;
                            break;
                        }
                        // A keyframe request is answered even while the picture is static.
                        if (forceKeyFrame.load()) break;
                        if (refinementsLeft > 0 && now - lastSubmitted >= frameDuration) break;
                        if (frameEvent != nullptr)
                        {
                            auto wait = kMaximumFrameWait;
                            if (refinementsLeft > 0)
                            {
                                const auto untilRefinement = std::chrono::ceil<std::chrono::milliseconds>(
                                    lastSubmitted + frameDuration - now);
                                wait = std::clamp(untilRefinement, std::chrono::milliseconds(0), wait);
                            }
                            (void)WaitForSingleObject(frameEvent, static_cast<DWORD>(wait.count()));
                        }
                        else
                        {
                            std::this_thread::sleep_for(kCapturePollInterval);
                        }
                    }
                    if (newFrame)
                        refinementsLeft = kRefinementFrames;
                    else if (refinementsLeft > 0)
                        --refinementsLeft;
                    lastSubmitted = Clock::now();

                    {
                        std::lock_guard lock(slot.mutex);
                        if (slot.stop) return;
                        slot.frame = prepared;
                        slot.sharedFrame = preparedShared;
                        slot.newFrame = newFrame;
                        slot.newSharedFrame = sharedTransport && newFrame;
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
        bool newFrame{};
        bool newSharedFrame{};
        auto activeSharedGeneration = initialSharedFrame.generation;
        std::uint64_t submittedFrames{};
        std::uint64_t lastPtsUs{};
        bool conversionReported{};
        const auto pingToken = RandomNonce();
        auto lastPingSent = Clock::now();
        lastReceivedMs = GetTickCount64();
        try
        {
            for (;;)
            {
                checkReceiver();
                if (StopRequested() || Clock::now() >= streamDeadline) break;
                // Heartbeat: Android answers each PING with PONG, so silence means it is gone.
                if (Clock::now() - lastPingSent >= std::chrono::milliseconds(kHeartbeatIntervalMs))
                {
                    sender.Send([&](std::uint64_t sequence) { return MakePing(sequence, pingToken); });
                    lastPingSent = Clock::now();
                }
                if (GetTickCount64() - lastReceivedMs.load() > kIdleTimeoutMs)
                    throw ProtocolFailure(kErrorTimeout, "Android did not answer the heartbeat");
                // A static picture sends no frames, so the watchdog is fed while both the tablet
                // and the producer (which polls the driver every millisecond) are alive.
                if (GetTickCount64() - producerAliveMs.load() < 5'000U) ReportProgress();

                {
                    std::unique_lock lock(slot.mutex);
                    // Wakes periodically so the heartbeat runs while the picture is static.
                    slot.ready.wait_for(lock, std::chrono::milliseconds(50), [&]
                    {
                        return slot.occupied || slot.finished || slot.failure;
                    });
                    if (slot.failure) std::rethrow_exception(slot.failure);
                    if (!slot.occupied)
                    {
                        if (slot.finished)
                            throw std::runtime_error("The frame preparation pipeline stopped unexpectedly");
                        continue;
                    }
                    bgraFrame = slot.frame;
                    sharedFrame = slot.sharedFrame;
                    newFrame = slot.newFrame;
                    newSharedFrame = slot.newSharedFrame;
                }

                const auto encodingStarted = Clock::now();
                if (newSharedFrame && sharedFrame.timestampUs != 0)
                {
                    const auto nowUs = QpcMicroseconds();
                    if (nowUs > sharedFrame.timestampUs)
                    {
                        const auto ageUs = nowUs - sharedFrame.timestampUs;
                        frameAgeTotalUs += ageUs;
                        maximumFrameAgeUs = std::max(maximumFrameAgeUs, ageUs);
                        ++agedFrames;
                    }
                }
                // Frames follow the picture changes, so PTS is wall-clock time, kept increasing.
                const auto ptsUs = std::max<std::uint64_t>(
                    lastPtsUs + 1U,
                    static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                        encodingStarted - streamStarted).count()));
                lastPtsUs = ptsUs;
                const auto keyFrameRequested = forceKeyFrame.exchange(false);
                if (keyFrameRequested) writer.RequestDiscontinuity();
                const auto requestKeyFrame = submittedFrames == 0 || keyFrameRequested;
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
                if (newFrame) ++newFrames;
                maximumConsumerWork = std::max(
                    maximumConsumerWork,
                    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - encodingStarted));
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

        const auto streamElapsed = Clock::now() - streamStarted;
        for (const auto& encoded : encoder.Drain()) writer.Send(encoded);
        writer.SendEndOfStream(lastPtsUs + 1U);
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
            << std::chrono::duration<double>(streamElapsed).count() << " s loop, "
            << submittedFrames << " frames (" << newFrames << " new, "
            << submittedFrames - newFrames << " refinement/keyframe repeats); avg/frame "
            << averageMilliseconds(captureTime) << " ms capture, "
            << averageMilliseconds(encodingTime) << " ms "
            << (encoder.UsesGpuBgraConversion() ? "GPU" : "CPU fallback")
            << " convert/encode, "
            << averageMilliseconds(sendTime) << " ms send; max encode/send "
            << std::chrono::duration<double, std::milli>(maximumConsumerWork).count()
            << " ms; driver frame age at encode avg "
            << (agedFrames > 0 ? static_cast<double>(frameAgeTotalUs) / agedFrames / 1'000.0 : 0.0)
            << " ms, max " << static_cast<double>(maximumFrameAgeUs) / 1'000.0 << " ms.\n";
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
            << "                    [--scale 1|2|3|4] [--fps 60|30] [--ignore-decoder-limit]\n"
            << "                    [--monitor 1|2|3|4] [--port PORT] [--stop-event NAME]\n"
            << "                    [--wifi-tablet ID [--wifi-address HOST]]\n"
            << "  TabletDisplayHost --version\n"
            << "--seconds 0 streams the virtual display until the stop event is signaled.\n"
            << "--scale N divides the tablet resolution by N, keeping its aspect ratio (default 1).\n"
            << "--fps 60|30 sets the virtual display refresh and stream rate (default 60).\n"
            << "--ignore-decoder-limit streams the chosen resolution beyond the decoder limits the tablet reports.\n"
            << "--monitor N streams TabletDisplay monitor N (default: the first free one), so several\n"
            << "  tablets can run one Host each; every Host needs its own --serial and --port.\n"
            << "--port PORT is the local adb forward port (default " << kPort << ").\n"
            << "--wifi-tablet ID streams to a tablet paired over Wi-Fi instead of using adb.\n";
    }

    // Runs protocol v1 over an established USB (adb) or Wi-Fi stream.
    void RunProtocolSession(
        SOCKET socket,
        tabletdisplay::wifi::ByteStream& stream,
        const std::string& deviceKey,
        RunMode mode,
        unsigned streamSeconds,
        unsigned resolutionDivisor,
        unsigned monitor)
    {
        auto handshake = PerformHandshake(stream);
        ProtocolSender sender(stream, handshake.nextSequence);
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
                    socket,
                    stream,
                    sender,
                    std::move(handshake.decoder),
                    handshake.display,
                    deviceKey,
                    resolutionDivisor,
                    monitor,
                    streamSeconds);
            }
        }
        catch (...)
        {
            SendFatalError(sender, std::current_exception());
            throw;
        }
    }

    // Wi-Fi without adb (doc/PROTOCOL.md section 10): the tablet must be paired from the tray.
    void ConnectWifi(RunMode mode, unsigned streamSeconds, unsigned resolutionDivisor, unsigned monitor)
    {
        namespace wifi = tabletdisplay::wifi;
        const auto id = wifi::FromHex(g_wifiTablet);
        if (!id || id->size() != wifi::kIdSize) throw std::runtime_error("--wifi-tablet must be 32 hex digits");
        const auto tablet = wifi::LoadPairing(*id);
        if (!tablet) throw std::runtime_error("This tablet is not paired; pair it from the tray menu");
        const auto address = g_wifiAddress.empty() ? tablet->address : g_wifiAddress;
        if (address.empty()) throw NoDeviceFailure("The tablet's Wi-Fi address is not known yet");

        WinsockSession winsock;
        SocketHandle socket;
        try
        {
            socket.reset(wifi::ConnectTcp(address, tablet->port, 3'000));
        }
        catch (const std::runtime_error& exception)
        {
            // Out of range or switched off: wait like for an unplugged USB tablet.
            throw NoDeviceFailure(exception.what());
        }
        const DWORD timeout = 5'000;
        setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(socket.get(), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        auto stream = wifi::OpenSession(socket.get(), wifi::WindowsId(), *tablet);
        std::cout << "Wi-Fi tablet: " << Narrow(tablet->name) << " at " << Narrow(address) << '\n';
        if (!g_wifiAddress.empty() && g_wifiAddress != tablet->address)
            wifi::SaveTabletAddress(*id, g_wifiAddress, tablet->port);
        RunProtocolSession(
            socket.get(), *stream, "wifi-" + Narrow(g_wifiTablet), mode, streamSeconds, resolutionDivisor, monitor);
    }

    int ConnectCommand(
        const std::string& requestedSerial,
        RunMode mode,
        unsigned streamSeconds,
        unsigned resolutionDivisor = 1,
        unsigned monitor = 0,
        std::uint16_t localPort = kPort)
    {
        if (!g_wifiTablet.empty())
        {
            ConnectWifi(mode, streamSeconds, resolutionDivisor, monitor);
            return 0;
        }
        const auto adb = FindAdb();
        std::cout << "ADB: " << Narrow(adb.wstring()) << '\n';
        const auto deviceResult = RunProcess(adb, L"devices -l");
        RequireAdbSuccess(deviceResult, "adb devices");
        const auto serial = SelectDevice(ParseDevices(deviceResult.output), requestedSerial);
        std::cout << "Device: " << serial << '\n';

        const auto forwardList = RunProcess(adb, L"forward --list");
        RequireAdbSuccess(forwardList, "adb forward --list");
        const auto hadForward = ForwardAlreadyExists(forwardList.output, serial, localPort);
        const auto selector = L"-s \"" + Widen(serial) + L"\" ";
        const auto localEndpoint = L"tcp:" + std::to_wstring(localPort);
        bool createdForward{};
        try
        {
            if (!hadForward)
            {
                const auto result = RunProcess(
                    adb, selector + L"forward " + localEndpoint + L" tcp:" + std::to_wstring(kPort));
                RequireAdbSuccess(result, "adb forward");
                createdForward = true;
            }
            WinsockSession winsock;
            auto socket = ConnectLoopback(localPort);
            tabletdisplay::wifi::PlainStream stream(socket.get());
            RunProtocolSession(socket.get(), stream, serial, mode, streamSeconds, resolutionDivisor, monitor);
            if (createdForward)
            {
                const auto result = RunProcess(adb, selector + L"forward --remove " + localEndpoint);
                RequireAdbSuccess(result, "adb forward --remove");
            }
        }
        catch (...)
        {
            if (createdForward)
            {
                try
                {
                    (void)RunProcess(adb, selector + L"forward --remove " + localEndpoint);
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
        unsigned resolutionDivisor,
        unsigned monitor,
        std::uint16_t localPort)
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
                ConnectCommand(
                    requestedSerial, RunMode::VirtualDisplay, attemptSeconds, resolutionDivisor, monitor, localPort);
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
            std::cout << "TabletDisplayHost 0.2.0\n";
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
        if (command == "--wifi-discover")
        {
            namespace wifi = tabletdisplay::wifi;
            WinsockSession winsock;
            const auto paired = wifi::LoadPairings();
            const auto tablets = wifi::DiscoverTablets(3'000);
            if (tablets.empty()) std::cout << "No TabletDisplay app found on the local network.\n";
            for (const auto& tablet : tablets)
            {
                const bool isPaired = std::ranges::any_of(paired, [&](const wifi::PairedTablet& known)
                {
                    return wifi::ToHex(known.id) == tablet.id;
                });
                std::cout << Narrow(tablet.name) << "  id " << Narrow(tablet.id) << "  "
                    << Narrow(tablet.address) << ':' << tablet.port << (isPaired ? "  paired" : "  not paired") << '\n';
            }
            return 0;
        }
        if (command == "--wifi-pair" && argc >= 3)
        {
            namespace wifi = tabletdisplay::wifi;
            WinsockSession winsock;
            const auto address = Widen(argv[2]);
            const auto port = argc >= 4 ? static_cast<std::uint16_t>(std::stoul(argv[3])) : wifi::kWifiPort;
            SocketHandle socket(wifi::ConnectTcp(address, port, 3'000));
            auto tablet = wifi::Pair(socket.get(), wifi::WindowsId(), wifi::WindowsName(),
                [](const std::string& code, const std::wstring& tabletName)
                {
                    std::cout << "Tablet " << Narrow(tabletName) << " shows code " << code.substr(0, 3) << ' '
                        << code.substr(3) << "? Confirm on the tablet too. Matches [y/N]: " << std::flush;
                    std::string answer;
                    std::getline(std::cin, answer);
                    return answer == "y" || answer == "Y";
                });
            if (!tablet)
            {
                std::cout << "Pairing was rejected.\n";
                return 1;
            }
            tablet->address = address;
            tablet->port = port;
            wifi::SavePairing(*tablet);
            std::cout << "Paired " << Narrow(tablet->name) << ", id " << Narrow(wifi::ToHex(tablet->id)) << ".\n";
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
            unsigned monitor = 0;
            std::uint16_t localPort = tabletdisplay::protocol::kPort;
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
                else if (option == "--monitor" && index + 1 < argc && command == "--virtual-stream")
                {
                    const auto parsed = std::stoul(argv[++index]);
                    if (parsed < 1 || parsed > tabletdisplay::ipc::kMaxMonitors)
                        throw std::runtime_error("--monitor must be 1, 2, 3 or 4");
                    monitor = parsed;
                }
                else if (option == "--port" && index + 1 < argc)
                {
                    const auto parsed = std::stoul(argv[++index]);
                    if (parsed < 1'024 || parsed > 65'535)
                        throw std::runtime_error("--port must be in 1024..65535");
                    localPort = static_cast<std::uint16_t>(parsed);
                }
                else if (option == "--fps" && index + 1 < argc && command == "--virtual-stream")
                {
                    const auto parsed = std::stoul(argv[++index]);
                    if (parsed != 60 && parsed != 30) throw std::runtime_error("--fps must be 60 or 30");
                    g_streamFps = static_cast<std::uint16_t>(parsed);
                }
                else if (option == "--wifi-tablet" && index + 1 < argc)
                {
                    g_wifiTablet = Widen(argv[++index]);
                }
                else if (option == "--wifi-address" && index + 1 < argc)
                {
                    g_wifiAddress = Widen(argv[++index]);
                }
                else if (option == "--ignore-decoder-limit" && command == "--virtual-stream")
                {
                    g_ignoreDecoderLimit = true;
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
                        "Expected [--serial SERIAL] [--seconds N] [--reconnect] [--scale N] [--fps N] [--ignore-decoder-limit] [--monitor N] "
                        "[--port PORT] [--stop-event NAME]");
                }
            }
            const auto mode = command == "--virtual-stream"
                ? RunMode::VirtualDisplay
                : (command == "--stream"
                    ? RunMode::Desktop
                    : (command == "--encoder-demo"
                        ? RunMode::EncoderDemo
                        : (command == "--demo" ? RunMode::Synthetic : RunMode::Connect)));
            if (reconnect)
                return ReconnectingVirtualDisplayCommand(serial, seconds, resolutionDivisor, monitor, localPort);
            return ConnectCommand(serial, mode, seconds, resolutionDivisor, monitor, localPort);
        }
        throw std::runtime_error("Unknown argument");
    }
    catch (const std::exception& exception)
    {
        std::cerr << "TabletDisplayHost: " << exception.what() << '\n';
        return 1;
    }
}
