#include "memory_scanner/page_scanner.h"
#include "memory_scanner/pointer_chain_resolver.h"
#include "core/logger.h"
#include "core/subsystem_context.h"
#include "core/seh_shield.h"
#include <Psapi.h>
#include <chrono>
#include <cmath>
#include <algorithm>

namespace vrinject {

bool PageScanner::Initialize() {
    auto info = GetModuleInfo("");
    if (!info.baseAddress) {
        LOG_ERROR("PageScanner failed to get main module info.");
        return false;
    }
    m_mainModuleBase = info.baseAddress;
    m_mainModuleSize = info.size;
    SubsystemContext::Get().GetPointerChainResolver()->SetModuleBounds(m_mainModuleBase, m_mainModuleSize);
    LOG_INFO("PageScanner initialized. Base: %p, Size: %zx", m_mainModuleBase, m_mainModuleSize);

    // Fast-path: check if a valid pointer chain was cached from a previous session
    char exePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) > 0) {
        std::string fullPath(exePath);
        size_t lastSlash = fullPath.find_last_of("\\/");
        std::string exeName = (lastSlash != std::string::npos) ? fullPath.substr(lastSlash + 1) : fullPath;
        
        PointerChain cachedChain;
        if (SubsystemContext::Get().GetPointerChainResolver()->LoadCache(exeName, cachedChain)) {
            uint8_t* ptr = SubsystemContext::Get().GetPointerChainResolver()->DereferenceChain(cachedChain);
            if (ptr) {
                float testMat[16];
                if (seh::SafeReadMemory(ptr, testMat, sizeof(testMat))) {
                    if (IsValidViewMatrixFloat(testMat)) {
                        LOG_INFO("PageScanner: Fast-path instant camera lock using cached pointer chain at %p!", ptr);
                        MemoryMatrixCandidate cand;
                        cand.address = ptr;
                        cand.isDoublePrecision = cachedChain.isDoublePrecision;
                        cand.isViewMatrix = true;
                        cand.isProjectionMatrix = false;
                        
                        std::lock_guard<std::mutex> lock(m_candidatesMutex);
                        m_cachedCandidates.push_back(cand);
                        m_dynamicCandidates.push_back(cand);
                        m_candidatePointers.push_back(ptr);
                    }
                }
            }
        }
    }

    return true;
}

PageScanner::ModuleInfo PageScanner::GetModuleInfo(const std::string& moduleName) {
    ModuleInfo info;
    HMODULE hModule = nullptr;
    if (moduleName.empty()) {
        hModule = GetModuleHandle(nullptr);
    } else {
        hModule = GetModuleHandleA(moduleName.c_str());
    }

    if (hModule) {
        MODULEINFO moduleInfo;
        if (GetModuleInformation(GetCurrentProcess(), hModule, &moduleInfo, sizeof(moduleInfo))) {
            info.baseAddress = static_cast<uint8_t*>(moduleInfo.lpBaseOfDll);
            info.size = moduleInfo.SizeOfImage;
        }
    }
    return info;
}

void PageScanner::StartDynamicScan(float targetFov) {
    if (m_scanRunning) return;
    m_targetFov = targetFov;
    m_scanRunning = true;
    m_scanThread = std::thread(&PageScanner::ScanDynamicHeaps, this);
    LOG_INFO("PageScanner: Async dynamic scan thread started (Target FOV: %.1f).", targetFov);
}

void PageScanner::StopDynamicScan() {
    {
        // Hold m_sleepMutex while clearing m_scanRunning to eliminate lost-wakeup race on 30s deep sleep (MED-02)
        std::lock_guard<std::mutex> lk(m_sleepMutex);
        if (!m_scanRunning.load(std::memory_order_acquire) && !m_scanThread.joinable()) {
            return;
        }
        m_scanRunning.store(false, std::memory_order_release);
    }
    m_wakeCv.notify_all();
    if (m_scanThread.joinable()) {
        m_scanThread.join();
        LOG_INFO("PageScanner: Async dynamic scan thread stopped.");
    }
}

std::vector<MemoryMatrixCandidate> PageScanner::GetDynamicCandidates() {
    std::vector<MemoryMatrixCandidate> result;
    if (m_candidatesMutex.try_lock()) {
        result = m_dynamicCandidates;
        m_candidatesMutex.unlock();
    }
    return result;
}

