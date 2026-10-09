# Standalone, user-context settings phase. Only Enable authorizes a write.
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [ValidateSet('Inspect', 'Enable')] [string] $Mode,
    [Parameter(Mandatory)] [string] $SystemInformerDirectory,
    [Parameter(Mandatory)] [string] $ResultFile,
    [string] $ExpectedSettingsPath,
    [ValidateSet('PluginLoading', 'ImageLoadProtection')] [string] $Operation = 'PluginLoading',
    [switch] $Reset,
    # Private continuation: never rediscover an alternate administrator's profile.
    [switch] $Elevated
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:SettingsPath = ''
$script:KphEnabled = $true

# System Informer treats these integer settings as booleans (any nonzero
# value is true). Resetting plugin policy must not change EnablePlugins.
if ($Operation -eq 'ImageLoadProtection') {
    $Wanted = if ($Reset) { 0 } else { 1 }
    $Targets = @([pscustomobject]@{ Name = 'KsiDisableImageLoadProtection'; Default = 0; Wanted = $Wanted })
    $ActionDescription = if ($Reset) { 'Restore image-load protection by setting KsiDisableImageLoadProtection to 0.' }
                         else { 'Allow DLLs not trusted by the kernel driver by setting KsiDisableImageLoadProtection to 1.' }
    $CompletedDescription = if ($Reset) { 'Image-load protection is configured to be restored at the next kernel-driver reload.' }
                            else { 'Untrusted DLL loading is configured for the next kernel-driver reload.' }
    $UnchangedDescription = if ($Reset) { 'Image-load protection is already configured as enabled.' }
                            else { 'Untrusted DLL loading is already configured as allowed.' }
} elseif ($Reset) {
    $Targets = @([pscustomobject]@{ Name = 'EnableDefaultSafePlugins'; Default = 1; Wanted = 1 })
    $ActionDescription = 'Restore the default safe-plugin policy by setting EnableDefaultSafePlugins to 1. EnablePlugins is preserved.'
    $CompletedDescription = 'Default safe-plugin policy restored. EnablePlugins is preserved.'
    $UnchangedDescription = 'Default safe-plugin policy is already enabled. EnablePlugins is preserved.'
} else {
    $Targets = @(
        [pscustomobject]@{ Name = 'EnableDefaultSafePlugins'; Default = 1; Wanted = 0 },
        [pscustomobject]@{ Name = 'EnablePlugins'; Default = 1; Wanted = 1 }
    )
    $ActionDescription = 'Allow third-party plugins by setting EnableDefaultSafePlugins to 0 and EnablePlugins to 1.'
    $CompletedDescription = 'Third-party plugin loading enabled.'
    $UnchangedDescription = 'Third-party plugin loading is already enabled.'
}
$ManualDescription = "Configure the selected settings manually in System Informer: $ActionDescription"

function Write-Result([bool] $Supported, [bool] $NeedsChange, [string] $Message) {
    $Message = $Message -replace '[\r\n]+', ' '
    $Text = "[Plugins]`r`nSupported=$([int]$Supported)`r`nNeedsChange=$([int]$NeedsChange)`r`nPath=$script:SettingsPath`r`nKphEnabled=$([int]$script:KphEnabled)`r`nMessage=$Message`r`n"
    [IO.File]::WriteAllText($ResultFile, $Text, [Text.Encoding]::Unicode)
}

function Find-FileStore([string] $BasePath, [string] $Kind) {
    # Matches phlib/settings.c: JSON wins; otherwise newest, ties XML/DAT/BIN.
    $Best = $null
    foreach ($Extension in @('.json', '.xml', '.dat', '.bin')) {
        $Path = $BasePath + $Extension
        if (Test-Path -LiteralPath $Path) {
            $Item = Get-Item -LiteralPath $Path -Force
            if ($Item.PSIsContainer) { throw "The settings path is a directory: $Path" }
            $Candidate = [pscustomobject]@{ Path = $Path; Kind = $Kind; Extension = $Extension; Time = $Item.LastWriteTimeUtc }
            if ($Extension -eq '.json') { return $Candidate }
            if ($null -eq $Best -or $Candidate.Time -gt $Best.Time) { $Best = $Candidate }
        }
    }
    return $Best
}

