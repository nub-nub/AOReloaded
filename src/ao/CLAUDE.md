# AO Client Interop (src/ao/)

Reverse-engineered types and runtime API bindings for interacting with the AO client.

## Key Files

- `types.h` — Struct layouts for AOString (0x18), AOVariant (0x10), AODistributedValue (0x50). All under `#pragma pack(push, 4)` to match MSVC x86 ABI.
- `game_api.h/cpp` — Runtime-resolved function pointers from Utils.dll. Uses MSVC mangled names via GetProcAddress.
- `dvalue_dump.h/cpp` — Walks the global DValue registry's internal std::map red-black tree to enumerate all entries.

## DValue Registry Internals (Utils.dll)

The global registry is an `std::map<String, ValueEntry>` backed by a red-black tree.

**Global addresses (RVAs in Utils.dll):**
- `_Myhead` (sentinel node pointer): RVA `0x2e61c`
- `_Mysize` (element count): RVA `0x2e620`
- Global mutex: RVA `0x2e648` (ACE_Thread_Mutex)

**Tree node layout (verified by hex dump 2026-04-14):**
```
+0x00: Node* _Left
+0x04: Node* _Parent
+0x08: Node* _Right
+0x0C: uint32_t (unknown field — NOT the key)
+0x10: AOString key (0x18 bytes)
+0x28: AOString default_key (0x18 bytes, duplicate)
+0x50: uint32_t variant_type (Variant tag)
+0x54: uint32_t (padding)
+0x58: 8-byte value payload
+0x61: bool has_min
+0x62: bool has_max
+0x68: Variant min_value (0x10 bytes)
+0x78: Variant max_value (0x10 bytes)
+0x88: int32_t category (DValueCategory_e)
+0x8C: vector<DistributedValue_c*> observers
+0xA0: char _Color
+0xA1: char _Isnil (0=valid, 1=sentinel)
```

**DValue categories:**
- 0: Global runtime state (CharacterID, Username)
- 1: Client-wide persistent settings (resolution, font, sound)
- 2: Per-user preferences (mouse, zoom, volume, ESC toggles)
- 3: Per-character UI state (window configs, visibility)
- 4: Transient event signals (camera_mode, module activators)

## Variant Type Tags

| Tag | Type | Storage |
|-----|------|---------|
| 0 | None | — |
| 4 | Int/UInt | 4 bytes at +0x08 |
| 5 | Int64 | 8 bytes at +0x08 |
| 6 | Bool | 1 byte at +0x08 |
| 7 | Float | 4 bytes at +0x08 |
| 8 | Double | 8 bytes at +0x08 |
| 9 | String | heap ptr at +0x08 |
| 10-12 | IRect/IPoint/Message | size at +0x08, heap ptr at +0x0C |
| 16-17 | Rect/Point | size at +0x08, heap ptr at +0x0C |

## Calling Convention Notes

- Static `SetDValue`/`GetDValue`: `__cdecl`
- `GetDValue` returns Variant by value — on MSVC x86, structs >8 bytes use hidden return pointer as first arg
- Instance methods: `__thiscall` (this in ECX)
- All implementations in Utils.dll, imported by GUI.dll and Gamecode.dll
- For inline-hook detours over `__thiscall`, declare the detour as `__fastcall(void* this_ecx, void* edx_unused, ...stack_args)`. ECX is preserved as `this`, EDX is the unused fastcall slot, and remaining stack args follow. Type the trampoline back as the original `__thiscall` so the trampoline's `RET N` cleanup matches the caller's expectations.

## N3.dll — Camera System (RE 2026-04-14)

### Resolved exports (mangled name → purpose)

