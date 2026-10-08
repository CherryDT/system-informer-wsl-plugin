# Run from Windows PowerShell or pwsh. Only existing VS/Windows SDK tools are used.
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')] [string] $Configuration = 'Release',
    [string] $BuildRoot = "$env:LOCALAPPDATA/WslTools/build",
    [switch] $SkipSdk
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Root = Split-Path $PSScriptRoot -Parent
$VsWhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
if (-not (Test-Path $VsWhere)) { throw 'Install Visual Studio 2022 with Desktop development with C++ and a Windows SDK.' }
$VisualStudio = & $VsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $VisualStudio) { throw 'No installed Visual Studio C++ toolchain was found.' }
$CMake = "$VisualStudio/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
if (-not (Test-Path $CMake)) { $CMake = (Get-Command cmake.exe -ErrorAction Stop).Source }
if (-not $SkipSdk) { & "$PSScriptRoot/prepare-sdk.ps1" -Root $Root -VisualStudio $VisualStudio }

# MSVC's dependency tracking and PDB writes are unreliable on some WSL/SSHFS
# mounts. Copy inputs to a local Windows build directory, keeping repo outputs
# separate. This never deletes the checkout or deploys into System Informer.
$Stage = Join-Path $BuildRoot 'source'
New-Item -ItemType Directory -Force $Stage | Out-Null
foreach ($Directory in @('plugin', 'vendor', '.deps/sdk')) {
    $Destination = Join-Path $Stage $Directory
    New-Item -ItemType Directory -Force $Destination | Out-Null
    & robocopy.exe (Join-Path $Root $Directory) $Destination /MIR /IS /IT /NFL /NDL /NJH /NJS /NP
    if ($LASTEXITCODE -gt 7) { throw "Unable to stage $Directory" }
}
[IO.File]::Copy("$Root/CMakeLists.txt", "$Stage/CMakeLists.txt", $true)
$Binary = Join-Path $BuildRoot 'native'
& $CMake -S $Stage -B $Binary -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
# Cross-filesystem timestamps and MSBuild's cached include dependencies can
# otherwise leave translation units compiled against different header layouts.
# Force copies above and clean native objects before every reproducible build.
& $CMake --build $Binary --config $Configuration --clean-first --parallel
if ($LASTEXITCODE -ne 0) { throw 'Native plugin compilation failed.' }
$Dist = Join-Path $Root 'dist'
New-Item -ItemType Directory -Force $Dist | Out-Null
[IO.File]::Copy("$Binary/dist/$Configuration/WslTools.dll", "$Dist/WslTools.dll", $true)
if (Test-Path "$Binary/dist/$Configuration/WslTools.pdb") {
    [IO.File]::Copy("$Binary/dist/$Configuration/WslTools.pdb", "$Dist/WslTools.pdb", $true)
}
Write-Host "Built $Dist/WslTools.dll"
