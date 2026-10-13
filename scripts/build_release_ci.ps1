#requires -Version 7.2
param(
    [ValidateSet('windows', 'win81')][string]$Channel = 'windows',
    [string]$BuildDir = 'build-ci'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo
$build = [IO.Path]::GetFullPath((Join-Path $repo $BuildDir))
if (-not $build.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'BuildDir must be inside the repository'
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$toolsetEnvironment = ''
$compilerArguments = ''
if (-not $vs) {
    # A local extracted v143 toolset can share the installed Windows SDK.
    $toolset = Join-Path $repo 'tools/win81-toolchain/VC/Tools/MSVC/14.44.35207'
    if (-not (Test-Path "$toolset/bin/Hostx64/x64/cl.exe")) { throw 'Visual Studio 2022 C++ tools are required' }
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'An installed Windows SDK developer environment is required' }
    $toolsetEnvironment = @"
set "PATH=$toolset\bin\Hostx64\x64;%PATH%"
set "INCLUDE=$toolset\include;%WindowsSdkDir%Include\%WindowsSDKVersion%ucrt;%WindowsSdkDir%Include\%WindowsSDKVersion%shared;%WindowsSdkDir%Include\%WindowsSDKVersion%um;%WindowsSdkDir%Include\%WindowsSDKVersion%winrt"
set "LIB=$toolset\lib\x64;%WindowsSdkDir%Lib\%WindowsSDKVersion%ucrt\x64;%WindowsSdkDir%Lib\%WindowsSDKVersion%um\x64"
"@
    $compilerArguments = "-DCMAKE_C_COMPILER=`"$toolset/bin/Hostx64/x64/cl.exe`" -DCMAKE_CXX_COMPILER=`"$toolset/bin/Hostx64/x64/cl.exe`" -DCMAKE_LINKER=`"$toolset/bin/Hostx64/x64/link.exe`""
}
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
# The release tag pins the complete SDK; CI verifies it before configuring CMake.
$sdkRoot = & (Join-Path $PSScriptRoot 'verify_lumatext_sdk.ps1')
$candidate = if ($Channel -eq 'win81') { 'ON' } else { 'OFF' }
$manifest = if ($Channel -eq 'win81') { 'update-manifest-win81.json' } else { 'update-manifest.json' }
$publicKey = (Get-Content (Join-Path $repo 'cmake/update-public-key.txt') -Raw).Trim()
New-Item -ItemType Directory -Path $build -Force | Out-Null
# Release verification is scoped to the changes being shipped. luling already
# depends on all three packaged hosts; standalone tests need explicit targets.
$testNames = @('luling_rename_ops_test', 'luling_child_edit_test', 'luling_localization_test',
    'luling_update_test', 'luling_update_installer_test', 'luling_update_session_test', 'luling_app_controllers_test',
    'luling_change_tracking_polling_test', 'luling_change_tracking_test', 'luling_index_delta_replay_test',
    'luling_runtime_log_test', 'luling_diagnostics_export_test', 'luling_shell_command_test', 'luling_reparse_entry_test',
    'luling_link_destination_test', 'luling_link_pill_test', 'luling_shell_icons_test', 'luling_network_locations_test',
    'luling_archive_integrity_test', 'luling_document_completeness_test', 'luling_preview_command_test',
    'luling_image_preview_integrity_test', 'luling_udf_listing_test', 'luling_preview_integrity_test',
    'luling_change_tracking_memory_test', 'luling_change_feed_memory_test', 'luling_usn_packet_queue_test',
    'luling_content_progress_ui_test', 'luling_operation_presentation_test', 'luling_column_strip_test', 'luling_entry_sort_key_test',
    'luling_file_lock_test', 'luling_dialogs_test')
$testTargets = (@('luling', 'luling_index_engine_test', 'luling_index_host_stress',
    'luling_preview_test', 'luling_preview_handler_probe', 'luling_playback_controls_test', 'luling_ops_test') + $testNames) -join ' '
$batch = Join-Path $build 'compile-release.bat'
@"
@echo off
call "$vcvars"
if errorlevel 1 exit /b 1
chcp 65001 >nul
set "VSLANG=1033"
$toolsetEnvironment
cmake -S "$repo" -B "$build" -G Ninja $compilerArguments -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DLULING_WIN81_CANDIDATE=$candidate -DLULING_WITH_SELFTEST=ON -DLULING_BUILD_PROGRESS_UI_TESTS=ON -DLULING_WITH_LUMATEXT=ON -DLUMATEXT_SOURCE_DIR= -DCMAKE_PREFIX_PATH="$sdkRoot" -DLULING_UPDATE_MANIFEST_URL="https://github.com/susu147226/fawn-bell/releases/latest/download/$manifest" -DLULING_UPDATE_PUBLIC_KEY_HEX=$publicKey
if errorlevel 1 exit /b 1
cmake --build "$build" --parallel 4 --target $testTargets
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $batch -Encoding ascii
& $batch
if ($LASTEXITCODE -ne 0) { throw 'Release compilation failed' }
foreach ($testName in $testNames) {
    & (Join-Path $build "$testName.exe")
    if ($LASTEXITCODE -ne 0) { throw "$testName failed" }
}
& (Join-Path $build 'luling_ops_test.exe') --update-shutdown
if ($LASTEXITCODE -ne 0) { throw 'Update shutdown and automatic wait regression failed' }
& (Join-Path $build 'luling_ops_test.exe') --delete-progress-only
if ($LASTEXITCODE -ne 0) { throw 'Delete progress rate and remaining-time regression failed' }
& (Join-Path $build 'luling_playback_controls_test.exe') --timeline-only
if ($LASTEXITCODE -ne 0) { throw 'Playback timeline regression failed' }
& (Join-Path $build 'luling_preview_test.exe') --vector-only
if ($LASTEXITCODE -ne 0) { throw 'Vector preview regression failed' }
& (Join-Path $build 'luling_preview_handler_probe.exe') --cooldown-test
if ($LASTEXITCODE -ne 0) { throw 'Preview provider cooldown regression failed' }
foreach ($mode in @('--startup-stop', '--shell-roundtrip')) {
    & (Join-Path $build 'luling_rename_ops_test.exe') $mode
    if ($LASTEXITCODE -ne 0) { throw "Rename lifecycle check $mode failed" }
}
$env:LULING_SELFTEST_NO_SCREENSHOTS = '1'
foreach ($mode in @('--parent-cycle-only', '--quiet-maintenance-only', '--name-pool-only',
    '--maintenance-only', '--usn-only', '--feed-only', '--folder-sizes-only', '--hierarchy-only', '--visibility-cache-only')) {
    & (Join-Path $build 'luling_index_engine_test.exe') $mode
    if ($LASTEXITCODE -ne 0) { throw "Index regression $mode failed" }
}
& (Join-Path $build 'luling_app_controllers_test.exe') --layout-search-prefs
if ($LASTEXITCODE -ne 0) { throw 'Panel layout/preferences regression failed' }
foreach ($mode in @('--layout-active-pane', '--network-locations-view')) {
    & (Join-Path $build 'luling_app_controllers_test.exe') $mode
    if ($LASTEXITCODE -ne 0) { throw "Panel/network navigation regression $mode failed" }
}
& (Join-Path $build 'luling_change_feed_memory_test.exe') --probe
if ($LASTEXITCODE -ne 0) { throw 'Opt-in memory probe regression failed' }
foreach ($mode in @('--service-start-only', '--shutdown-only', '--live-dedup-only', '--folder-sizes-only')) {
    & (Join-Path $build 'luling_index_host_stress.exe') $mode
    if ($LASTEXITCODE -ne 0) { throw "Index lifecycle check $mode failed" }
}
$selftestCases = @('rename-editor', 'rename-editor-native', 'operation-toast',
    'filter-controls', 'rename-outside', 'address-editor', 'address-editor-native', 'release-panels-hidden', 'pr-shell', 'pin-reorder', 'snapshot-patch', 'list-columns',
    'filename-render', 'filename-render-native', 'folder-views', 'pane-header-icons', 'folder-sizes', 'tray-stack')
$selftestLogs = @{
    'folder-sizes' = 'bench_data/folder-sizes/results.log'
    'pane-header-icons' = 'bench_data/pane-header-icons/results.log'
    'filename-render' = 'bench_data/filename-render/results.log'
    'filename-render-native' = 'bench_data/filename-render/results.log'
    'rename-editor' = 'bench_data/rename-editor/results.log'
    'rename-editor-native' = 'bench_data/rename-editor/results.log'
    'operation-toast' = 'bench_data/operation-toast/results.log'
    'address-editor' = 'bench_data/address-editor/results.log'
    'address-editor-native' = 'bench_data/address-editor/results.log'
}
foreach ($testCase in $selftestCases) {
    $env:LULING_SELFTEST_CASE = $testCase -replace '-native$', ''
    $env:LULING_LUMATEXT = if ($testCase.EndsWith('-native')) { '0' } else { '1' }
    $selftest = Start-Process -FilePath (Join-Path $build 'luling.exe') -ArgumentList '--selftest' -WindowStyle Hidden -PassThru
    $finished = $selftest.WaitForExit(120000)
    if (-not $finished) { $selftest.Kill(); $selftest.WaitForExit() }
    $selftest.Refresh()
    $caseLog = Join-Path $build "selftest-$testCase.log"
    $sourceLog = if ($selftestLogs.ContainsKey($testCase)) { $selftestLogs[$testCase] } else { 'bench_data/selftest_1b2_last.log' }
    Copy-Item $sourceLog $caseLog -ErrorAction SilentlyContinue
    if (-not $finished) { throw "Selftest $testCase timed out" }
    if ($selftest.ExitCode -ne 0) {
        Get-Content $caseLog -ErrorAction SilentlyContinue | Select-String '\[FAIL\]'
        throw "Selftest $testCase failed: $($selftest.ExitCode)"
    }
}
Remove-Item Env:LULING_SELFTEST_CASE
$liveLog = Join-Path $build 'content-live-selection.log'
$env:LULING_TEST_SEARCH_FLOW = $liveLog
$env:LULING_TEST_CONTENT_LIVE_SELECTION = '1'
$live = Start-Process -FilePath (Join-Path $build 'luling.exe') -ArgumentList '--test-instance', '--shot',
    (Join-Path $build 'content-live-selection.png'), $env:TEMP -WindowStyle Hidden -PassThru
$liveDone = $live.WaitForExit(60000)
if (-not $liveDone) { $live.Kill(); $live.WaitForExit() }
$live.Refresh()
Remove-Item Env:LULING_TEST_SEARCH_FLOW, Env:LULING_TEST_CONTENT_LIVE_SELECTION
if (-not $liveDone -or $live.ExitCode -ne 0) {
    Get-Content $liveLog -ErrorAction SilentlyContinue
    throw 'Live content selection regression failed'
}
Remove-Item Env:LULING_LUMATEXT
# Strip the embedded test suite from the shipped executable after verification.
(Get-Content -LiteralPath $batch -Raw).Replace('-DLULING_WITH_SELFTEST=ON', '-DLULING_WITH_SELFTEST=OFF').Replace("--target $testTargets", '--target luling') |
    Set-Content -LiteralPath $batch -Encoding ascii
& $batch
if ($LASTEXITCODE -ne 0) { throw 'Production compilation failed' }
& (Join-Path $repo 'scripts/check_release_payload.ps1') -BuildDir $build
if ($Channel -eq 'win81') {
    python tools/audit_win81_imports.py $build
    if ($LASTEXITCODE -ne 0) { throw 'Windows 8.1 startup import guard failed' }
}
$licenses = Join-Path $build 'licenses/LumaText'
New-Item -ItemType Directory -Path $licenses -Force | Out-Null
Copy-Item -Path (Join-Path $sdkRoot 'share/LumaText/licenses/*') -Destination $licenses
# Luling setup: bootstrapper + xz payload (scripts/pack_setup.bat).
& (Join-Path $repo 'scripts/pack_setup.bat') $build $Channel
if ($LASTEXITCODE -ne 0) { throw 'Installer packaging failed' }
# The portable ZIP targets Windows 10/11 x64, so only the normal channel packages it,
# from the same verified production build as the installer.
if ($Channel -eq 'windows') {
    & (Join-Path $repo 'scripts/package_portable.ps1') -BuildDir $build
}