@echo off
setlocal enabledelayedexpansion

cd /d "%~dp0\.."

echo [1/3] Building nanobot-manager...
call scripts\build.bat
if errorlevel 1 (
    echo ERROR: Build failed
    exit /b 1
)

echo.
echo [2/3] Preparing payload...

if not exist "build\Release\nanobot-manager.exe" (
    echo ERROR: nanobot-manager.exe not found
    exit /b 1
)

if not exist "dist" mkdir dist

echo.
echo [3/3] Creating installer...

set "NSIS_PATH="
for /f "usebackq tokens=*" %%i in (
    `where makensis 2^>nul`
) do set "NSIS_PATH=%%i"

if not defined NSIS_PATH (
    set "NSIS_PATH=C:\Program Files (x86)\NSIS\makensis.exe"
)

if not exist "!NSIS_PATH!" (
    echo ERROR: makensis not found. Install NSIS first.
    exit /b 1
)

"!NSIS_PATH!" installer\installer.nsi
if errorlevel 1 (
    echo ERROR: NSIS packaging failed
    exit /b 1
)

echo.
echo Package succeeded: dist\nanobot-manager-setup.exe
endlocal
