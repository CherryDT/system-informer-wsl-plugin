# WSL observer protocol, version 1

`wsl-observer` is a persistent Linux process. The Windows plugin launches one
helper in the selected distribution and communicates through redirected stdin
and stdout. It runs as root by default for complete process inspection; it never
executes shell expressions. Closing stdin ends the helper. No TCP listener or
Windows service is needed.

Every message is one UTF-8 JSON object followed by a newline (NDJSON). The helper
sends exactly one response for each complete input line and never emits unsolicited
messages. Requests are handled sequentially. The maximum request length is 1 MiB;
an oversized line is drained and rejected. Nesting is limited to 32 levels. Invalid UTF-8 in Linux names is
replaced with U+FFFD when serializing a response. Stdout is protocol-only.

```json
{"id":1,"op":"hello"}
{"id":1,"ok":true,"data":{"protocol":1,"uid":0,"cpus":16,"clock_ticks":100,"boot_id":"…","systemd":true,"helper_version":"0.1.0"}}
{"id":2,"op":"details","pid":123,"start_ticks":4567}
{"id":2,"ok":false,"error":"Process exited or its /proc entry is inaccessible"}
```

`id` must be an integer and is echoed unchanged. Errors are readable strings;
malformed requests without a valid ID return `id:null`. Clients must ignore unknown
response fields to allow compatible extensions. Client timeouts should close the
transport and start a fresh helper, rather than assuming a late response belongs
to a subsequent request.

## Process identity

PIDs belong to the selected distribution's PID namespace. The client additionally
tracks the distribution, helper session, and `boot_id`; a process operation uses
both `pid` and `start_ticks` (`/proc/PID/stat` start time in clock ticks since boot).

Read operations validate identity before and after collecting details. Signal
operations open a pidfd, validate start time, and use `pidfd_send_signal`. There is
no unsafe `kill(pid)` fallback. PID 1 and the helper itself are protected from
signals. A disappearing process is a normal, recoverable error.

## Operations

### `hello`

Returns `protocol`, `helper_version`, `boot_id`, `uid`, `cpus` (online logical CPUs),
`clock_ticks` (ticks per second), `systemd` (whether systemd is running), and
`gdb` (whether GDB is installed in a trusted system binary directory).

### `snapshot` (optional `fields`, `detect_32bit`, `default_uid`, `pid`, `start_ticks`)

A `fields` string array selects optional process metadata. Omitting it requests
all normal metadata for protocol-1 compatibility; an empty array requests only
inexpensive `/proc/PID/stat` data and VM graph counters. The response echoes
`fields` when supplied. `detect_32bit` defaults to false and must additionally be
true before an executable is read for its ELF class. `default_uid` is the distro's
configured default user UID, supplied by the Windows client; it defaults to -1
(unknown). Optional `pid` and `start_ticks` restrict collection to a validated
process identity.

Every process includes `pid`, `ppid`, `start_ticks`, `name`, `state`, `threads`,
`cpu_ticks`, `rss_bytes`, `virtual_bytes`, `nice`, `priority`, `pgrp`, `session`,
`tty_nr`, decoded `tty`, `no_tty`, `minor_faults`, `major_faults`, `processor`,
`policy`, `user_ticks`, and `kernel_ticks`. Optional groups are:

| `fields` entry | Added process fields |
| --- | --- |
| `status` | `uid`, `euid`, `gid`, `egid`, `status_accessible`, `is_own`, `tracer_pid`, `voluntary_switches`, `involuntary_switches`, `swap_bytes`, `seccomp`, `no_new_privs`, `capabilities` |
| `user` | Status fields and effective-user `user` name |
| `sudo` | Status fields and `sudo_root` |
| `command` | `command` |
| `io` | `io_accessible`, `read_bytes`, `write_bytes`, `read_chars`, `write_chars`, `syscr`, `syscw`, `cancelled_write_bytes` |
| `cgroup` | `cgroup`, `is_service`, `service_unit`, `service_scope` |
| `exe` | `exe`, `runtime` |
| `cwd` | `cwd` |
| `suspension` | `stopped_threads`, `is_suspended`, `is_partially_suspended` when thread state can be established |
| `elf32` | `is_32bit` when `detect_32bit:true` and the executable's ELF class can be read |
| `loadavg` | Top-level `loadavg` text |
| `pressure` | Top-level CPU, memory and I/O `pressure` text, where available |