function Find-Store {
    $Portable = Find-FileStore (Join-Path $SystemInformerDirectory 'SystemInformer.exe.settings') 'Portable'
    if ($Portable) { return $Portable }
    if ($Elevated) { throw 'The selected portable settings store changed. Run setup again without elevation.' }
    $Roaming = [Environment]::GetFolderPath([Environment+SpecialFolder]::ApplicationData)
    if (-not $Roaming) { throw 'The Windows roaming application-data folder could not be resolved.' }
    $Base = Join-Path $Roaming 'SystemInformer\settings'
    $Store = Find-FileStore $Base 'Roaming'
    if ($Store) { return $Store }
    if (Test-Path -LiteralPath 'HKCU:\Software\SystemInformer') {
        return [pscustomobject]@{ Path = 'HKCU\Software\SystemInformer'; Kind = 'Registry'; Extension = '' }
    }
    return [pscustomobject]@{ Path = $Base + '.json'; Kind = 'Roaming'; Extension = '.json' }
}

function Read-TextFile([string] $Path) {
    $Exists = [IO.File]::Exists($Path)
    [byte[]] $Bytes = @()
    $Encoding = New-Object Text.UTF8Encoding($false, $true)
    $Offset = 0
    if ($Exists) {
        $Bytes = [IO.File]::ReadAllBytes($Path)
        if ($Bytes.Length -ge 4 -and (($Bytes[0] -eq 0 -and $Bytes[1] -eq 0 -and $Bytes[2] -eq 254 -and $Bytes[3] -eq 255) -or
            ($Bytes[0] -eq 255 -and $Bytes[1] -eq 254 -and $Bytes[2] -eq 0 -and $Bytes[3] -eq 0))) {
            throw (New-Object NotSupportedException('UTF-32 settings are not supported by setup.'))
        }
        if ($Bytes.Length -ge 3 -and $Bytes[0] -eq 239 -and $Bytes[1] -eq 187 -and $Bytes[2] -eq 191) {
            $Encoding = New-Object Text.UTF8Encoding($true, $true); $Offset = 3
        } elseif ($Bytes.Length -ge 2 -and $Bytes[0] -eq 255 -and $Bytes[1] -eq 254) {
            $Encoding = New-Object Text.UnicodeEncoding($false, $true, $true); $Offset = 2
        } elseif ($Bytes.Length -ge 2 -and $Bytes[0] -eq 254 -and $Bytes[1] -eq 255) {
            $Encoding = New-Object Text.UnicodeEncoding($true, $true, $true); $Offset = 2
        }
    }
    $Text = if ($Exists) { $Encoding.GetString($Bytes, $Offset, $Bytes.Length - $Offset) } else { '{}' }
    return [pscustomobject]@{ Exists = $Exists; Bytes = $Bytes; Encoding = $Encoding; Text = $Text }
}

function Get-SettingInteger($Value, [bool] $IsString) {
    if ($IsString) {
        [uint32] $Number = 0
        if (-not [uint32]::TryParse([string]$Value, [Globalization.NumberStyles]::AllowHexSpecifier,
            [Globalization.CultureInfo]::InvariantCulture, [ref]$Number)) { throw 'A selected setting is not a valid hexadecimal integer.' }
        return $Number
    }
    if ([string]$Value -notmatch '^(0|[1-9][0-9]*)$') { throw 'A selected setting is not an unsigned integer.' }
    return [uint32]::Parse([string]$Value, [Globalization.CultureInfo]::InvariantCulture)
}

