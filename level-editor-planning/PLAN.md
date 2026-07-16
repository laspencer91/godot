# G-Level: a Hammer/Scythe-class level editor as a first-class workspace tab

**Status:** planning. **Prereq reading:** `workspace-editor-planning/ARCHITECTURE.md` (three-layer
taxonomy — this plan sorts every piece of the level editor into those layers).

**Companion docs:** `TOOL-FOUNDATIONS.md` — the eight shared foundation services, the binding
UV storage contract, and the per-tool pre-decision table (synthesis; tiebreaker over tool
plans). `tools/01-*.md … tools/11-*.md` — one deep plan per tool: ≥3 core difficulties each,
one chosen method per difficulty, algorithm sketches, and open-source/article references.
**Implementation agents work from: this file → TOOL-FOUNDATIONS.md → their tool's plan.**

**One sentence:** bring Source 2 Hammer's level-design workflow (block-out → mesh-edit → fast
face texturing → hotspot auto-UV) into the fork as a `LevelDocument` workspace tab, backed by a
new `modules/level_kernel` C++ geometry module, with One More House's modular kit and PSX
texture library as the first production content.

Research base (July 2026): Scythe Editor docs catalog (scytheeditor.com/guide), Scythe dev
history v0.0.7→v0.10.1 via Patreon/press, Source 2 Hammer docs + community lessons, TrenchBroom
manual/issues, UE5 Modeling Mode critiques, Cyclops Level Builder source assessment (MIT,
cloned + reviewed), BMesh/DMesh3 kernel literature, hotspot-UV implementations (Hammer .rect,
DreamUV, Zen UV, rmKit, Scythe).

---

## 0. Decisions already made (with rationale)

| # | Decision | Rationale |
|---|----------|-----------|
| D1 | **Engine-side, not an addon.** | In this fork a first-class tab = an `EditorDocument` subclass. GDScript main-screen plugins get parked inside the `ScreenHostDocument` "Editor" tab — second-class by construction. Script-level editor plugins are also the ecosystem's proven fragility point (Cyclops broke on 4.5/4.6/4.7 three different ways). |
| D2 | **Do not adopt Cyclops as the base.** | Its edit kernel is convex-hull-from-planes (Quake brush model): no topology-adding ops possible, one `ConvexPolygonShape3D` per block, 30k of its 37k LOC is GDScript welded to `EditorPlugin`. Mine it for: `MeshVectorData`'s columnar attribute schema, quick-hull/clip math reference, its UV-editor UX, pluggable snapping design. MIT license permits porting. |
| D3 | **Indexed mesh kernel (DMesh3-style), not pointer half-edge.** | Epic rejected pointer half-edge for FDynamicMesh3 (debuggability); BMesh itself uses disk/radial cycles, not strict half-edge. Indexed + free-lists is compact, serializable, snapshot-friendly for undo, and SoA-consistent with how this fork already writes native systems (horde_sim). |
| D4 | **Booleans via vendored Manifold, applied per-op — never live CSG.** | Godot's live CSG re-evaluates continuously and is documented prototyping-only with open coplanar-face bugs. Scythe's clip/mirror (shipped v0.6, a year+ in) was its crash hotspot. Booleans are one-shot kernel ops with reject-illegal-topology guards. |
| D5 | **Level geometry lives in the scene file, baked to shared meshes on save.** | UE5 Modeling Mode's most-cited complaint is asset-database pollution (every shape becomes a content-browser asset). Block topology serializes inside the `.tscn`/`.res`; runtime sees plain `ArrayMesh` + trimesh collision. |
| D6 | **Texel-density-first hotspot fitter with aspect fallback (Scythe's model), .rect-compatible patch catalog.** | Matches the reference implementations' consensus pipeline; `.rect` interchange lets Blender's Mallet addon and Source 2 tooling author patch layouts. |
| D7 | **Standard output.** | The strongest independent praise Scythe got ("just a mesh editor, nothing unusual about its output"). Bake path must produce plain ArrayMesh/GLB — usable even outside this project. |

---

## 1. Architecture — sorted by the workspace taxonomy

### SERVICE (one per editor): `LevelEditor` singleton
`editor/level/level_editor.{h,cpp}`. Owns project-shared snap settings, `ED_SHORTCUT`
registrations, hotspot/material registries, the material index/scanner, and one shared thumbnail
queue/cache. It mints document-bound Level and material-browser views for `DocumentView`, exactly
like `ShaderEditorPlugin::create_editor_view` (GS2 pattern). Tool-mode commands resolve the active
bound view; they are not broadcast to every Level document. Holds **no render state** (seam rule 1).

### VIEW STATE (one per pane): `LevelEditorView`
`editor/level/level_editor_view.{h,cpp}`. The editor surface a `DocumentView` mints: viewport
bound to the document's `World3D`, camera (orbit-around-selection, WASD flythrough, no roll,
+Z up), grid/origin decoration RIDs (create-detached → reconcile → free-in-dtor lifecycle,
copied from `Node3DEditorView`), selection-highlight overlay instances (RenderingServer, on the
document's scenario, per-world gizmo cull layer via the existing `allocate/free_gizmo_layer`
per-scenario map), viewport input routing to the active tool, per-pane toolbar mounted into
`DocumentView::toolbar_host`, compact tool rail, active-material swatch, and the fixed-width,
selection-aware texture/tool options panel.

