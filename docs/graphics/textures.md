# Textures — Storage, Loading, and Upscaling Feasibility

Where game textures live, how the engine fetches them, and whether an AI-upscale / HD-retexture pipeline is viable. Sources: inspection of `client/cd_image/rdb.db` and reverse-engineered `DisplaySystem.dll` / `ldb.dll` via Ghidra.

## Storage

**`client/cd_image/rdb.db` — 2.0 GB SQLite 3 database.**

Header: `SQLite format 3`. Not a proprietary archive. Opens cleanly in any SQLite tool (`sqlite3` CLI, Python `sqlite3`, DB Browser for SQLite, etc.).

Schema: **56 tables**, all with identical layout:

```sql
CREATE TABLE rdb_<TypeID> (id INTEGER PRIMARY KEY, version INTEGER, data BLOB)
```

- `<TypeID>` in the table name is the resource type.
- `id` is the asset id within that type.
- `version` supports hot-patch / asset replacement.
- `data` is the raw asset payload — format varies per type.

Loose files on disk are limited to:
- `client/cd_image/textures/PlanetMap/` — planet-map backgrounds (tile-split, not in rdb)
- `client/cd_image/textures/fonts/` — tiny bitmap legacy fonts
- `client/cd_image/twk/*.txt` — human-editable FX/tweak scripts that *reference* assets by name or id

Index files:
- `client/AnarchyOnline.til` — unconfirmed; likely table/index
- `client/AnarchyOnline.nam` — unconfirmed; likely name→id map

## Important clarification

**`ldb.dll` is NOT the texture loader.** `ldb.dll` is the **localisation / text database**. Its API — `LDBface::GetText`, `LDBformat`, `FormatTime`, `RemoteFormat`, `ArgType`, `LDBtoken`, `ToBase85/FromBase85` — is printf-style formatted-message support for internationalised game text. It loads `cd_image/text/ctext.ldb` and `cd_image/text/text.mdb`.

Texture loading is done by **`ResourceManager.dll`** (not yet reverse-engineered in this project) plus the `RResource_t` / `RTexture_t` layer in `DisplaySystem.dll` (imports `D3DXCreateTexture`, `D3DXLoadTextureFromMemory`). RDB access is most likely a thin SQLite wrapper sitting beneath ResourceManager.

## Blob formats by table

Sampled first-blob magic bytes across all 56 tables reveal texture storage directly (top entries shown):

| Table | Row count | Max blob | Magic (ASCII) | Interpretation |
|-------|-----------|---------:|---------------|----------------|
| `rdb_1010016` | 10,890 | 392 KB | `....JFIF..` | **JPEG textures** (highest-resolution bucket) |
| `rdb_1010017` | 11,153 | 199 KB | `....JFIF..` | **JPEG textures** |
| `rdb_1010004` | 11,722 | 5.5 MB | `....JFIF..` | **JPEG textures** (largest-per-item bucket) |
| `rdb_1010023` | 571 | 14 KB | `....JFIF..` | **JPEG textures** |
| `rdb_1010024` | 571 | 3.6 KB | `....JFIF..` | **JPEG textures** |
| `rdb_1010009` | 574 | 61 KB | `....JFIF..` | **JPEG textures** |
| `rdb_1010008` | 7,449 | 7 KB | `.PNG........` | **PNG textures** (smaller assets, likely UI / icons / fonts with alpha) |
| `rdb_1010002` | 770 | 2.2 MB | `Bip01_ac` | Animations (Bip01 = 3DS Max biped root) |
| `rdb_1010003` | 3,190 | 99 KB | `Bip01_ac` | Animations |
| `rdb_1000009` | 436 | 1.5 MB | `CHGA` | Proprietary ("CHaracter Geometry Asset"?) — meshes |
| `rdb_1010006` | 849 | 195 KB | `sand.sand.` | Terrain / material definitions |
| `rdb_1010001` | 9,925 | 3.1 MB | — | Large proprietary blobs (candidate: world chunks / statels) |
| `rdb_1000013` | 234,349 | 231 KB | — | Huge count — probably per-item metadata rows |
| `rdb_1040005` | 11,160 | 10 KB | — | Unknown (row count matches JPEG tables — possibly texture metadata or mip-chain companions) |

