# 10 — Hotspot System (full implementation spec)

**Expands `PLAN.md` §3.** Kernel-only fitter + `HotspotAtlas` resource + `.rect` I/O + patch-editor
document tab + apply commands. All fitter code lives in `modules/level_kernel` with **no editor
dependency** so `one-more-house/tools/checks/hotspot_fitter_check.gd` can drive it headless.

Reference reading done for this spec: DreamUV `DUV_HotSpot.py`/`DUV_Utils.py` (MIT — the readable
open fitter: aspect-bucket → size-bucket → random tie-break, U/V-swap rotation); Cyclops
`convex_volume.gd` (`fv.uv0 = f.uv_transform * fv.uv0` — dominant-axis planar base UV × per-face
`Transform2D`, with a later explicit per-loop `uv0` layer added alongside); Scythe hotspots doc
(4 mapping modes, `Shift+H` collinear-group / `Shift+F` individual, texel-density-first, patch
flags, inset-for-mip); Valve HL:A Hotspot Texturing + `.rect` subrect catalog; Insomniac Ultimate
Trim (tiling-trim reasoning); Zen UV / rmKit (trim-vs-hotspot storage).

---

## 1. Data model

### 1.1 `HotspotAtlas : Resource` (`modules/level_kernel/hotspot_atlas.{h,cpp}`)
Registered class, `.tres`-serialized, GDScript-exposed. One atlas per **pattern** (a texture family
shared by many materials), never per material.

| Field | Type | Meaning |
|-------|------|---------|
| `atlas_id` | `StringName` | stable key for caches/`.rect` provenance; never the material name (D-lesson: Scythe v0.8 same-name collision) |
| `reference_texture` | `Ref<Texture2D>` | authoring preview + **texel-density source dims**; the layout, not a runtime dependency |
| `texel_density_target` | `float` | texels per world metre the fitter aims for (default 256 px/m for the PSX kit) |
| `patches` | `Vector<HotspotPatch>` | the catalog |
| `default_mapping_mode` | `int` enum | Automatic / Square / Conforming / FollowActiveQuads |
| `disallow_random` | `bool` | force deterministic first-candidate pick (headless + user "pin" toggle) |
| `tiling_policy` | `int` enum | `NO` / `ALLOW` / `ONLY` — global gate over per-patch `allow_tiling` |
| `param_names` | `PackedStringArray` | recognized base-color uniform names, editor-configurable, default `["albedo_texture","BaseColor","base_color_texture","texture_albedo"]` |
| `target_materials` | `Array[StringName]` | material paths this atlas binds to (informational; binding is resolved by pattern, §1.4) |

Rects are stored **normalized (0..1, top-left origin)** in-resource so an atlas survives a texture
resize; px values in `.rect` are converted on import against the file's declared atlas dims.

### 1.2 `HotspotPatch` struct
```
struct HotspotPatch {
    Rect2  rect_uv;          // normalized, top-left origin, y-down in atlas space
    bool   allow_rotation;   // may 90° swap to match a tall/wide island
    bool   allow_mirror_x;   // flip U for variety
    bool   allow_mirror_y;   // flip V for variety
    bool   allow_tiling;     // "infinite trim": repeats along its long axis for faces longer than the patch
    int    tiling_axis;      // 0=U 1=V, only meaningful when allow_tiling
    float  inset_px;         // authored padding; mip-scaled at apply (§2.6)
    StringName patch_name;   // stable id for sticky re-fit (§3, difficulty E) and .rect labels
    // derived (cached, not serialized):
    float  aspect;           //  w/h in *texture pixels* (uses reference_texture dims)
    float  area_texels;      //  w_px * h_px
}
```
`aspect`/`area_texels` are recomputed on load and whenever `reference_texture` changes (keyed by
texture RID, never name).

### 1.3 `.rect` mapping (Source 2 / Mallet interchange)
`.rect` is a keyvalues subrect catalog: a top-level block with atlas pixel dims and a child list of
subrects, each `{ x y w h }` in px plus optional flags. Mapping table:

