# Lightmap AO — contextual ambient-occlusion baking for weathering masks

Status: **scoped, not started** (2026-07-07) · **Effort:** M overall (Phase 1 M, Phase 2 S–M, Phase 3 L and **deferred**) · **Depends on:** nothing in the workspace-editor track; touches `modules/lightmapper_rd` (upstream-shared code — see Risks)

Grounded in a code-mapping pass over `modules/lightmapper_rd/` and `scene/3d/lightmap_gi.{h,cpp}`
(file:line below, verified identical on `master` and `feature/workspace-editor` at merge-base
`895db87388`). Read alongside the workspace-editor planning docs for house conventions.

---

## What this is (and is not)

Add **AO baking** to the fork's GPU lightmapper. The output is an AO **texture** sampled on
**UV2** by custom spatial shaders as a **mask** driving procedural weathering — dirt/dust/grime
accumulating in occluded crevices, corners, and contact seams. It is **not** a lighting feature.

The value is **scene-aware, contextual AO**: occlusion contributed by *neighboring level
geometry* — a crate shadow-seamed against a wall, the junction line between two modular pieces —
which a per-asset Blender/Substance bake cannot know about. This grounds placed objects and makes
grime placement-aware. Because arenas are small and hand-authored, this bake can and must be
**near-real-time**: the overriding design priority is the asset-iteration loop, not bake quality
ceilings.

### Non-goals (explicit)

- **Do NOT bake AO into the lightmap's lighting** (no multiply of the lightmap by AO).
  `LightmapperRD` is a **path tracer**: the direct pass ray-traces shadows per light
  (`trace_direct_light`, `lm_compute.glsl:435-706`) and the bounce pass ray-traces
  environment/sky visibility (`trace_indirect_light` env-miss term, `lm_compute.glsl:801-804`)
  — occlusion is **already in the lightmap**. A separate AO multiply double-counts: crevices go
  wrongly black, and AO would darken direct light, which real AO must never do. Doing light-side
  AO "correctly" requires separating direct from indirect in the output — deliberately out of
  scope. *Future aside:* if light-affecting AO is ever wanted, it belongs in the **material AO
  channel** (`AO` + `AO_LIGHT_AFFECT` shader built-ins), never in the lightmap.
- **SSAO is not replaced and not built here.** SSAO is the complementary tool for grounding
  *dynamic* objects (players, props) that are not in the lightmap. It cannot substitute for the
  baked mask (view-dependent, no UV2 texture output). Out of scope; just enable it in the game
  renderer as usual.
- **No auto-wiring into materials in this feature.** The broader pipeline (asset import
  auto-binding the AO texture into the reusable "weathering" base-material) consumes this
  feature's output but is separate editor work. Here we only guarantee the output is a plain
  **bindable `Texture2D` resource** — for a mask sampled on UV2, that needs **zero** deep
  engine-material integration.
- **No incremental/partial re-bake in the initial delivery** (Phase 3, deferred — see below).

---

## The load-bearing finding (why this is tractable)

Everything AO needs already exists in `LightmapperRD`; AO is **an added accumulation term on an
existing GPU raytracer, not a new raytracer**:

1. **Per-lumel G-buffer already exists.** The raster pass (`_raster_geometry`,
   `lightmapper_rd.cpp:746-818`; `lm_raster.glsl`) renders every mesh's triangles in UV2 atlas
   space and writes world **position**, **normal**, and unocclude data per lumel
   (`lm_raster.glsl:92-94,159-166`). The AO pass reads exactly this, like the light passes do
   (`lm_compute.glsl:994,1000`).
2. **The ray tracer is a reusable compute function.** `trace_ray` (`lm_compute.glsl:149-309`)
   walks a 128³ DDA grid (`grid_size = 128`, `lightmapper_rd.cpp:1127`) of triangle clusters
   with an **any-hit early-out** (`p_any_hit` returns `RAY_ANY` immediately at
   `lm_compute.glsl:235-238`) — the cheap short-ray query AO wants, already wrapped as
   `trace_ray_any_hit` (`lm_compute.glsl:344-350`).
