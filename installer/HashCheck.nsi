SetCompressor /FINAL /SOLID lzma

!include MUI2.nsh
!include x64.nsh
!include WinVer.nsh
!include LogicLib.nsh
!include FileFunc.nsh

!insertmacro GetParameters
!insertmacro GetOptions

Unicode true

Name "HashCheck"
OutFile "HashCheckSetup-v2.6.2.0.exe"
ShowInstDetails show

RequestExecutionLevel admin
ManifestSupportedOS all

!define MUI_ICON ..\HashCheckInstaller.ico
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

VIProductVersion "2.6.2.0"
VIAddVersionKey /LANG=${LANG_ENGLISH} "ProductName" "HashCheck Shell Extension"
VIAddVersionKey /LANG=${LANG_ENGLISH} "ProductVersion" "2.6.2.0"
VIAddVersionKey /LANG=${LANG_ENGLISH} "Comments" "Installer distributed from https://github.com/idrassi/HashCheck/releases"
VIAddVersionKey /LANG=${LANG_ENGLISH} "LegalCopyright" "Copyright © 2008-2026 Kai Liu, Christopher Gurnee, Tim Schlueter, Mounir IDRASSI, et al. All rights reserved."
VIAddVersionKey /LANG=${LANG_ENGLISH} "FileDescription" "Installer (x86/x64) from https://github.com/idrassi/HashCheck/releases"
VIAddVersionKey /LANG=${LANG_ENGLISH} "FileVersion" "2.6.2.0"

; With solid compression, files that are required before the
; actual installation should be stored first in the data block,
; because this will make the installer start faster.
;
!insertmacro MUI_RESERVEFILE_LANGDLL

Var LogPath
Var LogHandle
Var LogMessage
Var LastStep
Var LastCode

!macro LogLine Text
    Push "${Text}"
    Call log_line
!macroend

!macro SetStep Text
    StrCpy $LastStep "${Text}"
    !insertmacro LogLine "${Text}"
!macroend

!macro AbortIfErrors Text
    IfErrors 0 +3
        StrCpy $LastStep "${Text}"
        Goto abort_on_error
!macroend

!macro AbortIfExitCode Text
    ${If} $LastCode != 0
        StrCpy $LastStep "${Text}"
        Goto abort_on_error
    ${EndIf}
!macroend

!macro FlagRebootIfFileExists Path
    IfFileExists "${Path}" 0 +4
        Delete /REBOOTOK "${Path}"
        IfFileExists "${Path}" 0 +2
            SetRebootFlag true
!macroend

Function init_log
    StrCpy $LogPath "$TEMP\HashCheckSetup.log"

    ${GetParameters} $R0
    ClearErrors
    ${GetOptions} $R0 "/LOG=" $R1
    ${IfNot} ${Errors}
        StrCpy $LogPath "$R1"
    ${EndIf}

    ClearErrors
    FileOpen $LogHandle "$LogPath" w
    IfErrors init_log_done
    FileWrite $LogHandle "HashCheck setup log$\r$\n"
    FileWrite $LogHandle "Command line: $CMDLINE$\r$\n"
    FileClose $LogHandle

init_log_done:
FunctionEnd

Function log_line
    Exch $LogMessage
    DetailPrint "$LogMessage"

    ClearErrors
    FileOpen $LogHandle "$LogPath" a
    IfErrors log_line_done
    FileSeek $LogHandle 0 END
    FileWrite $LogHandle "$LogMessage$\r$\n"
    FileClose $LogHandle

log_line_done:
    Pop $LogMessage
FunctionEnd