| Mangled name | Purpose |
|---|---|
| `?GetInstance@n3EngineClient_t@@SAPAV1@XZ` | Engine client singleton (also acts as `n3Engine_t`). |
| `?GetActiveCamera@n3EngineClient_t@@QBEPAVn3Camera_t@@XZ` | Returns active `n3Camera_t*`. |
| `?GetClientControlDynel@n3EngineClient_t@@QBEPAVn3VisualDynel_t@@XZ` | Returns the locally controlled player dynel. |
| `?GetGlobalPos@n3Dynel_t@@QBEABVVector3_t@@XZ` | Player position; returns `Vector3_t*` in EAX. |
| `?GetGlobalRot@n3Dynel_t@@QBE?AVQuaternion_t@@XZ` | Player rotation; returns Quaternion by value (hidden ret ptr). |
| `?IsFirstPerson@n3Camera_t@@QBE_NXZ` | Checks current camera mode (1st vs 3rd person). |
| `?ForceUpdatePrefDir@n3Camera_t@@QAEXXZ` | Internally calls `vehicle->ForcedUpdate(false)`. **Does NOT snap behind character** — it just reuses the current camera position as the new preferred direction. |
| `?ForcedUpdate@CameraVehicleFixedThird_t@@UAEX_N@Z` | Virtual; copies position from `+0x58` to `+0xdc`, calls `UpdateHeadingToPos`, then virtual `+0x98`. |
| `?SetRelRot@n3Dynel_t@@QAEXABVQuaternion_t@@@Z` | Writes body rotation (forwards to `Vehicle_t::SetRelRot` on `dynel+0x50`). For the unparented local player, rel == global rot. Used by RMB-align to snap character facing to camera look direction. **Has a synchronous side effect** that recomputes camera targeting from `vehicle+0x1F8` × `vehicle+0x16C` — callers snapping both must write the new heading BEFORE invoking this, or the cascade renders one frame of a stale-heading × new-quat position. |

### `n3Engine_t` instance (engine timing)

- `n3EngineClient_t::GetInstance()` returns the same singleton as `n3Engine_t::m_pcInstance`.
- `engine + 0x68`: `float dt` (frame delta time, seconds). Used by per-frame camera update.

### `CameraVehicleFixedThird_t` field map (partial)

| Offset | Type | Notes |
|---|---|---|
| `+0x16C` | `Quaternion_t` (4×float, layout `(qx, qy, qz, qw)`) | Character-yaw rotation. Pure Y-axis: `qx=qz=0`. `RecalcOptimalPos` rotates the heading vector at `+0x1F8` by this quaternion when `+0x204` flag is set. |
| `+0x198` | `float` | Preferred camera distance. |
| `+0x1EC..+0x1F4` | `Vector3_t` | Cached optimal camera position (output of `RecalcOptimalPos`). |
| `+0x1F8..+0x200` | `Vector3_t` | **Heading direction**, in pre-rotation (local) space. Steering uses this to compute optimal position. Modifying it is the supported way to redirect the camera. |
| `+0x204` | `uint8_t` | Flag: when non-zero, `RecalcOptimalPos` rotates `+0x1F8` by the quaternion at `+0x16c`. Empirically always 1 in 3rd person gameplay. |
| `+0x208..+0x210` | `Vector3_t` | Steering velocity. Zeroed by `UpdateHeadingToPos`. |

### Heading semantics (important)

The heading at `+0x1F8` is in **character-local space** (pre-rotation). RecalcOptimalPos applies the `+0x16c` quaternion to produce a world-space direction before placing the camera. Implications:

- Local `(0, 0, -1)` → camera ends up directly behind the character regardless of character facing.
- Writing a world-space "behind" direction into `+0x1F8` produces a doubled rotation (or any other character-yaw-dependent error).
- The steering then arrives toward `+0x1EC` — a recalculated point at distance `+0x198` along the rotated heading.

### Hook target — `CameraVehicleFixedThird_t::CalcSteering`

- RVA `0x1f752` in N3.dll
- Signature: `SteeringResult_e __thiscall CalcSteering(Vector3_t& result)` — returns int in EAX, callee cleans 4 stack bytes (`RET 4`).
- Prologue: `55 8B EC 51 56` (`push ebp; mov ebp,esp; push ecx; push esi`) — hookable with the standard 5-byte engine.
- Internally: `RecalcOptimalPos(this)` then either `ZoomSteer` (if zoom velocity at `+0x1B8` ≠ 0) or `SteeringCamArrive` toward the optimal position.

### Reset flag (alternative reset path — not currently used)

Setting bit `0x2000` on `n3Camera_t+0x184` (the camera flags field) makes the next per-frame camera update call the real "behind character" reset routine (the same path used by `COMMAND_RESET_CAMERA` / Numpad 5). The reset uses character facing, not camera position, so it produces a clean snap. This is what we used during early prototyping before switching to the per-frame heading lerp.
