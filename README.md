# LEGO Star Wars: The Complete Saga ARM64 Linux Port

Port of **LEGO Star Wars: The Complete Saga** (Android ARM64) to Linux ARM64, targeting the [Trimui Smart Pro](https://trimui.com) handheld (1280x720, PowerVR GE8300).

Loads the original `libTTapp.so` from the Android APK and runs it natively on Linux with a custom shim layer (fake JNI, OpenSL ES → SDL2 audio, EGL → SDL2 window, bionic → glibc wrappers).

> **Status: PLAYABLE** — The game boots, renders, and is playable on Trimui Smart Pro with gamepad controls.

## Requirements

- Docker (for cross-compilation)
- Game files from a legitimate Android copy of LEGO Star Wars: The Complete Saga (Google Play, APK v2.0.2.02)

## Build

```bash
make compile
```

This builds a Docker cross-compilation toolchain and produces `build/lswtcs_arm64`.

## Game files setup

Create the following directory structure next to the binary:

```
LSWTCS.pak/
├── launch.sh                  # launcher script (see below)
├── lswtcs_arm64               # compiled binary
├── libTTapp.so                # from APK lib/arm64-v8a/
├── libpthread-2.33.so         # bionic pthread shim
└── data/
    └── user/0/com.wb.lego.tcs/files/
        ├── assetpacks/
        │   ├── asset_Audio/20202/20202/assets/Audio.dat
        │   ├── asset_Levels/20202/20202/assets/Levels.dat
        │   ├── asset_Others/20202/20202/assets/Others.dat
        │   └── asset_Textures/20202/20202/assets/Textures.dat
        └── SavedGames/
            ├── SaveGame0.LEGO Star Wars - The Complete Saga_SavedGame.incomplete
            ├── SaveGame1.LEGO Star Wars - The Complete Saga_SavedGame.incomplete
            ├── SaveGame2.LEGO Star Wars - The Complete Saga_SavedGame.incomplete
            ├── SaveGame3.LEGO Star Wars - The Complete Saga_SavedGame.incomplete
            ├── SaveGame0.LEGO Star Wars - The Complete Saga_SavedGame
            ├── SaveGame1.LEGO Star Wars - The Complete Saga_SavedGame
            ├── SaveGame2.LEGO Star Wars - The Complete Saga_SavedGame
            ├── SaveGame3.LEGO Star Wars - The Complete Saga_SavedGame
            ├── SaveGame4.LEGO Star Wars - The Complete Saga_SavedGame
            └── SaveGame5.LEGO Star Wars - The Complete Saga_SavedGame
```

The entire `data/` directory tree must be created manually. The `.dat` files come from the APK asset packs. All `SaveGame*` files must exist as **empty files** (0 bytes) — the game expects them on startup.

`launch.sh`:

```bash
#!/bin/sh

cd "$(dirname "$0")"
./lswtcs_arm64 &> ./log.txt
```

- `libTTapp.so` — from APK `lib/arm64-v8a/`
- `.dat` files — game asset packs (Audio, Levels, Others, Textures)

## Run

```bash
./lswtcs_arm64
```

## Controls (Trimui Smart Pro)

Gamepad with Nintendo face-button layout (A/B swapped to match Android gamepad semantics). D-pad and analog sticks for navigation.

| Button | Action |
|--------|--------|
| A (Nintendo) | B (Android) — Back/Cancel |
| B (Nintendo) | A (Android) — Confirm/Attack |
| X | X — Switch character |
| Y | Y — Special action |
| L1/R1 | Shoulder buttons |
| L2/R2 | Triggers |
| START | Start/Pause |
| BACK | Back |
| D-pad / Left stick | Movement |
| Right stick | Camera |

## Makefile targets

| Target | Description |
|--------|-------------|
| `make compile` | Cross-compile for ARM64 Linux |
| `make shell` | Open a shell in the build container |
| `make clean` | Remove build artifacts and Docker image |

## Known bugs

- **Audio crackling** — Rare crackling when many SFX play simultaneously, mitigated by volume corruption guard and soft-clip limiter.

## Disclaimer

LEGO Star Wars: The Complete Saga is a product of [TT Games](https://www.ttgames.com/) and [Warner Bros. Interactive Entertainment](https://www.wbgames.com/). LEGO® is a trademark of the LEGO Group, which does not sponsor, authorize, or endorse this project. Star Wars® is a trademark of Lucasfilm Ltd. This project is an unofficial, non-commercial, hobbyist port. The author is not affiliated with TT Games, Warner Bros., the LEGO Group, or Lucasfilm in any way. You must own a legitimate copy of the game to use this port.

## License

Apache 2.0
