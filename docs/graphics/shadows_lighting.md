# Shadows & Lighting — Findings and Possibilities

Investigation snapshot for the AO client's shadow/lighting systems and what AOReloaded can do with them. Sources: reverse-engineered `DisplaySystem.dll` and `N3.dll` via Ghidra (annotations persist in `disasm/`).

## Renderer context

AO runs on a DirectX 7 fixed-function pipeline (`D3Dim700.dll`). No programmable pixel or vertex shaders. All shadow/lighting effects are implemented with multitexture stages, render states, and projected textures. Users may additionally run a D3D wrapper (dgVoodoo, aoia-resfix, or none — per-user launcher choice; dgVoodoo2 itself was discontinued January 2026). Engine-side mods work regardless of wrapper choice.

The renderer is three layers deep inside `DisplaySystem.dll`:

- **`Randy_t`** — low-level D3D7 driver wrapper. Owns back/front buffer, device state, stencil/Z/alpha bits, texture memory, device-lost handling.
- **`render_t`** — middle D3D state wrapper. Manages materials, render states, transform matrices, lights.
- **`DisplaySystem_t`** — high-level façade. Viewport, resolution prefs, world↔screen coords, debug toggles. Singleton via `DisplaySystem_t::GetInstance`.

## Lighting model

Fixed-function forward rendering with **three independent light banks** managed by `RandyShadowlandsData_s` (statics in `DisplaySystem.dll`):

| Bank | Affects | Settable at runtime |
|------|---------|---------------------|
| **CATLight** | Character / CAT meshes (players, NPCs, mobs) | Direction, Intensity, Parameters (2× `Vector3_t` — colour + spread/attenuation), Texture |
| **StatelLight** | Static environment meshes (buildings, props, "statels") | Direction, Intensity, Parameters, Texture |
| **GroundLight** | Terrain | Direction, Intensity, Parameters, Texture, **projection Matrix** |

Plus:

- **Ambient light**: `Randy_t::SetAmbientLight(RGB)` — single global colour added to every surface.
- **Fog**: `Randy_t::EnableFog` + D3D7 `D3DRS_FOGCOLOR`, `D3DRS_FOGSTART`, `D3DRS_FOGEND`. Works with depth fog and table fog.
- **W-buffer**: `Randy_t::EnableWBuffer(bool)` — swap Z for W depth to increase precision at long far planes.
- **Max active lights**: `RVisual_t::SetMaxActiveLightCount(uint)` — ceiling on D3D7 active lights per draw.

Per-mesh: each visual's ref-frame exposes `SetRelativePosition`, `SetLocalDirection`, `SetTransparency`, `SetSpecular(RGB)`.

## Shadows as implemented today

### Drop shadows (the visible-in-game shadow)

Every character and object gets a **textured quad** planted under it. The textures are named `shadow1`, `shadow1_lavadry`, `shadow_snow`, `shadowtile1-4`, `shadowrock`, etc. — one per terrain type, so the shadow picks up the terrain colour.

- Entry point: `n3Playfield_t::AddShadow(n3Shadow_t*)` (N3.dll).
- `n3Shadow_t` manages the mesh list; `n3Playfield_t::GetShadowList` returns `vector<n3Shadow_t*>`.
- The shadow quad is *not* dynamic — it's a soft blob approximation of the caster's silhouette, scaled to character size, faded by a distance function.
- `cd_image/twk/Child_*.txt` tweak scripts reference the shadow textures.

No depth-based shadow test. No self-shadowing. No character-onto-character shadow.

### Day/night cycle (base game — do not re-propose)

The base game already ships a day/night system. Observable behaviour:

- Global lighting brightness varies with in-game time (likely `SetAmbientLight` + `SetCATLightIntensity` / `SetStatelLightIntensity` / `SetGroundLightIntensity` driven by a timer).
- Skybox texture may swap at key times.

**This is not something to re-build.** AOReloaded mods touching lighting should respect the existing schedule, not override it globally, unless the user explicitly wants a static colour (photo mode, accessibility).

## What AOReloaded can do — possibilities

### Tier 1 — runtime lighting writes (trivial once statics resolved)

All via `RandyShadowlandsData_s::Set*` + `Randy_t::SetAmbientLight`. No new code paths, only resolved-by-mangled-name calls:

- **Night brightness slider** (accessibility). DValue `AOR_NightBoost` adds a floor to ambient RGB while keeping vanilla day/night shape intact. Hook a per-frame update site (`DisplaySystem_t::Commit` is a candidate) and clamp `SetAmbientLight` from below.
- **Colour grading via light temperature**. Tint the three light banks independently — warm CAT key light, neutral Statel, cool Ground, as a preset. Purely cosmetic, no perf cost.
- **Flat / full-bright debug mode**. Ambient = (1,1,1), all directional lights off. Great for equipment screenshots or colour-reference.
- **Per-bank toggles**. Check boxes for "character lighting on/off", "statel lighting on/off", "ground lighting on/off" — useful for RE and debugging, marginal gameplay value.
- **Light-parameter tuning** exposed as advanced options. `SetCATLightParameters(v1, v2)` takes two 3-vectors; figuring out what each component means (likely colour + attenuation coefficients) unlocks a full lighting-tweaks panel.

