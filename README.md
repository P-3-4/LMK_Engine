# lmk_engine

A custom userspace low-memory killer daemon for Android, written in pure C with no external dependencies.

Built and tuned using a MediaTek Helio G80 (MT6768) device, ~3.8GB RAM.
Only tested on AOSP ROMs.

## What it does

- **Adaptive_Clean** — tiered memory/ZRAM reclaim that escalates gradually under pressure instead of killing everything at once
- **AI_Swap** — learns which apps you actually use and protects them from being killed, instead of relying on raw recency alone
- **On-device learning** — no Python or external ML libs required

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for full internals, function reference, and tuning constants.

## Requirements

- Rooted Android device (Magisk)
- ARM64, root access
- clang or gcc to build

## Building

```bash
clang -O2 -Wall -Wextra -o lmk_engine src/lmk_engine.c -lm
```

## Installing

Flash a release zip from [Releases](../../releases), or zip the `module/` folder yourself and flash it via Magisk Manager, then reboot.

## Usage
It works automatically after flashing the module, but you can run this in ithe terminal for functions 

```
lmk_engine --start          start the daemon
lmk_engine --stop           stop the daemon
lmk_engine --status         show memory/ZRAM/swap status
lmk_engine --log            tail the live log
```

## Runtime files

Generated on-device at `/data/local/tmp/`:
`lmk_scores.dat`, `lmk_rank.cache`, `lmk_learn.log`

## Device-specific notes

Built around real quirks on this specific GSI/vendor combo (e.g. screen-off reporting as `Dozing` rather than `Asleep`). If porting to another device, check your own `dumpsys power` output before assuming detection works as-is — see the architecture doc for details.
Experience with the module may vary depending on device and chipset, report issues with logs [if possible]

## Status

Actively developed. See [`CHANGELOG.md`](CHANGELOG.md) for version history.

## License

MIT — see [`LICENSE`](LICENSE).

## Disclaimer

This modifies core memory-management behavior on your device. Use at your own risk.
