# Copies this plugin only. Settings and running System Informer instances are untouched.
[CmdletBinding()]
param(
    [string] $SystemInformerDirectory = "$env:ProgramFiles/SystemInformer",
    [string] $DistributionDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) 'dist'),
    [hashtable] $PathOverrides = @{}
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# In a ZIP, the script is next to the binaries instead of inside the checkout.
if (-not (Test-Path "$DistributionDirectory/WslTools.dll")) {
    if (Test-Path "$PSScriptRoot/WslTools.dll") { $DistributionDirectory = $PSScriptRoot }
    else { throw 'Build the project first, or run install.ps1 from the extracted release package.' }
}
foreach ($File in @('SystemInformer.exe')) {
    if (-not (Test-Path (Join-Path $SystemInformerDirectory $File))) { throw "Not a System Informer directory: $SystemInformerDirectory" }
}
foreach ($File in @('WslTools.dll', 'wsl-observer')) {
    if (-not (Test-Path (Join-Path $DistributionDirectory $File))) { throw "Missing release file: $File" }
}
$Destination = Join-Path $SystemInformerDirectory 'plugins'
New-Item -ItemType Directory -Force $Destination | Out-Null
foreach ($File in @('WslTools.dll', 'wsl-observer')) {
    try { Copy-Item (Join-Path $DistributionDirectory $File) (Join-Path $Destination $File) -Force }
    catch { throw "Could not install $File. Close System Informer before replacing a loaded plugin; Program Files also requires an elevated shell. $($_.Exception.Message)" }
}
if ($PathOverrides.Count) {
    $Key = 'HKCU:\Software\David Trapp\System Informer WSL Plugin\PathOverrides'
    New-Item -Path $Key -Force | Out-Null
    foreach ($Distro in $PathOverrides.Keys) {
        New-ItemProperty -Path $Key -Name $Distro -PropertyType String -Value $PathOverrides[$Distro] -Force | Out-Null
    }
}
Write-Host "Installed WSL Tools into $Destination"
Write-Host 'Enable plugins and set Advanced setting EnableDefaultSafePlugins to 0 if needed, then restart System Informer.'
Write-Host 'No existing System Informer settings were changed.'
