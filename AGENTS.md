# Architecture: Android ARM64 .so Wrapper Port

This document describes how the LEGO Star Wars: The Complete Saga ARM64 wrapper port works. It is intended for AI agents and developers who want to understand the codebase or create similar ports for other Android games.

## Overview

This project runs an **unmodified** Android ARM64 shared library (`libTTapp.so`) on Linux ARM64 by providing a fake Android environment. No game code is recompiled — the original binary is loaded into memory and executed directly.

The wrapper intercepts all calls the game makes to Android APIs (JNI, OpenSL ES, EGL, bionic libc) and translates them to Linux equivalents (SDL2, glibc, native OpenGL ES).

## Execution Flow

```
main.c
  |
  +-- mmap(256 MB heap, RWX)
  +-- so_load("libTTapp.so")              # Parse ELF, load segments into heap
  +-- so_relocate()                        # Apply ELF relocations
  +-- so_resolve(dynlib_functions)         # Patch GOT: Android imports -> our shims
  +-- patch_*()                            # Binary patches (stack checks, GL, gamepad)
  +-- so_finalize() + so_flush_caches()    # Make .text R-X, flush icache
  +-- so_execute_init_array()              # Run .init_array constructors
  +-- SDL_Init() + egl_shim_create_window()
  +-- jni_shim_init()                      # Create fake JavaVM + JNIEnv
  +-- tt_activity_init()                   # Resolve 24 TTActivity JNI symbols
  +-- JNI_OnLoad()                         # Game's JNI initialization
  +-- nativeSetGamePadConnected(1)         # Register gamepad
  +-- TTActivity startup sequence:
  |     nativeOnCreate -> nativeOnStart -> nativeOnResume
  |     nativeSetSurface -> nativeSetScreenDimesions
  |     nativeOnWindowFocusChanged
  +-- Event loop:
        SDL_PollEvent -> gamepad polling -> opensles_shim_pump_callbacks
```

Unlike Syberia (which uses `android_main` / `android_native_app_glue`), this game uses a JNI Activity pattern — the wrapper calls individual `TTActivity` JNI methods in sequence and drives the event loop from `main()`.

## Module Responsibilities

### `so_util.c` — ELF Loader (REUSABLE)
Loads an ARM64 ELF .so into a pre-allocated memory region. Parses program headers, applies relocations (R_AARCH64_RELATIVE, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT, R_AARCH64_ABS64), resolves symbols against an import table, and runs .init_array constructors.

Key API:
- `so_load(filename, base, max_size)` — load ELF into memory
- `so_relocate()` — apply all relocations
- `so_resolve(funcs, count, taint)` — patch GOT with our function pointers
- `so_find_addr(symbol)` — find symbol address in .so
- `so_find_addr_safe(symbol)` — find symbol address, return 0 if not found
- `so_finalize()` — make .text read-only+exec
- `so_flush_caches()` — flush instruction cache after patching

### `imports.c` — Import Table (GAME-SPECIFIC)
Maps every symbol the .so imports to a real function. This is where Android APIs are redirected to our shims. Contains entries covering:
- **libc/bionic**: stdio, stdlib, string, math, errno, locale, etc.
- **pthread**: Mutex/cond wrappers that translate bionic struct layouts to glibc (ALL mutexes are RECURSIVE — required because the game re-locks from the same thread)
- **OpenGL ES 2.0**: Mostly direct passthrough, with wrapper functions for logging/debugging
- **OpenSL ES**: Redirected to our SDL2 audio bridge
- **JNI**: `JNI_OnLoad` redirected to our fake JavaVM
- **zlib, liblog**: Direct passthrough or stubs

### `jni_shim.c` — Fake JNI (PARTIALLY REUSABLE)
Creates fake `JavaVM` and `JNIEnv` with vtables. Most methods are stubs. Key implementations:
- `FindClass` / `GetMethodID` — return fake handles
- `CallObjectMethod` / `CallBooleanMethod` — return game-specific values
- `GetObjectArrayElement` — returns asset pack paths from a fake jobjectArray
- `NewStringUTF` / `GetStringUTFChars` — pass through C strings as fake jstring

