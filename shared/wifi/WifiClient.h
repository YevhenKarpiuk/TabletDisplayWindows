#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>

#include "WifiCrypto.h"
#include "WifiPairings.h"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

// Windows side of the Wi-Fi transport, doc/PROTOCOL.md section 10.
namespace tabletdisplay::wifi
{
    // The byte stream protocol v1 runs over: plain TCP (USB through adb) or Wi-Fi records.
    class ByteStream
    {
    public:
        virtual ~ByteStream() = default;
        virtual void Send(ByteView bytes) = 0;
        // Bytes read (> 0), 0 when the peer closed, -1 when the socket receive timeout expired.
        // Throws on any other failure.
        [[nodiscard]] virtual int Receive(std::span<std::uint8_t> buffer) = 0;
    };

    class PlainStream final : public ByteStream
    {
    public:
        explicit PlainStream(SOCKET socket) noexcept : m_socket(socket) {}
        void Send(ByteView bytes) override;
        [[nodiscard]] int Receive(std::span<std::uint8_t> buffer) override;

    private:
        SOCKET m_socket;
    };

    // Encrypts every Send into records and decrypts received records; a record split by a
    // receive timeout is completed by the next Receive.
    class SecureStream final : public ByteStream
    {
    public:
        SecureStream(SOCKET socket, ByteView sendKey, ByteView receiveKey);
        void Send(ByteView bytes) override;
        [[nodiscard]] int Receive(std::span<std::uint8_t> buffer) override;

    private:
        SOCKET m_socket;
        RecordCipher m_sendCipher;
        RecordCipher m_receiveCipher;
        Bytes m_received;
        Bytes m_plaintext;
        std::size_t m_plaintextPosition{};
    };

    // Connects within timeoutMs; throws when the tablet does not answer.
    [[nodiscard]] SOCKET ConnectTcp(const std::wstring& address, std::uint16_t port, DWORD timeoutMs);

    // Pairing (section 10.3). confirm receives the six-digit code and the tablet name and returns
    // whether the user saw the same code on the tablet. Returns nothing when either side rejects.
    using ConfirmCode = std::function<bool(const std::string& code, const std::wstring& tabletName)>;
    [[nodiscard]] std::optional<PairedTablet> Pair(
        SOCKET socket,
        ByteView windowsId,
        const std::string& windowsName,
        const ConfirmCode& confirm);

    // Authenticated session (section 10.4); throws if the tablet is not the paired one.
    [[nodiscard]] std::unique_ptr<SecureStream> OpenSession(
        SOCKET socket,
        ByteView windowsId,
        const PairedTablet& tablet);
}
