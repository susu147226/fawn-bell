#requires -Version 7.2
#
# Runs the regression gate through CTest. The test list lives in
# cmake/tests.cmake and mirrors scripts/build_release_ci.ps1.
#
# Usage:
#   scripts\run_tests.ps1                 # whole gate
#   scripts\run_tests.ps1 -Filter 'preview'   # subset by CTest -R regex
#
# The build directory must already be configured and built for the tests you
# want to run; CTest only executes what exists.
#
# This file MUST stay pure ASCII, like every script in this directory.

param(
    [string]$BuildDir = 'build',
    [string]$Filter = '',
    [switch]$ShowOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$build = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $repo $BuildDir }
if (-not (Test-Path -LiteralPath $build)) {
    throw "Build directory not found: $build"
}

$arguments = @('--test-dir', $build, '--output-on-failure')
if ($Filter) { $arguments += @('-R', $Filter) }
if ($ShowOnly) { $arguments += '-N' }

& ctest @arguments
exit $LASTEXITCODE