3. **Cosine-weighted hemisphere sampling already exists.**
   `generate_ray_dir_from_normal` (`lm_compute.glsl:387-393`) builds the tangent frame over
   `generate_hemisphere_cosine_weighted_direction` (`lm_compute.glsl:372-377`). With
   cosine-distributed directions, the plain hit-count average **is** the cosine-weighted AO
   estimator (importance sampling) — no extra weighting math.
4. **A "second output texture" precedent exists end to end: the shadowmask.** It is a second
   RGBA8 texture array allocated beside the lightmap (`lightmapper_rd.cpp:1275-1283`), written
   by the direct pass at its own binding (`lm_compute.glsl:66-67,975-977`), **denoised** via the
   same `_denoise` (`lightmapper_rd.cpp:2224-2237`), **dilated** via the same `_dilate`
   (`:2249-2254`), read back and converted to `FORMAT_R8` (`:2436-2443`), exposed via
   `get_shadowmask_texture{_count}` (`:2483-2490`), and saved/reimported by `LightmapGI`
   (`lightmap_gi.cpp:834-909`). The AO texture is the same shape of change.
5. **Per-mesh UV2→atlas mapping is already stored and queryable.** `MeshInstance.offset/.slice`
   assigned during packing (`lightmapper_rd.cpp:423-427`), exposed as
   `get_bake_mesh_uv_scale` (`:2501-2508`) and `get_bake_mesh_texture_slice` (`:2510-2513`),
   persisted by `LightmapGIData::add_user(path, uv_scale, slice_index, sub_instance)`
   (`lightmap_gi.cpp:61-66,1539-1541`; `User` struct `lightmap_gi.h:75-80`).

## Reuse GI rays, or a dedicated pass? (assessed against the shader)

**Dedicated pass.** The bounce pass's hemisphere rays (`lm_compute.glsl:1005-1007`) are consumed
*inside* `trace_indirect_light` (`:722-858`) as **world-length** closest-hit path traces
(`position + ray_dir * length(bake_params.world_size)`, `:733`) with albedo/emission texture
fetches, transparency retries, and Russian roulette. No per-ray first-hit distance is surfaced
to the caller (it returns only radiance), so "piggybacking" would mean threading distance out of
the path tracer **and** coupling AO output to a full GI bake — which defeats the entire fast-loop
premise. AO wants the opposite profile: **many, short, any-hit** rays. A dedicated
`MODE_AO` shader version (a new entry in the `#[versions]` list, `lm_compute.glsl:1-9`) that does
`generate_ray_dir_from_normal` + `trace_ray_any_hit` capped at `ao_max_distance` is ~40 lines,
reuses every helper, and is dispatchable **without any light pass running**. The `MODE_UNOCCLUDE`
version (`lm_compute.glsl:1045-1090`) is the exact structural precedent: a small self-contained
raycasting mode with its own uniform set.

**Where it inserts (the exact seam).** In `LightmapperRD::bake()` the pass order is:

| # | Pass | Where |
|---|---|---|
| 1 | Atlas packing (`_blit_meshes_into_atlas`) | `lightmapper_rd.cpp:306-433`, called `:1137` |
| 2 | Texture allocation block | `:1242-1341` |
| 3 | Acceleration structures (`_create_acceleration_structures`) | `:435-744`, called `:1372` |
| 4 | `BakeParameters` UBO | `:1398-1434`; GLSL mirror `lm_common_inc.glsl:2-23` |
| 5 | UV2 raster → position/normal/unocclude (`_raster_geometry`) | `:1613` |
| 6 | UNOCCLUDE compute | `:1717-1751` |
| 7 | PRIMARY (direct light) | `:1797-1902` |
| 8 | SECONDARY (bounces) | `:1923-2051` |
| 9 | Light probes | `:2057-2166` |
| 10 | SH pack (`_pack_l1`) → DENOISE (`_denoise`/`_denoise_oidn`) → DILATE | `:2183-2256` |
| 11 | BLEND SEAMS raster | `:2267-2414` |
| 12 | Readback → `lightmap_textures` | `:2429-2434` |

