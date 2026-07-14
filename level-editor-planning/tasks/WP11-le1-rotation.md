# WP11 — LE1 modal rotation for the Select tool (implementation brief)

<task>
Add Blender-style modal rotation to the level editor's Select tool in this Godot 4.8 fork
(repo root = this workspace). Prereq reading: level-editor-planning/PLAN.md §2,
level-editor-planning/tools/03-transform-snapping.md §1 stages 4-6 (kernel preview lifecycle;
IGNORE its gizmo stages — gizmos are a later WP), then editor/level/select_tool.h,
select_tool.cpp (_handle_input), and select_tool_transform.cpp (the WP9-refactored transform
lifecycle: _begin_transform_drag, _rederive_transform_press_reference(mode, axis),
_closest_axis_parameter, _resolve_drag_delta, _end_transform_drag, _cycle_transform_constraint).

FEATURE
With a non-empty selection (any tier: vertex/edge/face at either tier, or object mode) and NO
active drag, pressing R (no modifiers) starts a MODAL rotation:
- The rotation pivot is the selection centroid (same computation _begin_transform_drag already
  does for moves). The default rotation axis is the CAMERA VIEW axis through the pivot
  (camera forward), i.e. free rotate in the view plane.
- While modal: plain pointer motion (no button held) rotates the selection preview around the
  axis. The angle is the signed screen-space angle swept between (press_screen -> pivot_screen)
  and (current_screen -> pivot_screen) vectors around the pivot's screen projection. Guard the
  degenerate case where the pointer is within a few pixels of the pivot projection (hold the
  last angle rather than jumping).
- X / Y / Z (no modifiers) during the modal constrain the rotation axis to that WORLD axis;
  pressing the same key again returns to the view axis (a plane-lock stage is meaningless for
  rotation — the cycle is view-axis <-> world-axis only). Reuse the existing
  TransformConstraintMode state and guide-overlay rendering (axis-lock guides through the pivot
  via ToolOverlay::update_constraint_guides; free/view-axis shows no guide). On every axis
  change, recompute the preview from the ORIGINAL captured positions under the new axis with the
  same total swept angle semantics (re-zero the angle at the constraint change is also
  acceptable if you document it — pick ONE and assert it in the smoke).
- Ctrl inverts angle snapping, mirroring the move drag's Ctrl semantics exactly (same
  inversion, not a separate toggle). Snapped angle step: add an angle-step entry point to the
  existing LevelSnapService (default 15 degrees) parallel to snap_delta — snap the swept ANGLE,
  never the resulting positions (snap-the-delta rule, PLAN.md §2).
- LMB press (or Enter) COMMITS: one undo action ("Rotate Level Selection" for mesh tiers /
  "Rotate Level Blocks" for object mode) via the existing commit path — mesh tiers through the
  open transform preview -> commit_transform_preview -> _register_mesh_undo, object mode via
  set_global_transform do/undo pairs like TRANSFORM_DRAG_OBJECT_MOVE.
- Escape (or RMB press) CANCELS via the existing cancel path — restores originals, no undo step.
- R is ONLY consumed when a selection exists, no drag/marquee/modal is already active, and no
  modifiers are held. It must not fire during an active move drag, during block-tool gestures,
  or steal any existing binding (grep select_tool.cpp and level_editor_view for current R/Enter
  usage first; if R collides with something, report it and pick a defensible alternative).

