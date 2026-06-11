@echo off
setlocal enabledelayedexpansion

cd /d "%~dp0\.."

for /f "usebackq tokens=*" %%i in (
    `"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`
) do set "VS_PATH=%%i"

if not defined VS_PATH (
    echo ERROR: Visual Studio not found
    exit /b 1
)

call "%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
if errorlevel 1 (
    echo ERROR: vcvarsall.bat failed
    exit /b 1
)

set "BUILD_DIR=build"

cmake -B %BUILD_DIR% -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="%VS_PATH%"
if errorlevel 1 (
    echo ERROR: CMake configure failed
    exit /b 1
)

cmake --build %BUILD_DIR% --config Release
if errorlevel 1 (
    echo ERROR: Build failed
    exit /b 1
)

echo.
echo Build succeeded: %BUILD_DIR%\Release\nanobot-manager.exe
endlocal