The **AO pass slots between 6 and 7** (it wants the unocclude-corrected lumel positions —
`MODE_UNOCCLUDE` pushes origins out of intersecting geometry, `lm_compute.glsl:1075-1082`,
directly improving AO in exactly the tight corners we care about). In **AO-only mode**, passes
7–9 and the SH pack are **skipped entirely**; denoise + dilate run on the AO texture (shadowmask
precedent); blend seams and readback follow. Dispatch shape copies the direct pass: region-chunked
with `submit()/sync()` per region (`:1857-1901`, region size from
`rendering/lightmapping/bake_performance/region_size`, `:1775`) so GPU-timeout safety is inherited.

---

## Design

### The AO term

Per lumel: `N` cosine-weighted hemisphere rays around the (raster-pass) normal, each traced
`any_hit` with max distance `ao_max_distance` (world units, default ~0.5–1.0 m — grime is a
*local contact* effect; short range is also what keeps it fast and what makes it stable under
unrelated far-away edits). `ao = 1 - hits/N` (1 = open, 0 = fully occluded — mask convention:
weathering shader uses `1 - ao` as grime density). Own sample count (`ao_ray_count`, default 128,
clamped 16–8192 like the light passes, `lightmapper_rd.cpp:1795`), independent of GI quality
settings. Optional smooth falloff (`1 - (d/max_d)^k` needs closest-hit distance via
`trace_ray_closest_hit_distance`, `lm_compute.glsl:338-342`) is a param-gated variant to evaluate
in Phase 1 — binary any-hit is cheaper and denoises well at 128+ rays; don't pay for falloff
until a visual comparison says so.

Backfaces: in any-hit mode `trace_ray` returns `RAY_ANY` regardless of facing (`:235-238`) —
count it occluded. Lumels *inside* geometry are already rescued by unocclude. Transparency
(alpha-scissor fences etc.) occludes fully in MVP — no albedo fetch in the AO ray loop (that is
part of what makes it fast); flagged in Open Questions.

### Shader + parameter changes (all additive)

