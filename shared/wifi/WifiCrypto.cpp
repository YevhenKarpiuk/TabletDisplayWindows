#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include "WifiCrypto.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>

#pragma comment(lib, "bcrypt.lib")

namespace tabletdisplay::wifi
{
    namespace
    {
        void Check(NTSTATUS status, const char* operation)
        {
            if (!BCRYPT_SUCCESS(status))
            {
                char text[32]{};
                std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(status));
                throw std::runtime_error(std::string(operation) + " failed, NTSTATUS " + text);
            }
        }

        class Algorithm
        {
        public:
            Algorithm(const wchar_t* name, ULONG flags = 0)
            {
                Check(BCryptOpenAlgorithmProvider(&m_handle, name, nullptr, flags), "BCryptOpenAlgorithmProvider");
            }
            ~Algorithm() { BCryptCloseAlgorithmProvider(m_handle, 0); }
            Algorithm(const Algorithm&) = delete;
            Algorithm& operator=(const Algorithm&) = delete;
            [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return m_handle; }

        private:
            BCRYPT_ALG_HANDLE m_handle{};
        };

        // Algorithm handles are thread-safe and reused for the life of the process.
        BCRYPT_ALG_HANDLE EcdhAlgorithm()
        {
            static Algorithm algorithm(BCRYPT_ECDH_P256_ALGORITHM);
            return algorithm.get();
        }