**Actual totals (verified by full extract + re-extract 2026-04-15):** 13 texture tables, **45,839 texture assets** — 27,707 JPEG + 18,132 PNG. Raw extract ~937 MB on disk. `tools/extract_textures.py` auto-detects texture tables by magic (including a **24-byte name prefix** used by the terrain chain), splits per-row by actual encoding so PNG alpha is preserved, and optionally embeds names via `tools/rename_extracts_with_names.py`.

Chain D (`rdb_1010006/21/22`) was initially missed because blobs start with a 24-byte block (name-then-0xCD-padding for Tier 0, all-zero for Tier 1/2) rather than JPEG/PNG magic at byte 0.

Tables confirmed to contain textures (auto-detected by per-row magic):

| Table | Total rows | JPEG rows | PNG rows |
|-------|-----------:|----------:|---------:|
| `rdb_1010004` | 11,715 | 8,498 | 3,217 |
| `rdb_1010008` | 7,449 | 14 | 7,435 |
| `rdb_1010009` | 574 | 566 | 8 |
| `rdb_1010011` | 263 | 60 | 203 |
| `rdb_1010016` | 10,883 | 7,920 | 2,963 |
| `rdb_1010017` | 11,146 | 8,152 | 2,994 |
| `rdb_1010019` | 60 | 60 | 0 |
| `rdb_1010020` | 60 | 60 | 0 |
| `rdb_1010023` | 571 | 563 | 8 |
| `rdb_1010024` | 571 | 563 | 8 |

**Important: tables are not encoding-homogeneous.** Most "JPEG" tables also carry PNG rows, and the PNG table (`rdb_1010008`) has a handful of JPEGs. This strongly suggests tables group by **texture category or mip-level**, not by encoding — encoding is chosen per-asset based on whether alpha is needed.

Row-count patterns suggest companion tables:
- `rdb_1010019` + `rdb_1010020`: both exactly 60 rows. Likely paired (mip levels, or diffuse/alpha split).
- `rdb_1010023` + `rdb_1010024`: both exactly 571 rows. Likely paired.
- `rdb_1010016` + `rdb_1010017`: both ~11,000 rows. Likely paired.

**Metadata architecture (verified 2026-04-15 via `tools/scan_rdb_blobs.py`, `dump_rdb_candidates.py`, `test_elfhash.py`).**

There is no single metadata companion table for `rdb_1010004` et al. But metadata *does* exist — it's specialised and distributed across multiple tables, each serving a different concept:

| Table | Rows | Role | How it names textures |
|-------|------|------|----------------------|
| `rdb_1010006` / `_1010021` / `_1010022` | 849 ea | **Terrain texture chain (4th LOD pyramid)** | First 28 bytes of blob = null-padded ASCII name, duplicated (e.g. `"sand\0sand\0"`, `"grass\0grass\0"`), then the JPEG. Name is embedded in the asset itself. |
| `rdb_1010027` | 8 | **Compiled scenery-object defs** | Blob contains object bone names, mesh refs, AND texture filenames as ASCII (`"landcontrol_tower_buff1.png"`, `"tower_base.png"`). |
| `rdb_1000013` | 234,349 | **Master item catalog** | Every item in AO. Packed binary ~50 byte rows with `0x5E132711` magic header. Contains u32 fields referencing icon ids, mesh ids, stat tables. |
| `rdb_1040005` | 11,160 | **Nano program definitions** | Localised nano descriptions + binary fields referencing icon ids into `rdb_1010008`. |
| `rdb_1040023` | 1,360 | **Pet / NPC templates** | Binary records ending with model name strings (`"protodroid"`, `"int_emis"`). |
| `rdb_1000038` | 813 | **Effect-category name strings** | Format: `[u32 length][ASCII]`. Strings like `"NO STACKING"`, `"DamageShields"`. |
| `rdb_1000014` / `_1000020` / `_1000029` | 600 / 119k / 600 | String-heavy binary catalogs | Unexplored — content-bearing but format unclear. |

