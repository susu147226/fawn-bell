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
  ; An upgrade writes over a running copy: without this the File writes fail
  ; ("error opening file for writing") and the old build stays behind, half
  ; replaced. Both the installer and the uninstaller run elevated, so the
  ; taskkill reaches every user's process.
  ExecWait 'taskkill /f /im luling.exe' $0
  ExecWait 'taskkill /f /im "Luling.Index.exe"' $0
  ExecWait 'taskkill /f /im "Luling.Preview.exe"' $0
  ExecWait 'taskkill /f /im "Luling.Document.exe"' $0
  ExecWait 'taskkill /f /im luling_shell.exe' $0
  ExecWait 'taskkill /f /im luling_integration.exe' $0
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