Individual field aliases are accepted for several groups; clients should use the
group names above. `runtime` is `"node"`, `"python"`, `"java"`, or empty when
unrecognized. `is_own` compares effective UID with `default_uid`. `sudo_root`
requires effective UID 0 and a nonzero numeric `SUDO_UID` from up to 256 KiB of
the process environment. `is_service` recognizes `.service` cgroup path
components, including user services. `service_unit` is the deepest `.service`
component and `service_scope` is `"user"` below a `user@UID.service` manager,
otherwise `"system"`; both are empty when no service is found. The manager itself
is a system service. Suspension examines task states `T`/`t`. For a process with
one thread, the existing process stat supplies that thread's state without a
task-directory scan. For multithreaded processes, fully suspended requires a
complete enumeration matching the process thread count. An unreadable or changing
task list does not establish full suspension.
ELF detection pins a regular executable and validates ELF magic and byte 4
(`EI_CLASS`); unknown is represented by an absent `is_32bit`, not false.

The response also includes:

- `processes` and `processes_truncated` (the encoded array has a 12 MiB budget).
- `monotonic_ms`: helper monotonic time, sampled at the end of collection.
- `uptime_seconds`, `memory_total`, `memory_available`, `cpus`, `clock_ticks`, `boot_id`.
  Memory and pressure values describe the shared WSL VM, not just this distro.

`cpu_ticks` is user plus system CPU time for this process, excluding reaped child
CPU time. Compute one-core usage as
`100 * delta(cpu_ticks) / clock_ticks / delta(seconds)`. Dividing by `cpus` gives
a fraction of the guest's capacity. Neither value is host VM CPU attribution.
Negative deltas, a changed boot ID, or a changed process identity reset the sample.
RSS and virtual memory are byte counts, and I/O counters are cumulative.
Missing I/O permission produces `io_accessible:false` with zero counters.
Unreadable status produces `status_accessible:false`, user `unknown` when
requested, and UID/GID 4294967295 instead of incorrectly reporting root.
Short-lived processes may disappear during collection and are omitted. Usernames
come from local `/etc/passwd`; other UIDs remain numeric, avoiding network name
services. Command lines are capped at 16 KiB per process.

The Windows view selects groups from visible or sorted columns and enabled
highlighting, applicable filters, and service-unit tooltips. It keeps cheap identity/CPU/RSS samples while hidden or minimized
for graph history and lifecycle tracking, then requests visible metadata on
return. Unrequested metadata may be retained for the same PID/start-time identity;
requested-but-unavailable metadata must clear the previous value. This avoids
making a changed executable or credential appear current because of old cache
contents. Sampling follows the host automatic-refresh setting. The Windows
`EnableBackgroundCapture` setting defaults to 1. With it set to 0, the controller
rejects requests for inactive distributions, closes their observer clients, and
stops discovery while the WSL tab or host is hidden/minimized. Inspector and
Find handles requests follow the same policy. Reopening the selected distro
reconnects and refreshes; uncaptured graph intervals remain gaps. This is client
policy rather than a wire-protocol request, and never shuts down a distro.

### `details` (`pid`, `start_ticks`)

Returns:

- `overview`: current process snapshot fields (including `runtime`), plus `cwd`, `cgroup`,
  `capabilities` (effective, permitted, inheritable, bounding, and ambient masks),
  `seccomp` (`Disabled`, `Strict`, or `Filter`), and `no_new_privs` (`Yes` or `No`).
  Missing status fields remain empty; numeric fields retain snapshot types.
- `summary`: human-readable status, I/O counters, cgroups, resource limits,
  namespace IDs, executable path, and current working directory. Status includes
  UIDs/GIDs, capability masks, seccomp state, and other kernel-provided fields.
- `files`: `{fd,target,flags,flags_text,inherited}`. `flags` is the original octal
  `/proc` flag string; `flags_text` contains symbolic names and a hexadecimal value
  (or `Unknown`). `inherited` means known flags without `O_CLOEXEC`, an exec-survival
  approximation rather than proof of fork inheritance. Targets may be paths,
  sockets, pipes, anonymous handles, or deleted paths.
