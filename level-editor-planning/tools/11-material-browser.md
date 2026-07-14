# Tool 11: Material Browser Dock + Active Material State

Owner subsystem: `LevelEditor` service (`editor/level/level_editor.{h,cpp}`), per PLAN.md's line
"owns ... the material browser dock and active material." This document details that ownership,
the indexing/thumbnail pipeline behind it, and the blockout-material quick-slot system. Scope:
LE2 (Fast Texturing) per PLAN.md phase order — material browsing must exist before hotspots
(LE3) can target materials.

Research base: `editor/inspector/editor_preview_plugins.{h,cpp}` and
`editor/inspector/editor_resource_preview.{h,cpp}` (read in full for this doc — see §0 findings),
`editor/file_system/editor_file_system.{h,cpp}` signals, OMH's
`resources/world/architecture/palettes/*.tres` + `default_palette_catalog.tres` (confirms the
`M_*` material-naming convention and the "palette references materials by `ExtResource` path"
pattern already live in the game), Scythe Editor's Active Material panel + quickslot docs, Source 2
Hammer asset browser.

---

## 0. Load-bearing findings from reading the existing preview infra

1. `EditorResourcePreview::cache` (`editor_resource_preview.h:103`) is `HashMap<String, Item>`
   keyed by **resource path** for saved resources, or by `"ID:" + instance_id` for
   `queue_edited_resource_preview` (unsaved/in-memory resources, hashed via
   `Resource::hash_edited_version_for_preview()` — see `editor_resource_preview.cpp:485-495`).
   Path-keying already rules out Scythe's name-collision bug *for this cache*; the browser's
   **own** indexes (favorites, recent, texel-density cache, hotspot badge lookup) must follow the
   same discipline independently — nothing here protects those.
2. **One preview per path, globally shared.** `_generate_preview` (`editor_resource_preview.cpp:175-253`)
   iterates `preview_generators` in registration order, calls `handles(type)`, and **breaks on the
   first generator that returns a non-null texture.** `EditorMaterialPreviewPlugin` is registered
   once at `editor_node.cpp:9997` and answers for every `Material` in the editor — FileSystemDock,
   inspector previews, and (if naively registered the same way) our material browser would all
   read/write the *same* cache entry for a given path. **This means a flat/cube variant cannot be
   a second competing `add_preview_generator()` entry** — see Difficulty 1.
3. Disk cache: `resthumb-<md5(globalized path)>.png` under `EditorPaths::get_cache_dir()`
   (`editor_preview_plugins.cpp:300-302`), invalidated by mtime/`.import` mtime comparison
   (`check_for_invalidation`, `editor_resource_preview.cpp:561-583`). Any parallel cache we add
   must use a disjoint filename namespace or it will silently clobber/be clobbered by the shared one.
3. `EditorFileSystem` signals available for indexing (`editor_file_system.cpp:3852-3857`):
   `filesystem_changed` (full-tree structural change, main thread only), `sources_changed(bool exist)`
   (initial scan done / rescan done), `resources_reimporting`/`resources_reimported` (paths about to
   / just changed), `resources_reload` (paths whose in-memory `Resource` should be swapped). No
   signal fires *only* for "a `.tres` Material was edited and saved" — that surfaces as
   `resources_reimported` (has `.import`, e.g. re-baked texture) or a plain filesystem write picked
   up by the next `filesystem_changed`. We must handle both.
4. `EditorFileSystemDirectory::get_file_type(idx)` (`editor_file_system.h:96`) gives the resource's
   top-level class (`"StandardMaterial3D"`, `"ShaderMaterial"`, `"ORMMaterial3D"`, …) without
   loading the resource — this is what makes indexing cheap.

---

## 1. End-to-end spec

### 1.1 Indexing (`MaterialIndex`, `editor/level/material_index.{h,cpp}`)

A `RefCounted` (or plain object) owned by `LevelEditor`, built once per editor session, kept warm
for the life of the editor (not per-document — materials are project-global).

- **Cold build** (on `EditorFileSystem::sources_changed(true)`, i.e. after the initial project
  scan, and lazily on first dock open if the scan already finished): walk
  `EditorFileSystem::get_filesystem()` recursively; for every file whose `get_file_type()` is
  `Material` or a registered subclass (`ClassDB::is_parent_class(type, "Material")`), create a
  `MaterialIndexEntry { path, display_name, folder, class_name, is_convention_named, favorite,
  last_used_usec }`. `display_name` strips the `M_` prefix per project convention
  (`assets/environment/psx_textures_ii/materials/M_DinerTealBand.tres` → `DinerTealBand`);
  `is_convention_named` flags whether the prefix matched at all, feeding the filter in §1's UI.
  O(number of project files) once; not repeated on every dock open.
