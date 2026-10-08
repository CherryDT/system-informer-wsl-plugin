# Architecture

## Host bridge

`plugin/entry.c` is the only file that includes the System Informer SDK. It registers the WSL tab, forwards tab visibility and shutdown, and provides font/theme access. Keep this layer small: export ordinals and the handful of structures it uses are the compatibility boundary with System Informer. The remaining Windows code uses ordinary Win32 APIs and C++17.

## Windows side

- `view.cpp`: main window creation, layout, messages and tab lifecycle.
- `view_state.hpp`: private view state and reply tags.
- `view_model.cpp`: process identities, CPU/I/O deltas, ancestry and filtering.
- `view_actions.cpp`: process/service actions and contextual menus.
- `details.cpp`: independently owned, asynchronous process/service inspectors.
- `common.cpp`: virtual list controls, semantic row colors, exports, clipboard and shared UI helpers.
- `settings.cpp`: registry configuration, validated path translation, settings window.
- `controller.cpp`: serialized work queue, connection reuse and cancellation.
- `transport.cpp`: WSL discovery, process creation, binary deployment and bounded protocol I/O.

The UI never performs a WSL request synchronously. A worker owns all connections and executes requests in order. Replies are posted through mailboxes. A mailbox mutex makes window teardown and posting mutually exclusive; destruction detaches then drains pending replies. Main-view reply tags include an epoch so responses to an old distro selection cannot overwrite the current view. Shutdown cancels the active connection before joining the worker.

A virtual list view owns lightweight rows and renders text on demand. Selection is restored by resource identity rather than row index after refresh/sort. Process identity is the distro/connection context plus Linux PID and start ticks; a change of VM boot ID resets CPU sampling state.

## Transport and deployment

The plugin enumerates running distros and excludes WSL1 using the distro registry metadata. It uses `wsl.exe --distribution <name> --user root --exec ...`, not a login shell. Selecting a distro first checks for the bundled observer at `/usr/local/lib/system-informer-wsl/wsl-observer`. If it is missing, the view presents an install notice and an explicit install-and-retry action. On connection, the plugin compares SHA-256 hashes and replaces a stale observer with the bundled version. The install bootstrap uses standard POSIX shell and core utilities; it does not invoke a distro package manager or require a Windows drive mount.

The observer runs as root when launched. It is a short-lived process tied to the plugin connection, not a daemon; closing the connection stops it, but leaves the installed binary in place. The launched process inherits only its designated pipe handles. Stdin writes use overlapped I/O with deadlines; stdout/stderr are independently bounded. Stdin/stdout carry versioned NDJSON. A recoverable remote-operation error leaves the transport usable; protocol errors and timeouts make that connection terminal. The controller does not transparently relaunch failed collectors. The plugin closes only its own launchers, never a distro or a user's terminal. It never removes the installed observer automatically.

## Linux observer

- `main.cpp`: bounded request parsing and operation dispatch.
- `procfs.cpp`: process snapshots, details, identities and pidfd signaling.
- `network.cpp`: TCP/UDP/Unix parsing and socket-inode ownership.
- `services.cpp`: validated systemd/GDB commands, bounded output and child cleanup.
- `runtime_stacks.cpp`: executable-based runtime detection and bounded Node.js, CPython and JVM stack capture.
- `observer.hpp`: internal data and function declarations.

The static executable reads procfs directly. It uses a local `/etc/passwd` lookup with numeric fallback rather than NSS or network directory lookups. Some procfs information is inherently racy: a process can exit while its files are read. Identity checks distinguish that from a replacement process.

Signal operations open a pidfd, validate the requested start time against the live task, then use `pidfd_send_signal`. They do not fall back to `kill(pid)`. Service commands use an allowlisted verb and a validated service name, absolute executable paths and an explicit environment. No user-supplied string becomes a shell program.

Runtime stack capture is a separate, user-requested operation. The inspector adds a runtime-specific page after native Stacks and before Details by classifying the resolved `/proc/PID/exe` basename. It recognizes `node`/`nodejs`, CPython `python` names (including version/debug/free-thread suffixes), and `java`; command lines, shell/npm wrappers, PyPy, renamed executables, and embedded runtimes do not determine the page. A refreshed overview can add, rename, or remove it. Selecting a Stacks page is passive; the capture button or Ctrl+R performs the attach without a confirmation dialog. The page itself explains the capture button, possible process pause, and required tool. Node's only modal is a backend choice when no Inspector listener exists; without Python 3, the dialog offers llnode but not Inspector activation.

