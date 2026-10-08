# WSL Tools for System Informer

A native System Informer plugin for inspecting and controlling WSL 2 from a dedicated **WSL** tab. Written in C++ with a small C SDK bridge and a self-contained Linux observer. MIT licensed to David Trapp.

## What it does

- **Processes:** sortable CPU, resident memory, storage I/O rates, user, state, threads, parent PID and command line; optional Linux scheduling, identity, memory and I/O columns; process tree; CPU history; live filtering.
- **Process inspector:** a structured General page, raw diagnostics, handles, verified ELF modules, all virtual memory mappings, environment variables, threads/wait channels, native GDB stacks, runtime-aware Node.js/Python/Java stacks and per-process connections. Inspect cgroups, namespaces, capabilities, seccomp, resource limits and status counters.
- **Network:** TCP, UDP and Unix sockets, local/remote endpoints, listening-port filter, socket ownership and a shortcut to the owning process. Search by port, process name, PID or address.
- **Services:** loaded and installed systemd service units, startup state, structured properties, unit configuration and recent journal entries. Start, stop, restart, reload, enable and disable services.
- **Process controls:** SIGTERM, SIGKILL, SIGSTOP, SIGCONT, SIGUSR1, SIGUSR2, SIGHUP and SIGWINCH, with process-identity checks using pidfds. PID 1 and the observer itself are protected.
- **Find handles:** search open descriptors, executable paths, working directories and mapped files across the selected distro, then open the owning process or file location.
- **Desktop integration:** native light/dark controls, separate dialog/grid/monospace font roles, System Informer row-highlighting colors and lifecycle highlights, keyboard navigation, filterable inspectors, copy/export, persistent column visibility/widths/order/sorting, and Explorer actions with per-distro path overrides.

The observer runs as **Linux root by default**. Windows administrator privileges are not needed for ordinary monitoring; installation into Program Files may require them. Only the selected, already-running WSL2 distribution is monitored. The plugin does not alter System Informer's main process tree or CPU totals.

## Install

The release includes `WslTools.dll` and `wsl-observer`. To install or update the plugin, first close System Informer, then run `setup.cmd` and choose the System Informer folder. The companion checkbox starts checked for all registered WSL 2 distributions, including stopped ones. Installing the Linux component starts a stopped distro and leaves it running; the installer does not stop a distro afterward. The Windows file-copy step requests administrator permission only when the chosen System Informer folder needs it. WSL operations remain in your Windows user context and run the companion as Linux root.

Run `uninstall.cmd`, or choose **Uninstall** in the setup window, to remove the plugin. It removes the plugin files and, while the companion checkbox is checked, removes the companion from the registered WSL 2 distros. Uncheck it to keep the Linux component. The setup process does not replace `SystemInformer.exe` or reset System Informer settings. Command-line alternatives are `install.ps1` (Windows plugin files only by default; add `-InstallCompanions` to install into all WSL 2 distros) and `uninstall.ps1` (Windows files only by default; add `-RemoveCompanions` to remove the Linux component too). In a source checkout these scripts are under `scripts/`. Manual file copying into `plugins` is also supported.

Enable plugins in System Informer's options. If the WSL tab does not appear, open **Advanced** settings, find **EnableDefaultSafePlugins**, and set it to **0**. This controls whether third-party DLLs are discovered; it is not a WSL Tools setting. Restart System Informer after changing it.

Open **WSL** and choose a running distribution. If that distro does not have the observer installed, the plugin shows an installation notice in place of the normal view. Choose **Install and retry** to install it as root, verify it, and reconnect.

The companion resides at `/usr/local/lib/system-informer-wsl/wsl-observer`. It is a short-lived helper, not a daemon. On connection, the plugin checks its SHA-256 against the bundled helper and atomically updates an installed copy when it differs. A missing copy requires the install action described above. It does not use a distro package manager. Installation needs `/bin/sh` and standard utilities including `id`, `stat`, `mkdir`, `sha256sum`, `mktemp`, `rm`, `printf`, `head`, `wc`, `chown`, `chmod`, and `mv`; the installer also uses `uname` and `rmdir`. Removal is optional; to remove it manually, disconnect the WSL view and run these commands for each distro (replace `Ubuntu` with the exact distro name):

```powershell
wsl.exe --distribution Ubuntu --user root --exec rm -- /usr/local/lib/system-informer-wsl/wsl-observer
wsl.exe --distribution Ubuntu --user root --exec rmdir -- /usr/local/lib/system-informer-wsl
```

The second command succeeds only if the directory is empty. System Informer settings are preserved, and neither the installer nor plugin replaces the original System Informer executable.