std::vector<uint8_t*> PageScanner::GetCandidateStaticPointers() {
    std::vector<uint8_t*> result;
    if (m_candidatesMutex.try_lock()) {
        result = m_candidatePointers;
        m_candidatesMutex.unlock();
    }
    return result;
}

bool PageScanner::IsValidProjectionMatrixFloat(const float* mat, float targetFov) {
    if (!mat) return false;
    bool rowMajor = (std::abs(std::abs(mat[11]) - 1.0f) <= 0.05f && std::abs(mat[15]) <= 0.01f);
    bool colMajor = (std::abs(std::abs(mat[14]) - 1.0f) <= 0.05f && std::abs(mat[15]) <= 0.01f);
    if (!rowMajor && !colMajor) return false;
    if (mat[0] <= 0.05f || mat[0] > 20.0f) return false;
    if (mat[5] <= 0.05f || mat[5] > 20.0f) return false;
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(mat[i])) return false;
    }
    return true;
}

bool PageScanner::IsValidProjectionMatrixDouble(const double* mat, float targetFov) {
    if (!mat) return false;
    bool rowMajor = (std::abs(std::abs(mat[11]) - 1.0) <= 0.05 && std::abs(mat[15]) <= 0.01);
    bool colMajor = (std::abs(std::abs(mat[14]) - 1.0) <= 0.05 && std::abs(mat[15]) <= 0.01);
    if (!rowMajor && !colMajor) return false;
    if (mat[0] <= 0.05 || mat[0] > 20.0) return false;
    if (mat[5] <= 0.05 || mat[5] > 20.0) return false;
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(mat[i])) return false;
    }
    return true;
}

bool PageScanner::IsValidViewMatrixFloat(const float* mat) {
    if (!mat) return false;

    // Check row-major or col-major orthonormal rotation block
    float l0 = mat[0]*mat[0] + mat[1]*mat[1] + mat[2]*mat[2];
    float l1 = mat[4]*mat[4] + mat[5]*mat[5] + mat[6]*mat[6];
    float l2 = mat[8]*mat[8] + mat[9]*mat[9] + mat[10]*mat[10];
    bool rowsNormalized = (std::abs(l0 - 1.0f) < 0.12f && std::abs(l1 - 1.0f) < 0.12f && std::abs(l2 - 1.0f) < 0.12f);

    float c0 = mat[0]*mat[0] + mat[4]*mat[4] + mat[8]*mat[8];
    float c1 = mat[1]*mat[1] + mat[5]*mat[5] + mat[9]*mat[9];
    float c2 = mat[2]*mat[2] + mat[6]*mat[6] + mat[10]*mat[10];
    bool colsNormalized = (std::abs(c0 - 1.0f) < 0.12f && std::abs(c1 - 1.0f) < 0.12f && std::abs(c2 - 1.0f) < 0.12f);

    if (!rowsNormalized && !colsNormalized) return false;

    if (rowsNormalized) {
        float dot01 = mat[0]*mat[4] + mat[1]*mat[5] + mat[2]*mat[6];
        float dot12 = mat[4]*mat[8] + mat[5]*mat[9] + mat[6]*mat[10];
        float dot02 = mat[0]*mat[8] + mat[1]*mat[9] + mat[2]*mat[10];
        if (std::abs(dot01) > 0.12f || std::abs(dot12) > 0.12f || std::abs(dot02) > 0.12f) return false;
    } else {
        float dot01 = mat[0]*mat[1] + mat[4]*mat[5] + mat[8]*mat[9];
        float dot12 = mat[1]*mat[2] + mat[5]*mat[6] + mat[9]*mat[10];
        float dot02 = mat[0]*mat[2] + mat[4]*mat[6] + mat[8]*mat[10];
        if (std::abs(dot01) > 0.12f || std::abs(dot12) > 0.12f || std::abs(dot02) > 0.12f) return false;
    }

    float det = mat[0]*(mat[5]*mat[10] - mat[6]*mat[9]) -
                mat[1]*(mat[4]*mat[10] - mat[6]*mat[8]) +
                mat[2]*(mat[4]*mat[9] - mat[5]*mat[8]);
    if (std::abs(std::abs(det) - 1.0f) > 0.20f) return false;

    if (std::abs(mat[15] - 1.0f) > 0.08f) return false;

    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(mat[i])) return false;
    }
    return true;
}

