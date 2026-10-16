; Arora Windows installer — Qt6 port.
;
; The script no longer enumerates Qt DLLs: CI (and local builds) stage a
; complete runtime with windeployqt into deploy\ and this installer just
; packages that tree recursively. That keeps the payload correct for
; Qt6/QtWebEngine without a hand-maintained file list — whatever
; windeployqt resolved (Qt6*.dll, QtWebEngineProcess, plugins/,
; resources/, translations/) ships verbatim.
;
;   makensis /DDEPLOYDIR=deploy /DPRODUCT_VERSION=0.12 BuildProcess\windowsinstaller.nsi

SetCompressor /SOLID /FINAL lzma

!define PRODUCT_NAME "Arora"
!ifndef PRODUCT_VERSION
  !define PRODUCT_VERSION "0.12"
!endif
!ifndef DEPLOYDIR
  !define DEPLOYDIR "deploy"
!endif
!define PRODUCT_DIR_REGKEY "Software\Microsoft\Windows\CurrentVersion\App Paths\arora.exe"
!define PRODUCT_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}"

; Dual-mode install: MultiUser adds an "all users vs just me" page —
; all-users lands in %ProgramFiles% (elevates via runas only when that
; mode is picked), just-me lands in %LOCALAPPDATA%\Programs with no
; UAC prompt at all. SHCTX resolves to HKLM or HKCU per the choice,
; and the shell vars ($SMPROGRAMS/$DESKTOP/$PROGRAMFILES64) follow the
; same context.
!define MULTIUSER_EXECUTIONLEVEL Highest
!define MULTIUSER_MUI
!define MULTIUSER_INSTALLMODE_COMMANDLINE
!define MULTIUSER_INSTALLMODE_INSTDIR "Arora"

!include "MUI.nsh"
!include "MultiUser.nsh"
!define MUI_ABORTWARNING
; makensis cd's into the script's directory, so paths must be either
; script-relative or passed in absolute via /D. ICONFILE/OUTDIR/SRCDIR
; are overridable for callers compiling from elsewhere.
!ifndef ICONFILE
  !define ICONFILE "browser.ico"
!endif
!ifndef OUTDIR
  !define OUTDIR "."
!endif
!define MUI_ICON "${ICONFILE}"
!define MUI_UNICON "${ICONFILE}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MULTIUSER_PAGE_INSTALLMODE
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\arora.exe"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"
!insertmacro MULTIUSER_INSTALLMODE
!insertmacro MULTIUSER_UNINSTALLMODE
Name "${PRODUCT_NAME} ${PRODUCT_VERSION}"
OutFile "${OUTDIR}\${PRODUCT_NAME} ${PRODUCT_VERSION} Installer.exe"
InstallDirRegKey SHCTX "${PRODUCT_DIR_REGKEY}" ""
ShowInstDetails show
ShowUnInstDetails show
; RequestExecutionLevel is owned by MultiUser (it elevates only when
; the all-users mode is chosen).

Section "Main Components"
  SetOverwrite on

  SetOutPath "$INSTDIR"
  File /r "${DEPLOYDIR}\*.*"

  SetOutPath "$INSTDIR\locale"
  !ifdef SRCDIR
    File /nonfatal "${SRCDIR}\.qm\locale\*.qm"
  !endif

  ; MSVC runtime — windeployqt deliberately does NOT ship the VC++
  ; redistributable. The build machine passes its redist dir via
  ;   makensis /DVCREDIST="%VCToolsRedistDir%x64\Microsoft.VC*.CRT"
  ; (%VCToolsRedistDir% is set by the MSVC dev environment); if the
  ; define is absent the installer just skips the DLLs rather than
  ; failing — arora.exe will then need a system-installed redist.
  !ifdef VCREDIST
    SetOutPath "$INSTDIR"
    File /nonfatal "${VCREDIST}\vcruntime140.dll"
    File /nonfatal "${VCREDIST}\vcruntime140_1.dll"
    File /nonfatal "${VCREDIST}\msvcp140.dll"
    File /nonfatal "${VCREDIST}\msvcp140_1.dll"
    File /nonfatal "${VCREDIST}\msvcp140_2.dll"
    File /nonfatal "${VCREDIST}\msvcp140_atomic_wait.dll"
    File /nonfatal "${VCREDIST}\concrt140.dll"
    File /nonfatal "${VCREDIST}\vcamp140.dll"
  !endif
SectionEnd

Section Icons
  CreateShortCut "$SMPROGRAMS\Arora.lnk" "$INSTDIR\arora.exe"
  CreateShortCut "$DESKTOP\Arora.lnk" "$INSTDIR\arora.exe"
SectionEnd

Section Uninstaller
  WriteUninstaller "$INSTDIR\uninst.exe"
  WriteRegStr SHCTX "${PRODUCT_DIR_REGKEY}" "" "$INSTDIR\arora.exe"
  WriteRegStr SHCTX "${PRODUCT_UNINST_KEY}" "DisplayName" "$(^Name)"
  WriteRegStr SHCTX "${PRODUCT_UNINST_KEY}" "UninstallString" "$INSTDIR\uninst.exe"
  WriteRegStr SHCTX "${PRODUCT_UNINST_KEY}" "DisplayIcon" "$INSTDIR\arora.exe"
  WriteRegStr SHCTX "${PRODUCT_UNINST_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
SectionEnd

Section Uninstall
  Delete "$SMPROGRAMS\Arora.lnk"
  Delete "$DESKTOP\Arora.lnk"
  RMDir /r "$INSTDIR"
  DeleteRegKey SHCTX "${PRODUCT_UNINST_KEY}"
  DeleteRegKey SHCTX "${PRODUCT_DIR_REGKEY}"
SectionEnd

BrandingText "arora-browser.org"