### Compatibility

The Windows plugin and supplied observer target **x64 Windows + x86-64 WSL2**. WSL1 is excluded. The observer requires pidfd-capable Linux for signal controls (normal current WSL2 kernels provide this). Systemd features require systemd to be running inside the selected distro; other views still work without it.

The SDK is pinned to System Informer revision `bfc8145f2744a415319ccaaa8f1e32dd2cf1e596` for reproducible builds. The plugin has been loaded and exercised on **4.0.26255.346**. The small SDK interface used by the bridge was also compared with revision `ac083373839c9a63aeb84bc296ff55c0afb8184b`: its relevant exports, structures and callbacks match. That source comparison is not a runtime test of the newer executable. There is **no exact-version runtime restriction**. Future ABI changes may require rebuilding or adapting `plugin/entry.c`.

## Using the views

The inner views are **Processes · Services · Network**. The top strip holds the distro picker, **Settings**, **Export view**, **Find handles**, and the active view's option (**Show process tree** or the network listener filter). Search uses the normal System Informer toolbar when available; otherwise a local filter appears in the view. The Settings button opens System Informer **Options → WSL**, where you can change CPU display mode, optional 32-bit detection, the Node Inspector preference, and a distro's Explorer prefix. Select a row and press Enter, double-click it, or use its context menu to inspect or act on it; there is no separate bottom Inspect/Actions row.

Process inspector tabs are **General · Threads · Modules · Memory · Environment · Handles · Network · Stacks**, then a runtime-specific **JS Stacks**, **Python Stacks**, or **Java Stacks** tab when the executable is recognized, followed by **Details**. Service inspectors have **General · Journal · Details**. Table pages keep their own filters.

WSL monitoring follows System Informer's global **View → Refresh automatically** setting and refresh interval. Use **View → Refresh** or F5 to rediscover running distributions and refresh the selected view, including when automatic updates are off. There is no separate WSL Refresh or Pause button. While the WSL tab is hidden, or System Informer is minimized or hidden, lightweight process samples keep graph history and process identities current. Previously loaded network collections continue tracking socket identities. Services are polled only while the Services subtab is visible in the active WSL tab and System Informer is neither minimized nor hidden; reopening that view refreshes it immediately. Installed unit-file/startup metadata is cached for 30 seconds, with manual refresh, reopening Services, and successful enable/disable actions bypassing the cache. Extra process metadata is requested for visible or sorted columns, enabled highlighting, applicable filters, and service-unit tooltips when the process view is visible. Returning to the view immediately refreshes that metadata. Turning automatic refresh off also pauses background monitoring. The main view lists only running WSL 2 distributions; the Options and setup dialogs can list stopped WSL 2 registrations too.

**Processes:** By default, process CPU follows the Windows convention: **100% = all WSL vCPUs**. In System Informer **Options → WSL**, switch to Linux's convention if preferred: **100% = one fully occupied vCPU**. The registry setting is `CpuPercentOfTotal` (`REG_DWORD`): `1` is the default total-WSL-capacity scale and `0` selects the one-vCPU scale. The summary and CPU graph always divide the selected distro's collected process total by the guest CPU count, regardless of the process-column setting. Memory available/total is VM-wide because WSL distributions share a kernel; RSS is per process and includes shared pages. Read/write rates are Linux storage accounting, not network or every buffered read/write operation.

Right-click a column header for **Size column to fit**, **Size all columns to fit**, **Hide column**, **Reset sort**, or **Choose columns...**. Additional process columns include UID/EUID/GID/EGID, TTY, niceness, scheduling priority/policy, relative start time, virtual memory, swap, session/process group, last CPU, page faults, executable and working-directory paths, cgroup, tracer PID, cumulative I/O, read/write calls, CPU times, context switches, seccomp and no-new-privileges. Hidden metadata is not continuously collected unless needed for sorting, an enabled color rule, an applicable filter, or service-unit tooltips; search uses the collected data. **Show process tree** retains the selected process and tries to center it after changing the order.

**Detect 32-bit processes** in **Options → WSL** is off by default. When enabled, the Architecture column or enabled 32-bit-process color can request the executable's ELF class. An inaccessible or non-ELF executable remains unknown. Numbers use your Windows number locale. CPU follows the host decimal precision (`MaxPrecisionUnit`, normally two); memory and I/O sizes automatically choose 1024-based B, kB, MB, GB and larger units, with two decimals above bytes. Zero CPU and zero-byte I/O rates are blank. Tiny nonzero CPU is also blank unless **View → Processes → Show CPU below 0.01%** is enabled; its cutoff follows the host precision. Numeric sorting always uses the original values, even when a cell is blank or its displayed unit changes.