- `lm_compute.glsl`: new version `ao = "#define MODE_AO";` in `#[versions]` (`:1-9`); new
  `#ifdef MODE_AO` uniform block (set 1: `source_position`, `source_normal`, writeonly
  `dest_ao` image — mirrors the direct pass block `:56-64`) and main-body branch (~40 lines
  next to `MODE_UNOCCLUDE`'s, `:1045-1090`). Reuses `params.ray_count/ray_from/ray_to` from the
  existing push constant (`lightmapper_rd.h:266-274`) for chunked ray budgets exactly like the
  bounce pass (`lightmapper_rd.cpp:2015-2022`).
- `BakeParameters`: append `ao_max_distance` (+ pad/falloff fields to keep the 16-byte row) in
  **both** `lightmapper_rd.h:42-62` and `lm_common_inc.glsl:2-23`. Appending a trailing row is
  std140-safe and rebase-friendly.
- `lightmapper_rd.{h,cpp}`: AO texture RIDs allocated in the texture block (`:1242-1341`,
  RGBA8 like shadowmask — single channel used, converted to `FORMAT_R8` on readback); new
  `ao_textures` vector + `get_bake_ao_texture{_count}` accessors mirroring shadowmask's
  (`:2483-2490`); new entry point (below).

### AO-only bake ≠ full lightmap bake (the iteration-speed core)

A new `Lightmapper::bake_ao(...)` virtual (default-error in the abstract base
`scene/3d/lightmapper.h`, implemented by `LightmapperRD`) that runs: atlas-pack → accel →
raster → unocclude → **AO** → JNLM denoise → dilate → readback. What it *removes* relative to a
full bake, per the measured-in-code cost centers:

- **No `bake_render_uv2` material capture.** The full bake's prep renders every mesh's
  albedo/emission on UV2 via the GPU and reads it back (`lightmap_gi.cpp:1124-1175`) — a
  per-mesh raster + readback. AO rays never fetch albedo. **Wrinkle found in source:**
  `add_mesh` hard-requires non-empty albedo/emission images (`lightmapper_rd.cpp:60-64`) and the
  atlas packer derives each mesh's atlas rect **from the albedo image dimensions**
  (`:311-313`) — the image doubles as the size carrier. The AO path therefore feeds cheap
  solid-white `Image`s created at the target lumel size (no GPU capture), preserving packing
  behavior without touching `_blit_meshes_into_atlas`.
- **No direct pass, no bounces, no probes, no SH, no environment.** Passes 7–9 skipped.
- **JNLM denoise only** (`_denoise`, `lightmapper_rd.cpp:1017-1102`), never OIDN in the loop:
  the OIDN path shells out to an external exe with PFM disk round-trips per slice
  (`:951-1015`) — the wrong latency profile for a debounced auto-bake. JNLM already accepts an
  arbitrary source/dest texture pair + normals + unocclude mask (shadowmask precedent `:2232`),
  `slice_count = 1`.
- **Independent (lower) texel density.** An `ao_texel_scale` (default 0.5–1.0 of the lightmap
  size hint, `lightmap_gi.cpp:1107-1114` shows the sizing math to mirror) — grime masks don't
  need lighting-grade density; quarter the lumels ≈ quarter the bake. This is the single
  biggest free knob.

With short rays, ~128 samples, no bounces, no material capture, and reduced density, an
arena-scale AO-only bake is expected in the **hundreds of milliseconds** — but that is a
prediction, and per house rules it gets **measured before** any further optimization is designed
(Phase 2 gate). Note the biggest fixed cost that *remains*: acceleration-structure build is
CPU-side per bake (vertex dedup, octree plot into the 128³ grid, cluster sort,
`lightmapper_rd.cpp:435-744`) plus local-`RenderingDevice` creation (`:1176-1210`). If
measurement shows accel/setup dominating, caching those across bakes is the first lever —
**before** any incremental-lumel scheme.

### Output storage: per-mesh textures first, shared atlas later

- **(b) Per-mesh AO textures — MVP.** After readback, crop each mesh's rect out of its atlas
  slice using the stored `offset`/`slice` (`get_bake_mesh_uv_scale` already encodes the exact
  rect math, `lightmapper_rd.cpp:2501-2508`), save as an R8 PNG per mesh. The mesh's **raw UV2
  samples it directly** — a custom shader binds `sampler2D ao_mask` and reads
  `texture(ao_mask, UV2).r`, zero per-instance plumbing. Dilation inside each rect (atlas rects
  are padded, `:346,372`) keeps bilinear sampling clean at island edges. Cost: packing
  inefficiency and one texture bind per mesh — irrelevant at arena scale.
- **(a) Shared UV2 atlas (texture array + per-mesh rect/slice) — later, if ever.** Efficient and
  it reuses `LightmapGIData`'s user-mapping shape (`add_user`, `lightmap_gi.h:75-96`), but a
  *custom material* (unlike the lightmap, which is applied per-instance by the renderer via
  `RS::instance_geometry_set_lightmap`, `lightmap_gi.cpp:1747-1752`) has no ambient way to learn
  its rect/slice — it needs per-instance shader uniforms wired by tooling. That plumbing belongs
  with the weathering-material auto-wiring work, not the MVP. Decision recorded below.

### Node: standalone AO baker, not LightmapGI fusion

A new tools-only node (working name **`AOBaker3D`**) owning the AO bake path, rather than a mode
on `LightmapGI`:

- The product is a **material input**, not lighting; coupling it to `LightmapGI` couples the fast
  loop to GI concepts (environment, probes, `LightmapGIData` lifecycle) it doesn't use, and
  most arena scenes will want AO *before* any lightmap exists.
- `LightmapperRD` is reused underneath either way (`Lightmapper::create()` factory).
- The node duplicates a *minimal* mesh-only gather (UV2 + size hint + GI-static filter) instead
  of refactoring `LightmapGI::_find_meshes_and_lights` (private, `lightmap_gi.cpp:1091`) into a
  shared helper — deliberate duplication: ~60 lines copied beats creating a merge-conflict
  surface inside `lightmap_gi.cpp` (Decisions).
- Persisted output: a small resource on the node mapping `NodePath → Ref<Texture2D>` (+ bake
  params), which the future import-pipeline/weathering-material tooling consumes.

### Editor iteration loop (primary priority)

- **Debounced auto-bake:** an editor plugin (or the node itself, tools-only) arms a one-shot
  ~300 ms timer on relevant scene mutations (transform changes / add / remove of GI-static mesh
  instances under the bake root); timer fire → AO-only bake → textures hot-update in place
  (`Image` → existing `ImageTexture` update, no reimport in the loop; disk save + `.import`
  bookkeeping happens on scene save, following `_save_and_reimport_atlas_textures`'s pattern
  `lightmap_gi.cpp:834-909` only at save time). No bake button in the flow (keep a manual action
  for recovery/debugging).
- **Whole-level bake first, measure, then decide.** Arenas are small and AO rays are short;
  Godot's baker is a full-scene batch today — `bake()` rebuilds *all* state per call
  (`lightmapper_rd.cpp:1104-2472`); there is no partial-bake machinery anywhere. The benchmark
  (Phase 2) instruments per-phase wall times on a representative arena. Only if total time
  crosses the comfort threshold (working target: **≤ 1 s** debounce-to-texture; stretch 250 ms)
  does Phase 3 get scheduled.
- **Threading note:** the bake runs on a **local** `RenderingDevice`
  (`create_local_rendering_device()`, `:1181`) — designed for offline compute, independent of
  the frame graph. MVP runs synchronously (an editor stall of a few hundred ms on debounce fire
  is acceptable to start); if measurement says otherwise, moving `bake_ao` to a worker thread is
  the mitigation to validate (local-device thread affinity is the open verification item).

---

## Phased build order

### Phase 1 — MVP: AO-only bake → per-mesh textures → sampled in a test material (M)

1. `MODE_AO` shader version + `ao_max_distance` in `BakeParameters` (both mirrors).
2. `LightmapperRD::bake_ao()` (atlas-pack with white dummy images → accel → raster → unocclude →
   AO pass region-chunked → JNLM denoise → dilate → readback) + `get_bake_ao_texture{_count}`.
   Reuse `DEBUG_TEXTURES` dumps (`lightmapper_rd.cpp:57-58`) for verification.
3. `AOBaker3D` node: mesh gather, `ao_texel_scale` / `ao_ray_count` / `ao_max_distance` /
   `denoise` properties, manual bake action, per-mesh crop + save, NodePath→texture map.
4. Hand-write one test weathering shader sampling the mask on UV2; verify a crate-against-wall
   contact seam produces contextual grime a per-asset bake could not.
   **Milestone: the capability exists end to end.**

### Phase 2 — Iteration loop: debounce + measure-first benchmark; atlas option (S–M)

5. Debounced auto-bake plugin (300 ms), in-place texture hot-update, save-time persistence.
6. **Benchmark gate:** per-phase timings (prep / accel / raster+unocclude / AO dispatch /
   denoise / readback / crop+save) on the representative arena, at 3 density/ray presets. This
   table is the deliverable that decides everything after it.
7. Cheap wins *if the table demands them, in this order:* lower `ao_texel_scale` default → skip
   denoise below a ray-count threshold → cache accel structures + local RD across bakes → worker
   thread. Optional: blend-seams pass over the AO texture (`:2267-2414` is generic over a
   framebuffer'd slice) if UV island seams show in masks.
8. Optional: shared-atlas output mode behind a flag, only once the weathering-material tooling
   exists to wire per-instance rect/slice uniforms.

### Phase 3 — DEFERRED: incremental regional re-bake (L)

Re-bake only lumels within `ao_max_distance` of edited objects' AABBs (short rays give AO a
naturally *local* dependency footprint — this is the one thing that makes partial AO bakes
sound), with persistent accel structures + grid/cluster refit and region-scoped dispatch via the
existing `region_ofs` mechanics. This is real engine work against a baker that is a from-scratch
batch today; **do not start it** unless the Phase 2 table shows whole-level time above threshold
on real content.

## Files to touch

| Path | Change |
|---|---|
| `modules/lightmapper_rd/lm_compute.glsl` | New `ao` entry in `#[versions]` (:1-9); `MODE_AO` uniform block + main-body branch (pattern: `MODE_UNOCCLUDE` :1045-1090); no edits to existing modes. |
| `modules/lightmapper_rd/lm_common_inc.glsl` | Append `ao_max_distance` (+ padding/falloff) row to `BakeParameters` (:2-23). |
| `modules/lightmapper_rd/lightmapper_rd.h` | Mirror `BakeParameters` field (:42-62); `ao_textures` vector; `bake_ao()` decl; AO accessors (pattern: shadowmask :321-322). |
| `modules/lightmapper_rd/lightmapper_rd.cpp` | `bake_ao()` implementation composed from existing helpers (`_blit_meshes_into_atlas` :306, `_create_acceleration_structures` :435, `_raster_geometry` :746, unocclude dispatch :1717, `_denoise` :1017, `_dilate` :840); AO texture alloc (pattern :1275-1283); AO pass dispatch (pattern: direct pass :1856-1901); readback (pattern :2436-2443). |
| `scene/3d/lightmapper.h` | `virtual BakeError bake_ao(...)` on the abstract `Lightmapper` + AO texture accessors (default: not-implemented). |
| `scene/3d/ao_baker_3d.{h,cpp}` (new) | Tools-only node: mesh gather (minimal copy of the mesh half of `_find_meshes_and_lights`), bake orchestration, white dummy image prep (sized per `lightmap_gi.cpp:1107-1114` math × `ao_texel_scale`), per-mesh crop via `get_bake_mesh_uv_scale`/`get_bake_mesh_texture_slice` (:2501-2513), texture save/hot-update, NodePath→texture resource. |
| `editor/plugins/ao_baker_plugin.{h,cpp}` (new, or in-node) | Debounced (300 ms) auto-bake trigger; manual bake action; benchmark timing dump. |
| `scene/3d/lightmap_gi.cpp` | **Reference only — no changes.** Save/reimport pattern (:834-909) and user-mapping pattern (:61-66,1539-1541) are copied, not modified. |

## Decisions

- **Dedicated short-ray AO pass vs piggybacking GI rays.** *Decision: dedicated `MODE_AO`.*
  Verified in source: bounce rays are world-length closest-hit path traces with no hit-distance
  surfaced (`lm_compute.glsl:722-858`); reuse would couple AO to a full GI bake and defeat the
  fast loop. The prompt-level hypothesis survived contact with the shader.
- **AO-only bake as a first-class entry point vs a flag soup on `bake()`.** *Decision: new
  `bake_ao()` composed from the same private helpers.* `bake()` is a 1,370-line function with
  interleaved abort/free paths; a separate composition is safer to write and far safer to
  rebase than threading `if (ao_only)` through it.
- **Per-mesh textures vs shared atlas.** *Decision: per-mesh for MVP, atlas later behind the
  weathering-tooling work.* Per-mesh = raw UV2 sampling, zero instance plumbing; atlas needs
  per-instance rect/slice uniforms that only make sense once material auto-wiring exists.
  Tradeoff (packing efficiency, bind count) is a non-issue at arena scale.
- **Standalone `AOBaker3D` node vs fusing into `LightmapGI`.** *Decision: standalone.* The
  output is a material input; the fast loop must not drag `LightmapGIData`/environment/probe
  lifecycle. Both reuse `LightmapperRD`. Mesh gather is *duplicated minimally* rather than
  refactored out of `lightmap_gi.cpp` — copied 60 lines beat a new conflict surface in
  upstream-shared code.
- **Dummy white albedo/emission images vs new size-only `add_mesh` API.** *Decision: dummy
  images.* `add_mesh` validation (:60-64) and atlas sizing (:311-313) stay untouched; the cost
  (a few MB of white RAM images, no GPU capture) is trivial; zero divergence in shared code.
- **JNLM in the loop; OIDN never auto-triggered.** Process-spawn + PFM disk I/O per slice
  (:951-1015) is incompatible with a 300 ms debounce loop. JNLM is generic over the texture
  (shadowmask precedent :2232).
- **Whole-level bake first; incremental deferred behind a measurement gate.** The baker has no
  partial-bake machinery (full state rebuild per `bake()`); building it is L-sized engine work
  that only pays if the Phase 2 table says whole-level time is above threshold. Measure-first,
  per house rule (G5).
- **Mask polarity:** store openness (1 = open); weathering shaders invert. Matches how AO is
  conventionally authored and previews legibly in the editor.

## Risks

- **Fork divergence in `modules/lightmapper_rd`** — this is upstream-active code (area lights,
  shadowmask, supersampling all landed recently) and a real rebase-conflict surface.
  *Mitigation:* every change is **additive** — new shader version block, appended UBO row, new
  methods, new files; zero edits inside existing pass bodies; the one shared struct touched
  (`BakeParameters`) is append-only in both mirrors. Keep the node + plugin in new files.
- **Denoise quality on masks.** JNLM sigmas are hand-tuned for HDR radiance
  (`lightmapper_rd.cpp:1020-1026`); a binary-ish mask may over-smooth crevice edges (grime
  detail is the product). *Mitigation:* AO rays are cheap — prefer more rays over stronger
  denoise; expose strength; evaluate denoise-off at ≥256 rays in Phase 1.
- **Atlas indexing / rect-crop bugs** (offset/slice mismatch, padding off-by-one) show up as
  grime bleeding across UV islands. *Mitigation:* `DEBUG_TEXTURES` dumps (:57-58) at each stage;
  a checkerboard-mesh test scene; the rect math is copied from `get_bake_mesh_uv_scale`
  (:2501-2508), not re-derived.
- **UV2 unwrap availability.** The bake silently skips surfaces without UV2/normals
  (`ERR_CONTINUE`, `lightmap_gi.cpp:1200-1201`) and falls back to a 64×64 hint when the mesh has
  no lightmap size hint (:1108-1112). Fine for GI, poison for a mask workflow. *Mitigation:*
  `AOBaker3D` surfaces per-mesh warnings (missing UV2 / missing size hint) in its editor UI;
  the importer's "use lightmap UV2" must be part of the asset checklist.
- **Editor stalls from synchronous bake** on debounce fire. *Mitigation:* measurement gate;
  local-RD worker thread as the prepared fallback (verify local `RenderingDevice`
  thread-affinity before committing to it).
- **GPU timeout on large dispatches** — inherited mitigation: region-chunked dispatch with
  submit/sync, same as the light passes (:1857-1901).

## Open questions

1. **Transparency policy:** should alpha-scissor surfaces (fences, grates) occlude fully, not at
   all, or via an albedo-alpha fetch in the AO loop (costs a texture fetch per hit)? MVP: fully
   occlude; revisit with real arena content.
2. **Falloff:** binary any-hit vs distance-falloff (closest-hit) — decide from the Phase 1
   side-by-side; falloff doubles ray cost by forfeiting the any-hit early-out.
3. **Comfort threshold:** is ≤ 1 s debounce-to-texture acceptable for the authoring loop, or is
   250 ms the real bar? Sets whether Phase 3 ever exists. **NEEDS USER INPUT** (and the Phase 2
   table).
4. **Representative arena:** which map + target lumel density (per-mesh 128²? 256²?) defines the
   benchmark workload? **NEEDS USER INPUT.**
5. **Static-only scope:** AO from GI-static meshes only (matches lightmap semantics), or include
   dynamic-marked props the artist has placed? MVP: static-only.
6. **Should `ao_max_distance` also clamp the DDA traversal** (`trace_ray`'s `p_to` already bounds
   it — verify the grid walk terminates early on short rays and doesn't pay full-cell-walk cost).
7. **Per-mesh texture lifecycle:** saved next to the scene vs under `.godot/` cache with only the
   node resource in-scene? Interacts with the future import-pipeline auto-wiring.