function Edit-Json([string] $Text) {
    Add-Type -AssemblyName System.Web.Extensions
    $Json = New-Object Web.Script.Serialization.JavaScriptSerializer
    $Json.MaxJsonLength = [int]::MaxValue
    # Validate the flat object and retain exact token offsets. Deserializing and
    # serializing the whole profile would lose formatting and unknown values.
    $StringToken = '"(?:[^"\\\x00-\x1f]|\\(?:["\\/bfnrt]|u[0-9a-fA-F]{4}))*"'
    $ValueToken = '(?:' + $StringToken + '|-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?|true|false|null)'
    $PairPattern = '\G\s*(?<key>' + $StringToken + ')\s*:\s*(?<value>' + $ValueToken + ')\s*'
    $Matcher = New-Object Text.RegularExpressions.Regex($PairPattern, [Text.RegularExpressions.RegexOptions]::None, [TimeSpan]::FromSeconds(5))
    $Open = [regex]::Match($Text, '\A\s*\{\s*')
    if (-not $Open.Success) { throw 'Expected a flat JSON settings object.' }
    $Position = $Open.Length
    $Pairs = New-Object 'Collections.Generic.List[object]'
    $Names = New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    if ($Position -lt $Text.Length -and $Text[$Position] -ne '}') {
        while ($true) {
            $Pair = $Matcher.Match($Text, $Position)
            if (-not $Pair.Success) { throw 'The JSON settings contain malformed data or an unsupported nested value.' }
            $Name = [string]$Json.DeserializeObject($Pair.Groups['key'].Value)
            if (-not $Names.Add($Name)) { throw "Duplicate settings name: $Name" }
            $Pairs.Add([pscustomobject]@{ Name = $Name; Token = $Pair.Groups['value'] })
            $Position = $Pair.Index + $Pair.Length
            if ($Position -ge $Text.Length) { throw 'The JSON settings object is incomplete.' }
            if ($Text[$Position] -eq '}') { break }
            if ($Text[$Position] -ne ',') { throw 'The JSON settings contain an invalid separator.' }
            ++$Position
        }
    }
    if ($Position -ge $Text.Length -or $Text.Substring($Position) -notmatch '^}\s*$') { throw 'The JSON settings have unexpected trailing content.' }
    # KsiEnable defaults to true. A disabled driver needs no relaxation of
    # its image policy; leave that preference untouched even if it is stricter.
    if ($Operation -eq 'ImageLoadProtection' -and -not $Reset) {
        $Driver = @($Pairs | Where-Object { $_.Name -ceq 'KsiEnable' })
        if ($Driver.Count) {
            $Token = $Driver[0].Token.Value
            $IsString = $Token.StartsWith('"')
            $Value = if ($IsString) { $Json.DeserializeObject($Token) } else { $Token }
            $script:KphEnabled = (Get-SettingInteger $Value $IsString) -ne 0
        } elseif ($Names.Contains('KsiEnable')) { throw 'Unexpected capitalization of setting: KsiEnable' }
    }
    $Edits = New-Object 'Collections.Generic.List[object]'
    $Missing = New-Object 'Collections.Generic.List[string]'
    $NeedsChange = $false
    foreach ($Target in $Targets) {
        $Name = $Target.Name
        $Wanted = $Target.Wanted
        $Entry = @($Pairs | Where-Object { $_.Name -ceq $Name })
        if ($Entry.Count -eq 0) {
            if ($Names.Contains($Name)) { throw "Unexpected capitalization of setting: $Name" }
            $Current = $Target.Default
        } else {
            $Token = $Entry[0].Token
            $IsString = $Token.Value.StartsWith('"')
            $Value = if ($IsString) { $Json.DeserializeObject($Token.Value) } else { $Token.Value }
            $Current = Get-SettingInteger $Value $IsString
        }
        if (($Current -ne 0) -ne ($Wanted -ne 0)) {
            $NeedsChange = $true
            if ($Entry.Count -eq 0) {
                $Missing.Add('"' + $Name + '": "' + $Wanted + '"')
            } else {
                $Edits.Add([pscustomobject]@{ Index = $Token.Index; Length = $Token.Length; Value = '"' + $Wanted + '"' })
            }
        }
    }
    if ($Missing.Count) {
        $Insert = if ($Pairs.Count) { ', ' } else { '' }
        $Edits.Add([pscustomobject]@{ Index = $Position; Length = 0; Value = $Insert + ($Missing -join ', ') })
    }
    foreach ($Edit in @($Edits | Sort-Object Index -Descending)) {
        $Text = $Text.Remove($Edit.Index, $Edit.Length).Insert($Edit.Index, $Edit.Value)
    }
    return [pscustomobject]@{ NeedsChange = $NeedsChange; Text = $Text }
}

