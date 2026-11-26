// d3d11_proxy.cpp – D3D11 proxy DLL entry point
//
// Compiles as d3d11.dll and forwards all real Direct3D 11 exports to the
// genuine system d3d11.dll located in the Windows System32 directory.
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

static HMODULE g_realD3D11 = nullptr;
static HMODULE g_proxyModule = nullptr;
static std::once_flag g_initOnce;

bool VerifyAndLoadLockedDll(const wchar_t* path) {
    if (!path || !*path) return false;
    DWORD attrs = GetFileAttributesW(path);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) {
        return false;
    }

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
        // Step 1: Resolve system d3d11.dll from System32
        wchar_t sysDir[MAX_PATH] = {};
        UINT len = GetSystemDirectoryW(sysDir, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            std::wstring realPath(sysDir, len);
            realPath += L"\\d3d11.dll";
            g_realD3D11 = LoadLibraryW(realPath.c_str());
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
    if (!g_realD3D11) return nullptr;
    return GetProcAddress(g_realD3D11, name);
}

} // anonymous namespace

extern "C" {

__declspec(dllexport) HRESULT WINAPI D3D11CreateDevice(
    void* pAdapter,
    int DriverType,
    HMODULE Software,
    UINT Flags,
    const void* pFeatureLevels,
    UINT FeatureLevels,
    UINT SDKVersion,
    void** ppDevice,
    void* pFeatureLevel,
    void** ppImmediateContext)
{
    typedef HRESULT (WINAPI *Fn)(void*, int, HMODULE, UINT,
                                const void*, UINT, UINT,
                                void**, void*, void**);
    auto fn = (Fn)GetRealProc("D3D11CreateDevice");
    return fn ? fn(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
                    SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext) : E_FAIL;
}

__declspec(dllexport) HRESULT WINAPI D3D11CreateDeviceAndSwapChain(
    void* pAdapter,
    int DriverType,
    HMODULE Software,
    UINT Flags,
    const void* pFeatureLevels,
    UINT FeatureLevels,
    UINT SDKVersion,
    const void* pSwapChainDesc,
    void** ppSwapChain,
    void** ppDevice,
    void* pFeatureLevel,
    void** ppImmediateContext)
{
    typedef HRESULT (WINAPI *Fn)(void*, int, HMODULE, UINT,
                                const void*, UINT, UINT,
                                const void*, void**,
                                void**, void*, void**);
    auto fn = (Fn)GetRealProc("D3D11CreateDeviceAndSwapChain");
    return fn ? fn(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
                    SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext) : E_FAIL;
}

__declspec(dllexport) HRESULT WINAPI D3D11On12CreateDevice(
    IUnknown* pDevice,
    UINT Flags,
    const void* pFeatureLevels,
    UINT FeatureLevels,
    IUnknown* const* ppCommandQueues,
    UINT NumQueues,
    UINT NodeMask,
    void** ppDevice11,
    void** ppImmediateContext11,
    void* pChosenFeatureLevel)
{
    typedef HRESULT (WINAPI *Fn)(IUnknown*, UINT, const void*, UINT,
                                IUnknown* const*, UINT, UINT,
                                void**, void**, void*);
    auto fn = (Fn)GetRealProc("D3D11On12CreateDevice");
    return fn ? fn(pDevice, Flags, pFeatureLevels, FeatureLevels, ppCommandQueues,
                    NumQueues, NodeMask, ppDevice11, ppImmediateContext11, pChosenFeatureLevel) : E_FAIL;
}

__declspec(dllexport) HRESULT WINAPI D3D11CoreCreateDevice(
    void* pFactory,
    void* pAdapter,
    UINT Flags,
    const void* pFeatureLevels,
    UINT FeatureLevels,
    void** ppDevice)
{
    typedef HRESULT (WINAPI *Fn)(void*, void*, UINT, const void*, UINT, void**);
    auto fn = (Fn)GetRealProc("D3D11CoreCreateDevice");
    return fn ? fn(pFactory, pAdapter, Flags, pFeatureLevels, FeatureLevels, ppDevice) : E_FAIL;
}

__declspec(dllexport) HRESULT WINAPI D3D11CoreCreateLayeredDevice(
    const void* pUnknown,
    DWORD unknown1,
    const void* unknown2,
    REFIID riid,
    void** ppDevice)
{
    typedef HRESULT (WINAPI *Fn)(const void*, DWORD, const void*, REFIID, void**);
    auto fn = (Fn)GetRealProc("D3D11CoreCreateLayeredDevice");
    return fn ? fn(pUnknown, unknown1, unknown2, riid, ppDevice) : E_FAIL;
}

__declspec(dllexport) SIZE_T WINAPI D3D11CoreGetLayeredDeviceSize(const void* pUnknown, DWORD unknown1) {
    typedef SIZE_T (WINAPI *Fn)(const void*, DWORD);
    auto fn = (Fn)GetRealProc("D3D11CoreGetLayeredDeviceSize");
    return fn ? fn(pUnknown, unknown1) : 0;
}

__declspec(dllexport) HRESULT WINAPI D3D11CoreRegisterLayers(const void* pUnknown, DWORD unknown1) {
    typedef HRESULT (WINAPI *Fn)(const void*, DWORD);
    auto fn = (Fn)GetRealProc("D3D11CoreRegisterLayers");
    return fn ? fn(pUnknown, unknown1) : E_FAIL;
}

__declspec(dllexport) HRESULT WINAPI D3D11CreateDeviceForD3D12(
    IUnknown* pDevice,
    UINT Flags,
    const void* pFeatureLevels,
    UINT FeatureLevels,
    UINT NumQueues,
    IUnknown* const* ppCommandQueues,
    UINT NodeMask,
    void** ppDevice11,
    void** ppImmediateContext11,
    void* pChosenFeatureLevel)
{
    typedef HRESULT (WINAPI *Fn)(IUnknown*, UINT, const void*, UINT,
                                UINT, IUnknown* const*, UINT,
                                void**, void**, void*);
    auto fn = (Fn)GetRealProc("D3D11CreateDeviceForD3D12");
    return fn ? fn(pDevice, Flags, pFeatureLevels, FeatureLevels, NumQueues, ppCommandQueues,
                    NodeMask, ppDevice11, ppImmediateContext11, pChosenFeatureLevel) : E_FAIL;
}

__declspec(dllexport) void WINAPI D3DPerformance_BeginEvent(DWORD col, LPCWSTR wszName) {
    typedef void (WINAPI *Fn)(DWORD, LPCWSTR);
    auto fn = (Fn)GetRealProc("D3DPerformance_BeginEvent");
    if (fn) fn(col, wszName);
}

__declspec(dllexport) void WINAPI D3DPerformance_EndEvent() {
    typedef void (WINAPI *Fn)();
    auto fn = (Fn)GetRealProc("D3DPerformance_EndEvent");
    if (fn) fn();
}

__declspec(dllexport) DWORD WINAPI D3DPerformance_GetStatus() {
    typedef DWORD (WINAPI *Fn)();
    auto fn = (Fn)GetRealProc("D3DPerformance_GetStatus");
    return fn ? fn() : 0;
}

__declspec(dllexport) void WINAPI D3DPerformance_SetMarker(DWORD col, LPCWSTR wszName) {
    typedef void (WINAPI *Fn)(DWORD, LPCWSTR);
    auto fn = (Fn)GetRealProc("D3DPerformance_SetMarker");
    if (fn) fn(col, wszName);
}

__declspec(dllexport) HRESULT WINAPI CreateDirect3D11DeviceFromDXGIDevice(
    void* dxgiDevice,
    IUnknown** graphicsDevice)
{
    typedef HRESULT (WINAPI *Fn)(void*, IUnknown**);
    auto fn = (Fn)GetRealProc("CreateDirect3D11DeviceFromDXGIDevice");
    return fn ? fn(dxgiDevice, graphicsDevice) : E_FAIL;
}

__declspec(dllexport) HRESULT WINAPI CreateDirect3D11SurfaceFromDXGISurface(
    void* dgxiSurface,
    IUnknown** graphicsSurface)
{
    typedef HRESULT (WINAPI *Fn)(void*, IUnknown**);
    auto fn = (Fn)GetRealProc("CreateDirect3D11SurfaceFromDXGISurface");
    return fn ? fn(dgxiSurface, graphicsSurface) : E_FAIL;
}

} // extern "C"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_proxyModule = hModule;
    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        // Do NOT call FreeLibrary inside DllMain (loader-lock deadlock violation, HIGH-06).
        g_realD3D11 = nullptr;
    }
    return TRUE;
}