- `modules`: verified ELF executable/shared-object images that have an executable
  mapping. One object is returned per device/inode identity, with `path`, `base`,
  `end`, `size_bytes`/`mapped_bytes`, `device`, `inode`, `identity`, `deleted`,
  `main_module`, `native_module`, `known_library`, and `mapped_module`. The main
  image matches `/proc/PID/exe` by device/inode. Native recognizes default loaders
  under standard library paths; Known libraries are other libraries there.
  `mapped_module` is currently false: ordinary mapped files belong in Memory,
  and ELF ASLR is not evidence of Windows-style image relocation.
  Addresses are hexadecimal strings. The mapped size sums the image's mapped
  segments and excludes gaps; it is not the base-to-end address range.
- `memory`: every parsed VMA from `/proc/PID/maps`, with `start`, `end`,
  `size_bytes`, `permissions`, `offset`, `path`, `device`, `inode`, `deleted`,
  `private_pages`, `system_pages`, and `execute_pages`. Private means a private
  anonymous/heap/stack mapping; system recognizes kernel-provided `[vdso]`,
  `[vvar]`, `[vsyscall]` and `[vvar_vclock]`; execute follows the `x` permission.
  No Windows CFG-page classification is inferred.
  It includes anonymous/JIT mappings, bracketed kernel mappings such as `[heap]`
  and `[vdso]`, and mapped files that are not verified ELF modules. Empty `path`
  represents an anonymous mapping. Addresses and offsets are hexadecimal strings.
- `environment`: `{name,value,scope}`. Ordering and duplicate names are retained.
  Scope is `system` for an exact literal name/value match from the target root
  `/etc/environment`, then `user` for exact account `USER`, `LOGNAME`, `HOME` or
  `SHELL` matches, otherwise `process`. Shell expansions are not evaluated. These
  are matching baselines, not evidence of where a value was inherited.
- `threads`: `{tid,name,state,wchan,wait_kind}` entries. Wait kind is `suspended`,
  `delay`, `alert` (futex), `queue`, `executive`, `user_request`, or empty. These
  descriptive Linux wait-channel analogues are not Windows wait-reason IDs.
- `files_accessible`, `modules_accessible`, `memory_accessible`: availability
  indicators. `modules_unverified` counts executable mapped files that could not
  be checked as ELF images. `module_classification_note` explains what appears
  under Modules versus Memory.
- `files_truncated`, `modules_truncated`, `memory_truncated`, `environment_truncated`,
  `threads_truncated`, `summary_truncated`: display-limit indicators. A visible
  notice is also appended to `summary` when any part was truncated.

The environment can contain credentials and other secrets; the UI should expose
it only through explicit inspection, without automatic logging. Inspection is a
best-effort snapshot, not a frozen view of the process. Maps are capped at 8 MiB,
environment at 4 MiB, and ordinary proc files at 1 MiB. Encoded files, modules,
memory, and environment arrays each have a 1 MiB budget; threads have 512 KiB. Summary
text is capped at 256 KiB. These limits keep detail responses below the Windows
transport limit even when paths contain characters requiring JSON escapes.
A partial final environment value is omitted instead of presenting it as complete.

### `find_handles` (`query`, optional `enumerate`, `case_sensitive`)

Searches open descriptors and file mappings across the selected distro's visible
PID namespace. `query` must be 1–1024 UTF-8 bytes. Matching applies only to the
resource's `path`, never the process name, command line, or PID. No shell, `lsof`,
or target-file open is used.

`enumerate` and `case_sensitive` default to false. With `enumerate:false`, the
helper uses a literal byte-substring prefilter, folding ASCII case unless
`case_sensitive:true`. Case-insensitive paths containing non-ASCII bytes are
retained for authoritative Unicode matching on Windows. `enumerate:true` skips
the prefilter and returns candidates for native regex or other matching that
cannot safely be reproduced in the helper. The Windows client applies the native
search control's matcher to returned paths, including case sensitivity and
regular expressions. It does not substitute a Linux regex dialect. Queries
using case-insensitive non-ASCII text, regex, or uncertain native option state
request enumeration; an invalid expression is rejected before the request.