function Edit-Xml([string] $Text) {
    $Options = New-Object Xml.XmlReaderSettings
    $Options.DtdProcessing = [Xml.DtdProcessing]::Prohibit
    $Options.XmlResolver = $null
    $Reader = [Xml.XmlReader]::Create((New-Object IO.StringReader($Text)), $Options)
    $Document = New-Object Xml.XmlDocument
    $Document.PreserveWhitespace = $true
    $Document.XmlResolver = $null
    try { $Document.Load($Reader) } finally { $Reader.Dispose() }
    if ($null -eq $Document.DocumentElement -or $Document.DocumentElement.get_Name() -cne 'settings' -or $Document.DocumentElement.get_NamespaceURI()) {
        throw 'Expected an XML settings root element.'
    }
    $Entries = @{}
    foreach ($Node in $Document.DocumentElement.ChildNodes) {
        if ($Node.NodeType -ne [Xml.XmlNodeType]::Element) { continue }
        if ($Node.get_Name() -cne 'setting' -or $Node.get_NamespaceURI() -or -not $Node.HasAttribute('name')) { throw 'Unknown XML settings structure.' }
        foreach ($Child in $Node.ChildNodes) {
            if ($Child.NodeType -notin @([Xml.XmlNodeType]::Text, [Xml.XmlNodeType]::CDATA, [Xml.XmlNodeType]::Whitespace, [Xml.XmlNodeType]::SignificantWhitespace)) {
                throw 'Unknown XML setting value structure.'
            }
        }
        $Name = $Node.GetAttribute('name')
        if ($Name.StartsWith('ProcessHacker.')) { throw 'Legacy-prefixed XML settings require manual configuration.' }
        if ($Entries.ContainsKey($Name)) { throw "Duplicate XML settings name: $Name" }
        $Entries[$Name] = $Node
    }
    if ($Operation -eq 'ImageLoadProtection' -and -not $Reset -and $Entries.ContainsKey('KsiEnable')) {
        $Driver = $Entries['KsiEnable']
        if ($Driver.GetAttribute('name') -cne 'KsiEnable') { throw 'Unexpected capitalization of setting: KsiEnable' }
        $script:KphEnabled = (Get-SettingInteger $Driver.get_InnerText() $true) -ne 0
    }
    $NeedsChange = $false
    foreach ($Target in $Targets) {
        $Name = $Target.Name
        $Wanted = $Target.Wanted
        $Current = $Target.Default
        if ($Entries.ContainsKey($Name)) {
            $Node = $Entries[$Name]
            if ($Node.GetAttribute('name') -cne $Name) { throw "Unexpected capitalization of setting: $Name" }
            $Current = Get-SettingInteger $Node.get_InnerText() $true
        }
        if (($Current -ne 0) -ne ($Wanted -ne 0)) {
            $NeedsChange = $true
            if (-not $Entries.ContainsKey($Name)) {
                $Node = $Document.CreateElement('setting')
                $Node.SetAttribute('name', $Name)
                [void] $Document.DocumentElement.AppendChild($Node)
            }
            $Node.set_InnerText([string]$Wanted)
        }
    }
    return [pscustomobject]@{ NeedsChange = $NeedsChange; Text = $Document.OuterXml }
}