Function register_sparse_package
    !insertmacro SetStep "Registering Windows 11 sparse package"

    StrCpy $1 "$TEMP\HashCheckWin11Install.ps1"
    Delete "$1"

    ClearErrors
    FileOpen $LogHandle "$1" w
    IfErrors register_sparse_prepare_failed
    FileWrite $LogHandle "param([string]$$PackagePath, [string]$$ExternalLocation, [string]$$PackageName, [string]$$LogPath)$\r$\n"
    ; An installer launched from pwsh inherits module paths that Windows PowerShell cannot use.
    FileWrite $LogHandle "$$env:PSModulePath = $$PSHOME + '\Modules'$\r$\n"
    FileWrite $LogHandle "$$ErrorActionPreference = 'Stop'$\r$\n"
    FileWrite $LogHandle "function Log([string]$$Message) { Add-Content -LiteralPath $$LogPath -Encoding UTF8 -Value $$Message }$\r$\n"
    FileWrite $LogHandle "try {$\r$\n"
    FileWrite $LogHandle "    Log ('PowerShell version: ' + $$PSVersionTable.PSVersion)$\r$\n"
    FileWrite $LogHandle "    Log ('OS version: ' + [Environment]::OSVersion.Version)$\r$\n"
    FileWrite $LogHandle "    Log ('Package path: ' + $$PackagePath)$\r$\n"
    FileWrite $LogHandle "    Log ('External location: ' + $$ExternalLocation)$\r$\n"
    FileWrite $LogHandle "    if ([Environment]::OSVersion.Version -lt [Version]'10.0.22000.0') { Log 'Skipping sparse package registration on pre-Windows 11'; exit 0 }$\r$\n"
    FileWrite $LogHandle "    if (!(Test-Path -LiteralPath $$PackagePath -PathType Leaf)) { throw ('MSIX package not found: ' + $$PackagePath) }$\r$\n"
    FileWrite $LogHandle "    if (!(Test-Path -LiteralPath $$ExternalLocation -PathType Container)) { throw ('External location not found: ' + $$ExternalLocation) }$\r$\n"
    FileWrite $LogHandle "    $$signature = Get-AuthenticodeSignature -LiteralPath $$PackagePath$\r$\n"
    FileWrite $LogHandle "    Log ('MSIX signature status: ' + $$signature.Status + ' - ' + $$signature.StatusMessage)$\r$\n"
    FileWrite $LogHandle "    if ($$signature.Status -ne 'Valid') { throw ('MSIX signature is not valid: ' + $$signature.Status + ' - ' + $$signature.StatusMessage) }$\r$\n"
    FileWrite $LogHandle "    $$existing = Get-AppxPackage -Name $$PackageName -ErrorAction SilentlyContinue$\r$\n"
    FileWrite $LogHandle "    foreach ($$package in $$existing) { Log ('Removing existing package: ' + $$package.PackageFullName); Remove-AppxPackage -Package $$package.PackageFullName -ErrorAction Stop }$\r$\n"
    FileWrite $LogHandle "    Log 'Calling Add-AppxPackage with ExternalLocation'$\r$\n"
    FileWrite $LogHandle "    Add-AppxPackage -Path $$PackagePath -ExternalLocation $$ExternalLocation -ForceUpdateFromAnyVersion -ErrorAction Stop$\r$\n"
    FileWrite $LogHandle "    $$installed = Get-AppxPackage -Name $$PackageName -ErrorAction SilentlyContinue$\r$\n"
    FileWrite $LogHandle "    if (!$$installed) { throw ('Add-AppxPackage completed but Get-AppxPackage did not find ' + $$PackageName) }$\r$\n"
    FileWrite $LogHandle "    Log ('Installed package: ' + $$installed.PackageFullName)$\r$\n"
    FileWrite $LogHandle "    Log ('Installed package location: ' + $$installed.InstallLocation)$\r$\n"
    FileWrite $LogHandle "    try {$\r$\n"
    FileWrite $LogHandle "        Log 'Staging and provisioning sparse package for machine-wide install'$\r$\n"
    FileWrite $LogHandle "        Add-AppxPackage -Stage $$PackagePath -ExternalLocation $$ExternalLocation -ErrorAction Stop$\r$\n"
    FileWrite $LogHandle "        Add-AppxProvisionedPackage -Online -PackagePath $$PackagePath -SkipLicense -ErrorAction Stop | Out-Null$\r$\n"
    FileWrite $LogHandle "        Log 'Sparse package provisioning completed'$\r$\n"
    FileWrite $LogHandle "    } catch {$\r$\n"
    FileWrite $LogHandle "        Log ('Sparse package provisioning warning: ' + $$_.Exception.Message)$\r$\n"
    FileWrite $LogHandle "    }$\r$\n"
    FileWrite $LogHandle "    exit 0$\r$\n"
    FileWrite $LogHandle "} catch {$\r$\n"
    FileWrite $LogHandle "    Log ('ERROR installing Windows 11 sparse package: ' + $$_.Exception.Message)$\r$\n"
    FileWrite $LogHandle "    if ($$_.InvocationInfo) { Log ('At: ' + $$_.InvocationInfo.PositionMessage) }$\r$\n"
    FileWrite $LogHandle "    exit 1$\r$\n"
    FileWrite $LogHandle "}$\r$\n"
    FileClose $LogHandle

    ClearErrors
    ; nsExec starts console-subsystem tools hidden from creation time. PowerShell's
    ; own -WindowStyle Hidden is not early enough to prevent a brief console flash.
    nsExec::ExecToLog '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File "$1" -PackagePath "$PROGRAMFILES64\HashCheck\HashCheckWin11.msix" -ExternalLocation "$PROGRAMFILES64\HashCheck" -PackageName "IDRIX.HashCheck" -LogPath "$LogPath"'
    Pop $LastCode
    !insertmacro LogLine "Sparse package registration exit code: $LastCode"
    Delete "$1"
    Return

