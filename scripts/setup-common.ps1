# Shared by the GUI and command-line installers. Windows copying and WSL work
# deliberately run in separate processes: elevation must not change the WSL user.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Find-SystemInformerDirectory {
    foreach ($Key in @('HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\SystemInformer',
                       'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\SystemInformer')) {
        $Entry = Get-ItemProperty -LiteralPath $Key -ErrorAction SilentlyContinue
        if ($Entry -and $Entry.PSObject.Properties['InstallLocation'] -and $Entry.InstallLocation) {
            if (Test-Path -LiteralPath (Join-Path $Entry.InstallLocation 'SystemInformer.exe')) { return $Entry.InstallLocation }
        }
    }
    return (Join-Path $env:ProgramFiles 'SystemInformer')
}

function Find-DistributionDirectory([string] $Directory) {
    if ($Directory -and (Test-Path -LiteralPath (Join-Path $Directory 'WslTools.dll'))) { return $Directory }
    if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'WslTools.dll')) { return $PSScriptRoot }
    $Directory = Join-Path (Split-Path $PSScriptRoot -Parent) 'dist'
    if (Test-Path -LiteralPath (Join-Path $Directory 'WslTools.dll')) { return $Directory }
    throw 'Extract the complete release package, or build the plugin first.'
}

function Get-Wsl2Distributions {
    # Unlike `wsl --list --running`, the registration includes stopped distros.
    # Read before any UAC prompt and keep all WSL launches in this user context.
    $Key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Lxss'
    if (-not (Test-Path -LiteralPath $Key)) { return }
    Get-ChildItem -LiteralPath $Key | ForEach-Object {
        $Entry = Get-ItemProperty -LiteralPath $_.PSPath
        # Version is the registry schema, not WSL 1/2. The VM-mode flag
        # identifies a WSL 2 registration (LXSS_DISTRO_FLAGS_VM_MODE).
        if ($Entry.PSObject.Properties['Flags'] -and ($Entry.Flags -band 8) -ne 0 -and
            $Entry.PSObject.Properties['DistributionName'] -and $Entry.DistributionName) {
            [string] $Entry.DistributionName
        }
    } | Sort-Object -Unique
}

function Write-SetupProgress([string] $Text, $ProgressQueue) {
    if ($ProgressQueue -is [string]) {
        # Inno Setup reads the worker log as UTF-16, including its BOM.
        # The wizard may briefly hold the file while refreshing its memo.
        for ($Attempt = 0; ; ++$Attempt) {
            try {
                [IO.File]::AppendAllText($ProgressQueue, $Text + [Environment]::NewLine, [Text.Encoding]::Unicode)
                break
            } catch [IO.IOException] {
                if ($Attempt -ge 4) { throw }
                Start-Sleep -Milliseconds 20
            }
        }
    }
    elseif ($null -ne $ProgressQueue) { $ProgressQueue.Enqueue($Text) }
    else { Write-Host $Text }
}

function Test-SetupCancelled($Cancellation) {
    $Cancelled = if ($Cancellation -is [string]) { [IO.File]::Exists($Cancellation) }
                 else { $null -ne $Cancellation -and $Cancellation.IsSet }
    if ($Cancelled) { throw 'Cancelled. Completed changes have been kept.' }
}

