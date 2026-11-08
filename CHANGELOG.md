# Changelog

## 0.2.0 - 2026-11-08

- Add paired WSLg system-distro targets using the `:system` suffix, with the same process, thread, memory, module, environment, handle, network, signal, scheduling and stack inspectors where supported.
- Stage and verify a temporary observer for WSLg at connection time; it removes itself on startup. Normal distros retain their persistent installed companion.
- Keep WSLg path opening separate from normal distro Explorer overrides; services are reported unavailable when systemd is absent.

## 0.1.2 - 2026-10-18

- Open Find strings in memory on the Details tab by default.
- Show the parent process as `Name (PID)` in process properties instead of just its PID.
- Move exported symbols, imported symbols, and declared dependencies into tabs in module properties, loaded on demand.
- Immediately refresh when returning to the WSL tab, unless automatic refresh is disabled.
- Shorten the README and move detailed usage and build documentation into linked guides.
- Prevent rapid focus refreshes from replacing meaningful CPU/I/O rates with short-interval readings or distorting CPU averages and graph history.
- Keep Escape working across plugin dialogs, including during asynchronous loading.

## 0.1.1 - 2026-10-13

- Automatically retry WSL collector connections after unexpected transport errors, including timeouts after resume from hibernation. Retries wait five seconds, run at most ten times, respect automatic refresh and background-capture settings, and stop when WSL reports the distribution is shut down.
- Never replay failed process or service actions automatically; a timed-out action may already have taken effect.

## 0.1.0

- Initial version.
