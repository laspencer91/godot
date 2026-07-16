<task>
WP30 — Repair the editor smoke suite to green in BOTH GUI and headless modes.

Repo: C:\Development\Engines\godot (custom Godot fork, master). Current dev binary
bin/godot.windows.editor.dev.x86_64.exe (built 17:14 today) is current with the tree.

Situation (bisect evidence, three suite runs on that binary):
- GUI mode (default `run_smoke.sh`): six cases FAIL consistently across two runs — block_tool,
  selection, transform, material_browser, face_texture, fast_texture. Errors are of the class
  "Face selection did not resolve through the pane input route", "Vertex selection did not
  resolve", "staged viewport gesture did not create a LevelBlock", "Shift+Alt+3 did not set
  procedural blockout slot 3". IMPORTANT VARIABILITY: selection_smoke failed at the FIRST face
  click in run 1 but got PAST it in run 2 and failed at the later vertex pick — the failure point
  moves between runs, i.e. a timing/layout race, not a deterministic logic bug.
- Headless mode (`SMOKE_HEADLESS=1`): those five selection/texture cases all PASS. Only
  block_tool FAILS, and with a DIFFERENT error: line ~65
  "BLOCK_TOOL_SMOKE: Ground-hover probe is outside the level viewport."

Recent history you need (all uncommitted, see workspace-editor-planning/DIVERGENCE-LEDGER.md):
1. "G-Level LE2 material UX / WP22 (2026-07-15)" — a large refactor landed today ~13:45-17:02:
   per-document material state, the material browser as a bottom shelf under the viewport
   (108 px default), UV/Hotspot commands in a fixed-width selection-aware LEFT context panel in
   LevelEditorView, and tool mode routed to the active/owning Level view via
   `LevelEditor::_get_active_level_document()` (= EditorNode's active document must be the level
   document) instead of broadcast. Its own final validation ran HEADLESS ONLY (task file
   level-editor-planning/tasks/WP22-le2-material-ux.md records SMOKE: PASS with SMOKE_HEADLESS=1).
   GUI mode was never re-validated after the layout/routing changes. DO NOT revert this refactor —
   fix forward.
2. "G-Level WP22 Block-tool minimum-cell base (2026-07-15)" — a separate block-tool change
   (editor/level/block_tool.{h,cpp} + a new within-one-cell section in
   level-editor-planning/testbed/addons/block_tool_smoke/plugin.gd). It was verified in GUI-style
   staged runs only; its new smoke section trips "Ground-hover probe is outside the level
   viewport" headless.

Diagnostic leads (verify, don't assume):
- Smoke plugins compute click coordinates as
  `container.get_global_transform_with_canvas() * camera.unproject_position(world)` and push them
  into `container.get_viewport()`. The new left context panel is SELECTION-AWARE — if it
  rebuilds/reflows after a selection change, the viewport container shifts between the coordinate
  computation and the click landing (positions are computed fresh per click but a reflow triggered
  by the PREVIOUS click may apply a frame later). That mechanism fits the moving failure point.
- The pane input route requires the level document to be the ACTIVE document; GUI-mode
  focus/activation ordering may differ from headless.
- The headless block_tool probe failure is location math: the new smoke section's probe position
  lands outside the (differently laid out) headless viewport.

Required outcome:
1. ROOT-CAUSE FIRST, then fix at the right altitude:
   - If real editor behavior is broken in GUI mode (e.g. a click can genuinely miss because the
     context panel reflows the viewport a frame late, or tool-mode routing fails when focus is
     elsewhere), fix the EDITOR (defer reflow to layout-safe points, stabilize the container rect,
     make routing robust) — that is a user-facing bug, not a harness problem.
   - If it is purely synthetic-input fragility (push_input bypassing activation that real OS input
     establishes), harden the SMOKE HARNESS/plugins (settle-and-reverify layout before gestures,
     re-derive coordinates after selection-triggered reflow, assert active-document state before
     input). Do not weaken or delete any assertion about outcomes; adding robustness waits/
     re-derivation is fine and pre-authorized.
   - The block_tool headless probe: make the WP22 smoke section layout-independent (derive the
     probe from the ACTUAL container rect, not assumed geometry).
2. Full suite green in BOTH modes, and state in your report which fixes were editor-side vs
   harness-side and why.

Do NOT touch editor/scene/3d/node_3d_editor_viewport.* (user's WIP). editor/gui/document_view.cpp
belongs to the material-UX surface — touch it only if the root cause genuinely lives there, with a
precise ledger note.
</task>

<action_safety>
- NEVER kill, Stop-Process, or taskkill ANY godot process — orphaned editors are for the human. On
  "Access is denied" exe-lock build failures (often masked by a cosmetic methods.py
  AttributeError), fall back to `extra_suffix=wp30`.
- Never run two scons builds concurrently in this tree; never run two smoke suites concurrently.
- No git commits.
- Append a WP30 entry to workspace-editor-planning/DIVERGENCE-LEDGER.md distinguishing editor-side
  fixes from harness-side hardening.
</action_safety>

<verification_loop>
1. If you changed C++: `scons platform=windows target=editor dev_build=yes -j4` (fallback
   extra_suffix=wp30).
2. Run the previously-failing cases in BOTH modes (from PowerShell; stage projects the way
   run_smoke.sh does — Git Bash may not start in this sandbox). Then a full sweep in both modes.
3. Because the GUI failures are timing-variable, run the GUI suite TWICE — a single green GUI run
   does not prove the race is fixed.
4. Kernel smokes (smoke.gd, transform_smoke.gd) to prove no kernel regressions.
5. Iterate until: GUI x2 green, headless x1 green (known allowance: none — scene_tree_drag now
   passes and stays required).
</verification_loop>

<compact_output_contract>
Report: the root cause per failing case (editor bug vs harness fragility, with the exact
mechanism); files touched with one-line summaries; every smoke edit and why it does not weaken
outcome assertions; verbatim final PASS/FAIL summary lines of each suite run (GUI run 1, GUI run
2, headless); ledger entry text. No process dumps, no full logs.
</compact_output_contract>
