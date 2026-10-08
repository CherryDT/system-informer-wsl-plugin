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

### `snapshot`

Returns:

- `processes`: objects with `pid`, `ppid`, `start_ticks`, `name`, `state`, `user`,
  `uid`, `threads`, `cpu_ticks`, `rss_bytes`, `virtual_bytes`, `read_bytes`,
  `write_bytes`, `io_accessible`, `status_accessible`, `command`, `exe`, and
  `runtime`. `runtime` is `"node"`, `"python"`, `"java"`, or an empty string
  when the resolved executable is not a recognized runtime.
- `processes_truncated`: true if the encoded process array reached its 12 MiB budget.
- `monotonic_ms`: helper monotonic time, sampled at the end of collection.
- `uptime_seconds`, `memory_total`, `memory_available`, `cpus`, `clock_ticks`, `boot_id`.
  Memory and pressure values describe the shared WSL VM, not just this distro.
- `loadavg`: Linux load-average text; `pressure`: CPU, memory, and I/O pressure
  text, where the kernel provides it.

`cpu_ticks` is user plus system CPU time for this process, excluding reaped child
CPU time. Compute one-core usage as
`100 * delta(cpu_ticks) / clock_ticks / delta(seconds)`. Dividing by `cpus` gives
a fraction of the guest's capacity. Neither value is host VM CPU attribution.
Negative deltas, a changed boot ID, or a changed process identity reset the sample.
RSS and virtual memory are byte counts, and I/O counters are cumulative byte
counts. Missing I/O permission produces `io_accessible:false` with zero counters. An
unreadable status file produces `status_accessible:false`, user `unknown`, and
UID 4294967295 instead of incorrectly reporting root.
Short-lived processes may disappear during collection and are omitted. Usernames
come from the local `/etc/passwd`; other UIDs remain numeric, so inspection never
blocks on network name services. Command lines are capped at 16 KiB per process.

### `details` (`pid`, `start_ticks`)

Returns:

- `overview`: current process snapshot fields (including `runtime`), plus `cwd`, `cgroup`,
  `capabilities` (effective, permitted, inheritable, bounding, and ambient masks),
  `seccomp` (`Disabled`, `Strict`, or `Filter`), and `no_new_privs` (`Yes` or `No`).
  Missing status fields remain empty; numeric fields retain snapshot types.
- `summary`: human-readable status, I/O counters, cgroups, resource limits,
  namespace IDs, executable path, and current working directory. Status includes
  UIDs/GIDs, capability masks, seccomp state, and other kernel-provided fields.
- `files`: `{fd,target,flags}`. `flags` is the original octal `/proc` flag string.
  Targets may be paths, sockets, pipes, anonymous handles, or deleted paths.
- `modules`: `{path,start,end,permissions}` for named memory mappings. Start/end
  are hexadecimal strings. A file may have several segments; bracketed kernel
  labels such as `[heap]` are retained and are not filesystem paths.
- `environment`: `{name,value}` pairs. Ordering and duplicate names are retained.
- `threads`: `{tid,name,state,wchan}` entries.
- `files_accessible`, `modules_accessible`: availability indicators.
- `files_truncated`, `modules_truncated`, `environment_truncated`,
  `threads_truncated`, `summary_truncated`: display-limit indicators. A visible
  notice is also appended to `summary` when any part was truncated.

The environment can contain credentials and other secrets; the UI should expose
it only through explicit inspection, without automatic logging. Inspection is a
best-effort snapshot, not a frozen view of the process. Maps are capped at 8 MiB,
environment at 4 MiB, and ordinary proc files at 1 MiB. Encoded files, modules,
and environment arrays each have a 1 MiB budget; threads have 512 KiB. Summary
text is capped at 256 KiB. These limits keep detail responses below the Windows
transport limit even when paths contain characters requiring JSON escapes.
A partial final environment value is omitted instead of presenting it as complete.

### `connections` (optional `pid`, `start_ticks`)

Returns `connections`, `connections_truncated`, `inaccessible_processes`, `tables_read`,
`network_namespace`, and `coverage`.

Each row has `protocol` (`tcp`, `tcp6`, `udp`, `udp6`, or `unix`),
`local_address`, `local_port`, `remote_address`, `remote_port`, `state`, `pid`,
`process`, `start_ticks`, and `inode`. The start time identifies the socket owner
from the ownership scan; PID 0 uses start time 0. Unix socket addresses are paths (including abstract
namespace names), with zero ports and an empty remote address. Stream listeners
use `LISTEN`; bound UDP sockets usually use `UNCONN`.

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

Allows only SIGTERM (15), SIGKILL (9), SIGSTOP (19), SIGCONT (18), SIGUSR1 (10), and
SIGUSR2 (12), SIGHUP (1), and SIGWINCH (28) on supported WSL x86-64/ARM64 Linux targets. Returns `{sent:true}`.
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
fallback also appends module/file-offset annotations and a shared-library list.
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

GDB runs only from `/usr/bin` or `/bin`, with init files and auto-loading disabled,
index-cache writes disabled, thread-debugging libraries restricted to GDB system
directories, debuginfod disabled, an empty `DEBUGINFOD_URLS`, a clean environment, and `/` as its
working directory. No caller-provided GDB commands are accepted. The batch ends
with an explicit detach; on timeout the debugger process group is terminated,
which releases ptrace ownership. It does not send SIGCONT to the target itself.

### `script_stacks` (`pid`, `start_ticks`)

Captures runtime-level stacks using tools installed by the distro administrator;
the observer does not install them. Runtime selection is based on the basename of
the resolved `/proc/PID/exe`, never the command line or script name:

- `node` and `nodejs` select Node.js. The helper uses LLDB with a compatible
  `llnode.so` plugin from a trusted plugin location. LLDB and llnode must be
  installed by the user; llnode must be built for that LLDB version and support
  the target Node/V8 version. Build llnode as a normal user, then have an
  administrator place the root-owned, non-group/world-writable plugin at
  `/usr/local/lib/llnode/llnode.so`, `/usr/lib/lldb/plugins/llnode.so`,
  `/usr/local/lib/node_modules/llnode/llnode.so`, or
  `/usr/lib/node_modules/llnode/llnode.so`. Never run npm as root. See the
  [llnode installation instructions](https://github.com/nodejs/llnode#install-instructions).
- `python`, `python2`, `python3`, and versioned/debug/free-threaded CPython
  executable names select Python. The helper uses a trusted, root-owned
  `py-spy` in `/usr/local/bin` or `/usr/bin`. It dumps all Python thread stacks
  without local variable values. See [py-spy](https://github.com/benfred/py-spy).
  PyPy is not recognized.
- `java` selects a JVM. The helper uses `jcmd` beside the target JVM's `java`
  executable, so it matches that target's JDK. It runs `jcmd PID Thread.print -l`
  with the JVM's effective UID and GID, even though the observer runs as root.
  The full matching JDK is needed when a custom JRE does not include `jcmd`.
  See Oracle's [`jcmd` reference](https://docs.oracle.com/en/java/javase/21/docs/specs/man/jcmd.html).

Shell/npm wrappers, PyPy, renamed executables, and embedded runtimes do not
automatically match. A recognized runtime can still return `supported:false`
when its required debugger is missing or untrusted. A present debugger that is
incompatible may instead return `supported:true` and an unsuccessful, partial
capture. For example,
LLDB 18 with llnode 4 does not reliably decode JavaScript names for Node.js 22;
the result may contain partial V8 data and native addresses rather than useful
JavaScript frames.

The response data contains `runtime`, `supported`, `success`, `text`, and
`message`. When the capture tool is available it also contains `tool`,
`timed_out`, and `exit_code`. Those tool and process-result fields are omitted
when no capture tool can be selected. `supported` means the runtime and required
tool are available for an attempt; it does not promise complete symbols or a
successful attach. A completed attempt can have `success:false` while returning
partial diagnostic text. Missing tools return `supported:false`, `success:false`,
empty `text`, and an installation or compatibility explanation in `message`.

```json
{"id":4,"op":"script_stacks","pid":123,"start_ticks":4567}
{"id":4,"ok":true,"data":{"runtime":"python","tool":"py-spy","supported":true,"success":true,"timed_out":false,"exit_code":0,"message":"Captured Python thread stacks. Local variable values are not collected.","text":"Thread 123: ..."}}
```

Captures are explicit because debugger attachment may pause the target briefly;
JVM attachment may request a safepoint. There is no automatic Node inspector,
runtime signal, or periodic capture. The helper checks PID/start-time identity
and the executable immediately before and after the command, but debugger tools
attach by numeric PID, leaving a narrow PID-reuse race during attachment. Each
capture has a 15-second deadline and a 512 KiB output limit. Node.js capture is
limited to 256 OS threads and 64 frames per thread, and does not provide
asynchronous promise/task history. Python captures all Python threads but no
locals. Java `Thread.print -l` includes locks, but traditional thread dumps do
not show every unmounted virtual thread. Ptrace restrictions, disabled JVM
attachment, missing symbols, or runtime/tool version mismatches can make a
capture incomplete or unavailable.

### `services`

Returns `{available,services,message?}`. Each service has `name`, `description`,
`load`, `active`, `sub`, and `enabled` (unit-file state, such as `enabled`,
`disabled`, `static`, `masked`, or `unknown`). Loaded units and installed service
unit files are merged by name. Installed units that are not loaded have
`load:"not loaded"`, `active:"inactive"`, and `sub:"dead"`; their description is
empty until systemd loads them. Template and alias unit files are retained. When systemd is
not running, `available:false` is a normal result.

The helper prefers systemctl JSON output and falls back to parsing its stable
leading columns using C locale, no legend, full names, and plain output. A failed
unit-file query preserves loaded-unit results and adds a warning in `message`.
On old systemd versions, the two queries and their fallbacks can take up to
20 seconds total.

### `service_details` (`name`)

Returns `{text,overview}`. `overview` has the string fields `name`, `description`,
`load`, `active`, `sub`, `enabled`, `main_pid`, `fragment_path`, `exec_start`,
`user`, `group`, `restarts`, `result`, `active_since`, `memory_current`, and
`tasks_current`. These retain systemd property values, including unknown or
unlimited markers; unavailable properties are empty. Each overview value has a
16 KiB display limit. The properties are extracted from the existing show result
without another subprocess.

`text` contains systemctl status, all properties, unit file contents and
drop-ins, and the last 100 journal entries. Each of the four text sections has a
128 KiB display limit with a visible truncation marker. Failed/inactive status is valid detail
output. Journal permission failures are included in the detail text.

### `service_action` (`name`, `action`)

Allows `start`, `stop`, `restart`, `reload`, `enable`, or `disable`. Names must be
valid `.service` unit identifiers, never shell syntax. Returns `{accepted:true,
message}` after systemctl accepts the request. Start/stop jobs are queued with
`--no-block`; refresh to inspect completion or failure. Enable/disable changes
boot activation and does not imply an immediate start/stop.

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
