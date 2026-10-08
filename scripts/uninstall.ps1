# Command-line alternative to uninstall.cmd. All user settings are retained.
[CmdletBinding()]
param(
    [string] $SystemInformerDirectory,
    [switch] $RemoveCompanions
)
. (Join-Path $PSScriptRoot 'setup-common.ps1')
if (-not $SystemInformerDirectory) { $SystemInformerDirectory = Find-SystemInformerDirectory }
$Distros = @(Get-Wsl2Distributions)
Invoke-Setup 'Uninstall' $SystemInformerDirectory '' $RemoveCompanions.IsPresent $Distros $null $null
