# Architecture

## Host bridge

`plugin/entry.c` and `plugin/tree_bridge.c` include the System Informer SDK. The entry adapter registers the WSL tab, forwards tab visibility and shutdown, and provides font/theme access. It also adapts System Informer global refresh and highlighting settings and opens its Options dialog at the WSL category. The TreeNew adapter keeps SDK node/column/callback structures out of C++ and uses the host’s registered control and public messages. These adapters form the compatibility boundary: their imported exports, structures and callbacks must remain compatible with the host. The remaining Windows code uses ordinary Win32 APIs and C++17.

## Windows side

- `view.cpp`: main window creation, layout, messages and tab lifecycle.
- `view_state.hpp`: private view state and reply tags.
- `view_model.cpp`: process identities, CPU/I/O deltas, retained CPU averages, ancestry and filtering.
- `view_actions.cpp`: process/service actions and contextual menus.
- `target_actions.cpp`: shared process/service Options menus and process-wide scheduling commands.
- `detail_options.cpp`: inspector filters and local highlighting preferences.
- `resource_dialog.cpp`: asynchronous resource properties, hex/string/ELF output, and scheduling input dialogs.
- `process_rules.cpp`: saved scheduling preferences by distribution/executable, with identity, rule-revision and thread-count tracking.
- `details.cpp`: independently owned, asynchronous process/service inspectors.
- `handle_search.cpp`: explicit resource searches and navigation to owning processes.
- `resource_tooltips.cpp`: process, service and network tooltips from cached row data.
- `common.cpp`: the shared TreeNew-backed Table model, semantic row colors, exports, clipboard and UI helpers.
- `graphs.cpp`: CPU and VM-memory sample history, drawing, moving time grid, and hover details.
- `settings.cpp`: registry configuration, validated path translation, settings window.
- `options.rc`: the native WSL page hosted inside System Informer Options.
- `controller.cpp`: serialized work queue, connection reuse and cancellation.
- `transport.cpp`: WSL discovery, process creation, binary deployment and bounded protocol I/O.

The UI never performs a WSL request synchronously. A worker owns all connections and executes requests in order. Replies are posted through mailboxes. A mailbox mutex makes window teardown and posting mutually exclusive; destruction detaches then drains pending replies. Main-view reply tags include an epoch so responses to an old distro selection cannot overwrite the current view. Shutdown cancels the active connection before joining the worker.

The shared `Table` owns lightweight rows and supplies text/colors/fonts/tooltips to the host TreeNew control on demand. Its C adapter owns native node storage; C++ retains filtering, exact numeric sorting, lifecycle highlights and persisted column identities. Native fixed columns, headers and PhScrollNew scrollbars replace the previous ListView painting and scroll corrections. Selection is restored by resource identity rather than row index after refresh/sort. Process identity is the distro/connection context plus Linux PID and start ticks; a change of VM boot ID resets CPU sampling state.

Automatic refresh and its interval come from System Informer's main View settings; the WSL view does not maintain a separate pause or interval. F5/View→Refresh requests a fresh discovery and snapshot even when automatic updates are off. Hiding the WSL tab keeps lightweight capture running by default; with background capture disabled, it cancels work and disconnects its collector. The view's Settings button opens System Informer Options at the WSL category.

