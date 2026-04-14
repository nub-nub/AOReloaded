# AOReloaded

DLL injection mod framework for the Anarchy Online client. Masquerades as `version.dll` (DLL search order hijacking) to load into the game process.

## Build

```bash
cmake --preset debug    # or release, dev
cmake --build build/debug
```

Clang-cl + Ninja, targeting x86 (i686-pc-windows-msvc). Static CRT. Auto-deploys `version.dll` and PDB to `../client/` on build.

## Architecture

```
src/
├── dllmain.cpp              # Entry point, deferred init thread
├── proxy/                   # version.dll forwarding to real System32 DLL
│   ├── version_proxy.cpp    # Lazy-loaded forwarders (16 exports)
│   └── version_proxy.def    # Export table
├── core/
│   └── logging.cpp          # Thread-safe file logging with flush + session header
└── ao/                      # AO client interop (see src/ao/CLAUDE.md for deep details)
    ├── types.h              # Reconstructed struct layouts (AOString, AOVariant, AODistributedValue)
    ├── game_api.h/.cpp      # Runtime-resolved function pointers into Utils.dll
    └── dvalue_dump.h/.cpp   # Walks the global DValue registry tree (268 entries found)
```

## How It Works

1. Windows loads our `version.dll` from client dir before System32's.
2. `DllMain` inits logging, spawns deferred init thread (can't resolve game APIs under loader lock).
3. Deferred thread polls for `Utils.dll`, resolves exported C++ methods by mangled name.
4. Registers custom DValues (e.g. `AOR_TestToggle`) via `AddVariable`.
5. Polls for `camera_mode` to detect game-world entry.
6. Game API functions called through resolved pointers.

## Key APIs (from Utils.dll)

All resolved by MSVC mangled name — stable as long as method signatures don't change.

| Function | Mangled Name | Calling Conv |
|----------|-------------|--------------|
| `SetDValue(static)` | `?SetDValue@DistributedValue_c@@SAXABVString@@ABVVariant@@@Z` | `__cdecl` |
| `GetDValue(static)` | `?GetDValue@DistributedValue_c@@SA?AVVariant@@ABVString@@_N@Z` | `__cdecl`, hidden ret ptr |
| `DoesVariableExist` | `?DoesVariableExist@DistributedValue_c@@SA_NABVString@@@Z` | `__cdecl` |
| `AddVariable` | `?AddVariable@DistributedValue_c@@SAXABVString@@ABVVariant@@_N2@Z` | `__cdecl` |

## DValue System

Game's CVar equivalent. Named string keys → Variant values in global thread-safe registry. Any game setting can be read/written by name.

**Full dump of all 268 registered DValues** was captured on 2026-04-14. Key categories:
- Cat 0: Runtime state (CharacterID, Username)
- Cat 1: Client-wide persistent (resolution, font, sound)
- Cat 2: Per-user prefs (mouse, zoom, volume, ESC toggles)
- Cat 3: Per-character UI state (window configs, visibility)
- Cat 4: Transient signals (camera_mode, module activators)

## Options Panel Integration

The game's options panel is pure data-driven XML at `client/cd_image/gui/Default/OptionPanel/Root.xml`. An **AOReloaded tab** has been added with test widgets bound to custom DValues. To add new options:

1. Register DValue in `dllmain.cpp` via `GameAPI::RegisterBool/Int/Float`
2. Add widget XML to the AOReloaded `<ScrollView>` in `Root.xml`
3. Widget types: `OptionCheckBox`, `OptionSlider`, `OptionRadioButtonGroup`
4. Bind via `opt_variable="YourDValueName"` and `opt_type="variant"`

## Confirmed Working (2026-04-14)

- SetDValue: changed ChatFontSize 140→300, visible in-game
- GetDValue: reads current values correctly
- DoesVariableExist: correctly reports presence/absence by game state
- AddVariable: registers custom DValues that game's option widgets bind to
- DValue dump: walked all 268 entries from global std::map tree
- Options panel: AOReloaded tab appears in F10 settings with working test widgets

## Documentation Maintenance

When implementing a new feature:
1. **`FEATURES.md`** — Add a user-facing entry. Write for players, not developers. Include config instructions if the feature has settings (options panel path, slash command, etc.). Use the template in the file.
2. **`../IDEAS.md`** — Update the status column for the implemented idea.
3. **This file** — Update the "Confirmed Working" section if there's a new API or system integration.
4. **`src/ao/CLAUDE.md`** — Update if new struct layouts, RVAs, or calling conventions were discovered during implementation.

## Logging

Log file: `client/AOReloaded.log`. Truncated each launch. Flushed after every write. Tail with:
```bash
less +F client/AOReloaded.log    # or: tail -f client/AOReloaded.log
```
