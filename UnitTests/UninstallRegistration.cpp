/**
 * Tests for HashCheck uninstall registration.
 * HKLM is redirected to a temporary HKCU key within this process only.
 **/

#include <windows.h>
#include <shlwapi.h>
#include <sddl.h>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
#include "../HashCheckInstall.h"
#include "../version.h"

static const wchar_t* const NewKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\idrassi.HashCheckShellExtension";
static const wchar_t* const LegacyKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\HashCheck Shell Extension";
static const wchar_t* const OtherKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Another application";
static const wchar_t* const DllPath = L"C:\\Program Files\\HashCheck\\HashCheck.dll";

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct RegistrySandbox
{
    HKEY Key = NULL;
    wchar_t Path[160];

    RegistrySandbox()
    {
        swprintf_s(Path, L"Software\\HashCheckTests\\UninstallRegistration-%lu-%llu",
            GetCurrentProcessId(), GetTickCount64());
        Require(RegCreateKeyExW(HKEY_CURRENT_USER, Path, 0, NULL, 0, KEY_ALL_ACCESS,
            NULL, &Key, NULL) == ERROR_SUCCESS, "Create registry sandbox");
        if (RegOverridePredefKey(HKEY_LOCAL_MACHINE, Key) != ERROR_SUCCESS)
        {
            RegCloseKey(Key);
            SHDeleteKeyW(HKEY_CURRENT_USER, Path);
            throw std::runtime_error("Redirect HKLM to registry sandbox");
        }
    }

    ~RegistrySandbox()
    {
        RegOverridePredefKey(HKEY_LOCAL_MACHINE, NULL);
        RegCloseKey(Key);
        SHDeleteKeyW(HKEY_CURRENT_USER, Path);
    }
};

static bool Exists(const wchar_t* path)
{
    HKEY key;
    LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &key);
    if (result == ERROR_SUCCESS) RegCloseKey(key);
    else Require(result == ERROR_FILE_NOT_FOUND, "Unexpected registry lookup failure");
    return result == ERROR_SUCCESS;
}

