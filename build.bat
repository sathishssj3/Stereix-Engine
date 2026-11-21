@echo off
setlocal enabledelayedexpansion

echo =======================================================
echo Building Stereix Engine (Release x64) Monorepo
echo =======================================================

:: Locate VsDevCmd.bat via vswhere if not already in developer environment
if not defined DevEnvDir (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -prerelease -products * -property installationPath`) do (
            if exist "%%i\Common7\Tools\VsDevCmd.bat" set "VS_DEV_CMD=%%i\Common7\Tools\VsDevCmd.bat"
        )
    )
    if not defined VS_DEV_CMD (
        if exist "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat" (
            set "VS_DEV_CMD=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat"
        ) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" (
            set "VS_DEV_CMD=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
        ) else if exist "C:\Program Files\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat" (
            set "VS_DEV_CMD=C:\Program Files\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat"
        ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" (
            set "VS_DEV_CMD=C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
        ) else if exist "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" (
            set "VS_DEV_CMD=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
        ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" (
            set "VS_DEV_CMD=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
        )
    )
    if defined VS_DEV_CMD (
        echo Calling: !VS_DEV_CMD!
        call "!VS_DEV_CMD!" -arch=x64
    )
)

:: Configure if build folder does not exist
set "VSINSTALLDIR="
if not exist "build\CMakeCache.txt" (
    echo Configuring CMake project...
    cmake -B build -S . -A x64
    if errorlevel 1 exit /b 1
)

:: Build Release
cmake --build build --config Release
if errorlevel 1 (
    echo Initial build attempt failed. Retrying with fresh CMake configuration...
    if exist "build\CMakeCache.txt" del /f /q "build\CMakeCache.txt"
    set "VSINSTALLDIR="
    cmake -B build -S . -A x64
    if errorlevel 1 exit /b 1
    cmake --build build --config Release
    if errorlevel 1 (
        echo Build FAILED!
        exit /b 1
    )
)

echo Build SUCCEEDED! Artifacts available in build\bin\
