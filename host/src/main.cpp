#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "DesktopCapture.h"
#include "MediaFoundationH264Encoder.h"
#include "protocol/Protocol.h"
#include "video/SyntheticH264.h"
#include "video/VideoProcessing.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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
        Desktop
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
        const auto frameDuration = std::chrono::microseconds(1'000'000 / fps);
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

        const auto frameDuration = std::chrono::microseconds(1'000'000 / fps);
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
        for (std::uint64_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
        {
            std::this_thread::sleep_until(nextFrameTime);
            const auto frame = capture.Capture();
            const auto nv12 = tabletdisplay::video::BgraToNv12(
                frame.bgra, frame.width, frame.height, frame.stride);
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
            std::cout << "TabletDisplayHost 0.4.0\n";
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
            command == "--encoder-demo" || command == "--stream")
        {
            std::string serial;
            unsigned seconds = 10;
            for (int index = 2; index < argc; ++index)
            {
                const std::string_view option = argv[index];
                if (option == "--serial" && index + 1 < argc)
                {
                    serial = argv[++index];
                }
                else if (option == "--seconds" && index + 1 < argc &&
                    (command == "--stream" || command == "--encoder-demo"))
                {
                    const auto parsed = std::stoul(argv[++index]);
                    if (parsed == 0 || parsed > 3'600) throw std::runtime_error("--seconds must be in 1..3600");
                    seconds = parsed;
                }
                else
                {
                    throw std::runtime_error("Expected [--serial SERIAL] [--seconds N]");
                }
            }
            const auto mode = command == "--stream"
                ? RunMode::Desktop
                : (command == "--encoder-demo"
                    ? RunMode::EncoderDemo
                    : (command == "--demo" ? RunMode::Synthetic : RunMode::Connect));
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
