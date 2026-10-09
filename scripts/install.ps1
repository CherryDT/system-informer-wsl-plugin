# ZIP/source command-line installation. Companions are opt-in; settings are preserved by default.
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
    # Keep profile discovery in the original caller's context. The settings
    # helper may elevate only a portable-file write, pinned to this inspection.
    $Report = [IO.Path]::GetTempFileName()
    try {
        $Arguments = @{
            SystemInformerDirectory = $SystemInformerDirectory
            ResultFile = $Report
            Operation = 'PathOverrides'
            PathOverridesJson = ConvertTo-Json -InputObject $PathOverrides -Compress
        }
        & (Join-Path $PSScriptRoot 'setup-settings.ps1') -Mode Inspect @Arguments
        $ResultCode = $LASTEXITCODE
        $Inspection = @{}
        foreach ($Line in Get-Content -LiteralPath $Report) {
            if ($Line -match '^([^=]+)=(.*)$') { $Inspection[$Matches[1]] = $Matches[2] }
        }
        if ($ResultCode -ne 0 -or $Inspection['Supported'] -ne '1') { throw $Inspection['Message'] }
        & (Join-Path $PSScriptRoot 'setup-settings.ps1') -Mode Enable @Arguments -ExpectedSettingsPath $Inspection['Path']
        $ResultCode = $LASTEXITCODE
        $Message = Get-Content -LiteralPath $Report | Where-Object { $_.StartsWith('Message=') } | Select-Object -First 1
        if ($Message) { $Message = $Message.Substring(8) }
        else { $Message = 'Could not update Explorer path prefixes in the System Informer settings file.' }
        if ($ResultCode -ne 0) { throw $Message }
        Write-Host $Message
    } finally {
        [IO.File]::Delete($Report)
    }
}
