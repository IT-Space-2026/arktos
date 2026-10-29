; Arktos Windows installer — Qt6 port.
;
; The script no longer enumerates Qt DLLs: CI (and local builds) stage a
; complete runtime with windeployqt into deploy\ and this installer just
; packages that tree recursively. That keeps the payload correct for
; Qt6/QtWebEngine without a hand-maintained file list — whatever
; windeployqt resolved (Qt6*.dll, QtWebEngineProcess, plugins/,
; resources/, translations/) ships verbatim.
;
;   makensis /DDEPLOYDIR=deploy /DPRODUCT_VERSION=1.0 BuildProcess\windowsinstaller.nsi

SetCompressor /SOLID /FINAL lzma

!define PRODUCT_NAME "Arktos"
!ifndef PRODUCT_VERSION
  !define PRODUCT_VERSION "1.2"
!endif
!ifndef DEPLOYDIR
  !define DEPLOYDIR "deploy"
!endif
!define PRODUCT_DIR_REGKEY "Software\Microsoft\Windows\CurrentVersion\App Paths\arktos.exe"
!define PRODUCT_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}"
; CA12/W13: Default Programs registration — without the
; StartMenuInternet capabilities + RegisteredApplications entry,
; Windows' "default browser" setting can never list Arktos.
!define PRODUCT_CLIENTS_KEY "Software\Clients\StartMenuInternet\Arktos"
!define PRODUCT_REGAPPS_KEY "Software\RegisteredApplications"

; Dual-mode install: MultiUser adds an "all users vs just me" page —
; all-users lands in %ProgramFiles% (elevates via runas only when that
; mode is picked), just-me lands in %LOCALAPPDATA%\Programs with no
; UAC prompt at all. SHCTX resolves to HKLM or HKCU per the choice,
; and the shell vars ($SMPROGRAMS/$DESKTOP/$PROGRAMFILES64) follow the
; same context.
!define MULTIUSER_EXECUTIONLEVEL Highest
!define MULTIUSER_MUI
!define MULTIUSER_INSTALLMODE_COMMANDLINE
!define MULTIUSER_INSTALLMODE_INSTDIR "Arktos"
!define MULTIUSER_USE_PROGRAMFILES64

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
!insertmacro MUI_PAGE_LICENSE "..\..\LICENSE.GPL2"
!insertmacro MULTIUSER_PAGE_INSTALLMODE
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\arktos.exe"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Function .onInit
  !insertmacro MULTIUSER_INIT
FunctionEnd

Function un.onInit
  !insertmacro MULTIUSER_UNINIT
FunctionEnd
Name "${PRODUCT_NAME} ${PRODUCT_VERSION}"
OutFile "${OUTDIR}\${PRODUCT_NAME} ${PRODUCT_VERSION} Installer.exe"
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
  ; failing — arktos.exe will then need a system-installed redist.
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
  !else
    ; CA12/W13: a bundle built without /DVCREDIST silently ships no
    ; CRT — arktos.exe then dies at launch on a clean machine with
    ; the useless "missing VCRUNTIME140.dll" dialog.  Fail makensis
    ; loudly instead.
    !warning "VCREDIST undefined — installer ships NO MSVC runtime; arktos.exe will fail on clean machines. Pass /DVCREDIST."
  !endif
SectionEnd

Section Icons
  CreateShortCut "$SMPROGRAMS\Arktos.lnk" "$INSTDIR\arktos.exe"
  CreateShortCut "$DESKTOP\Arktos.lnk" "$INSTDIR\arktos.exe"
SectionEnd

