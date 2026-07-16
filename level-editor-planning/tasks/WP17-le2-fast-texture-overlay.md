# WP17 — LE2 Fast Texture overlay (Shift+Q modal 2D UV editor)

<task>
Build the Fast Texture modal overlay for the level editor in this Godot 4.8 fork (repo root =
this workspace) — the UI half whose kernel (WP16 unwrap ops) already landed. Prereq reading IN
ORDER: level-editor-planning/PLAN.md §2, level-editor-planning/TOOL-FOUNDATIONS.md §2 + §4
(unwrap "overlay editor" row: tool-owned Control at the VIEW-STATE layer, not a second pane),
then level-editor-planning/tools/08-uv-unwrap-fast-texture.md IN FULL (§1 storage/mode table,
§3 pipeline steps 2-6, §4.4 the overlay design — binding), and
workspace-editor-planning/ARCHITECTURE.md §2 (the view-state test the design is scored
against). Then the code: WP16's unwrap bindings on LevelMesh (read the actual landed surface:
unwrap_square/unwrap_planar/unwrap_conforming/unwrap_follow_quads + the non-mutating
hinge-unfold utility), LevelMeshData.duplicate_data/copy paths, editor/level/
level_editor_view.* (per-pane lifecycle, input routing, WP15's op routing),
select_tool.* (selection access). UI porting reference (MIT, mine for mechanism per D2, do
not copy wholesale): the local Cyclops Level Builder clone at
C:\Users\laspe\AppData\Local\Temp\claude\C--Development-Games-one-more-house\a9ac96ea-3a04-42c8-9b6c-62a5c14e913b\scratchpad\cyclops\godot\addons\cyclops_level_builder\
— specifically gui/docks/uv_editor/uv_editor.gd (proj_transform pan/zoom, draw_grid/
draw_subdiv_grid/draw_uv_mesh) and tools/uv_editor/tool_uv_box_transform.gd (box-transform
gizmo shape). If that path is unreadable from your sandbox, implement from the spec's
description alone.

SESSION MODEL (08 §3, the working-copy rule is BINDING: nothing writes LevelMeshData until
accept)
- Shift+Q with a non-empty FACE selection (either tier) on the focused level pane opens the
  overlay for that pane (per-pane instance, created/freed by LevelEditorView — view state).
  No selection or wrong tier → status message, no overlay.
- On open: snapshot the selection's current UV state (uv_mode/origin/tangent/uv_transform +
  loop UVs) for exact cancel/reference, and push a scoped shortcut/input context that owns the
  overlay keys and swallows ALL pane 3D input while active (08 §4.4 — modal; the 3D viewport
  keeps rendering behind it). Pop the context on accept/cancel/pane-defocus (defocus =
  cancel).
- WORKING COPY mechanics: clone the affected mesh data (LevelMeshData duplicate or a scratch
  LevelMesh) and run mode recomputations against the CLONE, reading back UVs for the 2D
  display. WP16 ops are deterministic, so clone results == accept results. Mode keys always
  recompute from the ORIGINAL snapshot state (modes are exclusive per session, never
  compounded — 08 §3 step 3).
- ACCEPT (Enter): apply to the REAL mesh as ONE undoable editor action: the chosen unwrap op
  (if any mode other than Use Existing was picked) plus the composed box-transform Transform2D
  folded per the §1 rule — into uv_transform for Square/Planar/Use-Existing-on-PROJECTED,
  baked into loop UVs for Conforming/Follow-Quads/EXPLICIT. Close, pop context.
- CANCEL (Escape): drop the clone, write nothing, no undo entry, close, pop context.

OVERLAY UI
- 2D view: own pan/zoom Transform2D (MMB/scroll), _draw()-rendered: UV grid + subdivision
  grid ([ / ] change subdiv), the selected faces' UV polygons (from the working copy),
  selected-in-overlay highlight. F frames the UV bbox. Background: the active/original
  material's albedo texture tiled when B (repeat background) is on; T toggles between the
  face's own material and a neutral checker.
