#requires -Version 7.2
# Stages a release: signs the update manifest, creates the GitHub release and
# uploads its assets. With -Draft it stops while the release is still a draft,
# so it can be reviewed before it goes public; without it the release is
# published at the end.
param(
    [switch]$Draft
)
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path -Parent $PSScriptRoot)
$version = (Get-Content version.txt -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid release version' }
$tag = "v$version"
$repository = 'susu147226/fawn-bell'
$base = "https://github.com/$repository/releases/download/$tag"
$normal = "LulingSetup-$version.exe"
$win81 = "LulingSetup-$version-win81.exe"
$portable = "Luling-$version-portable-win-x64.zip"
if (-not (Test-Path -LiteralPath "dist/$normal")) { throw "Missing installer: $normal" }
# The Windows 8.1 build is a separate channel: ship it only when it was packaged.
$hasWin81 = Test-Path -LiteralPath "dist/$win81"
if (-not $env:LULING_UPDATE_PRIVATE_KEY) { throw 'Configure the LULING_UPDATE_PRIVATE_KEY Actions secret first' }
$keyPath = Join-Path ([IO.Path]::GetTempPath()) ("luling-sign-" + [guid]::NewGuid().ToString('N') + '.pem')
$publicKey = (Get-Content cmake/update-public-key.txt -Raw).Trim()
try {
    [IO.File]::WriteAllText($keyPath, $env:LULING_UPDATE_PRIVATE_KEY)
    & "$PSScriptRoot/create_update_manifest.ps1" -Installer "dist/$normal" -DownloadPage "$base/$normal" `
        -PrivateKey $keyPath -ExpectedPublicKey $publicKey -MinimumWindowsBuild 10240 -Output 'dist/update-manifest.json'
    if ($hasWin81) {
        & "$PSScriptRoot/create_update_manifest.ps1" -Installer "dist/$win81" -DownloadPage "$base/$win81" `
            -PrivateKey $keyPath -ExpectedPublicKey $publicKey -MinimumWindowsBuild 9600 -Output 'dist/update-manifest-win81.json'
    }
} finally {
    Remove-Item -LiteralPath $keyPath -ErrorAction SilentlyContinue
}
# CI packages the portable archive with the normal channel; a ZIP uploaded to the
# release draft before tagging is still accepted as a fallback.
if (-not (Test-Path -LiteralPath "dist/$portable")) {
    & gh release download $tag --repo $repository --pattern $portable --dir dist
    if ($LASTEXITCODE -ne 0) { throw 'Upload the portable ZIP to the release draft before publishing' }
}
$changesPath = "docs/releases/$version.md"
$changes = if (Test-Path $changesPath) { Get-Content $changesPath -Raw } else { '修复问题并改进使用体验。' }
$notesPath = 'dist/release-notes.md'
$packRows = "| **Windows 10 / Windows 11** | [$normal]($base/$normal) |"
if ($hasWin81) {
    $packRows += "`n| **Windows 8.1 兼容版** | [$win81]($base/$win81) |"
}
$packRows += "`n| **Windows 10 / 11 免安装版** | [$portable]($base/$portable) |"
@"
# 鹿铃 $version

## 下载安装

请按你的 Windows 版本选择，均为 **64 位（x64）**。

| 系统 | 安装包 |
| --- | --- |
$packRows

免安装版解压后运行 luling.exe；配置和缓存仍保存在当前用户的应用数据目录。ZIP 不会安装索引服务；需要全盘后台索引服务时请使用安装包。免安装版升级请下载新版 ZIP。

## 本次更新

$changes

## 客户端更新

鹿铃启动后会自动检查新版。有更新时窗口内会出现提示，点击即可下载安装包，校验通过后打开安装向导；也可以在「设置 → 关于与诊断」中手动检查。Windows 必要的管理员确认仍需由你确认。

Assets 中的 update-manifest.json 是客户端使用的更新清单，无需手动下载。
"@ | Set-Content -LiteralPath $notesPath -Encoding utf8
$existing = & gh release view $tag --repo $repository --json isDraft 2>$null
if ($LASTEXITCODE -eq 0) {
    if (-not ($existing | ConvertFrom-Json).isDraft) { throw 'This version is already published; increment version.txt' }
    & gh release edit $tag --repo $repository --title "鹿铃 $version" --notes-file $notesPath
} else {
    & gh release create $tag --repo $repository --draft --title "鹿铃 $version" --notes-file $notesPath
}
if ($LASTEXITCODE -ne 0) { throw 'Could not prepare the release draft' }
$assets = @("dist/$normal", "dist/$portable", 'dist/update-manifest.json')
if ($hasWin81) { $assets += @("dist/$win81", 'dist/update-manifest-win81.json') }
& gh release upload $tag --repo $repository @assets --clobber
if ($LASTEXITCODE -ne 0) { throw 'Could not upload release assets; release remains a draft' }
# Preview packs are published separately; verify them only when they were staged.
if (Test-Path -LiteralPath 'dist/preview-packs') {
    $packArgs = @('--tag', $tag, '--repository', $repository, '--asset-directory', 'dist/preview-packs')
    if ($env:LULING_PREVIEW_PACK_SOURCE_TAG) { $packArgs += @('--source-tag', $env:LULING_PREVIEW_PACK_SOURCE_TAG) }
    & python "$PSScriptRoot/ensure_release_packs.py" @packArgs
    if ($LASTEXITCODE -ne 0) { throw 'Preview pack verification failed; release remains a draft' }
}
if ($Draft) {
    Write-Host "Draft release ready: $base"
    exit 0
}
& gh release edit $tag --repo $repository --draft=false --latest
if ($LASTEXITCODE -ne 0) { throw 'Could not publish the release' }