### DOCUMENT STATE: `LevelDocument : SceneDocument`
`editor/editor_document.h` — new type `TYPE_LEVEL` (append-only enum). The edited thing IS a
scene (e.g. `greybox_house.tscn`); opening "in Level Editor" (FileSystem context action +
toolbar toggle on an open scene tab) creates a `LevelDocument` instead of a plain
`SceneDocument`. Inherits: isolated world, per-document `EditorSelection`, per-document undo
history id, tab persistence. Adds: level-editing session state (active block set, kernel
transaction journal head), including active material/binding, captured Lift mapping, and hotspot
mapping override. Drawer/filter/zoom/scroll and context-panel presentation live in the document's
`contextual_editor_states`. Two panes can show the same level; a normal 3D scene tab can sit
beside it — something UE/Scythe cannot do.

### KERNEL (no editor dependencies): `modules/level_kernel`
Sibling to `box3d_physics` / `horde_sim`.

- `LevelMeshData` (`Resource`) — serialized topology, columnar (Cyclops `MeshVectorData`-informed):
  vertex positions; edge tuples; per-face: vertex-count/offsets, `material_index`,
  `uv_mode : {PROJECTED, EXPLICIT}`, `uv_origin : Vector3` + `uv_tangent : Vector3` (frozen
  frame; bitangent re-derived live), `uv_transform : Transform2D` (valid when PROJECTED),
  polygroup id, smoothing, texture-lock flag, hotspot patch ref; per-face-vertex (loop): uv
  (ALWAYS materialized — the baker reads only loop UVs), color, normal. Free-list ids so undo
  diffs are stable. Full UV storage contract: `TOOL-FOUNDATIONS.md` §2. Every topology/position
  op ends with the shared `reconcile_face_uv` post-step (texture lock's single owner).
- `LevelMesh` (`RefCounted`) — the live edit structure + operator API, two tiers (BMesh lesson):
  atomic Euler-style ops → composed operators (extrude, push/pull, inset/outset, bevel
  [grid-quantized], connect/loop-insert, bridge, merge/weld, detach/extract, mirror, clip,
  subtract/intersect via Manifold, normals soft/hard/dominant). Every operator validates
  preconditions and **rejects** illegal topology (boundary flips, degenerate collapses) rather
  than attempting it.
- `LevelMeshBaker` — ArrayMesh (per-material surfaces) + trimesh collision regen for dirty
  blocks only; wireframe/handle mesh generation for overlays; save-time dedup of identical
  block bakes; xatlas UV2 on demand.
- `HotspotAtlas` (`Resource`) + `HotspotFitter` — see §3.
- `LevelBlock` (`Node3D`, registered in the module, works at runtime) — owns a `LevelMeshData`,
  builds its `MeshInstance3D` + `StaticBody3D`+trimesh child on load. Editor-only journal
  attaches via metadata, never ships.
- **Undo**: kernel ops produce data-diffs (changed spans of the columnar arrays), applied/reverted
  through the document's undo history. Selection changes are NEVER undo steps (TrenchBroom
  lesson). Derived state (bake, overlays, selection caches) invalidates transactionally with
  the diff (Cyclops crash-class lesson). Kernel itself stays undo-agnostic (DMesh3 lesson) —
  it emits diffs; the editor owns the stack.