function Read-Settings($Store) {
    if ($Store.Kind -eq 'Registry' -or $Store.Extension -notin @('.json', '.xml')) {
        return [pscustomobject]@{ Supported = $false; NeedsChange = $false; Message = "This settings store is not editable by setup. $ManualDescription" }
    }
    # Access failures are operational errors; malformed/unknown contents are a
    # supported inspection outcome and must never trigger a reset or migration.
    try { $Source = Read-TextFile $Store.Path }
    catch [NotSupportedException] {
        return [pscustomobject]@{ Supported = $false; NeedsChange = $false; Message = $_.Exception.Message }
    }
    catch [Text.DecoderFallbackException] {
        return [pscustomobject]@{ Supported = $false; NeedsChange = $false; Message = "The settings text encoding is invalid. $ManualDescription" }
    }
    try {
        $Edit = if ($Store.Extension -eq '.json') { Edit-Json $Source.Text } else { Edit-Xml $Source.Text }
    } catch {
        return [pscustomobject]@{ Supported = $false; NeedsChange = $false; Message = "Settings were left unchanged: $($_.Exception.Message) $ManualDescription" }
    }
    return [pscustomobject]@{ Supported = $true; NeedsChange = $Edit.NeedsChange; Source = $Source; Text = $Edit.Text; Message = '' }
}

