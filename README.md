# WSL Tools for System Informer

A native System Informer plugin for inspecting and controlling WSL 2 from a dedicated **WSL** tab. Written in C++ with a small C SDK bridge and a self-contained Linux observer. MIT licensed to David Trapp.

## What it does

- **Processes:** sortable CPU, resident memory, storage I/O rates, user, state, threads, parent PID and command line; process ancestry; CPU history; live filtering.
- **Process inspector:** a structured General page, raw diagnostics, open file descriptors, executable mappings, environment variables, threads/wait channels, native GDB stacks, runtime-aware Node.js/Python/Java stacks and per-process connections. Inspect cgroups, namespaces, capabilities, seccomp, resource limits and status counters.
- **Network:** TCP, UDP and Unix sockets, local/remote endpoints, listening-port filter, socket ownership and a shortcut to the owning process. Search by port, process name, PID or address.
- **Services:** loaded and installed systemd service units, startup state, structured properties, unit configuration and recent journal entries. Start, stop, restart, reload, enable and disable services.
- **Process controls:** SIGTERM, SIGKILL, SIGSTOP, SIGCONT, SIGUSR1, SIGUSR2, SIGHUP and SIGWINCH, with process-identity checks using pidfds. PID 1 and the observer itself are protected.
- **Desktop integration:** native light/dark controls, separate dialog/grid/monospace font roles, semantic row colors, keyboard navigation, filterable inspectors, copy/export, persistent column widths/order/sorting, and Explorer actions with per-distro path overrides.

The observer runs as **Linux root by default**. Windows administrator privileges are not needed for ordinary monitoring; installation into Program Files may require them. Only the selected, already-running WSL2 distribution is monitored. The plugin does not alter System Informer's main process tree or CPU totals.

## Install

The release folder contains `WslTools.dll` and `wsl-observer`. Keep these two files together in System Informer's `plugins` directory. The observer is the Linux-side component that collects data and performs requested actions.

1. Close System Informer yourself before replacing a loaded plugin.
2. Copy both files into its `plugins` directory (normally `C:\Program Files\SystemInformer\plugins`). Alternatively, run `scripts/install.ps1` from the checkout or `install.ps1` from the extracted package.
3. Enable plugins in System Informer's options. If the WSL tab does not appear, open the **Advanced** settings, find **EnableDefaultSafePlugins**, and set it to **0**. This setting controls whether third-party DLLs are discovered; it is not a WSL Tools setting. Restart System Informer after changing it.
4. Open **WSL** and choose a running distribution. If that distro does not have the Linux observer installed, the plugin shows an installation notice in place of the usual view. Choose **Install and retry** to install it as root, verify it, and reconnect. The first snapshot loads after installation succeeds.

The observer is installed at `/usr/local/lib/system-informer-wsl/wsl-observer` in each distro where it is used. There is no background daemon: the plugin launches the observer when it connects and shuts it down when the connection closes. On reconnect, the plugin checks its SHA-256 hash and automatically replaces an older or different observer with the version bundled beside the plugin. Installing into a distro requires `/bin/sh` and the standard utilities used by the bootstrap: `id`, `stat`, `mkdir`, `sha256sum`, `mktemp`, `rm`, `printf`, `head`, `wc`, `chown`, `chmod`, and `mv`. It does not use a distro package manager.

To remove the observer manually, first disconnect or close the WSL view, then run these commands for each distro where you installed it (replace `Ubuntu` with the exact distro name):

```powershell
wsl.exe --distribution Ubuntu --user root --exec rm -- /usr/local/lib/system-informer-wsl/wsl-observer
wsl.exe --distribution Ubuntu --user root --exec rmdir -- /usr/local/lib/system-informer-wsl
```

The second command succeeds only when that directory is empty. The plugin does not automatically uninstall the observer.

The installer copies only this plugin and observer. It does not reset System Informer settings, close processes, or change the default-plugin setting. Do not replace your installed System Informer executable.

### Compatibility

The Windows plugin and supplied observer target **x64 Windows + x86-64 WSL2**. WSL1 is excluded. The observer requires pidfd-capable Linux for signal controls (normal current WSL2 kernels provide this). Systemd features require systemd to be running inside the selected distro; other views still work without it.