### Tier 2 — texture replacement on existing shadow/light slots

- **HD drop shadows**. Swap the `shadow*` textures (see `textures.md`) for higher-resolution soft discs. Either hand-painted or runtime-generated once at init (a Gaussian disc into an offscreen texture, reused everywhere).
- **Custom CATLight texture**. The texture slot accepts any `RTexture_t*`. Swap for a cooler ramp or a cel-shading-style two-tone LUT for a stylised look.
- **Projected caustics on ground**. Replace GroundLight's projected texture with an animated caustic pattern for underwater/shoreline zones.

### Tier 3 — projected shadow prototype

Feasible within the existing API, no shaders needed:

1. Create an offscreen render-target (`Randy_t::SetRenderTargetAsTexture`).
2. Per frame, render the player mesh from directly above using a second `RCamera_t` into the RT. Draw only the silhouette (disable textures, use white-on-black material).
3. Bind the resulting texture as `CATLightTexture` or `GroundLightTexture` with a matching planar-projection matrix.
4. Terrain receives the silhouette as a "shadow" modulated by light intensity.

Caveats: silhouette only, no soft edges, no self-shadow, no LOD transition. Perf cost = one extra scene-graph pass per frame (cullable, character-sized). Useful mostly as a proof of concept — shows the engine can do something resembling real sun shadows without rewriting the pipeline.

### Tier 4 — exotic and not-recommended

- **Stencil shadow volumes** (Doom 3 / Unreal 2 era). D3D7 supports stencil, but implementation needs per-mesh edge detection at runtime. Effort disproportionate to payoff.
- **Screen-space ambient occlusion**. Impossible in pure D3D7 fixed-function (no depth-texture bind in pixel-shader land because there are no pixel shaders). Only achievable via a wrapper-layer post-process (ReShade over dgVoodoo) — not portable across wrapper choices.
- **Normal mapping**. No pixel shaders = no per-pixel lighting math. Not possible without replacing the renderer.

### Tier 5 — wrapper-layer augmentation

For users running dgVoodoo, a shipped ReShade preset can add SMAA, screen-space AO, colour grading, and tonemapping with zero AOReloaded code. Must be flagged as wrapper-dependent — users on aoia-resfix or raw D3D7 will not see it, and dgVoodoo itself won't receive further upstream fixes after its January 2026 EOL.

## Hook points summary

| Effect | Binary | Hook / write target | Cost |
|--------|--------|---------------------|------|
| Ambient floor (night boost) | DisplaySystem.dll | `Randy_t::SetAmbientLight` pre-call filter | Low |
| Light intensity/direction presets | DisplaySystem.dll | `RandyShadowlandsData_s::Set*Intensity/Direction/Parameters` | Low |
| Light texture swap | DisplaySystem.dll | `Set*LightTexture(RTexture_t*)` | Low |
| HD drop shadows | — | Replace `shadow*` textures in rdb.db (see `textures.md`) | Low (asset-side) |
| Projected sun shadow prototype | DisplaySystem.dll + N3.dll | Extra RT pass + GroundLight matrix/texture | Medium |
| Wireframe / occlusion debug overlays | DisplaySystem.dll | `VisualEnvFX_t::ToggleRandyDebugger*` | Trivial |

## Pre-existing debug / visualiser toggles

Already in the binary, can be surfaced behind DValue bools:

`ShowCATWireframe`, `ShowGroundWireframe`, `ShowLiquidWireframe`, `ShowStatelWireframe`, `ToggleFrustum`, `DepthDisplay`, `KDTreeDisplay`, `OcclusionBodies`, `OcclusionScan`, `OcclusionTest`, `OffscreenDisplay`, `RefractionDisplay`, `SphereTest`, `ShowSurfaceSliding`, `ShowMouseFix`, `DoNotRender`, `SyncDisplay`, `ToggleRenderBVolume`, `ToggleRenderEdge`, `ToggleRenderPrimCount`, `ToggleRenderVertex`.

Each is a parameterless toggle — mapping to a bool DValue is a one-liner per toggle.

## Open RE questions

1. `RandyShadowlandsData_s::SetCATLightParameters(v1, v2)` — meaning of the two vectors. Needs observation (set known values, watch output).
2. The day/night driver function — which symbol updates light intensities on a timer? Tracing xrefs to `SetGroundLightIntensity` will find it.
3. Drop-shadow scale and fade constants — likely in `n3Shadow_t` or `n3Playfield_t::AddShadow`. Needed before HD shadow swap decision (so we match mesh size).

## Priority for implementation

1. Resolve `Randy_t` + `RandyShadowlandsData_s` statics by mangled name in `aor::GameAPI`.
2. Ship **night brightness slider** (ambient floor) as first visible feature.
3. Ship the debug toggles panel — huge value for modders, trivial code.
4. Replace `shadow*` textures with higher-res versions via the rdb.db unpack pipeline (see `textures.md`).
5. Prototype projected ground shadow only after the above prove out; it's the most speculative item.
