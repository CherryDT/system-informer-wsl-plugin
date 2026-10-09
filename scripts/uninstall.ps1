# ZIP/source command-line removal. For an EXE installation, use Windows Installed apps.
[CmdletBinding()]
param(
    [string] $SystemInformerDirectory,
    [switch] $RemoveCompanions
)
. (Join-Path $PSScriptRoot 'setup-common.ps1')
if (-not $SystemInformerDirectory) { $SystemInformerDirectory = Find-SystemInformerDirectory }
$Distros = @(Get-Wsl2Distributions)
Invoke-Setup 'Uninstall' $SystemInformerDirectory '' $RemoveCompanions.IsPresent $Distros $null $null