Returns `{results,processes_scanned,inaccessible_processes,truncated}`. Each result
contains `pid`, `start_ticks`, `process`, `handle`, `type`, and `path`. A handle is
a decimal FD, `cwd`, `exe`, `root`, or a hexadecimal mapping address. Types are
`File`, `Socket`, `Pipe`, `Anonymous inode`, `Working directory`, `Executable`,
`Root directory`, `Executable mapping`, and `Mapped file`. Search mapping types
come from map permissions, not ELF verification. Repeated segments are grouped
by type, device/inode and path within a process. PID/start time is checked again
before publishing that process's rows; later inspections must also validate it.
`inaccessible_processes` counts processes whose FD directory could not be opened.

The scan checks a five-second monotonic deadline between procfs operations, caps
returned candidates at 10,000 and encoded rows at 8 MiB, and caps a process map
file at 4 MiB.
It sets `truncated` when a limit prevents full collection. This is a cooperative
scan deadline; the Windows transport independently enforces its request timeout.
An incomplete result can omit matches, even if the final Windows filter finds
none among the candidates. Searches begin only on an explicit Search action.
The UI's Cancel, query edits, and option changes detach the request mailbox and
ignore a late result. Cancellation does not kill the shared observer or
interfere with another inspector; a request
already running completes under its scan/transport limits. Closing the dialog
also detaches the mailbox before draining posted replies.

### `connections` (optional `pid`, `start_ticks`, `identities_only`, `resolve_names`)

Returns `connections`, `connections_truncated`, `inaccessible_processes`, `tables_read`,
`network_namespace`, and `coverage`.

Each row has `protocol` (`tcp`, `tcp6`, `udp`, `udp6`, or `unix`),
`local_address`, `local_port`, `remote_address`, `remote_port`, `state`, `pid`,
`process`, `start_ticks`, and `inode`. The start time identifies the socket owner
from the ownership scan; PID 0 uses start time 0. Unix socket addresses are paths (including abstract
namespace names), with zero ports and an empty remote address. Stream listeners
use `LISTEN`; bound UDP sockets usually use `UNCONN`.

With `identities_only:true`, rows omit `process` and `state`; all socket and owner
identity fields remain. The response echoes `identities_only`. This avoids
sending display-only metadata for hidden views. The FD and network-table scans
are still necessary to establish socket lifetimes and ownership.

With `resolve_names:true` and `identities_only:false`, each row also has
`remote_hostname`, empty when unknown. The helper validates numeric IPv4/IPv6
addresses and uses `getent hosts` through the trusted command runner, without a
shell. IPv4-mapped IPv6 shares its IPv4 cache key. At most one uncached lookup
runs per request, with a 500 ms command timeout and 4 KiB output limit. Successful
names are cached for 300 seconds and failures for 60 seconds, up to 1,024 entries.
Unspecified/multicast addresses, invalid answers, missing `getent`, and resolver
failures produce no name. No lookups run for identity-only requests. The Windows
client requests names only for a visible Network view when the host's
`EnableNetworkResolve` is enabled and the hostname column is visible. Hiding the
column disables lookups even when that column remains the sort key.

The Windows `HideWaitingConnections` setting filters PID 0 and TCP `CLOSE_WAIT`
rows in main and process-detail tables; it does not change the helper response.

Socket ownership is joined from visible `/proc/PID/fd` entries. Shared sockets
have one row for each owning process; duplicate FDs within a process are collapsed.
PID 0 means no visible owner, which is normal for TCP `TIME_WAIT` sockets. Optional
process filtering returns only sockets owned by the specified identity.

Coverage is explicitly limited to the helper's current network namespace. Sockets
inside another container/network namespace are not discovered. Processes visible
in the PID namespace can still be inaccessible due to procfs/security policy;
`inaccessible_processes` counts failures to open their FD directories, including
processes that disappeared during collection. Ownership and socket tables are
sampled separately, so extremely short-lived sockets can have no matched owner.
Each protocol table is capped at 32 MiB. The encoded connection array is capped
at 12 MiB and reports `connections_truncated:true` when full.

### `signal` (`pid`, `start_ticks`, `signal`)

