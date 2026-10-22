#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "WifiClient.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace tabletdisplay::wifi
{
    namespace
    {
        constexpr std::uint8_t kPairHello = 0x01;
        constexpr std::uint8_t kPairReply = 0x02;
        constexpr std::uint8_t kPairNonceW = 0x03;
        constexpr std::uint8_t kPairNonceA = 0x04;
        constexpr std::uint8_t kPairDecision = 0x05;
        constexpr std::uint8_t kSessionHello = 0x10;
        constexpr std::uint8_t kSessionReply = 0x11;
        constexpr std::uint8_t kSessionProof = 0x12;
        constexpr std::uint32_t kMaxFrame = 65'536;
        // The user compares the code on both screens; the tablet waits 120 s for its user.
        constexpr DWORD kPairingTimeoutMs = 130'000;

        void SendExactly(SOCKET socket, ByteView bytes)
        {
            std::size_t offset{};
            while (offset < bytes.size())
            {
                const auto chunk = static_cast<int>(std::min<std::size_t>(
                    bytes.size() - offset, static_cast<std::size_t>(std::numeric_limits<int>::max())));
                const auto sent = send(socket, reinterpret_cast<const char*>(bytes.data() + offset), chunk, 0);
                if (sent == SOCKET_ERROR || sent == 0) throw std::runtime_error("Wi-Fi send failed");
                offset += static_cast<std::size_t>(sent);
            }
        }

        void ReceiveExactly(SOCKET socket, std::uint8_t* target, std::size_t size)
        {
            std::size_t offset{};
            while (offset < size)
            {
                const auto received = recv(socket, reinterpret_cast<char*>(target + offset),
                    static_cast<int>(size - offset), 0);
                if (received == 0) throw std::runtime_error("The tablet closed the Wi-Fi connection");
                if (received == SOCKET_ERROR)
                {
                    throw std::runtime_error(WSAGetLastError() == WSAETIMEDOUT
                        ? "The tablet did not answer over Wi-Fi in time"
                        : "Wi-Fi receive failed");
                }
                offset += static_cast<std::size_t>(received);
            }
        }

        std::uint32_t ReadLe32(const std::uint8_t* bytes) noexcept
        {
            return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
                (static_cast<std::uint32_t>(bytes[2]) << 16U) | (static_cast<std::uint32_t>(bytes[3]) << 24U);
        }

        void WriteFrame(SOCKET socket, const Bytes& body)
        {
            Bytes frame{
                static_cast<std::uint8_t>(body.size()), static_cast<std::uint8_t>(body.size() >> 8U),
                static_cast<std::uint8_t>(body.size() >> 16U), static_cast<std::uint8_t>(body.size() >> 24U) };
            frame.insert(frame.end(), body.begin(), body.end());
            SendExactly(socket, frame);
        }

        Bytes ReadFrame(SOCKET socket)
        {
            std::uint8_t header[4]{};
            ReceiveExactly(socket, header, sizeof(header));
            const auto length = ReadLe32(header);
            if (length < 1 || length > kMaxFrame) throw std::runtime_error("Invalid Wi-Fi handshake length");
            Bytes body(length);
            ReceiveExactly(socket, body.data(), body.size());
            return body;
        }

        class FieldReader
        {
        public:
            explicit FieldReader(const Bytes& bytes) noexcept : m_bytes(bytes) {}

            void ExpectType(std::uint8_t type)
            {
                if (Take(1)[0] != type) throw std::runtime_error("Unexpected Wi-Fi handshake message");
            }

            Bytes Take(std::size_t count)
            {
                if (m_offset + count > m_bytes.size()) throw std::runtime_error("Truncated Wi-Fi handshake message");
                Bytes value(m_bytes.begin() + static_cast<std::ptrdiff_t>(m_offset),
                    m_bytes.begin() + static_cast<std::ptrdiff_t>(m_offset + count));
                m_offset += count;
                return value;
            }

            std::wstring Text()
            {
                const auto bytes = Take(Take(1)[0]);
                if (bytes.empty()) return {};
                const auto size = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(bytes.data()),
                    static_cast<int>(bytes.size()), nullptr, 0);
                std::wstring text(static_cast<std::size_t>(std::max(size, 0)), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(bytes.data()),
                    static_cast<int>(bytes.size()), text.data(), size);
                return text;
            }

            void End() const
            {
                if (m_offset != m_bytes.size()) throw std::runtime_error("Oversized Wi-Fi handshake message");
            }

        private:
            const Bytes& m_bytes;
            std::size_t m_offset{};
        };

        void SetReceiveTimeout(SOCKET socket, DWORD timeoutMs)
        {
            (void)setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
        }

        DWORD ReceiveTimeout(SOCKET socket)
        {
            DWORD timeout{};
            int size = sizeof(timeout);
            (void)getsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), &size);
            return timeout;
        }

        Bytes Concat(std::initializer_list<ByteView> parts)
        {
            Bytes output;
            for (const auto part : parts) output.insert(output.end(), part.begin(), part.end());
            return output;
        }
    }

    void PlainStream::Send(ByteView bytes)
    {
        SendExactly(m_socket, bytes);
    }

    int PlainStream::Receive(std::span<std::uint8_t> buffer)
    {
        const auto received = recv(m_socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
        if (received == SOCKET_ERROR)
        {
            if (WSAGetLastError() == WSAETIMEDOUT) return -1;
            throw std::runtime_error("TCP receive failed");
        }
        return received;
    }

    SecureStream::SecureStream(SOCKET socket, ByteView sendKey, ByteView receiveKey)
        : m_socket(socket), m_sendCipher(sendKey), m_receiveCipher(receiveKey)
    {
    }

    void SecureStream::Send(ByteView bytes)
    {
        std::size_t offset{};
        while (offset < bytes.size())
        {
            const auto length = std::min(bytes.size() - offset, kMaxRecordPlaintext);
            SendExactly(m_socket, m_sendCipher.Seal(bytes.subspan(offset, length)));
            offset += length;
        }
    }

    int SecureStream::Receive(std::span<std::uint8_t> buffer)
    {
        while (m_plaintextPosition == m_plaintext.size())
        {
            // A complete record already buffered?
            if (m_received.size() >= 4)
            {
                const auto length = ReadLe32(m_received.data());
                if (length <= kTagSize || length > kMaxRecordPlaintext + kTagSize)
                    throw std::runtime_error("Invalid Wi-Fi record length");
                if (m_received.size() >= 4U + length)
                {
                    m_plaintext = m_receiveCipher.Open(ByteView(m_received.data() + 4, length));
                    m_plaintextPosition = 0;
                    m_received.erase(m_received.begin(), m_received.begin() + 4 + length);
                    continue;
                }
            }
            std::uint8_t chunk[64 * 1024];
            const auto received = recv(m_socket, reinterpret_cast<char*>(chunk), sizeof(chunk), 0);
            if (received == 0) return 0;
            if (received == SOCKET_ERROR)
            {
                if (WSAGetLastError() == WSAETIMEDOUT) return -1;
                throw std::runtime_error("Wi-Fi receive failed");
            }
            m_received.insert(m_received.end(), chunk, chunk + received);
        }
        const auto count = std::min(buffer.size(), m_plaintext.size() - m_plaintextPosition);
        std::copy_n(m_plaintext.begin() + static_cast<std::ptrdiff_t>(m_plaintextPosition), count, buffer.begin());
        m_plaintextPosition += count;
        return static_cast<int>(count);
    }

    SOCKET ConnectTcp(const std::wstring& address, std::uint16_t port, DWORD timeoutMs)
    {
        ADDRINFOW hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        ADDRINFOW* resolved{};
        if (GetAddrInfoW(address.c_str(), std::to_wstring(port).c_str(), &hints, &resolved) != 0 || resolved == nullptr)
            throw std::runtime_error("Cannot resolve the tablet Wi-Fi address");
        const auto socketHandle = socket(resolved->ai_family, resolved->ai_socktype, resolved->ai_protocol);
        if (socketHandle == INVALID_SOCKET)
        {
            FreeAddrInfoW(resolved);
            throw std::runtime_error("socket() failed");
        }
        u_long nonBlocking = 1;
        (void)ioctlsocket(socketHandle, FIONBIO, &nonBlocking);
        const auto started = connect(socketHandle, resolved->ai_addr, static_cast<int>(resolved->ai_addrlen));
        FreeAddrInfoW(resolved);
        bool connected = started == 0;
        if (!connected && WSAGetLastError() == WSAEWOULDBLOCK)
        {
            fd_set writable{};
            fd_set failed{};
            FD_SET(socketHandle, &writable);
            FD_SET(socketHandle, &failed);
            timeval timeout{ static_cast<long>(timeoutMs / 1'000), static_cast<long>((timeoutMs % 1'000) * 1'000) };
            connected = select(0, nullptr, &writable, &failed, &timeout) == 1 && FD_ISSET(socketHandle, &writable);
        }
        if (!connected)
        {
            closesocket(socketHandle);
            throw std::runtime_error("The tablet does not answer over Wi-Fi");
        }
        nonBlocking = 0;
        (void)ioctlsocket(socketHandle, FIONBIO, &nonBlocking);
        const BOOL noDelay = TRUE;
        (void)setsockopt(socketHandle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        return socketHandle;
    }

    std::optional<PairedTablet> Pair(
        SOCKET socket,
        ByteView windowsId,
        const std::string& windowsName,
        const ConfirmCode& confirm)
    {
        const auto previousTimeout = ReceiveTimeout(socket);
        SetReceiveTimeout(socket, kPairingTimeoutMs);
        EcdhKeyPair keyPair;
        const auto& pkW = keyPair.PublicKey();
        const auto name = windowsName.substr(0, 255);
        Bytes hello = Ascii("TDW1");
        hello.push_back(kPairHello);
        hello.insert(hello.end(), windowsId.begin(), windowsId.end());
        hello.push_back(static_cast<std::uint8_t>(name.size()));
        hello.insert(hello.end(), name.begin(), name.end());
        hello.insert(hello.end(), pkW.begin(), pkW.end());
        WriteFrame(socket, hello);

        const auto reply = ReadFrame(socket);
        FieldReader replyReader(reply);
        replyReader.ExpectType(kPairReply);
        PairedTablet tablet;
        tablet.id = replyReader.Take(kIdSize);
        tablet.name = replyReader.Text();
        const auto pkA = replyReader.Take(kPublicKeySize);
        const auto commitment = replyReader.Take(kMacSize);
        replyReader.End();

        const auto nW = RandomBytes(kPairingNonceSize);
        WriteFrame(socket, Concat({ Bytes{ kPairNonceW }, nW }));
        const auto nonceFrame = ReadFrame(socket);
        FieldReader nonceReader(nonceFrame);
        nonceReader.ExpectType(kPairNonceA);
        const auto nA = nonceReader.Take(kPairingNonceSize);
        nonceReader.End();
        if (Commitment(pkA, pkW, nA) != commitment)
            throw std::runtime_error("The tablet's pairing commitment does not match; pairing aborted");

        const auto code = ComparisonCode(pkW, pkA, nW, nA);
        const bool accepted = confirm(code, tablet.name);
        WriteFrame(socket, Bytes{ kPairDecision, static_cast<std::uint8_t>(accepted ? 1 : 0) });
        const auto decision = ReadFrame(socket);
        FieldReader decisionReader(decision);
        decisionReader.ExpectType(kPairDecision);
        const bool tabletAccepted = decisionReader.Take(1)[0] == 1;
        decisionReader.End();
        SetReceiveTimeout(socket, previousTimeout);
        if (!accepted || !tabletAccepted) return std::nullopt;

        tablet.key = PairingKey(keyPair.SharedSecret(pkA), pkW, pkA, nW, nA);
        return tablet;
    }

    std::unique_ptr<SecureStream> OpenSession(SOCKET socket, ByteView windowsId, const PairedTablet& tablet)
    {
        const auto sW = RandomBytes(kSessionNonceSize);
        Bytes hello = Ascii("TDW1");
        hello.push_back(kSessionHello);
        hello.insert(hello.end(), windowsId.begin(), windowsId.end());
        hello.insert(hello.end(), sW.begin(), sW.end());
        WriteFrame(socket, hello);

        const auto reply = ReadFrame(socket);
        FieldReader reader(reply);
        reader.ExpectType(kSessionReply);
        if (reader.Take(1)[0] != 0)
            throw std::runtime_error("The tablet does not know this computer; pair it again from the tray");
        const auto idA = reader.Take(kIdSize);
        const auto sA = reader.Take(kSessionNonceSize);
        const auto macA = reader.Take(kMacSize);
        reader.End();
        if (idA != tablet.id || ServerMac(tablet.key, windowsId, idA, sW, sA) != macA)
            throw std::runtime_error("The Wi-Fi tablet failed authentication");
        WriteFrame(socket, Concat({ Bytes{ kSessionProof }, ClientMac(tablet.key, windowsId, idA, sW, sA) }));
        return std::make_unique<SecureStream>(
            socket,
            WindowsToAndroidKey(tablet.key, sW, sA),
            AndroidToWindowsKey(tablet.key, sW, sA));
    }
}
