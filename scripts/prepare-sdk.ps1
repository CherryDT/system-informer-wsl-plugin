# Build the public plugin SDK from a fixed System Informer source revision.
# System Informer itself does not need to be built. Its export definition is
# sufficient to produce the import library used by third-party plugins.
[CmdletBinding()]
param(
    [string] $Root = (Split-Path $PSScriptRoot -Parent),
    [string] $VisualStudio
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Revision = 'bfc8145f2744a415319ccaaa8f1e32dd2cf1e596'
$ArchiveHash = '322BA10F994900A559BDFD7CD7E47C7982F517AA69DC8B067F01D02C690E925D'
$Dependencies = Join-Path $Root '.deps'
$Source = Join-Path $Dependencies "systeminformer-$Revision"
$Sdk = Join-Path $Dependencies 'sdk'
New-Item -ItemType Directory -Force $Dependencies, "$Sdk/include", "$Sdk/lib" | Out-Null

if (-not (Test-Path "$Source/SystemInformer/sdk/phdk.h")) {
    $Archive = Join-Path $Dependencies "systeminformer-$Revision.tar.gz"
    if (-not (Test-Path $Archive)) {
        Write-Host "Downloading System Informer SDK sources ($Revision)..."
        Invoke-WebRequest "https://codeload.github.com/winsiderss/systeminformer/tar.gz/$Revision" -OutFile $Archive
    }
    if ((Get-FileHash $Archive -Algorithm SHA256).Hash -ne $ArchiveHash) {
        throw "SDK archive checksum mismatch: $Archive"
    }
    & tar.exe -xf $Archive -C $Dependencies
    if ($LASTEXITCODE -ne 0) { throw 'Unable to extract the System Informer SDK sources.' }
}

# Follow upstream HeaderGen: discover its input list, order local includes before
# their consumers, then retain public blocks and individual // phapppub lines.
# Reading the list from the pinned generator avoids maintaining a second list.
$Generator = Get-Content "$Source/tools/CustomBuildTool/HeaderGen.cs" -Raw
$FileList = [regex]::Match($Generator, '(?s)string\[\] Files\s*=\s*\[(.*?)\];').Groups[1].Value
if (-not $FileList) { throw 'The SDK header generator format has changed.' }
$Names = @([regex]::Matches($FileList, '"([^"]+\.h)"') | ForEach-Object { $_.Groups[1].Value })
$Headers = @{}
foreach ($Name in $Names) {
    $Headers[$Name] = @{ Lines = @(Get-Content "$Source/SystemInformer/include/$Name"); Dependencies = @() }
}
foreach ($Name in $Names) {
    $Lines = [Collections.Generic.List[string]]::new()
    foreach ($Line in $Headers[$Name].Lines) {
        if ($Line.Trim() -match '^#include <([^>]+)>$' -and $Headers.ContainsKey($Matches[1])) {
            $Headers[$Name].Dependencies += $Matches[1]
        } else { $Lines.Add($Line) }
    }
    $Headers[$Name].Lines = $Lines
}
$Visited = @{}
$Ordered = [Collections.Generic.List[string]]::new()
function Add-Header([string] $Name) {
    if ($Visited.ContainsKey($Name)) { return }
    $Visited[$Name] = $true
    foreach ($Dependency in $Headers[$Name].Dependencies) { Add-Header $Dependency }
    $Ordered.Add($Name)
}
foreach ($Name in $Names) { Add-Header $Name }
$Public = [Collections.Generic.List[string]]::new()
$Public.AddRange([string[]]@(
    '/* Copyright (c) Winsider Seminars & Solutions, Inc. All rights reserved.',
    ' * This file is part of System Informer. Generated from its public SDK declarations. */',
    '#ifndef _PH_PHAPPPUB_H', '#define _PH_PHAPPPUB_H',
    '#ifdef __cplusplus', 'extern "C" {', '#endif'
))
foreach ($Name in $Ordered) {
    $Public.Add("`r`n// $Name")
    $Modes = @{}
    $Blank = $false
    foreach ($Line in $Headers[$Name].Lines) {
        $Trimmed = $Line.Trim()
        if (-not $Trimmed) { $Blank = $true; continue }
        if ($Trimmed -match '^// begin_(.+)$') { $Modes[$Matches[1]] = $true; continue }
        if ($Trimmed -match '^// end_(.+)$') { $Modes.Remove($Matches[1]); continue }
        if ($Modes.ContainsKey('phapppub') -or $Trimmed -match '// phapppub[\p{L}\p{N} /]*$') {
            if ($Blank) { $Public.Add('') }
            $Public.Add($Line)
            $Blank = $false
        }
    }
}
$Public.AddRange([string[]]@('#ifdef __cplusplus', '}', '#endif', '#endif'))
[IO.File]::WriteAllLines("$Sdk/include/phapppub.h", $Public, [Text.UTF8Encoding]::new($false))
foreach ($Directory in @('phlib/include', 'phnt/include', 'kphlib/include')) {
    foreach ($Header in [IO.Directory]::GetFiles("$Source/$Directory", "*.h")) {
        [IO.File]::Copy($Header, (Join-Path "$Sdk/include" ([IO.Path]::GetFileName($Header))), $true)
    }
}
[IO.File]::Copy("$Source/SystemInformer/sdk/phdk.h", "$Sdk/include/phdk.h", $true)
[IO.File]::Copy("$Source/SystemInformer/include/phappres.h", "$Sdk/include/phappres.h", $true)
[IO.File]::Copy("$Source/LICENSE.txt", "$Sdk/LICENSE.txt", $true)

if (-not $VisualStudio) {
    $VsWhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
    if (-not (Test-Path $VsWhere)) { throw 'Visual Studio 2022 with Desktop development with C++ is required.' }
    $VisualStudio = & $VsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
$Version = (Get-Content "$VisualStudio/VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt" -Raw).Trim()
$Librarian = "$VisualStudio/VC/Tools/MSVC/$Version/bin/Hostx64/x64/lib.exe"
# Release System Informer executables export by ordinal only. Reproduce its
# ExportDefinitions step; the source-tree .def.h is not a reliable ordinal map.
$Definition = [Collections.Generic.List[string]]::new()
$Definition.Add('EXPORTS')
$Ordinal = 1001
foreach ($Line in Get-Content "$Source/SystemInformer/SystemInformer.def") {
    if ($Line -match '^    (\S+)(.*)$') {
        $ExportName = $Matches[1]
        $Data = if ($Matches[2] -match '\bDATA\b') { ' DATA' } else { '' }
        $Definition.Add("    $ExportName @$Ordinal NONAME$Data")
        $Ordinal++
    }
}
[IO.File]::WriteAllLines("$Sdk/SystemInformer.def", $Definition, [Text.UTF8Encoding]::new($false))
& $Librarian /nologo /machine:x64 "/def:$Sdk/SystemInformer.def" /name:SystemInformer.exe "/out:$Sdk/lib/SystemInformer.lib"
if ($LASTEXITCODE -ne 0) { throw 'Could not build the System Informer import library.' }
[IO.File]::WriteAllText("$Sdk/revision.txt", "$Revision`r`n4.0.26255.346`r`n")
Write-Host "SDK prepared in $Sdk (baseline: System Informer 4.0.26255.346, bfc8145f)"