- **Incremental update**: subscribe to `filesystem_changed` (re-diff only — walk the tree again
  but only insert/remove entries whose path didn't exist in the previous snapshot; the directory
  tree is already small enough post-cold-build that a full re-walk-and-diff is cheaper than trying
  to hook per-file add/remove deltas that `EditorFileSystem` doesn't expose directly) and
  `resources_reimported`/`resources_reload` (update the entry in place — display name/folder don't
  change, but this is the trigger to invalidate any cached texel-density reading, §3.3, and to ask
  `EditorResourcePreview::check_for_invalidation(path)` — that call already exists and handles the
  thumbnail side).
- **Never loads Materials into memory during indexing.** Only `get_file_type` string comparison.
  Actual `Resource` loading happens on demand (row becomes visible → thumbnail requested → texel
  density scan needs the material → `ResourceLoader::load(path)`, cached by Godot's own resource
  cache thereafter).
- Emits `material_added(path)`, `material_removed(path)`, `material_changed(path)` signals; the
  dock's virtualized grid (§3.2) listens and patches its model instead of rebuilding.

### 1.2 Thumbnail queue

Two independent thumbnail paths coexist deliberately (see Difficulty 1 for why they can't share
one):

- **Sphere thumbnails** (default shape, matches every other Godot material-preview surface the
  team already knows from the inspector/FileSystemDock): use the *existing*
  `EditorResourcePreview::queue_resource_preview(path, callback)` — zero new plumbing, gets the
  disk-cache + async-thread + invalidation behavior for free, and stays visually consistent with
  the inspector.
- **Flat/cube thumbnails** (trim/atlas materials, opt-in per §3's Difficulty 1): a dedicated
  `MaterialBrowserPreviewQueue` owned by `MaterialBrowserDock`, described in Difficulty 1.
- **Priority**: both queues are FIFO-with-reprioritization — the grid, on scroll, calls a
  `reprioritize(visible_paths)` that moves visible-row entries to the front. `EditorResourcePreview`
  itself has no priority concept (plain `List` FIFO, `editor_resource_preview.cpp` queue field) so
  for the sphere path this reprioritization is implemented in the *dock*, not the engine: the dock
  simply avoids calling `queue_resource_preview` for off-screen rows until they scroll near-visible
  (a lazy-enqueue windowed scheme), rather than trying to reorder Godot's internal queue.

### 1.3 Active-material state ownership

Lives on `LevelEditor` (the service), not `LevelEditorView` (per-pane) or `MaterialBrowserDock`
(the UI) — per PLAN.md's explicit assignment and the "no render state in the service" rule (the
active material is a `Ref<Material>` + source path, not render state; the *swatch widget* that
displays it is render state and belongs to the view).

```
class LevelEditor {
    Ref<Material> active_material;
    String active_material_path;              // empty if an in-memory/unsaved material
    void set_active_material(const Ref<Material>&, const String &path);
    // signal active_material_changed(material, path)
};
```

Every open `LevelEditorView` pane's lower-left swatch subscribes to `active_material_changed` —
one logical active material shared across panes on the same level (matches "two panes can show
the same level" from PLAN.md §DOCUMENT STATE; there is exactly one texturing tool state per editor
session, consistent with Scythe/Hammer having one Active Material panel, not one per viewport).
`MaterialBrowserDock` click handler calls `LevelEditor::set_active_material`; it does not hold the
state itself, so a second dock instance (should the workspace ever allow detaching it into a
second window) trivially stays in sync.

### 1.4 Blockout materials: built-in vs per-project

See Difficulty 4 for the full design; summary: built-ins ship compiled into the editor via an
embedded resource pack mounted at editor startup under a reserved virtual path prefix
(`res://__engine_builtin__/level_editor/blockout/`), invisible to `FileSystemDock`; a project may
supply overrides under a configurable folder (OMH: a new
`resources/world/architecture/blockout/` alongside the existing `palettes/`) that fill quick-slots
1..N before built-ins fill the remainder.

---

## 2. Foundation services required

| Service | File | Responsibility |
|---|---|---|
| `MaterialIndex` | `editor/level/material_index.{h,cpp}` | Project material discovery + incremental maintenance (§1.1). |
| `MaterialBrowserDock` | `editor/level/material_browser_dock.{h,cpp}` | UI: virtualized grid, folder/name/M_\* filter, favorites/recent, hotspot badge, texel-density readout, active-material swatch source. |
| `MaterialBrowserPreviewQueue` + flat/cube generator classes | `editor/level/material_preview_generator.{h,cpp}` | Difficulty 1 — private thumbnail pipeline, distinct from the shared `EditorResourcePreview` sphere path. |
| `TexelDensityScanner` | `editor/level/texel_density_scanner.{h,cpp}` | Difficulty 3 — resolve a Material to texture dims + world-units-per-texel. |
| `HotspotBadgeIndex` | folded into `HotspotAtlas`'s registry (PLAN.md §3) | Reverse-index `target_materials` → path set for O(1) badge lookup; rebuilds on any `HotspotAtlas` resource change. |
| `BlockoutMaterialRegistry` | `editor/level/blockout_material_registry.{h,cpp}` | Difficulty 4 — resolves engine built-ins + project overrides into ordered quick-slots. |
| `LevelEditor` additions | `editor/level/level_editor.{h,cpp}` | Owns active-material state (§1.3), owns the dock instance, wires `MaterialIndex`/`BlockoutMaterialRegistry` lifetimes to editor startup/shutdown (project-global, not document-scoped). |
| Editor settings additions | `editor/settings/editor_settings.cpp` | `level_editor/material_browser/name_filter_prefix` (default `M_`), `.../texel_density_param_names` (String array, Scythe-style configurable ShaderMaterial scan list), `.../project_target_texel_density`, `.../blockout_override_folder`. |

---

## 3. Core difficulties

### Difficulty 1 — Flat/cube preview generator for trims

**Problem**: sphere previews (the only shape `EditorMaterialPreviewPlugin` produces) badly distort
tiling trims/atlases — exactly the material class this tool exists to make legible (§0 finding 2
also means we *can't* just add a second `add_preview_generator()` call; the shared cache is
one-slot-per-path and `_generate_preview` stops at the first non-null result, so a second
Material-handling generator registered after `EditorMaterialPreviewPlugin` would never even run,
and one registered *before* it would silently steal every Material preview project-wide, including
the inspector's).

**Chosen solution**: build the flat/cube renderers as new `EditorResourcePreviewGenerator`
subclasses in `editor/inspector/editor_preview_plugins.{h,cpp}` (reuse the file — same RID-rig
pattern as `EditorMaterialPreviewPlugin`/`EditorMeshPreviewPlugin`: dedicated `scenario`,
`viewport` at `VIEWPORT_UPDATE_DISABLED`, orthographic camera for flat / perspective for cube,
`DrawRequester::request_and_wait`), but **never call
`EditorResourcePreview::add_preview_generator()` on them**. Instead `MaterialBrowserDock`
instantiates them privately and drives them through its own `MaterialBrowserPreviewQueue`:
own background thread (or reuse `WorkerThreadPool` — no need for a bespoke `Thread`), own
in-memory LRU keyed by `(path, shape)`, own disk cache file
`matbrowser-<shape>-<md5(globalized_path)>.png` (disjoint filename prefix from `resthumb-*` so the
two caches never collide), own mtime/`.import`-mtime invalidation copied from
`EditorResourcePreview::check_for_invalidation`'s logic (small, ~15-line function; duplicating it
is cheaper and safer than exposing it generically).

**Algorithm sketch**:
1. `EditorMaterialFlatPreviewPlugin`: unit quad mesh, orthographic camera framed to the quad's
   bounds, quad aspect ratio = source albedo texture's aspect ratio (from `TexelDensityScanner`,
   §Difficulty 3 — same texture lookup, reused) so non-square trims don't get squashed to a
   thumbnail square; UV set so the material's native tiling scale is visible at 1:1, not stretched
   to fill the quad.