**For texture tables (`rdb_1010004` / `_1010008` / Chains A/B/C):** there is no external width/height/format row. Dimensions come from the JPEG/PNG header. Assets are keyed by a pre-existing numeric id that the *caller* holds — an item row in `rdb_1000013` contains a u32 icon_id field that resolves directly to an `rdb_1010008` id via `ResourceManager::GetSync`.

**Id namespacing observed:**

| Id range | What holds it |
|----------|---------------|
| 1..943 | Terrain textures (hand-assigned per terrain type) |
| 8760..55740 | Character body UV unwraps (60-265 assets) |
| 5942..500103 | General textures (Chain A) — ids coincide with item/object ids |
| 6553603+ | Item catalog rows in `rdb_1000013` (no overlap with texture tables; items reference textures via internal u32 fields) |

**ElfHash(filename) is NOT the name→id map** — tested every variant of observed texture filenames (`landcontrol_tower_buff1.png` etc.) against every texture-table id set; zero hits. Hash values land in the 20M–250M range, but max texture id is ~500k. Name→id mapping probably lives in Gamecode.dll / N3.dll as hardcoded tables or is resolved at asset-compile time (tweak scripts → binary ids done offline at game build).

**Upscaling is still a pure `UPDATE rdb_<TypeID> SET data = ? WHERE id = ?` operation for the texture tables** — no companion updates required on the texture side. But any mod that wants to **add a new named texture** has to also add the name reference somewhere (scenery def, item def, or terrain type slot).

Any upscaler pipeline must preserve the per-row encoding choice (don't re-encode a PNG as JPEG — you'll lose alpha) and must handle companion-table updates if sizes change.

## Loading path (confirmed via ResourceManager.dll RE)

`ResourceManager.dll` exposes the asset-fetch API. Key classes and methods:

- **`Identity_t`** — 8-byte `{ TypeID_e type; uint asset_id; }`. TypeID is literally the integer suffix of the SQLite table name (e.g. `TypeID_e(1010004)` ↔ `rdb_1010004`).
- **`ResourceManager::GetSync(Identity_t const&, bool)`** — synchronous fetch. See annotated decompilation at RVA `0x2a67`. Flow:
  1. Acquire cache lock (`ACE_Token` at `this+0x44`).
  2. Unless `force_reload`, try `DbCache::GetResource(id, &obj)` and return on hit.
  3. Delegate to `DatabaseController_t` (via vtable) to run the actual SQLite SELECT.
  4. **On miss, walk the fallback chain.** Calls `GetFallback(requested_type)` → returns `{alt_type, converter_fn}`. Retries with `alt_type`; if there's a converter, runs the result through it. Loop continues until success or fallback chain ends.
  5. Cache the loaded object under both the resolved Identity and the originally-requested Identity.
- **`ResourceManager::GetAsync`** — same but posts a Signal2_c callback when the worker thread completes.
- **`ResourceManager::AddFallback(TypeID_e from, TypeID_e to, Converter*)`** — registers an entry in the static `m_cFallbacks` vector.
- **`ResourceWorker`** — owns an ACE async thread; `Schedule(Identity, weak_ptr<Signal>)` enqueues an async load.
- **`DbCache::GetResource` / `SetResource`** — cache layer keyed by Identity. `SetCacheLimit(size_t)` controls memory ceiling; `Cleanup()` and `FlushCache()` expose it to higher layers.