function Assert-Closed([string] $Kind) {
    $Processes = @(Get-Process -Name SystemInformer -ErrorAction SilentlyContinue)
    if (-not $Processes.Count) { return }
    if ($Operation -eq 'ImageLoadProtection') { throw 'Close all System Informer instances before changing image-load protection; the kernel driver is shared across instances.' }
    if ($Kind -eq 'Roaming') { throw 'Close all System Informer instances before changing shared roaming settings.' }
    if (-not ('WslSettings.NativeProcess' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
namespace WslSettings {
    public static class NativeProcess {
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool QueryFullProcessImageName(IntPtr process, uint flags, StringBuilder path, ref uint size);
        [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
        public static string Path(int pid) {
            IntPtr handle = OpenProcess(0x1000, false, pid);
            if (handle == IntPtr.Zero) throw new Win32Exception();
            try {
                uint size = 32768;
                var path = new StringBuilder((int)size);
                if (!QueryFullProcessImageName(handle, 0, path, ref size)) throw new Win32Exception();
                return path.ToString();
            } finally { CloseHandle(handle); }
        }
    }
}
'@
    }
    $Expected = Join-Path $SystemInformerDirectory 'SystemInformer.exe'
    foreach ($Process in $Processes) {
        try { $Path = [WslSettings.NativeProcess]::Path($Process.Id) }
        catch { throw 'A System Informer instance could not be inspected. Close it before changing settings.' }
        if ([string]::Equals($Expected, $Path, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Close System Informer in the selected folder before changing settings.'
        }
    }
}

function Assert-SameStore($Store) {
    $Current = Find-Store
    if ($Current.Kind -ne $Store.Kind -or -not [string]::Equals($Current.Path, $Store.Path, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The active settings store changed. Run setup again to review the current settings.'
    }
}

function Save-Settings($Store, $Settings) {
    Assert-Closed $Store.Kind
    Assert-SameStore $Store
    $Directory = [IO.Path]::GetDirectoryName($Store.Path)
    [void] [IO.Directory]::CreateDirectory($Directory)
    $Temporary = Join-Path $Directory ('.wsl-tools-settings-' + [guid]::NewGuid().ToString('N') + '.tmp')
    $Backup = $Store.Path + '.wsl-tools-' + [guid]::NewGuid().ToString('N') + '.bak'
    try {
        [IO.File]::WriteAllText($Temporary, $Settings.Text, $Settings.Source.Encoding)
        Assert-Closed $Store.Kind
        Assert-SameStore $Store
        if ($Settings.Source.Exists) {
            # Abort on a concurrent edit instead of overwriting newly saved data.
            $Latest = [IO.File]::ReadAllBytes($Store.Path)
            if ([Convert]::ToBase64String($Latest) -cne [Convert]::ToBase64String($Settings.Source.Bytes)) {
                throw 'The settings file changed during setup. Run setup again.'
            }
            [IO.File]::Replace($Temporary, $Store.Path, $Backup)
            return "$CompletedDescription Backup: $Backup"
        }
        # Move refuses to overwrite a file created after inspection.
        [IO.File]::Move($Temporary, $Store.Path)
        return "$CompletedDescription New settings file: $($Store.Path)"
    } finally {
        if ([IO.File]::Exists($Temporary)) { [IO.File]::Delete($Temporary) }
    }
}

function Quote-Argument([string] $Value) {
    return '"' + [regex]::Replace([regex]::Replace($Value, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
}

try {
    $SystemInformerDirectory = [IO.Path]::GetFullPath($SystemInformerDirectory)
    $ResultFile = [IO.Path]::GetFullPath($ResultFile)
    if (-not [IO.File]::Exists((Join-Path $SystemInformerDirectory 'SystemInformer.exe'))) { throw 'The selected folder does not contain SystemInformer.exe.' }
    if ($Elevated -and ($Mode -ne 'Enable' -or -not $ExpectedSettingsPath)) { throw 'Invalid elevated settings continuation.' }
    $Store = Find-Store
    $script:SettingsPath = $Store.Path
    if ($Mode -eq 'Enable' -and (-not $ExpectedSettingsPath -or
        -not [string]::Equals($ExpectedSettingsPath, $Store.Path, [StringComparison]::OrdinalIgnoreCase))) {
        throw 'The active settings store changed or was not inspected. Run setup again before changing settings.'
    }
    $Settings = Read-Settings $Store
    if ($Settings.Supported -and $Operation -eq 'ImageLoadProtection' -and -not $Reset -and -not $script:KphEnabled) {
        Write-Result $true $false 'Not required (KPH disabled). Image-load protection settings were unchanged.'
        exit 0
    }
    if ($Mode -eq 'Inspect') {
        $Message = if (-not $Settings.Supported) { $Settings.Message }
                   elseif ($Settings.NeedsChange) { "$ActionDescription Other settings, including DisabledPlugins, are preserved." }
                   else { "$UnchangedDescription Other settings, including DisabledPlugins, are preserved." }
        Write-Result $Settings.Supported $Settings.NeedsChange $Message
        exit 0
    }
    if (-not $Settings.Supported) { throw $Settings.Message }
    if (-not $Settings.NeedsChange) {
        Write-Result $true $false "$UnchangedDescription Settings were unchanged."
        exit 0
    }
    try { $Message = Save-Settings $Store $Settings }
    catch [UnauthorizedAccessException] {
        if ($Store.Kind -ne 'Portable' -or $Elevated) { throw }
        # Only the portable file operation may elevate; all paths are fixed in
        # the original user context and the child requires the same store.
        $Arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath,
            '-Mode', 'Enable', '-Operation', $Operation, '-SystemInformerDirectory', $SystemInformerDirectory,
            '-ResultFile', $ResultFile, '-ExpectedSettingsPath', $Store.Path, '-Elevated')
        if ($Reset) { $Arguments += '-Reset' }
        $Start = New-Object Diagnostics.ProcessStartInfo
        $Start.FileName = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
        $Start.Arguments = ($Arguments | ForEach-Object { Quote-Argument $_ }) -join ' '
        $Start.UseShellExecute = $true
        $Start.Verb = 'runas'
        $Start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
        $Child = [Diagnostics.Process]::Start($Start)
        try { $Child.WaitForExit(); $Code = $Child.ExitCode } finally { $Child.Dispose() }
        exit $Code
    }
    Write-Result $true $false $Message
    exit 0
} catch {
    try { Write-Result $false $false $_.Exception.Message } catch { Write-Error $_ -ErrorAction Continue }
    exit 1
}
