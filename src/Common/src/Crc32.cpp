#include "QuantumCache/Common/Crc32.h"
#include <array>

namespace QuantumCache::Common {

namespace {

constexpr std::array<std::uint32_t, 256> BuildTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kCrcTable = BuildTable();

} // namespace

std::uint32_t Crc32::Compute(const void* data, std::size_t length) noexcept {
    return Compute(data, length, 0xFFFFFFFFu) ^ 0xFFFFFFFFu;
}

std::uint32_t Crc32::Compute(const void* data, std::size_t length, std::uint32_t seed) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::uint32_t crc = seed;
    for (std::size_t i = 0; i < length; ++i) {
        crc = kCrcTable[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

} // namespace QuantumCache::Common
