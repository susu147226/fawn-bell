# Luling (鹿铃)

**A file manager for Windows: browse, search and organize, all in one workspace.**

Tabs and multi-pane layouts, whole-disk indexed search with pinyin matching, Space-bar Quick Look, a staging tray with text diff, plus a customizable look and close Windows integration.

[Download the latest release](https://github.com/susu147226/fawn-bell/releases/latest) · [Release notes](https://github.com/susu147226/fawn-bell/releases) · [Report an issue](https://github.com/susu147226/fawn-bell/issues) · [简体中文](README.md)

![The Luling main window](docs/images/appearance.png)

## Download and install

| Edition | System | File |
| --- | --- | --- |
| Installer (64-bit) | Windows 10 / Windows 11 | `LulingSetup-<version>.exe` |
| Portable (64-bit) | Windows 10 / Windows 11 | `Luling-<version>-portable-win-x64.zip` |

The installer can register the **LulingIndex** service for instant whole-disk search. The portable build runs straight from its folder; its settings and caches stay in your user data directory, and the index service needs the installer.

The UI ships in Simplified Chinese, Traditional Chinese and English; it follows Windows by default and can be changed in Settings → Display language.

## Browsing and views

- Five layouts — single, split left/right, split top/bottom, three panes and quad (Ctrl+1/2/3/4) — each tab remembering its own layout and location.
- Eight views: icons (four sizes), list, details, tiles, content, columns and folder view; the columns view expands subdirectories level by level.
- Seven sort keys, ascending or descending, with three folder-first modes; group by name, date, type or size with sticky headers; view, sort and grouping are remembered per folder and can be applied to all folders at once.
- The details view offers smart dates ("Today 14:32"), zebra rows, colour-coded extension chips, size-ratio bars, content-fitted column widths, and a column picker on the header.
- Compare two folders side by side to see what differs, what is missing and what is newer; folder sizes are computed in the background.

![Grouped view](docs/images/group.png)

## Tabs and windows

- Tabs can be grouped, torn out into new windows, pinned and colour-marked; a cap on open tabs is configurable.
- The tab strip can live vertically in the sidebar (Ctrl+B to collapse or expand).
- Tabs, layout and location are restored on restart.

![Tabs and colour marks](docs/images/tabs.png)

## Search

- **File-name search** runs on LulingIndex and updates as you type; scope it to the current folder or any folder you pick.
- Full pinyin, initials and polyphonic characters all match, so `sh` finds 品牌设计, 城市夜景 and 山湖晨雾, with the matched text highlighted.
- **Content search** covers text and code, PDF, Word, Excel, PowerPoint and more, streaming results as they arrive; opening a location keeps a way back to the results.
- Advanced search (Ctrl+Shift+F) filters by type, date and size; the global quick search (Alt+Space) is always one keystroke away and can hand results to the main window.
- Type `cmd`, `powershell`, `pwsh` or `wt` in the address bar to open a terminal in the current folder.

## Quick Look

Select a file and press Space — no need to open the app that owns it:

- Markdown rendered as laid out, code with syntax highlighting, PDFs page by page, folders as a contents list.
- CSV / TSV / XLSX as tables; JSON / XML as collapsible trees; Jupyter notebooks cell by cell.
- DOCX, EPUB and Office documents readable without the matching software installed; archives as a directory tree with in-archive search.
- Video playback with scrubbing and frame stepping, animated WebP / APNG playback, plus SVG, PSD, AI, DWG thumbnails and fonts.

Settings → Quick Look → Supported formats lists every previewable format and checks whether the HEIF / HEVC / AV1 / WebP system extensions are installed.

## Staging tray and text diff

Files you copy or cut (Ctrl+C / Ctrl+X) collect in the staging tray at the bottom left, so you can gather them across folders and tabs and drop them all at the destination at once. Drops copy by default; hold Shift to move.

With two files staged you can compare location, modified time, size and content; for text files "View differences" opens a diff window with side-by-side or unified views, intra-line highlighting, changes-only mode, whitespace and case options, and F7 / Shift+F7 navigation.

![Staging tray](docs/images/tray.png)

## Details panel

Selecting a file shows a preview, its basic properties and its tags in the details panel on the right. With several selected you can open a combined properties window, or tag and rename them together.

![Details panel](docs/images/details.png)

## Appearance

- Light, dark or follow-system, switchable from the title bar; accent colour, window effects (Mica, acrylic), wallpaper background, UI opacity and background blur.
- Three text-rendering modes (automatic, sharp, smooth) and three row heights (compact, standard, relaxed).
- Seven colour labels for files and folders, summarized in the sidebar and available from the Explorer context menu too.
- Sidebar sections can be reordered, collapsed and hidden.

## Windows integration

- Set Luling as the default file manager so folders, `Win+E` and "This PC" on the desktop open it; "Open file location" then selects the file in Luling. It can be turned off again, restoring the original setup.
- The context menu merges Explorer's and Windows 11's modern entries (VS Code, Windows Terminal, NanaZip and others); Luling's own entries can be hidden.
- Start minimized to the tray, choose a default location, and list the processes holding a file that cannot be deleted, with an option to end them and retry.
- "This PC" shows drive types and capacity bars.

## Automatic updates

Luling checks for a new version after it starts and shows a prompt in the window; clicking it downloads the installer and opens the setup wizard once verification passes. Settings → About & diagnostics can check manually or turn the check off. Downloads can be cancelled, and installation asks for the usual Windows administrator confirmation.

The update manifest and installer try the ghproxy.net and gh-proxy.com mirrors first, switch automatically on failure and fall back to GitHub — no configuration needed. Every source must pass signature and installer verification.

## Build from source

Windows x64, the Visual Studio 2022 C++ tools (v143), CMake 3.25+ and Ninja. Release builds additionally need PowerShell 7.2+ and Python 3 (to pack the installer and sign the update manifest).

```powershell
# Configure and build (CMakePresets.json also offers x64-debug / x64-release)
.\build_release.bat

# Run what you built
.\build\luling.exe

# Regression tests (registered by cmake/tests.cmake; scripts/build_release_ci.ps1 is the equivalent release gate)
pwsh ./scripts/run_tests.ps1
```

The stack is C++20, Win32, Direct2D and DirectComposition; filesystem, index, preview and Shell work live in their own modules so nothing blocks the UI. Release scripts verify the pinned LumaText SDK (`cmake/lumatext-sdk.json` pins the manifest SHA-256) and fall back to DirectWrite when it is missing.

More: [Updates and releases](docs/automatic-updates.md) · [Windows compatibility](docs/windows-compatibility.md) · [Index storage and migration](docs/index-migration.md)

## License and credits

This repository is **source-available proprietary software**, not an open-source project.

- Code, assets and documentation original to this project: copyright 云舒眠眠, under [LICENSE-luling.md](LICENSE-luling.md) (personal, non-commercial use; commercial use, redistribution, relicensing and removal of attribution are not permitted).
- This repository contains code derived from [Pulse](https://github.com/jimmgreen/pulse) v1.0.53, licensed under the [Apache License 2.0](LICENSE). Origin, modification notice and the third-party component list are in [NOTICE](NOTICE).
- Components under `third_party/` (SQLite, md4c, ib-pinyin-cpp, the LZMA SDK, PDFium, the LumaText SDK) are covered by their own bundled licenses, listed in NOTICE.
- Preview packs are not distributed with this repository; the app downloads them on demand, and their names and licence texts are shown in Settings › Quick Look › Supported formats.
