#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <vector>
#include <cstdint>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <string>

namespace vrinject {

struct MemoryMatrixCandidate {
    uint8_t* address = nullptr;
    bool isDoublePrecision = false;
    bool isViewMatrix = false;
    bool isProjectionMatrix = false;
};

class PageScanner {
public:

    bool Initialize();

    // Starts the background thread to scan for dynamic projection and view matrices
    void StartDynamicScan(float targetFov = 90.0f);

    // Stops the background scanning thread
    void StopDynamicScan();

    // Retrieves discovered dynamic matrix candidates
    std::vector<MemoryMatrixCandidate> GetDynamicCandidates();

    // Retrieves the candidate pointers (backwards-compatible)
    std::vector<uint8_t*> GetCandidateStaticPointers();

    // Matrix shape and invariant validators (public for unit tests and delta tracker)
    static bool IsValidProjectionMatrixFloat(const float* mat, float targetFov = 90.0f);
    static bool IsValidProjectionMatrixDouble(const double* mat, float targetFov = 90.0f);
    static bool IsValidViewMatrixFloat(const float* mat);
    static bool IsValidViewMatrixDouble(const double* mat);

    // Called by FrameCoordinator when camera tracking is locked/lost.
    // These control the adaptive sleep to avoid 2+ GB memory bus contention.
    void NotifyCameraLocked();
    void NotifyCameraLost();

    PageScanner() = default;
    ~PageScanner() { StopDynamicScan(); }

private:

    struct ModuleInfo {
        uint8_t* baseAddress = nullptr;
        size_t size = 0;
    };
    
    ModuleInfo GetModuleInfo(const std::string& moduleName);
    
    void ScanDynamicHeaps();

    // Returns true if the camera is locked (via external notification or CameraLockManager query)
    bool IsCameraLocked() const;

    uint8_t* m_mainModuleBase = nullptr;
    size_t m_mainModuleSize = 0;
    float m_targetFov = 90.0f;

    std::thread m_scanThread;
    std::atomic<bool> m_scanRunning{false};

    // Adaptive sleep: when camera is locked, scanner enters deep sleep (0 MB/s bus traffic).
    // Woken by NotifyCameraLost() or StopDynamicScan().
    std::atomic<bool> m_cameraLocked{false};
    std::mutex m_sleepMutex;
    std::condition_variable m_wakeCv;

    // Completed sweep counter — after enough sweeps with a lock, scanner retires permanently.
    std::atomic<uint32_t> m_completedSweeps{0};

    std::mutex m_candidatesMutex;
    std::vector<MemoryMatrixCandidate> m_cachedCandidates;
    std::vector<MemoryMatrixCandidate> m_dynamicCandidates;
    std::vector<uint8_t*> m_candidatePointers;
};

} // namespace vrinject
