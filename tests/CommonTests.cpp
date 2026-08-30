#include "QuantumCache/Common/Crc32.h"
#include "QuantumCache/Common/Result.h"
#include <gtest/gtest.h>
#include <cstring>

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
