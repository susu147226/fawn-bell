# check_token_literals.ps1
#
# Scans C++ and resource sources for literal colour values and hard-coded motion
# durations. Design tokens are the single source of truth (spec 11.1 and 15.2
# item 6), so any hit outside the token table fails the check.
#
# The colour rules are exact. The duration and size rules are heuristics on
# identifier names; extend $tokenAllowList if one of them ever fires on a
# legitimate constant.
#
# No trailing word boundary on the hex rule: a suffix such as the "u" in
# 0xFF1E88E5u would otherwise stop it from matching at all.
#
# "hash" colours (#RRGGBB) are deliberately not scanned: '#' is not colour
# syntax in C/C++/RC, and any rule loose enough to catch them also flags
# preprocessor lines such as "#def" from "#define" (d, e and f are hex digits).
#
# This file MUST stay pure ASCII (spec 2.2 rule 2): Windows PowerShell decodes a
# BOM-less UTF-8 script using the ANSI code page, and non-ASCII text here would
# turn into a ParserError.
#
# Exit code 0 = clean, 1 = violations found, 2 = usage error.

[CmdletBinding()]
param(
    [string]$Root = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Resolved here rather than in the param default: Windows PowerShell does not
# populate $PSScriptRoot until the script body runs.
if ([string]::IsNullOrEmpty($Root)) {
    $Root = Join-Path $PSScriptRoot '..'
}

if (-not (Test-Path -LiteralPath $Root)) {
    Write-Host ("usage: check_token_literals.ps1 [-Root <repo root>]")
    Write-Host ("root not found: {0}" -f $Root)
    exit 2
}

$sourceRoot = (Resolve-Path -LiteralPath $Root).Path

# Where literals are supposed to live: the token table. Everything else must
# take its values from there.
$tokenAllowList = @(
    'src\ui\FluentTokens.h'
)

# Exempted per file, not per value: reparse tags come from the Windows headers
# (IO_REPARSE_TAG_*), so no source has to spell them out today. Kept for a file
# that ever has to declare such constants by hand.
$systemConstantAllowList = @(
)

# Structural constants that are not palette colours: zero alpha, full opacity
# and a zero-alpha mask. They carry no design meaning and are safe to spell out.
$colourConstants = @(
    '0x00000000',
    '0xFFFFFFFF',
    '0xFF000000'
)

$rules = @(
    [pscustomobject]@{
        Name    = 'hex-colour'
        Detail  = 'literal colour value; take it from the token table'
        Pattern = '0x[0-9A-Fa-f]{6,8}'
    },
    [pscustomobject]@{
        Name    = 'colour-literal'
        Detail  = 'colour constructor called with numeric literals; token expected'
        # Non-zero leading digit only: ColorF(0, ...) is a transparent clear,
        # a structural constant rather than a palette colour.
        Pattern = '\b(?:RGB|RGBA|ColorF|Colour|Color)\s*\(\s*[1-9]'
    },
    [pscustomobject]@{
        Name    = 'motion-duration'
        Detail  = 'hard-coded duration; motionDuration token expected'
        Pattern = '(?i)\b\w*(?:duration|motion)(?:_\w*)?\s*=\s*[0-9]+'
    },
    [pscustomobject]@{
        Name    = 'hard-coded-size'
        Detail  = 'hard-coded size; take it from the token table'
        # Non-zero only: "= 0" is an unset initialiser, not a size.
        Pattern = '(?i)\b\w*(?:width|height|radius|spacing|padding|margin|gap)(?:_\w*)?\s*=\s*[1-9][0-9]*'
    }
)

$scannedExtensions = @('.h', '.hpp', '.cpp', '.cxx', '.rc', '.in')
$scanDirectories = @('src', 'res')

$files = @()
foreach ($directory in $scanDirectories) {
    $full = Join-Path $sourceRoot $directory
    if (-not (Test-Path -LiteralPath $full)) {
        continue
    }
    $found = Get-ChildItem -LiteralPath $full -Recurse -File |
        Where-Object { $scannedExtensions -contains $_.Extension }
    $files += $found
}

$violations = 0

foreach ($file in $files) {
    $relative = $file.FullName.Substring($sourceRoot.Length + 1)

    if ($tokenAllowList -contains $relative) {
        continue
    }
    if ($systemConstantAllowList -contains $relative) {
        continue
    }

    $lines = @(Get-Content -LiteralPath $file.FullName)
    for ($index = 0; $index -lt $lines.Count; $index++) {
        $text = $lines[$index]
        foreach ($rule in $rules) {
            foreach ($match in [regex]::Matches($text, $rule.Pattern)) {
                if ($rule.Name -eq 'hex-colour' -and ($colourConstants -contains $match.Value)) {
                    continue
                }
                Write-Host ("{0}({1}): {2}: {3} [{4}]" -f `
                    $relative, ($index + 1), $rule.Name, $match.Value, $rule.Detail)
                $violations++
            }
        }
    }
}

if ($violations -gt 0) {
    Write-Host ("token literal check FAILED: {0} violation(s)" -f $violations)
    exit 1
}

Write-Host ("token literal check passed: {0} file(s) scanned" -f $files.Count)
exit 0
