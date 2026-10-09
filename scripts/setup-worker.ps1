# Headless Inno Setup bridge. Run as the original Windows user; only the
# Windows file-copy helper may elevate, so WSL uses that user's registrations.
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [ValidateSet('Install', 'Uninstall')] [string] $Mode,
    [Parameter(Mandatory)] [ValidateSet('Windows', 'Companions', 'PluginSettings', 'ImageLoadProtection', 'ResetPluginSettings', 'RestoreImageLoadProtection')] [string] $Phase,
    [string] $SystemInformerDirectory,
    [string] $DistributionDirectory,
    [string] $SettingsFile,
    [Parameter(Mandatory)] [string] $ProgressFile,
    [Parameter(Mandatory)] [string] $CancelFile
)

$ErrorActionPreference = 'Stop'
try {
    . (Join-Path $PSScriptRoot 'setup-common.ps1')
    # Initialize even an empty log with a BOM for TStringList.LoadFromFile.
    [IO.File]::WriteAllText($ProgressFile, '', [Text.Encoding]::Unicode)
    Test-SetupCancelled $CancelFile
    if ($Phase -eq 'Windows' -and -not $SystemInformerDirectory) {
        throw 'SystemInformerDirectory is required for the Windows phase.'
    }
    if ($Mode -eq 'Install') {
        if (-not $DistributionDirectory) { throw 'DistributionDirectory is required for installation.' }
        foreach ($File in @('WslTools.dll', 'wsl-observer')) {
            if (-not [IO.File]::Exists((Join-Path $DistributionDirectory $File))) {
                throw "Missing release file: $File"
            }
        }
    }

    if ($Phase -eq 'Windows') {
        Invoke-WindowsFileAction $Mode $SystemInformerDirectory $DistributionDirectory $ProgressFile
        # A copy already in progress finishes before cancellation takes effect.
        Test-SetupCancelled $CancelFile
    } elseif ($Phase -in @('PluginSettings', 'ImageLoadProtection', 'ResetPluginSettings', 'RestoreImageLoadProtection')) {
        $Reset = $Phase -in @('ResetPluginSettings', 'RestoreImageLoadProtection')
        $Operation = if ($Phase -in @('ImageLoadProtection', 'RestoreImageLoadProtection')) { 'ImageLoadProtection' } else { 'PluginLoading' }
        if (($Reset -and $Mode -ne 'Uninstall') -or (-not $Reset -and $Mode -ne 'Install')) {
            throw 'Settings action does not match the setup mode.'
        }
        Test-SetupCancelled $CancelFile
        $Report = $ProgressFile + '.ini'
        & (Join-Path $PSScriptRoot 'setup-settings.ps1') -Mode Enable `
            -SystemInformerDirectory $SystemInformerDirectory -ExpectedSettingsPath $SettingsFile -ResultFile $Report `
            -Operation $Operation -Reset:$Reset
        $ResultCode = $LASTEXITCODE
        $Message = (Get-Content -LiteralPath $Report | Where-Object { $_.StartsWith('Message=') } |
            Select-Object -First 1)
        if ($Message) { $Message = $Message.Substring(8) }
        else { $Message = 'Could not update the selected host setting. Check the System Informer settings file.' }
        if ($ResultCode -ne 0) { throw $Message }
        Write-SetupProgress $Message $ProgressFile
        Test-SetupCancelled $CancelFile
    } else {
        $Distros = @(Get-Wsl2Distributions)
        $Failures = 0
        $Observer = if ($Mode -eq 'Install') { Join-Path $DistributionDirectory 'wsl-observer' } else { '' }
        if (-not $Distros.Count) {
            Write-SetupProgress 'No WSL 2 distributions are registered for this Windows user.' $ProgressFile
        }
        foreach ($Distro in $Distros) {
            Test-SetupCancelled $CancelFile
            Write-SetupProgress "$Distro`: $($Mode.ToLowerInvariant()) Linux companion (temporarily starts the distro if stopped)..." $ProgressFile
            try {
                # Invoke-WslComponent runs as root and enforces 90 seconds per distro.
                Invoke-WslComponent $Mode $Distro $Observer $CancelFile
                Test-SetupCancelled $CancelFile
                Write-SetupProgress "$Distro`: completed." $ProgressFile
            } catch {
                Write-SetupProgress "$Distro`: $($_.Exception.Message)" $ProgressFile
                Test-SetupCancelled $CancelFile
                ++$Failures
            }
        }
        Test-SetupCancelled $CancelFile
        if ($Failures) {
            throw "$Failures WSL distribution(s) failed. See the results above, then retry if needed."
        }
    }
    Write-SetupProgress 'Done. Unrelated settings were preserved.' $ProgressFile
    exit 0
} catch {
    $Diagnostic = $_.Exception.Message
    $ExitCode = if ([IO.File]::Exists($CancelFile)) { 2 } else { 1 }
    try { Write-SetupProgress $Diagnostic $ProgressFile }
    catch { Write-Error $Diagnostic -ErrorAction Continue }
    exit $ExitCode
}
