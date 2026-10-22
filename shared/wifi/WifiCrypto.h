#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string_view>
#include <span>
#include <string>
#include <vector>

// Primitives and derivations of the Wi-Fi transport, doc/PROTOCOL.md section 10.
namespace tabletdisplay::wifi
{
    inline constexpr std::uint16_t kWifiPort = 27185;
    inline constexpr std::size_t kPublicKeySize = 65;
    inline constexpr std::size_t kIdSize = 16;
    inline constexpr std::size_t kPairingNonceSize = 16;
    inline constexpr std::size_t kSessionNonceSize = 32;
    inline constexpr std::size_t kMacSize = 32;
    inline constexpr std::size_t kKeySize = 32;
    inline constexpr std::size_t kTagSize = 16;
    inline constexpr std::size_t kMaxRecordPlaintext = 16U * 1024U * 1024U;

    using Bytes = std::vector<std::uint8_t>;
    using ByteView = std::span<const std::uint8_t>;

    [[nodiscard]] Bytes RandomBytes(std::size_t count);
    [[nodiscard]] Bytes Ascii(std::string_view text);
    [[nodiscard]] Bytes Sha256(std::initializer_list<ByteView> parts);
    [[nodiscard]] Bytes Hmac(ByteView key, std::initializer_list<ByteView> parts);

    // An ephemeral P-256 key pair for the pairing exchange.
    class EcdhKeyPair
    {
    public:
        EcdhKeyPair();
        // For tests: the private scalar and its uncompressed public point.
        EcdhKeyPair(ByteView privateScalar, ByteView publicKey);
        ~EcdhKeyPair();
        EcdhKeyPair(const EcdhKeyPair&) = delete;
        EcdhKeyPair& operator=(const EcdhKeyPair&) = delete;

        // 04 || X || Y.
        [[nodiscard]] const Bytes& PublicKey() const noexcept { return m_publicKey; }
        // X coordinate of the shared point, big-endian.
        [[nodiscard]] Bytes SharedSecret(ByteView peerPublicKey) const;

    private:
        void* m_key{};
        Bytes m_publicKey;
    };

    [[nodiscard]] Bytes Commitment(ByteView pkA, ByteView pkW, ByteView nA);
    [[nodiscard]] std::string ComparisonCode(ByteView pkW, ByteView pkA, ByteView nW, ByteView nA);
    [[nodiscard]] Bytes PairingKey(ByteView z, ByteView pkW, ByteView pkA, ByteView nW, ByteView nA);
    [[nodiscard]] Bytes ServerMac(ByteView k, ByteView idW, ByteView idA, ByteView sW, ByteView sA);
    [[nodiscard]] Bytes ClientMac(ByteView k, ByteView idW, ByteView idA, ByteView sW, ByteView sA);
    [[nodiscard]] Bytes WindowsToAndroidKey(ByteView k, ByteView sW, ByteView sA);
    [[nodiscard]] Bytes AndroidToWindowsKey(ByteView k, ByteView sW, ByteView sA);

    // AES-256-GCM records of one direction: u32 N || ciphertext || tag, AAD = N.
    class RecordCipher
    {
    public:
        explicit RecordCipher(ByteView key);
        ~RecordCipher();
        RecordCipher(const RecordCipher&) = delete;
        RecordCipher& operator=(const RecordCipher&) = delete;

        [[nodiscard]] Bytes Seal(ByteView plaintext);
        // body = ciphertext || tag of a record whose length field was body.size().
        [[nodiscard]] Bytes Open(ByteView body);
        void SetCounter(std::uint64_t counter) noexcept { m_counter = counter; }

    private:
        void* m_key{};
        std::uint64_t m_counter{};
    };
}
