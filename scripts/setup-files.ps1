# Internal file-copy phase. This is the only process that may run elevated.
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [ValidateSet('Install', 'Uninstall')] [string] $Mode,
    [Parameter(Mandatory)] [string] $SystemInformerDirectory,
    [string] $DistributionDirectory
)
$ErrorActionPreference = 'Stop'
try {
    . (Join-Path $PSScriptRoot 'setup-common.ps1')
    $SystemInformerDirectory = [IO.Path]::GetFullPath($SystemInformerDirectory)
    if (-not (Test-Path -LiteralPath (Join-Path $SystemInformerDirectory 'SystemInformer.exe'))) {
        throw 'The destination does not contain SystemInformer.exe.'
    }
    Assert-SystemInformerClosed $SystemInformerDirectory
    $Destination = Join-Path $SystemInformerDirectory 'plugins'
    if ($Mode -eq 'Install') { [IO.Directory]::CreateDirectory($Destination) | Out-Null }
    $Files = @('WslTools.dll', 'wsl-observer', 'WslTools.pdb')
    if ($Mode -eq 'Install') {
        foreach ($File in @('WslTools.dll', 'wsl-observer')) {
            if (-not [IO.File]::Exists((Join-Path $DistributionDirectory $File))) { throw "Missing $File" }
        }
        foreach ($File in $Files) {
            $Source = Join-Path $DistributionDirectory $File
            if ([IO.File]::Exists($Source)) {
                # .NET copying also works when SSHFS reports duplicate file IDs.
                [IO.File]::Copy($Source, (Join-Path $Destination $File), $true)
            }
        }
    } else {
        foreach ($File in $Files) {
            $Target = Join-Path $Destination $File
            if ([IO.File]::Exists($Target)) { [IO.File]::Delete($Target) }
        }
    }
    exit 0
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
}
