# WP12 — LE1 transform gizmos (move arrows/planes + rotate rings) for the Select tool

<task>
Add persistent Hammer/Blender-style transform gizmo handles to the level editor's Select tool in
this Godot 4.8 fork (repo root = this workspace). Prereq reading: level-editor-planning/PLAN.md §2,
level-editor-planning/tools/03-transform-snapping.md §1 (stages 0-6 — THIS WP implements the gizmo
stages 1-2 that WP9/WP11 skipped) and §3.1 (pivot; Local basis is NOT in scope), then
editor/level/select_tool.h, select_tool.cpp (_handle_input, R-key modal entry, pointer routing),
select_tool_transform.cpp (the full transform lifecycle: _begin_transform_drag,
_collect_transform_selection, capture/originals, _apply_mesh_preview_delta, _apply_rotation_preview,
_resolve_drag_delta, _cycle_transform_constraint, _rederive_transform_press_reference,
_end_transform_drag), tool_overlay.h/.cpp, and level_editor_view.* (input + overlay wiring).
For reference ONLY (do not modify): the stock gizmo's screen-scale, handle hit-test, and
drag-plane math in editor/scene/3d/node_3d_editor_viewport.cpp (_transform_gizmo_select,
update_transform) and the gizmo mesh construction in editor/scene/3d/node_3d_editor_plugin.cpp
(_init_indicators).