The SDK is pinned to System Informer revision `bfc8145f2744a415319ccaaa8f1e32dd2cf1e596` for reproducible builds. The plugin has been loaded and exercised on **4.0.26255.346**. The small SDK interface used by the bridge was also compared with revision `ac083373839c9a63aeb84bc296ff55c0afb8184b`: its relevant exports, structures and callbacks match. That source comparison is not a runtime test of the newer executable. There is **no exact-version runtime restriction**. Future ABI changes may require rebuilding or adapting `plugin/entry.c`.

## Using the views

The inner views are **Processes · Services · Network**. When ToolStatus and its search box are enabled, the normal toolbar search filters the active WSL view using System Informer's search rules. Without it, a local filter is available. Process inspector tabs are **General · Threads · Modules · Environment · Handles · Network · Stacks**, then a runtime-specific **JS Stacks**, **Python Stacks**, or **Java Stacks** tab when the executable is recognized, followed by **Details**. Service inspectors have **General · Details**. Table pages keep their own filters.

**Processes:** By default, process CPU follows the Windows convention: **100% = all WSL vCPUs**. In **WSL → Settings**, you can switch to Linux's convention, where **100% means one fully occupied virtual CPU**. The registry setting is `CpuPercentOfTotal` (`REG_DWORD`): `1` is the default total-WSL-capacity scale and `0` selects the one-vCPU scale. The summary and CPU graph always divide the selected distro's visible process total by the guest CPU count, regardless of the process-column setting. Memory available/total is VM-wide because WSL distributions share a kernel; RSS is per process and includes shared pages. Read/write rates are Linux storage accounting, not network or every buffered read/write operation. Bordered CPU and VM-memory graphs remain visible and keep updating across Processes, Services and Network.

**Network:** enter a port such as `3000`, or combine terms such as `node 3000`. Space-separated search terms are ANDed across visible row values. **Listening / bound ports only** includes TCP listeners and unconnected UDP endpoints. Unix sockets are included. Double-click an owned socket to switch to Processes and select its owner; press Enter again to inspect it. PID 0 means no owner was visible; it is not a Windows PID.

**Services:** inspect a service for structured properties, full diagnostics, its unit file, and the last 100 journal entries. Actions have explicit confirmations and run against the system manager as root, not a user's `systemctl --user` manager. Enabling a service and starting it are separate operations.

**Native stacks:** on Threads, select a thread and choose View stack, or open Stacks and press Capture all stacks (Ctrl+R captures the selected stack page). The page explains that GDB is required and the target pauses while attached. Merely opening the tab does not attach. Captures run directly without an extra confirmation dialog. GDB startup scripts, automatic script loading, debuginfod and index-cache writes are disabled. If malformed or missing DWARF causes a symbol-reader failure, a second capture uses minimal symbols, retaining available exported function names, libraries, and module offsets. Original diagnostics remain visible. Ptrace restrictions or missing symbols can limit the result.

**Runtime stacks:** the inspector checks the resolved executable name, not the command line. It recognizes Node.js executables named `node` or `nodejs`, CPython names such as `python`, `python2`, `python3`, and versioned/debug/free-threaded `python2.x` or `python3.x` names, plus an executable named `java`. A refresh of process details can add, change, or remove the matching runtime tab if the process has executed a different program. Shell or npm wrappers, PyPy, renamed executables, and embedded runtimes are not detected automatically. The tab opens with instructions for its capture button, possible pause, and required tool; opening it does not attach. Captures run without a separate confirmation dialog. When no Inspector listener exists, the Node-specific dialog offers **Enable Inspector**, **Use llnode**, or **Cancel** if Python 3 is available. Without Python 3, it explains the requirement and still offers **Use llnode** or **Cancel**. This is a backend choice, not a confirmation step. The plugin never installs debugger tools.

