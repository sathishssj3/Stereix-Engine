#include "core/runtime_state.h"
#include "core/hook_manager.h"
#include "core/logger.h"
#include "core/subsystem_context.h"
#include "core/config_manager.h"
#include "core/seh_shield.h"
#include "core/drm_manager.h"
#include "core/version.h"
#include <windows.h>
#include <shlobj.h>
#include <chrono>

#include <tlhelp32.h>

namespace vrinject {
namespace {

bool CheckAntiCheatRunning() {
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
    HANDLE hSnap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);
    bool detected = false;
    if (::Process32First(hSnap, &pe)) {
        do {
            std::string pName = pe.szExeFile;
            for (char& c : pName) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (const char* blocked : strict_ac_blocklist) {
                if (pName.find(blocked) != std::string::npos) {
                    detected = true;
                    break;
                }
            }
            if (detected) break;
        } while (::Process32Next(hSnap, &pe));
    }
    ::CloseHandle(hSnap);
    return detected;
}

} // anonymous namespace

void RuntimeState::TransitionTo(RuntimePhase newPhase) {
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_phase.store(newPhase);
    }
    m_stateCv.notify_all();
}

void RuntimeState::WaitForPhase(RuntimePhase phase) {
    std::unique_lock<std::mutex> lock(m_stateMutex);
    m_stateCv.wait(lock, [this, phase]() {
        return m_phase.load() == phase;
    });
}

unsigned long __stdcall RuntimeState::InitThreadProc(void* param) {
    auto* self = static_cast<RuntimeState*>(param);
    if (self) {
        self->BackgroundInitialize();
    }
    return 0;
}

void RuntimeState::OnDllProcessAttach(void* hModule) {
    // HIGH-06: Called under Windows loader lock — NEVER construct std::thread or lock std::mutex here.
    // Use raw Win32 CreateThread + CloseHandle (BUG-05) and lock-free atomics.
    m_hModule = hModule;
    m_phase.store(RuntimePhase::Attached, std::memory_order_release);

    HANDLE hThread = ::CreateThread(nullptr, 0, &RuntimeState::InitThreadProc, this, 0, nullptr);
    if (hThread) {
        ::CloseHandle(hThread);
    } else {
        m_phase.store(RuntimePhase::Error, std::memory_order_release);
    }
}

void RuntimeState::OnDllProcessDetach(bool processTerminating) {
    // HIGH-06: Never acquire m_stateMutex under loader lock during DLL_PROCESS_DETACH.
    m_phase.store(RuntimePhase::Stopping, std::memory_order_release);
    if (processTerminating) {
        // Process is exiting and other threads are already terminated by the OS; return immediately.
        return;
    }
    HANDLE hStop = static_cast<HANDLE>(m_stopEvent.load(std::memory_order_acquire));
    if (hStop) {
        ::SetEvent(hStop);
    }
}

