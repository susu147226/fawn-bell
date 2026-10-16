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
; Set for the build that carries the preview packs; empty for the standard one.
!ifndef SUFFIX
  !define SUFFIX ""
!endif

Name "鹿铃"
Caption "鹿铃 ${VERSION} 安装程序"
OutFile "${OUTDIR}\LulingSetup-${VERSION}${SUFFIX}.exe"
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

  ; Did the user have the full-disk index service on? It goes with everything else
  ; below and is put back at the end, so an update does not silently drop a feature
  ; they turned on.
  ;
  ; Asked of the registry rather than `sc query`: the exit code of a spawned sc is
  ; not dependable from this context, and getting it wrong here silently turned the
  ; service off for everyone who had it. SYSTEM is not redirected for a 32-bit
  ; installer, so this reads the real key.
  ClearErrors
  ReadRegStr $2 HKLM "SYSTEM\CurrentControlSet\Services\LulingIndex" "ImagePath"
  IfErrors 0 luling_service_present
    StrCpy $2 "0"
    Goto luling_service_known
  luling_service_present:
    StrCpy $2 "1"
  luling_service_known:
  ; The service is stopped and deleted *before* any process is killed. Killing it
  ; first only makes the SCM start it again, and the restarted image holds
  ; Luling.Index.exe open once more by the time the files are written — which is
  ; the "cannot open the file to write" abort users hit when the index service
  ; was on. Deleting it first also means it cannot come back during the install.
  ExecWait 'sc stop LulingIndex' $0
  ExecWait 'sc delete LulingIndex' $0

  ; A running copy holds its own exe and its helper processes open: without this
  ; the File writes fail and half the old build stays behind. The installer runs
  ; elevated, so the kills reach every user's processes.
  ;
  ; luling.exe is killed on a silent run too, even though that run *is* the app
  ; updating itself and it also closes on its own: this process may already be
  ; running by the time it does, and a locked luling.exe makes every File write
  ; fail. With /S the failure is silent and the installer still exits 0, so the
  ; app reported a successful update that never replaced a single file -- the
  ; About page kept showing the old version. The launch at the end of this
  ; section brings the app back either way.
  ExecWait 'taskkill /f /im luling.exe' $0
  ExecWait 'taskkill /f /im "Luling.Index.exe"' $0
  ExecWait 'taskkill /f /im "Luling.Preview.exe"' $0
  ExecWait 'taskkill /f /im "Luling.Document.exe"' $0
  ExecWait 'taskkill /f /im luling_shell.exe' $0
  ExecWait 'taskkill /f /im luling_integration.exe' $0
  ; sc stop only signals, and an image takes a moment to unload: give the files
  ; a beat before anything is copied over them.
  Sleep 1200

  ; The file-manager takeover is deliberately left alone. This update lands in the
  ; same $INSTDIR, so its keys keep pointing at a live luling.exe, and the snapshot
  ; under Software\Luling\ShellIntegration is the only thing that makes the
  ; takeover reversible — removing it here would strand Explorer with no way back.
  ; (The uninstaller restores it, before it removes anything.)

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
  ;
  ; The downloaded preview packs are an exception. They are user content (100+ MB
  ; of downloads), not a cache: wiping them was what made the packs look like they
  ; had vanished after an update, and re-fetching them costs the user real time.
  ; The tree is moved aside first — a same-volume rename, so it is instant — and
  ; put back afterwards. The app re-associates through each pack's installed.json
  ; and never notices the install happened.
  RMDir /r "$LOCALAPPDATA\LulingPacksKeep"
  IfFileExists "$LOCALAPPDATA\Luling\packs\*.*" 0 luling_no_pack_keep
    Rename "$LOCALAPPDATA\Luling\packs" "$LOCALAPPDATA\LulingPacksKeep"
  luling_no_pack_keep:

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

  ; And so do the packs moved aside above.
  IfFileExists "$LOCALAPPDATA\LulingPacksKeep\*.*" 0 luling_no_pack_restore
    Rename "$LOCALAPPDATA\LulingPacksKeep" "$LOCALAPPDATA\Luling\packs"
  luling_no_pack_restore:

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

!ifdef PACKSDIR
  ; The preview packs ship as the very .lzms files a download would have fetched,
  ; so the app decompresses and verifies them from disk instead of the network.
  ; That is what makes this build work where the download does not. /r keeps the
  ; <packs>\<key>\ layout the app looks under.
  SetOutPath "$INSTDIR\packs"
  File /r "${PACKSDIR}\*.*"
  SetOutPath "$INSTDIR"
!endif

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

  ; A silent run is the app updating itself, so it is handed back now that its
  ; files are in place. explorer.exe starts it with the logged-in user's token:
  ; launching it directly from here would leave a file manager running elevated.
  IfSilent luling_relaunch
  Goto luling_no_relaunch
  luling_relaunch:
    Exec '"$WINDIR\explorer.exe" "$INSTDIR\luling.exe"'
  luling_no_relaunch:
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
