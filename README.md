# lmk_engine

A custom userspace low-memory killer daemon for Android, written in pure C with no external dependencies. Runs alongside (and largely supersedes) the stock PSI-based `lmkd`, managing app retention, ZRAM pressure, cgroup v2 memory controls, and kill decisions.

Built and tuned on an Infinix Hot 20 Play (MT6768/Helio G85, ~3.8GB RAM), kernel 4.14.141, running an Axion Android 16 GSI on an Android 10 vendor base. Only tested on this configuration — porting to another device means re-checking the device-specific bits called out in `docs/ARCHITECTURE.md`.

## What it does

- **Tiered adaptive reclaim** — escalates gradually through pressure tiers (idle housekeeping → light kills → deep clean) instead of one fixed kill policy, driven by ZRAM fullness, available RAM, and PSI memory stall.
- **Retention scoring & pinning** — ranks apps by real usage (foreground time, session frequency, recency) and protects the top set via `cgroup v2 memory.low`, with a short kill-exemption window layered on top.
- **Proactive squeeze** — pushes reclaimable pages out of background apps via `memory.high` before resorting to a kill, scaled to how much ZRAM headroom is actually available.
- **kswapd core pinning** — keeps kernel reclaim off the big cores under normal pressure so it doesn't compete with foreground UI rendering; releases the pin automatically as pressure climbs.
- **External-kill & unattributed-death tracking** — taps `logcat` and presence-based detection to attribute kills the daemon didn't cause (AMS, Phantom Process Killer, other actors), so retention/restart stats reflect the whole picture, not just this daemon's own decisions.
- **avail_pct-tiered VM tuning** — adjusts `extra_free_kbytes`/`watermark_scale_factor` based on real available-RAM headroom rather than ZRAM% alone.

No on-device machine learning — the earlier learned-weight scoring model (`AI_Swap`) was removed; retention scoring is now a deterministic, tunable formula plus dwell/hysteresis gates. See `docs/ARCHITECTURE.md` for the full internals and `CHANGELOG.md` for how this evolved.

## Requirements

- Rooted Android device (Magisk)
- ARM64 (or ARM), root access
- clang or gcc to build
- cgroup v2 with the `memory` controller (check `cat /sys/fs/cgroup/cgroup.controllers`)

## Building

```bash
clang -O2 -Wall -Wextra -o lmk_engine src/lmk_engine.c -lm
```

Recommended pre-flash check on the build host:

```bash
gcc -O2 -Wall -Wextra -Wshadow -Wunused -Wuninitialized -fsyntax-only src/lmk_engine.c
```

On-device, strip the rpath before flashing:

```bash
patchelf --remove-rpath lmk_engine
```

## Installing

Flash a release zip from [Releases](../../releases), or zip the `module/` folder yourself and flash it via Magisk Manager, then reboot.

## Usage

It works automatically after flashing the module, but you can run this in the terminal for diagnostics:

```
lmk_engine --start          start the daemon
lmk_engine --stop           stop the daemon
lmk_engine --status         show memory/ZRAM/swap/pin status
lmk_engine --log            tail the live log
```

## Known limitations

This device can't hot-swap ZRAM's compression backend or reliably complete a full `swapoff`/`swapon` cycle under real pressure — see `docs/ARCHITECTURE.md` §6 and `CHANGELOG.md` for why, and what the daemon does instead (kill/squeeze-based management rather than periodic ZRAM defragmentation).

## License

MIT — see `LICENSE`.
