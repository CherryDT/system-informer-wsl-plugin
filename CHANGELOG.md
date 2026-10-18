# Changelog

## 0.1.2 - 2026-10-18

- Open Find strings in memory on the Details tab by default.
- Show the parent process as `Name (PID)` in process properties instead of just its PID.
- Move exported symbols, imported symbols, and declared dependencies into tabs in module properties, loaded on demand.
- Immediately refresh when returning to the WSL tab, unless automatic refresh is disabled.
- Shorten the README and move detailed usage and build documentation into linked guides.

## 0.1.1 - 2026-10-13

- Automatically retry WSL collector connections after unexpected transport errors, including timeouts after resume from hibernation. Retries wait five seconds, run at most ten times, respect automatic refresh and background-capture settings, and stop when WSL reports the distribution is shut down.
- Never replay failed process or service actions automatically; a timed-out action may already have taken effect.

## 0.1.0

- Initial version.