| `.rect` field | atlas field | conversion |
|---------------|-------------|-----------|
| atlas `width`/`height` | (transient) | divisor for px→normalized |
| subrect `x,y,w,h` (px) | `rect_uv` | `/dims`; y already top-left |
| `allowrotation` / `rotate` | `allow_rotation` | 1:1 |
| `mirrorhoriz` / `mirrorvert` | `allow_mirror_x/y` | 1:1 |
| `tile` / `infinite` | `allow_tiling` (+ `tiling_axis`) | 1:1 |
| `inset` / `border` | `inset_px` | px, kept absolute |
| subrect `name` | `patch_name` | 1:1; synthesized `p{i}` if absent |

Unknown keys are preserved in a `Dictionary extra` per patch and re-emitted on export
(round-trip fidelity — headless test §4). Export writes px against `reference_texture` dims.

### 1.4 Material ↔ atlas binding
A `HotspotBinding` registry (on `LevelEditor` service, mirrored into a serialized
`res://levels/hotspot_bindings.tres` so headless checks see it): maps **pattern key → atlas path**.
The pattern key of a material is `param_names`-resolved base-color texture's *directory + basename
stem* (e.g. `psx/brick` for `M_Brick_A`/`M_Brick_Wet` sharing `brick_albedo.png`). Texel density for
a face is read from **that material's own base-color texture dims** (a wet variant at 2× res gets 2×
density) — but the *patch layout* is shared. Cache key = texture RID/path, never material name.

---

## 2. Fitter algorithm (kernel, deterministic under seed)

Entry: `fit(sel_faces, mesh, atlas, mode, seed) -> Vector<FaceUVResult>`.
`FaceUVResult { face_index, uv0_per_loop OR uv_transform, patch_name }` (§3-D chooses which).

### 2.1 Island partition
```
GROUPED (Shift+H):  build islands by flood-fill over selected faces where an edge is shared AND
                    faces are "collinear/coplanar-continuous":
    two faces A,B merge iff:
      dot(A.normal, B.normal) >= COS_COPLANAR (0.9998, ~1.15°)          # same plane, or
      OR  they are collinear-continuous: share an edge E, and the pair
          forms a developable strip — |dot(A.n,B.n)| within COS_COLLINEAR (0.9998)
          AND the fold edge E is parallel (within 1.15°) to the strip's
          running direction  => an L/strip of wall faces around a corner
          is ONE surface unwound about E.
    "collinear faces treated as a single hotspot" == the transitive closure of that
    merge relation over the selection. A flat wall of N coplanar quads => 1 island.
    A wall that turns a 90° corner but keeps a common horizontal run => 1 island
    (unwound, §3-A). A wall meeting a floor (fold edge not parallel to run) => 2 islands.

INDIVIDUAL (Shift+F): each selected face is its own island (no merge).
```

### 2.2 Island → planar unwrap → world-space OBB
```
for each island:
  n = area-weighted average face normal
  # base planar UV: project every loop vertex to the island plane.
  # For a single planar face use dominant-axis projection (Cyclops convex_volume.gd:718-724):
  #   longest_axis(n)==X -> (-z,-y); ==Y -> (-x,-z); ==Z -> (-x,-y)
  # For a multi-face developable strip, unwind about shared fold edges first (§3-A) so the
  #   strip lies flat, THEN project.
  P = { planar_uv(v) for v in island loops }              # 2D points, in world metres
  # OBB in that plane: use edge-aligned box, not min-area, so results snap to wall orientation.
  #   axis U0 = normalized longest boundary edge of the island (gravity-biased: if any boundary
  #   edge is within 15° of world-horizontal, prefer it -> trims read "level")
  U0 = choose_reference_axis(island)      # horizontal-biased longest edge
  V0 = rotate90(U0)
  (umin,umax,vmin,vmax) = project P onto (U0,V0) and take extents
  world_w = umax-umin ; world_h = vmax-vmin      # metres
  island_aspect = world_w / world_h
  # record the 2D frame so we can map atlas rect -> loop UVs later
```
`choose_reference_axis` is what makes trims sit horizontally; it is the OBB derivation, and it is
seed-independent (deterministic).

### 2.3 Scoring — texel-density first, aspect within margin, random tie-break
Reconciles Scythe (density-first) with Hammer/DreamUV (aspect-bucket + random). Two-key sort with an
explicit margin so "close enough" candidates all stay eligible for variety:

