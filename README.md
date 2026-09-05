# lmk_engine

A custom userspace low-memory killer daemon for Android, written in pure C with no external dependencies. Runs alongside the stock low-memory killer, keeping the apps you actually use alive longer under memory pressure without the smoothness cost of killing indiscriminately.

Built and tuned on an Infinix X683 (MT6768, 4GB RAM).

## What it does

- **App Pinning** - Keeps most-used apps alive under memory pressure, based on how you actually use your phone — not just what's most recent.
- **Adaptive Clean** - Reclaims memory from background apps gradually, escalating only as pressure actually increases.
- Manages ZRAM so the device stays usable even when RAM is chronically tight.

For internal design, subsystem details, and the full list of tunable constants, see [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md). For version history, see [`CHANGELOG.md`](CHANGELOG.md).

## Requirements

- Rooted Android device 
- clang or gcc to build

## Building

```bash
clang -O2 -Wall -Wextra -o lmk_engine src/lmk_engine.c -lm
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

## License

MIT — see `LICENSE`.
