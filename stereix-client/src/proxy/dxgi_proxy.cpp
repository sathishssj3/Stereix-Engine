// dxgi_proxy.cpp – DXGI proxy DLL entry point
//
// This file compiles as dxgi.dll and forwards all real DXGI exports to the
// system dxgi.dll located in the Windows System32 directory.
//
// Hardened against loader-lock violations (M-1/HIGH-06), TOCTOU (CRIT-02), and
// persistence vector (H-1): System DLL resolution and vrinject chain-loading are
// deferred outside DllMain, vrinject.dll is opened read-locked (FILE_SHARE_READ
// only, no reparse points) and verified via SHA-256 against EXPECTED_DLL_HASH
// while holding the handle across LoadLibraryW, and FreeLibrary is never called
// inside DllMain.

#include <windows.h>
#include <mutex>
#include <vector>
#include <string>
#include <bcrypt.h>
#include "expected_hash.h"

#pragma comment(lib, "bcrypt.lib")

namespace {

static HMODULE g_realDxgi = nullptr;
static HMODULE g_proxyModule = nullptr;
static std::once_flag g_initOnce;

bool VerifyAndLoadLockedDll(const wchar_t* path) {
    if (!path || !*path) return false;
    DWORD attrs = GetFileAttributesW(path);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
        return false;
    }

    // Open with FILE_SHARE_READ only so writers/deleters cannot swap the file between hash and LoadLibraryW
    HANDLE hFile = CreateFileW(
        path,
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(hFile, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
        CloseHandle(hFile);
        return false;
    }

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        CloseHandle(hFile);
        return false;
    }

    DWORD cbData = 0, cbHashObject = 0, cbHash = 0;
    if (BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PBYTE>(&cbHashObject), sizeof(DWORD), &cbData, 0) != 0 ||
        BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, reinterpret_cast<PBYTE>(&cbHash), sizeof(DWORD), &cbData, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        CloseHandle(hFile);
        return false;
    }

    std::vector<BYTE> pbHashObject(cbHashObject);
    std::vector<BYTE> pbHash(cbHash);
    if (BCryptCreateHash(hAlg, &hHash, pbHashObject.data(), cbHashObject, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        CloseHandle(hFile);
        return false;
    }

    BYTE buffer[8192];
    DWORD readBytes = 0;
    bool firstChunk = true;
    bool peValid = false;
    bool hashOk = false;

    while (ReadFile(hFile, buffer, sizeof(buffer), &readBytes, nullptr) && readBytes > 0) {
        if (firstChunk) {
            firstChunk = false;
            if (readBytes >= 2 && buffer[0] == 'M' && buffer[1] == 'Z') {
                peValid = true;
            } else {
                break;
            }
        }
        if (BCryptHashData(hHash, buffer, readBytes, 0) != 0) {
            peValid = false;
            break;
        }
    }

    if (peValid && BCryptFinishHash(hHash, pbHash.data(), cbHash, 0) == 0) {
        std::wstring computedHash;
        computedHash.reserve(static_cast<size_t>(cbHash) * 2);
        wchar_t hex[3];
        for (DWORD i = 0; i < cbHash; ++i) {
            swprintf_s(hex, L"%02X", pbHash[i]);
            computedHash += hex;
        }
        hashOk = (computedHash == EXPECTED_DLL_HASH);
    }

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    bool loaded = false;
    if (hashOk) {
        // Hold hFile open across LoadLibraryW to eliminate TOCTOU
        loaded = (LoadLibraryW(path) != nullptr);
    }

    CloseHandle(hFile);
    return loaded;
}

void EnsureInitialized() {
    std::call_once(g_initOnce, []() {
        // Step 1: Resolve system dxgi.dll from System32
        wchar_t sysDir[MAX_PATH] = {};
        UINT len = GetSystemDirectoryW(sysDir, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            std::wstring realPath(sysDir, len);
            realPath += L"\\dxgi.dll";
            g_realDxgi = LoadLibraryW(realPath.c_str());
        }

        // Step 2: Verify and chain-load vrinject.dll without TOCTOU
        if (g_proxyModule) {
            wchar_t szPath[1024] = {};
            DWORD pathLen = GetModuleFileNameW(g_proxyModule, szPath, static_cast<DWORD>(std::size(szPath)));
            if (pathLen > 0 && pathLen < std::size(szPath)) {
                std::wstring dllPath(szPath, pathLen);
                size_t lastSlash = dllPath.find_last_of(L'\\');
                if (lastSlash != std::wstring::npos) {
                    dllPath.resize(lastSlash + 1);
                    dllPath += L"vrinject.dll";
                    VerifyAndLoadLockedDll(dllPath.c_str());
                }
            }
        }
    });
}