- Fully scriptable: classes exposed to GDScript/traits so headless checks in the game repo can
  drive every operator without the editor.
- **LE2 UV schema delta:** projected/explicit face recipes and always-materialized loop `uv0`
  serialize and undo together; the baker remains loop-UV-only.

### Picking
Face/edge/vertex picking against `LevelMesh` via a per-block BVH (reuse the fork's
per-world gizmo BVH infrastructure and scoping fix `630785ab`), not physics queries.
Polygroup expansion happens at selection level, not pick level.

---

## 2. UX contract (from the Scythe catalog + convergent editor rules)

- **Modifier grammar** (adopt wholesale): `Shift` = create/apply/extrude family; `Alt` =
  sample/transfer/flip family; `Shift+Ctrl` / `Ctrl+Alt` = specialized variants. Universal
  `Enter`/`Escape` accept/cancel for every modal tool. MMB-drag = temp pivot, auto-reset on
  selection change.
- **Selection**: `1/2/3` verts/edges/faces (polygroup tier), `4` objects, `6` toggles
  polygroup↔triangle tier; double-click flood fill; `L`/`X` loop/ring; drag = path select;
  plane-select modifier.
- **Snapping**: ON by default, power-of-two steps + the project's 4 m kit grid as a preset,
  one-key toggle, `[`/`]` grid resize, **never** state-dependent (UE5's Local-gizmo snapping
  bug is the counter-example). Vertices-to-grid command.
- **Camera/viewport**: single 3D viewport is primary (TrenchBroom lesson); separate
  camera-speed for flythrough vs orbit (UE5 counter-lesson).
- **Texture lock ON by default** — UVs survive geometry transforms.
- **One-stop shop**: block tools, kit placement, entity/prop placement, and light placement all
  reachable from the tab; leaving the tab for the 3D editor should be rare, not constant.
- **Toolbar layout**: left vertical toolbar = compact tool-mode icons plus the active-material
  swatch (Scythe-style); top strip (per-pane `toolbar_host`) = selection tier, grid size, texel
  density, snap toggle. A user-collapsible contextual panel between the rail and viewport shows
  block, selection, UV, or modal-session controls without resizing itself on selection changes.
  The material browser is a document-owned bottom drawer.
- **Texture-alignment UI gets first-class design attention** — it's the one place Scythe's UX
  demonstrably failed (a user shipped a replacement panel). Numpad nudge/justify/fit commands are
  organized into semantic groups in the selection-aware contextual panel, not mixed into the
  material gallery and not shown when the current selection cannot use them.

## 3. Hotspot system (headline feature — the differentiator)