```
# target patch pixel size that would hit texel_density_target on this island:
want_w_px = world_w * face_texel_density        # face_texel_density from THIS material's texture
want_h_px = world_h * face_texel_density
want_area = want_w_px * want_h_px
want_aspect = island_aspect

candidates = atlas.patches filtered by tiling_policy (§2.5) and by flags that can satisfy
             this island's orientation (rotation may make a wide patch tall, §2.4)

for p in candidates:
  # effective aspect after allowed rotation:
  a = p.aspect
  if p.allow_rotation and orientation_needs_swap(a, want_aspect): a = 1.0/a
  density_err(p) = abs(log2(p.area_texels) - log2(want_area))      # scale-invariant, octaves
  aspect_err(p)  = abs(log2(a)            - log2(want_aspect))      # symmetric wide/tall

# PRIMARY key: density. Find best density_err; keep all within DENSITY_MARGIN of it.
d_best = min(density_err)
bucket = { p : density_err(p) <= d_best + DENSITY_MARGIN }
# SECONDARY key: aspect, within the density bucket. Keep all within ASPECT_MARGIN of best.
a_best = min(aspect_err(p) for p in bucket)
finalists = { p in bucket : aspect_err(p) <= a_best + ASPECT_MARGIN }
# TIE-BREAK: random among finalists (anti-repetition), unless disallow_random.
rng = deterministic_rng(seed, island_stable_hash)     # island hash from sorted vertex ids
if atlas.disallow_random: pick = finalists[0]  (finalists pre-sorted by patch_name)
else:
    pick = rng.choice(finalists)
    if pick == last_pick_for_neighbor and len(finalists) > 1:   # DreamUV anti-repeat
        pick = rng.choice(finalists - {pick})
```

**Constants (editor-tunable, defaults):** `DENSITY_MARGIN = 0.35` octaves (~±27% area),
`ASPECT_MARGIN = 0.20` octaves (~±15%), `COS_COPLANAR = COS_COLLINEAR = 0.9998`,
horizontal-bias cone `15°`. Using `log2` for both error terms makes them scale-symmetric (a patch
2× too big scores the same as one 2× too small) — the fix for Hammer's raw-difference bias toward
large rects. Debug viz (§3-B) renders these buckets.

### 2.4 Rotation & mirror application
```
orientation_needs_swap(patch_aspect, want_aspect):
    return (patch_aspect >= 1) != (want_aspect >= 1)     # one wide, one tall
if chosen patch needs swap and allow_rotation: rotate island frame (U0,V0)->(V0,-U0) (90° CW)
if allow_mirror_x and rng.bit(): flip U within rect
if allow_mirror_y and rng.bit(): flip V within rect
# square patches (aspect==1) may take a random 0..3 quarter-turn if allow_rotation
#   (DreamUV uvcycle) — seeded.
```
All rng draws come from the same seeded stream so the whole fit is reproducible.

### 2.5 Tiling patches (faces longer than the patch)
`tiling_policy`: `NO` drops `allow_tiling` patches from candidates; `ALLOW` includes them competing
normally; `ONLY` restricts candidates to `allow_tiling` patches. When a tiling patch wins and the
island's length along `tiling_axis` exceeds the patch's world length at target density, the rect
**repeats** rather than stretches:
```
reps = max(1, round(island_len_along_axis * face_texel_density / patch_len_px))
# map island param t in [0..island_len] to atlas U (or V): u = rect.min + frac(t*reps) * rect.extent
# the cross axis maps 1:1 into the rect (trim height fixed). Seam falls on rep boundaries.
```
This is the Insomniac Ultimate-Trim behavior: one horizontal trim strip tiles across a long wall at
constant texel density with no stretch.

### 2.6 Inset (mip-aware) & final write
```
mip_levels = floor(log2(min(patch_px_w, patch_px_h)))          # usable mip chain of the patch
inset_uv   = (patch.inset_px + INSET_MIP_BLEED * mip_levels) / atlas_dims   # bleed grows with chain
rect_fit   = patch.rect_uv shrunk by inset_uv on all sides
# then for every loop: uv = rect_fit.min + normalized_island_frame_coord * rect_fit.size
```
`INSET_MIP_BLEED = 1.0` texel/level default: guards against the bevel/halo that appears when the GPU
samples a coarse mip across a patch border. Write path per §3-D.

---

## 3. Core difficulties (chosen solution each)