**Fallback chain — this is the LOD pyramid mechanism.** When the engine requests a high-quality texture and it's missing, `GetFallback` redirects to a lower-quality TypeID. This is why Chain A has 354 ids only in Tier 0 (`rdb_1010004`) and 586 only in Tier 2 (`rdb_1010017`) but no Jaccard-1.0 overlap — assets are registered at whichever tiers exist, and the fallback chain handles gaps transparently. The user's "Texture Resolution" quality setting almost certainly changes which TypeID is requested first.

Step 4–5 — the D3D7 side — is handled in `DisplaySystem.dll`: `D3DXLoadTextureFromMemory` (imported) decodes the JPEG/PNG bytes into `IDirect3DTexture7`, wrapped as `RTexture_t`. No quirks re: dimension — `D3DX` reads the image header directly.

## Upscaling feasibility — tiered

### Tier A — Offline tooling (no AOReloaded code)

**Directly feasible today.** Approximate workflow:

1. Open `rdb.db` with any SQLite library.
2. For each row in a JPEG/PNG table: decode the blob to an image buffer.
3. Run through an upscaler (`Real-ESRGAN`, `waifu2x`, `ESRGAN`, or classical Lanczos for speed). Different models per content type give best results: anime/UI art for PNG, photographic for JPEG diffuse.
4. Re-encode (preserve JPEG/PNG choice to match the original blob type).
5. `UPDATE rdb_<TypeID> SET data = ?, version = version + 1 WHERE id = ?`.

Write the result as a separate `rdb-hd.db` file and ship it as an optional download. Users swap the file (or the launcher does). **No engine patch required.**

Risks:
- **Texture size limits.** D3D7 on vanilla drivers capped at 2048×2048. Modern GPUs via dgVoodoo support much higher. Cap upscale at 2048 on the short side unless HD is dgVoodoo-only.
- **UV assumptions.** If the original is 256×256 and tiled in a tweak script with hardcoded pixel coords, larger texture still works as long as UV coords are normalised 0–1. Sprite-sheets (font glyph atlases especially) are at risk — glyph slot pixel positions in `FontInfo_t` are absolute. Recommend **excluding** font / atlas / icon tables from the first pass.
- **Storage cost.** 2 GB → likely 10–20 GB after 4× upscale. Ship incrementally (terrain first, characters second, UI last).
- **Visual regressions.** AI upscalers invent detail. Some textures (company logos, readable signs) regress under naive upscaling. Curated pipeline needed.

### Tier B — AOReloaded loose-file override hook (confirmed feasible)

Hook point: **`ResourceManager::GetSync`** (exported; mangled `?GetSync@ResourceManager@@QAEPAVDbObject_t@@ABVIdentity_t@@_N@Z`; RVA `0x2a67` in ResourceManager.dll). Intercept either before the cache check (override unconditionally) or between the cache miss and the DatabaseController call (override only when vanilla would hit the db).

Override-path design:
- Inspect `Identity_t` → `{type, id}`.
- If `type` is a texture TypeID (1010004 / 1010008 / 1010009 / 1010011 / 1010016 / 1010017 / 1010019 / 1010020 / 1010023 / 1010024) AND a file exists at `client/cd_image/textures_override/<type>/<id>.{jpg,png}`, read that file into a blob.
- Either (a) wrap the blob into a `DbObject_t*` directly (needs RE of the DbObject_t wrapper), or (b) inject the blob into the `DatabaseController` return path (simpler — replace the SELECT result).

Enable via DValue `AOR_TexOverride` (bool). Server-distributed HD packs fit on top of this trivially.

Effort: low. Main unknown is the shape of `DbObject_t` (not yet reversed). If wrapping is tricky, fall back to in-place SQLite UPDATE — write to the rdb at AOReloaded init time, then let the vanilla load path do its thing unchanged.

### Tier C — Runtime upscale

Decode at vanilla resolution, upscale in memory on the CPU (bilinear / Lanczos / lightweight neural), pass enlarged buffer to `D3DXLoadTextureFromMemory`. Pure hack with no offline storage footprint. Quality ceiling = real-time-feasible upscalers. Use cases: user-toggleable sharper textures without distributing a 20 GB pack.