FARPROC GetRealProc(const char* name) {
    EnsureInitialized();
    if (!g_realDxgi) return nullptr;
    return GetProcAddress(g_realDxgi, name);
}

} // anonymous namespace

extern "C" {

__declspec(dllexport) void WINAPI ApplyCOMPats() {
    auto fn = (void(WINAPI*)())GetRealProc("ApplyCOMPats");
    if (fn) fn();
}

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory) {
    auto fn = (HRESULT(WINAPI*)(REFIID, void**))GetRealProc("CreateDXGIFactory");
    return fn ? fn(riid, ppFactory) : E_FAIL;
}

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    auto fn = (HRESULT(WINAPI*)(REFIID, void**))GetRealProc("CreateDXGIFactory1");
    return fn ? fn(riid, ppFactory) : E_FAIL;
}

__declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory) {
    auto fn = (HRESULT(WINAPI*)(UINT, REFIID, void**))GetRealProc("CreateDXGIFactory2");
    return fn ? fn(Flags, riid, ppFactory) : E_FAIL;
}

__declspec(dllexport) HRESULT WINAPI DXGID3D10CreateDevice(
    HMODULE d3d10core, void* pFactory, void* pAdapter,
    UINT Flags, void* pUnknown, void** ppDevice)
{
    typedef HRESULT(WINAPI* Fn)(HMODULE, void*, void*, UINT, void*, void**);
    auto fn = (Fn)GetRealProc("DXGID3D10CreateDevice");
    return fn ? fn(d3d10core, pFactory, pAdapter, Flags, pUnknown, ppDevice) : E_FAIL;
}

__declspec(dllexport) HRESULT WINAPI DXGID3D10CreateLayeredDevice(void* pUnknown) {
    typedef HRESULT(WINAPI* Fn)(void*);
    auto fn = (Fn)GetRealProc("DXGID3D10CreateLayeredDevice");
    return fn ? fn(pUnknown) : E_FAIL;
}

__declspec(dllexport) SIZE_T WINAPI DXGID3D10GetLayeredDeviceSize(const void* pLayers, UINT NumLayers) {
    typedef SIZE_T(WINAPI* Fn)(const void*, UINT);
    auto fn = (Fn)GetRealProc("DXGID3D10GetLayeredDeviceSize");
    return fn ? fn(pLayers, NumLayers) : 0;
}

__declspec(dllexport) HRESULT WINAPI DXGID3D10RegisterLayers(const void* pLayers, UINT NumLayers) {
    typedef HRESULT(WINAPI* Fn)(const void*, UINT);
    auto fn = (Fn)GetRealProc("DXGID3D10RegisterLayers");
    return fn ? fn(pLayers, NumLayers) : E_FAIL;
}

__declspec(dllexport) HRESULT WINAPI DXGIDeclareAdapterRemovalSupport() {
    auto fn = (HRESULT(WINAPI*)())GetRealProc("DXGIDeclareAdapterRemovalSupport");
    return fn ? fn() : E_FAIL;
}

__declspec(dllexport) HRESULT WINAPI DXGIGetDebugInterface1(UINT Flags, REFIID riid, void** pDebug) {
    auto fn = (HRESULT(WINAPI*)(UINT, REFIID, void**))GetRealProc("DXGIGetDebugInterface1");
    return fn ? fn(Flags, riid, pDebug) : E_FAIL;
}

__declspec(dllexport) void WINAPI DXGIReportAdapterConfiguration() {
    auto fn = (void(WINAPI*)())GetRealProc("DXGIReportAdapterConfiguration");
    if (fn) fn();
}

} // extern "C"

// ---------------------------------------------------------------------------
// DllMain — never calls FreeLibrary under loader lock (HIGH-06)
// ---------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_proxyModule = hModule;
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        // Do NOT call FreeLibrary inside DllMain (loader-lock deadlock violation).
        // The OS loader reclaims all module handles safely on process exit.
        g_realDxgi = nullptr;
    }
    return TRUE;
}