### A. Grouped island whose merged region is non-rectangular (L-shaped wall run)
**Problem:** `Shift+H` merges a corner-turning wall run into one island; its unfolded outline is an
L, not a rectangle, so a single rect either overhangs (waste) or crops.
**Chosen: unwind-then-fit as one strip, per-face UV split on the shared frame — NOT a per-face
re-fit, NOT one rect across the raw OBB.** During §2.2 we unwind the developable strip about its
fold edges into a single flat 2D layout (each face keeps its metric length; the fold becomes a
straight seam). The strip is then a rectangle-ish ribbon whose length = summed face runs; we fit ONE
patch (usually a tiling trim) across that ribbon length and split its U-range back onto each face by
arc-length. A genuinely 2D L (not developable — e.g. a wall notch) fails the §2.1 fold-parallel test
and splits into 2 islands, each fit independently. So: developable run = 1 sticky patch tiled along
the run; non-developable corner = clean per-sub-island fit. Sketch: `unfold_strip()` walks the face
adjacency spanning tree, accumulates 2D transforms across shared edges, flattens; `arc_split()`
partitions the fitted U back by cumulative edge length.
Refs: Insomniac Ultimate Trim (GDC 2015 — trims tiled along wall runs); DreamUV `DUV_HotSpot.py`
island bbox unwrap; Zen UV trim-sheet "unwrap to trim" docs.

### B. Scoring that "feels right" — reconciling Hammer margin+random with Scythe density-first
**Problem:** Hammer picks by aspect-margin then random (varied but density drifts); Scythe picks by
density (consistent but repetitive/deterministic-looking).
**Chosen: density is the hard primary key with a log2 margin; aspect is the secondary key with its
own margin; random tie-break only among the survivors, gated by `disallow_random`.** (§2.3.) This
keeps texel density visually locked (primary) while still giving Hammer's anti-repetition variety
(tertiary), and the log2 terms remove the large-rect bias. Constants exposed in editor settings.
**Debug visualization:** a "Hotspot Fit Debug" overlay colors each fitted face by which key decided
it — green = unique density winner, amber = tie broken by aspect, red = random tie-break (so a level
that looks too repetitive shows red clusters), plus a per-island HUD readout of
`want_area/want_aspect`, the finalist patch names, and chosen patch. Same data dumped as JSON by the
headless check for regression diffing.
Refs: Valve HL:A Hotspot Texturing (margin-of-error + random); Scythe hotspots doc (density-first);
Matthew Trevelyan Johns hotspot-texturing article (why aspect matching alone misreads scale).

### C. Storage: per-face `uv_transform` vs per-loop UVs (Conforming / Follow-Quads)
**Problem:** Cyclops's compact model is one `Transform2D` per face over a dominant-axis planar base
(`fv.uv0 = f.uv_transform * fv.uv0`). That is exact for planar Square mode but **cannot** express
Conforming (non-affine, per-loop) or Follow-Active-Quads (arbitrary per-loop) results, nor a tiling
seam that repeats within a face.
**Chosen: dual storage with a per-face `uv_mode_flag` selecting the interpretation** (Cyclops itself
migrated this way — it added an explicit `face_vertex_uv0` loop layer beside `uv_transform`).
`LevelMeshData` carries BOTH: per-face `uv_transform : Transform2D` (+ `material_index`, planar
base axis) AND an optional per-loop `uv0` array. Rule: Square/planar hotspot writes the compact
`uv_transform` (texture-lock survives geometry moves for free — the transform re-multiplies the new
planar base); Conforming, Follow-Quads, and tiling-with-internal-seam write **baked per-loop `uv0`**
and set `uv_mode_flag = EXPLICIT`, foregoing free texture-lock (re-fit needed after big edits,
which is acceptable and matches Scythe). The baker reads `uv_mode_flag` to decide which path
feeds the ArrayMesh. Sketch: `write_result()` branches on mode; `EXPLICIT` faces store loop UVs and
are excluded from the texture-lock re-multiply in transform ops.
Refs: Cyclops `convex_volume.gd` (uv_transform × planar base, later uv0 loop layer); rmKit UV docs
(per-loop conform); Godot `ArrayMesh` ARRAY_TEX_UV semantics.

*(Extra difficulties addressed inline: patch-editor UX in §5; re-fit stability below.)*

