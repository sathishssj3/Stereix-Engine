#pragma once
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>
#include "core/diagnostic_context.h"

namespace vrinject {

enum class RuntimePhase {
    Uninitialized,
    Attached,
    Initializing,
    Running,
    Stopping,
    Stopped,
    Error
};

class RuntimeState {
public:
    static RuntimeState& Get() {
        static RuntimeState instance;
        return instance;
    }

    void OnDllProcessAttach(void* hModule);
    void OnDllProcessDetach(bool processTerminating = false);
    
    RuntimePhase GetPhase() const { return m_phase.load(); }
    void* GetModuleHandle() const { return m_hModule; }

    // Used for tests to wait for a phase
    void WaitForPhase(RuntimePhase phase);

private:
    RuntimeState() : m_phase(RuntimePhase::Uninitialized), m_hModule(nullptr) {}

    static unsigned long __stdcall InitThreadProc(void* param);
    void BackgroundInitialize();
    void BackgroundTeardown();

    std::atomic<RuntimePhase> m_phase;
    void* m_hModule;
    std::atomic<void*> m_stopEvent{nullptr};
    
    std::mutex m_stateMutex;
    std::condition_variable m_stateCv;

    void TransitionTo(RuntimePhase newPhase);
};

} // namespace vrinject
