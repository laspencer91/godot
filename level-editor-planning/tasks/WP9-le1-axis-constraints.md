# WP9 — LE1 axis/plane constraints for move drags (implementation brief)

<task>
Add Blender-style axis and plane constraints to the level editor's ACTIVE move drags in this
Godot 4.8 fork (repo root = this workspace). Prereq reading: level-editor-planning/PLAN.md §2,
then editor/level/select_tool_transform.cpp (the transform lifecycle) and select_tool.h.

FEATURE
While a move drag is ACTIVE (vertex/edge/face move, TRANSFORM_DRAG_MESH_MOVE-style modes, and
object move), pressing X, Y, or Z (no modifiers) cycles the constraint for that axis:
  free → axis-lock (delta restricted to that single WORLD axis)
  axis-lock → plane-lock (delta restricted to the world plane EXCLUDING that axis)
  plane-lock → free
Pressing a DIFFERENT axis key while constrained switches directly to that axis's axis-lock.
The constraint resets to free on every drag begin, commit, and cancel. Keys are only consumed
while a move drag is active — zero shortcut collisions otherwise (Shift+X / Shift+Z stay
reserved for future clip/mirror). Do NOT apply constraints to extrude or push/pull drags —
those are already normal-constrained by design; X/Y/Z is ignored there. Nudge is untouched.

IMPLEMENTATION SHAPE (reuse, don't invent)
select_tool_transform.cpp already contains BOTH solvers you need:
- Single-axis: the `transform_axis` path (_resolve_drag_delta ~line 334: closest-point-between-
  lines parameterization with the screen-space fallback when the axis is edge-on). Axis-lock =
  run this path with transform_axis set to the world basis vector.
- Plane: the free path (`transform_drag_plane` through `transform_pivot`). Plane-lock = same
  path with the plane normal set to the excluded world axis instead of the camera normal.
Add explicit constraint state to SelectTool (enum free/axis/plane + axis index) rather than
overloading transform_axis's emptiness as the mode signal — extrude already uses transform_axis
and must keep ignoring the constraint state.
On every constraint CHANGE mid-drag, re-derive the press reference (transform_press_axis_parameter
or transform_drag_plane + transform_press_point) from the ORIGINAL press_position under the new
constraint, then let the existing per-motion delta resolution run — this keeps snap-the-delta
semantics intact (the world-space DELTA is snapped; never absolute points) and means the preview
re-solves cleanly from the same origin with no accumulated error. The preview vertices always
re-apply from captured original positions, so switching constraints mid-drag must not compound.
Ctrl (snap inversion) and Escape (cancel) must keep working unchanged under any constraint.

VISUAL FEEDBACK
While constrained, render a guide through transform_pivot: axis-lock = one infinite-ish line
(extend a few hundred meters both ways) in the standard axis color (X red, Y green, Z blue,
match the editor theme's axis colors if accessible — see Node3DEditor's axis color settings —
otherwise hardcode the conventional values); plane-lock = the two lines of the in-plane axes.
Reuse the existing overlay infrastructure in editor/level (tool_overlay / the selection highlight
overlay's RenderingServer immediate-mesh pattern) — pick whichever needs the smaller seam. Guide
appears on constraint activation, updates never (pivot is fixed during a drag), disappears on
drag end/cancel/free. If the overlay seam turns out to require >~100 lines of new plumbing, ship
the constraint logic anyway and report the guide as deferred instead of forcing it.

SMOKE (additive only)
Extend level-editor-planning/testbed/addons/transform_smoke/plugin.gd with a new section (do not
modify existing assertions): select a vertex, start a drag toward a diagonal screen target, send
KEY_X mid-drag (use the existing _send_key helper pattern but WITHOUT the trailing release
requirement — check how the tool reads keys), complete the drag, assert the committed delta is
non-zero on X and exactly zero on Y and Z. Then a second drag with KEY_X pressed twice
(plane-lock): assert the committed delta has zero X component. Keep prints/failure style
consistent with the file.

FILES YOU MAY TOUCH: editor/level/select_tool.h, select_tool.cpp, select_tool_transform.cpp,
tool_overlay.* or selection_highlight_overlay.* (guide), level_editor_view.* ONLY if input
routing requires it, testbed transform_smoke plugin.gd (additive). Append one entry to
workspace-editor-planning/DIVERGENCE-LEDGER.md under the G-Level log (this is editor/level-local,
so it may just be a feature note, follow the ledger's existing granularity).
</task>

<action_safety>
NEVER touch editor/scene/3d/node_3d_editor_viewport.* (user's uncommitted WIP). No git commits.
No changes outside the files listed above. Existing bound signatures must not change. Existing
smoke assertions must pass UNMODIFIED — if one fails, your change altered behavior; fix the
change, not the smoke.
</action_safety>

<verification_loop>
Build from repo root: `scons platform=windows target=editor dev_build=yes -j4`; fix all errors
and new warnings. If the FINAL LINK fails with "Access is denied" on
bin/godot.windows.editor.dev.x86_64.exe, the USER'S LIVE EDITOR holds the lock — NEVER kill or
Stop-Process ANY godot process for ANY reason (sandbox-visible process metadata is unreliable).
Retry a few times over ~2 minutes; if still locked, build with extra_suffix=wp9 and verify
against bin/godot.windows.editor.dev.x86_64.wp9.console.exe, and say so in the report.
Then run, all green: the kernel module smokes (--headless --path
modules/level_kernel/tests/smoke_project --script smoke.gd, and --script transform_smoke.gd) and
the editor suite via `bash workspace-editor-planning/smoke/run_smoke.sh "$(pwd)/bin/<console binary>"`.
Known pre-existing failure that is NOT yours: floating_camera_preview (sub_viewport null).
</verification_loop>

<compact_output_contract>
Final report: constraint state design (enum + where it lives); how the press reference is
re-derived on constraint change; guide overlay approach (or deferred + why); files touched;
build result (standard or suffixed + why); verbatim tails of transform_smoke (module) and the
editor transform_smoke case; any behavior deltas beyond the new feature (should be none).
</compact_output_contract>