The WSL Options page stores CPU mode, background capture, Node Inspector preference, optional ELF32 detection, interop-process filters and per-distro Explorer prefixes. Background capture, automatic Inspector use and both interop filters default on; explicit saved values take precedence. ELF32 detection defaults off. Its distro picker contains all registered WSL 2 distros, including stopped ones. Path translation recognizes `/mnt/<letter>` only at a path-component boundary and maps it directly to the corresponding Windows drive before checking a per-distro prefix. Thus `/mnt/c` maps to `C:\`, `/mnt/c/Users` to `C:\Users`, while `/mnt/cfoo` uses the normal Linux-path prefix.

Lifecycle highlighting uses the host's `ColorNew`, `ColorRemoved`, and `HighlightingDuration`; temporary new/removed colors take precedence over semantic categories. Semantic color choices read the corresponding host `UseColor...` flags and colors. Process colors follow the enabled native priority: debugged, suspended/partially suspended, background, observer/filtered, elevated, optional 32-bit, system, own, then service. Elevated means effective UID 0 with a nonzero numeric `SUDO_UID`; other root processes belong to the System processes category. Own processes match the distro’s default effective UID. Service detection uses the innermost `.service` cgroup component, including user services, without requiring `/system.slice/`. Running services with a PID use the enabled Service processes color, without bold text; other resource tables use their corresponding state fields. The first collection establishes the row baseline. Filtering does not remove rows from the model. A row absent from an incomplete or inaccessible collection is retained until a complete snapshot can establish that it exited; confirmed removals remain copyable but are excluded from actions for the host highlight duration.

The two graphs use 120 sampled points and stay in a fixed layout across Processes, Services, and Network. CPU history is process CPU divided by guest vCPU count; VM memory uses total minus available memory. Hover text includes the local timestamp with milliseconds, sample interval, precise byte values, CPU normalization, process count, and the highest-CPU/largest-RSS process from that snapshot. Graph colors come from the host settings, and vertical grid lines move with the sample history.

Process properties are presented in this order: General, Threads, Modules, Memory, Environment, Handles, Network, native Stacks, the recognized runtime Stacks page (if any), then Details. Modules groups verified ELF executables/shared objects by device and inode, including deleted-file markers. Memory lists all VMAs from procfs. Their note separates verified images from anonymous/JIT mappings and files that could not be verified.

Process and service inspectors share bottom Options, Refresh, Copy/Save and Close controls. Grid pages have their own Options menu and rich search field. Resource dialogs select a properties grid, text view, or both according to the operation; scheduling editors use compact Save/Cancel forms with CPU checkboxes, policy/class dropdowns and numeric spinners. Expensive resource inspections run only on request. The Services view persists its Show inactive services checkbox, defaulting off while retaining failed units.

Saved scheduling rules match the distribution and full executable path. Existing snapshots supply identities and thread counts; executable paths are additionally requested while rules need them. The controller queues at most one saved action per snapshot, with checks against stale rules and process identities. A new identity, executable, boot, thread count or rule revision permits another attempt; persistent failures do not cause a retry on every tick. Capture policy also applies to these jobs. Process-wide settings operate on the current thread set and report partial results; the numeric-TID Linux APIs cannot make the update atomic against thread exit/reuse.

## Transport and deployment

The main view lists running WSL 2 distributions. WSL 2 is identified from the `Flags` VM-mode bit (`0x8`) in each `HKCU\...\Lxss` registration; the registry `Version` value is a schema version, not the WSL generation. The WSL Options page and setup inventory use the same flag to list all registered WSL 2 distributions, including stopped ones, without starting them just to show the list.

For monitoring, the plugin uses `wsl.exe --distribution <name> --user root --exec ...`, not a login shell. Selecting a running distro checks for `/usr/local/lib/system-informer-wsl/wsl-observer`. If it is missing, the view presents an install notice and an explicit Install and retry action. On every connection, the plugin compares SHA-256 hashes and replaces a stale observer with the bundled version. The bootstrap uses `/bin/sh` and basic utilities; it does not invoke a distro package manager or require a Windows drive mount.

The observer runs as root when launched. It is a short-lived process tied to the plugin connection, not a daemon; closing the connection stops it, but leaves the installed binary in place. The launched process inherits only its designated pipe handles. Stdin writes use overlapped I/O with deadlines; stdout/stderr are independently bounded. Stdin/stdout carry versioned NDJSON. A recoverable remote-operation error leaves the transport usable; protocol errors and timeouts make that connection terminal. The controller does not transparently relaunch failed collectors. The plugin closes only its own launchers, never a distro or a user's terminal. It never removes the installed observer automatically.

The setup GUI (`setup.cmd`) defaults to copying the Windows plugin and installing the companion in every registered WSL 2 distro; its uninstall mode defaults to removing both. A stopped distro is started by WSL during companion install/removal and left running. The GUI separates the Windows file-copy child from WSL work: UAC is requested only if the chosen Program Files folder needs it, while WSL commands run in the original Windows user's context and as root inside Linux. Neither GUI action replaces the System Informer executable or resets settings. The command-line `install.ps1` and `uninstall.ps1` leave companion installation/removal opt-in with `-InstallCompanions` and `-RemoveCompanions`.

## Linux observer

- `main.cpp`: bounded request parsing and operation dispatch.
- `procfs.cpp`: process snapshots, details, identities and pidfd signaling.
- `network.cpp`: TCP/UDP/Unix parsing, socket-inode ownership and optional cached reverse DNS.
- `handles.cpp`: searches descriptors, process paths and mapped files.
- `resource_tools.cpp` / `.hpp`: on-demand mapping, memory, ELF and descriptor inspection.
- `thread_tools.cpp` / `.hpp`: thread diagnostics, affinity discovery and scheduling controls.
- `services.cpp`: systemd queries/actions and the shared trusted-command runner, including deadlines, output limits and child cleanup.
- `runtime_stacks.cpp`: executable-based runtime detection and Node.js, CPython and JVM stack capture.
- `node_inspector.py`: embedded standard-library client for Node Inspector discovery, capture and cleanup.
- `observer.hpp`: shared internal data and function declarations.

The static executable reads procfs directly. Process usernames use a local `/etc/passwd` lookup with numeric fallback rather than NSS or network directory lookups. Optional remote-hostname resolution separately uses the distro’s name service through `getent hosts`. Some procfs information is inherently racy: a process can exit while its files are read. Identity checks distinguish that from a replacement process.

Signal operations open a pidfd, validate the requested start time against the live task, then use `pidfd_send_signal`. They do not fall back to `kill(pid)`. Service commands use an allowlisted verb and a validated service name, absolute executable paths and an explicit environment. No user-supplied string becomes a shell program.

Runtime stack capture is a separate, user-requested operation. The inspector adds a runtime-specific page after native Stacks and before Details by classifying the resolved `/proc/PID/exe` basename. It recognizes `node`/`nodejs`, CPython `python` names (including version/debug/free-thread suffixes), and `java`; command lines, shell/npm wrappers, PyPy, renamed executables, and embedded runtimes do not determine the page. A refreshed overview can add, rename, or remove it. Selecting a Stacks page is passive; the capture button or Ctrl+R performs capture without a confirmation dialog. Each page explains the capture action, pause behavior, and required tool. With automatic Inspector use disabled, Node’s capture-method choice dialog appears when no verified Inspector listener exists, headed **Node Inspector is not enabled for this process**. Without Python 3, the dialog instead shows installation guidance and offers llnode or Cancel, even when automatic Inspector use is enabled.

For Node.js, the default backend finds a listener owned by the process's socket inode and verifies that the Inspector WebSocket endpoint reports the innermost `NSpid` for the selected process. Ordinary sockets with failed or non-Inspector HTTP probes are ignored. Inspector captures include the main JavaScript thread and up to 32 reported worker contexts, sampled sequentially with up to 256 frames each. Each context has a one-second pause-point window; idle contexts are skipped and pending pauses are safely canceled. It does not collect native frames or asynchronous task history.

The Windows preference `UseNodeInspectorWithoutAsking` defaults to 1. On an explicit JS capture, the UI requests `backend:"inspector"` with `enable_inspector:true`, allowing temporary activation without another prompt. If the preference is disabled and no listener exists, the user can choose **Temporarily Enable Inspector**, **Use llnode**, or **Cancel**. The **Don't show again** checkbox saves that preference; the WSL Options page exposes it too. Activation uses SIGUSR1 through `pidfd_send_signal` after process/executable revalidation. A missing-Python dialog displays the helper’s installation instructions, including `apt install python3` for Ubuntu/Debian; Cancel retains that guidance in the stack page. The helper checks identifiable command-line and `NODE_OPTIONS` bind settings for public/unverified addresses, while acknowledging that runtime-mutated `debugPort` and custom signal handlers cannot be inferred from `/proc`. It records listener inodes before activation so pre-existing Inspector listeners are never closed accidentally. A port conflict (for example, two processes using `9229`) cannot redirect capture to the wrong process; `--inspect-port=0` lets each process request an available port.

The embedded Node Inspector client uses the distro's Python 3 standard library with `-I -S`; no packages or separate script deployment are needed. It has a 12-second capture window, reserves two seconds for cleanup, and is bounded by a 20-second observer command timeout and 35-second Windows request timeout. Output is capped at 512 KiB. It resumes only pauses it requested, preserves pre-existing pauses, and closes only a newly created listener after verifying the session. If another client is already connected at cleanup, the Inspector remains enabled; a new client can still race the check. Because Node may disconnect clients while closing its Inspector, concurrent debugger sessions should be avoided. Cleanup warnings appear with the stack. If cleanup is uncertain, `fallback_safe:false` prevents a second attach.

When Inspector capture fails but cleanup is safe, the helper can automatically try llnode and return the original Inspector error alongside fallback output. The fallback is suppressed when `fallback_safe` is false. LLDB/llnode compatibility depends on the LLDB and Node/V8 versions; it may return native addresses or incomplete JavaScript frames. The other backends use root-owned `py-spy` for CPython or `jcmd` from the target JVM's matching JDK. Java's `jcmd` child runs with the target's effective UID and GID. Python captures all Python threads without locals. Traditional Java `Thread.print -l` includes locks but not every unmounted virtual thread. Native GDB, py-spy, llnode, and jcmd can briefly pause the target. The py-spy, llnode and jcmd calls have a 15-second deadline and 512 KiB output limit. Native GDB uses 15 seconds and 256 KiB per attempt, with one possible retry for specific debugger/symbol-reader failures. Numeric-PID debugger attachment retains a narrow PID-reuse race despite identity checks before and after.

The process snapshot and detail overview include a `runtime` classification based on the resolved executable basename. The inspector uses the refreshed overview value to add, rename, or remove the runtime-specific Stacks tab while retaining the native GDB Stacks page. Command lines and script names are deliberately not used for runtime detection.

## Accounting

Raw process CPU is `100 * delta(utime + stime) / CLK_TCK / elapsed_seconds`; one busy vCPU is 100%. The `CpuPercentOfTotal` display setting defaults to 1: it divides process-row CPU by the guest vCPU count, making 100% represent all WSL vCPUs, like the Windows convention. Setting it to 0 selects the Linux one-vCPU scale. The distro summary and graph sum all collected processes before display filtering, then divide by the number of guest CPUs. Hiding or filtering rows does not change that total. Child CPU counters are deliberately excluded. First/new samples have no measurable delta and start at zero. Storage rates similarly use counter deltas. Missing counters, restarts and negative deltas do not create spikes.

**CPU (average)** is the arithmetic mean of retained valid interval samples, not a lifetime average. A per-process buffer and running sum use the host’s `SampleCount` capacity independently of the graphs’ 120-point history. Samples remain in raw one-vCPU units; rendering and numeric sorting apply the selected CPU scale. History is collected even when the column is hidden, with no additional WSL query. Paused capture periods and baseline-only readings are excluded; valid history survives a pause, while exited/reused identities, distro changes, counter resets and boot changes discard it.

Memory totals and pressure data describe the shared WSL VM. Individual process RSS can count shared pages multiple times. The host's `vmmemWSL` accounting is not modified. Snapshot polling is not a complete process/socket event history.

## Verification guidance

Use the existing build scripts for compiler checks. Run UI smoke checks in an isolated System Informer instance with separate settings; never point test settings at the user's normal configuration. If the preview contains only this third-party plugin, set `EnableDefaultSafePlugins=0` in that isolated configuration.

## Reproducible native builds

The Windows staging copy always overwrites inputs, and the native build uses `--clean-first`. A real crash during development revealed that incremental builds across this filesystem boundary could combine translation units with different layouts for the shared `View` structure. Full native recompilation prevents that class of mixed-object binary. Release PDB files are retained with their matching DLL.
