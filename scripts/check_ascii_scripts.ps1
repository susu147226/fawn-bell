# check_ascii_scripts.ps1
#
# Fails if any PowerShell script in the repository contains non-ASCII bytes.
#
# Spec 2.2 rule 2 is a hard rule: Windows PowerShell decodes a BOM-less UTF-8
# script using the ANSI code page, so a single Chinese character in a .ps1 makes
# the parser fail with a ParserError. This has already happened once, which is
# why the rule is machine-checked rather than trusted.
#
# This file MUST stay pure ASCII itself - it is subject to its own rule.
#
# Exit code 0 = clean, 1 = violations found, 2 = usage error.

[CmdletBinding()]
param(
    [string]$Root = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrEmpty($Root)) {
    $Root = Join-Path $PSScriptRoot '..'
}

if (-not (Test-Path -LiteralPath $Root)) {
    Write-Host ("usage: check_ascii_scripts.ps1 [-Root <repo root>]")
    Write-Host ("root not found: {0}" -f $Root)
    exit 2
}

$sourceRoot = (Resolve-Path -LiteralPath $Root).Path
$scriptsRoot = Join-Path $sourceRoot 'scripts'

if (-not (Test-Path -LiteralPath $scriptsRoot)) {
    Write-Host ("scripts directory not found under {0}" -f $sourceRoot)
    exit 2
}

$scripts = @(Get-ChildItem -LiteralPath $scriptsRoot -Recurse -File |
    Where-Object { $_.Extension -eq '.ps1' })

$violations = 0

foreach ($script in $scripts) {
    $bytes = [System.IO.File]::ReadAllBytes($script.FullName)
    for ($index = 0; $index -lt $bytes.Length; $index++) {
        if ($bytes[$index] -gt 0x7F) {
            $line = 1
            for ($scan = 0; $scan -lt $index; $scan++) {
                if ($bytes[$scan] -eq 0x0A) {
                    $line++
                }
            }
            Write-Host ("{0}({1}): non-ASCII byte 0x{2:X2}" -f `
                $script.Name, $line, $bytes[$index])
            $violations++
            break
        }
    }
}

if ($violations -gt 0) {
    Write-Host ("ASCII check FAILED: {0} script(s) contain non-ASCII bytes" -f $violations)
    exit 1
}

Write-Host ("ASCII check passed: {0} script(s) scanned" -f $scripts.Count)
exit 0
