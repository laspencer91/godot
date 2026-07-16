# WP14 — LE2 material browser dock, active material, blockout quick-slots

<task>
Build the level editor's material browser and active-material state in this Godot 4.8 fork
(repo root = this workspace). Prereq reading IN ORDER: level-editor-planning/PLAN.md §1
(LevelEditor service ownership) + §2 (UX contract) + §4 LE2,
level-editor-planning/TOOL-FOUNDATIONS.md §4 (material browser row of the pre-decision table),
then level-editor-planning/tools/11-material-browser.md IN FULL (the detailed plan — §0's
load-bearing findings about EditorResourcePreview's first-match-wins generator loop and
path-keyed single-slot cache are binding constraints), then the existing code:
editor/level/level_editor.{h,cpp}, level_editor_view.*, editor/inspector/
editor_preview_plugins.{h,cpp} + editor_resource_preview.{h,cpp} (READ ONLY — reference for the
RID preview rig), editor/file_system/editor_file_system.h (signals),
editor/docks/filesystem_dock.* (how docks register; the fork already has a level-editor seam
here).

DESIGN AMENDMENTS from studying Scythe's shipped browser (these OVERRIDE tools/11 where they
conflict):
- FLAT TILED THUMBNAILS ARE THE DEFAULT for every material — Scythe's pitch is literally "no
  more squinting at sphere previews." Drop tools/11's sphere-default heuristic entirely: the
  browser's own flat-quad preview queue renders every tile; the shared
  EditorResourcePreview sphere path is NOT used by this dock at all (which also sidesteps §0's
  shared-cache hazard). A per-material cube option (tiling continuity across an edge) may ship
  later; do not build it this WP.
- Dense edge-to-edge grid: uniform square cells, minimal padding, thin dark separators, no
  per-tile buttons. Sorted by display name so texture families (M_BrickClassic3b/3d/3e...)
  cluster as visual runs.
- Per-tile INFO OVERLAY in the top-left corner, small text over the texture: line 1 = display
  name (M_ prefix stripped, per tools/11), line 2 = folder path, line 3 = material class +
  albedo texture dimensions (e.g. "StandardMaterial3D | 256x256", dims from the
  TexelDensityScanner below; "—" when no texture). Overlay text needs a subtle shadow/scrim so
  it reads on bright textures.
- Clicking a tile sets it as the ACTIVE MATERIAL, shown in a persistent swatch in the
  lower-left of every level pane (Scythe parity).
- SOURCE FILTER (Scythe v0.9): a filter control with Project (all indexed materials — default)
  / In Level (materials referenced by the current level document's blocks) / Hidden (materials
  the user hid via a context-menu "Hide" toggle; hidden ones are excluded from the other views
  and only listed here to un-hide). Plus the name-substring search field and the M_*-only
  toggle from tools/11. Favorites/recents from tools/11 are OPTIONAL this WP — skip unless
  trivial.
- Context menu per tile: Set Active, Hide/Unhide, Open in Inspector (Scythe Ctrl+E analog:
  EditorNode::edit_resource equivalent), Reveal in FileSystem dock (Ctrl+B analog).

SCOPE (implement)
1. MaterialIndex (editor/level/material_index.{h,cpp}) — exactly per tools/11 §1.1: cold build
   off EditorFileSystem::get_filesystem() walking get_file_type() with
   ClassDB::is_parent_class(type, "Material"); never loads resources during indexing;
   incremental updates from filesystem_changed (re-walk + diff) and
   resources_reimported/resources_reload (patch in place, invalidate that path's thumbnail +
   texel cache); path-keyed everywhere, never name-keyed. Hidden set persisted in the project
   metadata side-file (EditorSettings project-metadata mechanism).
2. TexelDensityScanner (editor/level/texel_density_scanner.{h,cpp}) — tools/11 Difficulty 3
   verbatim: typed albedo getter for BaseMaterial3D family; configurable uniform-name list
   (EditorSettings level_editor/material_browser/texel_density_param_names, defaults per the
   doc) for ShaderMaterial; dimensions ALWAYS from the loaded Texture2D (post-import), never
   file stats; returns none() when no texture (render "—", never fabricate). The
   density-mismatch badge is NOT this WP (needs the project texel-density target which arrives
   with hotspots); the scanner ships now because the tile overlay needs dimensions.
3. MaterialBrowserPreviewQueue + flat-quad generator
   (editor/level/material_preview_generator.{h,cpp}) — private classes NEVER registered with
   EditorResourcePreview (first-match-wins would hijack all editor material previews).
   Mirror EditorMaterialPreviewPlugin's RID rig (dedicated scenario + disabled-update viewport +
   DrawRequester) but render a unit QUAD, orthographic camera, quad aspect from the scanner's
   texture dims so non-square trims aren't squashed, UVs showing native tiling at 1:1.
   Background generation via WorkerThreadPool or one Thread; in-memory LRU keyed by path; disk
   cache matbrowser-<md5(globalized path)>.png in EditorPaths::get_cache_dir() with
   mtime/.import-mtime invalidation copied from check_for_invalidation's logic (disjoint
   namespace from resthumb-*).
4. MaterialBrowserDock (editor/level/material_browser_dock.{h,cpp}) — virtualized grid per
   tools/11 Difficulty 2: Controls exist only for visible rows + overscan, pooled and recycled;
   thumbnails enqueued lazily when a row nears visibility (scroll-driven), never at dock-open;
   index signals patch the model without full rebuilds. Register the dock alongside the
   editor's other docks, visible when a level document is active (follow however existing
   level-editor UI gates on document type; if no such gate exists, always-available dock is
   acceptable — say which in the report).
