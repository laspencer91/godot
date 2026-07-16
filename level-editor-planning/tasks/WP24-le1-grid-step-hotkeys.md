<task>
WP24 — Grid snap-step hotkeys: `+` grows the grid, `-` shrinks it.

Repo: C:\Development\Engines\godot (custom Godot fork, master). Files: editor/level/level_editor.*,
editor/level/level_editor_view.* (wherever viewport-level key handling and the snap UI live).

Contract:
1. While a level editor view has focus (same key-routing altitude as the existing 1/2/3/4 mode keys
   and tool hotkeys — must NOT fire while a text field has focus), `+` doubles the snap step and
   `-` halves it. Accept all natural keys: KEY_EQUAL (the unshifted `+=` key), KEY_PLUS,
   KEY_KP_ADD for grow; KEY_MINUS, KEY_KP_SUBTRACT for shrink. No modifiers (plain press only);
   ignore when Ctrl/Alt held so future combos stay free.
2. Step ladder: powers of two on the existing `LevelEditor::set_snap_step` value, clamped to
   [0.0625, 16.0] meters. If the current step is off-ladder (user typed 0.3 in the UI), snap to the
   nearest ladder rung in the pressed direction first.
3. Every consumer updates live: the snap UI control in the top strip reflects the new value, the
   block tool idle hover footprint resizes on the next mouse move, and in-flight block gestures keep
   their frozen `gesture_snap_step` (do not mutate an active gesture — that freeze is deliberate).
4. Show the new step briefly in the existing status/toast path used by texture ops
   (`_show_texture_status`-style) or the equivalent view-level status label, e.g. "Grid: 0.5 m".
5. Keep it at one altitude: the ladder logic lives on LevelEditor (a `step_snap_step(bool p_grow)`
   or similar), the view only routes keys.

Out of scope: per-axis grids, grid rendering changes.
Do NOT touch editor/scene/3d/node_3d_editor_viewport.* (user's WIP) or editor/gui/document_view.cpp.
</task>

<action_safety>
- NEVER kill, Stop-Process, or taskkill ANY godot process — orphaned editors are for the human. On
  "Access is denied" exe-lock build failures (often masked by a cosmetic methods.py AttributeError),
  fall back to `extra_suffix=wp24`.
- Never run two scons builds concurrently in this tree.
- No git commits.
- Append a WP24 entry to workspace-editor-planning/DIVERGENCE-LEDGER.md.
</action_safety>

<verification_loop>
1. Build: `scons platform=windows target=editor dev_build=yes -j4` (fallback extra_suffix=wp24).
2. Existing smoke assertions pass UNMODIFIED; if one must change, STOP and flag it in your report
   instead of editing it.
3. Extend an appropriate editor smoke: simulate `-` twice from the 1.0 default -> get_snap_step()
   == 0.25; `+` three times -> 2.0; clamp check at both ends; off-ladder 0.3 + `+` -> 0.5.
4. Run the touched editor smoke cases from PowerShell (Git Bash may not start in this sandbox) and
   the kernel smoke (`--headless --path modules/level_kernel/tests/smoke_project --script smoke.gd`)
   to prove no regressions. Known pre-existing failure unrelated to you: scene_tree_drag (user's
   inspector-lock WIP) — ignore it.
5. Iterate until green.
</verification_loop>

<compact_output_contract>
Report: files touched with one-line summaries; exact keycodes handled and the focus/modifier
guards; the ladder/clamp semantics; new smoke sections; verbatim final PASS/FAIL lines of every
smoke you ran; ledger entry text. No process dumps, no full logs.
</compact_output_contract>