Allows only SIGTERM (15), SIGINT (2), SIGKILL (9), SIGSTOP (19), SIGCONT (18),
SIGUSR1 (10), SIGUSR2 (12), SIGHUP (1), and SIGWINCH (28) on supported WSL
x86-64/ARM64 Linux targets. Returns `{sent:true}`.
A successful return means the kernel accepted the signal, not that the process
has already exited. Refresh to observe the resulting state.

### `stacks` (`pid`, `start_ticks`, optional `tid`)

Returns `{available,text,message,timed_out?,exit_code?,fallback?,primary_exit_code?}` with GDB user-space
backtraces. If GDB is absent, `available:false` includes an installation hint;
the helper never installs software. PID 1 and the helper itself are protected.
Each thread has at most 64 frames, the capture limit is 256 KiB, and the deadline
is 15 seconds per attempt. An optional Linux TID selects one thread through a fixed GDB Python
expression; this requires a GDB build with Python support. Otherwise all threads
are shown. Missing symbols and ptrace/security restrictions appear in the output.
Frame arguments and entry values are omitted to avoid needless reads of fragile
variable debug information. GDB internal errors are configured not to write core
dumps or wait for interactive confirmation.

If GDB reports a DWARF/split-DWARF reader error or an internal debugger failure,
the helper retries once with `--readnever` and the executable supplied through
`--se`. This retains ELF minimal symbols and shared-library lookup, including
exported function names, while skipping symbolic DWARF debug information. The
fallback also appends a shared-library list. Both ordinary and fallback captures
annotate frame PCs with mapped-file paths and file offsets, replacing `??` when
possible while retaining GDB symbol names and source locations. An annotation is
used only when the complete mapping line is identical before and after capture;
anonymous or changed mappings remain unannotated. Offsets are file offsets, not
ELF virtual addresses or inferred function offsets.
`fallback:true` and `message` disclose the retry, and the original diagnostics are
preserved beneath the fallback trace. Missing DWARF unwind information can make
these traces shorter or less reliable; the result is not advertised as a fully
symbolicated stack. The two attempts can take up to approximately 31 seconds in
total, including command cleanup. Permission errors do not trigger this retry.

