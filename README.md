# AOReloaded

Client mod framework for Anarchy Online. Injects into the game process via DLL search-order hijacking (`version.dll`) and patches game behavior at runtime through inline hooks and the game's own DValue (CVar) system.

Built for the [Project Rubi-Ka](https://project-rk.com/) private server.
There are no guarantees of functionality nor stability on other Anarchy Online client distributions.

## Installation

1. Download `version.dll` from the [latest release](../../releases/latest).
2. Drop it into the same folder as `AnarchyOnline.exe` (should be `[game root]\client\`).
3. Launch the game normally.

To uninstall, delete `version.dll` from the client folder.

## Features

### WoW-style Camera

Replaces the stock mouse camera with modern MMO controls:

- **Auto-follow** — camera smoothly returns behind your character when you move (configurable lerp speed).
- **RMB-drag align** — right-click drag snaps your character to face the camera direction.
- **Mouse-run** — hold both mouse buttons to run forward with mouse steering. Keyboard forward (W) and mouse-run coexist cleanly.

### Options Panel

All features are configurable from an **AOReloaded** tab in the in-game options panel (F10).

Note that LMB+RMB mouse-run requires the WoW-style camera option enabled.


## Building

Requires Clang-cl, Ninja, and MSVC (for headers/libs). CMake 3.24+.

```bash
cmake --preset release
cmake --build build/release
```

Available presets: `debug`, `release`, `dev` (RelWithDebInfo).

The build produces `version.dll`. The post-build step copies it (and its PDB) into `../client/` automatically if the client directory exists.

### Toolchain

| Component | Version |
|-----------|---------|
| Compiler | Clang-cl (LLVM) |
| Linker | lld-link |
| Generator | Ninja |
| Target | i686-pc-windows-msvc (x86) |
| CRT | Static (`/MT`) |
| Standard | C++20 |

## How It Works

1. Windows loads our `version.dll` from the client directory before the real one in System32.
2. All 16 original `version.dll` exports are forwarded transparently to the system DLL.
3. On load, a deferred init thread waits for the game's DLLs to finish loading, then resolves internal game APIs by their mangled C++ symbol names.
4. Inline hooks (5-byte `jmp rel32`) detour game functions through our code. Trampolines preserve original calling conventions.
5. Custom DValues are registered into the game's global CVar registry, where the options panel XML can bind to them directly.

## License

[MIT](LICENSE)
