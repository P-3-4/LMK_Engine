# lmk_engine

A custom userspace low-memory killer daemon for Android, written in pure C with no external dependencies. Built for devices where the stock LMKD/memory management doesn't behave well under real-world pressure.

Originally developed and tuned on a MediaTek Helio G85 (MT6768) device with ~3.8GB RAM, running an Android 16 GSI over an Android 10 vendor base — packaged as a Magisk module.

## What it does

- **Adaptive_Clean** — tiered kill/reclaim system (T0–T3, MAINT, IDLE, IDLE-DEEP) that responds to memory pressure in stages rather than killing everything at once
- **AI_Swap** — a scoring/retention subsystem that learns which apps you actually use and protects them from being killed, instead of relying on raw recency alone
- **Rank system** — classifies processes as EXEMPT / FOREGROUND / HOT / WARM / COLD / STALE
- **Priority system** — NEVER / SEMI_PROTECTED / LAUNCHER / BACKGROUND / JUNK, so system-critical and user-critical processes are never touched
- **On-device learning** — logs outcomes to `lmk_learn.log` and retrains its own scoring weights, no Python or external ML libs required

## Why

Stock Android's low-memory killer doesn't adapt to usage patterns, doesn't distinguish "app you use daily" from "app you opened once," and often kills persistent system services that just get relaunched anyway — burning battery and causing jank. `lmk_engine` replaces that with something that observes, scores, and learns.

## Requirements

- Rooted Android device (Magisk)
- ARM64 Android device (tested on MT6768 / Android 16 GSI)
- Enough headroom to compile with clang (Termux) or cross-compile

## Building

```bash
clang -O2 -Wall -Wextra -o lmk_engine src/lmk_engine.c
```

## Installing

Flash the Magisk module zip from `releases/`, or build the module folder yourself:

```bash
cd module/
zip -r lmk_engine_module.zip .
```

Then flash via Magisk Manager and reboot.

## Runtime files

The daemon persists state at `/data/local/tmp/`:
- `lmk_scores.dat` — app retention scores
- `lmk_rank.cache` — rank cache
- `lmk_learn.log` — learning history

These are generated on-device and are **not** part of the repo — they're specific to your usage patterns.

## Known device-specific quirks

This project was built around real quirks found on a specific GSI/vendor combo — for example, some builds report screen-off state as `Dozing` rather than `Asleep` in `dumpsys power`. If you're porting this to another device, check your own `dumpsys power` output before assuming the screen-off detection works as-is.

## Status

Actively developed. See `CHANGELOG.md` for version history.

## License

See `LICENSE`.

## Disclaimer

This modifies core memory-management behavior on your device. Use at your own risk, keep backups, and test on a device you're comfortable experimenting with.