2. `EditorMaterialCubePreviewPlugin`: reuses `EditorMeshPreviewPlugin`'s box-mesh + two-light rig
   verbatim (it already exists for generic mesh previews) with the target material assigned to all
   six faces — shows tiling continuity across an edge, which sphere previews cannot.
3. Shape selection: per-material user toggle persisted in `MaterialIndexEntry.preview_shape`
   (stored in the project-metadata side-file, §Difficulty 4's persistence mechanism), defaulting
   via a heuristic at first index: `HotspotBadgeIndex` says "hotspot-enabled" → flat; source
   texture aspect ratio outside `[0.9, 1.11]` → flat; else sphere (delegates to the existing shared
   `EditorResourcePreview` path — most materials keep the familiar sphere for free).
4. Cache invalidation on texture swap: the scanner's `(path, shape)` cache entry is invalidated by
   the same `MaterialIndex.material_changed(path)` signal that already fires from
   `resources_reimported`/`resources_reload` (§1.1) — one invalidation trigger feeds both the
   thumbnail cache and the texel-density cache, so a texture reimport can't leave one stale while
   the other updates.

**References**: `editor/inspector/editor_preview_plugins.cpp:338-360` (sphere generate, to diverge
from) and `:121-143`/`EditorMeshPreviewPlugin` (box-mesh rig to reuse for cube);
`editor/inspector/editor_resource_preview.cpp:202-253` (`_generate_preview`'s first-match-wins loop
— the reason a second global registration doesn't work); Scythe Editor's Active Material panel
docs describe an explicit sphere/flat toggle per material, confirming this isn't a novel UX ask.

### Difficulty 2 — Scalable browsing (hundreds of materials)

**Problem**: OMH already has dozens of materials across `psx_textures_ii` and
`modular_architecture`; a production project's atlas-heavy palette will have hundreds. A naive
dock that builds one `TextureRect` + `queue_resource_preview` call per row on open will hitch on
open and burn thumbnail-thread time on off-screen rows.

**Chosen solution**: virtualized grid (only instantiate Control nodes for visible rows + a small
overscan margin) backed directly by `MaterialIndex`'s flat entry list (already O(project files),
built once), with windowed, scroll-driven thumbnail enqueue.

**Algorithm sketch**:
1. `MaterialIndex` exposes a stable, filtered `Vector<MaterialIndexEntry*>` (filter = folder +
   name substring + `M_*`-only toggle + favorites-only toggle), recomputed only when the filter
   changes or `material_added`/`material_removed` fires (not per-frame).
2. Grid layout is pure math: `row = index / columns`, `rect = Rect2(col*cell, row*cell, cell,
   cell)` — no per-item Control exists until it scrolls into `[visible_range.first - overscan,
   visible_range.last + overscan]`. On scroll, a `_update_visible_range()` diffs old vs new visible
   index range: entries leaving the range free their Control (or return it to a small pool of
   reusable `TextureRect`+label Controls, sized to `columns * (visible_rows + 2*overscan)` — this
   pool size, not the material count, bounds node count regardless of project size); entries
   entering the range grab a pooled Control and call `queue_resource_preview`/enqueue on the flat
   queue *at that moment*, not at dock-open time.
3. This directly gives the priority behavior from §1.2: nothing is queued until it's about to be
   visible, so opening the dock on a filtered subset never queues the other 300 materials.
4. `EditorFileSystem` rescan events (`filesystem_changed`) must not cause a full grid rebuild while
   scrolling: `MaterialIndex`'s diff-based incremental update (§1.1) means the filtered vector is
   patched (insert/remove single entries, resort if the sort key is folder+name) rather than
   regenerated; the grid recomputes total content height (cheap) and only rebuilds the Controls
   whose backing index actually shifted, not the full visible window.

**References**: this is the standard virtualized-list technique used by every large asset-browser
(Unreal Content Browser, Unity Project window, Blender's Asset Browser flat-grid mode all use
windowed instantiation over a flat backing array rather than per-item persistent widgets); Godot's
own `ItemList` does *not* virtualize (it keeps one internal row struct per item but that's cheap —
the expensive part here is the `TextureRect`/thumbnail request per item, which is what must be
windowed, not the index itself).

### Difficulty 3 — Texel-density detection robustness

**Problem**: `StandardMaterial3D` has a fixed `albedo_texture` property, but `ShaderMaterial`
requires scanning shader params by name (Scythe's own convention-based approach), and both cases
must degrade gracefully when there's no texture, an NPOT texture, or an import variant (e.g. a
texture resized by the importer's `Detect 3D`/max-size setting) whose on-disk dimensions don't
match its *imported* dimensions.

**Chosen solution**: a small pure-function scanner, `TexelDensityScanner::scan(Ref<Material>) ->
Optional<TexelDensityResult>`, with a project-configurable parameter-name list (editor setting,
§2) rather than a hardcoded one — directly Scythe's own documented convention-naming lesson.

**Algorithm sketch**:
1. If `material is StandardMaterial3D` (or `ORMMaterial3D`/`BaseMaterial3D` family): read
   `albedo_texture` directly via the typed getter. No name-scanning needed — this is the common
   case and should be O(1) with no string matching.
2. Else (`ShaderMaterial`): iterate `get_shader()->get_shader_uniform_list()`, filter to
   `UNIFORM_HINT_*` sampler types, match uniform name (case-insensitive) against the configurable
   list (default: `albedo`, `albedo_texture`, `basecolor`, `base_color`, `diffuse`,
   `diffuse_texture`, `color_texture`) in priority order; first match wins. This list lives in
   `EditorSettings` (`level_editor/material_browser/texel_density_param_names`), editable without
   an engine rebuild — new shader authoring conventions don't require a C++ patch.
3. Resolve dimensions from the **imported** texture, not the source file: call
   `texture->get_width()/get_height()` on the loaded `Texture2D` (this already reflects import
   settings — max size clamp, VRAM compression block rounding, etc. — because it's reading the
   post-import `CompressedTexture2D`, not re-parsing the PNG). This sidesteps the "import variant"
   half of the robustness ask entirely: there is exactly one dimension source (the loaded
   resource), never a raw file stat.
4. NPOT textures: no special-casing needed for the *reading* side — density math is
   `world_units_per_texel = face_world_size / texture_dimension_in_that_axis`, which works for any
   integer dimension. The only NPOT-specific concern is the hotspot fitter's mip-safe inset
   (PLAN.md §3, not this tool's problem) — flagged here only so the two specs don't duplicate
   NPOT-handling logic.
5. No texture found (pure-color `ShaderMaterial`, or an unrecognized uniform name): return
   `Optional::none()`. The browser renders "—" for texel density on that row rather than a
   fabricated number, and logs the material path once per session to an editor-settings-gated
   verbose channel (so an artist hitting this on a custom shader has a debuggable trail without
   spamming the console for the common no-texture case, e.g. a flat-color debug material).
6. **Density-mismatch badge**: compare `world_units_per_texel` against
   `level_editor/material_browser/project_target_texel_density` (a single project-wide target,
   editable per-project — OMH's PSX kit has one deliberate density); badge renders if the ratio
   exceeds a configurable tolerance (default ±15%, matching common trim/hero-prop density
   deliberate-mismatch margins so intentionally-denser hero materials don't nag).

**References**: Scythe Editor's documented naming-convention texel-density reading (its own
`.rect`/hotspot pipeline scans shader param names, not a fixed property) is the direct precedent
for step 2's configurability; Godot's `BaseMaterial3D`/`ShaderMaterial` API
(`shader->get_shader_uniform_list()`) is the concrete engine surface step 2 walks — no vendored
reflection needed, it's already exposed for the inspector's own shader-param UI.

### Difficulty 4 — Blockout materials: engine built-in vs project asset

**Problem**: the tool must work in a brand-new, empty Godot project (so it's a generically useful
editor feature, not an OMH-only fork patch) yet OMH wants its own house-style blockout set
(e.g., matching the existing `M_*` PSX/architecture palette look) to override the generic grey/
orange checkers.

**Chosen solution**: built-ins ship as an editor-bundled resource pack (`.tres` materials +
checker/grid textures) mounted at editor startup under a reserved virtual path prefix,
`res://__engine_builtin__/level_editor/blockout/`, via the same `PackedData`/pack-mounting
mechanism Godot already uses for self-contained exports — just invoked once more, pointed at a
pack shipped alongside the editor binary instead of the game's own export PCK. A project may
additionally point `level_editor/material_browser/blockout_override_folder` (editor setting) at a
real project folder (OMH: a new `resources/world/architecture/blockout/`, sibling to the existing
`palettes/`); `BlockoutMaterialRegistry` resolves the 10 quick-slots (`Shift+Alt+1..0`, per
PLAN.md §3/§UX contract — note this supersedes the 6-slot count in earlier task framing; PLAN.md's
explicit `1..0` is the source of truth) by filling from the override folder first (materials found
there, sorted by filename, claim slots in order — or by an explicit `blockout_slot` int in a
resource metadata field if the project wants stable ordering independent of filename), then
padding any remaining slots with built-ins.

**Algorithm sketch**:
1. At editor startup (`EditorNode` init, alongside the other one-time editor setup), mount the
   built-in pack: `PackedData::get_singleton()->add_pack(exe_dir.path_join("level_editor_blockout.pck"), …)`.
   The pack's internal paths were baked (build-time, via the existing `PCKPacker` tooling already
   used for export templates) as `res://__engine_builtin__/level_editor/blockout/blockout_01.tres`
   etc. — the double-underscore prefix is a reserved namespace: `FileSystemDock` filters it out of
   the project tree exactly like it already hides `.godot`, so it never appears as a "phantom
   project file" a user could accidentally edit or delete.
2. `BlockoutMaterialRegistry::resolve_slots()`: `Vector<Ref<Material>> slots(10)`; if
   `blockout_override_folder` is set and non-empty, list `Material`-typed files there (reusing
   `MaterialIndex`'s classification, not a second file-type scan), fill slots `[0, override_count)`;
   fill `[override_count, 10)` from the built-in pack's fixed 10-entry manifest (a small
   `.tres` array resource inside the pack itself, so the built-in set's *order* is also
   data-driven, not hardcoded C++ enum values — a future built-in-set update doesn't need a
   recompile of `BlockoutMaterialRegistry` itself).
3. `Shift+Alt+<N>` (tool-mode input handling in `LevelEditorView`, per PLAN.md's shortcut grammar)
   calls `LevelEditor::set_active_material(slots[N])` — same entry point as a browser click
   (§1.3), so quick-slots are just a fast path into the one active-material state, never a
   parallel state machine.
4. OMH populates its override folder once, at content-authoring time, by pointing 3-6 of its
   existing `M_*` architecture materials there (e.g. `M_DarkMetalPanel`,
   `M_RoadsideConcrete` — both already used across multiple palettes in
   `default_palette_catalog.tres`, so they're already the project's de facto neutral/structural
   go-tos) — no new asset production required for the first cut.

**References**: `core/io/file_access_pack.h`/`PackedData` (the export-PCK mounting mechanism,
repurposed here for editor-bundled-not-project content) is exactly how Godot already ships
self-contained export templates, so this isn't a new engine capability, only a new call site;
Source 2 Hammer's asset browser ships built-in "dev textures" the same way (bundled with the
tool, not the mapper's content, but distinguishable/overridable per-game); Scythe Editor's
blockout quick-slots (`Shift+Alt+1..6` in its docs) are the direct UX precedent for the shortcut
grammar, though this fork's slot count follows PLAN.md's `1..0` (10 slots) instead.

---

## 4. Test ideas

- **Headless kernel-adjacent checks** (`one-more-house/tools/checks/material_browser_check.gd`,
  driving `MaterialIndex`/`TexelDensityScanner`/`BlockoutMaterialRegistry` directly — these three
  have no rendering dependency and should be scriptable headlessly per PLAN.md's "fully scriptable"
  kernel rule, even though the dock/preview generators are editor-UI-only):
  - Index a fixture folder of `.tres` materials (mix of `StandardMaterial3D`/`ShaderMaterial`,
    `M_*`-prefixed and not) → assert filtered counts match expectations for each filter
    combination.
  - Rename two materials to the same `resource_name` (distinct paths) → assert
    `MaterialIndex`/favorites/recent never collapse them (the Scythe v0.8 regression, directly
    fuzzed).
  - `TexelDensityScanner` against: a `StandardMaterial3D` with albedo set, one with no albedo, a
    `ShaderMaterial` whose uniform matches the configured list, one that doesn't, and a NPOT
    texture — assert correct dims or `none()` for each, never a crash.
  - `BlockoutMaterialRegistry::resolve_slots()` with 0, 3, and 12 override-folder materials →
    assert correct fill/pad/truncate behavior (12 case: only first 10 by sort order are used,
    doesn't overflow the fixed slot vector).
- **Interactive smoke scenario** (per PLAN.md's phase-landing requirement, editor-side, not
  headless — thumbnail rendering and virtualization hitching are exactly the class of bug headless
  checks miss): open the dock against OMH's real `assets/environment/psx_textures_ii/materials/`
  folder (already dozens of real `M_*` materials), scroll the full list rapidly, confirm no
  dropped-frame stall and no duplicate/leaked thumbnail requests in the console (verbose preview
  logging); edit and re-save a material's albedo texture externally, confirm the thumbnail AND
  texel-density badge both update within one `resources_reimported` cycle, not just one of them
  (regression guard for §Difficulty 1's shared-invalidation design point).
- **Cache-collision regression test**: request a sphere preview and a flat preview for the same
  material path in the same session; assert both textures are non-identical and both survive an
  editor restart (disk-cache filenames genuinely disjoint, not just in-memory).
- **Built-in-pack isolation test**: open a brand-new, empty Godot project (not OMH) with this
  editor build; confirm the 10 blockout slots populate from the built-in pack with zero project
  configuration, and that `__engine_builtin__` never appears in `FileSystemDock`'s tree.