register_sparse_prepare_failed:
    StrCpy $LastCode 1
    SetErrors
    !insertmacro LogLine "ERROR: Unable to create temporary sparse package registration script"
    Return
FunctionEnd

; The install script
;
Section

    GetTempFileName $0
    !insertmacro LogLine "Log file: $LogPath"

    ${If} ${RunningX64}
        ${DisableX64FSRedirection}

        ; Install the 64-bit dll
        !insertmacro SetStep "Extracting 64-bit HashCheck.dll"
        ClearErrors
        File /oname=$0 ..\Bin\x64\Release\HashCheck.dll
        !insertmacro AbortIfErrors "Extracting 64-bit HashCheck.dll"

        !insertmacro SetStep "Registering 64-bit shell extension"
        ClearErrors
        ExecWait 'regsvr32 /i:"NoRebootPrompt" /n /s "$0"' $LastCode
        !insertmacro LogLine "64-bit regsvr32 exit code: $LastCode"
        !insertmacro AbortIfErrors "Launching 64-bit regsvr32"
        !insertmacro AbortIfExitCode "Registering 64-bit shell extension"
        Delete $0
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES64\HashCheck\HashCheck.dll.0"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES64\HashCheck\HashCheck.dll.1"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES64\HashCheck\HashCheck.dll.2"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES64\HashCheck\HashCheck.dll.3"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES64\HashCheck\HashCheck.dll.4"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES64\HashCheck\HashCheck.dll.5"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES64\HashCheck\HashCheck.dll.6"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES64\HashCheck\HashCheck.dll.7"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES64\HashCheck\HashCheck.dll.8"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES64\HashCheck\HashCheck.dll.9"

        ; Install the 64-bit TBB runtime beside the shell extension DLL.
        Delete /REBOOTOK "$SYSDIR\ShellExt\tbb12.dll"
        ClearErrors
        !insertmacro SetStep "Creating $PROGRAMFILES64\HashCheck"
        SetOutPath "$PROGRAMFILES64\HashCheck"
        !insertmacro AbortIfErrors "Creating $PROGRAMFILES64\HashCheck"

        !insertmacro SetStep "Extracting 64-bit TBB runtime"
        Delete "$PROGRAMFILES64\HashCheck\tbb12.dll.new"
        ClearErrors
        File /oname=tbb12.dll.new ..\Bin\x64\Release\tbb12.dll
        !insertmacro AbortIfErrors "Extracting 64-bit TBB runtime"

        !insertmacro SetStep "Installing 64-bit TBB runtime"
        Delete "$PROGRAMFILES64\HashCheck\tbb12.dll"
        ClearErrors
        Rename "$PROGRAMFILES64\HashCheck\tbb12.dll.new" "$PROGRAMFILES64\HashCheck\tbb12.dll"
        ${If} ${Errors}
            !insertmacro LogLine "Direct TBB runtime replacement failed; scheduling replacement on reboot"
            Delete /REBOOTOK "$PROGRAMFILES64\HashCheck\tbb12.dll"
            ClearErrors
            Rename /REBOOTOK "$PROGRAMFILES64\HashCheck\tbb12.dll.new" "$PROGRAMFILES64\HashCheck\tbb12.dll"
            !insertmacro AbortIfErrors "Scheduling 64-bit TBB runtime replacement"
            SetRebootFlag true
        ${EndIf}

        !insertmacro SetStep "Extracting TBB runtime license"
        ClearErrors
        File /oname=tbb12-LICENSE.txt ..\libs\oneTBB\LICENSE.txt
        !insertmacro AbortIfErrors "Extracting TBB runtime license"

        ; Install the standalone launcher used for command-line checksum creation.
        ; On Windows 11, the sparse package also declares this executable as its
        ; Application/Executable, and the IExplorerCommand handler launches it so
        ; long-lived HashCheck UI runs outside the COM surrogate.
        !insertmacro SetStep "Extracting packaged app host"
        ClearErrors
        File ..\Bin\x64\Release\HashCheckPackageHost.exe
        !insertmacro AbortIfErrors "Extracting packaged app host"

        ${If} ${AtLeastWin11}
            ; Windows 11 uses the sparse package app identity, not
            ; IExplorerCommand::GetIcon, for the grouped flyout icon. The app
            ; visual resources must therefore exist in the package's external
            ; location beside HashCheckPackageHost.exe.
            !insertmacro SetStep "Extracting Windows 11 package visual assets"
            ClearErrors
            File /r HashCheckWin11Package\Assets
            !insertmacro AbortIfErrors "Extracting Windows 11 package visual assets"

            !insertmacro SetStep "Extracting Windows 11 package resource index"
            ClearErrors
            File /nonfatal HashCheckWin11Package\resources*.pri
            ${If} ${Errors}
                !insertmacro LogLine "WARNING: Windows 11 package resource index was not embedded; Win11 grouped menu icon may be missing"
                ClearErrors
            ${EndIf}

            !insertmacro SetStep "Extracting Windows 11 sparse package"
            ClearErrors
            File /nonfatal /oname=HashCheckWin11.msix ..\Bin\HashCheckWin11.msix
            ${If} ${Errors}
                !insertmacro LogLine "Windows 11 sparse package was not embedded in this installer"
            ${EndIf}
            ClearErrors

            ; Register the sparse identity package used by the Windows 11 context menu.
            IfFileExists "$PROGRAMFILES64\HashCheck\HashCheckWin11.msix" do_register_sparse_package skip_sparse_package
            do_register_sparse_package:
            Call register_sparse_package
            !insertmacro AbortIfErrors "Preparing Windows 11 sparse package registration"
            !insertmacro AbortIfExitCode "Registering Windows 11 sparse package"
            skip_sparse_package:
        ${Else}
            ; Downlevel Windows builds keep the legacy shell extension path.
            !insertmacro LogLine "Skipping Windows 11 sparse package on pre-Windows 11 OS"
        ${EndIf}

        ; Clean up the old System32 install location used by earlier releases.
        !insertmacro SetStep "Cleaning up old 64-bit install location"
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
        !insertmacro SetStep "Extracting 32-bit HashCheck.dll"
        ClearErrors
        File /oname=$0 ..\Bin\Win32\Release\HashCheck.dll
        !insertmacro AbortIfErrors "Extracting 32-bit HashCheck.dll"

        !insertmacro SetStep "Registering 32-bit shell extension"
        ClearErrors
        ExecWait 'regsvr32 /i:"NoUninstall NoRebootPrompt" /n /s "$0"' $LastCode
        !insertmacro LogLine "32-bit regsvr32 exit code: $LastCode"
        !insertmacro AbortIfErrors "Launching 32-bit regsvr32"
        !insertmacro AbortIfExitCode "Registering 32-bit shell extension"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES32\HashCheck\HashCheck.dll.0"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES32\HashCheck\HashCheck.dll.1"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES32\HashCheck\HashCheck.dll.2"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES32\HashCheck\HashCheck.dll.3"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES32\HashCheck\HashCheck.dll.4"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES32\HashCheck\HashCheck.dll.5"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES32\HashCheck\HashCheck.dll.6"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES32\HashCheck\HashCheck.dll.7"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES32\HashCheck\HashCheck.dll.8"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES32\HashCheck\HashCheck.dll.9"

        ; Clean up the old SysWOW64 install location used by earlier releases.
        !insertmacro SetStep "Cleaning up old 32-bit install location"
        Delete /REBOOTOK $SYSDIR\ShellExt\HashCheck.dll
    ${Else}
        ; Install the 32-bit dll
        !insertmacro SetStep "Extracting 32-bit HashCheck.dll"
        ClearErrors
        File /oname=$0 ..\Bin\Win32\Release\HashCheck.dll
        !insertmacro AbortIfErrors "Extracting 32-bit HashCheck.dll"

        !insertmacro SetStep "Registering 32-bit shell extension"
        ClearErrors
        ExecWait 'regsvr32 /i:"NoRebootPrompt" /n /s "$0"' $LastCode
        !insertmacro LogLine "32-bit regsvr32 exit code: $LastCode"
        !insertmacro AbortIfErrors "Launching 32-bit regsvr32"
        !insertmacro AbortIfExitCode "Registering 32-bit shell extension"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES\HashCheck\HashCheck.dll.0"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES\HashCheck\HashCheck.dll.1"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES\HashCheck\HashCheck.dll.2"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES\HashCheck\HashCheck.dll.3"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES\HashCheck\HashCheck.dll.4"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES\HashCheck\HashCheck.dll.5"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES\HashCheck\HashCheck.dll.6"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES\HashCheck\HashCheck.dll.7"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES\HashCheck\HashCheck.dll.8"
        !insertmacro FlagRebootIfFileExists "$PROGRAMFILES\HashCheck\HashCheck.dll.9"

        ; Clean up the old System32 install location used by earlier releases.
        !insertmacro SetStep "Cleaning up old 32-bit install location"
        Delete /REBOOTOK $SYSDIR\ShellExt\HashCheck.dll

        ; Install the launcher used for command-line checksum creation.
        !insertmacro SetStep "Creating $PROGRAMFILES\HashCheck"
        ClearErrors
        SetOutPath "$PROGRAMFILES\HashCheck"
        !insertmacro AbortIfErrors "Creating $PROGRAMFILES\HashCheck"

        !insertmacro SetStep "Extracting HashCheck launcher"
        ClearErrors
        File ..\Bin\Win32\Release\HashCheckPackageHost.exe
        !insertmacro AbortIfErrors "Extracting HashCheck launcher"
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

    IfRebootFlag 0 no_reboot_required
        !insertmacro LogLine "A reboot is required to complete installation"
        IfSilent no_reboot_required
        MessageBox MB_ICONINFORMATION|MB_OK "A reboot is required to complete installation and use the newly installed HashCheck components."

    no_reboot_required:
    Return

    abort_on_error:
        !insertmacro LogLine "ERROR: $LastStep"
        Delete $0
        IfSilent +2
        MessageBox MB_ICONSTOP|MB_OK "An unexpected error occurred during installation.$\r$\n$\r$\nFailed step: $LastStep$\r$\nLog file: $LogPath"
        Quit

SectionEnd

Function .onInit
    Call init_log
    !insertmacro MUI_LANGDLL_DISPLAY
FunctionEnd