5. Active material state on LevelEditor (tools/11 §1.3 verbatim): Ref<Material> +
   source path + active_material_changed signal; set_active_material called by the dock, the
   quick-slots, and (future) Lift. Per-pane lower-left swatch widget in LevelEditorView
   subscribes; swatch shows the flat thumbnail + display name.
6. BlockoutMaterialRegistry (editor/level/blockout_material_registry.{h,cpp}) — TEN slots,
   Shift+Alt+1..Shift+Alt+0 (hotkeys in the level view's tool-mode input path, must not fire
   during text editing or when another modal owns input). V1 SIMPLIFICATION of tools/11
   Difficulty 4 (the editor-bundled PCK is deferred — note in ledger): built-ins are
   PROCEDURAL materials constructed in C++ at first use (StandardMaterial3D + generated
   checker/grid ImageTexture, 10 visually distinct variants — greys, orange, blue, green,
   measurement-friendly), so an empty project works with zero packaging. Project override
   folder via EditorSettings level_editor/material_browser/blockout_override_folder (fill
   slots from it by filename order, pad the rest with built-ins). Quick-slot chord = "set
   active material" ONLY this WP — the "also Apply to current selection" behavior arrives with
   WP15's Apply op; leave a clear seam and note it.
7. Scythe-parity affordance: when the active material changes and the dock is open, scroll the
   grid to that material's tile (this is what makes Lift discoverable later).

NOT THIS WP: Apply/Lift/Wrap ops (WP15 — nothing here touches faces), hotspot badges, density
badge, cube previews, favorites/recents (optional), FileSystemDock-selection-sets-active
(revisit with WP15).

SMOKE
Editor suite addition (additive, follow the existing case pattern in
workspace-editor-planning/smoke/): a material_browser_smoke that runs in the testbed project —
seed the testbed with a small fixture folder of .tres materials (a few StandardMaterial3D with
albedo textures of different sizes, one ShaderMaterial with a matching uniform name, one with
no texture, mixed M_ and non-M_ names) and assert: index count and classification correct;
M_ filter and name search counts; hide/unhide round-trip persists; set_active_material fires
the signal and the swatch/dock agree; BlockoutMaterialRegistry resolves 10 slots with 0 and
with 3 override materials (0-case = all procedural built-ins, non-null, distinct); quick-slot
key event sets the active material. Thumbnail PIXEL content must not be asserted (headless);
asserting that a queue request completes with a non-null texture is fine if it works headless —
if the RID rig can't render headless, skip pixel-level checks and assert queue bookkeeping
only (say which in the report).

FILES YOU MAY TOUCH: new editor/level/material_index.*, texel_density_scanner.*,
material_preview_generator.*, material_browser_dock.*, blockout_material_registry.*;
editor/level/level_editor.{h,cpp} and level_editor_view.* (state, swatch, hotkeys, dock
wiring); editor/settings/editor_settings.cpp (new settings, additive); the smoke harness files
(additive case) + testbed fixture assets; editor/level/SCsub only if the glob misses new
files; ONE registration seam outside editor/level/ if dock registration requires it (name it
in the report; keep it to a few lines). Append one entry to
workspace-editor-planning/DIVERGENCE-LEDGER.md under the G-Level log (note the PCK deferral,
flat-default decision, and the Apply seam left for WP15).
</task>

<action_safety>
NEVER touch editor/scene/3d/node_3d_editor_viewport.* (user's uncommitted WIP). Do NOT
register anything with EditorResourcePreview and do NOT modify editor_preview_plugins.* or
editor_resource_preview.* — read-only references. No git commits. No changes outside the files
listed above. Existing bound signatures must not change. Existing smoke assertions must pass
UNMODIFIED — if one fails, your change altered behavior; fix the change, not the smoke.
</action_safety>

<verification_loop>
Build from repo root: `scons platform=windows target=editor dev_build=yes -j4`; fix all errors
and new warnings. If the FINAL LINK fails with "Access is denied" on
bin/godot.windows.editor.dev.x86_64.exe, the USER'S LIVE EDITOR may hold the lock — NEVER kill
or Stop-Process ANY godot process for ANY reason (sandbox-visible process metadata is
unreliable). Retry a few times over ~2 minutes; if still locked, build with extra_suffix=wp14
and verify against bin/godot.windows.editor.dev.x86_64.wp14.console.exe, and say so in the
report. Then run, all green: the kernel module smokes (--headless --path
modules/level_kernel/tests/smoke_project --script smoke.gd, --script transform_smoke.gd, plus
uv_smoke.gd if WP13 has landed it) and the editor suite via
`bash workspace-editor-planning/smoke/run_smoke.sh "$(pwd)/bin/<console binary>"` (if Bash
cannot start under your sandbox, run the suite's cases one-for-one from PowerShell as prior
WPs did and say so). Known pre-existing failure that is NOT yours: floating_camera_preview
(sub_viewport null). Rare pre-existing flakes (rerun once before investigating):
selection_smoke exit-139 at teardown, block_tool extrude-height 2.0-vs-3.0.
</verification_loop>

<compact_output_contract>
Final report: dock registration seam used; how virtualization + lazy thumbnail enqueue work
(pool size, overscan); the flat-preview rig (and whether it renders headless); scanner
behavior on each fixture class; how built-in blockout materials are generated; files touched;
build result (standard or suffixed + why); verbatim tails of the kernel smokes and the new
browser smoke case; any behavior deltas beyond the new feature (should be none).
</compact_output_contract>
