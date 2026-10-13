# Changelog

## 0.1.1 - 2026-10-13

- Automatically retry WSL collector connections after unexpected transport errors, including timeouts after resume from hibernation. Retries wait five seconds, run at most ten times, respect automatic refresh and background-capture settings, and stop when WSL reports the distribution is shut down.
- Never replay failed process or service actions automatically; a timed-out action may already have taken effect.

## 0.1.0

- Initial version.
