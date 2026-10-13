@echo off
rem Pack a Release build into dist\LulingSetup-<version>[-win81].exe with the
rem Luling setup (src\setup bootstrapper + tools\pack_installer\pack_installer.py).
rem usage: scripts\pack_setup.bat <build dir> <windows|win81>
setlocal
cd /d "%~dp0.."

set "LULING_BUILD_DIR=%~1"
set "LULING_CHANNEL=%~2"
if not defined LULING_BUILD_DIR set "LULING_BUILD_DIR=build"
if not defined LULING_CHANNEL set "LULING_CHANNEL=windows"
rem Accept a relative (to the repository) or absolute build directory.
for %%I in ("%LULING_BUILD_DIR%") do set "LULING_BUILD_DIR=%%~fI"
set "LULING_SUFFIX="
if /i "%LULING_CHANNEL%"=="win81" set "LULING_SUFFIX=-win81"

set /p LULING_VERSION=<version.txt
if not defined LULING_VERSION (
    echo Missing version in version.txt
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%CD%\scripts\check_release_payload.ps1" -BuildDir "%LULING_BUILD_DIR%" || exit /b 1

for %%F in (luling.exe Luling.Index.exe Luling.Document.exe Luling.Preview.exe luling_shell.exe luling_elevated.exe luling_integration.exe lumatext.dll pdfium.dll) do (
    if not exist "%LULING_BUILD_DIR%\%%F" (
        echo Missing build output: %LULING_BUILD_DIR%\%%F
        exit /b 1
    )
)

rem The payload carries no CRT DLLs, so every binary must link the static runtime.
powershell -NoProfile -ExecutionPolicy Bypass -File "%CD%\scripts\stage_installer_runtime.ps1" -BuildDir "%LULING_BUILD_DIR%" -RequireStaticRuntime || exit /b 1

call "%CD%\scripts\vcvars.bat" >nul || exit /b 1
chcp 65001 >nul
set "VSLANG=1033"
cmake --build "%LULING_BUILD_DIR%" --target luling_setup || exit /b 1

set "LULING_PYTHON=python"
where python >nul 2>&1 || set "LULING_PYTHON=py -3"
%LULING_PYTHON% tools\pack_installer\pack_installer.py --root . --build "%LULING_BUILD_DIR%" --stub "%LULING_BUILD_DIR%\luling_setup.exe" --version %LULING_VERSION% --channel %LULING_CHANNEL% --out "dist\LulingSetup-%LULING_VERSION%%LULING_SUFFIX%.exe" || exit /b 1
echo.
echo Installer written to dist\LulingSetup-%LULING_VERSION%%LULING_SUFFIX%.exe
