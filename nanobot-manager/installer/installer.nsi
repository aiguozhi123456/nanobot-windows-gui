!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"

Unicode true
SetCompressor /SOLID lzma

!define PRODUCT_NAME "nanobot-manager"
!include "..\build\generated\version.nsh"
!define PRODUCT_PUBLISHER "nanobot"
!define PRODUCT_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}"
!define PRODUCT_AUTORUN_KEY "Software\Microsoft\Windows\CurrentVersion\Run"
!define PRODUCT_AUTORUN_NAME "nanobot-manager"

Name "${PRODUCT_NAME} ${PRODUCT_VERSION}"
OutFile "..\dist\nanobot-manager-setup.exe"
; Per-user install (matches nanobot's own install form): no UAC, no admin.
InstallDir "$LOCALAPPDATA\Programs\${PRODUCT_NAME}"
RequestExecutionLevel user

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

    ; Everything per-user: uninstall key (SHCTX -> HKCU), shortcuts, and by
    ; default the install location. No elevation anywhere in this installer.
    SetShellVarContext current

    StrCpy $R1 ""

    ReadRegStr $R0 HKLM "${PRODUCT_UNINST_KEY}" "DisplayVersion"
    ${If} $R0 == ""
        ReadRegStr $R0 HKCU "${PRODUCT_UNINST_KEY}" "DisplayVersion"
    ${EndIf}

    ${If} $R0 != ""
        ReadRegStr $R1 HKLM "${PRODUCT_UNINST_KEY}" "InstallLocation"
        ${If} $R1 == ""
            ReadRegStr $R1 HKCU "${PRODUCT_UNINST_KEY}" "InstallLocation"
        ${EndIf}

        ${If} $R1 == ""
            ReadRegStr $R2 HKLM "${PRODUCT_UNINST_KEY}" "UninstallString"
            ${If} $R2 == ""
                ReadRegStr $R2 HKCU "${PRODUCT_UNINST_KEY}" "UninstallString"
            ${EndIf}
            ${If} $R2 != ""
                StrCpy $R3 $R2 1
                ${If} $R3 == '"'
                    StrCpy $R2 $R2 "" 1
                    StrCpy $R2 $R2 -1
                ${EndIf}
                ${GetParent} $R2 $R1
            ${EndIf}
        ${EndIf}

        MessageBox MB_YESNO|MB_ICONINFORMATION \
            "Detected v$R0 installed. Upgrade to v${PRODUCT_VERSION}?" \
            IDYES upgrade
        Abort
    ${EndIf}
    Goto done

upgrade:
    nsExec::ExecToLog 'taskkill /IM "nanobot-manager.exe" /F'
    Sleep 500

    ${If} $R1 != ""
        ; Reuse the previous location only when it is writable without
        ; elevation (per-user locations). Legacy Program Files installs
        ; (admin-era setup) are not writable by this per-user installer.
        ClearErrors
        FileOpen $6 "$R1\~nmwrite.tmp" w
        ${IfNot} ${Errors}
            FileClose $6
            Delete "$R1\~nmwrite.tmp"
            StrCpy $INSTDIR $R1
            Goto done
        ${EndIf}
    ${EndIf}

    ; Legacy elevated install. This installer can neither replace nor remove
    ; it silently, and sharing the HKCU uninstall key with a half-removed
    ; install would orphan the old copy -- so the old uninstaller must run
    ; first (it elevates itself). Fresh installs then go to the per-user dir.
    ${If} ${FileExists} "$R1\uninstall.exe"
        MessageBox MB_YESNO|MB_ICONINFORMATION \
            "The previous installation (v$R0 at $R1) requires administrator rights to remove.$\r$\nRun its uninstaller now? Windows will ask for administrator confirmation; run this setup again afterwards." \
            IDYES run_legacy IDNO legacy_manual
        run_legacy:
            ExecShell "runas" "$R1\uninstall.exe"
            Abort
        legacy_manual:
            MessageBox MB_OK|MB_ICONINFORMATION \
                "Please uninstall the previous version first (Settings > Apps, or run $R1\uninstall.exe), then run this setup again."
            Abort
    ${Else}
        ; Stale registry entry with no uninstaller left behind: proceed with a
        ; fresh per-user install (the -Post section rewrites the key anyway).
        DetailPrint "Previous install at $R1 has no uninstaller; installing fresh to $INSTDIR"
    ${EndIf}

done:
FunctionEnd

Section "MainSection" SEC01
    SetOutPath "$INSTDIR"
    SetOverwrite on

    File "..\build\Release\nanobot-manager.exe"
    File "..\build\Release\core-ui.dll"
    File "..\assets\app.ico"

    SetOutPath "$INSTDIR\ui"
    File /nonfatal /r "..\ui\*.*"

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
    WriteRegStr SHCTX "${PRODUCT_UNINST_KEY}" "InstallLocation" "$INSTDIR"

    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    IntFmt $0 "0x%08X" $0
    WriteRegDWORD SHCTX "${PRODUCT_UNINST_KEY}" "EstimatedSize" "$0"
SectionEnd

Section Uninstall
    ; Uninstaller runs as its own process: re-declare the per-user context
    ; (SHCTX -> HKCU, per-user shortcuts) like .onInit does for the installer.
    SetShellVarContext current

    nsExec::ExecToLog 'taskkill /IM "nanobot-manager.exe" /F'
    Sleep 500

    DeleteRegValue HKCU "${PRODUCT_AUTORUN_KEY}" "${PRODUCT_AUTORUN_NAME}"

    ; Credential Manager records are scoped to a Windows user. This elevated
    ; uninstaller can only remove the credential visible in its current context.
    nsExec::ExecToLog '"$INSTDIR\nanobot-manager.exe" --delete-credential'
    Pop $0
    ${If} $0 != 0
        DetailPrint "Warning: bootstrap credential cleanup failed (exit $0). It can only remove the credential for the current executing Windows user."
    ${EndIf}

    Delete "$INSTDIR\nanobot-manager.exe"
    Delete "$INSTDIR\core-ui.dll"
    Delete "$INSTDIR\app.ico"
    Delete "$INSTDIR\uninstall.exe"
    Delete "$INSTDIR\config.json"
    Delete "$INSTDIR\nanobot-manager-autostart.log"
    RMDir /r "$INSTDIR\ui"
    RMDir "$INSTDIR"

    Delete "$SMPROGRAMS\${PRODUCT_NAME}\${PRODUCT_NAME}.lnk"
    Delete "$SMPROGRAMS\${PRODUCT_NAME}\Uninstall ${PRODUCT_NAME}.lnk"
    RMDir "$SMPROGRAMS\${PRODUCT_NAME}"
    Delete "$DESKTOP\${PRODUCT_NAME}.lnk"

    DeleteRegKey SHCTX "${PRODUCT_UNINST_KEY}"
SectionEnd