The process view also follows **Hide processes from other users** (relative to the distro's configured default user), **Scroll to new processes**, and **Sort child processes** / **Sort root processes** when Show process tree is enabled. Windows' signed/system-process filters depend on Windows image-signature semantics and are not applied to Linux processes. The host's Collapse/Expand and column-set commands do not control this separate grid.

Hover the main column for a structured process, service, or connection tooltip. Process tips identify the innermost systemd service unit, including whether it belongs to the system or a user manager. Tips use collected row data and respect the host's tooltip, command-line-tooltip, and instant-tooltip settings. Header text uses normal window text; header hover uses the native light-blue highlight in light mode.

**Find handles:** enter a path substring, process-name substring, or exact PID and choose **Search**. ASCII letters match without case sensitivity. Results include open descriptors, working/root directories, the executable and file-backed mappings. Executable mappings in search results do not imply the file has been verified as ELF; the process inspector's Modules tab performs that check. Double-click or press Enter for process properties; the context menu also offers file-location and copy actions. Searches have a five-second scan deadline, a 10,000-result limit and an 8 MB response budget; a notice indicates partial results. **Cancel** immediately discards the search result and restores the dialog. An already-running scan finishes its limited request without interrupting other inspectors. No `lsof` installation is required.

CPU and VM-memory graphs stay visible and update across Processes, Services and Network. Hover a sample for its local timestamp, sample interval, and detailed values: CPU of total WSL capacity and one vCPU, process count, highest-CPU process, or memory percent plus exact used/available/total bytes and the largest-RSS process in that sample. The graphs use System Informer host colors and a scrolling time grid.

**Network:** enter a port such as `3000`, or combine terms such as `node 3000`. Space-separated search terms are ANDed across visible row values. **Listening / bound ports only** includes TCP listeners and unconnected UDP endpoints. Unix sockets are included. Double-click an owned socket to switch to Processes and select its owner; press Enter again to inspect it. PID 0 means no owner was visible; it is not a Windows PID. **View → Network → Hide waiting connections** hides ownerless entries and TCP `CLOSE_WAIT` in both main and process-detail network tables. The **Remote hostname** column follows the host's resolve-addresses setting and is resolved only while that column is visible in the main Network view. Hiding it stops lookups even if it remains the sort column. Lookups use the distro's name service, are cached, and leave the numeric endpoint usable when a name is unavailable.

**Services:** inspect a service for structured properties, full diagnostics, its unit file, and a separate **Journal** tab showing the last 100 journal entries. The PID column shows the current main process when known; **Go to process** in the context menu selects that process in Processes. Enter and double-click still open the service inspector. Actions have explicit confirmations and run against the system manager as root, not a user's `systemctl --user` manager. Enabling a service and starting it are separate operations.

**Native stacks:** on Threads, select a thread and choose View stack, or open Stacks and press Capture all stacks (Ctrl+R captures the selected stack page). The initial page explains the capture button, that GDB is required, and that the target pauses while attached. Opening the page does not attach, and capture uses no confirmation dialog. GDB startup scripts, automatic script loading, debuginfod and index-cache writes are disabled. If malformed or missing DWARF causes a symbol-reader failure, a second capture uses minimal symbols, retaining available exported function names, libraries, and module offsets. Both ordinary and fallback captures annotate frames with their mapped module and file offset when that mapping is unchanged across capture, replacing `??` where possible. GDB-provided symbols and source locations are retained; no symbol name is invented. Original diagnostics remain visible. Ptrace restrictions or missing symbols can limit the result.

**Runtime stacks:** the inspector checks the resolved executable name, not the command line. It recognizes Node.js executables named `node` or `nodejs`, CPython names such as `python`, `python2`, `python3`, and versioned/debug/free-threaded `python2.x` or `python3.x` names, plus an executable named `java`. Refreshing process details can add, change, or remove the runtime tab after an `exec()` changes the program. Shell/npm wrappers, PyPy, renamed executables and embedded runtimes are not auto-detected. Each stack page opens with instructions about its capture button, pause behavior and required tool. Captures are explicit button/Ctrl+R actions without a confirmation dialog. The only capture-method choice dialog is for Node with no verified Inspector listener; its heading is **Node Inspector is not enabled for this process**.

Node.js first looks for a valid Inspector listener owned by the selected process and verifies that the PID returned by its WebSocket endpoint matches the process's innermost `NSpid`. Ordinary application sockets are ignored. It captures the main JavaScript thread and up to 32 reported worker contexts, sequentially, with up to 256 JavaScript frames per context. Each context gets at most one second to reach a JavaScript pause point; idle threads are skipped, and pending pauses are canceled so they do not unexpectedly stop the process later. Native frames and asynchronous promise/task history are not included. Threads are sampled at different times, not as one simultaneous snapshot.

If no Inspector is listening, choose **Temporarily Enable Inspector**, **Use llnode**, or **Cancel**. The Inspector path uses SIGUSR1 through a pidfd after verifying the selected Node process. It refuses automatic activation when recognizable command-line or `NODE_OPTIONS` settings request a public or unverified bind address. Existing listeners are not reconfigured. Before activation, the plugin records the process's listener identities; it closes only a listener newly created by this capture, and preserves existing pauses. If another client is present when cleanup is checked, the Inspector is left enabled. A connection can still race that check, and Node may disconnect clients when closing the listener, so avoid concurrent debugger sessions. If closure cannot be confirmed, the warning is kept with the stack; the plugin will not attach LLDB while cleanup is uncertain. Node's [Inspector API](https://nodejs.org/api/inspector.html), [Inspector options](https://nodejs.org/api/cli.html#--inspectporthostport), [debugging guide](https://nodejs.org/learn/getting-started/debugging), and [security guidance](https://nodejs.org/learn/getting-started/debugging#security-implications) explain the underlying behavior. Inspector access permits code execution as the Node process user; loopback is still reachable by local programs in the distro.

When Inspector activation is available, the choice dialog has **Don't show again**. Checking it and choosing **Temporarily Enable Inspector** saves `UseNodeInspectorWithoutAsking=1` and tries this backend automatically on later captures. The same preference appears in **System Informer Options → WSL** as **Use Node Inspector for JS stacks without asking**. Python 3 is required by the embedded Inspector client; if it is missing, the dialog explains this and offers only llnode and Cancel, even when the remember preference is enabled. If an Inspector attempt fails but cleanup is confirmed safe, the plugin tries llnode automatically and keeps the Inspector error beside the fallback. If cleanup is uncertain, it shows the Inspector failure without starting a second debugger. Use **Use llnode** to choose that backend directly.

Each Node process must own its Inspector port. If multiple processes request the default `9229`, a later listener may fail, but the plugin checks ownership and PID instead of attaching to the wrong process. Use `--inspect-port=0` to let Node choose a separate available port. The Inspector client is embedded in `wsl-observer` and runs with system Python 3 using isolated `-I -S` flags; it needs no Python packages or separate script deployment. Without Python 3, the choice dialog explains that Inspector is unavailable and still offers llnode.

Java capture can pause threads at a JVM safepoint. The helper allows up to 15 seconds and 512 KiB of output (Node Inspector has a bounded capture window and cleanup interval). LLDB/llnode captures OS-thread stacks, up to 256 threads and 64 frames per thread. Python uses `py-spy` to collect all Python thread stacks without local variable values. Java uses `jcmd` from the target JVM's matching JDK to run `Thread.print -l` and include locks; traditional thread dumps do not cover every unmounted virtual thread. Ptrace restrictions, disabled JVM attach, runtime/tool version mismatches, or missing debugging metadata can prevent or limit a capture.

Install the tools yourself in the selected distro. For Python, place a trusted, root-owned `py-spy` executable in `/usr/local/bin` or `/usr/bin`; you can build/install it as your normal user and have an administrator copy the binary. See the [py-spy project](https://github.com/benfred/py-spy). For Java, install the JDK matching the target JVM and keep its `jcmd` beside that JVM's `bin/java`; a `jcmd` from another JDK version is not interchangeable. See Oracle's [`jcmd` reference](https://docs.oracle.com/en/java/javase/21/docs/specs/man/jcmd.html).

For the optional Node.js fallback, install LLDB and build `llnode` for that LLDB version. Build it as a normal user (the upstream instructions use `npm install llnode`), then have an administrator copy `llnode.so` to a root-owned system location that is not group/world writable. Checked locations include `/usr/local/lib/llnode/llnode.so`, `/usr/lib/lldb/plugins/llnode.so`, `/usr/local/lib/node_modules/llnode/llnode.so`, and `/usr/lib/node_modules/llnode/llnode.so`. Do not run npm with `sudo`; see the [llnode install instructions](https://github.com/nodejs/llnode#install-instructions). LLDB/llnode version support varies with Node/V8. In particular, LLDB 18 with llnode 4 does not reliably decode JavaScript names for Node.js 22, so fallback output may contain partial V8 data or native addresses instead of useful JS frames.

**Inspectors:** use the structured fields in General and Details for full diagnostics in a fixed-width font. Modules lists verified ELF executables and shared objects, grouped by device/inode. Memory lists every mapped virtual-memory area (VMA), including anonymous/JIT mappings and files that are not verified ELF modules. A classification note explains files that could not be verified; those remain in Memory. Sizes in Modules sum mapped segments and exclude gaps. A deleted mapped file is still shown with its deleted marker. Expensive details are requested on demand. A process that exits is reported as unavailable; the inspector never silently follows its reused PID. Environment variables are live sensitive data, so copy/export them deliberately.

**Colors:** WSL tables use enabled colors from **Options → Highlighting**, with the applicable native priority. New and removed rows use the host lifecycle colors and duration ahead of semantic colors. Hover shades the existing row color instead of replacing it with a blue background. Process priority is debugged (`TracerPid`), fully stopped, partially stopped, background (no controlling TTY), filtered (`wsl-observer`), elevated, 32-bit, system, own, then service. Elevated means effective UID 0 with a nonzero numeric `SUDO_UID`; other root processes use System processes. Own processes match the distro's configured default user's effective UID. Service processes have a `.service` cgroup component, including user services. Root/service processes may therefore use an earlier enabled category's color. The observer uses the enabled Filtered processes color at its normal priority; an earlier enabled rule can still take precedence.

Linux-specific color meanings are approximations where the operating systems differ:

- Thread wait-channel names identify stopped threads, sleeps, futex waits, event/message queues, completion/I/O waits and user-request waits. Unknown wait sites stay unclassified.
- Environment colors compare exact name/value pairs against literal `/etc/environment` values, then the account's `USER`, `LOGNAME`, `HOME` and `SHELL`. Other values use Process environment. These comparisons describe matching baselines, not a proven inheritance history.
- Memory uses Execute pages for executable mappings, System pages for kernel-provided mappings such as `[vdso]` and `[vvar]`, and Private pages for private anonymous/heap/stack mappings. Linux provides no direct Windows CFG-page equivalent, so that color is not guessed.
- Modules use Known DLLs for libraries under standard `/lib*` and `/usr/lib*` paths and Native modules for recognized dynamic loaders there. The main executable is bold. ELF ASLR is not treated as Windows relocated-module detection; ordinary file mappings stay in Memory rather than receiving a guessed module category.
- Inherited handles means `O_CLOEXEC` is absent. Linux normally inherits descriptors across `fork` regardless; this color describes whether they can survive `exec`. Flags show symbolic names plus their hexadecimal value.
- Network uses Unknown process when no owner PID is known. Stopped disabled/masked services use Disabled services; other stopped services can use the host stopped-service text color.

The first snapshot establishes a baseline without new-row highlights. Search/filter changes do not count as removals. A row is marked removed only after a complete collection, remains copyable during the host highlight period, and is unavailable to actions. A partial or inaccessible collection retains uncertain old rows instead of declaring them removed.

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

## Explorer path overrides

Use the WSL view's **Settings** button to open **System Informer Options → WSL** for CPU display mode, the Node Inspector preference, and a distro path prefix. You can also configure a registry string value under:

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

Standard WSL Windows-drive mounts are recognized before the distro prefix: `/mnt/c` opens as `C:\`, and `/mnt/c/Users` as `C:\Users`, regardless of the prefix configured for other Linux paths. The mount must be exactly `/mnt/<letter>` or followed by `/`; a path like `/mnt/cfoo` does not match and uses the normal per-distro prefix.

Column visibility, widths, display order and the sort column/direction are saved independently for each table under `Table.*` values in the plugin registry key. Widths are stored in DPI-independent units.

The refresh interval and automatic-update choice come from System Informer's main View settings; WSL Tools has no separate refresh-interval or pause setting. `CpuPercentOfTotal` (`REG_DWORD`, `0` or `1`) controls process-row CPU percentages; default is `1`. `UseNodeInspectorWithoutAsking` (`REG_DWORD`, `0` or `1`) controls whether future Node.js stack captures skip the backend-choice dialog and try Inspector directly; it defaults to `0`. `Detect32BitProcesses` (`REG_DWORD`, `0` or `1`) enables optional ELF-class reads; it defaults to `0`.

## Build

Requirements: a Linux C++17 compiler with static C/C++ libraries, Windows Visual Studio 2022's **Desktop development with C++**, the Windows SDK, CMake (the VS component is supported), and PowerShell. The standard process/network/service views need no Node.js or Python runtime; the optional Node Inspector backend uses system Python 3 in the distro. The Windows plugin needs no Python, Node.js, or .NET application runtime.

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
