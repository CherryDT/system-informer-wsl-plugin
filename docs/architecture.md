# Architecture

## Host bridge

`plugin/entry.c` is the only file that includes the System Informer SDK. It registers the WSL tab, forwards tab visibility and shutdown, and provides font/theme access. It also adapts System Informer global refresh and highlighting settings and opens its Options dialog at the WSL category. Keep this layer small: export ordinals and the handful of structures it uses are the compatibility boundary with System Informer. The remaining Windows code uses ordinary Win32 APIs and C++17.

## Windows side

- `view.cpp`: main window creation, layout, messages and tab lifecycle.
- `view_state.hpp`: private view state and reply tags.
- `view_model.cpp`: process identities, CPU/I/O deltas, ancestry and filtering.
- `view_actions.cpp`: process/service actions and contextual menus.
- `details.cpp`: independently owned, asynchronous process/service inspectors.
- `common.cpp`: virtual list controls, semantic row colors, exports, clipboard and shared UI helpers.
- `graphs.cpp`: CPU and VM-memory sample history, drawing, moving time grid, and hover details.
- `settings.cpp`: registry configuration, validated path translation, settings window.
- `options.rc`: the native WSL page hosted inside System Informer Options.
- `controller.cpp`: serialized work queue, connection reuse and cancellation.
- `transport.cpp`: WSL discovery, process creation, binary deployment and bounded protocol I/O.

The UI never performs a WSL request synchronously. A worker owns all connections and executes requests in order. Replies are posted through mailboxes. A mailbox mutex makes window teardown and posting mutually exclusive; destruction detaches then drains pending replies. Main-view reply tags include an epoch so responses to an old distro selection cannot overwrite the current view. Shutdown cancels the active connection before joining the worker.

A virtual list view owns lightweight rows and renders text on demand. Selection is restored by resource identity rather than row index after refresh/sort. Process identity is the distro/connection context plus Linux PID and start ticks; a change of VM boot ID resets CPU sampling state.

Automatic refresh and its interval come from System Informer's main View settings; the WSL view does not maintain a separate pause or interval. F5/View→Refresh requests a fresh discovery and snapshot even when automatic updates are off. Hiding the WSL tab disconnects its collector. The view's Settings button opens System Informer Options at the WSL category.