bool PageScanner::IsValidViewMatrixDouble(const double* mat) {
    if (!mat) return false;

    double l0 = mat[0]*mat[0] + mat[1]*mat[1] + mat[2]*mat[2];
    double l1 = mat[4]*mat[4] + mat[5]*mat[5] + mat[6]*mat[6];
    double l2 = mat[8]*mat[8] + mat[9]*mat[9] + mat[10]*mat[10];
    bool rowsNormalized = (std::abs(l0 - 1.0) < 0.12 && std::abs(l1 - 1.0) < 0.12 && std::abs(l2 - 1.0) < 0.12);

    double c0 = mat[0]*mat[0] + mat[4]*mat[4] + mat[8]*mat[8];
    double c1 = mat[1]*mat[1] + mat[5]*mat[5] + mat[9]*mat[9];
    double c2 = mat[2]*mat[2] + mat[6]*mat[6] + mat[10]*mat[10];
    bool colsNormalized = (std::abs(c0 - 1.0) < 0.12 && std::abs(c1 - 1.0) < 0.12 && std::abs(c2 - 1.0) < 0.12);

    if (!rowsNormalized && !colsNormalized) return false;

    if (rowsNormalized) {
        double dot01 = mat[0]*mat[4] + mat[1]*mat[5] + mat[2]*mat[6];
        double dot12 = mat[4]*mat[8] + mat[5]*mat[9] + mat[6]*mat[10];
        double dot02 = mat[0]*mat[8] + mat[1]*mat[9] + mat[2]*mat[10];
        if (std::abs(dot01) > 0.12 || std::abs(dot12) > 0.12 || std::abs(dot02) > 0.12) return false;
    } else {
        double dot01 = mat[0]*mat[1] + mat[4]*mat[5] + mat[8]*mat[9];
        double dot12 = mat[1]*mat[2] + mat[5]*mat[6] + mat[9]*mat[10];
        double dot02 = mat[0]*mat[2] + mat[4]*mat[6] + mat[8]*mat[10];
        if (std::abs(dot01) > 0.12 || std::abs(dot12) > 0.12 || std::abs(dot02) > 0.12) return false;
    }

    double det = mat[0]*(mat[5]*mat[10] - mat[6]*mat[9]) -
                 mat[1]*(mat[4]*mat[10] - mat[6]*mat[8]) +
                 mat[2]*(mat[4]*mat[9] - mat[5]*mat[8]);
    if (std::abs(std::abs(det) - 1.0) > 0.20) return false;

    if (std::abs(mat[15] - 1.0) > 0.08) return false;

    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(mat[i])) return false;
    }
    return true;
}

void PageScanner::NotifyCameraLocked() {
    m_cameraLocked.store(true, std::memory_order_release);
    LOG_INFO("PageScanner: Camera lock notification received. Scanner entering deep sleep.");
}

void PageScanner::NotifyCameraLost() {
    bool wasLocked = false;
    {
        std::lock_guard<std::mutex> lk(m_sleepMutex);
        wasLocked = m_cameraLocked.exchange(false, std::memory_order_acq_rel);
    }
    if (wasLocked) {
        LOG_INFO("PageScanner: Camera tracking lost. Waking scanner for revalidation.");
        m_wakeCv.notify_one();
    }
}

bool PageScanner::IsCameraLocked() const {
    // Fast path: check the atomic flag set by FrameCoordinator
    if (m_cameraLocked.load(std::memory_order_acquire)) {
        return true;
    }
    // Slow path: query the CameraLockManager state machine directly
    auto* cameraLockMgr = SubsystemContext::Get().GetCameraLockManager();
    if (cameraLockMgr) {
        auto state = cameraLockMgr->GetState();
        if (state == CameraLockState::LOCKED || state == CameraLockState::RELOCKING) {
            return true;
        }
    }
    return false;
}

