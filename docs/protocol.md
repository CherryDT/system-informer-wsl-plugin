# WSL observer protocol, version 1

`wsl-observer` is a persistent Linux process. The Windows plugin launches one
helper in the selected distribution and communicates through redirected stdin
and stdout. It runs as root by default for complete process inspection; it never
executes shell expressions. Closing stdin ends the helper. No TCP listener or
Windows service is needed.

Every message is one UTF-8 JSON object followed by a newline (NDJSON). The helper
sends exactly one response for each complete input line and never emits unsolicited
messages. Requests are handled sequentially. The maximum request length is 1 MiB;
an oversized line is drained and rejected. Invalid UTF-8 in Linux names is
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
`clock_ticks` (ticks per second), and `systemd` (whether systemd is running).

### `snapshot`

Returns:

- `processes`: objects with `pid`, `ppid`, `start_ticks`, `name`, `state`, `user`,
  `uid`, `threads`, `cpu_ticks`, `rss_bytes`, `virtual_bytes`, `read_bytes`,
  `write_bytes`, `io_accessible`, `status_accessible`, `command`, and `exe`.
- `monotonic_ms`: helper monotonic time, sampled at the end of collection.
- `uptime_seconds`, `memory_total`, `memory_available`, `cpus`, `boot_id`.
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
Short-lived processes may disappear during collection and are omitted.

### `details` (`pid`, `start_ticks`)

Returns:

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

The environment can contain credentials and other secrets; the UI should expose
it only through explicit inspection, without automatic logging. Inspection is a
best-effort snapshot, not a frozen view of the process. Maps are capped at 8 MiB,
environment at 4 MiB, and ordinary proc files at 1 MiB.

### `connections` (optional `pid`, `start_ticks`)

Returns `connections`, `inaccessible_processes`, `tables_read`,
`network_namespace`, and `coverage`.

Each row has `protocol` (`tcp`, `tcp6`, `udp`, `udp6`, or `unix`),
`local_address`, `local_port`, `remote_address`, `remote_port`, `state`, `pid`,
`process`, and `inode`. Unix socket addresses are paths (including abstract
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
Each protocol table is capped at 32 MiB.

### `signal` (`pid`, `start_ticks`, `signal`)

Allows only SIGTERM (15), SIGKILL (9), SIGSTOP (19), SIGCONT (18), SIGUSR1 (10), and
SIGUSR2 (12) on supported WSL x86-64/ARM64 Linux targets. Returns `{sent:true}`.
A successful return means the kernel accepted the signal, not that the process
has already exited. Refresh to observe the resulting state.

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

Returns `{text}` with systemctl status, all properties, unit file contents and
drop-ins, and the last 100 journal entries. Failed/inactive status is valid detail
output. Journal permission failures are included in the detail text.

### `service_action` (`name`, `action`)

Allows `start`, `stop`, `restart`, `reload`, `enable`, or `disable`. Names must be
valid `.service` unit identifiers, never shell syntax. Returns `{accepted:true,
message}` after systemctl accepts the request. Start/stop jobs are queued with
`--no-block`; refresh to inspect completion or failure. Enable/disable changes
boot activation and does not imply an immediate start/stop.

System tools run with explicit argv, no shell, no interactive password prompt,
no pager, and C locale. Output is limited to 2 MiB per command. Read-only commands
have a 5-second deadline each; service actions have a 10-second deadline. A
service detail query makes four sequential commands and can therefore take up to
20 seconds. On timeout the helper kills the command process group, but a service
job already accepted by systemd can continue independently.
