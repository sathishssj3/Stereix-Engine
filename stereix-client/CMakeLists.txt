cmake_minimum_required(VERSION 3.25)
project(StereixRuntime VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
include(cmake/CompilerFlags.cmake)
include(cmake/Dependencies.cmake)
include(cmake/Shaders.cmake)

file(GLOB_RECURSE VRINJECT_CORE_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp")
file(GLOB_RECURSE VRINJECT_HEURISTICS_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/heuristics/*.cpp")
file(GLOB_RECURSE VRINJECT_RENDERING_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rendering/*.cpp")
file(GLOB_RECURSE VRINJECT_OPENXR_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/openxr/*.cpp")
file(GLOB_RECURSE VRINJECT_VR_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/vr/*.cpp")
file(GLOB_RECURSE VRINJECT_HOOKS_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/hooks/*.cpp")
file(GLOB_RECURSE VRINJECT_AI_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/ai/*.cpp")
file(GLOB_RECURSE VRINJECT_RUNTIME_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/*.cpp")
file(GLOB_RECURSE VRINJECT_ADAPTIVE_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/adaptive/*.cpp")
file(GLOB_RECURSE VRINJECT_DIAGNOSTICS_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/diagnostics/*.cpp")
file(GLOB_RECURSE VRINJECT_CONFIG_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/config/*.cpp")
file(GLOB_RECURSE VRINJECT_MEMORY_SCANNER_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/memory_scanner/*.cpp")
file(GLOB_RECURSE VRINJECT_UPDATER_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/updater/*.cpp")
file(GLOB_RECURSE VRINJECT_PLUGINS_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/*.cpp")
file(GLOB_RECURSE VRINJECT_UI_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.cpp")
file(GLOB_RECURSE VRINJECT_RELEASE_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/release/*.cpp")
file(GLOB_RECURSE VRINJECT_VALIDATION_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/validation/*.cpp")
file(GLOB_RECURSE VRINJECT_FEEDBACK_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/feedback/*.cpp")

add_library(vrinject SHARED
    ${VRINJECT_CORE_SRCS}
    ${VRINJECT_HEURISTICS_SRCS}
    ${VRINJECT_RENDERING_SRCS}
    ${VRINJECT_OPENXR_SRCS}
    ${VRINJECT_VR_SRCS}
    ${VRINJECT_HOOKS_SRCS}
    ${VRINJECT_AI_SRCS}
    ${VRINJECT_RUNTIME_SRCS}
    ${VRINJECT_ADAPTIVE_SRCS}
    ${VRINJECT_DIAGNOSTICS_SRCS}
    ${VRINJECT_CONFIG_SRCS}
    ${VRINJECT_MEMORY_SCANNER_SRCS}
    ${VRINJECT_UPDATER_SRCS}
    ${VRINJECT_PLUGINS_SRCS}
    ${VRINJECT_UI_SRCS}
    ${VRINJECT_RELEASE_SRCS}
    ${VRINJECT_VALIDATION_SRCS}
    ${VRINJECT_FEEDBACK_SRCS}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dllmain.cpp
    ${MINHOOK_SOURCES}
    ${IMGUI_SOURCES}
)

# ---------------------------------------------------------------------------
# Proxy DLLs (dxgi.dll & d3d11.dll)
# ---------------------------------------------------------------------------
add_library(dxgi_proxy SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/src/proxy/dxgi_proxy.cpp
)
# NOTE: these proxies are deliberately named dxgi.dll / d3d11.dll so they can be
# dropped next to a game executable and take load precedence over the system
# DLLs. That same precedence is harmful inside bin/, where it shadowed the real
# system DLLs for every test executable and made them fail to start with
# 0xC000007B (STATUS_INVALID_IMAGE_FORMAT). They are emitted into bin/proxy/
# instead; deployment copies them next to the target game, not next to tests.
set_target_properties(dxgi_proxy PROPERTIES
    OUTPUT_NAME "dxgi"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/proxy
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/bin/proxy
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin/proxy
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/bin/proxy
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_BINARY_DIR}/bin/proxy
)
target_link_libraries(dxgi_proxy PRIVATE user32 bcrypt)
target_include_directories(dxgi_proxy PRIVATE ${CMAKE_BINARY_DIR})
add_dependencies(dxgi_proxy vrinject)

add_library(d3d11_proxy SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/src/proxy/d3d11_proxy.cpp
)
set_target_properties(d3d11_proxy PROPERTIES
    OUTPUT_NAME "d3d11"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin/proxy
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/bin/proxy
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin/proxy
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/bin/proxy
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_BINARY_DIR}/bin/proxy
)
target_link_libraries(d3d11_proxy PRIVATE user32 bcrypt)
target_include_directories(d3d11_proxy PRIVATE ${CMAKE_BINARY_DIR})
add_dependencies(d3d11_proxy vrinject)

# S1.1 / MED-01 / CRIT-03: Secure in-memory code-signing helper
# (no %TEMP% plaintext password files, HTTPS timestamp server, signs pre-hash)
file(WRITE "${CMAKE_BINARY_DIR}/sign_target.ps1"
"param([Parameter(Mandatory=\$true)][string]\$TargetFile)
\$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath \$TargetFile)) { exit 0 }

\$cert = \$null
\$useTimestamp = \$false

if (-not [string]::IsNullOrWhiteSpace(\$env:SIGN_CERT_PATH) -and (Test-Path -LiteralPath \$env:SIGN_CERT_PATH)) {
    \$cert = [Security.Cryptography.X509Certificates.X509Certificate2]::new(\$env:SIGN_CERT_PATH, \$env:SIGN_CERT_PASS)
    \$useTimestamp = \$true
} elseif ([string]::IsNullOrWhiteSpace(\$env:CI) -and [string]::IsNullOrWhiteSpace(\$env:GITHUB_ACTIONS)) {
    \$cert = Get-ChildItem -Path Cert:\\CurrentUser\\My -CodeSigningCert -ErrorAction SilentlyContinue |
        Where-Object { \$_.Subject -in @('CN=Stereix Local Development', 'CN=NexVR Local Development') } |
        Select-Object -First 1
}

if (-not \$cert) { exit 0 }

if (\$useTimestamp) {
    \$sig = Set-AuthenticodeSignature -FilePath \$TargetFile -Certificate \$cert -HashAlgorithm SHA256 -TimestampServer 'https://timestamp.digicert.com'
} else {
    \$sig = Set-AuthenticodeSignature -FilePath \$TargetFile -Certificate \$cert -HashAlgorithm SHA256
}
if (\$sig.Status -notin @('Valid', 'UnknownError')) {
    throw \"Code signing failed for \$TargetFile : \$(\$sig.Status) - \$(\$sig.StatusMessage)\"
}
")

# S1.1: Code-sign d3d11.dll & dxgi.dll after link
add_custom_command(TARGET d3d11_proxy POST_BUILD
  COMMAND powershell -NonInteractive -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_BINARY_DIR}/sign_target.ps1" "$<TARGET_FILE:d3d11_proxy>"
  COMMENT "Signing d3d11.dll")

add_custom_command(TARGET dxgi_proxy POST_BUILD
  COMMAND powershell -NonInteractive -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_BINARY_DIR}/sign_target.ps1" "$<TARGET_FILE:dxgi_proxy>"
  COMMENT "Signing dxgi.dll")

if(MSVC)
    target_compile_options(vrinject PRIVATE /sdl)
    target_compile_options(dxgi_proxy PRIVATE /sdl)
    target_compile_options(d3d11_proxy PRIVATE /sdl)
    set_source_files_properties(${IMGUI_SOURCES} ${MINHOOK_SOURCES} PROPERTIES COMPILE_OPTIONS "/sdl-")
endif()

target_include_directories(vrinject PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${MINHOOK_INCLUDE_DIR}
    ${ONNXRUNTIME_INCLUDE_DIR}
    ${OPENXR_INCLUDE_DIR}
    ${JSON_INCLUDE_DIR}
    ${IMGUI_INCLUDE_DIR}
    ${SHADER_OUTPUT_DIR}
)

target_link_directories(vrinject PRIVATE ${ONNXRUNTIME_LIB_DIR})

target_link_libraries(vrinject PRIVATE
    d3d11
    d3d12
    dxgi
    d3dcompiler
    dxguid
    onnxruntime
    openxr_loader
    xinput
    Vulkan::Vulkan
    bcrypt
    shell32
)

# Copy native AI runtime dependencies next to the injected DLL.
add_custom_command(TARGET vrinject POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${onnxruntime_SOURCE_DIR}/runtimes/win-x64/native/onnxruntime.dll"
        $<TARGET_FILE_DIR:vrinject>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${directml_SOURCE_DIR}/bin/x64-win/DirectML.dll"
        $<TARGET_FILE_DIR:vrinject>
)

# S1.1 / CRIT-03: Sign vrinject.dll BEFORE generating expected_hash.h so embedded hashes match signed bytes
add_custom_command(TARGET vrinject POST_BUILD
  COMMAND powershell -NonInteractive -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_BINARY_DIR}/sign_target.ps1" "$<TARGET_FILE:vrinject>"
  COMMENT "Signing vrinject.dll prior to hash generation")

# S1.4 / CRIT-02: Generate expected SHA-256 hashes for vrinject.dll, onnxruntime.dll, and DirectML.dll
file(WRITE "${CMAKE_BINARY_DIR}/generate_hash.ps1"
"param([Parameter(Mandatory=\$true)][string]\$DllPath, [Parameter(Mandatory=\$true)][string]\$OutPath)
\$ErrorActionPreference = 'Stop'
function Get-Sha256Upper([string]\$FilePath) {
    if (-not (Test-Path -LiteralPath \$FilePath)) { return '' }
    return (Get-FileHash -LiteralPath \$FilePath -Algorithm SHA256).Hash.ToUpperInvariant()
}
\$binDir = Split-Path -Parent \$DllPath
\$dllHash = Get-Sha256Upper \$DllPath
\$ortHash = Get-Sha256Upper (Join-Path \$binDir 'onnxruntime.dll')
\$dmlHash = Get-Sha256Upper (Join-Path \$binDir 'DirectML.dll')
\$content = @(
    '#pragma once',
    \"constexpr wchar_t EXPECTED_DLL_HASH[] = L`\"\$dllHash`\";\",
    \"constexpr wchar_t EXPECTED_ONNXRUNTIME_HASH[] = L`\"\$ortHash`\";\",
    \"constexpr wchar_t EXPECTED_DIRECTML_HASH[] = L`\"\$dmlHash`\";\"
) -join \"`r`n\"
if (-not (Test-Path -LiteralPath \$OutPath) -or (Get-Content -LiteralPath \$OutPath -Raw).Trim() -ne \$content.Trim()) {
    Set-Content -LiteralPath \$OutPath -Value \$content -Encoding ASCII
}
")

add_custom_command(TARGET vrinject POST_BUILD
  COMMAND powershell -NonInteractive -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_BINARY_DIR}/generate_hash.ps1" "$<TARGET_FILE:vrinject>" "${CMAKE_BINARY_DIR}/expected_hash.h"
  COMMENT "Generating DLL hash header")

# Make sure shaders are compiled and copied before the DLL is built.
add_dependencies(vrinject compile_shaders)
add_dependencies(vrinject copy_shaders_for_dx12)

# ---------------------------------------------------------------------------
# vr-inject-cli - injection CLI
# ---------------------------------------------------------------------------
add_executable(vr-inject-cli ${CMAKE_CURRENT_SOURCE_DIR}/src/injector/main.cpp)
add_dependencies(vr-inject-cli vrinject)
add_custom_target(generate_expected_hash DEPENDS "${CMAKE_BINARY_DIR}/expected_hash.h")
add_dependencies(vr-inject-cli generate_expected_hash)
set_target_properties(vr-inject-cli PROPERTIES LINK_FLAGS "/MANIFESTUAC:\"level='asInvoker' uiAccess='false'\" /SUBSYSTEM:CONSOLE /CETCOMPAT")
if(MSVC)
    target_compile_options(vr-inject-cli PRIVATE /sdl)
endif()
target_include_directories(vr-inject-cli PRIVATE ${CMAKE_BINARY_DIR} ${JSON_INCLUDE_DIR})
target_link_libraries(vr-inject-cli PRIVATE shlwapi bcrypt psapi)

# S1.1: Code-sign vr-inject-cli.exe
add_custom_command(TARGET vr-inject-cli POST_BUILD
  COMMAND powershell -NonInteractive -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_BINARY_DIR}/sign_target.ps1" "$<TARGET_FILE:vr-inject-cli>"
  COMMENT "Signing vr-inject-cli.exe")

add_executable(test_xr ${CMAKE_CURRENT_SOURCE_DIR}/src/test_xr.cpp)
target_include_directories(test_xr PRIVATE ${OPENXR_INCLUDE_DIR})
target_link_libraries(test_xr PRIVATE openxr_loader)

# ---------------------------------------------------------------------------
# vk_test_app - target app for testing Vulkan injection
# ---------------------------------------------------------------------------
add_executable(vk_test_app ${CMAKE_CURRENT_SOURCE_DIR}/src/test_apps/vk_test_app.cpp)
target_link_libraries(vk_test_app PRIVATE Vulkan::Vulkan)

add_library(NexVRCore STATIC
    ${VRINJECT_CORE_SRCS}
    ${VRINJECT_HEURISTICS_SRCS}
    ${VRINJECT_RENDERING_SRCS}
    ${VRINJECT_OPENXR_SRCS}
    ${VRINJECT_VR_SRCS}
    ${VRINJECT_HOOKS_SRCS}
    ${VRINJECT_AI_SRCS}
    ${VRINJECT_RUNTIME_SRCS}
    ${VRINJECT_DIAGNOSTICS_SRCS}
    ${VRINJECT_CONFIG_SRCS}
    ${VRINJECT_MEMORY_SCANNER_SRCS}
    ${VRINJECT_ADAPTIVE_SRCS}
    ${VRINJECT_PLUGINS_SRCS}
    ${VRINJECT_UPDATER_SRCS}
    ${VRINJECT_RELEASE_SRCS}
    ${VRINJECT_VALIDATION_SRCS}
    ${VRINJECT_FEEDBACK_SRCS}
    ${MINHOOK_SOURCES}
    ${IMGUI_SOURCES}
)

target_include_directories(NexVRCore PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${MINHOOK_INCLUDE_DIR}
    ${ONNXRUNTIME_INCLUDE_DIR}
    ${OPENXR_INCLUDE_DIR}
    ${JSON_INCLUDE_DIR}
    ${IMGUI_INCLUDE_DIR}
    ${SHADER_OUTPUT_DIR}
)
target_link_libraries(NexVRCore PRIVATE Vulkan::Vulkan openxr_loader d3d11 d3d12 dxgi)
add_dependencies(NexVRCore copy_shaders_for_dx12)

add_executable(test_openxr_vulkan_swapchain ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_openxr_vulkan_swapchain.cpp)
target_include_directories(test_openxr_vulkan_swapchain PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src ${GTEST_INCLUDE_DIRS} ${OPENXR_INCLUDE_DIR})
target_link_libraries(test_openxr_vulkan_swapchain PRIVATE NexVRCore Vulkan::Vulkan openxr_loader)

add_executable(test_openxr_harness ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/test_openxr_harness.cpp ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/mock_openxr_runtime.cpp)
target_include_directories(test_openxr_harness PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR} ${GTEST_INCLUDE_DIRS} ${OPENXR_INCLUDE_DIR} ${MINHOOK_INCLUDE_DIR} ${Vulkan_INCLUDE_DIRS})
target_link_libraries(test_openxr_harness PRIVATE NexVRCore d3d11 gtest_main)
add_test(NAME test_openxr_harness COMMAND test_openxr_harness)

include(cmake/Testing.cmake)
