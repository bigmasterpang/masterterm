Unicode true

!include "MUI2.nsh"

!ifndef APP_VERSION
  !define APP_VERSION "0.1.36"
!endif
!ifndef SOURCE_DIR
  !error "SOURCE_DIR is required"
!endif
!ifndef OUTPUT_FILE
  !error "OUTPUT_FILE is required"
!endif
!ifndef ROOT_DIR
  !error "ROOT_DIR is required"
!endif

Name "MasterTerm"
Caption "MasterTerm ${APP_VERSION} Setup"
OutFile "${OUTPUT_FILE}"
InstallDir "$PROGRAMFILES64\MasterTerm"
InstallDirRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MasterTerm" "InstallLocation"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
SetDatablockOptimize on
CRCCheck on
ShowInstDetails show
ShowUninstDetails show
BrandingText "MasterTerm"

Icon "${ROOT_DIR}\resources\icons\MasterTerm.ico"
UninstallIcon "${ROOT_DIR}\resources\icons\MasterTerm.ico"

!define MUI_ABORTWARNING
!define MUI_ICON "${ROOT_DIR}\resources\icons\MasterTerm.ico"
!define MUI_UNICON "${ROOT_DIR}\resources\icons\MasterTerm.ico"
!define MUI_FINISHPAGE_RUN "$INSTDIR\MasterTerm.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Launch MasterTerm"
!define MUI_FINISHPAGE_LINK "Open the MasterTerm project page"
!define MUI_FINISHPAGE_LINK_LOCATION "https://gitee.com/bigmasterwang/masterterm"

VIProductVersion "${APP_VERSION}.0"
VIAddVersionKey "ProductName" "MasterTerm"
VIAddVersionKey "ProductVersion" "${APP_VERSION}"
VIAddVersionKey "CompanyName" "MasterTerm"
VIAddVersionKey "FileDescription" "MasterTerm desktop terminal workbench"
VIAddVersionKey "FileVersion" "${APP_VERSION}"
VIAddVersionKey "LegalCopyright" "Copyright (c) MasterTerm contributors"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "MasterTerm" SEC_MAIN
  SectionIn RO
  SetRegView 64
  SetShellVarContext all
  SetOutPath "$INSTDIR"
  File /r "${SOURCE_DIR}\*"

  CreateDirectory "$SMPROGRAMS\MasterTerm"
  CreateShortcut "$SMPROGRAMS\MasterTerm\MasterTerm.lnk" "$INSTDIR\MasterTerm.exe" "" "$INSTDIR\MasterTerm.exe" 0
  CreateShortcut "$SMPROGRAMS\MasterTerm\Uninstall.lnk" "$INSTDIR\Uninstall.exe"
  CreateShortcut "$DESKTOP\MasterTerm.lnk" "$INSTDIR\MasterTerm.exe" "" "$INSTDIR\MasterTerm.exe" 0

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MasterTerm" "DisplayName" "MasterTerm"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MasterTerm" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MasterTerm" "Publisher" "MasterTerm"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MasterTerm" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MasterTerm" "DisplayIcon" "$INSTDIR\MasterTerm.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MasterTerm" "URLInfoAbout" "https://gitee.com/bigmasterwang/masterterm"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MasterTerm" "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MasterTerm" "QuietUninstallString" "$\"$INSTDIR\Uninstall.exe$\" /S"
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MasterTerm" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MasterTerm" "NoRepair" 1
SectionEnd

Section "Uninstall"
  SetRegView 64
  SetShellVarContext all
  Delete "$DESKTOP\MasterTerm.lnk"
  Delete "$SMPROGRAMS\MasterTerm\MasterTerm.lnk"
  Delete "$SMPROGRAMS\MasterTerm\Uninstall.lnk"
  RMDir "$SMPROGRAMS\MasterTerm"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MasterTerm"
  RMDir /r "$INSTDIR"
SectionEnd