IMPLEMENTATION SHAPE (reuse, don't invent)
- Model the modal as a new TransformDragMode (e.g. TRANSFORM_DRAG_ROTATE) so the ENTIRE existing
  lifecycle is reused: _collect_transform_selection, pivot computation, capture_original_positions,
  _open_mesh_previews, _end_transform_drag, Escape handling. The differences from move: it is
  keyboard-initiated (no pointer press), preview updates on UNBUTTONED pointer motion, and commit
  is a button PRESS rather than release — thread that through _handle_input/level_editor_view
  input routing with the smallest seam (check how motion events reach the tool when no button is
  held; the block tool's hover preview is the reference if one exists).
- Rotation preview positions are computed editor-side from the captured originals (exactly like
  _apply_mesh_preview_delta does for translation): world-space rotation about the pivot,
  R = Basis(axis, angle); per vertex: new_world = pivot + R * (orig_world - pivot), converted
  back to block-local through the block's global transform. Object mode: preview_transform =
  rotate the ORIGINAL transform about the world pivot (origin orbits, basis pre-multiplied).
  No new kernel API — the existing begin/preview/commit/cancel_transform_preview vertex-position
  path is sufficient.
- Angle snapping lives in LevelSnapService as e.g. snap_angle(angle_radians) with a
  DEFAULT_ANGLE_STEP of 15 degrees, honoring the service's existing enabled/inversion patterns.
  Keep the headless-testable invariant: output depends only on the input angle and step.
- Do NOT implement: gizmo handles, temp/MMB pivot overrides, Local basis, texture-lock UV
  compensation (LE2 §3.4 — note it as deferred in the ledger entry), scale, per-object
  individual-origin pivots. Extrude/push-pull/move behavior must be untouched.

SMOKE (additive only)
Extend level-editor-planning/testbed/addons/transform_smoke/plugin.gd with a new rotation
section (do not modify existing assertions; reuse _send_key/_send_drag helpers, adding a
mid-motion variant only if unbuttoned motion needs one):
  a. select a face (or the block in object mode — pick whichever is most deterministic), press R,
     move the pointer to sweep an angle with Ctrl held so the 15-degree snap makes the result
     exact, press X to constrain to world X, commit with an LMB press; assert the committed
     vertex positions match a closed-form rotation of the originals about the pivot and world X
     axis to within epsilon, distances to the pivot preserved, and exactly ONE undo step was
     created (undo restores originals byte-identical).
  b. second run: press R, sweep, then Escape; assert positions restored exactly and no undo step.
  c. assert R with an active move drag in progress does nothing (constraint keys still work).

FILES YOU MAY TOUCH: editor/level/select_tool.h, select_tool.cpp, select_tool_transform.cpp,
level_snap_service.{h,cpp} (angle step), tool_overlay.* ONLY if the guide rendering needs a
seam beyond what exists, level_editor_view.* ONLY if unbuttoned-motion routing requires it,
testbed transform_smoke plugin.gd (additive). Append one entry to
workspace-editor-planning/DIVERGENCE-LEDGER.md under the G-Level log (editor/level-local feature
note, matching the ledger's existing granularity; mention texture-lock deferral).
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
bin/godot.windows.editor.dev.x86_64.exe, the USER'S LIVE EDITOR may hold the lock — NEVER kill
or Stop-Process ANY godot process for ANY reason (sandbox-visible process metadata is
unreliable). Retry a few times over ~2 minutes; if still locked, build with extra_suffix=wp11
and verify against bin/godot.windows.editor.dev.x86_64.wp11.console.exe, and say so in the
report. Then run, all green: the kernel module smokes (--headless --path
modules/level_kernel/tests/smoke_project --script smoke.gd, and --script transform_smoke.gd) and
the editor suite via `bash workspace-editor-planning/smoke/run_smoke.sh "$(pwd)/bin/<console binary>"`
(if Bash cannot start under your sandbox, run the suite's cases one-for-one from PowerShell as
prior WPs did and say so). Known pre-existing failure that is NOT yours: floating_camera_preview
(sub_viewport null).
</verification_loop>

<compact_output_contract>
Final report: the modal's input-routing seam (how unbuttoned motion and the commit press reach
the tool); the angle math and which axis-change semantic you picked (preserved swept angle vs
re-zeroed, and why); the LevelSnapService angle entry point; files touched; build result
(standard or suffixed + why); verbatim tails of both module smokes and the editor transform
case; any behavior deltas beyond the new feature (should be none).
</compact_output_contract>
