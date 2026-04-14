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