Probably not worth building given how cheap Tier A is.

### Tier D — Bypass rdb entirely with an external texture manifest

Ship a curated HD set as DDS / BC7-compressed textures in a side directory. Hook at texture-upload (inside `DisplaySystem.dll::RTexture_t` creation) rather than at blob-load. Advantages: DDS keeps compressed pipeline intact, GPU-native, no JPEG/PNG decode cost. Disadvantages: more hook surface, needs mapping from rdb `(type, id)` to DDS filename.

## Recommended roadmap

1. **Prove unpack-and-view.** Script (~30 lines) that extracts all JPEG blobs from one table to a directory, names them `<id>.jpg`. No engine changes. Confirms the pipeline end-to-end.
2. **Identify which table is which.** Correlate table IDs with asset categories by cross-referencing `cd_image/twk/*.txt` and `cd_image/rdb.db` ids that appear in tweak scripts. Produces a map: `rdb_1010016 = "character diffuse"`, etc.
3. **Pick a small, safe table for HD pilot.** The `shadow*` textures from `shadows_lighting.md` are probably in a smaller table (drop shadows are well under 256×256). Ship an HD pilot as an `rdb-hd.db` file overlayed via the launcher.
4. **Scale up to terrain** (usually the biggest visual win in old MMOs). Test view-distance interaction — larger terrain textures cost more VRAM at distance.
5. **Consider ResourceManager.dll hook** for loose-file override only after the offline tooling path is validated and the community has built at least one HD pack.

## LOD / tier architecture (verified 2026-04-15)

Three asset "chains", each with a fallback-registered LOD pyramid up to 3 tiers deep:

| Chain | Contents | Tier 0 (hi) | Tier 1 (½×) | Tier 2 (¼×) |
|-------|----------|-------------|-------------|-------------|
| A | General: gear / character diffuse / terrain brushes / effects / signs | `rdb_1010004` (11,715) | `rdb_1010016` (10,883) | `rdb_1010017` (11,146) |
| B | Map / ground (unnamed) | `rdb_1010009` (574) | `rdb_1010023` (571) | `rdb_1010024` (571) |
| C | Character body UV unwraps (torso/limbs/feet/faces) | `rdb_1010011` (265) | `rdb_1010019` (60) | `rdb_1010020` (60) |
| **D** | **Named terrain textures** (embedded name prefix) | `rdb_1010006` (849) | `rdb_1010021` (849) | `rdb_1010022` (849) |

Confirmed via `tools/profile_textures.py`:
- All paired tables have **exact 2.00× dimension ratio** for shared ids (verified across every pair).
- Jaccard id-overlap within chains: 0.89–1.00. Across chains: 0.00.
- JPEG-vs-PNG split is per-asset inside each table: ~70/30 JPEG/PNG in Chain A, reflecting per-asset alpha need.

**Named assets verified against catalog:**
- Chain A Tier 0 (`rdb_1010004`) has 11,576 of 11,722 rows named (98.8%). Categories from `tools/categorise_textures.py`: 2,611 character body; 1,375 buildings; 1,044 terrain/ground; 528 signs/posters; 339 armor/clothing; 247 props; plus ~6k uncategorised (mostly abstract-named tiles like `128x128_fringes.png` and numeric-prefix assets).
- Shadow-related assets: `rdb_1010004` contains 184 shadow-named textures (drop-shadow textures + Shadowlands-expansion assets: `shadow.png` id=18629, `shadowmutant.png`, `ground_cloudshadow.png` etc.).

The `ResourceManager::AddFallback` registry in ResourceManager.dll implements these chains; search xrefs to the export from Gamecode/N3/DisplaySystem at startup to identify which TypeIDs pair to which. (Not yet done — easy follow-up.)

**UI icons** (`rdb_1010008`, 7,449 rows, ~48×48 each) are standalone, no LOD pyramid.

## Name → asset-id mapping (SOLVED)

