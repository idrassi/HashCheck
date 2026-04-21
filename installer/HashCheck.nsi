SetCompressor /FINAL /SOLID lzma

!include MUI2.nsh
!include x64.nsh

Unicode true

Name "HashCheck"
OutFile "HashCheckSetup-v2.6.0.0.exe"

RequestExecutionLevel admin
ManifestSupportedOS all

!define MUI_ICON ..\HashCheck.ico
!define MUI_ABORTWARNING

; Some stripped NSIS layouts omit Contrib\UIs\modern.exe. Prefer the normal
; Modern UI resource when it exists, but fall back to the Unicode LZMA stub's
; built-in dialogs so makensis can still build the installer.
!ifndef MUI_UI
!if /FileExists "${NSISDIR}\Contrib\UIs\modern.exe"
!define MUI_UI "${NSISDIR}\Contrib\UIs\modern.exe"
!else
!if /FileExists "${NSISDIR}\Stubs\lzma_solid-x86-unicode"
!warning "NSIS Modern UI resource modern.exe not found; using Unicode LZMA stub UI resources."
!define MUI_UI "${NSISDIR}\Stubs\lzma_solid-x86-unicode"
!else
!error "NSIS Modern UI resource modern.exe is missing and no Unicode LZMA stub fallback was found."
!endif
!endif
!endif

!insertmacro MUI_PAGE_WELCOME
!define MUI_PAGE_CUSTOMFUNCTION_SHOW change_license_font
!insertmacro MUI_PAGE_LICENSE "..\license.txt"
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

; Change to a smaller monospace font to more nicely display the license
;
Function change_license_font
    FindWindow $0 "#32770" "" $HWNDPARENT
    CreateFont $1 "Lucida Console" "7"
    GetDlgItem $0 $0 1000
    SendMessage $0 ${WM_SETFONT} $1 1
FunctionEnd

; These are the same languages supported by HashCheck
;
!insertmacro MUI_LANGUAGE "English" ; the first language is the default
!insertmacro MUI_LANGUAGE "TradChinese"
!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "Czech"
!insertmacro MUI_LANGUAGE "German"
!insertmacro MUI_LANGUAGE "Greek"
!insertmacro MUI_LANGUAGE "SpanishInternational"
!insertmacro MUI_LANGUAGE "French"
!insertmacro MUI_LANGUAGE "Italian"
!insertmacro MUI_LANGUAGE "Japanese"
!insertmacro MUI_LANGUAGE "Korean"
!insertmacro MUI_LANGUAGE "Dutch"
!insertmacro MUI_LANGUAGE "Polish"
!insertmacro MUI_LANGUAGE "PortugueseBR"
!insertmacro MUI_LANGUAGE "Portuguese"
!insertmacro MUI_LANGUAGE "Romanian"
!insertmacro MUI_LANGUAGE "Russian"
!insertmacro MUI_LANGUAGE "Swedish"
!insertmacro MUI_LANGUAGE "Turkish"
!insertmacro MUI_LANGUAGE "Ukrainian"
!insertmacro MUI_LANGUAGE "Catalan"

VIProductVersion "2.6.0.0"
VIAddVersionKey /LANG=${LANG_ENGLISH} "ProductName" "HashCheck Shell Extension"
VIAddVersionKey /LANG=${LANG_ENGLISH} "ProductVersion" "2.6.0.0"
VIAddVersionKey /LANG=${LANG_ENGLISH} "Comments" "Installer distributed from https://github.com/idrassi/HashCheck/releases"
VIAddVersionKey /LANG=${LANG_ENGLISH} "LegalCopyright" "Copyright © 2008-2021 Kai Liu, Christopher Gurnee, Tim Schlueter, Mounir IDRASSI, et al. All rights reserved."
VIAddVersionKey /LANG=${LANG_ENGLISH} "FileDescription" "Installer (x86/x64) from https://github.com/idrassi/HashCheck/releases"
VIAddVersionKey /LANG=${LANG_ENGLISH} "FileVersion" "2.6.0.0"

; With solid compression, files that are required before the
; actual installation should be stored first in the data block,
; because this will make the installer start faster.
;
!insertmacro MUI_RESERVEFILE_LANGDLL

