#define _CRT_SECURE_NO_WARNINGS
#include "core/drm_manager.h"
#include "core/logger.h"
#include <vector>
#include <stdio.h>
#include <chrono>
#include <atomic>

namespace vrinject {

#define REG_PATH "Software\\VRInject\\DRM"
#define MAX_PLAYTIME_MINUTES 180
#define MAX_GAMES 10

TrialManager::~TrialManager() {
    Shutdown();
}

std::string TrialManager::GetExecutableName() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string fullPath(path);
    size_t lastSlash = fullPath.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        return fullPath.substr(lastSlash + 1);
    }
    return fullPath;
}

constexpr DWORD OBFUSCATION_KEY = 0x9E3779B9;

std::string ObfuscateString(const std::string& input) {
    std::string output = input;
    for (size_t i = 0; i < output.length(); ++i) {
        output[i] ^= (OBFUSCATION_KEY >> ((i % 4) * 8)) & 0xFF;
    }
    return output;
}

bool TrialManager::ReadRegistryData() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    HKEY hKey;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, REG_PATH, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE, NULL, &hKey, NULL) != ERROR_SUCCESS) {
        return false;
    }

    DWORD type = REG_DWORD;
    DWORD size = sizeof(DWORD);
    DWORD obfuscatedPlaytime = 0;
    if (RegQueryValueExA(hKey, "T_Val", NULL, &type, (LPBYTE)&obfuscatedPlaytime, &size) != ERROR_SUCCESS) {
        m_playtimeMinutes = 0;
    } else {
        m_playtimeMinutes = obfuscatedPlaytime ^ OBFUSCATION_KEY;
    }

    type = REG_DWORD;
    size = sizeof(DWORD);
    DWORD obfuscatedGameCount = 0;
    if (RegQueryValueExA(hKey, "G_Count", NULL, &type, (LPBYTE)&obfuscatedGameCount, &size) != ERROR_SUCCESS) {
        m_gameCount = 0;
    } else {
        m_gameCount = obfuscatedGameCount ^ OBFUSCATION_KEY;
    }

    for (DWORD i = 0; i < m_gameCount && i < MAX_GAMES; ++i) {
        char keyName[16];
        snprintf(keyName, sizeof(keyName), "G_Name%lu", static_cast<unsigned long>(i));
        char buffer[MAX_PATH] = {};
        DWORD valType = 0;
        DWORD bufSize = sizeof(buffer);
        if (RegQueryValueExA(hKey, keyName, NULL, &valType, (LPBYTE)buffer, &bufSize) == ERROR_SUCCESS && bufSize > 0) {
            size_t len = static_cast<size_t>(bufSize);
            if (valType == REG_SZ && buffer[len - 1] == '\0') {
                --len;
            }
            m_games[i] = ObfuscateString(std::string(buffer, len));
        }
    }

    RegCloseKey(hKey);
    return true;
}

void TrialManager::WriteRegistryData() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    HKEY hKey;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, REG_PATH, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        DWORD obfuscatedPlaytime = m_playtimeMinutes ^ OBFUSCATION_KEY;
        RegSetValueExA(hKey, "T_Val", 0, REG_DWORD, (const BYTE*)&obfuscatedPlaytime, sizeof(DWORD));
        
        DWORD obfuscatedGameCount = m_gameCount ^ OBFUSCATION_KEY;
        RegSetValueExA(hKey, "G_Count", 0, REG_DWORD, (const BYTE*)&obfuscatedGameCount, sizeof(DWORD));

        for (DWORD i = 0; i < m_gameCount && i < MAX_GAMES; ++i) {
            char keyName[16];
            snprintf(keyName, sizeof(keyName), "G_Name%lu", static_cast<unsigned long>(i));
            std::string obfName = ObfuscateString(m_games[i]);
            // Use REG_BINARY so XOR-obfuscated zero bytes are never truncated (MED-02)
            RegSetValueExA(hKey, keyName, 0, REG_BINARY, reinterpret_cast<const BYTE*>(obfName.data()), static_cast<DWORD>(obfName.size()));
        }
        RegCloseKey(hKey);
    }
}

void TrialManager::TriggerExpiration(const char* reason) {
    LOG_INFO("TrialManager: Expired! Reason: %s", reason);
    // FIX #12: Do NOT call MessageBoxA while holding m_mutex.
    // Set the flag here; the actual dialog is shown after releasing the lock
    // in the enforcement thread or in the caller that checked InitializeTrial.
    m_expiryPending = true;
}

bool TrialManager::InitializeTrial() {
    m_initialized = true;
    return true;
}



void TrialManager::Shutdown() {
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_running = false;
    }
    m_stopCondition.notify_all();
}

} // namespace vrinject
