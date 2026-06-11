!include "MUI2.nsh"
!include "FileFunc.nsh"

Unicode true
SetCompressor /SOLID lzma

!define PRODUCT_NAME "nanobot-manager"
!define PRODUCT_VERSION "1.0.0"
!define PRODUCT_PUBLISHER "nanobot"
!define PRODUCT_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}"

Name "${PRODUCT_NAME} ${PRODUCT_VERSION}"
OutFile "..\dist\nanobot-manager-setup.exe"
InstallDir "$PROGRAMFILES64\${PRODUCT_NAME}"
RequestExecutionLevel admin

!define MUI_ICON "..\assets\app.ico"
!define MUI_UNICON "..\assets\app.ico"

!insertmacro MUI_PAGE_LICENSE "..\..\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES

!define MUI_FINISHPAGE_RUN "$INSTDIR\nanobot-manager.exe"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_RESERVEFILE_LANGDLL

Function .onInit
    !insertmacro MUI_LANGDLL_DISPLAY
FunctionEnd

Section "MainSection" SEC01
    SetOutPath "$INSTDIR"
    SetOverwrite on

    File "..\build\Release\nanobot-manager.exe"
    File "..\build\Release\core-ui.dll"
    File "..\assets\app.ico"

    SetOutPath "$INSTDIR\ui"
    File /nonfatal "..\ui\*.*"

    CreateDirectory "$SMPROGRAMS\${PRODUCT_NAME}"
    CreateShortCut "$SMPROGRAMS\${PRODUCT_NAME}\${PRODUCT_NAME}.lnk" "$INSTDIR\nanobot-manager.exe"
    CreateShortCut "$SMPROGRAMS\${PRODUCT_NAME}\Uninstall ${PRODUCT_NAME}.lnk" "$INSTDIR\uninstall.exe"

    CreateShortCut "$DESKTOP\${PRODUCT_NAME}.lnk" "$INSTDIR\nanobot-manager.exe"
SectionEnd

Section -Post
    WriteUninstaller "$INSTDIR\uninstall.exe"

    WriteRegStr SHCTX "${PRODUCT_UNINST_KEY}" "DisplayName" "${PRODUCT_NAME}"
    WriteRegStr SHCTX "${PRODUCT_UNINST_KEY}" "UninstallString" "$\"$INSTDIR\uninstall.exe$\""
    WriteRegStr SHCTX "${PRODUCT_UNINST_KEY}" "DisplayIcon" "$INSTDIR\app.ico"
    WriteRegStr SHCTX "${PRODUCT_UNINST_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
    WriteRegStr SHCTX "${PRODUCT_UNINST_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"

    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    IntFmt $0 "0x%08X" $0
    WriteRegDWORD SHCTX "${PRODUCT_UNINST_KEY}" "EstimatedSize" "$0"
SectionEnd

Section Uninstall
    Delete "$INSTDIR\nanobot-manager.exe"
    Delete "$INSTDIR\core-ui.dll"
    Delete "$INSTDIR\app.ico"
    Delete "$INSTDIR\uninstall.exe"
    RMDir /r "$INSTDIR\ui"
    RMDir "$INSTDIR"

    Delete "$SMPROGRAMS\${PRODUCT_NAME}\${PRODUCT_NAME}.lnk"
    Delete "$SMPROGRAMS\${PRODUCT_NAME}\Uninstall ${PRODUCT_NAME}.lnk"
    RMDir "$SMPROGRAMS\${PRODUCT_NAME}"
    Delete "$DESKTOP\${PRODUCT_NAME}.lnk"

    DeleteRegKey SHCTX "${PRODUCT_UNINST_KEY}"
SectionEnd
