@echo off
call "%~dp0scripts\vcvars.bat"
if errorlevel 1 exit /b 1
rem Match CMake/Ninja dependency output encoding, including localized MSVC installs.
chcp 65001 >nul
set "VSLANG=1033"
set "LULING_LUMATEXT_PREBUILT="
if exist "%~dp0third_party\lumatext\bin\lumatext.dll" set "LULING_LUMATEXT_PREBUILT=1"
if not defined LUMATEXT_SOURCE_DIR if not defined LULING_LUMATEXT_PREBUILT if exist "%~dp0..\lumatext\CMakeLists.txt" set "LUMATEXT_SOURCE_DIR=%~dp0..\lumatext"
set "LULING_CMAKE_FRESH="
set "LULING_CMAKE_CLEAN="
set "LULING_BUILD_TYPE=Release"
rem Signed packages archive PDBs; optimized RelWithDebInfo emits matching symbols.
if /i "%~1"=="/symbols" set "LULING_BUILD_TYPE=RelWithDebInfo"
if /i "%~1"=="/clean" (
    set "LULING_CMAKE_FRESH=--fresh"
    set "LULING_CMAKE_CLEAN=--clean-first"
)
set "LULING_DEPS="
set "LULING_SYMBOLS=OFF"
if /i "%~1"=="/symbols" set "LULING_SYMBOLS=ON"
set /p LULING_UPDATE_KEY=<"%~dp0cmake\update-public-key.txt"
set LULING_UPDATE_CONFIG=-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DLULING_WITH_SELFTEST=OFF -DLULING_RELEASE_SYMBOLS=%LULING_SYMBOLS% -DLULING_UPDATE_MANIFEST_URL="https://github.com/susu147226/fawn-bell/releases/latest/download/update-manifest.json" -DLULING_UPDATE_PUBLIC_KEY_HEX=%LULING_UPDATE_KEY%
if exist "%LUMATEXT_SOURCE_DIR%\build-vs18\_deps\harfbuzz-src\src\harfbuzz.cc" set LULING_DEPS=-DFETCHCONTENT_SOURCE_DIR_HARFBUZZ="%LUMATEXT_SOURCE_DIR%\build-vs18\_deps\harfbuzz-src" -DFETCHCONTENT_SOURCE_DIR_FREETYPE="%LUMATEXT_SOURCE_DIR%\build-vs18\_deps\freetype-src"
if defined LUMATEXT_SOURCE_DIR (
    cmake %LULING_CMAKE_FRESH% -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=%LULING_BUILD_TYPE% -DLUMATEXT_SOURCE_DIR="%LUMATEXT_SOURCE_DIR%" -DLULING_WITH_LUMATEXT=ON %LULING_DEPS% %LULING_UPDATE_CONFIG%
) else if defined LULING_LUMATEXT_PREBUILT (
    rem -DLUMATEXT_SOURCE_DIR= clears a cached source tree; CMakeLists.txt picks
    rem up the committed third_party\lumatext package on its own.
    cmake %LULING_CMAKE_FRESH% -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=%LULING_BUILD_TYPE% -DLUMATEXT_SOURCE_DIR= -DLULING_WITH_LUMATEXT=ON %LULING_UPDATE_CONFIG%
) else (
    cmake %LULING_CMAKE_FRESH% -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=%LULING_BUILD_TYPE% %LULING_UPDATE_CONFIG%
)
if errorlevel 1 exit /b 1
cmake --build build %LULING_CMAKE_CLEAN%
if errorlevel 1 exit /b 1
