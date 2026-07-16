# WP21 — LE3 hotspot patch editor tab + fit-debug overlay

<task>
Build the HotspotAtlas patch-editor document tab and the fitter debug overlay in this Godot
4.8 fork (repo root = this workspace) — the UI half of LE3; the atlas resource (WP19) and
fitter (WP20) already landed. Prereq reading IN ORDER: level-editor-planning/PLAN.md §3,
level-editor-planning/tools/10-hotspot-system.md §5 (patch editor — BINDING) + §3-B (the
fit-debug visualization deferred from WP20), workspace-editor-planning/ARCHITECTURE.md (the
document/view taxonomy), then the code: how the workspace opens RESOURCE documents as tabs —
the shader editor's create_editor_view factory is the canonical template (find ShaderDocument
/ ResourceDocument in editor/, plus editor_document.h TYPE_* seams and how LevelDocument was
added for LE0); WP19's HotspotAtlas/HotspotPatch/binding registry and hotspot_rect_io; WP20's
fitter entry + per-island diagnostics API; editor/level/level_editor.{h,cpp} (registry
hosting), level_editor_view.* + tool_overlay.* (how level panes draw overlays);
level_snap_service (the [ / ] grid-step grammar to mirror).

PATCH EDITOR TAB (tools/10 §5, follow it)
1. Opening a HotspotAtlas .tres (FileSystem double-click / edit_resource route) opens a
   dedicated document tab — a ResourceDocument-style view via the same factory seam the
   shader editor uses (smallest possible shared-file touches; whatever seam you add, mirror
   the existing TYPE_LEVEL/GS2 patterns and log it). Do NOT invent a floating window.
2. Left pane: zoomable/pannable view of reference_texture (checkerboard when unset), 1:1
   texel zoom option, patches drawn as outlined rects with name labels; selected patch
   highlighted on top, others dimmed; overlapping patches expected — hover cycles hits,
   Alt+click picks beneath (§5). Drag on empty space = create patch; handles resize; drags
   snap to a px grid (power-of-two steps, [ / ] resize, snap toggle — same grammar as the
   level editor, stateless).
3. Right pane: patch list (list order = priority order, reorderable) + per-patch inspector:
   name, live numeric rect in PX (converted to normalized on store against
   reference_texture dims), allow_rotation, mirror x/y, tiling + axis, inset_px. Atlas-level:
   texel_density_target, default_mapping_mode, tiling_policy, disallow_random, param_names.
4. Every edit goes through the document's undo history (one action per gesture/field commit)
   and marks the resource dirty so the standard save path persists it. Rect drawing/resizing
   previews live and commits one undo step on release.
5. .rect Import/Export buttons wiring WP19's hotspot_rect_io (file dialogs; import replaces
   the patch set as ONE undoable action; typed parse errors surface in a dialog/status, never
   crash).
6. Binding sub-panel: list pattern-key → atlas bindings from the registry; add/remove a
   binding for THIS atlas (pattern key text field with a "derive from material…" picker that
   runs the scanner on a chosen material). Registry saves through its existing lazy path.

LIVE PREVIEW + FIT-DEBUG OVERLAY
7. "Preview on selection" toggle (§5): while on, and some level pane has a face selection,
   run the WP20 fitter (non-committing — preview path only, no diff) against the current
   selection with THIS atlas and show the resulting UVs in that pane's viewport via the
   existing preview machinery; patch edits re-run it (debounced). Toggle off / tab close /
   selection change beyond the level document's lifetime restores the real state exactly.
   If the fitter's preview path can't render UVs without committing, the minimal acceptable
   v1 is: preview draws the ISLAND partition + chosen patch names as colored face overlays +
   HUD labels instead of live UVs — say which you shipped.
8. Fit-debug overlay (§3-B, deferred from WP20): a toggle (on the Modify Texture panel
   section or the preview toggle's row) that colors each fitted face by decision key —
   green = unique density winner, amber = aspect tie-break, red = random tie-break — from the
   fitter's diagnostics, plus a HUD readout (want_area/want_aspect, finalists, chosen) for
   the hovered/selected island. Works after real Shift+H/F fits too (read the stored
   diagnostics of the last fit on the document; keep the storage ephemeral/per-session).

SMOKE (editor suite, additive — own testbed project + addon per the WP14/15/17 pattern; own
QUIT_AFTER if needed)
  a. open a fixture HotspotAtlas .tres via the document route → assert a patch-editor view
     minted in the workspace tab strip (document type + view class), not a generic inspector
     fallback; close cleanly.
  b. scripted rect create + resize on the open document → patch count/rect values correct,
     each gesture one undo step, undo restores; resource dirty flag set; save + reload
     round-trips.
  c. .rect import via the wiring (call the same handler the button uses) → patch set
     replaced as one undo step; export → file exists and reimports identically.
  d. binding add/remove round-trips through the registry file.
  (Live-preview and debug-overlay pixel output are NOT asserted — headless-unfriendly; assert
  the toggles flip state and the preview path runs without errors if drivable.)

FILES YOU MAY TOUCH: new editor/level/hotspot_patch_editor.{h,cpp} (+ split files as needed),
editor/level/level_editor*.{h,cpp} + tool_overlay.* (preview/debug overlay seams),
modules/level_kernel/** ONLY for additive fitter-preview/diagnostics accessors, the SAME
shared document seams the shader/level documents already touch (editor_document.*,
editor_data.*, editor_node.*, document_view.cpp, filesystem_dock.* — keep each touch minimal
and additive), the smoke harness + testbed (additive), DIVERGENCE-LEDGER.md (one entry
listing every shared-file touch).
</task>

<action_safety>
NEVER touch editor/scene/3d/node_3d_editor_viewport.* (user's uncommitted WIP). No git
commits. No changes outside the files listed above. Existing bound signatures must not change
(additive only). Existing smoke assertions — kernel AND editor suite — must pass UNMODIFIED;
if one fails, your change altered behavior; fix the change, not the smoke.
</action_safety>

<verification_loop>
Build from repo root: `scons platform=windows target=editor dev_build=yes -j4`; fix all errors
and new warnings. If the FINAL LINK fails with "Access is denied" on
bin/godot.windows.editor.dev.x86_64.exe, the USER'S LIVE EDITOR may hold the lock — NEVER kill
or Stop-Process ANY godot process for ANY reason (sandbox-visible process metadata is
unreliable). Retry a few times over ~2 minutes; if still locked, build with extra_suffix=wp21
and verify against bin/godot.windows.editor.dev.x86_64.wp21.console.exe, and say so in the
report. Then run, all green: every kernel smoke in modules/level_kernel/tests/smoke_project
(all existing, including hotspot smokes) and the editor suite via
`bash workspace-editor-planning/smoke/run_smoke.sh "$(pwd)/bin/<console binary>"` (if Bash
cannot start under your sandbox, run the suite's cases one-for-one from PowerShell as prior
WPs did and say so). Rare pre-existing flakes (rerun once before investigating):
selection_smoke exit-139 at teardown, block_tool extrude-height 2.0-vs-3.0.
</verification_loop>

<compact_output_contract>
Final report: the document/view seam used (every shared file touched, and how it mirrors the
shader/level document patterns); the rect-editing interaction model and undo granularity; what
the live preview actually renders (full UV preview vs the fallback) and how state restores;
how the debug overlay reads diagnostics; files touched; build result (standard or suffixed +
why); verbatim tails of all kernel smokes and the new editor case; any behavior deltas beyond
the new feature (should be none).
</compact_output_contract>