For Node.js, the default backend first searches for a listener owned by the selected process, then verifies the Inspector WebSocket endpoint reports that same process PID (including its innermost `NSpid`). It captures the main JavaScript thread only; it does not include worker threads, native frames, or async task history. If no listener exists, the user can choose **Enable Inspector**, **Use llnode**, or **Cancel**. Enabling uses SIGUSR1 only after explicit selection, through `pidfd_send_signal`, and only after checking identifiable command-line and `NODE_OPTIONS` bind settings for public or unverified addresses. A custom signal handler or runtime-mutated `debugPort` cannot be inferred from procfs; the endpoint must still pass process and socket ownership checks before capture. Port conflicts fail closed: a process with the default port 9229 occupied cannot capture another process's Inspector. `--inspect-port=0` lets separate processes request their own available port.

The embedded Node Inspector client runs with the distro's system Python 3 using `-I -S`; it uses only the standard library, is compiled into `wsl-observer`, and needs no separate deployed Python file or third-party package. It bounds its capture phase to 12 seconds and reserves up to 2 seconds for cleanup; the observer stops the client at 20 seconds, and the Windows request deadline is 35 seconds. Inspector output is capped at 512 KiB. It resumes only a pause it requested and preserves an already-paused target. It closes an Inspector only if this capture enabled it and verified the session, using a feature-detected `inspector.close()` call, then disconnects its session and checks whether the listener disappeared. A pre-existing listener is left enabled. Cleanup is best-effort; Node's close operation can disconnect other debugger clients that joined the temporary listener, so avoid concurrent debuggers. A message warns if closure cannot be confirmed. Enabling is declined when configured bind options identify a public/unverified host; an already-running listener is not reconfigured.

The other backends use trusted installed tools: version-matched LLDB/`llnode` for Node.js fallback, root-owned `py-spy` for CPython, or the `jcmd` sibling of the target JVM's matching JDK. Java's `jcmd` child runs with the target process's effective UID and GID even though the observer itself runs as root. llnode captures OS-thread stacks with a 256-thread/64-frame cap; py-spy captures all Python threads without locals; traditional Java `Thread.print -l` includes locks but not every unmounted virtual thread. These tool captures have a 15-second deadline and a 512 KiB output limit. Process identity and executable are checked before and after capture; numeric-PID attachment leaves a narrow PID-reuse race.

The process snapshot and detail overview include a `runtime` classification based on the resolved executable basename. The inspector uses the refreshed overview value to add, rename, or remove the runtime-specific Stacks tab while retaining the native GDB Stacks page. Command lines and script names are deliberately not used for runtime detection.

## Accounting

Raw process CPU is `100 * delta(utime + stime) / CLK_TCK / elapsed_seconds`; one busy vCPU is 100%. The `CpuPercentOfTotal` display setting defaults to 1: it divides process-row CPU by the guest vCPU count, making 100% represent all WSL vCPUs, like the Windows convention. Setting it to 0 selects the Linux one-vCPU scale. The distro summary and graph always divide the visible process sum by the number of guest CPUs. Child CPU counters are deliberately excluded. First/new samples have no measurable delta and start at zero. Storage rates similarly use counter deltas. Missing counters, restarts and negative deltas do not create spikes.

Memory totals and pressure data describe the shared WSL VM. Individual process RSS can count shared pages multiple times. The host's `vmmemWSL` accounting is not modified. Snapshot polling is not a complete process/socket event history.

## Verification approach

Use the existing build scripts for compiler checks. During development, the observer was exercised against real procfs/systemd data; signaling used only a temporary process created for that purpose. Windows transport checks covered root launch, discovery, request/response framing, recoverable errors and cancellation. UI checks use an isolated copy of System Informer with its own settings; never point test settings at the user's normal configuration, and set `EnableDefaultSafePlugins=0` when the preview contains only this third-party plugin.

## Reproducible native builds

The Windows staging copy always overwrites inputs, and the native build uses `--clean-first`. A real crash during development revealed that incremental builds across this filesystem boundary could combine translation units with different layouts for the shared `View` structure. Full native recompilation prevents that class of mixed-object binary. Release PDB files are retained with their matching DLL.
