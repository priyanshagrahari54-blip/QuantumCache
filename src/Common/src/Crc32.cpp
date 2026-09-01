#include "QuantumCache/Common/Crc32.h"
#include <array>
#include <cstring>

namespace QuantumCache::Common {

namespace {

using CrcTable8 = std::array<std::array<std::uint32_t, 256>, 8>;

// Builds 8 slicing-by-8 tables for IEEE 802.3 CRC32 at compile-time (constexpr).
constexpr CrcTable8 BuildTable8() {
    CrcTable8 table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[0][i] = c;
    }
    for (std::uint32_t i = 0; i < 256; ++i) {
        for (std::size_t t = 1; t < 8; ++t) {
            table[t][i] = (table[t - 1][i] >> 8) ^ table[0][table[t - 1][i] & 0xFFu];
        }
    }
    return table;
}

constexpr CrcTable8 kCrcTable8 = BuildTable8();

} // namespace

std::uint32_t Crc32::Compute(const void* data, std::size_t length) noexcept {
    return Compute(data, length, 0xFFFFFFFFu) ^ 0xFFFFFFFFu;
}

// Optimized slicing-by-8 algorithm: processes 8 bytes per iteration for multi-byte payloads.
std::uint32_t Crc32::Compute(const void* data, std::size_t length, std::uint32_t seed) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::uint32_t crc = seed;

    while (length >= 8) {
        std::uint32_t word1 = 0;
        std::uint32_t word2 = 0;
        std::memcpy(&word1, bytes, 4);
        std::memcpy(&word2, bytes + 4, 4);
        std::uint32_t val = crc ^ word1;
        crc = kCrcTable8[7][val & 0xFFu]
            ^ kCrcTable8[6][(val >> 8) & 0xFFu]
            ^ kCrcTable8[5][(val >> 16) & 0xFFu]
            ^ kCrcTable8[4][val >> 24]
            ^ kCrcTable8[3][word2 & 0xFFu]
            ^ kCrcTable8[2][(word2 >> 8) & 0xFFu]
            ^ kCrcTable8[1][(word2 >> 16) & 0xFFu]
            ^ kCrcTable8[0][word2 >> 24];
        bytes += 8;
        length -= 8;
    }

    while (length > 0) {
        crc = kCrcTable8[0][(crc ^ *bytes) & 0xFFu] ^ (crc >> 8);
        ++bytes;
        --length;
    }

    return crc;
}

} // namespace QuantumCache::Common