### D/E. Re-fit stability after small geometry edits — sticky vs jumpy
**Problem:** user tweaks a vertex and re-runs `Shift+H`; naive re-fit re-rolls the random tie-break
and every trim visibly jumps.
**Chosen: sticky-by-patch-name with a deterministic per-island seed.** Each fitted face records the
winning `patch_name`. On re-fit: recompute finalists (§2.3); if the previously-stored `patch_name`
is still in `finalists`, **keep it** (no re-roll). Only if the geometry changed enough that the old
patch left the finalist set do we pick anew. The tie-break RNG is seeded by
`hash(sorted island vertex ids) ^ atlas_id`, so an unchanged island is bit-identical across runs and
across machines (headless == editor). `disallow_random` forces `finalists[0]`. This gives Scythe-like
stability without a hidden per-face UUID.

---

## 4. Headless tests (`one-more-house/tools/checks/`)

Run: `godot ... --headless --path . --script tools/checks/hotspot_fitter_check.gd`
(after a one-time `--import` post-merge, per project CLAUDE.md).

1. **Seeded determinism** — fit the same mesh+atlas twice with the same seed; assert
   byte-identical `FaceUVResult` (patch_name + UVs). Then fit with `disallow_random=true`; assert
   it equals `finalists[0]` for every island and is stable across 100 runs.
2. **Texel-density bounds** — for a generated wall grid at known metres, assert every fitted face's
   realized `texels/m` is within `2^DENSITY_MARGIN` of `texel_density_target`; assert no face is
   stretched beyond `2^ASPECT_MARGIN` in aspect. Fails loudly if a patch stretched instead of tiled.
3. **`.rect` round-trip** — load a Source 2 `.rect`, export, reload; assert patch count, rects (px,
   within 0.5 px), all flags, and preserved `extra` keys match. Include a rect with rotation+tiling+
   inset + an unknown key.
4. **PSX-kit smoke fit** — build the 19-texture PSX atlases via the existing
   `tools/assets/convert_psx_modular_architecture.py` output, greybox a room from the kit, run
   grouped fit on all walls; assert: every face got a patch, no NaN/inf UVs, all UVs in the patch's
   inset rect, density-in-bounds, and sticky re-fit after a 1-vertex nudge leaves ≥95% of faces on
   their prior `patch_name`.
5. **Grouped-partition unit** — a coplanar wall of 5 quads → 1 island; an L developable run → 1
   island (unwound); a wall+floor → 2 islands; assert island counts and that `arc_split` U-ranges
   are contiguous and non-overlapping.

---

## 5. Patch Editor tab (workspace resource-document)

`HotspotAtlas` opens as a `ResourceDocument`-style tab (dogfoods the document model; no bespoke
window). Layout: left = zoomable/pannable texture view (`reference_texture` at 1:1 texel option),
right = patch list + per-patch flag inspector.
- **Rect drawing:** drag to create; handles to resize; **grid snap** (power-of-two + px grid,
  `[`/`]` to resize grid, snap toggle — same grammar as the level editor, never state-dependent).
- **Overlapping patches:** allowed and expected (nested trims); list-order = draw/priority order;
  selected patch draws on top with a highlighted outline, others dimmed. Hover cycles overlapping
  hits (`Alt+click` to pick the one underneath).
- **Per-patch inspector:** allow-rotation, mirror-x/y, tiling(+axis), inset px, name; live numeric
  rect (px, converted to normalized on store).
- **Live preview:** if a level pane has a selection, a "Preview on selection" toggle runs the fitter
  against the current selection with this atlas and shows the result in that viewport (non-committing)
  so patch-layout edits are seen on real geometry immediately.
- **`.rect` import/export** buttons wire to §1.3; **texel-density target** and **param-names** edit
  here. Binding editor (pattern → atlas) lives in a small sub-panel.

---

## 6. Apply commands (kernel ops through the document undo stack)

- **`Hotspot` (`Shift+H`)** — grouped: §2.1 GROUPED partition, fit, one undo entry (columnar UV
  diff). Uses `default_mapping_mode`.
- **`Hotspot Individual Faces` (`Shift+F`)** — per-face islands, one undo entry.
- Mapping-mode override via the Modify-Texture panel dropdown: Automatic (Square→Conforming on
  distortion > threshold), Square, Conforming, Follow Active Quads (never auto-picked — §2 mode gate).
- Selection changes are never undo steps; derived bake/overlay invalidates transactionally with the
  UV diff (PLAN.md undo rules). Fitter is pure/kernel; the command only wraps its result as a diff.