The behavior of `--readnever` and executable/symbol selection is documented in
the [GDB file options](https://www.sourceware.org/gdb/current/onlinedocs/gdb.html/File-Options.html)
and [file commands](https://www.sourceware.org/gdb/current/onlinedocs/gdb.html/Files.html).

This operation attaches a debugger and briefly stops the target's threads. It
must be an explicit inspection action, not an automatic refresh. GDB accepts a
numeric PID, so unlike signal actions this attachment cannot be made atomically
against a pidfd identity: identity is checked immediately before and after, but
a narrow PID-reuse race remains. A successful post-check does not guarantee an
atomic snapshot. Do not describe this operation as passive or pidfd-safe.

GDB runs only from trusted system locations (`/usr/local/bin`, `/usr/bin`, or `/bin`), with init files and auto-loading disabled,
index-cache writes disabled, thread-debugging libraries restricted to GDB system
directories, debuginfod disabled, an empty `DEBUGINFOD_URLS`, a clean environment, and `/` as its
working directory. No caller-provided GDB commands are accepted. The batch ends
with an explicit detach; on timeout the debugger process group is terminated,
which releases ptrace ownership. It does not send SIGCONT to the target itself.

Routine GDB thread/attach announcements are suppressed. When a backtrace is
present, startup/source warnings follow it under `GDB diagnostics`; failed
attachments without frames retain their diagnostic output.

### `script_stacks` (`pid`, `start_ticks`)

Captures runtime-level stacks for a process identified by `pid` and `start_ticks`.
Runtime selection uses the basename of the resolved `/proc/PID/exe`, never the
command line or script name. The response's `runtime` is `node`, `python`, or
`java`; unrecognized executables return an empty value. Shell/npm wrappers,
PyPy, renamed executables, and embedded runtimes do not automatically match.
Details are refreshed to add, rename, or remove the matching process inspector
tab after an `exec()` changes the runtime.

For Node.js, the optional `backend` field accepts `auto` (default), `inspector`,
or `llnode`. `enable_inspector:true` is honored only with `backend:"inspector"`;
it records the user's explicit choice to activate an Inspector that is not
already listening. With `auto`, a listener owned by the selected process is
preferred. The helper verifies the socket inode belongs to that process and
checks the Inspector endpoint's reported PID before capture. It captures the
main JavaScript thread and up to 32 reported worker contexts, with up to 256
JavaScript frames per context. Contexts are sampled sequentially, not as one
simultaneous snapshot. Each context gets up to one second to reach a JavaScript
pause point. Idle threads are skipped, and a pending pause is canceled so it
cannot stop the process later. Native frames and asynchronous promise/task
history are not included.

When `backend` is `auto` or `inspector`, activation was not requested, and no
Inspector listener was discovered, Node returns `choice_required:true` so the UI
can offer **Temporarily Enable Inspector**, **Use llnode**, or **Cancel**. This
is a backend choice, not a capture confirmation. When Inspector activation is
available, the choice includes a **Don't show again** checkbox; checking it and
choosing temporary activation sets the per-user `UseNodeInspectorWithoutAsking`
preference. If Python 3 is missing, the helper also returns
`inspector_unavailable:true` with an installation explanation; the UI then
offers **Use llnode** or **Cancel**, without the Inspector activation choice or
remember-preference checkbox. This llnode-only choice remains available even if
`UseNodeInspectorWithoutAsking` is set, because Inspector activation cannot run
without the embedded Python client.
An explicit `backend:"llnode"` request skips Inspector discovery and capture.
Enabling Inspector uses SIGUSR1 through a pidfd after
rechecking the selected Node executable and process identity. Before doing so,
the helper checks command-line arguments and `NODE_OPTIONS` for recognizable
non-loopback or unverified bind hosts and declines activation if it finds one.
Runtime changes to `debugPort` and custom signal handlers cannot be inferred from
`/proc`; the socket and endpoint ownership checks still have to succeed. If two
processes both request the default port 9229, the second cannot use that occupied
port and the helper does not attach to the first process by mistake. Configure
`--inspect-port=0` to let Node choose an available port for each process.

The helper disconnects its own Inspector WebSocket client after capture. If this
request created the listener, it resumes only pauses it requested, detaches worker
sessions, requests `inspector.close()` with feature detection, disconnects, and
checks whether the listener disappeared. It records listener inodes before
activation and never closes a pre-existing listener. If another client is
detected during cleanup, the temporary listener is left enabled. A client can
still race that check; Node does not provide atomic close-if-alone. Node's close
operation can disconnect concurrent debugger clients, so avoid attaching another
debugger during capture. If resume or closure cannot be confirmed, cleanup
warnings remain at the start of `message` and `text`, and `fallback_safe:false`
prevents a second debugger attach. Unsupported `inspector.close()` and an older
ESM runtime or busy event loop can leave the listener enabled; the result says so.

Inspector is a code-execution interface; keep listeners on loopback. Loopback limits remote
exposure, but local programs in the distro can still connect. See Node's
[Inspector API](https://nodejs.org/api/inspector.html), [Inspector options](https://nodejs.org/api/cli.html#--inspectporthostport),
and [debugging security guidance](https://nodejs.org/learn/getting-started/debugging#security-implications).

The Inspector client is embedded in `wsl-observer` and runs with the distro's
system Python 3 using `-I -S`. It uses only Python's standard library: no extra
Python packages or external script deployment are required. If system Python 3
is unavailable, the `auto`/`inspector` backend returns `choice_required:true`
and `inspector_unavailable:true` with an installation message; a protocol caller
can still request `backend:"llnode"` directly.
The client allows a 12-second capture phase plus up to 2 seconds for cleanup,
with a 20-second observer command timeout and a 35-second Windows request
deadline. Captured text is limited to 512 KiB.

For a main or worker thread paused by this capture, an exact singleton
`processTimers` frame triggers short resampling: resume, wait 10 ms, then pause
the same session again. Retries share a 200 ms budget clipped by the existing
capture deadline with 150 ms reserved for cleanup. The first nonempty non-timer
stack is kept; otherwise the latest actual timer stack remains in the output.
Threads already paused before capture are never resumed for resampling. A retry
that cannot reach a pause point uses the existing pending-pause cancellation.

If an Inspector attempt fails and cleanup is safe, the observer tries llnode
automatically. That response sets `fallback:true`, includes the original failure
as `inspector_error`, and retains both diagnostics in `text`. If Inspector
cleanup is uncertain (`fallback_safe:false`), no second debugger is attached.

The llnode backend uses LLDB and a compatible `llnode.so` plugin. Build llnode
for the installed LLDB version as a normal user, then have an administrator place
the root-owned, non-group/world-writable plugin at `/usr/local/lib/llnode/llnode.so`,
`/usr/lib/lldb/plugins/llnode.so`, `/usr/local/lib/node_modules/llnode/llnode.so`,
or `/usr/lib/node_modules/llnode/llnode.so`. Never run npm as root. See the
[llnode installation instructions](https://github.com/nodejs/llnode#install-instructions).
LLDB 18 with llnode 4 does not reliably decode JavaScript names for Node.js 22;
fallback output may contain partial V8 data or native addresses.

Python executables `python`, `python2`, `python3`, and versioned/debug/free-threaded
CPython names select Python. The helper uses a trusted, root-owned `py-spy` in
`/usr/local/bin` or `/usr/bin`. It dumps all Python thread stacks without local
variable values. PyPy is not recognized. See [py-spy](https://github.com/benfred/py-spy).

`java` selects a JVM. The helper uses `jcmd` beside the target JVM's `java`, so
the tool matches that JDK. It executes `jcmd PID Thread.print -l` with the target
process's effective UID and GID, even though the observer runs as root. Traditional
thread dumps include locks but not every unmounted virtual thread. The full
matching JDK is needed if a custom JRE does not include `jcmd`. See Oracle's
[`jcmd` reference](https://docs.oracle.com/en/java/javase/21/docs/specs/man/jcmd.html).

The stack pages are passive; their capture buttons and Ctrl+R start collection
without a separate confirmation dialog. Node's method-choice dialog appears only
when the automatic Inspector route has no verified listener. The page text
explains the button, possible pause, and tool requirements. Java capture can
pause threads at a JVM safepoint. Native GDB, py-spy, and llnode can briefly
pause the target. Tools and runtime versions must be installed and compatible;
the plugin does not install them. Ptrace restrictions, disabled JVM attachment,
or missing debugging metadata can limit or prevent a capture.

The operation's common response data includes `runtime`, `supported`, `success`,
`text`, and `message`. `tool`, `timed_out`, `exit_code`, `choice_required`,
`inspector_unavailable`, `fallback`, `fallback_safe`, and `inspector_error` are
conditional on the backend and outcome. `supported:false` means the requested
backend cannot run; `choice_required` may still offer another backend. A present
but incompatible debugger can return `supported:true`, `success:false`, and
partial diagnostics. Node cleanup warnings are included in `message` and, when
cleanup is uncertain, prepended to `text`. Safe automatic llnode fallback keeps
the Inspector failure in `inspector_error`. A timeout or output limit can leave
partial text.

```json
{"id":4,"op":"script_stacks","pid":123,"start_ticks":4567}
{"id":4,"ok":true,"data":{"runtime":"node","tool":"Node Inspector","supported":true,"success":false,"choice_required":true,"message":"No Inspector listener owned by this Node process was found. Choose whether to enable Inspector for this capture or use llnode instead.","text":""}}
{"id":5,"op":"script_stacks","pid":123,"start_ticks":4567}
{"id":5,"ok":true,"data":{"runtime":"node","supported":false,"success":false,"choice_required":true,"inspector_unavailable":true,"message":"Node Inspector capture requires Python 3 in this distribution.","text":""}}
{"id":6,"op":"script_stacks","pid":123,"start_ticks":4567,"backend":"inspector","enable_inspector":true}
{"id":6,"ok":true,"data":{"runtime":"node","tool":"Node Inspector","supported":true,"success":true,"choice_required":false,"message":"Captured the main JavaScript thread through Node Inspector. The Inspector listener enabled for this capture was closed.","text":"Node Inspector: main JavaScript thread\n\n#0 main at app.js:10:1"}}
```

Debugger tools attach by numeric PID. The helper checks PID/start-time identity
and the executable before and after capture, but this cannot eliminate the narrow
PID-reuse race during attachment.

### `services` (optional `identities_only`, `include_pids`, `refresh_metadata`)

Returns `available`, `services`, `message`, `identities_only`, `include_pids`,
`pids_complete`, and `services_truncated` when systemd is available. Each service
has `name`, `description`, `load`, `active`, `sub`, and `enabled` (unit-file state,
such as `enabled`, `disabled`, `static`, `masked`, or `unknown`). Loaded units and
installed service unit files are merged by name. Installed units that are not
loaded have `load:"not loaded"`, `active:"inactive"`, and `sub:"dead"`; their
description is empty until systemd loads them. Template and alias unit files are
retained. When systemd is not running, `{available:false,services:[],message}` is
a normal result.

`include_pids:true` adds `pid` (systemd MainPID) and `start_ticks` for navigation.
The helper obtains state and MainPID together in one `systemctl show` request
for loaded service units, then checks the process start time. Zero values mean
that no live identity was established. `pids_complete` describes the MainPID
query, not whether every service has a running process. If that query fails,
ordinary unit enumeration can still return the services with a warning.

Installed unit-file/startup metadata is cached for 30 seconds. A failed refresh
retains previous metadata, reports an incomplete collection, and retries after
five seconds. `refresh_metadata:true` bypasses the cache; successful enable or
disable actions invalidate it. Loaded-unit state is collected on every request.
The helper prefers systemctl JSON output where applicable and falls back to its
stable leading columns with C locale, no legend, full names, and plain output.
The encoded service array has a 12 MiB budget. `services_truncated:true` also
marks incomplete loaded-unit or installed-unit enumeration, so clients must not
interpret absent rows as removed.

With `identities_only:true`, service rows contain only `name`; `include_pids` is
forced false. This protocol mode remains available, but the Windows client does
not poll Services in the background. It requests services only when the Services
subtab is visible in the active WSL tab and the host is neither minimized nor
hidden. Reopening Services or manually refreshing requests fresh metadata.

### `service_details` (`name`)

Returns `{text,journal,overview}`. `overview` has the string fields `name`, `description`,
`load`, `active`, `sub`, `enabled`, `main_pid`, `fragment_path`, `exec_start`,
`user`, `group`, `restarts`, `result`, `active_since`, `memory_current`, and
`tasks_current`. These retain systemd property values, including unknown or
unlimited markers; unavailable properties are empty. Each overview value has a
16 KiB display limit. The properties are extracted from the existing show result
without another subprocess.

`text` contains systemctl status, all properties, unit file contents and
drop-ins. Status uses `--lines=0` so it does not duplicate recent logs. `journal`
contains the last 100 journal entries (`short-iso` format) for the separate
Journal tab. Each of the four command outputs has a 128 KiB display limit with
a visible truncation marker. Failed/inactive status is valid detail output.
Journal permission failures appear in `journal`.

For bare templates (`name` ending in `@.service`), the helper does not call
runtime `systemctl show` or `status`. It obtains the unit definition with `cat`,
the installed startup state with `list-unit-files`, and recent journal entries
for the instance pattern (for example, `getty@*.service`). `overview.is_template:true`
identifies this response; its other fields explain that there is no runtime
instance or PID, and specifiers remain unexpanded. `text` contains the template
explanation and unit definition. This path makes three commands instead of four.

### `service_action` (`name`, `action`)

Allows `start`, `stop`, `restart`, `reload`, `enable`, or `disable`. Names must be
valid `.service` unit identifiers, never shell syntax. Returns `{accepted:true,
message}` after systemctl accepts the request. Start/stop jobs are queued with
`--no-block`; refresh to inspect completion or failure. Enable/disable changes
boot activation and does not imply an immediate start/stop.
Bare templates reject start/stop/restart/reload: those require a named instance.
Enable/disable remain allowed and follow systemd's template installation rules.

System tools run from trusted `/usr/bin` or `/bin` paths with explicit argv and
a minimal environment. Caller PATH, bus-address, loader, and pager overrides are
not inherited. They use no shell, no interactive password prompt,
no pager, and C locale. Output is limited to 2 MiB per command. Read-only commands
have a 5-second deadline each; service actions have a 10-second deadline. A
service detail query makes four sequential commands and can therefore take up to
20 seconds. On timeout the helper kills the command process group and reaps the child. A
child stuck in an uninterruptible kernel wait is reaped on a later command, so it
cannot indefinitely block the transport. A service
job already accepted by systemd can continue independently.