Node.js defaults to its Inspector when the selected process already owns a verifiable listener. The plugin finds the listener from that process's sockets and checks the Inspector-reported PID before reading a stack, so it will not accidentally attach to another process. Inspector capture shows the main JavaScript thread only; it does not include worker threads, native frames, or asynchronous promise/task history. If no listener exists and you choose **Enable Inspector**, the plugin activates it with SIGUSR1 using a pidfd, then resumes only the pause it requested. It disconnects its own Inspector session after capture. If this capture enabled the listener and verified the session, it requests closure and checks the result. It leaves pre-existing Inspectors enabled and preserves a pause that was already in effect. If another client is detected, the listener is left enabled. A new client can still race that check, so avoid concurrent debugger sessions during temporary activation. A cleanup warning explains when closure could not be verified. When asked to enable Inspector, the plugin declines if command-line or `NODE_OPTIONS` settings identify a public or unverified bind address. An Inspector that was already running is left as configured, so keep existing listeners on loopback too. Inspector access permits code execution as the Node process user, and a loopback listener is still reachable by local programs in that distro. See Node's [Inspector API](https://nodejs.org/api/inspector.html), [Inspector options](https://nodejs.org/api/cli.html#--inspectporthostport), [debugging guide](https://nodejs.org/learn/getting-started/debugging), and [security guidance](https://nodejs.org/learn/getting-started/debugging#security-implications).

Each Node process must own its own Inspector port. If multiple processes both request the default port `9229`, the second listener can fail to start; the plugin still verifies socket ownership and PID instead of capturing the first process by mistake. Use `--inspect-port=0` to let Node choose an available port when enabling Inspector in multiple processes. The embedded client uses the distro's system Python 3 standard library with isolated `-I -S` flags; it needs no Python packages and ships inside `wsl-observer`, so there is no separate script to deploy. System Python 3 is required for Inspector capture; llnode remains available as the alternate backend.

Java capture can pause threads at a JVM safepoint. The helper allows up to 15 seconds and 512 KiB of output (Node Inspector has a bounded capture window and cleanup interval). LLDB/llnode captures OS-thread stacks, up to 256 threads and 64 frames per thread. Python uses `py-spy` to collect all Python thread stacks without local variable values. Java uses `jcmd` from the target JVM's matching JDK to run `Thread.print -l` and include locks; traditional thread dumps do not cover every unmounted virtual thread. Ptrace restrictions, disabled JVM attach, runtime/tool version mismatches, or missing debugging metadata can prevent or limit a capture.

Install the tools yourself in the selected distro. For Python, place a trusted, root-owned `py-spy` executable in `/usr/local/bin` or `/usr/bin`; you can build/install it as your normal user and have an administrator copy the binary. See the [py-spy project](https://github.com/benfred/py-spy). For Java, install the JDK matching the target JVM and keep its `jcmd` beside that JVM's `bin/java`; a `jcmd` from another JDK version is not interchangeable. See Oracle's [`jcmd` reference](https://docs.oracle.com/en/java/javase/21/docs/specs/man/jcmd.html).

For the optional Node.js fallback, install LLDB and build `llnode` for that LLDB version. Build it as a normal user (the upstream instructions use `npm install llnode`), then have an administrator copy `llnode.so` to a root-owned system location that is not group/world writable. Checked locations include `/usr/local/lib/llnode/llnode.so`, `/usr/lib/lldb/plugins/llnode.so`, `/usr/local/lib/node_modules/llnode/llnode.so`, and `/usr/lib/node_modules/llnode/llnode.so`. Do not run npm with `sudo`; see the [llnode install instructions](https://github.com/nodejs/llnode#install-instructions). LLDB/llnode version support varies with Node/V8. In particular, LLDB 18 with llnode 4 does not reliably decode JavaScript names for Node.js 22, so fallback output may contain partial V8 data or native addresses instead of useful JS frames.

**Inspectors:** use the normal field layout in General; use Details for full diagnostics in a fixed-width font. Expensive details are requested on demand. A process that exits is reported as unavailable; the inspector never silently follows its reused PID. Environment variables are live sensitive data, so copy/export them deliberately.

**Colors:** purple marks root processes, amber marks stopped processes or masked services, red marks zombie/dead processes, failed services or writable executable mappings, green marks active services, and blue marks listening sockets. Inactive services use muted text. Selection uses the native control's selection colors.

### Keyboard shortcuts

| View | Shortcut | Action |
| --- | --- | --- |
| Main views | Ctrl+K | Standard ToolStatus toolbar search |
| Main views without ToolStatus search | Ctrl+F | Focus local filter |
| Main views | F5 | Rediscover/reconnect and refresh |
| Selected row | Enter | Inspect resource; Network goes to its process |
| Selected row | Ctrl+C | Copy row |
| Process row | Delete | Offer SIGTERM confirmation |
| Inspector | Ctrl+R | Refresh current inspection |
| Inspector | Ctrl+F | Focus table filter |
| Inspector | Ctrl+C / Ctrl+S | Copy / export |
| Inspector | Ctrl+Shift+C | Copy environment value or file/module path |
| Inspector | Ctrl+Tab | Next page |
| Inspector/settings | Escape | Close window |

**Pause** disconnects the main collector and leaves a snapshot visible. Switching away from the WSL main tab also disconnects it. An explicit inspector refresh can open a connection independently. Exiting System Informer closes all observer connections.

## Explorer path overrides

Open **WSL → Settings** for the selected distro, or configure a registry string value under:

```text
HKEY_CURRENT_USER\Software\David Trapp\System Informer WSL Plugin\PathOverrides
```

The value name is the exact distribution name; its `REG_SZ` value is the Windows prefix for `/`.

For example, to map an Ubuntu distro's Linux paths to drive `R:`:

```powershell
$key = 'HKCU:\Software\David Trapp\System Informer WSL Plugin\PathOverrides'
New-Item -Path $key -Force | Out-Null
New-ItemProperty -Path $key -Name Ubuntu -PropertyType String -Value 'R:\' -Force
```

Thus `/usr/bin/bash` opens as `R:\usr\bin\bash` instead of `\\wsl.localhost\Ubuntu\usr\bin\bash`. Replace `R:\` with a drive prefix available on your system. Without an override the plugin uses `\\wsl.localhost\<distro>\`. Overrides affect Explorer navigation only; all inspection and actions still happen inside the correct distro. Deleted files, sockets, pipes and Linux-only paths that Windows cannot represent are not opened as ordinary files.

Column widths, display order and the sort column/direction are saved independently for each table under `Table.*` values in the plugin registry key. Widths are stored in DPI-independent units.

Settings are independent of System Informer's normal configuration. The refresh interval is `RefreshInterval` (`REG_DWORD`, decimal milliseconds, 500–60000) in the parent registry key; default is 2000.

`CpuPercentOfTotal` (`REG_DWORD`, `0` or `1`) controls process-row CPU percentages as described above; default is `1`.

## Build

Requirements: a Linux C++17 compiler with static C/C++ libraries, Windows Visual Studio 2022's **Desktop development with C++**, the Windows SDK, CMake (the VS component is supported), and PowerShell. No Node.js, Python, .NET application runtime or Linux package install is needed to run the plugin.

From the Linux checkout:

```sh
./scripts/build-helper.sh
```

From Windows PowerShell, using the Windows-visible checkout path:

```powershell
.\scripts\build-windows.ps1
.\scripts\package.ps1
```

The first Windows build downloads the pinned upstream SDK source and checks its archive SHA-256. It derives public headers and the import library without building System Informer. Later builds can use `-SkipSdk`. Every Windows build still cleans native objects: this prevents stale header layouts across the WSL/Windows staging boundary. Matching PDB symbols are retained for crash diagnosis. Windows compilation is staged under `%LOCALAPPDATA%\WslTools\build`; `-BuildRoot` can select another staging directory.

`dist/` contains the DLL, observer and ZIP package; generated files and `.deps/` are ignored by Git. `scripts/build-helper.sh` refuses to silently publish a dynamically linked observer. The optional `CXX` environment variable selects another existing Linux compiler. The current Windows build target is x64, even though the helper source can also compile on ARM64.

## Architecture and limits

See [architecture](docs/architecture.md) and the [wire protocol](docs/protocol.md).

- This is sampling, not an audit trail. Very short-lived processes and sockets can disappear between snapshots.
- Socket tables cover the observer's current network namespace. Processes in this distro's PID namespace are considered for ownership; other distributions or private container network namespaces can leave sockets unowned or invisible. The UI reports incomplete ownership rather than inventing a PID.
- There is no per-process network-byte accounting, GPU accounting, interactive debugger, memory editor, remote shell, or custom Windows driver. GDB is used only for explicitly requested stack captures.
- RSS/PSS and VM memory use different accounting. Guest CPU is not subtracted from `vmmemWSL`.
- Collector startup uses WSL's public command interface and a running-state check. A distribution stopping between the check and launch can still race with startup. An open collector can affect WSL idle shutdown. Pausing/disconnecting releases it; the plugin never issues `wsl --shutdown` or terminates a distro.
- Large collections have explicit limits, timeouts and truncation notices. Inspection errors preserve the last snapshot and label it stale; transport failures require reconnection.
- Service actions can legitimately restart or stop components you depend on. SIGUSR1/2 are program-defined and terminate a process if it has no handler. The plugin names the exact action before asking for confirmation.

## Licensing

Original project code: [MIT, David Trapp](LICENSE). The vendored JSON library is nlohmann/json 3.11.3 under its [MIT license](vendor/json.LICENSE.MIT). System Informer's SDK is fetched separately and retains its upstream license. See [third-party notices](THIRD_PARTY_NOTICES.md).
