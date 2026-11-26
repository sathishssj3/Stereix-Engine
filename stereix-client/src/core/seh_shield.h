#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <psapi.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include "core/logger.h"

#pragma comment(lib, "psapi.lib")

namespace vrinject {
namespace seh {

// Module bounds for vrinject.dll (single process-wide inline atomics, HIGH-06)
inline std::atomic<ULONG_PTR>& GetVrinjectBase() {
    static std::atomic<ULONG_PTR> s_base{0};
    return s_base;
}

inline std::atomic<ULONG_PTR>& GetVrinjectEnd() {
    static std::atomic<ULONG_PTR> s_end{0};
    return s_end;
}

inline std::atomic<PVOID>& GetVehHandle() {
    static std::atomic<PVOID> s_vehHandle{nullptr};
    return s_vehHandle;
}

// Lock-free diagnostic counters populated inside first-chance VEH (never lock/allocate in VEH, HIGH-06)
inline std::atomic<uint64_t>& GetVehObservedCount() {
    static std::atomic<uint64_t> s_count{0};
    return s_count;
}
inline std::atomic<DWORD>& GetVehLastCode() {
    static std::atomic<DWORD> s_code{0};
    return s_code;
}
inline std::atomic<ULONG_PTR>& GetVehLastAddress() {
    static std::atomic<ULONG_PTR> s_lastAddress{0};
    return s_lastAddress;
}

/// Check if a pointer is a valid memory address for safe COM / vtable dereferencing.
/// Null, near-null (e.g. 0x1, 0x5, 0xFFFFFFFF), or unaligned pointers return false.
inline bool IsValidMemoryPointer(const void* ptr, size_t requiredAlignment = alignof(void*)) {
    if (!ptr) return false;
    ULONG_PTR addr = reinterpret_cast<ULONG_PTR>(ptr);
    // Minimum user-mode valid memory address in 64-bit Windows is 0x10000 (64KB)
    if (addr < 0x10000 || addr >= 0x7FFFFFFFFFFF) return false;
    if (requiredAlignment > 1 && (addr % requiredAlignment != 0)) return false;
    return true;
}

// Safe memory read utilizing Windows Structured Exception Handling (SEH).
// Wraps the read to prevent Access Violations (0xC0000005) from crashing the game
// if the memory is freed by the engine concurrently (TOCTOU).
// Note: This is explicitly MSVC/Windows specific and not cross-platform.
inline bool SafeReadMemory(const void* src, void* dst, size_t size) {
    if (!src || !dst || size == 0) return false;

    __try {
        // Attempt the volatile read. If src points to a page that was just freed,
        // this will trigger an Access Violation exception.
        std::memcpy(dst, src, size);
        return true;
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        // Discard candidate safely; debug log only so memory sweeps do not flood disk I/O in release
        LOG_DEBUG("SEH Shield: Access Violation (TOCTOU) intercepted while reading %zu bytes from %p. Discarding candidate.", size, src);
        // Ensure dst is zeroed out so garbage data is never propagated
        std::memset(dst, 0, size);
        return false;
    }
}

// Safe memory write utilizing Windows Structured Exception Handling (SEH).
// Wraps the write to prevent Access Violations (0xC0000005) from crashing the game
// if the memory is freed by the engine concurrently or is read-only.
inline bool SafeWriteMemory(void* dst, const void* src, size_t size) {
    if (!src || !dst || size == 0) return false;

    __try {
        std::memcpy(dst, src, size);
        return true;
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        LOG_WARN("SEH Shield: Access Violation intercepted while writing %zu bytes to %p.", size, dst);
        return false;
    }
}

/// Vectored Exception Handler to observe hardware exceptions inside vrinject.dll.
/// Strictly lock-free and allocation-free (HIGH-06): NEVER calls LOG_ERROR/mutex/malloc inside VEH.
inline LONG WINAPI VrinjectVehHandler(PEXCEPTION_POINTERS pExceptionInfo) {
    if (!pExceptionInfo || !pExceptionInfo->ExceptionRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const ULONG_PTR crashAddr = reinterpret_cast<ULONG_PTR>(pExceptionInfo->ExceptionRecord->ExceptionAddress);
    const ULONG_PTR base = GetVrinjectBase().load(std::memory_order_acquire);
    const ULONG_PTR end = GetVrinjectEnd().load(std::memory_order_acquire);

    if (base != 0 && end != 0 && crashAddr >= base && crashAddr < end) {
        GetVehLastCode().store(pExceptionInfo->ExceptionRecord->ExceptionCode, std::memory_order_relaxed);
        GetVehLastAddress().store(crashAddr, std::memory_order_relaxed);
        GetVehObservedCount().fetch_add(1, std::memory_order_relaxed);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

inline void RegisterVehShield(HMODULE hModule) {
    if (!hModule) return;
    
    MODULEINFO modInfo{};
    if (GetModuleInformation(GetCurrentProcess(), hModule, &modInfo, sizeof(modInfo))) {
        const ULONG_PTR base = reinterpret_cast<ULONG_PTR>(modInfo.lpBaseOfDll);
        const ULONG_PTR end = base + modInfo.SizeOfImage;
        GetVrinjectBase().store(base, std::memory_order_release);
        GetVrinjectEnd().store(end, std::memory_order_release);
        LOG_INFO("[VEH SHIELD] Registered vrinject memory bounds: 0x%p - 0x%p",
                 reinterpret_cast<void*>(base),
                 reinterpret_cast<void*>(end));
    }

    PVOID expected = nullptr;
    if (GetVehHandle().load(std::memory_order_acquire) == nullptr) {
        PVOID handle = AddVectoredExceptionHandler(1, VrinjectVehHandler);
        if (handle) {
            if (GetVehHandle().compare_exchange_strong(expected, handle, std::memory_order_acq_rel)) {
                LOG_INFO("[VEH SHIELD] Universal Vectored Exception Handler successfully attached.");
            } else {
                RemoveVectoredExceptionHandler(handle);
            }
        }
    }
}

inline void UnregisterVehShield() {
    PVOID handle = GetVehHandle().exchange(nullptr, std::memory_order_acq_rel);
    if (handle) {
        RemoveVectoredExceptionHandler(handle);
        LOG_INFO("[VEH SHIELD] Vectored Exception Handler detached (observed %llu in-module exceptions).",
                 static_cast<unsigned long long>(GetVehObservedCount().load(std::memory_order_relaxed)));
    }
}

} // namespace seh
} // namespace vrinject
