# WP18 — Block tool usability fixes (surface-start, hover ghost, silent-failure repair)

<task>
Fix the level editor's Block tool usability defects in this Godot 4.8 fork (repo root = this
workspace), found by a read-only investigation of user-reported symptoms ("holding Shift at a
viable position shows no preview"; "dragging often produces nothing — feels like it needs to
be just right"). Prereq reading IN ORDER: level-editor-planning/tools/01-block-tool.md (the
spec — §drag-plane freeze, §surface snap: hit normal → nearest world axis + axis-keyed tangent
table + sloped-face fallback, §degenerate-drag guards), TOOL-FOUNDATIONS.md §4 Block row, then
the code: editor/level/block_tool.cpp (whole file — the Pending/Base/Height state machine),
editor/level/tool_overlay.{h,cpp} (update_box, material flags), editor/level/level_editor_view.cpp
(input routing to active_tool), editor/level/level_snap_service.*, and how select_tool.cpp
does viewport→element raycasts (the per-block element BVH picking path — reuse it, do not
invent a second raycast).

ROOT CAUSES (verified, with the defect list from the investigation):
- Only the world ground plane Y=0 is a viable gesture start; the spec's surface-hit start was
  never implemented. Aiming at any face (wall, upper floor) silently does nothing.
- There is no hover-time preview at all; the ghost first appears after press + 4px drag +
  valid snapped base area.
- _begin_pending → BASE_DRAG transition happens even when the base intersection fails,
  leaving a silent dead gesture.
- _update_preview does nothing on invalid input instead of clearing — stale ghosts linger.
- The ghost is depth-tested, so a valid preview can be hidden inside existing geometry.
- The "second LMB accepts default height" path can recompute the height to zero and commit
  nothing.
- LevelSnapService's snap_enabled flag is ignored — the tool always snaps via
  snap_step_or_default().

IMPLEMENT
1. SURFACE-HIT GESTURE START (the spec feature): on press (and on hover, below), raycast the
   pane's level geometry via the SAME per-block element BVH path the Select tool picks with.
   On a face hit: classify the hit face normal to the NEAREST WORLD AXIS (spec rule), freeze
   the drag plane at the hit point with that axis as plane normal, tangent basis from the
   spec's axis-keyed stable tangent table (deterministic, no per-frame re-derivation, no
   flicker). No geometry hit → fall back to the existing Y=0 ground plane. Near-45° sloped
   faces: classify to the nearest axis anyway (deterministic tie → document which); the plane
   passes through the hit point regardless, so blocks build flush against the aimed surface.
   The rest of the state machine (Base/Height, snapping the DELTA, has_volume guards, default
   height, commit) operates on the frozen plane exactly as it does for ground today.
2. HOVER FOOTPRINT GHOST (new UX requirement from the user): while the Block tool is active
   with NO gesture in progress, every pointer motion computes the viable start (rule 1) and
   shows a one-grid-cell footprint ghost (wireframe rectangle of the current snap step, lying
   in the frozen-candidate plane, snapped the way a press there would snap). Nothing viable
   under the cursor → ghost hidden. This doubles as the "this spot is viable" indicator the
   user expected. Keep it cheap: one raycast per motion event, reusing cached BVHs; no kernel
   calls.
3. FEEDBACK/STATE REPAIR: (a) a failed base intersection must NOT enter BASE_DRAG — stay
   PENDING (or cancel cleanly on release); (b) _update_preview clears the overlay when the
   spec/box is invalid instead of leaving the stale ghost; (c) ghost WIREFRAME renders with
   FLAG_DISABLE_DEPTH_TEST like the constraint guides already do (keep the translucent fill
   depth-tested so occlusion still reads); (d) fix the second-LMB default-height acceptance so
   accepting at the press position commits the default-height block instead of recomputing a
   zero height; (e) respect LevelSnapService's snap_enabled — when snapping is off, use raw
   plane coordinates (keep the has_volume/degenerate guards).
4. Deliberately NOT in scope (note as deferred in the ledger): active-selection top plane,
   Ctrl camera-facing basis flip, height-locked visual indicator, Select-tool fallthrough on
   bare click.

SMOKE (additive to level-editor-planning/testbed/addons/block_tool_smoke/plugin.gd; existing
assertions unchanged)
  a. surface-start: create a box, then drive a full drag gesture starting ON its top face →
     a new block sits on top of it (base plane = the face plane), correct height/volume, one
     undo step.
  b. wall-start: drag starting on a vertical side face → block extrudes flush from that wall
     plane (base plane normal = the wall's nearest world axis).
  c. second-LMB default-height acceptance: base drag, release, LMB again without motion →
     block committed with the default height (regression for the zero-recompute bug).
  d. hover ghost: with the tool active and the pointer over the ground/a face, assert the
     overlay has geometry; over empty sky, assert it is cleared (drive via the overlay's
     has_geometry-style state or an equivalent scriptable probe — smallest seam wins).
  e. failed-start: press with the pointer at a guaranteed-miss ray (horizon/sky) → no state
     advance, next valid gesture works immediately (regression for the dead-gesture bug).
Mind the suite's per-case frame budget: block_tool_smoke may need its own QUIT_AFTER bump like
TRANSFORM_QUIT_AFTER if the new sections push it past 200 frames.

FILES YOU MAY TOUCH: editor/level/block_tool.{h,cpp}, tool_overlay.{h,cpp} (no-depth wireframe
+ clear semantics; do NOT restructure the shared-mesh design), level_editor_view.* ONLY if
hover motion doesn't already reach the active tool, level_snap_service.* (snap_enabled
respect, additive), the block_tool smoke + run_smoke.sh (budget only), and
workspace-editor-planning/DIVERGENCE-LEDGER.md (one entry; list the deferrals).
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
unreliable). Retry a few times over ~2 minutes; if still locked, build with extra_suffix=wp18
and verify against bin/godot.windows.editor.dev.x86_64.wp18.console.exe, and say so in the
report. Then run, all green: every kernel smoke in modules/level_kernel/tests/smoke_project
(smoke.gd, transform_smoke.gd, uv_smoke.gd, face_texture_smoke.gd, unwrap_smoke.gd) and the
editor suite via `bash workspace-editor-planning/smoke/run_smoke.sh "$(pwd)/bin/<console binary>"`
(if Bash cannot start under your sandbox, run the suite's cases one-for-one from PowerShell as
prior WPs did and say so). Rare pre-existing flakes (rerun once before investigating):
selection_smoke exit-139 at teardown, block_tool extrude-height 2.0-vs-3.0.
</verification_loop>

<compact_output_contract>
Final report: the surface-hit classification + tangent-table implementation and where the
raycast plugs into the existing picking path; the hover-ghost update path and its per-motion
cost; each state-machine defect fix; files touched; build result (standard or suffixed + why);
verbatim tails of all kernel smokes and the block_tool editor case; any behavior deltas beyond
the fixes (should be none).
</compact_output_contract>
