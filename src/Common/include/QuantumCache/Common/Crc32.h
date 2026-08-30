#pragma once
#include <cstddef>
#include <cstdint>

namespace QuantumCache::Common {

// Real CRC-32 (IEEE 802.3 polynomial, reflected, same variant used by zlib/PNG).
// Used to detect torn/corrupt records in the crash-recovery journal. This is a
// genuine implementation (table-driven), not a stub.
class Crc32 {
public:
    [[nodiscard]] static std::uint32_t Compute(const void* data, std::size_t length) noexcept;
    [[nodiscard]] static std::uint32_t Compute(const void* data, std::size_t length, std::uint32_t seed) noexcept;
};

} // namespace QuantumCache::Common
