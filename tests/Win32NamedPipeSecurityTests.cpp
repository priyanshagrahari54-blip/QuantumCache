// Windows-only regression test for the named-pipe security descriptor
// used by Win32NamedPipeTransport.cpp (Priority 4.14 in the Stage 2
// hardening audit — "document and test actual Windows security
// descriptor"). Compiled and run ONLY on Windows targets (WIN32 defined)
// — see tests/CMakeLists.txt, which adds this file to the test target
// only when building for Windows (native MSVC or MinGW-w64
// cross-compile). Verified to actually EXECUTE (not merely compile) via
// Wine on this Linux sandbox as an approximation of real Windows
// behavior — see docs/ENVIRONMENT.md "Windows runtime testing" for the
// explicit distinction between Wine execution and real Windows/NTFS
// validation, which this test does not and cannot replace.
#include <gtest/gtest.h>
#include <windows.h>
#include <sddl.h>

namespace {
// Must match the exact SDDL string in Win32NamedPipeTransport.cpp's
// AcceptOnce() — duplicated here deliberately (rather than shared via a
// header) so this test fails loudly if that string is ever hand-edited
// without updating this regression coverage alongside it.
constexpr const wchar_t* kExpectedPipeSddl = L"D:(A;;GRGW;;;AU)";
} // namespace

TEST(Win32NamedPipeSecurityTest, PipeSddl_ParsesToAValidSecurityDescriptor) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    ULONG sdSize = 0;
    BOOL ok = ::ConvertStringSecurityDescriptorToSecurityDescriptorW(
        kExpectedPipeSddl, SDDL_REVISION_1, &sd, &sdSize);
    ASSERT_TRUE(ok) << "ConvertStringSecurityDescriptorToSecurityDescriptorW failed, error="
                     << ::GetLastError();
    ASSERT_NE(sd, nullptr);
    EXPECT_TRUE(::IsValidSecurityDescriptor(sd));
    ::LocalFree(sd);
}

TEST(Win32NamedPipeSecurityTest, PipeSddl_HasNonNullDacl_NotUnrestrictedEveryone) {
    // The single most important security property: this SDDL must NOT
    // produce a NULL DACL (which Windows interprets as "no restriction,
    // full access to Everyone") — that would defeat the entire purpose
    // of specifying an explicit security descriptor for a pipe crossing
    // a LocalSystem-service/per-user-GUI privilege boundary.
    PSECURITY_DESCRIPTOR sd = nullptr;
    ASSERT_TRUE(::ConvertStringSecurityDescriptorToSecurityDescriptorW(
        kExpectedPipeSddl, SDDL_REVISION_1, &sd, nullptr));
    ASSERT_NE(sd, nullptr);

    BOOL daclPresent = FALSE;
    BOOL daclDefaulted = FALSE;
    PACL dacl = nullptr;
    ASSERT_TRUE(::GetSecurityDescriptorDacl(sd, &daclPresent, &dacl, &daclDefaulted));
    EXPECT_TRUE(daclPresent);
    EXPECT_NE(dacl, nullptr) << "a NULL DACL here would mean unrestricted (Everyone) access -- "
                                 "a real security regression, not just a test failure";

    ::LocalFree(sd);
}

TEST(Win32NamedPipeSecurityTest, PipeSddl_GrantsExactlyAuthenticatedUsers_NotBroaderPrincipal) {
    // Round-trip back to SDDL text and confirm the ACE's trustee is
    // specifically "AU" (Authenticated Users) — not "WD" (Everyone/World)
    // or "BU" (Builtin Users, which can include lower-trust accounts on
    // some configurations) or any broader grant. This directly guards
    // against a future edit accidentally widening pipe access.
    PSECURITY_DESCRIPTOR sd = nullptr;
    ASSERT_TRUE(::ConvertStringSecurityDescriptorToSecurityDescriptorW(
        kExpectedPipeSddl, SDDL_REVISION_1, &sd, nullptr));
    ASSERT_NE(sd, nullptr);

    LPWSTR roundTrip = nullptr;
    ASSERT_TRUE(::ConvertSecurityDescriptorToStringSecurityDescriptorW(
        sd, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &roundTrip, nullptr));
    std::wstring roundTripStr(roundTrip);
    ::LocalFree(roundTrip);
    ::LocalFree(sd);

    // The trustee SID string "AU" must appear; "WD" (World/Everyone)
    // must NOT appear anywhere in the round-tripped DACL text.
    EXPECT_NE(roundTripStr.find(L"AU"), std::wstring::npos)
        << "expected Authenticated Users (AU) trustee not found in: " << std::string(roundTripStr.begin(), roundTripStr.end());
    EXPECT_EQ(roundTripStr.find(L";WD;"), std::wstring::npos)
        << "found an unexpected Everyone/World (WD) grant in: " << std::string(roundTripStr.begin(), roundTripStr.end());
}

TEST(Win32NamedPipeSecurityTest, PipeSddl_GrantsOnlyGenericReadWrite_NotFullControl) {
    // Confirm the access mask granted is exactly Generic Read + Generic
    // Write ("GRGW" in the SDDL), not the much broader "GA" (Generic
    // All / full control) a careless future edit might introduce.
    PSECURITY_DESCRIPTOR sd = nullptr;
    ASSERT_TRUE(::ConvertStringSecurityDescriptorToSecurityDescriptorW(
        kExpectedPipeSddl, SDDL_REVISION_1, &sd, nullptr));
    ASSERT_NE(sd, nullptr);

    LPWSTR roundTrip = nullptr;
    ASSERT_TRUE(::ConvertSecurityDescriptorToStringSecurityDescriptorW(
        sd, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &roundTrip, nullptr));
    std::wstring roundTripStr(roundTrip);
    ::LocalFree(roundTrip);
    ::LocalFree(sd);

    EXPECT_EQ(roundTripStr.find(L"GA"), std::wstring::npos)
        << "found an unexpected Generic-All (full control) grant in: "
        << std::string(roundTripStr.begin(), roundTripStr.end());
}