### `egl_shim.c` — EGL to SDL2 (REUSABLE)
Translates EGL calls to SDL2 window management:
- `eglGetDisplay` / `eglInitialize` — create SDL window with OpenGL ES context
- `eglSwapBuffers` — calls `SDL_GL_SwapWindow`
- `eglCreateWindowSurface` / `eglMakeCurrent` — return fake handles
- `egl_shim_ensure_current()` — ensures GL context is bound on the calling thread (the game's render thread is separate from the main thread)

### `opensles_shim.c` — OpenSL ES to SDL2 Audio (REUSABLE)
Full OpenSL ES BufferQueue player implementation using SDL2 audio:
- Up to 16 simultaneous audio players
- Per-player lock-free SPSC ring buffers (4 MB each; game thread writes, SDL audio thread reads)
- Linear interpolation resampling (11025/22050 Hz → 44100 Hz)
- Mono-to-stereo upmixing
- Volume control (millibel to linear conversion) with corruption guard
- Fade-in/fade-out envelopes to prevent clicks at buffer boundaries
- Soft-clip tanh-style limiter for mixing headroom
- HEADATEND detection: when decoder stops feeding data and ring buffer drains, fires play callback
- 4-level player allocation: inactive → stopped+drained → any stopped (with audio lock) → force-kill oldest
- BufferQueue callbacks fired from main event loop via `opensles_shim_pump_callbacks()`

### `android_shim.c` — Fake Android Environment (PARTIALLY REUSABLE)
Provides minimal Android runtime stubs:
- `AAssetManager` — translates asset paths to filesystem reads from game data directory
- Data path configuration via `android_shim_set_data_path()`

### `main.c` — Entry Point, Patches & Event Loop (GAME-SPECIFIC)
- Allocates 256 MB RWX heap, loads .so, resolves imports
- **TTActivity JNI bridge**: Resolves and calls 24 `Java_com_tt_tech_TTActivity_*` functions in correct startup order
- **Binary patches** (see below)
- **Crash handler**: SIGSEGV/SIGBUS/SIGABRT handler with full register dump, backtrace, and memory map
- **SDL event loop**: Polls SDL events, translates gamepad to Android keycodes, pumps audio callbacks
- **Gamepad mapping**: Nintendo face-button layout swap (A↔B), D-pad, analog sticks, triggers
- **Gamepad activation**: Synthetic BUTTON_A pulse + `isPressedStart` flag to bypass title screen prompts

### `util.c` / `error.c` — Helpers (REUSABLE)
Debug printing, fatal error handling.

## Binary Patches

The game binary requires several runtime patches to work on this wrapper:

### Stack Canary Bypasses
The bionic `__stack_chk_fail` PLT entry is not available in our environment, causing false-positive crashes. Two approaches:
1. **Named function patches**: NOP the conditional branch to `__stack_chk_fail` in specific functions (NuRenderDevice::Initialize, InitialiseOpenGLContext, NuPostFilter, NuMtlInitEx, loadDefaultTexture, NuQFntReadBuffer, NuInitHardware, NuFrameEnd)
2. **Global scan**: `patch_all_stack_chk_branches()` scans the entire .text section for `bl __stack_chk_fail@plt` calls, walks backwards to find the conditional branch that guards them, and NOPs each one

### GL Context Management
- **EndCriticalSectionGL**: Patched to always release the EGL context, preventing `EGL_BAD_ACCESS` when the render thread tries to acquire it
- **g_glConstantSetterTable**: Function pointer table for `glUniform*` replaced with wrapper functions that ensure GL context is current on the calling thread

### Gamepad & Touch
- **enable_touch_controls**: Global flag set to 0 (touch overlay disabled for gamepad-only device)
- **isPressedStart**: Global flag set to 1 (bypasses "Press the START button" touch overlay)
- **NuPadUpdatePads activation gate**: `tbz` branch at +0x294 NOPped so gamepad is active from the first frame
- **g_nupadMapping**: Force pad 0 → port 1, is_active=1 (maintained every frame in event loop)

## Threading Model

- **Main thread**: Runs `main()` → TTActivity startup → event loop (SDL events + gamepad polling + audio pump)
- **Render thread**: Spawned by the game internally, runs the GL rendering pipeline
- **SDL audio thread**: Reads from per-player ring buffers, mixes, applies limiter, outputs to hardware
- Audio data path is lock-free (SPSC ring buffers); SDL_LockAudioDevice used only for state changes (play/stop/clear)
- All TTActivity JNI calls and gamepad input happen on the main thread

## Build

Docker cross-compilation targeting ARM64 Linux (Trimui Smart Pro sysroot):

```bash
make compile    # builds via Docker
make shell      # interactive shell in build container
make clean      # remove artifacts
```