**`rdb_1000010`** is the master asset-name catalog. Single 1.4 MB row whose blob encodes 7 `TypeID_e` sections of named assets.

**Format (decoded, `tools/decode_name_catalog.py`):**
```
u32 version = 7
repeated per TypeID section:
  u32 type_id         (matches rdb_<type_id> table)
  u32 entry_count
  entry_count x:
    u32 asset_id
    u32 name_len      (bytes; no trailing null)
    u8[name_len] name (ASCII)
```

**Contents (40,473 names total):**

| type_id | Extension | Count | Table rows | Example |
|---------|-----------|------:|-----------:|---------|
| 1010001 | `.abiff` (meshes) | 11,828 | 9,925 | `midtech_building2.abiff`, `planet.abiff` |
| 1010002 | `.cir` (creature/character models) | 805 | 770 | `athrox_male.cir`, `solitus_male.cir` |
| 1010003 | `.ani` (animations) | 3,559 | 3,190 | `athrox_run_01_01.ani` |
| 1010004 | textures (`.png` / `.jpg` / `.psd` / `.tif` / `.bmp`) | 13,177 | 11,722 | `128x128basewall.png`, `door.png`, `shadow.png` |
| 1010005 | — | — | (no table) | dangling section |
| 1010008 | UI icons (`.png`) | 11,040 | 7,449 | `jar_icon48.png`, `armor_icon48.png` |
| 1010011 | character body parts (`.png`) | 60 | 265 | `feet_solitusmale_asian_naked.png` |

**"Dangling names"** — names in the catalog referencing asset ids not present in the table — are common (~18% of entries). These are assets that were named but whose blobs were stripped (cut content, replaced assets, etc.). They are harmless.

**"Unnamed table rows"** — blobs present without a catalog entry — exist for Chain A LOD tiers (1010016/17, Chain B 1010009/23/24, Chain C 1010019/20, UI icons 1010008) because the catalog only covers the canonical Tier-0 name; downstream LOD tiers share ids and resolve to the same name via the LOD relation.

**Tools:**
- `tools/decode_name_catalog.py` — parses `rdb_1000010` and emits `textures_extracted/_names.tsv` (40,473 rows, `type_id <TAB> asset_id <TAB> name`).
- `tools/rename_extracts_with_names.py --mode=rename` — renames extracted files to `<id>__<sanitised_name>.<ext>`, propagating Tier-0 names into LOD tiers.
- `tools/categorise_textures.py` — classifies named assets into 15 categories (`character_body`, `weapon`, `armor_clothing`, `terrain_ground`, `building`, `effect`, …).

**ElfHash(filename) is NOT the name→id mechanism** — tested in `tools/test_elfhash.py`. Every variant (with/without extension, case-folded) produced hashes that don't match any id in any texture table. The catalog is the ground truth; name→id is a direct table lookup, not a runtime hash.

## Open RE questions

1. **`DbObject_t` internal layout** — needed if we want to synthesise a resource object in the loose-file override rather than mutating the SQLite DB.
2. **`DatabaseController_t` vtable slot for "load by Identity"** — the call site inside `GetSync` at `(vtable + 8)`. RE'ing this gives a second hook target one layer down.
3. **xrefs to `ResourceManager::AddFallback`** — identify who registers the texture-tier chains and what the TypeID arguments are. This confirms the LOD mapping unambiguously and tells us whether the user's "Texture Resolution" setting picks start-type or short-circuits the chain.
4. **Name → id mapping** (above).
5. **`CHGA` mesh format** (table `rdb_1000009`) — out of scope for texture work.
6. **`rdb_1010002` / `rdb_1010003` anim blob format** (`Bip01_ac` magic) — out of scope.

## Immediate priority

If the texture pipeline is to be the next feature, start with step 1 above — a small offline Python script that reads `rdb.db` and extracts one texture table's JPEG blobs to disk. That's 15 minutes of work and validates every assumption in this document. Once we see the textures, picking the first HD target is trivial.