static void SeedEntry(const wchar_t* path, const wchar_t* version)
{
    HKEY key;
    Require(RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_ALL_ACCESS,
        NULL, &key, NULL) == ERROR_SUCCESS, "Create seeded uninstall entry");
    LONG result = RegSetValueExW(key, L"DisplayVersion", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(version), static_cast<DWORD>((wcslen(version) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    Require(result == ERROR_SUCCESS, "Set seeded version");
}

static std::wstring ReadString(const wchar_t* path, const wchar_t* name)
{
    wchar_t value[1024];
    DWORD size = sizeof(value);
    Require(RegGetValueW(HKEY_LOCAL_MACHINE, path, name, RRF_RT_REG_SZ,
        NULL, value, &size) == ERROR_SUCCESS, "Read uninstall string value");
    return value;
}

static void CheckRegistration(const wchar_t* dllPath)
{
    Require(Exists(NewKey), "Fork has its own uninstall identity");
    Require(!Exists(LegacyKey), "Legacy uninstall identity is absent");
    Require(ReadString(NewKey, L"DisplayName") == L"HashCheck Shell Extension", "Display name matches WinGet");
    Require(ReadString(NewKey, L"Publisher") == L"idrassi", "Publisher matches WinGet");
    Require(ReadString(NewKey, L"DisplayVersion") == TEXT(HASHCHECK_VERSION_STR), "Version matches the DLL");
    Require(ReadString(NewKey, L"DisplayIcon") == dllPath, "Icon points to installed DLL");
    Require(ReadString(NewKey, L"URLInfoAbout") == L"https://github.com/idrassi/HashCheck", "Project URL is complete");
    Require(ReadString(NewKey, L"HelpLink") == L"https://github.com/idrassi/HashCheck/issues", "Support URL");
    Require(ReadString(NewKey, L"URLUpdateInfo") == L"https://github.com/idrassi/HashCheck/releases/latest", "Update URL");
    Require(ReadString(NewKey, L"UninstallString") ==
        std::wstring(L"regsvr32.exe /u /i /n /s \"") + dllPath + L"\"", "Uninstaller quotes the DLL path");
    Require(ReadString(NewKey, L"QuietUninstallString") ==
        std::wstring(L"regsvr32.exe /u /i:\"NoRebootPrompt\" /n /s \"") + dllPath + L"\"", "Quiet uninstall suppresses reboot prompt");
}

static void TestCleanInstallAndRepair()
{
    RegistrySandbox sandbox;
    Require(RegisterUninstallEntry(DllPath), "Register clean installation");
    CheckRegistration(DllPath);
    const wchar_t* movedDll = L"C:\\Custom HashCheck\\HashCheck.dll";
    Require(RegisterUninstallEntry(movedDll), "Repair existing registration");
    CheckRegistration(movedDll);
}

static void TestLegacyMigration()
{
    for (const wchar_t* version : { L"2.4.0.55", L"2.5.0.1", L"2.6.2.0" })
    {
        RegistrySandbox sandbox;
        SeedEntry(LegacyKey, version);
        Require(RegisterUninstallEntry(DllPath), "Replace inherited uninstall registration");
        CheckRegistration(DllPath);
    }
}

static void TestCleanup()
{
    RegistrySandbox sandbox;
    SeedEntry(OtherKey, L"1.0");
    Require(UnregisterUninstallEntry(), "Cleanup tolerates absent entries");
    SeedEntry(LegacyKey, L"2.5.0.1");
    Require(UnregisterUninstallEntry(), "NoUninstall removes legacy registration");
    Require(!Exists(LegacyKey) && !Exists(NewKey), "NoUninstall creates no entry");
    Require(RegisterUninstallEntry(DllPath), "Register before uninstall");
    SeedEntry(LegacyKey, L"2.5.0.1");
    Require(UnregisterUninstallEntry(), "Remove current and stale legacy registrations");
    Require(!Exists(LegacyKey) && !Exists(NewKey), "Uninstall leaves no registration");
    Require(UnregisterUninstallEntry(), "Repeated cleanup succeeds");
    Require(ReadString(OtherKey, L"DisplayVersion") == L"1.0", "Other applications are untouched");
}

static void TestInvalidPathPreservesLegacyEntry()
{
    RegistrySandbox sandbox;
    SeedEntry(LegacyKey, L"2.5.0.1");
    std::wstring longPath(1024, L'x');
    Require(!RegisterUninstallEntry(longPath.c_str()), "Reject truncated uninstall commands");
    Require(ReadString(LegacyKey, L"DisplayVersion") == L"2.5.0.1", "Failure retains legacy registration");
    Require(!Exists(NewKey), "Failure creates no partial registration");
}

static void TestAccessDeniedPreservesEntries()
{
    RegistrySandbox sandbox;
    SeedEntry(LegacyKey, L"2.5.0.1");
    SeedEntry(NewKey, L"previous registration");

    HKEY key;
    Require(RegOpenKeyExW(HKEY_LOCAL_MACHINE, NewKey, 0, KEY_ALL_ACCESS, &key) == ERROR_SUCCESS,
        "Open entry for permission test");
    DWORD size = 0;
    Require(RegGetKeySecurity(key, DACL_SECURITY_INFORMATION, NULL, &size) == ERROR_INSUFFICIENT_BUFFER,
        "Get original security descriptor size");
    std::vector<BYTE> original(size);
    Require(RegGetKeySecurity(key, DACL_SECURITY_INFORMATION, original.data(), &size) == ERROR_SUCCESS,
        "Save original permissions");
    PSECURITY_DESCRIPTOR readOnly = NULL;
    Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;KR;;;WD)",
        SDDL_REVISION_1, &readOnly, NULL), "Create read only permissions");
    LONG result = RegSetKeySecurity(key, DACL_SECURITY_INFORMATION, readOnly);
    LocalFree(readOnly);
    Require(result == ERROR_SUCCESS, "Apply read only permissions");

    BOOL registered = RegisterUninstallEntry(DllPath);
    result = RegSetKeySecurity(key, DACL_SECURITY_INFORMATION, original.data());
    RegCloseKey(key);
    Require(result == ERROR_SUCCESS, "Restore original permissions");
    Require(!registered, "Registration reports access denied");
    Require(ReadString(LegacyKey, L"DisplayVersion") == L"2.5.0.1", "Access denied retains legacy entry");
    Require(ReadString(NewKey, L"DisplayVersion") == L"previous registration", "Access denied retains current entry");
}

int main()
{
    try
    {
        TestCleanInstallAndRepair();
        TestLegacyMigration();
        TestCleanup();
        TestInvalidPathPreservesLegacyEntry();
        TestAccessDeniedPreservesEntries();
        puts("All uninstall registration tests passed.");
        return 0;
    }
    catch (const std::exception& error)
    {
        fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
