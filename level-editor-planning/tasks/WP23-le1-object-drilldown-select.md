<task>
WP23 — Select tool: object-first drill-down selection (Scythe/Hammer style).

Repo: C:\Development\Engines\godot (custom Godot fork, master). Files: editor/level/select_tool.*,
editor/level/selection_model.*, editor/level/level_editor_view.* as needed.

Design change (user-ordered; supersedes the flat click contract of tools/02-selection-picking.md §
click routing — record the amendment in the ledger):

Today, in component modes (1=vertex, 2=edge, 3=face), the first click on any block immediately picks
a component. The user wants object-first drill-down:

1. Clicking a block that is NOT currently object-selected selects the OBJECT (the LevelBlock node,
   routed through the existing `_apply_object` -> document EditorSelection path), regardless of the
   current component mode. This gives whole-object move/rotate via the existing object drag +
   gizmo paths, and node deletion via the standard scene-tree/Del flow, using Godot's native node
   transform machinery already wired for MODE_OBJECT.
2. Clicking on a block that IS currently object-selected drills in: resolve the component under the
   cursor per the current mode/tier (existing `_resolve_vertex/_resolve_edge/_resolve_face` paths,
   including double-click flood/edge-walk behaviors).
3. Clicking a DIFFERENT block while drilled in (or while another object is selected) object-selects
   the new block completely (OP_REPLACE), clearing the component selection. Shift/Ctrl modifier
   operations keep their existing add/toggle/subtract meaning at whichever level the click lands.
4. Click on empty space: existing miss behavior (clear at the current level). Escape steps back out
   one level: component selection -> object selection -> none.
5. The engagement signal is the document's EditorSelection: a block counts as "object-selected" iff
   it is in EditorSelection. Component picks are only resolvable on engaged blocks.
6. Mode 4 (explicit object mode) keeps its current behavior unchanged. Marquee behavior stays as-is
   in all modes (do not rework marquee this WP). Texture ops (Shift+T/Lift/Wrap/Flow/Shift+H/F) and
   the transform gizmo must keep working for both object selections and drilled component
   selections exactly as they do now.
7. Visual feedback: when an object is selected but not drilled in, the existing object-selection
   highlight (EditorSelection box or the selection_highlight_overlay object path) must make it
   obvious. No new art; reuse what MODE_OBJECT shows today.

Out of scope: paint select, depth cycling, grow/shrink, loop cuts, scale gizmo.
Do NOT touch editor/scene/3d/node_3d_editor_viewport.* (user's WIP) or editor/gui/document_view.cpp.
</task>

<action_safety>
- NEVER kill, Stop-Process, or taskkill ANY godot process for any reason — orphaned editors are for
  the human. If a build fails with "Access is denied" on the output exe (often masked by a cosmetic
  methods.py AttributeError), fall back to `extra_suffix=wp23`.
- Never run two scons builds concurrently in this tree.
- No git commits.
- Log the change + the tools/02 click-contract amendment in
  workspace-editor-planning/DIVERGENCE-LEDGER.md (WP23 entry).
</action_safety>

<verification_loop>
1. Build: `scons platform=windows target=editor dev_build=yes -j4` (fallback extra_suffix=wp23).
2. SMOKE AMENDMENT PRE-AUTHORIZATION: this WP deliberately changes the first-click contract, so
   existing selection/transform/gizmo smoke sequences that click a block expecting an immediate
   component pick WILL need one extra engaging click (or an explicit object-select step) inserted
   at the start of their gestures. That amendment is pre-authorized — but ONLY that shape of
   amendment: add engagement steps; do not weaken or delete any assertion about what ends up
   selected/transformed. List every amended case in your report with a one-line justification.
3. Extend the selection smoke with the drill-down contract itself: (a) first click on a block in
   face mode -> EditorSelection contains the block, no face selected; (b) second click on the same
   block -> face under cursor selected; (c) click on a second block -> second block object-selected,
   component selection empty; (d) Escape from drilled state -> component cleared, object still
   selected; Escape again -> nothing selected.
4. Run the full editor suite cases relevant to selection/transform/gizmo from PowerShell (Git Bash
   may not start in this sandbox), plus kernel smokes:
   `bin/<console exe> --headless --path modules/level_kernel/tests/smoke_project --script smoke.gd`
   and transform_smoke.gd. Known pre-existing failures unrelated to you: scene_tree_drag
   ("Locked Inspector followed a Scene Tree selection change") is the user's WIP — ignore it.
5. Iterate until green (minus the known WIP failure above).
</verification_loop>

<compact_output_contract>
Report: files touched with one-line summaries; the exact engagement rule as implemented; every
amended smoke case + justification; new smoke sections added; verbatim final PASS/FAIL lines of
every smoke you ran; ledger entry text. No process dumps, no full logs.
</compact_output_contract>