        BCRYPT_ALG_HANDLE AesGcmAlgorithm()
        {
            static const auto algorithm = []
            {
                auto created = std::make_unique<Algorithm>(BCRYPT_AES_ALGORITHM);
                Check(BCryptSetProperty(
                    created->get(),
                    BCRYPT_CHAINING_MODE,
                    reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                    static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_GCM)),
                    0), "BCRYPT_CHAIN_MODE_GCM");
                return created;
            }();
            return algorithm->get();
        }

        Bytes Concatenate(std::initializer_list<ByteView> parts)
        {
            Bytes output;
            for (const auto part : parts) output.insert(output.end(), part.begin(), part.end());
            return output;
        }

        Bytes PublicBlobToPoint(const Bytes& blob)
        {
            const auto* header = reinterpret_cast<const BCRYPT_ECCKEY_BLOB*>(blob.data());
            if (blob.size() < sizeof(BCRYPT_ECCKEY_BLOB) + 64 || header->cbKey != 32)
                throw std::runtime_error("Unexpected ECDH public key blob");
            Bytes point{ 4 };
            point.insert(point.end(), blob.begin() + sizeof(BCRYPT_ECCKEY_BLOB),
                blob.begin() + sizeof(BCRYPT_ECCKEY_BLOB) + 64);
            return point;
        }

        Bytes PointToBlob(ByteView point, ULONG magic, ByteView privateScalar = {})
        {
            if (point.size() != kPublicKeySize || point[0] != 4)
                throw std::runtime_error("Invalid P-256 public key");
            Bytes blob(sizeof(BCRYPT_ECCKEY_BLOB));
            auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
            header->dwMagic = magic;
            header->cbKey = 32;
            blob.insert(blob.end(), point.begin() + 1, point.end());
            blob.insert(blob.end(), privateScalar.begin(), privateScalar.end());
            return blob;
        }

        std::array<std::uint8_t, 4> LittleEndian32(std::uint32_t value) noexcept
        {
            return { static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8U),
                static_cast<std::uint8_t>(value >> 16U), static_cast<std::uint8_t>(value >> 24U) };
        }
    }

    Bytes RandomBytes(std::size_t count)
    {
        Bytes bytes(count);
        Check(BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG), "BCryptGenRandom");
        return bytes;
    }

    Bytes Ascii(std::string_view text)
    {
        return Bytes(text.begin(), text.end());
    }

    Bytes Sha256(std::initializer_list<ByteView> parts)
    {
        const auto input = Concatenate(parts);
        Bytes digest(32);
        Check(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
            const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()),
            digest.data(), static_cast<ULONG>(digest.size())), "SHA-256");
        return digest;
    }

    Bytes Hmac(ByteView key, std::initializer_list<ByteView> parts)
    {
        const auto input = Concatenate(parts);
        Bytes mac(32);
        Check(BCryptHash(BCRYPT_HMAC_SHA256_ALG_HANDLE,
            const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()),
            const_cast<PUCHAR>(input.data()), static_cast<ULONG>(input.size()),
            mac.data(), static_cast<ULONG>(mac.size())), "HMAC-SHA256");
        return mac;
    }

    EcdhKeyPair::EcdhKeyPair()
    {
        BCRYPT_KEY_HANDLE key{};
        Check(BCryptGenerateKeyPair(EcdhAlgorithm(), &key, 256, 0), "BCryptGenerateKeyPair");
        m_key = key;
        Check(BCryptFinalizeKeyPair(key, 0), "BCryptFinalizeKeyPair");
        ULONG size{};
        Check(BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &size, 0), "BCryptExportKey");
        Bytes blob(size);
        Check(BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob.data(), size, &size, 0), "BCryptExportKey");
        m_publicKey = PublicBlobToPoint(blob);
    }

    EcdhKeyPair::EcdhKeyPair(ByteView privateScalar, ByteView publicKey)
    {
        if (privateScalar.size() != 32) throw std::runtime_error("Invalid P-256 private scalar");
        const auto blob = PointToBlob(publicKey, BCRYPT_ECDH_PRIVATE_P256_MAGIC, privateScalar);
        BCRYPT_KEY_HANDLE key{};
        Check(BCryptImportKeyPair(EcdhAlgorithm(), nullptr, BCRYPT_ECCPRIVATE_BLOB, &key,
            const_cast<PUCHAR>(blob.data()), static_cast<ULONG>(blob.size()), 0), "BCryptImportKeyPair");
        m_key = key;
        m_publicKey.assign(publicKey.begin(), publicKey.end());
    }

    EcdhKeyPair::~EcdhKeyPair()
    {
        if (m_key != nullptr) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(m_key));
    }

    Bytes EcdhKeyPair::SharedSecret(ByteView peerPublicKey) const
    {
        const auto blob = PointToBlob(peerPublicKey, BCRYPT_ECDH_PUBLIC_P256_MAGIC);
        BCRYPT_KEY_HANDLE peer{};
        Check(BCryptImportKeyPair(EcdhAlgorithm(), nullptr, BCRYPT_ECCPUBLIC_BLOB, &peer,
            const_cast<PUCHAR>(blob.data()), static_cast<ULONG>(blob.size()), 0), "Import peer key");
        BCRYPT_SECRET_HANDLE secret{};
        const auto agreement = BCryptSecretAgreement(static_cast<BCRYPT_KEY_HANDLE>(m_key), peer, &secret, 0);
        BCryptDestroyKey(peer);
        Check(agreement, "BCryptSecretAgreement");
        Bytes z(32);
        ULONG size{};
        const auto derive = BCryptDeriveKey(secret, BCRYPT_KDF_RAW_SECRET, nullptr, z.data(),
            static_cast<ULONG>(z.size()), &size, 0);
        BCryptDestroySecret(secret);
        Check(derive, "BCryptDeriveKey(RAW_SECRET)");
        // CNG returns the raw secret little-endian; the protocol uses the X coordinate big-endian.
        std::reverse(z.begin(), z.end());
        return z;
    }

    Bytes Commitment(ByteView pkA, ByteView pkW, ByteView nA)
    {
        const auto label = Ascii("TDW1 commit");
        return Sha256({ label, pkA, pkW, nA });
    }

    std::string ComparisonCode(ByteView pkW, ByteView pkA, ByteView nW, ByteView nA)
    {
        const auto label = Ascii("TDW1 sas");
        const auto digest = Sha256({ label, pkW, pkA, nW, nA });
        const auto value = (static_cast<std::uint32_t>(digest[0]) << 24U) |
            (static_cast<std::uint32_t>(digest[1]) << 16U) |
            (static_cast<std::uint32_t>(digest[2]) << 8U) | digest[3];
        char code[8]{};
        std::snprintf(code, sizeof(code), "%06u", static_cast<unsigned>(value % 1'000'000U));
        return code;
    }

    Bytes PairingKey(ByteView z, ByteView pkW, ByteView pkA, ByteView nW, ByteView nA)
    {
        const auto label = Ascii("TDW1 pair");
        return Hmac(z, { label, pkW, pkA, nW, nA });
    }

    Bytes ServerMac(ByteView k, ByteView idW, ByteView idA, ByteView sW, ByteView sA)
    {
        const auto label = Ascii("TDW1 server");
        return Hmac(k, { label, idW, idA, sW, sA });
    }

    Bytes ClientMac(ByteView k, ByteView idW, ByteView idA, ByteView sW, ByteView sA)
    {
        const auto label = Ascii("TDW1 client");
        return Hmac(k, { label, idW, idA, sW, sA });
    }

    Bytes WindowsToAndroidKey(ByteView k, ByteView sW, ByteView sA)
    {
        const auto label = Ascii("TDW1 w2a");
        return Hmac(k, { label, sW, sA });
    }

    Bytes AndroidToWindowsKey(ByteView k, ByteView sW, ByteView sA)
    {
        const auto label = Ascii("TDW1 a2w");
        return Hmac(k, { label, sW, sA });
    }

    RecordCipher::RecordCipher(ByteView key)
    {
        if (key.size() != kKeySize) throw std::runtime_error("Invalid record key");
        BCRYPT_KEY_HANDLE handle{};
        Check(BCryptGenerateSymmetricKey(AesGcmAlgorithm(), &handle, nullptr, 0,
            const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0), "BCryptGenerateSymmetricKey");
        m_key = handle;
    }

    RecordCipher::~RecordCipher()
    {
        if (m_key != nullptr) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(m_key));
    }

    Bytes RecordCipher::Seal(ByteView plaintext)
    {
        if (plaintext.empty() || plaintext.size() > kMaxRecordPlaintext)
            throw std::runtime_error("Invalid record size");
        if (m_counter == UINT64_MAX) throw std::runtime_error("Record counter overflow");
        const auto length = static_cast<std::uint32_t>(plaintext.size() + kTagSize);
        auto header = LittleEndian32(length);
        std::array<std::uint8_t, 12> nonce{};
        for (unsigned index = 0; index < 8; ++index)
            nonce[4 + index] = static_cast<std::uint8_t>(m_counter >> (8U * index));

        Bytes record(4 + plaintext.size() + kTagSize);
        std::copy(header.begin(), header.end(), record.begin());
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = nonce.data();
        info.cbNonce = static_cast<ULONG>(nonce.size());
        info.pbAuthData = header.data();
        info.cbAuthData = static_cast<ULONG>(header.size());
        info.pbTag = record.data() + 4 + plaintext.size();
        info.cbTag = static_cast<ULONG>(kTagSize);
        ULONG written{};
        Check(BCryptEncrypt(static_cast<BCRYPT_KEY_HANDLE>(m_key),
            const_cast<PUCHAR>(plaintext.data()), static_cast<ULONG>(plaintext.size()), &info,
            nullptr, 0, record.data() + 4, static_cast<ULONG>(plaintext.size()), &written, 0), "AES-GCM seal");
        ++m_counter;
        return record;
    }

    Bytes RecordCipher::Open(ByteView body)
    {
        if (body.size() <= kTagSize || body.size() > kMaxRecordPlaintext + kTagSize)
            throw std::runtime_error("Invalid Wi-Fi record length");
        if (m_counter == UINT64_MAX) throw std::runtime_error("Record counter overflow");
        auto header = LittleEndian32(static_cast<std::uint32_t>(body.size()));
        std::array<std::uint8_t, 12> nonce{};
        for (unsigned index = 0; index < 8; ++index)
            nonce[4 + index] = static_cast<std::uint8_t>(m_counter >> (8U * index));
        const auto cipherSize = body.size() - kTagSize;
        Bytes plaintext(cipherSize);
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = nonce.data();
        info.cbNonce = static_cast<ULONG>(nonce.size());
        info.pbAuthData = header.data();
        info.cbAuthData = static_cast<ULONG>(header.size());
        info.pbTag = const_cast<PUCHAR>(body.data() + cipherSize);
        info.cbTag = static_cast<ULONG>(kTagSize);
        ULONG written{};
        Check(BCryptDecrypt(static_cast<BCRYPT_KEY_HANDLE>(m_key),
            const_cast<PUCHAR>(body.data()), static_cast<ULONG>(cipherSize), &info,
            nullptr, 0, plaintext.data(), static_cast<ULONG>(cipherSize), &written, 0), "AES-GCM open");
        ++m_counter;
        return plaintext;
    }
}