Section Uninstaller
  WriteUninstaller "$INSTDIR\uninst.exe"
  WriteRegStr SHCTX "${PRODUCT_DIR_REGKEY}" "" "$INSTDIR\arktos.exe"
  ; CA12/W13: App Paths carries the exe's install dir so shellexec of
  ; the bare name resolves sibling DLLs.
  WriteRegStr SHCTX "${PRODUCT_DIR_REGKEY}" "Path" "$INSTDIR"
  WriteRegStr SHCTX "${PRODUCT_UNINST_KEY}" "DisplayName" "$(^Name)"
  WriteRegStr SHCTX "${PRODUCT_UNINST_KEY}" "UninstallString" "$INSTDIR\uninst.exe"
  WriteRegStr SHCTX "${PRODUCT_UNINST_KEY}" "DisplayIcon" "$INSTDIR\arktos.exe"
  WriteRegStr SHCTX "${PRODUCT_UNINST_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"

  ; CA12/W13: Default Programs registration — the capabilities key is
  ; what StartMenuInternet candidates enumerate, and
  ; RegisteredApplications points Windows at it.
  WriteRegStr SHCTX "${PRODUCT_CLIENTS_KEY}" "" "Arktos"
  WriteRegStr SHCTX "${PRODUCT_CLIENTS_KEY}\Capabilities" \
      "ApplicationName" "Arktos"
  WriteRegStr SHCTX "${PRODUCT_CLIENTS_KEY}\Capabilities" \
      "ApplicationDescription" "Arktos web browser"
  WriteRegStr SHCTX "${PRODUCT_CLIENTS_KEY}\Capabilities" \
      "ApplicationIcon" "$INSTDIR\arktos.exe,0"
  WriteRegStr SHCTX "${PRODUCT_CLIENTS_KEY}\Capabilities\URLAssociations" \
      "http" "Arktos"
  WriteRegStr SHCTX "${PRODUCT_CLIENTS_KEY}\Capabilities\URLAssociations" \
      "https" "Arktos"
  WriteRegStr SHCTX "${PRODUCT_CLIENTS_KEY}\Capabilities\FileAssociations" \
      ".htm" "Arktos"
  WriteRegStr SHCTX "${PRODUCT_CLIENTS_KEY}\Capabilities\FileAssociations" \
      ".html" "Arktos"
  WriteRegStr SHCTX "${PRODUCT_CLIENTS_KEY}\shell\open\command" \
      "" '"$INSTDIR\arktos.exe"'
  WriteRegStr SHCTX "${PRODUCT_REGAPPS_KEY}" "Arktos" \
      "Software\Clients\StartMenuInternet\Arktos\Capabilities"

  ; CA12/W3: the sync LAN transport binds an inbound QUIC listener
  ; (UDP 14431) plus the mDNS responder (UDP 5353) — without an
  ; inbound allow rule, private-profile pairing silently times out
  ; whenever the consent prompt was dismissed.  Program-scoped so a
  ; reinstall updates it; private profile only (Public stays locked
  ; down, matching Windows' own default posture).  netsh needs
  ; elevation in all-users mode only — a just-me install where the
  ; call fails simply keeps the first-run consent prompt.
  nsExec::Exec 'netsh advfirewall firewall add rule name="Arktos LAN Sync" dir=in action=allow program="$INSTDIR\arktos.exe" enable=yes profile=private protocol=UDP localport=14431,5353'
SectionEnd

Section Uninstall
  Delete "$SMPROGRAMS\Arktos.lnk"
  Delete "$DESKTOP\Arktos.lnk"
  ; CA12/W3: retire the install-time firewall rule.
  nsExec::Exec 'netsh advfirewall firewall delete rule name="Arktos LAN Sync" program="$INSTDIR\arktos.exe"'
  RMDir /r "$INSTDIR"
  DeleteRegKey SHCTX "${PRODUCT_UNINST_KEY}"
  DeleteRegKey SHCTX "${PRODUCT_DIR_REGKEY}"
  DeleteRegKey SHCTX "${PRODUCT_CLIENTS_KEY}"
  DeleteRegValue SHCTX "${PRODUCT_REGAPPS_KEY}" "Arktos"
SectionEnd

BrandingText "arktos-browser.com"