- **`HotspotAtlas` resource** (`.tres`, kernel-registered): texture ref + texel-density target +
  patch list. Per patch: rect (px, top-left origin), allow-rotation, allow-mirror-x/y,
  allow-tiling ("infinite trim"), inset px. Importer/exporter for Source 2 `.rect` (Mallet
  interchange). One atlas serves many materials (Scythe's "one data asset per pattern" rule):
  target-materials list lives on the atlas.
- **Fitter** (pure kernel code, headless-testable):
  1. partition selection into islands (coplanar/collinear face groups for `Hotspot` grouped
     apply; single faces for `Hotspot Individual`),
  2. world-space OBB per island,
  3. score patches by texel-density error, then aspect, within a margin; random tie-break
     among equals (anti-repetition, Hammer behavior) with a `Disallow Random` deterministic
     toggle (also what the headless check uses — seeded),
  4. respect per-patch flags; tiling three-way setting (No / Allow / Only),
  5. write the winning rect into face `uv_transform`s with mip-aware inset
     (padding scales with mip chain).
  Mapping modes: Automatic (Square → Conforming on distortion), Square, Conforming,
  Follow Active Quads (never auto-selected).
- **Hotspot Editor** — patch annotation over the texture (draw rects on a grid, set flags).
  Implemented as a `ResourceDocument`-style workspace tab for the atlas resource: the
  workspace gives this for free and it dogfoods the document model.
- **Material browser drawer**: document-bound, horizontally virtualized flat thumbnails backed by
  one shared async preview queue/cache; source/search/`M_*` filters, 80–160 px zoom (108 default),
  release-to-select, resource drag, and blockout-material quick slots (`Shift+Alt+1..0`). It is
  hosted below only the Level surface, so the right accordion remains full height.
- **Texel density source**: read base-color texture dims from the material; recognized
  parameter names configurable in editor settings (Scythe's naming-convention lesson), default
  covering `albedo_texture`/`BaseColor`/etc. Never key caches by material *name*
  (Scythe's v0.8 same-name collision bug) — key by RID/path.
- **Content**: first production atlases come from the PSX modular kit's 19 shared textures
  (`tools/assets/convert_psx_modular_architecture.py` already rebuilds them deterministically);
  a Hotspot-Texture-Maker equivalent (compositing wear masks over tiling textures) is optional
  later work, game-side.

## 4. Phases

Build order follows Scythe's revealed lessons: geometry+selection first, texturing EARLY (not
after modeling depth), booleans LAST and hardened. Every phase lands with (a) headless kernel
checks in `one-more-house/tools/checks/`, (b) an interactive smoke scenario (headless misses
editor bugs — HammerForge lesson; use the workspace smoke-test-project pattern), (c) a
DIVERGENCE-LEDGER entry for any upstream-file touch, (d) a `DECISIONS.md` note game-side.

- **LE0 — Tab + kernel skeleton (the thin slice).**
  `TYPE_LEVEL` document + view factory + empty toolbar; camera + grid; `modules/level_kernel`
  with `LevelMeshData`/`LevelMesh`/baker for box topology only; block drag-create
  (`Shift+B`, ground-plane + surface-snap placement), grid snap, undo, save/reload,
  plays in-game with collision. *Proves: document seam, kernel seam, undo seam, bake seam.*
- **LE1 — Selection + core edit.** All selection modes/tiers, BVH picking, flood fill,
  loop/ring, path select; move verts/edges/faces/objects; face extrude (`Shift`-drag) +
  push/pull (`Shift+Ctrl`); boundary-edge extrude; nudge; vertices-to-grid; duplicate.
- **LE2 — Fast texturing.** Document-owned active material + horizontal material drawer;
  apply (`Shift+T`), lift (`Shift+RMB`), wrap (`Alt+RMB`), wrap-to-selection, flow;
  align-to-grid/face; texture lock; contextual texture controls (numpad grammar: shift/scale/
  rotate/fit/justify); Fast Texture overlay (`Shift+Q`) with unwrap modes (Use Existing / Conforming /
  Square / Follow Quads / Planar); blockout material slots.
- **LE3 — Hotspots.** Atlas resource + `.rect` I/O + Hotspot Editor tab + fitter + grouped
  (`Shift+H`) / individual (`Shift+F`) apply + PSX kit atlases. **End of slice-critical scope.**
- **LE4 — Modeling depth.** Bevel (grid-quantized), inset/outset, connect/loop-insert,
  bridge, merge faces, weld-overlapping, detach/extract, subdivide, normals control, stitch.
- **LE5 — Booleans.** Clip (`Shift+X`), mirror (`Shift+Z`), subtract/intersect via Manifold.
  Fuzzed headless checks across every selection-mode × op combination (Scythe's v0.6 crash
  class). Reject-don't-crash on illegal input. **Entry criteria — two spikes from
  `tools/07-booleans.md`:** (a) confirm `faceID` survives `BatchBoolean` in the vendored
  Manifold; (b) determine which operand Manifold attributes new cut faces to.
- **LE6 — Level-design multipliers.** Kit-placement palette (drives the existing
  `ArchitecturePalette`/`Catalog` system — Godot scene instancing already gives Hammer-style
  live-propagating instances for free; do NOT build a custom linked-group system, it's the
  known undo-correctness tarpit); arch/stairs/spline generators; physics-settle prop scatter
  (Box3D, convex-proxy props only, mesh geometry excluded — Hammer's scope rule); VisGroups
  (auto + user).
- **LE7 — Bake + pipeline.** Merge/dedup static geometry, UV2 lightmap unwrap (xatlas),
  occluder + navmesh generation, GLB export (D7), game-side check integration
  (`level_editor_check.gd`, `hotspot_fitter_check.gd` seeded-deterministic).

Suggested first milestone gate (Logan V-G): LE0 demo — create a room from blocks in the tab,
save, `launch_two.ps1`, walk it co-op.

### Testbed project (decided 2026-07-13)

Development and smoke testing happen in a dedicated standalone project at
`level-editor-planning/testbed/` (same pattern as `workspace-editor-planning/smoke/` with its
per-flow `*_project.godot` files and `run_smoke.sh`) — NOT inside One More House, so the game
repo stays clean until phases actually land. Seed it with a copied subset of OMH content:
a handful of the PSX modular-kit textures + `M_*` materials (carry `SOURCE.md` provenance
along per ASSET_SOURCING rules), one greybox-style scene, and the 4 m grid project settings.
The game repo only gains `tools/checks/` scripts once a phase is integration-ready.

## 5. Risks & mitigations

1. **Boolean robustness** — the documented graveyard. Manifold + precondition rejection +
   fuzz checks; scope clip/mirror to faces/polygroups/objects first (Scythe's v0.6 restriction,
   made explicit instead of a crash).
2. **Derived-state desync** (stale UVs/gizmos/selection after topology edits — Cyclops crash
   class). Transactional invalidation keyed to kernel diffs; a headless check asserts
   bake/selection cache consistency after randomized op sequences.
3. **Scope vs the 3-month slice.** LE0–LE3 is the slice-valuable core (greybox + texture the
   house better/faster); LE4–LE7 is post-slice. Scythe took ~1 year to its v0.6 booleans with
   one developer — respect that data point.
4. **Upstream merge tax.** Keep everything in `editor/level/` + `modules/level_kernel/`;
   touches to shared files (document enum, `DocumentView` factory switch, FileSystem context
   menu) stay minimal and ledger-logged. The 1758-commit upstream merge precedent says seams
   hold if they're thin.
5. **Undo correctness under multi-pane.** Per-document history already exists; kernel diffs
   must be replayable against a document regardless of which pane issued them. Covered by LE0
   exit criteria (two panes, one level, interleaved undo).

## 6. Explicit non-goals (v1)

- Full DCC parity (sculpt, subdiv surfaces, n-gon-heavy organic modeling) — "you don't need
  Blender" ≠ "we are Blender".
- Custom linked-group/prefab system (use scene instancing).
- Live CSG preview of pending booleans (cheap wireframe preview only).
- Supporting the legacy CSG nodes or func_godot-style brush import inside this editor
  (separate importers can bake INTO `LevelMeshData` later).
- Terrain (Terrain3D already owns that).
