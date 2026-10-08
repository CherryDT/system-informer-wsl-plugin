# Command-line alternative to setup.cmd. No settings are changed by default.
[CmdletBinding()]
param(
    [string] $SystemInformerDirectory,
    [string] $DistributionDirectory,
    [switch] $InstallCompanions,
    [hashtable] $PathOverrides = @{}
)
. (Join-Path $PSScriptRoot 'setup-common.ps1')
if (-not $SystemInformerDirectory) { $SystemInformerDirectory = Find-SystemInformerDirectory }
$Distros = @(Get-Wsl2Distributions)
Invoke-Setup 'Install' $SystemInformerDirectory $DistributionDirectory $InstallCompanions.IsPresent $Distros $null $null
if ($PathOverrides.Count) {
    $Key = 'HKCU:\Software\David Trapp\System Informer WSL Plugin\PathOverrides'
    New-Item -Path $Key -Force | Out-Null
    foreach ($Distro in $PathOverrides.Keys) {
        New-ItemProperty -Path $Key -Name $Distro -PropertyType String -Value $PathOverrides[$Distro] -Force | Out-Null
    }
}
