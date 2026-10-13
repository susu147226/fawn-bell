@echo off
setlocal
cd /d "%~dp0"
rem v143 is required: VS 2026 dropped the Windows 8.1 runtime target.
if not defined LULING_VC_ROOT set "LULING_VC_ROOT=%~dp0tools\win81-toolchain\VC\Tools\MSVC\14.44.35207"
if not exist "%LULING_VC_ROOT%\bin\Hostx64\x64\cl.exe" (
    echo Set LULING_VC_ROOT to a VS 2022 v143 toolset, or run tools\prepare_win81_toolchain.py.
    exit /b 1
)
call "%~dp0scripts\vcvars.bat"
if errorlevel 1 exit /b 1
set "PATH=%LULING_VC_ROOT%\bin\Hostx64\x64;%PATH%"
set "INCLUDE=%LULING_VC_ROOT%\include;%WindowsSdkDir%Include\%WindowsSDKVersion%ucrt;%WindowsSdkDir%Include\%WindowsSDKVersion%shared;%WindowsSdkDir%Include\%WindowsSDKVersion%um;%WindowsSdkDir%Include\%WindowsSDKVersion%winrt"
set "LIB=%LULING_VC_ROOT%\lib\x64;%WindowsSdkDir%Lib\%WindowsSDKVersion%ucrt\x64;%WindowsSdkDir%Lib\%WindowsSDKVersion%um\x64"
set "VSLANG=1033"
if not defined CMAKE_BUILD_PARALLEL_LEVEL set "CMAKE_BUILD_PARALLEL_LEVEL=4"
if not defined LUMATEXT_SOURCE_DIR if exist "%~dp0..\lumatext\CMakeLists.txt" set "LUMATEXT_SOURCE_DIR=%~dp0..\lumatext"
set "LULING_DEPS="
set "LULING_WIN81_SELFTEST=ON"
if /i "%~1"=="/release" set "LULING_WIN81_SELFTEST=OFF"
if /i "%~1"=="/production" set "LULING_WIN81_SELFTEST=OFF"
set /p LULING_UPDATE_KEY=<"%~dp0cmake\update-public-key.txt"
set LULING_UPDATE_CONFIG=-DLULING_UPDATE_MANIFEST_URL="https://github.com/susu147226/fawn-bell/releases/latest/download/update-manifest-win81.json" -DLULING_UPDATE_PUBLIC_KEY_HEX=%LULING_UPDATE_KEY%
if exist "%LUMATEXT_SOURCE_DIR%\build-vs18\_deps\harfbuzz-src\src\harfbuzz.cc" set LULING_DEPS=-DFETCHCONTENT_SOURCE_DIR_HARFBUZZ="%LUMATEXT_SOURCE_DIR%\build-vs18\_deps\harfbuzz-src" -DFETCHCONTENT_SOURCE_DIR_FREETYPE="%LUMATEXT_SOURCE_DIR%\build-vs18\_deps\freetype-src"
cmake -S . -B build-win81 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="%LULING_VC_ROOT%\bin\Hostx64\x64\cl.exe" -DCMAKE_CXX_COMPILER="%LULING_VC_ROOT%\bin\Hostx64\x64\cl.exe" -DCMAKE_LINKER="%LULING_VC_ROOT%\bin\Hostx64\x64\link.exe" -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DLULING_WITH_SELFTEST=%LULING_WIN81_SELFTEST% -DLULING_WIN81_CANDIDATE=ON -DLUMATEXT_SOURCE_DIR="%LUMATEXT_SOURCE_DIR%" -DLULING_WITH_LUMATEXT=ON %LULING_DEPS% %LULING_UPDATE_CONFIG%
if errorlevel 1 exit /b 1
if /i "%~1"=="/production" (cmake --build build-win81 --target luling) else (cmake --build build-win81)
exit /b %errorlevel%