void PageScanner::ScanDynamicHeaps() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);

    // Maximum number of full sweeps before the scanner retires permanently.
    // After 3 complete sweeps (or 1 sweep + camera lock), there's no value in
    // continuing — CameraDeltaTracker reads the locked pointer directly each frame.
    constexpr uint32_t MAX_SWEEPS_BEFORE_RETIRE = 3;
    // Maximum wall-clock time for a single sweep before aborting mid-sweep.
    // Prevents the 37-second sweep that caused Onimusha lag.
    constexpr int64_t MAX_SWEEP_MS = 8000;

    while (m_scanRunning) {
        // ── Adaptive Sleep Gate ─────────────────────────────────────────────
        // If camera is already locked, enter deep sleep (0 MB/s bus traffic,
        // 0% CPU). We block on a condition variable instead of polling.
        if (IsCameraLocked()) {
            LOG_INFO("PageScanner: Camera locked. Entering deep sleep (0 MB/s memory bandwidth).");
            {
                std::unique_lock<std::mutex> lk(m_sleepMutex);
                // Sleep indefinitely until camera tracking is lost or scan is stopped.
                // Re-check every 30 seconds as a safety net in case notification was missed.
                m_wakeCv.wait_for(lk, std::chrono::seconds(30), [this] {
                    return !m_scanRunning || !m_cameraLocked.load(std::memory_order_acquire);
                });
            }
            if (!m_scanRunning) break;
            // If we woke but camera is still locked (30s safety timeout), loop back to sleep.
            if (IsCameraLocked()) continue;
            LOG_INFO("PageScanner: Woke from deep sleep — camera tracking lost, starting revalidation sweep.");
        }

        // ── Retirement Check ────────────────────────────────────────────────
        // After enough sweeps, the scanner has done its job. CameraDeltaTracker
        // reads the locked pointer directly; continued scanning is pure waste.
        uint32_t sweepCount = m_completedSweeps.load(std::memory_order_acquire);
        if (sweepCount >= MAX_SWEEPS_BEFORE_RETIRE) {
            LOG_INFO("PageScanner: %u sweeps completed. Retiring permanently to prevent memory bus contention.", sweepCount);
            break;
        }

        auto sweepStart = std::chrono::high_resolution_clock::now();
        size_t bytesScanned = 0;
        int viewCandidates = 0;
        int projCandidates = 0;
        bool abortedEarly = false;

        MEMORY_BASIC_INFORMATION mbi;
        uint8_t* currentAddress = nullptr;

        std::vector<MemoryMatrixCandidate> newDynamicCandidates;
        std::vector<uint8_t*> newCandidatePointers;
        const size_t MAX_CANDIDATES_PER_SWEEP = 512;

        while (m_scanRunning && newDynamicCandidates.size() < MAX_CANDIDATES_PER_SWEEP) {
            if (VirtualQuery(currentAddress, &mbi, sizeof(mbi)) == 0) break;

            // ── Mid-Sweep Abort Checks ──────────────────────────────────────
            // Check every memory region (not just at loop top) so we abort
            // within milliseconds of a lock event, not after 37 seconds.
            if (IsCameraLocked()) {
                LOG_INFO("PageScanner: Camera locked mid-sweep. Aborting sweep to stop memory bus contention.");
                abortedEarly = true;
                break;
            }
            auto now = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - sweepStart).count();
            if (elapsed > MAX_SWEEP_MS) {
                LOG_INFO("PageScanner: Sweep time budget exceeded (%lld ms > %lld ms). Aborting to prevent lag.",
                         elapsed, MAX_SWEEP_MS);
                abortedEarly = true;
                break;
            }

            bool isGuarded = (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0;
            bool isReadable = (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0;
            if (mbi.State == MEM_COMMIT && !isGuarded && isReadable && 
                (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_MAPPED) &&
                mbi.RegionSize > sizeof(double) * 16) {
                uint8_t* scanStart = static_cast<uint8_t*>(mbi.BaseAddress);
                uint8_t* scanEnd = scanStart + (mbi.RegionSize - sizeof(double) * 16);
                
                const size_t CHUNK_SIZE = 4096;
                uint8_t buffer[CHUNK_SIZE];
                
                for (uint8_t* p = scanStart; p < scanEnd && newDynamicCandidates.size() < MAX_CANDIDATES_PER_SWEEP; p += CHUNK_SIZE) {
                    size_t readSize = (std::min)(static_cast<size_t>(CHUNK_SIZE), static_cast<size_t>(scanEnd - p + sizeof(double)*16));
                    if (seh::SafeReadMemory(p, buffer, readSize)) {
                        bytesScanned += readSize;
                        
                        // 16-byte alignment step for matrix scans
                        for (size_t i = 0; i + sizeof(float)*16 <= readSize && newDynamicCandidates.size() < MAX_CANDIDATES_PER_SWEEP; i += 16) {
                            float* fmat = reinterpret_cast<float*>(buffer + i);
                            uint8_t* actualAddr = p + i;

                            bool isView = IsValidViewMatrixFloat(fmat);
                            bool isProj = !isView && IsValidProjectionMatrixFloat(fmat, m_targetFov);

                            if (isView || isProj) {
                                MemoryMatrixCandidate cand;
                                cand.address = actualAddr;
                                cand.isDoublePrecision = false;
                                cand.isViewMatrix = isView;
                                cand.isProjectionMatrix = isProj;

                                newDynamicCandidates.push_back(cand);
                                newCandidatePointers.push_back(actualAddr);
                                if (isView) viewCandidates++; else projCandidates++;
                            }
                            
                            // Check Double64 on 32-byte boundaries
                            if (i % 32 == 0 && i + sizeof(double)*16 <= readSize && newDynamicCandidates.size() < MAX_CANDIDATES_PER_SWEEP) {
                                double* dmat = reinterpret_cast<double*>(buffer + i);
                                bool isViewD = IsValidViewMatrixDouble(dmat);
                                bool isProjD = !isViewD && IsValidProjectionMatrixDouble(dmat, m_targetFov);

                                if (isViewD || isProjD) {
                                    MemoryMatrixCandidate cand;
                                    cand.address = actualAddr;
                                    cand.isDoublePrecision = true;
                                    cand.isViewMatrix = isViewD;
                                    cand.isProjectionMatrix = isProjD;

                                    newDynamicCandidates.push_back(cand);
                                    newCandidatePointers.push_back(actualAddr);
                                    if (isViewD) viewCandidates++; else projCandidates++;
                                }
                            }
                        }
                    }
                }
            }
            
            const uintptr_t baseAddr = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            const uintptr_t nextAddr = baseAddr + mbi.RegionSize;
            if (nextAddr <= reinterpret_cast<uintptr_t>(currentAddress) || nextAddr < baseAddr) {
                break; // Guard against address-space wraparound (MED-02)
            }
            currentAddress = reinterpret_cast<uint8_t*>(nextAddr);
            std::this_thread::yield();
        }

        if (m_scanRunning) {
            std::lock_guard<std::mutex> lock(m_candidatesMutex);
            // L-5: Merge and preserve fast-path cached candidates across sweeps
            for (const auto& cached : m_cachedCandidates) {
                bool alreadyPresent = false;
                for (const auto& existing : newDynamicCandidates) {
                    if (existing.address == cached.address) {
                        alreadyPresent = true;
                        break;
                    }
                }
                if (!alreadyPresent) {
                    if (cached.isDoublePrecision) {
                        double mat[16];
                        if (seh::SafeReadMemory(cached.address, mat, sizeof(mat)) && IsValidViewMatrixDouble(mat)) {
                            newDynamicCandidates.insert(newDynamicCandidates.begin(), cached);
                            newCandidatePointers.insert(newCandidatePointers.begin(), cached.address);
                        }
                    } else {
                        float mat[16];
                        if (seh::SafeReadMemory(cached.address, mat, sizeof(mat)) && IsValidViewMatrixFloat(mat)) {
                            newDynamicCandidates.insert(newDynamicCandidates.begin(), cached);
                            newCandidatePointers.insert(newCandidatePointers.begin(), cached.address);
                        }
                    }
                }
            }
            m_dynamicCandidates = newDynamicCandidates;
            m_candidatePointers = newCandidatePointers;
        }

        auto sweepEnd = std::chrono::high_resolution_clock::now();
        auto msTaken = std::chrono::duration_cast<std::chrono::milliseconds>(sweepEnd - sweepStart).count();
        
        LOG_INFO("PageScanner: Sweep %s. Scanned %zu MB in %lld ms. Found %d View, %d Projection candidates.",
                 abortedEarly ? "aborted (camera locked or time budget exceeded)" : "complete",
                 bytesScanned / (1024 * 1024), msTaken, viewCandidates, projCandidates);

        if (!abortedEarly) {
            m_completedSweeps.fetch_add(1, std::memory_order_release);
        }

        // Inter-sweep cooldown: 8 seconds between sweeps, but interruptible
        // by camera lock (enters deep sleep above) or shutdown.
        for (int i = 0; i < 80 && m_scanRunning && !IsCameraLocked(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

} // namespace vrinject