FEATURE
Whenever the Select tool is active and the selection is non-empty (any tier: vertex/edge/face at
either tier, or object mode), draw a transform gizmo at the selection pivot (the same centroid
_begin_transform_drag computes) with WORLD-axis orientation:
- MOVE handles: three axis arrows (X red / Y green / Z blue, using the same editor-theme axis
  colors ToolOverlay's _ensure_axis_materials already resolves) and three plane quads (XY/XZ/YZ)
  offset from the pivot like the stock gizmo.
- ROTATE handles: three axis-aligned circles (rings) around the pivot in the same colors.
- Constant screen-size scaling: scale the gizmo by distance-to-camera / FOV so it occupies a
  fixed apparent size, same formula family as the stock viewport's gizmo scale. Recompute per
  frame (camera moves during drags).
- Hover highlight: the handle under the cursor brightens (material swap or albedo boost).

INTERACTION
- LMB press on a MOVE arrow begins the EXISTING move-drag lifecycle with the constraint pre-set
  to (AXIS, that axis); press on a plane quad pre-sets (PLANE, that axis = the plane's normal).
  From there the drag IS a normal WP9 move drag: same preview, same snapping (snap the world
  DELTA via LevelSnapService, Ctrl inversion), same commit-on-release with one undo step, same
  Escape/RMB cancel. X/Y/Z constraint-cycling keys must still work mid-drag and may override the
  gizmo-chosen constraint (they already operate on the same state — verify, don't fork it).
- LMB press on a ROTATE ring begins the EXISTING rotation lifecycle (TRANSFORM_DRAG_ROTATE) with
  the axis pre-constrained to that ring's WORLD axis, but with DRAG semantics: preview updates on
  buttoned pointer motion, COMMIT on LMB RELEASE (one undo step), Escape/RMB cancels. This is the
  same state machine as the R modal — thread a "commit on release vs commit on press" distinction
  through the existing mode with the smallest seam (e.g. a bool set at begin time), do NOT clone
  the lifecycle. Angle math: reuse the WP11 screen-space swept-angle computation about the pivot's
  screen projection; Ctrl inverts the existing LevelSnapService::snap_angle 15-degree snapping.
  X/Y/Z during a ring drag keeps its WP11 meaning (world-axis toggle).
- Gizmo hit test runs BEFORE click-selection when the selection is non-empty: a press on a handle
  never changes the selection; a press anywhere else falls through to the existing pick/marquee
  behavior unchanged. Hit-test approach: project handle geometry to rays like the stock
  _transform_gizmo_select (arrow = ray-vs-capsule/segment distance, plane quad = ray-plane
  intersection inside the quad bounds, ring = ray-plane intersection with |distance-to-pivot -
  radius| band check), with a few-pixel screen-space tolerance. Keep it in editor code — no new
  kernel API.
- The gizmo hides during: any active drag/marquee (only the active handle or the WP9 guide lines
  should show mid-gesture — pick the stock behavior: hide inactive handles during a drag), the R
  modal, and block-tool gestures. It reappears at the new pivot after commit/cancel and tracks
  selection changes immediately.
- The R modal, bare-G-style move drags on selected geometry, and all existing bindings must be
  completely unaffected when the cursor is not on a handle.

IMPLEMENTATION SHAPE (reuse, don't invent)
- New files editor/level/transform_gizmo.{h,cpp} (name at your discretion) owning the handle
  meshes, per-frame placement/scaling, hover state, and hit testing — built once, instanced into
  the pane's World3D alongside ToolOverlay (follow ToolOverlay's RID/instance lifecycle pattern,
  including its teardown discipline; editor/level/*.cpp is picked up by the existing SCsub glob —
  verify, and add explicitly only if not).
- Do NOT render gizmo geometry through ToolOverlay's shared ImmediateMesh (it is last-writer-wins
  with the selection box and guides — a known wart; gizmo handles are persistent meshes, not
  immediate geometry).
- SelectTool drives it: selection-changed → reposition; input routing gives the gizmo first
  refusal on LMB press; begin/end of any transform gesture toggles visibility. Keep the gizmo
  object dumb (geometry + hit test), keep the lifecycle decisions in SelectTool.
- Pivot/basis: centroid + world axes only. Do NOT implement: Local basis, active-element pivot,
  MMB temp pivot (all §3.1, later WPs), scale handles (no modal scale exists yet — note the
  deferral in the ledger entry), texture-lock UV compensation (LE2 §3.4), CapsLock drag-align.

SMOKE (additive only)
Extend level-editor-planning/testbed/addons/transform_smoke/plugin.gd with a new gizmo section
(do not modify existing assertions; reuse _send_key/_send_drag/_send_click helpers):
  a. select geometry, compute the pivot's screen projection plus the X-arrow's screen offset,
     drag from the arrow along X; assert the committed positions moved on world X ONLY (Y/Z
     unchanged to epsilon), snapping honored, exactly ONE undo step, undo restores originals.
  b. drag starting on a plane quad; assert motion confined to that plane (normal component zero
     to epsilon), one undo step.
  c. drag starting on a rotate ring with Ctrl held (15-degree snap makes it exact); assert
     committed positions match a closed-form rotation about the pivot and that ring's world axis,
     pivot distances preserved, one undo step; a second ring drag cancelled with Escape restores
     originals exactly with no undo step.
  d. with a non-empty selection, an LMB press/drag NOT on any handle still performs normal
     click-selection/marquee (assert selection changed as before); with the gizmo visible, a
     press ON a handle does not change the selection.

FILES YOU MAY TOUCH: new editor/level/transform_gizmo.{h,cpp} (or your chosen name),
editor/level/select_tool.h, select_tool.cpp, select_tool_transform.cpp, level_editor_view.*
(wiring/routing only), tool_overlay.* ONLY if a shared seam (e.g. axis-material access) needs
extracting, editor/level/SCsub only if the glob misses new files, testbed transform_smoke
plugin.gd (additive). Append one entry to workspace-editor-planning/DIVERGENCE-LEDGER.md under
the G-Level log (mention the scale-handle and Local-basis deferrals).
</task>

<action_safety>
NEVER touch editor/scene/3d/node_3d_editor_viewport.* or node_3d_editor_plugin.* (read-only
references; the viewport files are the user's uncommitted WIP). No git commits. No changes
outside the files listed above. Existing bound signatures must not change. Existing smoke
assertions must pass UNMODIFIED — if one fails, your change altered behavior; fix the change,
not the smoke.
</action_safety>

<verification_loop>
Build from repo root: `scons platform=windows target=editor dev_build=yes -j4`; fix all errors
and new warnings. If the FINAL LINK fails with "Access is denied" on
bin/godot.windows.editor.dev.x86_64.exe, the USER'S LIVE EDITOR may hold the lock — NEVER kill
or Stop-Process ANY godot process for ANY reason (sandbox-visible process metadata is
unreliable). Retry a few times over ~2 minutes; if still locked, build with extra_suffix=wp12
and verify against bin/godot.windows.editor.dev.x86_64.wp12.console.exe, and say so in the
report. Then run, all green: the kernel module smokes (--headless --path
modules/level_kernel/tests/smoke_project --script smoke.gd, and --script transform_smoke.gd) and
the editor suite via `bash workspace-editor-planning/smoke/run_smoke.sh "$(pwd)/bin/<console binary>"`
(if Bash cannot start under your sandbox, run the suite's cases one-for-one from PowerShell as
prior WPs did and say so). Known pre-existing failure that is NOT yours: floating_camera_preview
(sub_viewport null). Rare pre-existing flakes (rerun once before investigating): selection_smoke
exit-139 at teardown, block_tool extrude-height 2.0-vs-3.0.
</verification_loop>

<compact_output_contract>
Final report: the gizmo's render/instance lifecycle and where hit-testing hooks into input
routing; how the pre-set constraint enters the existing move lifecycle and how commit-on-release
was threaded into the rotation mode; the screen-scale formula used; files touched; build result
(standard or suffixed + why); verbatim tails of both module smokes and the editor transform
case; any behavior deltas beyond the new feature (should be none).
</compact_output_contract>
