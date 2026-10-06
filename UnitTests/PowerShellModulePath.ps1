# Check the actual installer and uninstaller initialization in fresh processes.
# Only resolve commands: no packages are installed or removed by this test.
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$installer = [IO.File]::ReadAllText((Join-Path $repoRoot 'installer\HashCheck.nsi'))
$uninstaller = [IO.File]::ReadAllText((Join-Path $repoRoot 'HashCheck.cpp'))

$installMatch = [regex]::Match($installer, '(?m)^\s*FileWrite \$LogHandle "(\$\$env:PSModulePath[^"\r\n]*)"\s*$')
$uninstallMatch = [regex]::Match($uninstaller, 'TEXT\("(\$env:PSModulePath[^"\r\n]*)"\)')
if (!$installMatch.Success -or !$uninstallMatch.Success) {
    throw 'Could not find the module initialization in both production helpers.'
}
$initializers = @{
    Installer = $installMatch.Groups[1].Value.Replace('$\r$\n', '').Replace('$$', '$')
    Uninstaller = $uninstallMatch.Groups[1].Value.Replace('\\', '\')
}

$check = @'
$ErrorActionPreference = 'Stop'
$commands = @('Get-AuthenticodeSignature', 'Add-Content', 'Test-Path')
if ([IO.File]::Exists($PSHOME + '\Modules\Appx\Appx.psd1')) {
    $commands += @('Get-AppxPackage', 'Add-AppxPackage', 'Remove-AppxPackage')
}
if ([IO.File]::Exists($PSHOME + '\Modules\Dism\Dism.psd1')) {
    $commands += @('Get-AppxProvisionedPackage', 'Add-AppxProvisionedPackage', 'Remove-AppxProvisionedPackage')
}
foreach ($name in $commands) {
    $command = Get-Command $name -ErrorAction Stop
    # Appx exports cmdlets from a nested assembly in the Windows assembly cache.
    if ($name -in @('Get-AuthenticodeSignature', 'Add-Content', 'Test-Path') -and
        !$command.Module.Path.StartsWith($PSHOME + '\Modules\', [StringComparison]::OrdinalIgnoreCase)) {
        throw ('Wrong module for ' + $name + ': ' + $command.Module.Path)
    }
}
foreach ($module in (Get-Module Appx, Dism)) {
    if (!$module.ModuleBase.StartsWith($PSHOME + '\Modules\', [StringComparison]::OrdinalIgnoreCase)) {
        throw ('Wrong system module location: ' + $module.ModuleBase)
    }
}
[Console]::WriteLine('Resolved ' + $commands.Count + ' commands from Windows PowerShell modules.')
'@

$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('HashCheck-PSModules-' + [Guid]::NewGuid())
$null = [IO.Directory]::CreateDirectory($testRoot)
$scriptPath = Join-Path $testRoot 'check.ps1'
$shadowRoot = Join-Path $testRoot 'incompatible-modules'
$shadowModule = Join-Path $shadowRoot 'Microsoft.PowerShell.Security'
$null = [IO.Directory]::CreateDirectory($shadowModule)
# Model a module inherited from another PowerShell installation. Command discovery
# can see its export, but importing the module fails in the child process.
[IO.File]::WriteAllText((Join-Path $shadowModule 'Microsoft.PowerShell.Security.psd1'), @'
@{
    ModuleVersion = '7.0.0'
    PowerShellVersion = '3.0'
    RootModule = 'Microsoft.PowerShell.Security.psm1'
    FunctionsToExport = @('Get-AuthenticodeSignature')
}
'@)
[IO.File]::WriteAllText((Join-Path $shadowModule 'Microsoft.PowerShell.Security.psm1'),
    "throw 'This test module is incompatible with the child process.'")
$powerShellExe = Join-Path ([Environment]::GetFolderPath('System')) 'WindowsPowerShell\v1.0\powershell.exe'

function Run-Check([string]$Prefix) {
    [IO.File]::WriteAllText($scriptPath, $Prefix + [Environment]::NewLine + $check)
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $powerShellExe
    $start.Arguments = '-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + $scriptPath + '"'
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.EnvironmentVariables['PSModulePath'] = $shadowRoot + ';' + $PSHOME + '\Modules'
    $process = [Diagnostics.Process]::Start($start)
    try {
        $output = $process.StandardOutput.ReadToEndAsync()
        $errors = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit(30000)) {
            $process.Kill()
            throw 'Module lookup timed out.'
        }
        return @{
            Code = $process.ExitCode
            Text = $output.GetAwaiter().GetResult() + $errors.GetAwaiter().GetResult()
        }
    } finally {
        $process.Dispose()
    }
}

try {
    $baseline = Run-Check ''
    if ($baseline.Code -eq 0) {
        throw 'The incompatible inherited module did not trigger the expected baseline failure.'
    }
    foreach ($name in $initializers.Keys) {
        $result = Run-Check $initializers[$name]
        if ($result.Code -ne 0) {
            throw ($name + ' module lookup failed: ' + $result.Text)
        }
        Write-Host ($name + ': ' + $result.Text.Trim())
    }
} finally {
    $resolvedRoot = [IO.Path]::GetFullPath($testRoot)
    $resolvedTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    if ([IO.Path]::GetDirectoryName($resolvedRoot) -ne $resolvedTemp -or
        ![IO.Path]::GetFileName($resolvedRoot).StartsWith('HashCheck-PSModules-')) {
        throw 'Refusing to remove a directory outside the test location.'
    }
    Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
}