void RuntimeState::BackgroundInitialize() {
    HANDLE hStop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (hStop) {
        m_stopEvent.store(hStop, std::memory_order_release);
        if (m_phase.load(std::memory_order_acquire) == RuntimePhase::Stopping) {
            ::SetEvent(hStop);
        }
    }

    TransitionTo(RuntimePhase::Initializing);
    
    // Pin module
    HMODULE pinnedModule = nullptr;
    ::GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCSTR>(&RuntimeState::Get),
        &pinnedModule);

    // Initialize logger using std::string (MED-02: avoids MAX_PATH strcat_s overflow crash)
    char dllBuf[1024]{};
    std::string dllDir;
    DWORD dllLen = ::GetModuleFileNameA(static_cast<HMODULE>(m_hModule), dllBuf, static_cast<DWORD>(std::size(dllBuf)));
    if (dllLen > 0 && dllLen < std::size(dllBuf)) {
        dllDir.assign(dllBuf, dllLen);
        size_t lastSlash = dllDir.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            dllDir.resize(lastSlash + 1);
        } else {
            dllDir.clear();
        }
    }

    char exeBuf[1024]{};
    std::string exePath;
    DWORD exeLen = ::GetModuleFileNameA(nullptr, exeBuf, static_cast<DWORD>(std::size(exeBuf)));
    if (exeLen > 0 && exeLen < std::size(exeBuf)) {
        exePath.assign(exeBuf, exeLen);
    }

    std::string logPath;
    char appDataBuf[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appDataBuf))) {
        std::string vrDir = std::string(appDataBuf) + "\\VRInject";
        ::CreateDirectoryA(vrDir.c_str(), nullptr);
        logPath = vrDir + "\\vrinject.log";
    } else {
        logPath = dllDir + "vrinject.log";
    }
    
    auto logger = std::make_shared<FileLogger>();
    logger->Init(logPath);

    // Also attach secondary log in dll directory
    if (!dllDir.empty()) {
        logger->AddSecondaryPath(dllDir + "vrinject.log");
    }

    // Also attach secondary log in game executable directory if different
    size_t exeSlash = exePath.find_last_of("\\/");
    if (exeSlash != std::string::npos) {
        std::string exeDirLogPath = exePath.substr(0, exeSlash + 1) + "vrinject.log";
        logger->AddSecondaryPath(exeDirLogPath);
    }

    auto config = std::make_shared<ConfigManager>();
    config->Load(dllDir);
    SubsystemContext::Get().Initialize(logger, config);

    seh::RegisterVehShield(static_cast<HMODULE>(m_hModule));

    LOG_INFO("========================================");
    LOG_INFO("NexVR Engine v" NEXVR_ENGINE_VERSION " - Initializing");
    LOG_INFO("========================================");
    LOG_INFO("[DIAGNOSTICS] Host Process: %s", exePath.c_str());
    LOG_INFO("[DIAGNOSTICS] Module DLL Dir: %s", dllDir.c_str());
    LOG_INFO("[DIAGNOSTICS] Primary Log Path: %s", logPath.c_str());

    const auto& cfg = config->GetConfig();
    LOG_INFO("[DIAGNOSTICS] Config: IPD=%.3fm, Convergence=%.1fm, ResScale=%.2f", cfg.ipd, cfg.convergence, cfg.resolutionScale);
    LOG_INFO("[DIAGNOSTICS] Color Calibration: Brightness=%.2f, Contrast=%.2f, Saturation=%.2f, sRGB=%s",
             cfg.brightness, cfg.contrast, cfg.saturation, cfg.srgbCorrection ? "On" : "Off");
    LOG_INFO("[DIAGNOSTICS] Engine Profile: %s | API Override: %s",
             cfg.engineType.empty() ? "Auto" : cfg.engineType.c_str(),
             cfg.api.empty() ? "Auto" : cfg.api.c_str());
    
    SubsystemContext::Get().GetDiagnosticContext()->PostEvent(DiagnosticLevel::Info, "Runtime", "Starting background initialization");
    
    if (!TrialManager::GetInstance().InitializeTrial()) {
        LOG_ERROR("Trial validation failed - shutting down");
        TransitionTo(RuntimePhase::Error);
        return;
    }
    
    // L-6: Check if anti-cheat process is running before installing hooks
    if (CheckAntiCheatRunning()) {
        LOG_ERROR("[SECURITY] Anti-Cheat service detected at runtime. Aborting injection to protect account from bans.");
        TransitionTo(RuntimePhase::Error);
        return;
    }

    LOG_DEBUG("Waiting for remote injection thread to exit...");
    Sleep(1000); // Prevent MH_EnableHook from deadlocking against the exiting injection thread

    LOG_DEBUG("About to call HookManager::InitializeHooks()...");
    bool success = HookManager::Get().InitializeHooks();
    LOG_DEBUG("HookManager::InitializeHooks() returned: %s", success ? "true" : "false");
    
    if (success) {
        LOG_INFO("[OK] HookManager: Detours & Memory Interceptors Active");
        SubsystemContext::Get().GetDiagnosticContext()->PostEvent(DiagnosticLevel::Info, "Runtime", "Initialization complete");
        TransitionTo(RuntimePhase::Running);
    } else {
        SubsystemContext::Get().GetDiagnosticContext()->PostEvent(DiagnosticLevel::Error, "Runtime", "Initialization failed");
        TransitionTo(RuntimePhase::Error);
    }

    // R5 / L-6: Park the existing worker thread here, waking via lock-free Win32 event + periodic anti-cheat polling
    while (m_phase.load(std::memory_order_acquire) != RuntimePhase::Stopping &&
           m_phase.load(std::memory_order_acquire) != RuntimePhase::Stopped) {
        HANDLE hWait = static_cast<HANDLE>(m_stopEvent.load(std::memory_order_acquire));
        if (hWait) {
            DWORD waitRes = ::WaitForSingleObject(hWait, 5000);
            if (waitRes == WAIT_OBJECT_0) {
                break;
            }
        } else {
            ::Sleep(500);
        }
        // Periodic L-6 anti-cheat tripwire check while running
        if (m_phase.load(std::memory_order_acquire) == RuntimePhase::Running && CheckAntiCheatRunning()) {
            LOG_ERROR("[SECURITY] Anti-Cheat service launched after injection! Initiating emergency shutdown.");
            m_phase.store(RuntimePhase::Stopping, std::memory_order_release);
            break;
        }
    }

    // Perform teardown on the original worker thread
    BackgroundTeardown();

    HANDLE hClose = static_cast<HANDLE>(m_stopEvent.exchange(nullptr, std::memory_order_acq_rel));
    if (hClose) {
        ::CloseHandle(hClose);
    }
}

void RuntimeState::BackgroundTeardown() {
    auto* diag = SubsystemContext::Get().GetDiagnosticContext();
    if (diag) {
        diag->PostEvent(DiagnosticLevel::Info, "Runtime", "Starting background teardown");
    }
    
    HookManager::Get().ShutdownHooks();
    seh::UnregisterVehShield();
    
    if (diag) {
        diag->PostEvent(DiagnosticLevel::Info, "Runtime", "Teardown complete");
    }

    SubsystemContext::Get().Shutdown();
    TransitionTo(RuntimePhase::Stopped);
}

} // namespace vrinject
