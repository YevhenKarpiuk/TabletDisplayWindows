#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "DesktopCapture.h"
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
            throw std::runtime_error("Truncated protocol payload");
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

    void ConfigureTabletDisplayMode(
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t frequency)
    {
        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        for (DWORD index = 0; EnumDisplayDevicesW(nullptr, index, &device, 0); ++index)
        {
            if (std::wstring_view(device.DeviceString) == L"TabletDisplay Virtual Display")
            {
                DEVMODEW mode{};
                mode.dmSize = sizeof(mode);
                if (!EnumDisplaySettingsExW(device.DeviceName, ENUM_CURRENT_SETTINGS, &mode, 0))
                    throw std::runtime_error("Cannot query the TabletDisplay mode");
                if (mode.dmPelsWidth == width && mode.dmPelsHeight == height &&
                    mode.dmDisplayFrequency == frequency)
                    return;

                mode.dmPelsWidth = width;
                mode.dmPelsHeight = height;
                mode.dmDisplayFrequency = frequency;
                mode.dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
                const auto testResult = ChangeDisplaySettingsExW(
                    device.DeviceName, &mode, nullptr, CDS_TEST, nullptr);
                if (testResult != DISP_CHANGE_SUCCESSFUL)
                    throw std::runtime_error("Windows rejected the requested TabletDisplay mode");
                const auto applyResult = ChangeDisplaySettingsExW(
                    device.DeviceName, &mode, nullptr, CDS_UPDATEREGISTRY, nullptr);
                if (applyResult != DISP_CHANGE_SUCCESSFUL)
                    throw std::runtime_error("Windows could not apply the requested TabletDisplay mode");
                std::cout << "Windows display mode: " << width << 'x' << height
                    << " @ " << frequency << " Hz\n";
                return;
            }
            device = {};
            device.cb = sizeof(device);
        }
        throw std::runtime_error("TabletDisplay virtual display was not found by Windows");
    }

    [[nodiscard]] DWORD TabletDisplayOrientation()
    {
        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        for (DWORD index = 0; EnumDisplayDevicesW(nullptr, index, &device, 0); ++index)
        {
            if (std::wstring_view(device.DeviceString) == L"TabletDisplay Virtual Display")
            {
                DEVMODEW mode{};
                mode.dmSize = sizeof(mode);
                if (!EnumDisplaySettingsExW(device.DeviceName, ENUM_CURRENT_SETTINGS, &mode, 0))
                    throw std::runtime_error("Cannot query the TabletDisplay orientation");
                if (mode.dmDisplayOrientation > DMDO_270)
                    throw std::runtime_error("Windows reported an invalid TabletDisplay orientation");
                return mode.dmDisplayOrientation;
            }
            device = {};
            device.cb = sizeof(device);
        }
        throw std::runtime_error("TabletDisplay virtual display was not found by Windows");
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
        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        for (DWORD index = 0; EnumDisplayDevicesW(nullptr, index, &device, 0); ++index)
        {
            if (std::wstring_view(device.DeviceString) == L"TabletDisplay Virtual Display")
            {
                DEVMODEW mode{};
                mode.dmSize = sizeof(mode);
                if (!EnumDisplaySettingsExW(device.DeviceName, ENUM_CURRENT_SETTINGS, &mode, 0))
                    throw std::runtime_error("Cannot query the TabletDisplay position");
                return { mode.dmPosition.x, mode.dmPosition.y, mode.dmPelsWidth, mode.dmPelsHeight };
            }
            device = {};
            device.cb = sizeof(device);
        }
        throw std::runtime_error("TabletDisplay virtual display was not found by Windows");
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
        const DisplayBounds& display,
        std::atomic_bool& primaryButtonDown)
    {
        if (ReadLe<std::uint32_t>(payload, 0) != 1 || ReadLe<std::uint32_t>(payload, 4) != 0)
            throw std::runtime_error("TOUCH targets an unexpected stream or pointer");

        const auto action = payload[8];
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
        else if (action != 2) throw std::runtime_error("TOUCH has an unknown action");
        if (SendInput(1, &input, sizeof(input)) != 1)
            throw std::runtime_error("Windows rejected touch mouse input");
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
        const std::wstring& arguments)
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

        std::string output;
        std::array<char, 4096> buffer{};
        DWORD count{};
        while (ReadFile(readPipe, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) && count != 0)
        {
            output.append(buffer.data(), count);
        }
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD exitCode{};
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(readPipe);
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
            if (match == devices.end()) throw std::runtime_error("Requested ADB device was not found");
            if (match->state != "device") throw std::runtime_error("Requested ADB device is " + match->state);
            return match->serial;
        }
        std::vector<std::string> ready;
        for (const auto& device : devices)
        {
            if (device.state == "device") ready.push_back(device.serial);
        }
        if (ready.empty()) throw std::runtime_error("No authorized ADB device is connected");
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
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
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
            if (sent == SOCKET_ERROR || sent == 0) throw std::runtime_error("TCP send failed");
            offset += static_cast<std::size_t>(sent);
        }
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
        AppendLe(payload, static_cast<std::uint16_t>(1'000));
        AppendLe(payload, static_cast<std::uint16_t>(5'000));
        return {
            { static_cast<std::uint16_t>(MessageType::Hello), Critical,
              static_cast<std::uint32_t>(payload.size()), 1, NowUs() },
            std::move(payload)
        };
    }

    [[nodiscard]] Message MakePing(std::uint64_t sequence, std::uint64_t token)
    {
        std::vector<std::uint8_t> payload;
        payload.reserve(16);
        AppendLe(payload, token);
        AppendLe(payload, NowUs());
        return {
            { static_cast<std::uint16_t>(MessageType::Ping), Critical,
              static_cast<std::uint32_t>(payload.size()), sequence, NowUs() },
            std::move(payload)
        };
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
        return {
            { static_cast<std::uint16_t>(MessageType::VideoConfig), Critical,
              static_cast<std::uint32_t>(payload.size()), sequence, NowUs() },
            std::move(payload)
        };
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
        return {
            { static_cast<std::uint16_t>(MessageType::VideoFrame), flags,
              static_cast<std::uint32_t>(payload.size()), sequence, NowUs() },
            std::move(payload)
        };
    }

    void SendMessage(SOCKET socket, const Message& message)
    {
        SendAll(socket, EncodeMessage(message.header, message.payload));
    }

    [[nodiscard]] std::uint64_t PerformHandshake(SOCKET socket)
    {
        const auto nonce = RandomNonce();
        SendMessage(socket, MakeHello(nonce));
        IncrementalDecoder decoder;
        std::array<std::uint8_t, 16 * 1024> buffer{};
        bool ackReceived{};
        bool displayReceived{};
        bool pingSent{};
        const auto pingToken = RandomNonce();

        while (true)
        {
            const auto count = recv(socket, reinterpret_cast<char*>(buffer.data()),
                static_cast<int>(buffer.size()), 0);
            if (count == 0) throw std::runtime_error("Android closed the handshake connection");
            if (count == SOCKET_ERROR) throw std::runtime_error("TCP receive timed out or failed");
            for (const auto& message : decoder.Feed(
                std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(count))))
            {
                const auto type = static_cast<MessageType>(message.header.messageType);
                if (type == MessageType::HelloAck && !ackReceived)
                {
                    const auto status = ReadLe<std::uint16_t>(message.payload, 0);
                    if (status != 0) throw std::runtime_error("Android rejected HELLO, status " + std::to_string(status));
                    if (ReadLe<std::uint64_t>(message.payload, 8) != nonce)
                        throw std::runtime_error("HELLO_ACK nonce mismatch");
                    const auto capabilities = ReadLe<std::uint64_t>(message.payload, 24);
                    if ((capabilities & kRequiredCapabilities) != kRequiredCapabilities ||
                        (capabilities & kAvcProfiles) == 0)
                        throw std::runtime_error("HELLO_ACK is missing an MVP capability");
                    ackReceived = true;
                }
                else if (type == MessageType::DisplayInfo && ackReceived && !displayReceived)
                {
                    std::cout
                        << "Android display: " << ReadLe<std::uint32_t>(message.payload, 0)
                        << 'x' << ReadLe<std::uint32_t>(message.payload, 4)
                        << " @ " << ReadLe<std::uint32_t>(message.payload, 24) << " FPS\n";
                    displayReceived = true;
                }
                else if (type == MessageType::Pong && pingSent)
                {
                    if (ReadLe<std::uint64_t>(message.payload, 0) != pingToken)
                        throw std::runtime_error("PONG token mismatch");
                    std::cout << "Protocol handshake and PING/PONG succeeded.\n";
                    return 3;
                }
                else
                {
                    throw std::runtime_error("Unexpected Android-to-Windows message during handshake");
                }
            }
            if (ackReceived && displayReceived && !pingSent)
            {
                SendMessage(socket, MakePing(2, pingToken));
                pingSent = true;
            }
        }
    }

    void RunSyntheticDemo(SOCKET socket, std::uint64_t nextSequence)
    {
        const auto stream = tabletdisplay::video::BuildSyntheticColorBars();
        std::cout
            << "Synthetic H.264: " << stream.width << 'x' << stream.height
            << ", IDR " << stream.idrAccessUnit.size() << " bytes\n";
        SendMessage(socket, MakeVideoConfig(
            nextSequence++, stream.width, stream.height, stream.fps,
            stream.profileIdc, stream.levelIdc, 5'000'000, stream.codecSpecificData));
        SendMessage(socket, MakeVideoFrame(
            nextSequence++, 1, 0, stream.idrAccessUnit, true, true));
        std::cout << "Color bars sent; keeping the demo session alive for 6 seconds.\n";
        for (unsigned second = 0; second < 6; ++second)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            SendMessage(socket, MakePing(nextSequence++, RandomNonce()));
        }
        std::cout << "Synthetic video demo completed.\n";
    }

    void RunEncoderDemo(SOCKET socket, std::uint64_t nextSequence, unsigned seconds)
    {
        constexpr std::uint16_t width = 1280;
        constexpr std::uint16_t height = 800;
        constexpr std::uint16_t fps = 30;
        constexpr std::uint32_t bitrate = 5'000'000;
        tabletdisplay::host::MediaFoundationH264Encoder encoder({
            width, height, fps, bitrate, false });
        std::cout << "Media Foundation encoder demo: " << encoder.Name() << '\n';

        std::vector<std::uint8_t> nv12(
            static_cast<std::size_t>(width) * height * 3U / 2U,
            128);
        std::uint64_t frameId = 1;
        std::uint64_t bytesSent{};
        bool configSent{};
        bool firstFrame = true;
        const auto sendEncoded = [&](const tabletdisplay::host::EncodedAccessUnit& encoded)
        {
            if (!configSent)
            {
                const auto* config = encoder.CodecConfig();
                if (config == nullptr) throw std::runtime_error("H.264 encoder produced no SPS/PPS");
                SendMessage(socket, MakeVideoConfig(
                    nextSequence++, width, height, fps,
                    config->profileIdc == 0 ? 66 : config->profileIdc,
                    config->levelIdc == 0 ? 40 : config->levelIdc,
                    bitrate,
                    config->annexB));
                configSent = true;
            }
            SendMessage(socket, MakeVideoFrame(
                nextSequence++,
                frameId++,
                encoded.presentationTimeUs,
                encoded.bytes,
                encoded.keyFrame,
                firstFrame));
            bytesSent += encoded.bytes.size();
            firstFrame = false;
        };

        const auto frameCount = static_cast<std::uint64_t>(seconds) * fps;
        auto nextFrameTime = std::chrono::steady_clock::now();
        const auto frameDuration = std::chrono::nanoseconds(1'000'000'000ULL / fps);
        for (std::uint64_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
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
                sendEncoded(encoded);
            nextFrameTime += frameDuration;
            std::this_thread::sleep_until(nextFrameTime);
        }
        for (const auto& encoded : encoder.Drain()) sendEncoded(encoded);
        if (!configSent) throw std::runtime_error("H.264 encoder produced no frames");
        SendMessage(socket, MakeVideoFrame(
            nextSequence++,
            frameId,
            static_cast<std::uint64_t>(seconds) * 1'000'000ULL,
            {},
            false,
            false,
            true));
        std::cout
            << "Encoder demo completed: " << (frameId - 1) << " frames, "
            << bytesSent << " encoded bytes.\n";
    }

    void RunDesktopStream(SOCKET socket, std::uint64_t nextSequence, unsigned seconds)
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

        const auto frameDuration = std::chrono::nanoseconds(1'000'000'000ULL / fps);
        auto nextFrameTime = std::chrono::steady_clock::now();
        std::uint64_t frameId = 1;
        std::uint64_t bytesSent{};
        bool configSent{};
        bool firstFrame = true;

        const auto sendEncoded = [&](const tabletdisplay::host::EncodedAccessUnit& encoded)
        {
            if (!configSent)
            {
                const auto* config = encoder.CodecConfig();
                if (config == nullptr) throw std::runtime_error("H.264 encoder produced no SPS/PPS");
                SendMessage(socket, MakeVideoConfig(
                    nextSequence++, width, height, fps,
                    config->profileIdc == 0 ? 66 : config->profileIdc,
                    config->levelIdc == 0 ? 40 : config->levelIdc,
                    bitrate,
                    config->annexB));
                configSent = true;
            }
            if (firstFrame && !encoded.keyFrame)
                throw std::runtime_error("H.264 encoder did not produce an initial IDR");
            SendMessage(socket, MakeVideoFrame(
                nextSequence++,
                frameId++,
                encoded.presentationTimeUs,
                encoded.bytes,
                encoded.keyFrame,
                firstFrame));
            bytesSent += encoded.bytes.size();
            firstFrame = false;
        };

        const auto frameCount = static_cast<std::uint64_t>(seconds) * fps;
        std::vector<std::uint8_t> nv12;
        for (std::uint64_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
        {
            std::this_thread::sleep_until(nextFrameTime);
            const auto frame = capture.Capture();
            tabletdisplay::video::BgraToNv12(
                frame.bgra, frame.width, frame.height, frame.stride, nv12);
            const auto ptsUs = frameIndex * 1'000'000ULL / fps;
            for (const auto& encoded : encoder.Encode(nv12, ptsUs, frameIndex == 0))
                sendEncoded(encoded);
            nextFrameTime += frameDuration;
        }
        for (const auto& encoded : encoder.Drain()) sendEncoded(encoded);
        if (!configSent) throw std::runtime_error("H.264 encoder produced no frames");
        SendMessage(socket, MakeVideoFrame(
            nextSequence++,
            frameId,
            static_cast<std::uint64_t>(seconds) * 1'000'000ULL,
            {},
            false,
            false,
            true));
        std::cout
            << "Desktop stream completed: " << (frameId - 1) << " frames, "
            << bytesSent << " encoded bytes.\n";
    }

    void RunVirtualDisplayStream(SOCKET socket, std::uint64_t nextSequence, unsigned seconds)
    {
        const auto orientation = TabletDisplayOrientation();
        const bool portrait = orientation == DMDO_90 || orientation == DMDO_270;
        const std::uint16_t width = portrait ? 800 : 1280;
        const std::uint16_t height = portrait ? 1280 : 800;
        const std::uint16_t rawWidth = portrait ? height : width;
        const std::uint16_t rawHeight = portrait ? width : height;
        constexpr std::uint16_t fps = 60;
        constexpr std::uint32_t bitrate = 8'000'000;
        ConfigureTabletDisplayMode(width, height, fps);
        tabletdisplay::host::IddFrameCapture capture;
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
        std::cout
            << "IddCx capture: virtual display -> " << width << 'x' << height
            << " @ " << fps << " FPS\n"
            << "Media Foundation encoder: " << encoder.Name()
            << (encoder.IsHardware() ? " (hardware)" : " (software fallback)") << '\n'
            << "Driver/Host frame transport: "
            << (sharedTransport ? "shared D3D11 texture (zero-copy)" : "CPU BGRA IOCTL fallback")
            << '\n';

        const auto displayBounds = FindTabletDisplayBounds();
        const DWORD receiveTimeoutMs = 250;
        if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                reinterpret_cast<const char*>(&receiveTimeoutMs), sizeof(receiveTimeoutMs)) != 0)
            throw std::runtime_error("Cannot configure the touch receiver timeout");
        std::atomic_bool forceKeyFrame{};
        std::atomic_uint64_t receivedTouches{};
        std::atomic_bool primaryButtonDown{};
        [[maybe_unused]] TabletMouseButtonGuard mouseButtonGuard(primaryButtonDown);
        std::mutex receiverFailureMutex;
        std::exception_ptr receiverFailure;
        std::jthread touchReceiver([&](std::stop_token stopToken)
        {
            try
            {
                IncrementalDecoder decoder(kMaxPayloadSize, false);
                std::array<std::uint8_t, 16 * 1024> buffer{};
                std::uint64_t expectedSequence = 4;
                while (!stopToken.stop_requested())
                {
                    const auto count = recv(socket, reinterpret_cast<char*>(buffer.data()),
                        static_cast<int>(buffer.size()), 0);
                    if (count == SOCKET_ERROR)
                    {
                        if (WSAGetLastError() == WSAETIMEDOUT) continue;
                        if (stopToken.stop_requested()) return;
                        throw std::runtime_error("Android-to-Windows receive failed");
                    }
                    if (count == 0)
                    {
                        if (stopToken.stop_requested()) return;
                        throw std::runtime_error("Android closed the streaming connection");
                    }
                    for (const auto& message : decoder.Feed(
                            std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(count))))
                    {
                        if (message.header.sequence != expectedSequence++)
                            throw std::runtime_error("Android-to-Windows message sequence gap");
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
                        else if (type == MessageType::Error)
                        {
                            throw std::runtime_error("Android reported a streaming error");
                        }
                        else
                        {
                            throw std::runtime_error("Unexpected Android-to-Windows streaming message");
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

        std::uint64_t frameId = 1;
        std::uint64_t bytesSent{};
        bool configSent{};
        bool firstFrame = true;
        const auto sendEncoded = [&](const tabletdisplay::host::EncodedAccessUnit& encoded)
        {
            if (!configSent)
            {
                const auto* config = encoder.CodecConfig();
                if (config == nullptr) throw std::runtime_error("H.264 encoder produced no SPS/PPS");
                SendMessage(socket, MakeVideoConfig(
                    nextSequence++, width, height, fps,
                    config->profileIdc == 0 ? 66 : config->profileIdc,
                    config->levelIdc == 0 ? 40 : config->levelIdc,
                    bitrate,
                    config->annexB));
                configSent = true;
            }
            if (firstFrame && !encoded.keyFrame)
                throw std::runtime_error("H.264 encoder did not produce an initial IDR");
            SendMessage(socket, MakeVideoFrame(
                nextSequence++,
                frameId++,
                encoded.presentationTimeUs,
                encoded.bytes,
                encoded.keyFrame,
                firstFrame));
            bytesSent += encoded.bytes.size();
            firstFrame = false;
        };

        const auto frameDuration = std::chrono::nanoseconds(1'000'000'000ULL / fps);
        using Clock = std::chrono::steady_clock;
        const auto streamStarted = Clock::now();
        const auto frameCount = static_cast<std::uint64_t>(seconds) * fps;

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
                    if (frameIndex != 0)
                    {
                        const auto captureStarted = Clock::now();
                        if (sharedTransport)
                        {
                            slot.newSharedFrame = capture.CaptureShared(preparedShared, 0);
                            if (slot.newSharedFrame &&
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
                    else
                    {
                        slot.newSharedFrame = sharedTransport;
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
        try
        {
            for (std::uint64_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
            {
                checkReceiver();
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
                const auto requestKeyFrame =
                    frameIndex == 0 || forceKeyFrame.exchange(false);
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
                    sendEncoded(encoded);
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
            producer.join();
            checkReceiver();
        }
        catch (...)
        {
            {
                std::lock_guard lock(slot.mutex);
                slot.stop = true;
                slot.occupied = false;
            }
            slot.empty.notify_all();
            slot.ready.notify_all();
            if (producer.joinable()) producer.join();
            throw;
        }
        if (submittedFrames != frameCount)
            throw std::runtime_error("The frame preparation pipeline stopped before completing the stream");

        const auto streamElapsed = Clock::now() - streamStarted;
        for (const auto& encoded : encoder.Drain()) sendEncoded(encoded);
        if (!configSent) throw std::runtime_error("H.264 encoder produced no frames");
        SendMessage(socket, MakeVideoFrame(
            nextSequence++,
            frameId,
            static_cast<std::uint64_t>(seconds) * 1'000'000ULL,
            {},
            false,
            false,
            true));
        touchReceiver.request_stop();
        touchReceiver.join();
        checkReceiver();
        std::cout
            << "Virtual-display stream completed: " << (frameId - 1) << " frames, "
            << bytesSent << " encoded bytes, " << receivedTouches.load() << " touch events.\n";
        const auto averageMilliseconds = [frameCount](std::chrono::nanoseconds duration)
        {
            return std::chrono::duration<double, std::milli>(duration).count() /
                static_cast<double>(frameCount);
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
            << (frameCount > 0 ? frameCount - 1 : 0) << ".\n";
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
            << "  TabletDisplayHost --version\n";
    }

    int ConnectCommand(
        const std::string& requestedSerial,
        RunMode mode,
        unsigned streamSeconds)
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
            const auto nextSequence = PerformHandshake(socket.get());
            if (mode == RunMode::Synthetic)
            {
                RunSyntheticDemo(socket.get(), nextSequence);
            }
            else if (mode == RunMode::EncoderDemo)
            {
                RunEncoderDemo(socket.get(), nextSequence, streamSeconds);
            }
            else if (mode == RunMode::Desktop)
            {
                RunDesktopStream(socket.get(), nextSequence, streamSeconds);
            }
            else if (mode == RunMode::VirtualDisplay)
            {
                RunVirtualDisplayStream(socket.get(), nextSequence, streamSeconds);
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
                (void)RunProcess(adb, selector + L"forward --remove tcp:27183");
            }
            throw;
        }
        return 0;
    }

    int ReconnectingVirtualDisplayCommand(
        const std::string& requestedSerial,
        unsigned totalSeconds)
    {
        using Clock = std::chrono::steady_clock;
        const auto deadline = Clock::now() + std::chrono::seconds(totalSeconds);
        unsigned reconnectCount{};
        while (Clock::now() < deadline)
        {
            const auto remaining = deadline - Clock::now();
            const auto wholeSeconds = std::chrono::duration_cast<std::chrono::seconds>(remaining);
            const auto attemptSeconds = static_cast<unsigned>(wholeSeconds.count()) +
                (remaining > wholeSeconds ? 1U : 0U);
            try
            {
                ConnectCommand(requestedSerial, RunMode::VirtualDisplay, attemptSeconds);
                std::cout << "Continuous virtual-display stream completed after "
                    << reconnectCount << " reconnect(s).\n";
                return 0;
            }
            catch (const std::exception& exception)
            {
                if (Clock::now() >= deadline) break;
                ++reconnectCount;
                std::cerr << "Virtual-display connection lost: " << exception.what()
                    << ". Reconnect attempt " << reconnectCount << " will start in 1 second.\n";
                std::this_thread::sleep_until(std::min(deadline, Clock::now() + std::chrono::seconds(1)));
            }
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
            std::cout << "TabletDisplayHost 0.1.1\n";
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
                    if (parsed == 0 || parsed > 3'600) throw std::runtime_error("--seconds must be in 1..3600");
                    seconds = parsed;
                }
                else if (option == "--reconnect" && command == "--virtual-stream")
                {
                    reconnect = true;
                }
                else
                {
                    throw std::runtime_error("Expected [--serial SERIAL] [--seconds N] [--reconnect]");
                }
            }
            const auto mode = command == "--virtual-stream"
                ? RunMode::VirtualDisplay
                : (command == "--stream"
                    ? RunMode::Desktop
                    : (command == "--encoder-demo"
                        ? RunMode::EncoderDemo
                        : (command == "--demo" ? RunMode::Synthetic : RunMode::Connect)));
            if (reconnect) return ReconnectingVirtualDisplayCommand(serial, seconds);
            return ConnectCommand(serial, mode, seconds);
        }
        throw std::runtime_error("Unknown argument");
    }
    catch (const std::exception& exception)
    {
        std::cerr << "TabletDisplayHost: " << exception.what() << '\n';
        return 1;
    }
}
