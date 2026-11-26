#include <gtest/gtest.h>
#include "injector/injector_security.h"

using namespace stereix::injector;

TEST(InjectorSecurityChecksTest, AuthTokenValidationRejectsWeakAndBypassTokens) {
    // CRIT-02: Require high-entropy >=32 char token; reject bypass literals and control chars
    EXPECT_TRUE(ValidateAuthToken("7d9b4f1a-8c2e-4f3b-9a1d-5e6c7b8a9f0e"));
    EXPECT_TRUE(ValidateAuthToken("a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718"));

    EXPECT_FALSE(ValidateAuthToken(""));
    EXPECT_FALSE(ValidateAuthToken("short-token-123"));
    EXPECT_FALSE(ValidateAuthToken("internal_agent_bypass"));
    EXPECT_FALSE(ValidateAuthToken("internal_agent_bypass_0123456789abcdef"));
    EXPECT_FALSE(ValidateAuthToken("00000000-0000-0000-0000-000000000000"));
    EXPECT_FALSE(ValidateAuthToken("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    EXPECT_FALSE(ValidateAuthToken("7d9b4f1a-8c2e-4f3b-9a1d-5e6c7b8a9f0e;calc"));
    EXPECT_FALSE(ValidateAuthToken("7d9b4f1a-8c2e-4f3b-9a1d-5e6c7b8a9f0e'"));
}

TEST(InjectorSecurityChecksTest, PathContainmentBlocksPrefixCollisionsAndTraversal) {
    // CRIT-02: Trailing separator boundary prevents C:\Games\SekiroEvil matching C:\Games\Sekiro
    EXPECT_TRUE(IsPathInsideDirectory(L"C:\\Games\\Sekiro\\sekiro.exe", L"C:\\Games\\Sekiro"));
    EXPECT_TRUE(IsPathInsideDirectory(L"C:/Games/Sekiro/Binaries/Win64/game.exe", L"C:\\Games\\Sekiro\\"));
    EXPECT_TRUE(IsPathInsideDirectory(L"\\\\?\\C:\\Games\\Sekiro\\sekiro.exe", L"C:\\Games\\Sekiro"));

    EXPECT_FALSE(IsPathInsideDirectory(L"C:\\Games\\SekiroEvil\\sekiro.exe", L"C:\\Games\\Sekiro"));
    EXPECT_FALSE(IsPathInsideDirectory(L"D:\\Games\\Sekiro\\sekiro.exe", L"C:\\Games\\Sekiro"));
    EXPECT_FALSE(IsPathInsideDirectory(L"", L"C:\\Games\\Sekiro"));

    EXPECT_TRUE(HasPathTraversal(L"C:\\Games\\..\\Windows\\System32\\dxgi.dll"));
    EXPECT_TRUE(HasPathTraversal(L"C:\\Games\\foo\nbar.dll"));
    EXPECT_FALSE(HasPathTraversal(L"C:\\Games\\Assassin\u2019s Creed\\ac.exe"));

    EXPECT_TRUE(IsAbsoluteDrivePath(L"C:\\Games\\vrinject.dll"));
    EXPECT_FALSE(IsAbsoluteDrivePath(L"relative\\vrinject.dll"));
    EXPECT_FALSE(IsAbsoluteDrivePath(L"\\\\evil-server\\share\\vrinject.dll"));
}

TEST(InjectorSecurityChecksTest, CallerImagePathAllowsOnlyCanonicalSystemShellsAndLauncher) {
    // CRIT-02: Substring matching on powershell/cmd/svchost is forbidden
    const std::wstring winDir = L"C:\\Windows";
    EXPECT_TRUE(IsAllowedCallerImagePath(L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe", winDir));
    EXPECT_TRUE(IsAllowedCallerImagePath(L"C:\\Windows\\SysWOW64\\WindowsPowerShell\\v1.0\\powershell.exe", winDir));
    EXPECT_TRUE(IsAllowedCallerImagePath(L"C:\\Windows\\System32\\cmd.exe", winDir));
    EXPECT_TRUE(IsAllowedCallerImagePath(L"C:\\Windows\\explorer.exe", winDir));
    EXPECT_TRUE(IsAllowedCallerImagePath(L"C:\\Users\\player\\AppData\\Local\\Programs\\Stereix Engine\\Stereix Engine.exe", winDir));

    EXPECT_FALSE(IsAllowedCallerImagePath(L"C:\\evil\\powershell.exe", winDir));
    EXPECT_FALSE(IsAllowedCallerImagePath(L"C:\\Temp\\cmd.exe", winDir));
    EXPECT_FALSE(IsAllowedCallerImagePath(L"C:\\Users\\attacker\\powershell\\pwsh.exe", winDir));
    EXPECT_FALSE(IsAllowedCallerImagePath(L"C:\\Temp\\svchost.exe", winDir));
    EXPECT_FALSE(IsAllowedCallerImagePath(L"powershell.exe", winDir));
    EXPECT_FALSE(IsAllowedCallerImagePath(L"C:\\Windows\\System32\\..\\..\\evil\\cmd.exe", winDir));
}

TEST(InjectorSecurityChecksTest, JobFileJsonHandlesUnicodeSmartQuotesWithoutShellQuoting) {
    // CRIT-01: Unicode smart quote U+2019 (’) passes cleanly through UTF-8 JSON job file
    const std::string json = R"({
        "authToken": "7d9b4f1a-8c2e-4f3b-9a1d-5e6c7b8a9f0e",
        "pid": 4242,
        "dllPath": "C:\\Program Files\\Stereix\\vrinject.dll",
        "copySrc": "C:\\Program Files\\Stereix\\resources",
        "copyDst": "C:\\Games\\Assassin\u2019s Creed\\Binaries",
        "expectedExeName": "ac.exe",
        "expectedInstallPath": "C:\\Games\\Assassin\u2019s Creed",
        "allowUnverifiedHotfix": false
    })";
    InjectorJobConfig cfg{};
    std::string err;
    ASSERT_TRUE(ParseJobConfigJson(json, cfg, err)) << err;
    EXPECT_EQ(cfg.pid, 4242u);
    EXPECT_TRUE(ValidateAuthToken(cfg.authToken));
    EXPECT_EQ(cfg.copyDst, L"C:\\Games\\Assassin\u2019s Creed\\Binaries");
    EXPECT_FALSE(cfg.allowUnverifiedHotfix);

    InjectorJobConfig badCfg{};
    EXPECT_FALSE(ParseJobConfigJson("not valid json", badCfg, err));
    EXPECT_FALSE(ParseJobConfigJson("[1, 2, 3]", badCfg, err));
}
