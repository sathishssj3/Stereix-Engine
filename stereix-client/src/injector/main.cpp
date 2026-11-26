// ============================================================================
// main.cpp – Standalone hardened DLL injector for Stereix Engine
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <shellapi.h>
#include <TlHelp32.h>
#include <Shlwapi.h>
#include <Psapi.h>

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#ifndef NTSTATUS
typedef LONG NTSTATUS;
#endif
#include <bcrypt.h>
#include "expected_hash.h"
#include "injector_security.h"

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "Psapi.lib")

namespace {

using namespace stereix::injector;

FILE* g_logFile = nullptr;

enum class Colour : WORD {
    Default = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE,
    Green   = FOREGROUND_GREEN | FOREGROUND_INTENSITY,
    Yellow  = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,
    Red     = FOREGROUND_RED | FOREGROUND_INTENSITY,
    Cyan    = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY,
};

void SetColour(Colour c) {
    HANDLE hCon = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (hCon && hCon != INVALID_HANDLE_VALUE)
        ::SetConsoleTextAttribute(hCon, static_cast<WORD>(c));
}

void PrintStatus(const char* tag, Colour c, const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    SetColour(c);
    std::printf("[%s] ", tag);
    SetColour(Colour::Default);
    std::printf("%s\n", buf);
    std::fflush(stdout);

    if (g_logFile) {
        std::fprintf(g_logFile, "[%s] %s\n", tag, buf);
        std::fflush(g_logFile);
    }
}

void PrintOK(const char* fmt, ...) {
    va_list a; va_start(a, fmt);
    char buf[1024]; std::vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    PrintStatus("  OK  ", Colour::Green, "%s", buf);
}

void PrintInfo(const char* fmt, ...) {
    va_list a; va_start(a, fmt);
    char buf[1024]; std::vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    PrintStatus(" INFO ", Colour::Cyan, "%s", buf);
}

void PrintWarn(const char* fmt, ...) {
    va_list a; va_start(a, fmt);
    char buf[1024]; std::vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    PrintStatus(" WARN ", Colour::Yellow, "%s", buf);
}

void PrintErr(const char* fmt, ...) {
    va_list a; va_start(a, fmt);
    char buf[1024]; std::vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    PrintStatus(" FAIL ", Colour::Red, "%s", buf);
}

std::string LastErrorMessage() {
    DWORD err = ::GetLastError();
    if (err == 0) return "No error";

    char* msgBuf = nullptr;
    DWORD len = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err,
        MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
        reinterpret_cast<LPSTR>(&msgBuf), 0, nullptr);

    std::string result;
    if (len && msgBuf) {
        result.assign(msgBuf, len);
        while (!result.empty() && (result.back() == '\r' || result.back() == '\n'))
            result.pop_back();
        ::LocalFree(msgBuf);
    }
    return result;
}

// Compute SHA-256 and validate basic MZ/PE header from an ALREADY-OPEN read-locked file handle.
// Resets file pointer to 0 before reading.
bool HashAndVerifyPeFromHandle(HANDLE hFile, std::wstring& outHash, bool requirePeHeader) {
    outHash.clear();
    if (hFile == INVALID_HANDLE_VALUE || hFile == nullptr) return false;

    LARGE_INTEGER zero{};
    if (!::SetFilePointerEx(hFile, zero, nullptr, FILE_BEGIN)) return false;

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;

    DWORD cbData = 0, cbHashObject = 0, cbHash = 0;
    if (BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PBYTE>(&cbHashObject), sizeof(DWORD), &cbData, 0) != 0 ||
        BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, reinterpret_cast<PBYTE>(&cbHash), sizeof(DWORD), &cbData, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }

    std::vector<BYTE> pbHashObject(cbHashObject);
    std::vector<BYTE> pbHash(cbHash);
    if (BCryptCreateHash(hAlg, &hHash, pbHashObject.data(), cbHashObject, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }

    BYTE buffer[16384];
    DWORD bytesRead = 0;
    bool firstChunk = true;
    bool peValid = !requirePeHeader;
    bool readError = false;

    while (::ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, nullptr)) {
        if (bytesRead == 0) break;
        if (firstChunk) {
            firstChunk = false;
            if (requirePeHeader) {
                peValid = false;
                if (bytesRead >= sizeof(IMAGE_DOS_HEADER)) {
                    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(buffer);
                    if (dos->e_magic == IMAGE_DOS_SIGNATURE &&
                        dos->e_lfanew >= static_cast<LONG>(sizeof(IMAGE_DOS_HEADER)) &&
                        static_cast<DWORD>(dos->e_lfanew) + sizeof(DWORD) <= bytesRead) {
                        const DWORD* sig = reinterpret_cast<const DWORD*>(buffer + dos->e_lfanew);
                        if (*sig == IMAGE_NT_SIGNATURE) {
                            peValid = true;
                        }
                    }
                }
                if (!peValid) {
                    break;
                }
            }
        }
        if (BCryptHashData(hHash, buffer, bytesRead, 0) != 0) {
            readError = true;
            break;
        }
    }

    bool ok = false;
    if (!readError && peValid && BCryptFinishHash(hHash, pbHash.data(), cbHash, 0) == 0) {
        wchar_t hex[3];
        outHash.reserve(static_cast<size_t>(cbHash) * 2);
        for (DWORD i = 0; i < cbHash; ++i) {
            swprintf_s(hex, L"%02X", pbHash[i]);
            outHash += hex;
        }
        ok = true;
    }

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

