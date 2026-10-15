; Luling installer, built with NSIS (makensis).
;
; Replaces the in-tree self-extracting bootstrapper: that one had to unpack its
; payload into the destination *before* it could elevate, so the extraction was
; refused on any machine whose install tree is Administrators-only.
;
; Build: makensis /DVERSION=0.1.11 /DBUILDDIR=build /DOUTDIR=dist scripts\installer_luling.nsi

Unicode true
SetCompressor /SOLID lzma

!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!ifndef BUILDDIR
  !define BUILDDIR "build"
!endif
!ifndef OUTDIR
  !define OUTDIR "dist"
!endif
; Repository root: LICENSE / NOTICE live there, not in the build tree.
!ifndef ROOT
  !define ROOT "."
!endif

Name "鹿铃"
Caption "鹿铃 ${VERSION} 安装程序"
OutFile "${OUTDIR}\LulingSetup-${VERSION}.exe"
InstallDir "$LOCALAPPDATA\Programs\Luling"
InstallDirRegKey HKCU "Software\Luling" "InstallLocation"

; Elevate up front: the shortcut and uninstall entries are per-user, but the
; file copy and the optional service registration are not.
RequestExecutionLevel admin
ShowInstDetails show
ShowUninstDetails show

!include "MUI2.nsh"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

Section "鹿铃" SecMain
  SectionIn RO
  ; ---------------------------------------------------------------------------
  ; Clear the previous version out before writing anything, so an update can
  ; never leave two builds mixed in one directory.
  ; ---------------------------------------------------------------------------

  ; A running copy holds its own exe and its helper processes open: without this
  ; the File writes fail and half the old build stays behind. The installer runs
  ; elevated, so the kills reach every user's processes.
  ExecWait 'taskkill /f /im luling.exe' $0
  ExecWait 'taskkill /f /im "Luling.Index.exe"' $0
  ExecWait 'taskkill /f /im "Luling.Preview.exe"' $0
  ExecWait 'taskkill /f /im "Luling.Document.exe"' $0
  ExecWait 'taskkill /f /im luling_shell.exe' $0
  ExecWait 'taskkill /f /im luling_integration.exe' $0

  ; The file-manager takeover is deliberately left alone. This update lands in the
  ; same $INSTDIR, so its keys keep pointing at a live luling.exe, and the snapshot
  ; under Software\Luling\ShellIntegration is the only thing that makes the
  ; takeover reversible — removing it here would strand Explorer with no way back.
  ; (The uninstaller restores it, before it removes anything.)

  ; Did the user have the full-disk index service on? It goes with everything else
  ; below and is put back at the end, so an update does not silently drop a feature
  ; they turned on.
  ExecWait 'sc query LulingIndex' $0
  StrCmp $0 "0" 0 luling_no_service
    StrCpy $2 "1"
    Goto luling_service_known
  luling_no_service:
    StrCpy $2 "0"
  luling_service_known:
  ; Stop it first, or its index files stay locked while the tree is removed.
  ExecWait 'sc stop LulingIndex' $0
  ExecWait 'sc delete LulingIndex' $0

  ; The user's own state is every top-level .json in the data directory: settings,
  ; pinned places, tags, custom menu rows, address and search history, saved
  ; layouts. Everything else in there — indexes, caches, logs, whatever a crash
  ; left behind — is rebuilt from scratch, and a stale one is exactly what a clean
  ; install is meant to avoid. Walking the wildcard beats a hardcoded list that
  ; would quietly start discarding files as the app grows.
  RMDir /r "$TEMP\LulingKeep"
  CreateDirectory "$TEMP\LulingKeep"
  FindFirst $R0 $R1 "$LOCALAPPDATA\Luling\*.json"
  luling_keep_loop:
    StrCmp $R1 "" luling_keep_done
    CopyFiles /SILENT "$LOCALAPPDATA\Luling\$R1" "$TEMP\LulingKeep\"
    FindNext $R0 $R1
    Goto luling_keep_loop
  luling_keep_done:
  FindClose $R0

  ; The whole tree the previous version owned: program files, data directories,
  ; leftovers under ProgramData, shortcuts and registration.
  RMDir /r "$INSTDIR"
  RMDir /r "$LOCALAPPDATA\Luling"
  RMDir /r "$LOCALAPPDATA\鹿铃"
  RMDir /r "$APPDATA\Luling"
  RMDir /r "$APPDATA\鹿铃"
  ReadEnvStr $1 "ProgramData"
  StrCmp $1 "" luling_no_programdata
    RMDir /r "$1\Luling"
    RMDir /r "$1\鹿铃"
  luling_no_programdata:

  Delete "$DESKTOP\鹿铃.lnk"
  Delete "$SMPROGRAMS\鹿铃\鹿铃.lnk"
  RMDir "$SMPROGRAMS\鹿铃"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Luling"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Luling"
  ; Rewritten at the end of this section; the shell-integration snapshot inside
  ; the same key is what has to survive, so only this value is cleared.
  DeleteRegValue HKCU "Software\Luling" "InstallLocation"
  ; The tag submenu and its icons. The takeover verbs are not touched: only
  ; --restore knows which of their values were Luling's.
  DeleteRegKey HKCU "Software\Classes\*\shell\Luling"
  DeleteRegKey HKLM "Software\Classes\*\shell\Luling"
  DeleteRegKey HKCU "Software\Classes\*\shell\LulingTags"
  DeleteRegKey HKLM "Software\Classes\*\shell\LulingTags"

  ; The user's state goes back before the new build's files land.
  CreateDirectory "$LOCALAPPDATA\Luling"
  CopyFiles /SILENT "$TEMP\LulingKeep\*.*" "$LOCALAPPDATA\Luling\"
  RMDir /r "$TEMP\LulingKeep"

  SetOutPath "$INSTDIR"
  File "${BUILDDIR}\luling.exe"
  File "${BUILDDIR}\Luling.Index.exe"
  File "${BUILDDIR}\Luling.Preview.exe"
  File "${BUILDDIR}\Luling.Document.exe"
  File "${BUILDDIR}\luling_shell.exe"
  File "${BUILDDIR}\luling_elevated.exe"
  File "${BUILDDIR}\luling_integration.exe"
  File "${BUILDDIR}\lumatext.dll"
  File "${BUILDDIR}\pdfium.dll"
  File "${ROOT}\LICENSE"
  File "${ROOT}\LICENSE-luling.md"
  File "${ROOT}\NOTICE"

  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateDirectory "$SMPROGRAMS\鹿铃"
  CreateShortCut "$SMPROGRAMS\鹿铃\鹿铃.lnk" "$INSTDIR\luling.exe"
  CreateShortCut "$DESKTOP\鹿铃.lnk" "$INSTDIR\luling.exe"

  WriteRegStr HKCU "Software\Luling" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Luling" "DisplayName" "鹿铃"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Luling" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Luling" "Publisher" "云舒眠眠"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Luling" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Luling" "DisplayIcon" "$INSTDIR\luling.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Luling" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Luling" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Luling" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Luling" "NoRepair" 1

  ; Put the index service back, now that its binary is on disk again. The app's
  ; own installer path is used rather than a raw `sc create`, so the service keeps
  ; one definition instead of two that can drift apart.
  StrCmp $2 "1" 0 luling_service_restored
    ExecWait '"$INSTDIR\Luling.Index.exe" --install' $0
  luling_service_restored:
SectionEnd

Section "Uninstall"
  ; Never ExecWait the application's own exe here: without a matching argument it
  ; starts the full UI and the uninstaller blocks on it forever. Kill leftovers
  ; and stop the service instead -- both return immediately.
  ExecWait 'taskkill /f /im luling.exe' $0
  ExecWait 'taskkill /f /im "Luling.Index.exe"' $0
  ExecWait 'taskkill /f /im "Luling.Preview.exe"' $0
  ExecWait 'taskkill /f /im "Luling.Document.exe"' $0
  ExecWait 'taskkill /f /im luling_shell.exe' $0
  ExecWait 'sc stop LulingIndex' $0
  ExecWait 'sc delete LulingIndex' $0

  ; Give the file manager back before anything is deleted. The takeover keys
  ; point at $INSTDIR\luling.exe, so removing the files first leaves Explorer
  ; unable to open a folder at all. The snapshot that makes this reversible
  ; lives in Software\Luling, so it has to outlive this call.
  IfFileExists "$INSTDIR\luling_integration.exe" 0 luling_restore_done
    ExecWait '"$INSTDIR\luling_integration.exe" --restore --exe "$INSTDIR\luling.exe"' $0
    StrCmp $0 "0" luling_restore_done 0
    DetailPrint "Shell integration restore returned $0; check the default file manager setting."
  luling_restore_done:

  Delete "$DESKTOP\鹿铃.lnk"
  Delete "$SMPROGRAMS\鹿铃\鹿铃.lnk"
  RMDir "$SMPROGRAMS\鹿铃"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Luling"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Luling"
  DeleteRegKey HKCU "Software\Luling"
  DeleteRegKey HKLM "Software\Luling"
  ; Shell integration entries the app may have written. The takeover verbs
  ; (Directory / Drive / the two CLSIDs) are not listed here on purpose: only
  ; --restore above knows which of their values were ours, and deleting the
  ; subtrees outright would throw away a setting the snapshot had put back.
  DeleteRegKey HKCU "Software\Classes\*\shell\Luling"
  DeleteRegKey HKLM "Software\Classes\*\shell\Luling"
  DeleteRegKey HKCU "Software\Classes\*\shell\LulingTags"
  DeleteRegKey HKLM "Software\Classes\*\shell\LulingTags"

  ; The index service released handles for its store only seconds ago; the
  ; 400 MB of index files stay locked if the directories go immediately.
  Sleep 1500
  ; Program files, then everything the app keeps outside them. Leaving these
  ; behind is what makes an "uninstalled" Luling still show up in searches and
  ; in %LOCALAPPDATA%.
  RMDir /r "$INSTDIR"
  RMDir /r "$LOCALAPPDATA\Luling"
  RMDir /r "$LOCALAPPDATA\鹿铃"
  RMDir /r "$APPDATA\Luling"
  RMDir /r "$APPDATA\鹿铃"
  ; $COMMONAPPDATA is not an NSIS constant. NSIS silently ignored it, so the
  ; whole %ProgramData% tree -- where the index service keeps its store -- and
  ; with it hundreds of MB survived every uninstall.
  ReadEnvStr $1 "ProgramData"
  StrCmp $1 "" luling_no_programdata
    RMDir /r "$1\Luling"
    RMDir /r "$1\鹿铃"
  luling_no_programdata:
SectionEnd
