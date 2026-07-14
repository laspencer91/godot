# WP4 — LE0 block drag-create tool (implementation brief)

<task>
Implement the Add Block tool in the level editor tab of this Godot 4.8 fork (repo root = this
workspace). READ FIRST, in order: level-editor-planning/tools/01-block-tool.md (the tool's own
plan — its chosen solutions are BINDING: frozen drag plane/basis at gesture start, ghost preview
architecturally unable to touch the kernel, has_volume guards + default_block_height fallback,
single exit_gesture() path), level-editor-planning/TOOL-FOUNDATIONS.md (§1 services S1/S2/S7,
§3 patterns), level-editor-planning/PLAN.md (§2 UX contract). Existing code to build on:
editor/level/ (LevelEditor service + LevelEditorView from WP2), modules/level_kernel
(LevelMesh.create_box / LevelMeshData / LevelBlock from WP1 — see
modules/level_kernel/tests/smoke_project/smoke.gd for the exact API).

Deliverables (LE0 scope):
1. Minimal modal-tool framework in editor/level/: `LevelEditorTool` base (activate/deactivate,
   handle_input(camera, event) -> handled, exit_gesture() single cleanup path, Enter/Escape
   convention) + `ToolOverlay` ghost-preview helper owning transient RenderingServer instances in
   the view's world (create-detached → free-in-dtor lifecycle, same discipline as the WP2 grid).
   LevelEditorView routes viewport input to the active tool; LevelEditor service owns which tool
   is active. Wire the WP2 toolbar buttons: Select (default, no-op for LE0) and Block (activates
   this tool); Shift+B activates Block while the level tab is focused (ED_SHORTCUT).
2. `BlockTool` implementing tools/01-block-tool.md's state machine: IDLE → PENDING (mouse down on
   ground plane y=0 of the document world) → BASE_DRAG (grid-snapped rectangle on the frozen
   plane/basis) → HEIGHT_DRAG (second stage sets height along the frozen plane normal) → COMMIT
   (LMB click / Enter). Escape cancels from any state via exit_gesture(). Grid snap = 1m step
   (read a `snap_step` from the LevelEditor service; default 1.0, no UI yet). Ghost preview =
   translucent box + wireframe in the ToolOverlay. Degenerate-drag guards per the tool plan:
   has_volume() checks; flat click-drag-release commits with default_block_height = 3.0m.
   DEFERRABLE as LE0 warts if they threaten the schedule: Ctrl camera-flip and Shift
   surface-snap placement (note them if deferred).
3. Commit path: create a `LevelBlock` node named "Block", child of the level document's scene
   root, owner set for persistence, with a LevelMeshData produced via LevelMesh
   begin_transaction/create_box(frame,size,active-material-index=0)/commit. Undo/redo through
   EditorUndoRedoManager against the document's undo history (creation undo = node add/remove —
   follow how existing editor code does undoable node creation with owner + reference-keeping).
   Undo must remove the node; redo restores it with identical mesh data.
4. Headless smoke `block_tool_smoke` following the WP2 level_tab_smoke pattern (addon + project
   entry + run_smoke.sh wiring): open testbed main.tscn as a level tab, activate BlockTool via
   the service API, drive the state machine programmatically (synthesize the mouse/key events or
   call the tool's staged methods directly — prefer synthetic InputEventMouse* through the real
   input path), commit a 4x4x3 block at a known position; assert: a LevelBlock child exists with
   expected LevelMeshData counts + AABB; undo → node gone; redo → node back with identical data
   arrays; save the scene to a temp path, reload it headless, assert the LevelBlock persisted
   with mesh + collision children rebuilt. Print one OK marker; no error-class output beyond the
   known environment Vulkan loader warning.
</task>

<action_safety>
Allowed files: editor/level/** (new tool files + edits to WP2's view/service), minimal edit to
modules/level_kernel ONLY if a small binding gap blocks the tool (flag it), smoke files under
level-editor-planning/testbed/ + workspace-editor-planning/smoke/run_smoke.sh, DIVERGENCE-LEDGER.md
append if any new shared file is touched. NEVER touch editor/scene/3d/node_3d_editor_viewport.*.
No git commits. Match the established editor/level/ style from WP2.
</action_safety>

<verification_loop>
Build `scons platform=windows target=editor dev_build=yes`; fix all errors/new warnings. Run
block_tool_smoke and level_tab_smoke via the run_smoke.sh single-project mechanism until green
(treat the environment Vulkan "Loader Message" warning as non-failing, consistent with existing
smokes). Confirm the WP1 kernel check still passes. Done = clean build + block_tool_smoke green +
no regression in level_tab_smoke or the kernel check.
</verification_loop>

<compact_output_contract>
Final report: files created/modified (flag shared-file touches); build result; verbatim
block_tool_smoke output tail; deferred warts; deviations from tools/01-block-tool.md with
one-line rationale each.
</compact_output_contract>
