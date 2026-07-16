<task>
WP22 — Block tool: always-visible, minimum-one-cell base preview.

Repo: C:\Development\Engines\godot (custom Godot fork, master). File: editor/level/block_tool.cpp/.h
(plus modules/level_kernel/tests / workspace-editor-planning/smoke as needed for verification).

User-reported problem (playtest, verified in code):
- During the first (base) drag, a lot of the time NOTHING is drawn. Root cause chain:
  1. `_update_base` snaps p1 to the plane grid; until the drag crosses a full cell boundary on BOTH
     tangent axes, `p1 - p0` has a zero local extent, `_get_box_spec` fails `has_volume()`, and
     `_update_preview()` falls through to `_clear_preview()` — the user drags with zero feedback.
  2. Releasing while the base is degenerate hits the `!_has_base_area()` branch in `_handle_input`
     release handling: `exit_gesture()` + re-arm PENDING — the gesture silently produces nothing.
  3. The idle hover footprint only renders in STATE_IDLE, so during PENDING/BASE_DRAG there is no
     fallback visual at all.

Required behavior (the contract):
1. Minimum-one-cell base. When `gesture_snap_enabled`, each tangent-axis extent of the base rect is
   clamped to at least one `gesture_snap_step`. The sign of a clamped axis comes from the UNSNAPPED
   drag-plane intersection delta on that axis (which way the mouse actually moved); if that delta is
   exactly zero, pick the positive tangent direction. Intuition from the user: "if I click in a grid
   spot and drag to the next grid spot — that's it; it should take up the grid's width anyway."
   When snapping is disabled (free drag on the ground plane with snap off), keep the existing
   epsilon guard — no clamp needed; degenerate extents are practically impossible unsnapped.
2. The preview is ALWAYS visible from the moment `drag_started` fires until commit/cancel. With the
   min-cell clamp, `_get_box_spec` succeeds for every base drag; do not clear the preview
   mid-gesture except when the drag-plane ray intersection genuinely fails (grazing/behind camera) —
   and in that case keep the last valid preview rather than flashing to nothing.
3. Releasing after a real drag NEVER dead-ends. The `!_has_base_area()` exit_gesture/re-arm branch
   goes away for snapped gestures: release with a within-one-cell drag proceeds exactly like any
   other base release (into HEIGHT_DRAG, or straight commit under the look-down-axis shortcut) with
   the clamped one-cell base. A bare click with NO drag (STATE_PENDING release) stays a no-op —
   that branch is correct and must not change.
4. Commit uses the same clamped base — a one-cell-base block is a legitimate commit result.
5. The clamp lives at ONE altitude: prefer clamping where the base extents are derived (a shared
   helper used by `_update_base`/`_get_box_spec`/`_has_base_area`) rather than sprinkling
   special cases in the state machine.

Out of scope: height-stage behavior, hover footprint styling, Ctrl basis flip, top-plane starts.
Do NOT touch editor/scene/3d/node_3d_editor_viewport.* (user's WIP) or editor/gui/document_view.cpp.
</task>

<action_safety>
- NEVER kill, Stop-Process, or taskkill ANY godot process for any reason, including ones you think
  are stale — orphaned editors are for the human to clean up. If a build fails with "Access is
  denied" on the output exe (often masked by a cosmetic methods.py AttributeError), fall back to
  `extra_suffix=wp22` instead of fighting the lock.
- Never run two scons builds concurrently in this tree.
- No git commits. Leave everything uncommitted.
- Stay inside C:\Development\Engines\godot; you cannot write outside it.
- Log the change in workspace-editor-planning/DIVERGENCE-LEDGER.md (append a WP22 entry).
</action_safety>

<verification_loop>
1. Build: `scons platform=windows target=editor dev_build=yes -j4` (fallback extra_suffix=wp22).
2. Existing block_tool smoke assertions must pass UNMODIFIED — the current smoke's committed-AABB
   expectations must hold as-is. If you believe an existing assertion must change, STOP and say so
   in your report instead of editing it; deliberate amendments are the orchestrator's call.
3. EXTEND the block tool smoke with the new contract: a base drag whose screen travel stays inside
   one grid cell must (a) report overlay geometry present during the drag (`_level_block_overlay_has_geometry`),
   and (b) after height accept, commit a block whose base is exactly one snap_step by one snap_step.
   Also assert the sign rule: a small negative-direction drag yields the cell on the negative side.
4. Run the block tool smoke case from PowerShell (Git Bash may not start in this sandbox) with the
   pattern used by workspace-editor-planning/smoke/run_smoke.sh, and the kernel smokes:
   `bin/<console exe> --headless --path modules/level_kernel/tests/smoke_project --script smoke.gd`
   (plus transform_smoke.gd) to prove no kernel regressions.
5. Iterate until green.
</verification_loop>

<compact_output_contract>
Report: files touched with one-line summaries; the clamp helper's name and exact semantics
(sign rule, snap-disabled behavior); which smoke sections you added; verbatim final PASS/FAIL lines
of every smoke you ran; any existing assertion you believe needs amendment (flagged, not edited);
ledger entry text. No process dumps, no full logs.
</compact_output_contract>