; The install script
;
Section

    GetTempFileName $0

    ${If} ${RunningX64}
        ${DisableX64FSRedirection}

        ; Install the 64-bit dll
        File /oname=$0 ..\Bin\x64\Release\HashCheck.dll
        ExecWait 'regsvr32 /i /n /s "$0"'
        IfErrors abort_on_error
        Delete $0

        ; Install the 64-bit TBB runtime beside the shell extension DLL.
        Delete /REBOOTOK "$SYSDIR\ShellExt\tbb12.dll"
        ClearErrors
        SetOutPath "$PROGRAMFILES64\HashCheck"
        File /oname=tbb12.dll ..\Bin\x64\Release\tbb12.dll
        IfErrors abort_on_error
        File /oname=tbb12-LICENSE.txt ..\libs\oneTBB\LICENSE.txt
        IfErrors abort_on_error

        ; Clean up the old System32 install location used by earlier releases.
        Delete /REBOOTOK $SYSDIR\ShellExt\HashCheck.dll

        ; One of these 64-bit dlls exists and is undeletable if and
        ; only if it was in use and therefore a reboot is now required
        Delete /REBOOTOK $SYSDIR\ShellExt\HashCheck.dll.0
        Delete /REBOOTOK $SYSDIR\ShellExt\HashCheck.dll.1
        Delete /REBOOTOK $SYSDIR\ShellExt\HashCheck.dll.2
        Delete /REBOOTOK $SYSDIR\ShellExt\HashCheck.dll.3
        Delete /REBOOTOK $SYSDIR\ShellExt\HashCheck.dll.4
        Delete /REBOOTOK $SYSDIR\ShellExt\HashCheck.dll.5
        Delete /REBOOTOK $SYSDIR\ShellExt\HashCheck.dll.6
        Delete /REBOOTOK $SYSDIR\ShellExt\HashCheck.dll.7
        Delete /REBOOTOK $SYSDIR\ShellExt\HashCheck.dll.8
        Delete /REBOOTOK $SYSDIR\ShellExt\HashCheck.dll.9

        ${EnableX64FSRedirection}

        ; Install the 32-bit dll (the 64-bit dll handles uninstallation for both)
        File /oname=$0 ..\Bin\Win32\Release\HashCheck.dll
        ExecWait 'regsvr32 /i:"NoUninstall" /n /s "$0"'
        IfErrors abort_on_error

        ; Clean up the old SysWOW64 install location used by earlier releases.
        Delete /REBOOTOK $SYSDIR\ShellExt\HashCheck.dll
    ${Else}
        ; Install the 32-bit dll
        File /oname=$0 ..\Bin\Win32\Release\HashCheck.dll
        ExecWait 'regsvr32 /i /n /s "$0"'
        IfErrors abort_on_error	

        ; Clean up the old System32 install location used by earlier releases.
        Delete /REBOOTOK $SYSDIR\ShellExt\HashCheck.dll
    ${EndIf}

    Delete $0

    ; One of these 32-bit dlls exists and is undeletable if and
    ; only if it was in use and therefore a reboot is now required
    Delete /REBOOTOK $SYSDIR\ShellExt\HashCheck.dll.0
    Delete /REBOOTOK $SYSDIR\ShellExt\HashCheck.dll.1
    Delete /REBOOTOK $SYSDIR\ShellExt\HashCheck.dll.2
    Delete /REBOOTOK $SYSDIR\ShellExt\HashCheck.dll.3
    Delete /REBOOTOK $SYSDIR\ShellExt\HashCheck.dll.4
    Delete /REBOOTOK $SYSDIR\ShellExt\HashCheck.dll.5
    Delete /REBOOTOK $SYSDIR\ShellExt\HashCheck.dll.6
    Delete /REBOOTOK $SYSDIR\ShellExt\HashCheck.dll.7
    Delete /REBOOTOK $SYSDIR\ShellExt\HashCheck.dll.8
    Delete /REBOOTOK $SYSDIR\ShellExt\HashCheck.dll.9

    Return

    abort_on_error:
        Delete $0
        IfSilent +2
        MessageBox MB_ICONSTOP|MB_OK "An unexpected error occurred during installation"
        Quit

SectionEnd

Function .onInit
    !insertmacro MUI_LANGDLL_DISPLAY
FunctionEnd
