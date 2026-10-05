# WinGet package identity

HashCheck registers its uninstall entry under `HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\idrassi.HashCheckShellExtension` in the native registry view. Its display name is `HashCheck Shell Extension` and its publisher is `idrassi`, matching the existing WinGet package `idrassi.HashCheckShellExtension`.

The former uninstall key, `HashCheck Shell Extension`, is also used by gurnec's fork. WinGet identifies that key as `gurnec.HashCheckShellExtension`. Installation removes the old key after writing the replacement. Both uninstall and `NoUninstall` clean up the current and legacy keys in the calling DLL's registry view. On a 64-bit system the companion 32-bit DLL uses `NoUninstall`; it must not remove the native DLL's uninstall entry.

## Metadata for the next release

When publishing a release containing this change, add these fields to its installer manifest in `microsoft/winget-pkgs`:

```yaml
ProductCode: idrassi.HashCheckShellExtension
AppsAndFeaturesEntries:
- DisplayName: HashCheck Shell Extension
  Publisher: idrassi
  ProductCode: idrassi.HashCheckShellExtension
```

Use the new release's actual package version, installer URL and SHA-256. Keep `PackageIdentifier: idrassi.HashCheckShellExtension` and `Publisher: idrassi` in the appropriate manifests. The default locale's license should be `BSD-3-Clause`.

These fields describe the new installer only. Do not assign the new product code to an older installer or claim the legacy product code under both forks. Existing installations need the updated installer to migrate their registry entry. The distinct publisher also allows name and publisher matching against the existing catalog while the explicit product code is being added.

## Validation

The native tests exercise the production registration code with HKLM redirected to a temporary HKCU key within the test process. They do not install HashCheck or modify its real registration. Run these commands from a Visual Studio Developer PowerShell, repeating them with `Platform=Win32` and the corresponding executable path for the companion DLL's build:

```powershell
msbuild UnitTests\UninstallRegistrationTests.vcxproj /p:Configuration=Release /p:Platform=x64
.\UnitTests\bin\UninstallRegistration\x64\UninstallRegistrationTests.exe
```

With Visual Studio 2022, append `/p:PlatformToolset=v143` if v142 is not installed.

Before publishing, use a disposable Windows machine to check a clean install and migration from the legacy registration. Confirm that the native uninstall entry remains after the companion DLL has registered, and that uninstall removes it. Check both a manual installation and one made through WinGet:

```powershell
winget list --id idrassi.HashCheckShellExtension --exact --source winget
winget export --source winget --output packages.json
winget export --source winget --include-versions --output packages-with-versions.json
```

The exports should identify `idrassi.HashCheckShellExtension`. A versioned export also requires that installed version to be published in the catalog before it can be imported on another machine.
