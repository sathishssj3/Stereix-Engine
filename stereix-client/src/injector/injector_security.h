#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <set>
#include <string>
#include <vector>

namespace stereix {
namespace injector {

struct InjectorJobConfig {
    std::string authToken;
    DWORD pid = 0;
    std::wstring dllPath;
    std::wstring copySrc;
    std::wstring copyDst;
    std::wstring logPath;
    std::wstring expectedExeName;
    std::wstring expectedInstallPath;
    std::wstring resultFile;
    bool allowUnverifiedHotfix = false;
};

inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int needed = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) {
        // Fallback to ANSI code page for legacy CLI invocations
        needed = ::MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        if (needed <= 0) return L"";
        std::wstring out(static_cast<size_t>(needed), L'\0');
        ::MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
        return out;
    }
    std::wstring out(static_cast<size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

inline std::string WideToUtf8(const std::wstring& ws) {
    if (ws.empty()) return "";
    int needed = ::WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return "";
    std::string out(static_cast<size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

inline std::wstring ToLowerWide(std::wstring s) {
    for (auto& ch : s) {
        ch = static_cast<wchar_t>(::towlower(ch));
    }
    return s;
}

inline std::string ToLowerAscii(std::string s) {
    for (auto& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

inline std::wstring NormalizeWindowsPath(std::wstring p) {
    for (auto& ch : p) {
        if (ch == L'/') ch = L'\\';
    }
    // Strip extended-length prefix \\?\ if present
    if (p.size() >= 4 && p.compare(0, 4, L"\\\\?\\") == 0) {
        p.erase(0, 4);
    }
    while (p.size() > 3 && p.back() == L'\\') {
        p.pop_back();
    }
    return ToLowerWide(std::move(p));
}

inline bool ValidateAuthToken(const std::string& token) {
    if (token.size() < 32 || token.size() > 256) return false;
    std::set<char> distinct;
    for (char c : token) {
        unsigned char uc = static_cast<unsigned char>(c);
        bool ok = (uc >= 'a' && uc <= 'z') || (uc >= 'A' && uc <= 'Z') ||
                  (uc >= '0' && uc <= '9') || uc == '-' || uc == '_';
        if (!ok) return false;
        distinct.insert(static_cast<char>(std::tolower(uc)));
    }
    if (distinct.size() < 8) return false;
    const std::string lower = ToLowerAscii(token);
    if (lower.find("bypass") != std::string::npos ||
        lower.find("internal_agent") != std::string::npos ||
        lower == "00000000-0000-0000-0000-000000000000") {
        return false;
    }
    return true;
}

inline bool HasPathTraversal(const std::wstring& path) {
    if (path.empty()) return false;
    if (path.find(L"..") != std::wstring::npos) return true;
    for (wchar_t ch : path) {
        if (ch == L'\0' || ch == L'\r' || ch == L'\n') return true;
    }
    return false;
}

inline bool IsAbsoluteDrivePath(const std::wstring& path) {
    if (path.size() < 3) return false;
    wchar_t drive = static_cast<wchar_t>(::towlower(path[0]));
    return (drive >= L'a' && drive <= L'z') && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
}

inline bool IsPathInsideDirectory(const std::wstring& candidatePath, const std::wstring& rootDir) {
    if (candidatePath.empty() || rootDir.empty()) return false;
    std::wstring normCand = NormalizeWindowsPath(candidatePath);
    std::wstring normRoot = NormalizeWindowsPath(rootDir);
    if (normCand == normRoot) return true;
    if (!normRoot.empty() && normRoot.back() != L'\\') {
        normRoot.push_back(L'\\');
    }
    return normCand.size() > normRoot.size() && normCand.compare(0, normRoot.size(), normRoot) == 0;
}

inline bool IsSystemDirectory(const std::wstring& dirPath) {
    if (dirPath.empty()) return false;
    wchar_t winDir[MAX_PATH] = {};
    UINT len = ::GetWindowsDirectoryW(winDir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    return IsPathInsideDirectory(dirPath, std::wstring(winDir));
}

inline bool IsReparsePointPath(const std::wstring& path) {
    if (path.empty()) return false;
    DWORD attrs = ::GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

inline std::wstring ExtractBasenameLower(const std::wstring& path) {
    std::wstring norm = NormalizeWindowsPath(path);
    size_t pos = norm.find_last_of(L'\\');
    return (pos == std::wstring::npos) ? norm : norm.substr(pos + 1);
}

inline bool IsAllowedCallerImagePath(const std::wstring& fullImagePath, const std::wstring& windowsDirOverride = L"") {
    if (!IsAbsoluteDrivePath(fullImagePath) || HasPathTraversal(fullImagePath)) {
        return false;
    }
    const std::wstring norm = NormalizeWindowsPath(fullImagePath);
    const std::wstring base = ExtractBasenameLower(norm);

    // Direct launcher / dev-runner executables (basename exact match)
    if (base == L"stereix engine.exe" ||
        base == L"nexvr engine.exe" ||
        base == L"electron.exe" ||
        base == L"node.exe" ||
        base == L"antigravity.exe") {
        return true;
    }

    // System elevation/shell helpers MUST reside in Windows System32/SysWOW64/WindowsPowerShell or Program Files\PowerShell
    std::wstring winDir = windowsDirOverride;
    if (winDir.empty()) {
        wchar_t buf[MAX_PATH] = {};
        UINT len = ::GetWindowsDirectoryW(buf, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            winDir.assign(buf, len);
        }
    }
    if (winDir.empty()) return false;
    const std::wstring normWin = NormalizeWindowsPath(winDir);
    const std::wstring sys32 = normWin + L"\\system32";
    const std::wstring syswow64 = normWin + L"\\syswow64";

    if (base == L"powershell.exe") {
        return IsPathInsideDirectory(norm, sys32 + L"\\windowspowershell") ||
               IsPathInsideDirectory(norm, syswow64 + L"\\windowspowershell");
    }
    if (base == L"pwsh.exe") {
        std::wstring pfPowerShell;
        wchar_t pfBuf[MAX_PATH] = {};
        DWORD pfLen = ::GetEnvironmentVariableW(L"ProgramFiles", pfBuf, MAX_PATH);
        if (pfLen > 0 && pfLen < MAX_PATH) {
            pfPowerShell = NormalizeWindowsPath(std::wstring(pfBuf, pfLen)) + L"\\powershell";
        } else {
            pfPowerShell = normWin.substr(0, 2) + L"\\program files\\powershell";
        }
        return IsPathInsideDirectory(norm, pfPowerShell) ||
               IsPathInsideDirectory(norm, sys32);
    }
    if (base == L"cmd.exe" || base == L"svchost.exe" || base == L"consent.exe") {
        return norm == sys32 + L"\\" + base || norm == syswow64 + L"\\" + base;
    }
    if (base == L"explorer.exe") {
        return norm == normWin + L"\\explorer.exe";
    }

    return false;
}

inline bool ParseJobConfigJson(const std::string& jsonText, InjectorJobConfig& out, std::string& errOut) {
    try {
        auto j = nlohmann::json::parse(jsonText);
        if (!j.is_object()) {
            errOut = "Job file root must be a JSON object";
            return false;
        }
        if (j.contains("authToken") && j["authToken"].is_string()) {
            out.authToken = j["authToken"].get<std::string>();
        }
        if (j.contains("pid") && j["pid"].is_number_unsigned()) {
            out.pid = static_cast<DWORD>(j["pid"].get<uint64_t>());
        }
        if (j.contains("dllPath") && j["dllPath"].is_string()) {
            out.dllPath = Utf8ToWide(j["dllPath"].get<std::string>());
        }
        if (j.contains("copySrc") && j["copySrc"].is_string()) {
            out.copySrc = Utf8ToWide(j["copySrc"].get<std::string>());
        }
        if (j.contains("copyDst") && j["copyDst"].is_string()) {
            out.copyDst = Utf8ToWide(j["copyDst"].get<std::string>());
        }
        if (j.contains("logPath") && j["logPath"].is_string()) {
            out.logPath = Utf8ToWide(j["logPath"].get<std::string>());
        }
        if (j.contains("expectedExeName") && j["expectedExeName"].is_string()) {
            out.expectedExeName = Utf8ToWide(j["expectedExeName"].get<std::string>());
        }
        if (j.contains("expectedInstallPath") && j["expectedInstallPath"].is_string()) {
            out.expectedInstallPath = Utf8ToWide(j["expectedInstallPath"].get<std::string>());
        }
        if (j.contains("resultFile") && j["resultFile"].is_string()) {
            out.resultFile = Utf8ToWide(j["resultFile"].get<std::string>());
        }
        if (j.contains("allowUnverifiedHotfix") && j["allowUnverifiedHotfix"].is_boolean()) {
            out.allowUnverifiedHotfix = j["allowUnverifiedHotfix"].get<bool>();
        }
        return true;
    } catch (const std::exception& ex) {
        errOut = ex.what();
        return false;
    }
}

} // namespace injector
} // namespace stereix