- Mode keys: 1 Use Existing (display current state, no recompute), 2 Conforming, 3 Square,
  4 Follow Quads (Ctrl+1/2/3 = Length/Even/Length-Average spacing, re-runs the mode),
  5 Planar. Show the active mode + spacing in a small HUD line. Kernel rejections (e.g.
  non-quad follow-quads seed) surface as a HUD message, overlay stays open on the previous
  state.
- Box-transform: a rect gizmo around the working copy's UV bbox — drag inside = translate,
  corner handles = scale (Shift = uniform), Q/E = rotate steps (15°), Alt+R / Alt+T = flip
  H/V. All compose ONE additional Transform2D on top of the mode result (bbox-center pivot),
  live-updating the 2D view. G toggles world-scale display units, C toggles a cursor UV
  coordinate readout. MMB temp pivot and free-rotate handles are DEFERRED (ledger note).
- No 3D-viewport live material preview this WP (the 3D view shows the pre-accept state until
  accept) — deferred, ledger note.

SCRIPTABLE SESSION API (the smoke needs this; keep UI thin over it)
A session object (or methods on the overlay) bindable/creatable headlessly: open(face_ids) /
set_mode(mode, spacing) / set_nudge(Transform2D) / accept() -> bool / cancel(), with getters
for the working copy's loop UVs. The editor overlay drives exactly this API — no logic in
_gui_input beyond translating input to these calls (08 §5 "overlay accept/cancel diff count"
requires headless drivability).

SMOKE
Kernel/testbed: new fast_texture_smoke (editor suite case following the WP14/WP15 pattern —
own testbed project + addon; give it its own QUIT_AFTER if it needs >200 frames):
  a. open session on a box face selection → set_mode(SQUARE) → accept → exactly ONE undo
     action; undo restores pre-session UV columns byte-identical.
  b. open → set_mode(CONFORMING) → cancel → mesh byte-identical, no undo entry.
  c. open → mode → set_nudge(translation) → accept → resulting loop UVs = mode result shifted
     by the nudge (PROJECTED case folds into uv_transform: assert via get_uv).
  d. Shift+Q with empty selection does not open; Escape closes and restores pane input
     routing (a subsequent normal selection click works).
  e. mode keys recompute from the ORIGINAL snapshot: apply SQUARE then PLANAR in one session,
     accept → result identical to a fresh session applying only PLANAR.

FILES YOU MAY TOUCH: new editor/level/fast_texture_overlay.{h,cpp} (+ a session/model file if
you split it), editor/level/level_editor_view.* (open/close wiring, input context),
editor/level/select_tool.* ONLY if selection access needs a getter, modules/level_kernel/**
ONLY for additive session-support bindings (no behavior changes to WP16 ops), the smoke
harness + testbed (additive). Append one entry to
workspace-editor-planning/DIVERGENCE-LEDGER.md under the G-Level log (note the deferred MMB
pivot, 3D live preview, and anything else cut).
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
unreliable). Retry a few times over ~2 minutes; if still locked, build with extra_suffix=wp17
and verify against bin/godot.windows.editor.dev.x86_64.wp17.console.exe, and say so in the
report. Then run, all green: every kernel smoke in modules/level_kernel/tests/smoke_project
(smoke.gd, transform_smoke.gd, uv_smoke.gd, face_texture_smoke.gd, unwrap_smoke.gd) and the
editor suite via `bash workspace-editor-planning/smoke/run_smoke.sh "$(pwd)/bin/<console binary>"`
(if Bash cannot start under your sandbox, run the suite's cases one-for-one from PowerShell as
prior WPs did and say so). Rare pre-existing flakes (rerun once before investigating):
selection_smoke exit-139 at teardown, block_tool extrude-height 2.0-vs-3.0.
</verification_loop>

<compact_output_contract>
Final report: the session API surface and how the working-copy clone is built; the input
context push/pop mechanics and what happens on pane defocus; how accept composes mode + nudge
into one action for PROJECTED vs EXPLICIT results; files touched; build result (standard or
suffixed + why); verbatim tails of ALL kernel smokes and the new editor case; any behavior
deltas beyond the new feature (should be none).
</compact_output_contract>