bool IsSameFileW(const std::wstring& path1, const std::wstring& path2) {
    if (NormalizeWindowsPath(path1) == NormalizeWindowsPath(path2)) return true;
    HANDLE h1 = ::CreateFileW(path1.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h1 == INVALID_HANDLE_VALUE) return false;
    HANDLE h2 = ::CreateFileW(path2.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h2 == INVALID_HANDLE_VALUE) {
        ::CloseHandle(h1);
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info1{}, info2{};
    bool same = false;
    if (::GetFileInformationByHandle(h1, &info1) && ::GetFileInformationByHandle(h2, &info2)) {
        same = (info1.dwVolumeSerialNumber == info2.dwVolumeSerialNumber &&
                info1.nFileIndexHigh == info2.nFileIndexHigh &&
                info1.nFileIndexLow == info2.nFileIndexLow);
    }
    ::CloseHandle(h1);
    ::CloseHandle(h2);
    return same;
}

std::wstring GetParentDirW(const std::wstring& p) {
    size_t pos = p.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L"";
    return p.substr(0, pos);
}

// Open an existing file exclusively against concurrent writers/deleters (FILE_SHARE_READ only),
// rejecting symlinks/reparse points on the file and its immediate directory.
HANDLE OpenReadLockedNonReparseFile(const std::wstring& filePath) {
    if (filePath.empty() || HasPathTraversal(filePath) || !IsAbsoluteDrivePath(filePath)) {
        return INVALID_HANDLE_VALUE;
    }
    if (IsReparsePointPath(filePath)) {
        return INVALID_HANDLE_VALUE;
    }
    std::wstring parentDir = GetParentDirW(filePath);
    if (!parentDir.empty() && IsReparsePointPath(parentDir)) {
        return INVALID_HANDLE_VALUE;
    }
    HANDLE h = ::CreateFileW(
        filePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ, // Prevents write/rename/delete TOCTOU while allowing LoadLibraryW
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(h, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
        ::CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

// Resolve true final kernel path on disk (fail-closed) and block Windows system directories
// as well as ancestor-junction escapes outside optional expectedRoot.
bool VerifyFinalHandlePathSafe(HANDLE hFile, const std::wstring& expectedRoot = L"") {
    if (hFile == INVALID_HANDLE_VALUE || hFile == nullptr) return false;
    wchar_t finalBuf[1024] = {};
    DWORD finalLen = ::GetFinalPathNameByHandleW(
        hFile, finalBuf, static_cast<DWORD>(std::size(finalBuf)), FILE_NAME_NORMALIZED);
    if (finalLen == 0 || finalLen >= std::size(finalBuf)) {
        return false; // Fail-closed if kernel path cannot be resolved
    }
    const std::wstring finalPath(finalBuf, finalLen);
    if (IsSystemDirectory(finalPath)) {
        return false;
    }
    if (!expectedRoot.empty() && !IsPathInsideDirectory(finalPath, expectedRoot)) {
        return false;
    }
    return true;
}

// Safely open a non-reparse output file handle for writing/appending, rejecting parent reparse points
// and ancestor-junction redirects into Windows system directories.
HANDLE OpenSafeOutputFileHandle(const std::wstring& filePath, bool appendMode, const std::wstring& expectedRoot = L"") {
    if (filePath.empty() || HasPathTraversal(filePath) || !IsAbsoluteDrivePath(filePath)) {
        return INVALID_HANDLE_VALUE;
    }
    if (IsReparsePointPath(filePath)) return INVALID_HANDLE_VALUE;
    const std::wstring parentDir = GetParentDirW(filePath);
    if (parentDir.empty() || IsReparsePointPath(parentDir) || IsSystemDirectory(parentDir)) {
        return INVALID_HANDLE_VALUE;
    }
    HANDLE hFile = ::CreateFileW(
        filePath.c_str(),
        appendMode ? FILE_APPEND_DATA : GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        appendMode ? OPEN_ALWAYS : CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(hFile, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
        !VerifyFinalHandlePathSafe(hFile, expectedRoot)) {
        ::CloseHandle(hFile);
        if (!appendMode) {
            ::DeleteFileW(filePath.c_str());
        }
        return INVALID_HANDLE_VALUE;
    }
    return hFile;
}

// Copy bytes from an already-verified, read-locked source handle to a destination file safely.
bool CopyFromVerifiedHandle(HANDLE hSrc, const std::wstring& dstPath, const std::wstring& expectedRoot = L"") {
    if (hSrc == INVALID_HANDLE_VALUE || dstPath.empty()) return false;
    if (IsReparsePointPath(dstPath)) return false;
    std::wstring dstParent = GetParentDirW(dstPath);
    if (dstParent.empty() || IsReparsePointPath(dstParent) || IsSystemDirectory(dstParent)) {
        return false;
    }

    LARGE_INTEGER zero{};
    if (!::SetFilePointerEx(hSrc, zero, nullptr, FILE_BEGIN)) return false;

    std::wstring tmpPath = dstPath + L".stereix_tmp_" + std::to_wstring(::GetCurrentProcessId());
    if (IsReparsePointPath(tmpPath)) return false;

    HANDLE hDst = ::CreateFileW(
        tmpPath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (hDst == INVALID_HANDLE_VALUE) return false;

    // Resolve the true final kernel path on disk (fail-closed) to block ancestor-junction writes
    if (!VerifyFinalHandlePathSafe(hDst, expectedRoot)) {
        ::CloseHandle(hDst);
        ::DeleteFileW(tmpPath.c_str());
        return false;
    }

    BYTE buf[32768];
    DWORD bytesRead = 0;
    bool ok = true;
    while (::ReadFile(hSrc, buf, sizeof(buf), &bytesRead, nullptr)) {
        if (bytesRead == 0) break;
        DWORD bytesWritten = 0;
        if (!::WriteFile(hDst, buf, bytesRead, &bytesWritten, nullptr) || bytesWritten != bytesRead) {
            ok = false;
            break;
        }
    }
    ::CloseHandle(hDst);

    if (!ok) {
        ::DeleteFileW(tmpPath.c_str());
        return false;
    }

    if (::GetFileAttributesW(dstPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring dstOld = dstPath + L".old";
        if (!IsReparsePointPath(dstOld)) {
            ::DeleteFileW(dstOld.c_str());
            ::MoveFileExW(dstPath.c_str(), dstOld.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }

    if (!::MoveFileExW(tmpPath.c_str(), dstPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        ::DeleteFileW(tmpPath.c_str());
        return false;
    }
    return true;
}

bool QueryProcessImageAndMem(DWORD pid, std::wstring& outImagePath, SIZE_T& outWorkingSet) {
    outImagePath.clear();
    outWorkingSet = 0;
    HANDLE hProc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) {
        hProc = ::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    }
    if (!hProc) return false;

    wchar_t imgBuf[1024] = {};
    DWORD imgSize = static_cast<DWORD>(std::size(imgBuf));
    bool hasPath = ::QueryFullProcessImageNameW(hProc, 0, imgBuf, &imgSize) != FALSE;
    if (hasPath) {
        outImagePath.assign(imgBuf, imgSize);
    }

    PROCESS_MEMORY_COUNTERS pmc{};
    if (::GetProcessMemoryInfo(hProc, &pmc, sizeof(pmc))) {
        outWorkingSet = pmc.WorkingSetSize;
    }
    ::CloseHandle(hProc);
    return hasPath;
}

// Re-resolve live game PID inside the elevated injector using verified exe name + install directory.
DWORD ResolveLiveGamePid(const std::wstring& expectedExeName, const std::wstring& expectedInstallPath) {
    if (expectedExeName.empty()) return 0;
    const std::wstring targetBase = ExtractBasenameLower(expectedExeName);

    HANDLE hSnap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return 0;

    DWORD bestPid = 0;
    SIZE_T bestMem = 0;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (::Process32FirstW(hSnap, &pe)) {
        do {
            if (pe.th32ProcessID <= 4) continue;
            if (ToLowerWide(pe.szExeFile) != targetBase) continue;

            std::wstring imgPath;
            SIZE_T ws = 0;
            if (!QueryProcessImageAndMem(pe.th32ProcessID, imgPath, ws)) continue;
            if (ws < 5 * 1024 * 1024) continue; // Ignore tiny stubs (< 5 MB)
            if (!expectedInstallPath.empty() && !IsPathInsideDirectory(imgPath, expectedInstallPath)) {
                continue;
            }
            if (ws > bestMem) {
                bestMem = ws;
                bestPid = pe.th32ProcessID;
            }
        } while (::Process32NextW(hSnap, &pe));
    }
    ::CloseHandle(hSnap);
    return bestPid;
}

bool InjectDll(DWORD pid, const std::wstring& dllPathW) {
    PrintInfo("Opening process PID %lu ...", pid);
    // Least-privilege access mask (no PROCESS_ALL_ACCESS)
    constexpr DWORD kInjectAccess =
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;
    HANDLE hProcess = ::OpenProcess(kInjectAccess, FALSE, pid);
    if (!hProcess) {
        PrintErr("OpenProcess failed: %s", LastErrorMessage().c_str());
        return false;
    }
    PrintOK("Process handle acquired: 0x%p", hProcess);

    BOOL isWow64 = FALSE;
    if (::IsWow64Process(hProcess, &isWow64) && isWow64) {
        PrintErr("Target process is 32-bit (x86). Stereix Engine only supports 64-bit (x64) games.");
        ::CloseHandle(hProcess);
        return false;
    }

    // L-4: Target process validation - verify working set is > 1MB to avoid uninitialized/ghost processes
    PROCESS_MEMORY_COUNTERS pmc{};
    if (::GetProcessMemoryInfo(hProcess, &pmc, sizeof(pmc))) {
        if (pmc.WorkingSetSize < 1024 * 1024) {
            PrintErr("Target process working set is too small (< 1 MB). Process may be uninitialized or exiting.");
            ::CloseHandle(hProcess);
            return false;
        }
    }

    // Duplicate Injection Shield (case-insensitive wide path check)
    HMODULE hExistingMods[1024];
    DWORD cbExistingNeeded = 0;
    if (::EnumProcessModules(hProcess, hExistingMods, sizeof(hExistingMods), &cbExistingNeeded)) {
        DWORD numMods = cbExistingNeeded / sizeof(HMODULE);
        wchar_t modName[MAX_PATH];
        for (DWORD m = 0; m < numMods; ++m) {
            if (::GetModuleFileNameExW(hProcess, hExistingMods[m], modName, MAX_PATH)) {
                std::wstring sMod = ToLowerWide(modName);
                if (sMod.find(L"vrinject.dll") != std::wstring::npos) {
                    PrintOK("Stereix Engine (vrinject.dll) is already loaded and active in target process (PID %lu).", pid);
                    PrintOK("Hook pipeline and OpenXR runtime active. Injection verified successfully.");
                    ::CloseHandle(hProcess);
                    return true;
                }
            }
        }
    }

    SIZE_T pathBytes = (dllPathW.size() + 1) * sizeof(wchar_t);
    void* remoteMem = ::VirtualAllocEx(hProcess, nullptr, pathBytes,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        PrintErr("VirtualAllocEx failed: %s", LastErrorMessage().c_str());
        ::CloseHandle(hProcess);
        return false;
    }

    SIZE_T written = 0;
    if (!::WriteProcessMemory(hProcess, remoteMem, dllPathW.c_str(), pathBytes, &written) || written != pathBytes) {
        PrintErr("WriteProcessMemory failed: %s", LastErrorMessage().c_str());
        ::VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        ::CloseHandle(hProcess);
        return false;
    }

    HMODULE hKernel32 = ::GetModuleHandleW(L"kernel32.dll");
    if (!hKernel32) {
        PrintErr("Could not get kernel32.dll handle");
        ::VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        ::CloseHandle(hProcess);
        return false;
    }

    auto pLoadLibraryW = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        ::GetProcAddress(hKernel32, "LoadLibraryW"));
    if (!pLoadLibraryW) {
        PrintErr("Could not resolve LoadLibraryW");
        ::VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        ::CloseHandle(hProcess);
        return false;
    }

    PrintInfo("Creating remote thread ...");

    bool success = false;
    for (int attempt = 1; attempt <= 10; ++attempt) {
        HANDLE hThread = ::CreateRemoteThread(hProcess, nullptr, 0,
                                              pLoadLibraryW, remoteMem,
                                              0, nullptr);
        if (!hThread) {
            PrintErr("CreateRemoteThread failed: %s", LastErrorMessage().c_str());
            break;
        }

        PrintInfo("Waiting for remote thread to complete (Attempt %d/10)...", attempt);
        DWORD waitResult = ::WaitForSingleObject(hThread, 20000);

        if (waitResult == WAIT_OBJECT_0) {
            DWORD exitCode = 0;
            ::GetExitCodeThread(hThread, &exitCode);
            bool isLoaded = (exitCode != 0);
            if (!isLoaded) {
                // L-1: Verify load via EnumProcessModules in case low 32 bits of 64-bit HMODULE were 0
                HMODULE hMods[1024];
                DWORD cbNeeded = 0;
                if (::EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded)) {
                    DWORD numMods = cbNeeded / sizeof(HMODULE);
                    wchar_t modName[MAX_PATH];
                    for (DWORD m = 0; m < numMods; ++m) {
                        if (::GetModuleFileNameExW(hProcess, hMods[m], modName, MAX_PATH)) {
                            if (ToLowerWide(modName).find(L"vrinject.dll") != std::wstring::npos) {
                                isLoaded = true;
                                break;
                            }
                        }
                    }
                }
            }

            if (isLoaded) {
                PrintOK("DLL loaded successfully! Remote HMODULE = 0x%08lX", exitCode);
                success = true;
                ::CloseHandle(hThread);
                break;
            } else {
                if (attempt < 10) {
                    PrintWarn("LoadLibraryW returned NULL. Retrying in 500ms... (AV lock suspected)");
                    ::Sleep(500);
                } else {
                    PrintErr("LoadLibraryW returned NULL in the remote process after 10 attempts.");
                }
            }
        } else if (waitResult == WAIT_TIMEOUT) {
            PrintErr("Remote thread timed out (20 s). The target may be hung.");
            ::CloseHandle(hThread);
            // H-4: Do NOT free remoteMem here because the remote thread may still be executing.
            remoteMem = nullptr;
            break;
        } else {
            PrintErr("WaitForSingleObject failed: %s", LastErrorMessage().c_str());
            ::CloseHandle(hThread);
            remoteMem = nullptr;
            break;
        }

        ::CloseHandle(hThread);
    }

    if (remoteMem) {
        ::VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    }
    ::CloseHandle(hProcess);

    return success;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    HANDLE hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (hOut && ::GetConsoleMode(hOut, &mode)) {
        ::SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }

    InjectorJobConfig job{};
    std::wstring jobFilePath;

    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--job-file") == 0 && i + 1 < argc) {
            jobFilePath = argv[++i];
        } else if (std::wcscmp(argv[i], L"--pid") == 0 && i + 1 < argc) {
            job.pid = static_cast<DWORD>(std::wcstoul(argv[++i], nullptr, 10));
        } else if (std::wcscmp(argv[i], L"--dll") == 0 && i + 1 < argc) {
            job.dllPath = argv[++i];
        } else if (std::wcscmp(argv[i], L"--copy-src") == 0 && i + 1 < argc) {
            job.copySrc = argv[++i];
        } else if (std::wcscmp(argv[i], L"--copy-dst") == 0 && i + 1 < argc) {
            job.copyDst = argv[++i];
        } else if (std::wcscmp(argv[i], L"--log") == 0 && i + 1 < argc) {
            job.logPath = argv[++i];
        } else if (std::wcscmp(argv[i], L"--allow-unverified-hotfix") == 0) {
            job.allowUnverifiedHotfix = true;
        }
    }

    // Load JSON job file if supplied (eliminates PowerShell quoting and command-line token exposure)
    if (!jobFilePath.empty()) {
        if (HasPathTraversal(jobFilePath) || !IsAbsoluteDrivePath(jobFilePath) || IsReparsePointPath(jobFilePath)) {
            PrintErr("[ERROR] Invalid or untrusted --job-file path rejected.");
            return 10;
        }
        HANDLE hJob = OpenReadLockedNonReparseFile(jobFilePath);
        if (hJob == INVALID_HANDLE_VALUE) {
            PrintErr("[ERROR] Unable to open --job-file securely.");
            return 10;
        }
        LARGE_INTEGER fileSize{};
        std::string jsonContent;
        if (::GetFileSizeEx(hJob, &fileSize) && fileSize.QuadPart > 0 && fileSize.QuadPart <= 1024 * 1024) {
            jsonContent.resize(static_cast<size_t>(fileSize.QuadPart));
            DWORD readBytes = 0;
            if (!::ReadFile(hJob, jsonContent.data(), static_cast<DWORD>(jsonContent.size()), &readBytes, nullptr) ||
                readBytes != jsonContent.size()) {
                jsonContent.clear();
            }
        }
        ::CloseHandle(hJob);

        if (jsonContent.empty()) {
            PrintErr("[ERROR] Empty or unreadable --job-file.");
            return 10;
        }
        std::string parseErr;
        if (!ParseJobConfigJson(jsonContent, job, parseErr)) {
            PrintErr("[ERROR] Failed to parse --job-file JSON: %s", parseErr.c_str());
            return 10;
        }
    }

    struct LogCloser {
        ~LogCloser() {
            if (g_logFile) {
                std::fclose(g_logFile);
                g_logFile = nullptr;
            }
        }
    } logCloser;

    // S3.3: Origin Restriction (Authentication & caller verification run FIRST before ANY file writes - CRIT-02)
    if (job.authToken.empty()) {
        wchar_t envTokBuf[512] = {};
        DWORD tokLen = ::GetEnvironmentVariableW(L"STEREIX_AUTH_TOKEN", envTokBuf, 512);
        if (tokLen == 0 || tokLen >= 512) {
            tokLen = ::GetEnvironmentVariableW(L"NEXVR_AUTH_TOKEN", envTokBuf, 512);
        }
        if (tokLen > 0 && tokLen < 512) {
            job.authToken = WideToUtf8(std::wstring(envTokBuf, tokLen));
        }
    }
    if (!ValidateAuthToken(job.authToken)) {
        PrintErr("[ERROR] Unauthorized origin. Missing or weak security token. Please launch via the Stereix Engine UI.");
        return 13;
    }

    DWORD parentPid = 0;
    HANDLE hSnap2 = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap2 == INVALID_HANDLE_VALUE) {
        PrintErr("[ERROR] Failed to query caller processes — aborting.");
        return 22;
    }
    PROCESSENTRY32W pe2{};
    pe2.dwSize = sizeof(pe2);
    DWORD selfPid = ::GetCurrentProcessId();
    if (::Process32FirstW(hSnap2, &pe2)) {
        do {
            if (pe2.th32ProcessID == selfPid) {
                parentPid = pe2.th32ParentProcessID;
                break;
            }
        } while (::Process32NextW(hSnap2, &pe2));
    }
    ::CloseHandle(hSnap2);

    if (parentPid == 0) {
        PrintErr("[ERROR] Unable to resolve parent process — aborting");
        return 22;
    }

    HANDLE hParent = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parentPid);
    if (!hParent) {
        PrintErr("[ERROR] Unable to inspect parent process — aborting");
        return 22;
    }
    wchar_t parentName[1024] = {};
    DWORD parentNameSize = static_cast<DWORD>(std::size(parentName));
    if (!::QueryFullProcessImageNameW(hParent, 0, parentName, &parentNameSize)) {
        ::CloseHandle(hParent);
        PrintErr("[ERROR] Unable to verify caller process identity — aborting");
        return 22;
    }
    ::CloseHandle(hParent);

    if (!IsAllowedCallerImagePath(std::wstring(parentName, parentNameSize))) {
        PrintErr("[ERROR] Unauthorized caller — aborting");
        return 22;
    }

    // Open log file ONLY AFTER auth token + caller verification, with full reparse-point & final-path checks
    if (!job.logPath.empty()) {
        HANDLE hLogCheck = OpenSafeOutputFileHandle(job.logPath, true);
        if (hLogCheck != INVALID_HANDLE_VALUE) {
            ::CloseHandle(hLogCheck);
            _wfopen_s(&g_logFile, job.logPath.c_str(), L"a, ccs=UTF-8");
        }
    }

    // S3.4: Anti-Cheat Protection Tripwire (M-5: covers BattlEye variants and active anti-cheat engines)
    const char* strict_ac_blocklist[] = {
        "vgc.exe",
        "vgtray.exe",
        "easyanticheat.exe",
        "easyanticheat_eos.exe",
        "beservice.exe",
        "beservice_x64.exe",
        "beservice.dll",
        "ricochet.exe",
        "faceitservice.exe"
    };
    HANDLE hSnapAC = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapAC != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W peAC{};
        peAC.dwSize = sizeof(peAC);
        if (::Process32FirstW(hSnapAC, &peAC)) {
            do {
                std::string pName = ToLowerAscii(WideToUtf8(peAC.szExeFile));
                for (const char* blocked : strict_ac_blocklist) {
                    if (pName.find(blocked) != std::string::npos) {
                        PrintErr("[SECURITY] Active Anti-Cheat process detected (%s). Injection REFUSED to protect account from bans.", WideToUtf8(peAC.szExeFile).c_str());
                        ::CloseHandle(hSnapAC);
                        return 15;
                    }
                }
            } while (::Process32NextW(hSnapAC, &peAC));
        }
        ::CloseHandle(hSnapAC);
    }

    DWORD targetPid = job.pid;
    std::wstring dllPath = job.dllPath;
    std::wstring copySrc = job.copySrc;
    std::wstring copyDst = job.copyDst;
    const bool allowUnverifiedHotfix = job.allowUnverifiedHotfix;

    // Validate required arguments
    if (targetPid <= 4 || dllPath.empty()) {
        PrintErr("[ERROR] Missing or invalid required arguments (--pid and --dll). Target PID must be > 4.");
        return 1;
    }

    // S3.1: System Process Protection, Exe/Path Verification & Post-UAC Re-Resolution
    bool pidFound = false;
    std::wstring verifiedTargetExePath;
    for (int retry = 0; retry < 10 && !pidFound; ++retry) {
        std::wstring imgPath;
        SIZE_T ws = 0;
        if (QueryProcessImageAndMem(targetPid, imgPath, ws) && ws >= 1024 * 1024) {
            bool matchesExpected = true;
            if (!job.expectedExeName.empty() &&
                ExtractBasenameLower(imgPath) != ExtractBasenameLower(job.expectedExeName)) {
                matchesExpected = false;
            }
            if (!job.expectedInstallPath.empty() &&
                !IsPathInsideDirectory(imgPath, job.expectedInstallPath)) {
                matchesExpected = false;
            }
            if (matchesExpected) {
                pidFound = true;
                verifiedTargetExePath = imgPath;
                break;
            }
        }
        // If target PID exited during UAC prompt, re-resolve strictly within expectedInstallPath
        if (!job.expectedExeName.empty() && !job.expectedInstallPath.empty()) {
            DWORD reresolved = ResolveLiveGamePid(job.expectedExeName, job.expectedInstallPath);
            if (reresolved > 4 && reresolved != targetPid) {
                std::wstring newImg;
                SIZE_T newWs = 0;
                if (QueryProcessImageAndMem(reresolved, newImg, newWs) && newWs >= 1024 * 1024) {
                    targetPid = reresolved;
                    verifiedTargetExePath = newImg;
                    pidFound = true;
                    PrintInfo("[Injector] Re-resolved live game PID after UAC: %lu", targetPid);
                    break;
                }
            }
        }
        if (!pidFound) {
            ::Sleep(200);
        }
    }

    if (!pidFound) {
        PrintErr("[ERROR] Real Game Process (PID %lu) not found, exited, path mismatch, or insufficient working set.", targetPid);
        return 21;
    }

    // S1.2: Path Traversal, Absolute Path & Reparse Point Checks
    if (HasPathTraversal(dllPath) ||
        (!copySrc.empty() && HasPathTraversal(copySrc)) ||
        (!copyDst.empty() && HasPathTraversal(copyDst))) {
        PrintErr("[ERROR] Path traversal rejected");
        return 10;
    }

    if (!IsAbsoluteDrivePath(dllPath)) {
        PrintErr("[ERROR] Relative DLL paths are blocked for security");
        return 11;
    }
    if (IsReparsePointPath(dllPath) || IsReparsePointPath(GetParentDirW(dllPath))) {
        PrintErr("[SECURITY ERROR] Reparse point / junction rejected for DLL path.");
        return 10;
    }

    // S1.3 & S1.4: Open DLL with FILE_SHARE_READ lock, verify PE + SHA-256 directly from handle
    HANDLE hSourceDll = OpenReadLockedNonReparseFile(dllPath);
    if (hSourceDll == INVALID_HANDLE_VALUE) {
        PrintErr("[ERROR] Failed to open DLL securely (missing, locked, or reparse point): %s", WideToUtf8(dllPath).c_str());
        return 12;
    }

    std::wstring computedHash;
    if (!HashAndVerifyPeFromHandle(hSourceDll, computedHash, true) || computedHash.empty()) {
        ::CloseHandle(hSourceDll);
        PrintErr("[ERROR] Invalid PE header or failed SHA-256 computation for DLL: %s", WideToUtf8(dllPath).c_str());
        return 12;
    }

    if (computedHash != EXPECTED_DLL_HASH) {
        if (allowUnverifiedHotfix) {
            PrintWarn("[SECURITY WARNING] DLL hash differs from build baseline, but --allow-unverified-hotfix was explicitly granted.");
        } else {
            ::CloseHandle(hSourceDll);
            PrintErr("[SECURITY ERROR] DLL hash verification failed!");
            PrintErr("  Expected: %ls", EXPECTED_DLL_HASH);
            PrintErr("  Computed: %ls", computedHash.c_str());
            PrintErr("Injection aborted to prevent untrusted code execution. Use --allow-unverified-hotfix if deploying an authorized OTA hotfix.");
            return 14;
        }
    } else {
        PrintOK("DLL hash verified against build baseline.");
    }

    // Auto-resolve copy paths if omitted
    wchar_t selfExePath[1024] = {};
    ::GetModuleFileNameW(nullptr, selfExePath, static_cast<DWORD>(std::size(selfExePath)));
    std::wstring selfDir = GetParentDirW(selfExePath);

    if (copySrc.empty()) {
        copySrc = selfDir;
    }

    std::wstring targetExeDir = GetParentDirW(verifiedTargetExePath);
    if (copyDst.empty() && !targetExeDir.empty()) {
        copyDst = targetExeDir;
    }

    // Validate copyDst strictly before privileged file copy
    if (!copyDst.empty()) {
        if (IsSystemDirectory(copyDst) || IsReparsePointPath(copyDst)) {
            ::CloseHandle(hSourceDll);
            PrintErr("[SECURITY ERROR] Refusing to deploy dependencies into system directory or reparse point.");
            return 10;
        }
        if (!job.expectedInstallPath.empty() && !IsPathInsideDirectory(copyDst, job.expectedInstallPath)) {
            ::CloseHandle(hSourceDll);
            PrintErr("[SECURITY ERROR] Refusing to deploy dependencies outside verified game install directory.");
            return 10;
        }
        if (job.expectedInstallPath.empty() && !targetExeDir.empty() && !IsPathInsideDirectory(copyDst, targetExeDir) &&
            !IsSameFileW(copyDst, selfDir)) {
            ::CloseHandle(hSourceDll);
            PrintErr("[SECURITY ERROR] Refusing to deploy dependencies to unrelated destination directory.");
            return 10;
        }
    }

    // Copy dependencies to game directory using elevated permissions (ONLY verified binaries!)
    if (!copySrc.empty() && !copyDst.empty()) {
        PrintInfo("Synchronizing engine dependencies from %s to %s", WideToUtf8(copySrc).c_str(), WideToUtf8(copyDst).c_str());

        std::vector<std::wstring> srcDirs;
        size_t start = 0;
        while (start < copySrc.size()) {
            size_t delim = copySrc.find_first_of(L";,\n\r", start);
            std::wstring dir = (delim == std::wstring::npos) ? copySrc.substr(start) : copySrc.substr(start, delim - start);
            while (!dir.empty() && (dir.front() == L' ' || dir.front() == L'"' || dir.front() == L'\'')) dir.erase(dir.begin());
            while (!dir.empty() && (dir.back() == L' ' || dir.back() == L'"' || dir.back() == L'\'')) dir.pop_back();
            if (!dir.empty() && IsAbsoluteDrivePath(dir) && !HasPathTraversal(dir) && !IsReparsePointPath(dir) &&
                ::GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) {
                srcDirs.push_back(dir);
            }
            if (delim == std::wstring::npos) break;
            start = delim + 1;
        }
        if (srcDirs.empty() && !selfDir.empty() && !IsReparsePointPath(selfDir)) {
            srcDirs.push_back(selfDir);
        }

        // 1. Deploy vrinject.dll directly from the ALREADY-VERIFIED hSourceDll handle (zero TOCTOU)
        const std::wstring verifiedRoot = !job.expectedInstallPath.empty() ? job.expectedInstallPath : copyDst;
        std::wstring targetDllDst = copyDst + L"\\vrinject.dll";
        if (!IsSameFileW(dllPath, targetDllDst)) {
            if (CopyFromVerifiedHandle(hSourceDll, targetDllDst, verifiedRoot)) {
                PrintOK("Deployed vrinject.dll to target directory from verified handle.");
                dllPath = targetDllDst;
            } else {
                PrintWarn("Could not deploy vrinject.dll to target directory (Error: %lu). Retaining verified source DLL path.", ::GetLastError());
            }
        }

        // 2. Synchronize support runtime DLLs (onnxruntime.dll, DirectML.dll) ONLY after SHA-256 verification!
        struct SupportDllSpec {
            const wchar_t* name;
            const wchar_t* expectedHash;
        };
        const SupportDllSpec supportDlls[] = {
            { L"onnxruntime.dll", EXPECTED_ONNXRUNTIME_HASH },
            { L"DirectML.dll",    EXPECTED_DIRECTML_HASH },
        };
        for (const auto& spec : supportDlls) {
            std::wstring dst = copyDst + L"\\" + spec.name;
            for (const auto& d : srcDirs) {
                std::wstring src = d + L"\\" + spec.name;
                if (::GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
                if (IsSameFileW(src, dst)) break;

                HANDLE hSupport = OpenReadLockedNonReparseFile(src);
                if (hSupport == INVALID_HANDLE_VALUE) {
                    PrintWarn("[SECURITY] Skipping untrusted or reparse-point support DLL: %s", WideToUtf8(src).c_str());
                    continue;
                }
                std::wstring supHash;
                bool peOk = HashAndVerifyPeFromHandle(hSupport, supHash, true);
                bool hashMatches = peOk && !supHash.empty() &&
                                   (spec.expectedHash[0] != L'\0') &&
                                   (supHash == spec.expectedHash);
                if (!hashMatches) {
                    ::CloseHandle(hSupport);
                    PrintErr("[SECURITY ERROR] Support DLL %ls failed SHA-256 verification from %s — refusing privileged copy!",
                             spec.name, WideToUtf8(src).c_str());
                    continue;
                }
                if (CopyFromVerifiedHandle(hSupport, dst, verifiedRoot)) {
                    PrintOK("Verified and deployed %ls to target directory.", spec.name);
                }
                ::CloseHandle(hSupport);
                break;
            }
        }

        // 3. Synchronize vrinject.json ONLY if destination does not already exist
        std::wstring jsonDst = copyDst + L"\\vrinject.json";
        if (::GetFileAttributesW(jsonDst.c_str()) == INVALID_FILE_ATTRIBUTES && !IsReparsePointPath(jsonDst)) {
            for (const auto& d : srcDirs) {
                std::wstring jsonSrc = d + L"\\vrinject.json";
                if (::GetFileAttributesW(jsonSrc.c_str()) != INVALID_FILE_ATTRIBUTES && !IsSameFileW(jsonSrc, jsonDst)) {
                    HANDLE hJson = OpenReadLockedNonReparseFile(jsonSrc);
                    if (hJson != INVALID_HANDLE_VALUE) {
                        if (CopyFromVerifiedHandle(hJson, jsonDst, verifiedRoot)) {
                            PrintOK("Deployed initial vrinject.json to target directory.");
                        }
                        ::CloseHandle(hJson);
                    }
                    break;
                }
            }
        } else {
            PrintInfo("Preserving existing per-game vrinject.json in target directory.");
        }
    }

    // Hold a FILE_SHARE_READ lock on the final DLL path ACROSS InjectDll to eliminate TOCTOU!
    HANDLE hFinalDllLock = hSourceDll;
    if (!IsSameFileW(dllPath, job.dllPath)) {
        ::CloseHandle(hSourceDll);
        hSourceDll = INVALID_HANDLE_VALUE;

        hFinalDllLock = OpenReadLockedNonReparseFile(dllPath);
        if (hFinalDllLock == INVALID_HANDLE_VALUE) {
            PrintErr("[ERROR] Unable to lock target DLL for final pre-injection verification: %s", WideToUtf8(dllPath).c_str());
            return 12;
        }
        std::wstring finalHash;
        if (!HashAndVerifyPeFromHandle(hFinalDllLock, finalHash, true) || finalHash.empty()) {
            ::CloseHandle(hFinalDllLock);
            PrintErr("[ERROR] Target DLL image corrupted before injection");
            return 12;
        }
        // Always require deployed bytes to match the originally verified source hash (even with --allow-unverified-hotfix)
        if (finalHash != computedHash || (finalHash != EXPECTED_DLL_HASH && !allowUnverifiedHotfix)) {
            ::CloseHandle(hFinalDllLock);
            PrintErr("[SECURITY ERROR] Final DLL hash check failed immediately before injection!");
            return 14;
        }
    }

    PrintInfo("Target PID:  %lu", targetPid);
    PrintInfo("DLL path:    %s", WideToUtf8(dllPath).c_str());
    std::printf("\n");

    // hFinalDllLock remains open with FILE_SHARE_READ only throughout LoadLibraryW retries!
    bool ok = InjectDll(targetPid, dllPath);

    if (hFinalDllLock != INVALID_HANDLE_VALUE) {
        ::CloseHandle(hFinalDllLock);
    }

    // Persist resolved PID back to launcher so activeTargetPid/monitor/cancel stay in sync
    if (ok && !job.resultFile.empty()) {
        HANDLE hRes = OpenSafeOutputFileHandle(job.resultFile, false);
        if (hRes != INVALID_HANDLE_VALUE) {
            char payload[64] = {};
            int len = std::snprintf(payload, sizeof(payload), "{\"resolvedPid\":%lu}\n", targetPid);
            if (len > 0) {
                DWORD written = 0;
                ::WriteFile(hRes, payload, static_cast<DWORD>(len), &written, nullptr);
            }
            ::CloseHandle(hRes);
        }
    }

    std::printf("\n");
    if (ok) {
        PrintOK("Injection complete. Check vrinject.log for hook status.");
    } else {
        PrintErr("Injection failed. See messages above for details.");
    }

    SetColour(Colour::Default);
    return ok ? 0 : 1;
}
