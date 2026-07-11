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

rem Prefer cmake from PATH; fall back to the CMake bundled with Visual Studio.
set "CMAKE_EXE=cmake"
where cmake >nul 2>&1
if errorlevel 1 (
    set "CMAKE_EXE=%VS_PATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if not exist "!CMAKE_EXE!" (
        echo ERROR: cmake not found. Install CMake or the Visual Studio C++ CMake tools.
        exit /b 1
    )
)

set "BUILD_DIR=build"

rem Prefer the newest installed Visual Studio generator available to CMake.
set "CMAKE_GENERATOR=Visual Studio 17 2022"
if exist "%VS_PATH%\MSBuild\Current\Bin\MSBuild.exe" (
    for /f "usebackq tokens=*" %%i in (
        `"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property catalog_productLineVersion`
    ) do set "VS_YEAR=%%i"
    if defined VS_YEAR (
        if "!VS_YEAR!"=="2026" set "CMAKE_GENERATOR=Visual Studio 18 2026"
        if "!VS_YEAR!"=="2022" set "CMAKE_GENERATOR=Visual Studio 17 2022"
        if "!VS_YEAR!"=="2019" set "CMAKE_GENERATOR=Visual Studio 16 2019"
    )
    rem Fallback: VS 18 path layout without catalog year
    echo !VS_PATH! | findstr /I "\\18\\" >nul && set "CMAKE_GENERATOR=Visual Studio 18 2026"
)

echo Using CMake generator: !CMAKE_GENERATOR!
rem CMAKE_POLICY_VERSION_MINIMUM: CMake 4.x rejects cJSON's cmake_minimum_required(3.0)
"!CMAKE_EXE!" -B %BUILD_DIR% -G "!CMAKE_GENERATOR!" -A x64 -DCMAKE_PREFIX_PATH="%VS_PATH%" -DCMAKE_POLICY_VERSION_MINIMUM=3.5
if errorlevel 1 (
    echo ERROR: CMake configure failed
    exit /b 1
)

"!CMAKE_EXE!" --build %BUILD_DIR% --config Release
if errorlevel 1 (
    echo ERROR: Build failed
    exit /b 1
)

echo.
echo Build succeeded: %BUILD_DIR%\Release\nanobot-manager.exe
endlocal