function ConvertTo-NativeArgument([string] $Value) {
    # Like the plugin transport, leave simple arguments unquoted: WSL parses
    # switches from the raw command line and treats quoted switches as commands.
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    # Windows PowerShell 5.1 lacks ProcessStartInfo.ArgumentList. Apply the
    # CommandLineToArgvW quoting rules, including backslashes before quotes.
    return '"' + [regex]::Replace([regex]::Replace($Value, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
}

function Invoke-WindowsFileAction([string] $Mode, [string] $Directory, [string] $DistributionDirectory, $ProgressQueue) {
    $Directory = [IO.Path]::GetFullPath($Directory)
    if (-not (Test-Path -LiteralPath (Join-Path $Directory 'SystemInformer.exe'))) {
        throw "Not a System Informer installation: $Directory"
    }
    # The child repeats the check, since a process could start after this one.
    Assert-SystemInformerClosed $Directory
    $Arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
        (Join-Path $PSScriptRoot 'setup-files.ps1'), '-Mode', $Mode,
        '-SystemInformerDirectory', $Directory)
    if ($Mode -eq 'Install') { $Arguments += @('-DistributionDirectory', $DistributionDirectory) }
    $Start = New-Object Diagnostics.ProcessStartInfo
    $Start.FileName = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $Start.Arguments = ($Arguments | ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' '
    $Start.UseShellExecute = $true
    $Start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    # Probe only our destination directory. Do not elevate the wizard or WSL.
    $ProbeDirectory = Join-Path $Directory 'plugins'
    if (-not [IO.Directory]::Exists($ProbeDirectory)) { $ProbeDirectory = $Directory }
    $Probe = Join-Path $ProbeDirectory ('.wsl-tools-access-' + [guid]::NewGuid().ToString('N'))
    try { $Stream = [IO.File]::Open($Probe, 'CreateNew', 'Write', 'None'); $Stream.Dispose(); [IO.File]::Delete($Probe) }
    catch [UnauthorizedAccessException] {
        $Start.Verb = 'runas'
        Write-SetupProgress 'Windows administrator permission is needed to change the plugin files.' $ProgressQueue
    }
    $Process = [Diagnostics.Process]::Start($Start)
    try {
        # This finite file-copy phase is not cancelled halfway through. It runs
        # on the wizard worker, so a UAC prompt never blocks window repainting.
        $Process.WaitForExit()
        if ($Process.ExitCode -ne 0) {
            throw "Windows file operation failed (code $($Process.ExitCode)). Close System Informer and retry; check access to the selected folder and release files."
        }
    } finally { $Process.Dispose() }
    Write-SetupProgress "Windows plugin files: $($Mode.ToLowerInvariant()) completed." $ProgressQueue
}

function Assert-SystemInformerClosed([string] $Directory) {
    if (-not ('WslSetup.NativeProcess' -as [type])) {
        # QueryLimitedInformation works across elevation without requesting the
        # memory-reading access used by Process.MainModule.
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
namespace WslSetup {
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
    $Expected = [IO.Path]::GetFullPath((Join-Path $Directory 'SystemInformer.exe'))
    foreach ($Process in @(Get-Process -Name SystemInformer -ErrorAction SilentlyContinue)) {
        try { $Path = [WslSetup.NativeProcess]::Path($Process.Id) }
        catch { throw 'A System Informer instance cannot be inspected. Close it before installing or uninstalling this plugin.' }
        if (-not $Path) { throw 'Close System Informer before installing or uninstalling this plugin.' }
        if ([string]::Equals($Path, $Expected, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Close System Informer in the selected folder, then retry. The installer will not close it for you.'
        }
    }
}

function Invoke-WslManagement([string[]] $Arguments, $Cancellation) {
    $Start = New-Object Diagnostics.ProcessStartInfo
    $Start.FileName = Join-Path $env:SystemRoot 'System32\wsl.exe'
    $Start.Arguments = ($Arguments | ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' '
    $Start.UseShellExecute = $false
    $Start.CreateNoWindow = $true
    $Start.RedirectStandardOutput = $true
    $Start.RedirectStandardError = $true
    # Pin WSL's redirected management output to its default UTF-16 format,
    # even when the caller opts into UTF-8 for their own terminals.
    $Start.EnvironmentVariables.Remove('WSL_UTF8')
    $Start.StandardOutputEncoding = [Text.Encoding]::Unicode
    $Start.StandardErrorEncoding = [Text.Encoding]::Unicode
    $Process = New-Object Diagnostics.Process
    $Process.StartInfo = $Start
    $Started = $false
    try {
        Test-SetupCancelled $Cancellation
        [void] $Process.Start()
        $Started = $true
        $Output = $Process.StandardOutput.ReadToEndAsync()
        $Errors = $Process.StandardError.ReadToEndAsync()
        $Watch = [Diagnostics.Stopwatch]::StartNew()
        # Include redirected-stream completion in the timeout as well.
        while (-not ($Process.HasExited -and $Output.IsCompleted -and $Errors.IsCompleted)) {
            Test-SetupCancelled $Cancellation
            if ($Watch.Elapsed.TotalSeconds -gt 15) { throw 'WSL management command did not respond within 15 seconds.' }
            Start-Sleep -Milliseconds 50
        }
        $Text = $Output.GetAwaiter().GetResult()
        $Diagnostic = $Errors.GetAwaiter().GetResult().Trim()
        if ($Process.ExitCode -ne 0) {
            if (-not $Diagnostic) { $Diagnostic = $Text.Trim() }
            throw "WSL management command exited with code $($Process.ExitCode): $Diagnostic"
        }
        return $Text
    } finally {
        try {
            if ($Started -and -not $Process.HasExited) {
                $Process.Kill()
                [void] $Process.WaitForExit(1000)
            }
        } finally { $Process.Dispose() }
    }
}

function Invoke-WslComponent([string] $Mode, [string] $Distro, [string] $Observer, $Cancellation) {
    Test-SetupCancelled $Cancellation
    try {
        $RunningText = Invoke-WslManagement @('--list', '--running', '--quiet') $Cancellation
        $RunningDistros = @($RunningText -split '\r?\n' | ForEach-Object { $_.Trim().Trim([char]0xFEFF) } | Where-Object { $_ })
        $RestoreStopped = $RunningDistros -notcontains $Distro
    } catch {
        throw "Could not determine whether $Distro is running; its companion was not changed. $($_.Exception.Message)"
    }
    $Script = @'
set -eu
umask 077
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
dir=/usr/local/lib/system-informer-wsl
target=$dir/wsl-observer
fail() { printf '%s\n' "$1" >&2; exit 1; }
safe_owner_mode() {
    [ "$(stat -c %u -- "$1")" = 0 ] || fail "Not owned by root: $1"
    mode=$(stat -c %a -- "$1")
    [ "$((0$mode & 022))" -eq 0 ] || fail "Writable by another account: $1"
}
[ "$(id -u)" = 0 ] || fail 'The WSL component requires root.'
for parent in / /usr /usr/local /usr/local/lib "$dir"; do
    [ ! -L "$parent" ] || fail "Path must not be a symlink: $parent"
    if [ ! -e "$parent" ]; then
        [ "$operation" != remove ] || exit 0
        mkdir -m 755 -- "$parent"
    fi
    [ -d "$parent" ] || fail "Not a directory: $parent"
    safe_owner_mode "$parent"
done
[ ! -L "$target" ] || fail 'The component must not be a symlink.'
if [ -e "$target" ]; then
    [ -f "$target" ] || fail 'The component must be a regular file.'
    safe_owner_mode "$target"
    [ "$(stat -c %h -- "$target")" = 1 ] || fail 'The component must not have hard links.'
fi
if [ "$operation" = remove ]; then
    # Remove just the product file. Never recursively delete a root directory.
    [ ! -e "$target" ] || rm -- "$target"
    rmdir -- "$dir" 2>/dev/null || :
    exit 0
fi
[ "$(uname -m)" = x86_64 ] || fail 'The bundled observer requires x86-64 WSL.'
temporary=$(mktemp "$dir/.observer.XXXXXX")
trap 'rm -f -- "$temporary"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
head -c "$count" > "$temporary"
[ "$(wc -c < "$temporary")" -eq "$count" ] || fail 'Incomplete component upload.'
actual=$(sha256sum -- "$temporary")
actual=${actual%% *}
[ "$actual" = "$expected" ] || fail 'Component SHA256 verification failed.'
chown 0:0 -- "$temporary"
chmod 700 -- "$temporary"
mv -fT -- "$temporary" "$target"
trap - EXIT HUP INT TERM
'@
    $InputFile = $null
    if ($Mode -eq 'Install') {
        $Length = (Get-Item -LiteralPath $Observer).Length
        if ($Length -le 0 -or $Length -gt 128MB) { throw 'The Linux observer has an invalid size.' }
        $Hash = (Get-FileHash -LiteralPath $Observer -Algorithm SHA256).Hash.ToLowerInvariant()
        # Only generated numbers and a hash enter the shell; the distro name is
        # a separate wsl.exe argument, never shell source.
        $Script = "operation=install; count=$Length; expected=$Hash`n" + $Script
        $InputFile = [IO.File]::OpenRead($Observer)
    } else { $Script = "operation=remove`n" + $Script }
    $Start = New-Object Diagnostics.ProcessStartInfo
    $Start.FileName = Join-Path $env:SystemRoot 'System32\wsl.exe'
    $Start.Arguments = (@('--distribution', $Distro, '--user', 'root', '--exec', '/bin/sh', '-c', $Script) |
        ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' '
    $Start.UseShellExecute = $false
    $Start.CreateNoWindow = $true
    $Start.RedirectStandardInput = $true
    $Start.RedirectStandardOutput = $true
    $Start.RedirectStandardError = $true
    $Process = New-Object Diagnostics.Process
    $Process.StartInfo = $Start
    $Started = $false
    $OperationError = $null
    $CleanupErrors = New-Object 'Collections.Generic.List[string]'
    try {
        Test-SetupCancelled $Cancellation
        [void] $Process.Start()
        $Started = $true
        $Output = $Process.StandardOutput.ReadToEndAsync()
        $Errors = $Process.StandardError.ReadToEndAsync()
        $Copy = $null
        if ($InputFile) { $Copy = $InputFile.CopyToAsync($Process.StandardInput.BaseStream) }
        else { $Process.StandardInput.Close() }
        $Watch = [Diagnostics.Stopwatch]::StartNew()
        $Closed = $null -eq $Copy
        while (-not $Process.WaitForExit(50)) {
            Test-SetupCancelled $Cancellation
            if ($Watch.Elapsed.TotalSeconds -gt 90) { throw 'WSL did not respond within 90 seconds.' }
            if (-not $Closed -and $Copy.IsCompleted) {
                $Copy.GetAwaiter().GetResult()
                $Process.StandardInput.Close()
                $Closed = $true
            }
        }
        if ($Process.ExitCode -ne 0) {
            $Diagnostic = $Errors.GetAwaiter().GetResult().Trim()
            if (-not $Diagnostic) { $Diagnostic = $Output.GetAwaiter().GetResult().Trim() }
            throw "WSL exited with code $($Process.ExitCode): $Diagnostic"
        }
        if ($Copy) { $Copy.GetAwaiter().GetResult() }
    } catch {
        $OperationError = $_.Exception.Message
    } finally {
        try {
            if ($Started -and -not $Process.HasExited) {
                # Let an incomplete upload run its cleanup trap before ending
                # this launcher. A distro that was already running stays up.
                try { $Process.StandardInput.Close() } catch {}
                if (-not $Process.WaitForExit(1000)) { $Process.Kill(); [void] $Process.WaitForExit(1000) }
            }
        } catch {
            $CleanupErrors.Add("Could not close the WSL command: $($_.Exception.Message)")
        } finally {
            try {
                if ($InputFile) { $InputFile.Dispose() }
                $Process.Dispose()
            } finally {
                if ($Started -and $RestoreStopped) {
                    try {
                        # Ignore cancellation during restoration: this distro was
                        # started only temporarily for the companion operation.
                        [void] (Invoke-WslManagement @('--terminate', $Distro) $null)
                    } catch {
                        $CleanupErrors.Add("Could not restore $Distro to its stopped state: $($_.Exception.Message)")
                    }
                }
            }
        }
    }
    if ($OperationError -or $CleanupErrors.Count) {
        throw ((@($OperationError) + @($CleanupErrors) | Where-Object { $_ }) -join [Environment]::NewLine)
    }
}

function Invoke-Setup([string] $Mode, [string] $Directory, [string] $DistributionDirectory,
    [bool] $Companions, [string[]] $Distros, $ProgressQueue, $Cancellation) {
    if ($Mode -eq 'Install') {
        $DistributionDirectory = Find-DistributionDirectory $DistributionDirectory
        foreach ($File in @('WslTools.dll', 'wsl-observer')) {
            if (-not (Test-Path -LiteralPath (Join-Path $DistributionDirectory $File))) { throw "Missing release file: $File" }
        }
    }
    Test-SetupCancelled $Cancellation
    Invoke-WindowsFileAction $Mode $Directory $DistributionDirectory $ProgressQueue
    $Failures = 0
    if ($Companions) {
        if (-not $Distros.Count) { Write-SetupProgress 'No WSL 2 distributions are registered for this Windows user.' $ProgressQueue }
        foreach ($Distro in $Distros) {
            Test-SetupCancelled $Cancellation
            Write-SetupProgress "$Distro`: $($Mode.ToLowerInvariant()) Linux companion (temporarily starts the distro if stopped)..." $ProgressQueue
            try {
                $Observer = if ($Mode -eq 'Install') { Join-Path $DistributionDirectory 'wsl-observer' } else { '' }
                Invoke-WslComponent $Mode $Distro $Observer $Cancellation
                Write-SetupProgress "$Distro`: completed." $ProgressQueue
            } catch {
                Write-SetupProgress "$Distro`: $($_.Exception.Message)" $ProgressQueue
                Test-SetupCancelled $Cancellation
                ++$Failures
            }
        }
    }
    if ($Failures) { throw "Windows files completed; $Failures WSL distribution(s) failed. See the results above, then retry if needed." }
    Write-SetupProgress 'Done. System Informer and plugin settings were preserved.' $ProgressQueue
    if ($Mode -eq 'Install') {
        Write-SetupProgress 'Enable plugins and set Advanced > EnableDefaultSafePlugins to 0 if required, then start System Informer.' $ProgressQueue
    }
}
