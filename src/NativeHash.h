#pragma once

// Compact self-contained SHA-256 (hex digest) shared by the SSH session
// (host key fingerprints) and the update installer (artifact verification).
// Verified against Windows Get-FileHash and FIPS test vectors.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

namespace NativeHash {

inline std::string hexEncode(const unsigned char *data, std::size_t size)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (std::size_t index = 0; index < size; ++index) {
        result.push_back(hex[data[index] >> 4]);
        result.push_back(hex[data[index] & 0x0f]);
    }
    return result;
}

// SHA-256 digest words are big-endian: each 32-bit word must be emitted
// from its most significant byte, regardless of host byte order.
inline std::string digestHex(const std::uint32_t state[8])
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (int index = 0; index < 8; ++index) {
        const std::uint32_t word = state[index];
        for (int shift = 28; shift >= 0; shift -= 4)
            result.push_back(hex[(word >> shift) & 0x0f]);
    }
    return result;
}

// Kept as a portable fallback for non-Windows builds and for the unlikely
// case that the platform crypto provider cannot be opened.
inline std::string sha256HexPortable(const unsigned char *data, std::size_t size)
{
    std::uint32_t state[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    const auto rotateRight = [](std::uint32_t value, unsigned int bits) {
        return (value >> bits) | (value << (32 - bits));
    };
    auto processBlock = [&state, &rotateRight](const unsigned char block[64]) {
        static constexpr std::uint32_t k[] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
            0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
            0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
            0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
            0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
            0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
            0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
            0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
        std::uint32_t w[64];
        for (int index = 0; index < 16; ++index)
            w[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24)
                | (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16)
                | (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8)
                | static_cast<std::uint32_t>(block[index * 4 + 3]);
        for (int index = 16; index < 64; ++index) {
            const std::uint32_t s0 = rotateRight(w[index - 15], 7)
                ^ rotateRight(w[index - 15], 18) ^ (w[index - 15] >> 3);
            const std::uint32_t s1 = rotateRight(w[index - 2], 17)
                ^ rotateRight(w[index - 2], 19) ^ (w[index - 2] >> 10);
            w[index] = w[index - 16] + s0 + w[index - 7] + s1;
        }
        std::uint32_t a = state[0], b = state[1], c = state[2];
        std::uint32_t d = state[3], e = state[4], f = state[5];
        std::uint32_t g = state[6], h = state[7];
        for (int index = 0; index < 64; ++index) {
            const std::uint32_t s1 = rotateRight(e, 6)
                ^ rotateRight(e, 11) ^ rotateRight(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + s1 + ch + k[index] + w[index];
            const std::uint32_t s0 = rotateRight(a, 2)
                ^ rotateRight(a, 13) ^ rotateRight(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + maj;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    };
    std::uint64_t totalBits = static_cast<std::uint64_t>(size) * 8;
    unsigned char block[128]{0x80};
    std::size_t offset = 0;
    while (size > 0) {
        const std::size_t take = (std::min)(
            size, static_cast<std::size_t>(64 - offset));
        std::memcpy(block + offset, data, take);
        offset += take;
        data += take;
        size -= take;
        if (offset == 64) {
            processBlock(block);
            offset = 0;
            std::memset(block, 0, 64);
        }
    }
    block[offset] = 0x80;
    if (offset > 55) {
        processBlock(block);
        std::memset(block, 0, 64);
        block[0] = 0x80;
        offset = 0;
    }
    for (int index = 0; index < 8; ++index)
        block[63 - index] = static_cast<unsigned char>(totalBits >> (index * 8));
    processBlock(block);
    return digestHex(state);
}

#ifdef _WIN32
inline std::string sha256HexWindows(const unsigned char *data, std::size_t size)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                    nullptr, 0) != 0)
        return {};

    ULONG objectLength = 0;
    ULONG propertyLength = 0;
    ULONG resultLength = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectLength),
                          sizeof(objectLength), &resultLength, 0) != 0
        || BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                             reinterpret_cast<PUCHAR>(&propertyLength),
                             sizeof(propertyLength), &resultLength, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }

    const ULONG digestLength = propertyLength;
    std::vector<unsigned char> object(objectLength);
    std::vector<unsigned char> digest(digestLength);
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectLength,
                         nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }

    bool success = true;
    while (size > 0) {
        const ULONG chunk = static_cast<ULONG>((std::min)(
            size, static_cast<std::size_t>(0xffffffffu)));
        if (BCryptHashData(hash, const_cast<PUCHAR>(data), chunk, 0) != 0) {
            success = false;
            break;
        }
        data += chunk;
        size -= chunk;
    }
    if (success && BCryptFinishHash(hash, digest.data(), digestLength, 0) != 0)
        success = false;

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return success ? hexEncode(digest.data(), digest.size()) : std::string();
}
#endif

inline std::string sha256Hex(const unsigned char *data, std::size_t size)
{
#ifdef _WIN32
    const std::string result = sha256HexWindows(data, size);
    return result.empty() ? sha256HexPortable(data, size) : result;
#else
    return sha256HexPortable(data, size);
#endif
}

inline std::string sha256Hex(const std::string &data)
{
    return sha256Hex(reinterpret_cast<const unsigned char *>(data.data()),
                     data.size());
}

} // namespace NativeHash
