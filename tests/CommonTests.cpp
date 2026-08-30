#include "QuantumCache/Common/Crc32.h"
#include "QuantumCache/Common/Result.h"
#include <gtest/gtest.h>
#include <cstring>
#include <vector>

using namespace QuantumCache::Common;

TEST(Crc32Test, KnownVector_EmptyString) {
    EXPECT_EQ(Crc32::Compute(nullptr, 0), 0x00000000u);
}

TEST(Crc32Test, KnownVector_123456789) {
    // Standard CRC-32 (IEEE 802.3) check value for the ASCII string
    // "123456789" is well known to be 0xCBF43926.
    const char* data = "123456789";
    EXPECT_EQ(Crc32::Compute(data, std::strlen(data)), 0xCBF43926u);
}

TEST(Crc32Test, DifferentInputsProduceDifferentChecksums) {
    const char* a = "quantum-cache-record-A";
    const char* b = "quantum-cache-record-B";
    EXPECT_NE(Crc32::Compute(a, std::strlen(a)), Crc32::Compute(b, std::strlen(b)));
}

TEST(Crc32Test, VariousLengthsAndAlignments) {
    // Reference byte-by-byte CRC32 calculation
    auto referenceCrc = [](const uint8_t* data, size_t len, uint32_t seed) -> uint32_t {
        uint32_t crc = seed;
        for (size_t i = 0; i < len; ++i) {
            uint32_t byte = data[i];
            crc ^= byte;
            for (int k = 0; k < 8; ++k) {
                crc = (crc & 1) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
            }
        }
        return crc;
    };

    std::vector<uint8_t> buffer(256);
    for (size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = static_cast<uint8_t>((i * 31 + 17) & 0xFF);
    }

    // Test different lengths (0 to 128) and unaligned offsets
    for (size_t offset = 0; offset < 8; ++offset) {
        for (size_t len = 0; len <= 128; ++len) {
            uint32_t expectedSeed = 0xFFFFFFFFu;
            uint32_t refVal = referenceCrc(buffer.data() + offset, len, expectedSeed) ^ 0xFFFFFFFFu;
            uint32_t actualVal = Crc32::Compute(buffer.data() + offset, len);
            EXPECT_EQ(actualVal, refVal) << "Failed at offset=" << offset << ", len=" << len;
        }
    }
}

TEST(ResultTest, SuccessCarriesValue) {
    auto r = Result<int>::Success(42);
    ASSERT_TRUE(r.IsOk());
    EXPECT_EQ(r.Value(), 42);
}

TEST(ResultTest, FailureCarriesError) {
    auto r = Result<int>::Failure(Error{ErrorCode::NotFound, "missing", 0});
    ASSERT_FALSE(r.IsOk());
    EXPECT_EQ(r.Err().code, ErrorCode::NotFound);
    EXPECT_EQ(r.Err().message, "missing");
}

TEST(ResultVoidTest, SuccessAndFailure) {
    auto ok = Result<void>::Success();
    EXPECT_TRUE(ok.IsOk());

    auto fail = Result<void>::Failure(Error{ErrorCode::IoError, "disk gone", 5});
    EXPECT_FALSE(fail.IsOk());
    EXPECT_EQ(fail.Err().platformErrorValue, 5u);
}
