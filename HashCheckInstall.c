/**
 * HashCheck uninstall registration
 * Copyright (C) 2026 Mounir IDRASSI. All rights reserved.
 * Please refer to license.txt for details about distribution and modification.
 **/

#include "globals.h"
#include "HashCheckInstall.h"
#include "RegHelpers.h"
#include "libs/Wow64.h"
#include <strsafe.h>

static const TCHAR szUninstallKey[] =
    TEXT("Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\") UNINSTALL_KEY_STR_HashCheck;
static const TCHAR szLegacyUninstallKey[] =
    TEXT("Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\HashCheck Shell Extension");

BOOL WINAPI RegisterUninstallEntry( LPCTSTR lpszDllPath )
{
    TCHAR szUninstall[MAX_PATH << 1];
    TCHAR szQuietUninstall[MAX_PATH << 1];
    HKEY hKey;
    DWORD dwDisposition;
    BOOL bSuccess;

    if (FAILED(StringCchPrintf(szUninstall, countof(szUninstall),
            TEXT("regsvr32.exe /u /i /n /s \"%s\""), lpszDllPath)) ||
        FAILED(StringCchPrintf(szQuietUninstall, countof(szQuietUninstall),
            TEXT("regsvr32.exe /u /i:\"NoRebootPrompt\" /n /s \"%s\""), lpszDllPath)))
        return(FALSE);

    // Use the calling DLL's registry view. On 64-bit Windows only the native
    // DLL registers an uninstaller; the companion DLL uses NoUninstall.
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, szUninstallKey, 0, NULL,
            REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &hKey, &dwDisposition) != ERROR_SUCCESS)
        return(FALSE);

    Wow64DisableRegReflection(hKey);
    bSuccess =
        RegSetSZ(hKey, TEXT("DisplayIcon"), lpszDllPath) &&
        RegSetSZ(hKey, TEXT("DisplayName"), TEXT(HASHCHECK_NAME_STR)) &&
        RegSetSZ(hKey, TEXT("DisplayVersion"), TEXT(HASHCHECK_VERSION_STR)) &&
        RegSetSZ(hKey, TEXT("Publisher"), PUBLISHER_STR_HashCheck) &&
        RegSetDW(hKey, TEXT("EstimatedSize"), 1073) &&
        RegSetSZ(hKey, TEXT("HelpLink"), TEXT("https://github.com/idrassi/HashCheck/issues")) &&
        RegSetDW(hKey, TEXT("NoModify"), 1) &&
        RegSetDW(hKey, TEXT("NoRepair"), 1) &&
        RegSetSZ(hKey, TEXT("UninstallString"), szUninstall) &&
        RegSetSZ(hKey, TEXT("QuietUninstallString"), szQuietUninstall) &&
        RegSetSZ(hKey, TEXT("URLInfoAbout"), TEXT("https://github.com/idrassi/HashCheck")) &&
        RegSetSZ(hKey, TEXT("URLUpdateInfo"), TEXT("https://github.com/idrassi/HashCheck/releases/latest"));
    RegCloseKey(hKey);

    if (!bSuccess)
    {
        if (dwDisposition == REG_CREATED_NEW_KEY)
            RegDelete(HKEY_LOCAL_MACHINE, szUninstallKey, NULL);
        return(FALSE);
    }

    // Installation has already replaced the shared shell registration. Remove
    // the superseded ARP identity only after its replacement has been written.
    return(RegDelete(HKEY_LOCAL_MACHINE, szLegacyUninstallKey, NULL));
}

BOOL WINAPI UnregisterUninstallEntry( VOID )
{
    // Also used by NoUninstall, so stale entries in the companion DLL's view
    // are removed without touching the native DLL's entry.
    BOOL bCurrentDeleted = RegDelete(HKEY_LOCAL_MACHINE, szUninstallKey, NULL);
    BOOL bLegacyDeleted = RegDelete(HKEY_LOCAL_MACHINE, szLegacyUninstallKey, NULL);
    return(bCurrentDeleted && bLegacyDeleted);
}