The WSL Options page stores CPU mode and `UseNodeInspectorWithoutAsking`; its distro picker contains all registered WSL 2 distros, including stopped ones. Path translation recognizes `/mnt/<letter>` only at a path-component boundary and maps it directly to the corresponding Windows drive before checking a per-distro prefix. Thus `/mnt/c` maps to `C:\`, `/mnt/c/Users` to `C:\Users`, while `/mnt/cfoo` uses the normal Linux-path prefix.

Lifecycle highlighting uses the host's `ColorNew`, `ColorRemoved`, and `HighlightingDuration`; temporary new/removed colors take precedence over semantic categories. Semantic color choices read the corresponding host `UseColor...` flags and colors. Process coloring uses `tracer_pid`, state, UID 0 and `is_service` (the cgroup path contains `/system.slice/` and `.service`); service, thread, memory and network tables use their matching state fields. The first collection establishes the row baseline. Filtering does not remove rows from the model. A row absent from an incomplete or inaccessible collection is retained until a complete snapshot can establish that it exited; confirmed removals remain copyable but are excluded from actions for the host highlight duration.

The two graphs use 120 sampled points and stay in a fixed layout across Processes, Services, and Network. CPU history is process CPU divided by guest vCPU count; VM memory uses total minus available memory. Hover text includes the local timestamp with milliseconds, sample interval, precise byte values, CPU normalization, process count, and the highest-CPU/largest-RSS process from that snapshot. Graph colors come from the host settings, and vertical grid lines move with the sample history.

Process properties are presented in this order: General, Threads, Modules, Memory, Environment, Handles, Network, native Stacks, the recognized runtime Stacks page (if any), then Details. Modules groups verified ELF executables/shared objects by device and inode, including deleted-file markers. Memory lists all VMAs from procfs. Their note separates verified images from anonymous/JIT mappings and files that could not be verified.

## Transport and deployment

The main view lists running WSL 2 distributions. WSL 2 is identified from the `Flags` VM-mode bit (`0x8`) in each `HKCU\...\Lxss` registration; the registry `Version` value is a schema version, not the WSL generation. The WSL Options page and setup inventory use the same flag to list all registered WSL 2 distributions, including stopped ones, without starting them just to show the list.

For monitoring, the plugin uses `wsl.exe --distribution <name> --user root --exec ...`, not a login shell. Selecting a running distro checks for `/usr/local/lib/system-informer-wsl/wsl-observer`. If it is missing, the view presents an install notice and an explicit Install and retry action. On every connection, the plugin compares SHA-256 hashes and replaces a stale observer with the bundled version. The bootstrap uses `/bin/sh` and basic utilities; it does not invoke a distro package manager or require a Windows drive mount.

The observer runs as root when launched. It is a short-lived process tied to the plugin connection, not a daemon; closing the connection stops it, but leaves the installed binary in place. The launched process inherits only its designated pipe handles. Stdin writes use overlapped I/O with deadlines; stdout/stderr are independently bounded. Stdin/stdout carry versioned NDJSON. A recoverable remote-operation error leaves the transport usable; protocol errors and timeouts make that connection terminal. The controller does not transparently relaunch failed collectors. The plugin closes only its own launchers, never a distro or a user's terminal. It never removes the installed observer automatically.

The setup GUI (`setup.cmd`) defaults to copying the Windows plugin and installing the companion in every registered WSL 2 distro; its uninstall mode defaults to removing both. A stopped distro is started by WSL during companion install/removal and left running. The GUI separates the Windows file-copy child from WSL work: UAC is requested only if the chosen Program Files folder needs it, while WSL commands run in the original Windows user's context and as root inside Linux. Neither GUI action replaces the System Informer executable or resets settings. The command-line `install.ps1` and `uninstall.ps1` leave companion installation/removal opt-in with `-InstallCompanions` and `-RemoveCompanions`.

## Linux observer

- `main.cpp`: bounded request parsing and operation dispatch.
- `procfs.cpp`: process snapshots, details, identities and pidfd signaling.
- `network.cpp`: TCP/UDP/Unix parsing and socket-inode ownership.
- `services.cpp`: validated systemd/GDB commands, bounded output and child cleanup.
- `runtime_stacks.cpp`: executable-based runtime detection and bounded Node.js, CPython and JVM stack capture.
- `observer.hpp`: internal data and function declarations.

The static executable reads procfs directly. It uses a local `/etc/passwd` lookup with numeric fallback rather than NSS or network directory lookups. Some procfs information is inherently racy: a process can exit while its files are read. Identity checks distinguish that from a replacement process.

Signal operations open a pidfd, validate the requested start time against the live task, then use `pidfd_send_signal`. They do not fall back to `kill(pid)`. Service commands use an allowlisted verb and a validated service name, absolute executable paths and an explicit environment. No user-supplied string becomes a shell program.

Runtime stack capture is a separate, user-requested operation. The inspector adds a runtime-specific page after native Stacks and before Details by classifying the resolved `/proc/PID/exe` basename. It recognizes `node`/`nodejs`, CPython `python` names (including version/debug/free-thread suffixes), and `java`; command lines, shell/npm wrappers, PyPy, renamed executables, and embedded runtimes do not determine the page. A refreshed overview can add, rename, or remove it. Selecting a Stacks page is passive; the capture button or Ctrl+R performs capture without a confirmation dialog. Each page explains the capture action, pause behavior, and required tool. Node's capture-method choice dialog appears when no verified Inspector listener exists, headed **Node Inspector is not enabled for this process**; without Python 3, it offers llnode but not Inspector activation.

For Node.js, the default backend finds a listener owned by the process's socket inode and verifies that the Inspector WebSocket endpoint reports the innermost `NSpid` for the selected process. Ordinary sockets with failed or non-Inspector HTTP probes are ignored. Inspector captures include the main JavaScript thread and up to 32 reported worker contexts, sampled sequentially with up to 256 frames each. Each context has a one-second pause-point window; idle contexts are skipped and pending pauses are safely canceled. It does not collect native frames or asynchronous task history.

If no listener exists, the user can choose **Temporarily Enable Inspector**, **Use llnode**, or **Cancel**. The **Don't show again** checkbox on the temporary-activation choice stores `UseNodeInspectorWithoutAsking`; the WSL Options page exposes the same preference. Enabling uses SIGUSR1 through `pidfd_send_signal` only after explicit selection and process/executable revalidation. The helper checks identifiable command-line and `NODE_OPTIONS` bind settings for public/unverified addresses, while acknowledging that runtime-mutated `debugPort` and custom signal handlers cannot be inferred from `/proc`. It records listener inodes before activation so pre-existing Inspector listeners are never closed accidentally. A port conflict (for example, two processes using `9229`) cannot redirect capture to the wrong process; `--inspect-port=0` lets each process request an available port.

The embedded Node Inspector client uses the distro's Python 3 standard library with `-I -S`; no packages or separate script deployment are needed. It has a 12-second capture window, reserves two seconds for cleanup, and is bounded by a 20-second observer command timeout and 35-second Windows request timeout. Output is capped at 512 KiB. It resumes only pauses it requested, preserves pre-existing pauses, and closes only a newly created listener after verifying the session. If another client is already connected at cleanup, the Inspector remains enabled; a new client can still race the check. Because Node may disconnect clients while closing its Inspector, concurrent debugger sessions should be avoided. Cleanup warnings appear with the stack. If cleanup is uncertain, `fallback_safe:false` prevents a second attach.

When Inspector capture fails but cleanup is safe, the helper can automatically try llnode and return the original Inspector error alongside fallback output. The fallback is suppressed when `fallback_safe` is false. LLDB/llnode remains version-dependent; for example, LLDB 18 with llnode 4 may not decode Node 22 JavaScript names. The other backends use root-owned `py-spy` for CPython or `jcmd` from the target JVM's matching JDK. Java's `jcmd` child runs with the target's effective UID and GID. Python captures all Python threads without locals. Traditional Java `Thread.print -l` includes locks but not every unmounted virtual thread. Native GDB, py-spy, llnode, and jcmd can briefly pause the target; captures have a 15-second tool deadline and 512 KiB output limit. Numeric-PID debugger attachment retains a narrow PID-reuse race despite identity checks before and after.

The process snapshot and detail overview include a `runtime` classification based on the resolved executable basename. The inspector uses the refreshed overview value to add, rename, or remove the runtime-specific Stacks tab while retaining the native GDB Stacks page. Command lines and script names are deliberately not used for runtime detection.

## Accounting

Raw process CPU is `100 * delta(utime + stime) / CLK_TCK / elapsed_seconds`; one busy vCPU is 100%. The `CpuPercentOfTotal` display setting defaults to 1: it divides process-row CPU by the guest vCPU count, making 100% represent all WSL vCPUs, like the Windows convention. Setting it to 0 selects the Linux one-vCPU scale. The distro summary and graph always divide the visible process sum by the number of guest CPUs. Child CPU counters are deliberately excluded. First/new samples have no measurable delta and start at zero. Storage rates similarly use counter deltas. Missing counters, restarts and negative deltas do not create spikes.

Memory totals and pressure data describe the shared WSL VM. Individual process RSS can count shared pages multiple times. The host's `vmmemWSL` accounting is not modified. Snapshot polling is not a complete process/socket event history.

## Verification guidance

Use the existing build scripts for compiler checks. Run UI smoke checks in an isolated System Informer instance with separate settings; never point test settings at the user's normal configuration. If the preview contains only this third-party plugin, set `EnableDefaultSafePlugins=0` in that isolated configuration.

## Reproducible native builds

The Windows staging copy always overwrites inputs, and the native build uses `--clean-first`. A real crash during development revealed that incremental builds across this filesystem boundary could combine translation units with different layouts for the shared `View` structure. Full native recompilation prevents that class of mixed-object binary. Release PDB files are retained with their matching DLL.
