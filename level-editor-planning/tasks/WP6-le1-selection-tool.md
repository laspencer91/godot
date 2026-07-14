# WP6 — LE1 selection tool: modes, tiers, picking, highlights (implementation brief)

<task>
Implement the real Select tool for the level editor tab of this Godot 4.8 fork (repo root = this
workspace), replacing the LE0 placeholder. READ FIRST, in order:
level-editor-planning/tools/02-selection-picking.md (BINDING: the full pick pipeline §1, services
§2, chosen solutions §3 — geometric BVH picking, quad-gated loop/ring, generation-stamped handles,
marquee semantics), level-editor-planning/TOOL-FOUNDATIONS.md (§1 services S4/S5, §3 patterns),
level-editor-planning/PLAN.md (§2 UX contract). Existing code to build on: editor/level/
(LevelEditor service, LevelEditorView, LevelEditorTool base, ToolOverlay from WP2/WP4);
modules/level_kernel WP5 additions (LevelMeshAdjacency, element BVH ray_closest, stable handles,
LevelMeshDiff removed_*_handles) — read the WP5 report/API first; the fork's world-scoped gizmo
BVH (Node3DEditor::gizmo_bvh_ray_query / insert_gizmo_bvh_node, editor/scene/3d/node_3d_editor_plugin.cpp
— call it, do not modify it).

Deliverables (LE1 scope; leave clearly-marked seams for WP7):
1. `SelectionModel` owned by the level document layer (per tools/02 §2): per feature type an
   ordered set of stable kernel handles + active handle + current (mode, tier); apply(SelectionOp)
   with replace/add(Shift)/toggle(Ctrl)/subtract(Shift+Ctrl); revalidate(removed_handles) hooked
   to every diff apply/revert including undo/redo; selection changes are NEVER undo steps;
   emits selection_changed.
2. LevelBlock registration in the fork's gizmo BVH (insert on enter-world, update AABB on bake,
   remove on exit) so broad-phase pick returns this document's blocks only.
3. Select tool pick pipeline (tools/02 §1): ray from pane camera → gizmo BVH broad phase →
   per-block element BVH narrow phase (kernel-local ray) → element resolve by mode+tier.
   Modes/hotkeys while the level view is focused: 1/2/3 = polygroup-tier vert/edge/face,
   4 = object, 6 toggles polygroup↔triangle tier, Shift+Ctrl+1/2/3 = triangle tier. Vertex/edge
   resolve = screen-space nearest within px tolerance (verts 10px, edges 8px) with depth reject
   vs the narrow-phase surface hit + distance-scaled bias. Object mode selects the LevelBlock
   node and routes through EditorSelection for scene-tree interop.
4. Extended gestures: double-click = coplanar flood fill (kernel query); L = loop, X = ring from
   the active edge (kernel walks); Ctrl+A select all / Ctrl+I invert in current mode; marquee
   box-select (frustum from drag rect → gizmo_bvh_frustum_query broad phase; per-element
   vertex-center / both-endpoints / all-corners tests; x-ray by default per tools/02 §D).
   DEFERRABLE as warts if they threaten schedule (note them): Spacebar depth-cycling, path/paint
   drag select, grow/shrink, CapsLock plane-wide fill, enclose-vs-touch toggle.
5. Highlight overlay (S5): extend ToolOverlay (or a sibling overlay owned by the view) rendering
   selected verts (point/billboard handles), edges (PRIMITIVE_LINES), faces (translucent fill)
   from resolved handles; rebuilt only for dirty blocks on selection_changed/topology diffs;
   RenderingServer instances on the document world, create-detached → free-in-dtor, same
   discipline as the WP2 grid. Active element tinted differently. Marquee rectangle drawn as a
   2D overlay on the viewport container.
6. Toolbar/UX: Select button (already present) activates the tool; mode buttons or a compact
   mode indicator in the top strip showing current (mode, tier); Escape clears selection (via
   exit_gesture path), per the universal Enter/Escape contract.
7. Headless smoke `selection_smoke` following the block_tool_smoke pattern (addon +
   single-project entry + run_smoke.sh wiring): open testbed main.tscn as a level tab, create
   two blocks via the kernel API, then with a fixed camera drive synthetic mouse/key events:
   click-pick a known face (assert face handle + polygroup expansion), switch to edge/vertex
   modes and pick with tolerance (assert exact handles), Shift-add and Ctrl-toggle, marquee
   across one block (assert its elements selected, other block untouched), double-click flood
   fill, L/X on a box edge (assert termination per WP5 semantics), object-mode click selects the
   LevelBlock node, undo of a block creation revalidates selection (no stale handles, no
   crash). Print one OK marker; no error-class output beyond the known Vulkan loader warning.
</task>

<action_safety>
Allowed files: editor/level/** (new + edits), minimal edits to editor/editor_document.* or the
level document seam ONLY if SelectionModel storage requires it (flag it),
modules/level_kernel/** ONLY for small binding gaps blocking the tool (flag them), smoke files
under level-editor-planning/testbed/ + workspace-editor-planning/smoke/run_smoke.sh,
DIVERGENCE-LEDGER.md append for any shared-file touch. NEVER touch
editor/scene/3d/node_3d_editor_viewport.* — the gizmo BVH entry points live in
node_3d_editor_plugin.cpp and are consumed as-is. No git commits. Match editor/level/ style.
</action_safety>

<verification_loop>
Build `scons platform=windows target=editor dev_build=yes`; fix all errors/new warnings. If the
final link fails with "Access is denied" on bin/godot.windows.editor.dev.x86_64.exe, the USER'S
INTERACTIVE EDITOR holds the lock — NEVER kill or Stop-Process ANY godot process for ANY reason
(process metadata visible from the sandbox is unreliable; a "windowless" process may be the
user's editor). Wait ~60s and retry a few times; if still locked, build with
`scons platform=windows target=editor dev_build=yes extra_suffix=wp6` and run all verification
against bin/godot.windows.editor.dev.x86_64.wp6.console.exe instead (WP5 proved this works; a
.wp5-suffixed binary pair already exists in bin/ from that run). Run selection_smoke until green via
run_smoke.sh (console binary, absolute path as arg 1), then confirm zero regressions:
block_tool_smoke, level_tab_smoke, module kernel smoke. Done = clean build + selection_smoke
green + no regressions.
</verification_loop>

<compact_output_contract>
Final report: files created/modified (flag shared-file touches); build result; verbatim
selection_smoke output tail; deferred warts; deviations from tools/02 / TOOL-FOUNDATIONS with
one-line rationale each.
</compact_output_contract>
