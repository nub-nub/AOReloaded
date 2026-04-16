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
- **Inline hook engine** (`src/hooks/hook_engine.cpp`): 5-byte `jmp rel32` over verified prologues. Supports the standard hot-patch prologue plus several MSVC frame-prologues (`55 8B EC 51 5x`, etc.). Trampoline preserves callee-cleanup conventions (e.g. `RET 4`).
- **Replacement ActionViewMouseHandler_c** (`src/hooks/camera_hook.cpp`): hooks 4 of the stock handler's callbacks (OnMouseDown, OnMouseMove, OnMouseUp, EndDrag) plus CalcSteering. Implements an independent LMB/RMB state machine with 6 states (IDLE, PENDING_LMB, PENDING_RMB, DRAGGING_LMB, DRAGGING_RMB, BOTH_HELD). OnMouseRelease is NOT hooked (unhookable prologue) — its call to EndDrag flows through our EndDrag detour.
- **CalcSteering hook** on `CameraVehicleFixedThird_t` (N3.dll RVA `0x1f752`): per-frame yaw-follow, lerps heading toward "behind character" during movement. Reads button state from `InputState` struct (not `GetAsyncKeyState`).
- **RMB-align** in CalcSteering: on RMB drag start edge, snaps character facing to camera direction via `SetRelRot`. **Order matters**: heading reset MUST precede `SetRelRot` — see `src/ao/CLAUDE.md`.
- **Both mouse buttons → forward**: forward movement is a **derived signal** evaluated per-frame in CalcSteering: `shouldForward = BOTH_HELD || ForwardKeyHeld`. CalcSteering is the sole dispatcher of StartForward/StopForward, calling through the trampoline (bypasses filter). OnMouseDown and EndDrag only manage mouse state transitions, not movement.
- **N3Msg_MovementChanged filter** (Gamecode.dll hook): intercepts the central movement dispatcher. While BOTH_HELD is active, suppresses all StartForward/StopForward events from external sources (keyboard W-release, etc.) so they don't fight the mouse-driven forward movement. All other actions (strafe, turn, jump) pass through unmodified.
- **GUI.dll API resolution**: N3Msg dispatch functions (`CameraMouseLookMovement`, `MouseMovement`, `EndCameraMouseLook`, `EndMouseLook`) resolved by reading cached function pointers from GUI.dll's data section at known RVAs. `WindowController_c` and `AFCM` functions resolved by direct RVA. See `GUIAPI` namespace.
- **WndProc hook removed**: the old WndProc hook (AnarchyOnline.exe RVA 0x4a66) is no longer needed — all mouse button state management is handled at the callback level.
- **Options panel SSO limit**: DValue names bound to `OptionCheckBox`/`OptionSlider` MUST be ≤ 15 chars. `AOString::FromShort` silently returns an empty string for longer names, which corrupts the registry and hangs the slider widget. Sliders also expect Int (not Float) DValues — Float-bound sliders render with no knob and hang on click.
- **Numpad text input fix** (`src/hooks/numpad_fix.cpp`): hooks `InputConfig_t::CheckInput` (GUI.dll RVA `0x1a4c1`, prologue `55 8B EC 51 53`). When text input mode is active (`this+0x67` or `this+0x27`), translates numpad keys (AO codes 0x42-0x50) to characters and injects via `WindowController_c::HandleTextInput`. Consumes the key event to prevent action bindings from firing. Respects AFCM redirect mode (hotkey assignment). Toggled by `AOR_NumpadFix` DValue.

## Caveats / footguns

- DValue name length: see SSO limit above. If a slider hangs the game on click, suspect this first.
- Detour calling conventions: hooked `__thiscall` methods need a detour with `__fastcall` signature (`ECX`=this, `EDX`=junk). The trampoline must keep the original `__thiscall` typing or `RET N` cleanup will mis-balance the stack.
- `RecalcOptimalPos` rotates the heading at `vehicle+0x1F8` by the character-yaw quaternion at `vehicle+0x16c` before computing the optimal camera position. Writing to `+0x1F8` in **world space** produces wrong directions; instead, write the **local-space** target (e.g. local "behind" is the constant `(0, 0, -1)`) and let the rotation produce world-space output.

## Graphics Research

Deep-dive notes on the client's rendering/lighting/asset systems live in `docs/graphics/`:
- `shadows_lighting.md` — three-light-bank forward renderer, drop-shadow model, day/night (base-game), AOReloaded possibilities
- `textures.md` — `rdb.db` is SQLite3 with JPEG/PNG texture blobs in tables `rdb_1010004/08/09/16/17/23/24`. `ldb.dll` is *localisation text*, not textures. Upscaling feasibility tiers.

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
