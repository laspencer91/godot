# WP7 — LE1 transform + extrude family: move, nudge, extrude, push/pull, duplicate (implementation brief)

<task>
Implement the LE1 edit operations for the level editor of this Godot 4.8 fork (repo root = this
workspace). LE1 is MOVE-ONLY: no rotate, no scale gizmos. READ FIRST, in order:
level-editor-planning/tools/03-transform-snapping.md (BINDING: snap-the-DELTA rule §*, preview/
commit lifecycle, texture-lock affine solve), level-editor-planning/tools/04-extrude-family.md
(BINDING: rim rule, local wall winding, push/pull as topology-inert moves, attribute inheritance),
level-editor-planning/TOOL-FOUNDATIONS.md (§1 services S6/S7). Existing code to build on —
read before writing anything:
- modules/level_kernel: LevelMesh transaction API (begin_transaction/commit→LevelMeshDiff/rollback),
  landed WP5 queries you MUST consume rather than re-derive:
  `Ref<LevelMeshAdjacency> LevelMesh::get_adjacency()` with `walk_edge_loop/walk_edge_ring/
  coplanar_flood_fill/faces_on_plane/classify_region_rim(PackedInt32Array face_ids)→Dictionary/
  get_face_edges/get_edge_faces/get_edge_vertices/find_edge`,
  `LevelMesh::ray_closest(local_origin, local_dir)→Dictionary{hit,t,face_id,tri_id,barycentric}`,
  bulk accessors `get_face_corner_vertex_ids/get_face_corner_positions/
  get_face_boundary_edge_ids(face_id, polygroup_tier)/get_face_boundary_edge_positions`,
  stable handles `make_vertex/edge/face_handle(id)→int64` / `resolve_vertex/edge/face(handle)→id or -1`,
  `LevelMeshDiff::get_removed_*_handles()/get_revert_removed_*_handles()/touches_topology()/
  touches_geometry()`.
- editor/level: the WP6 Select tool, SelectionModel, and overlay (read the code first —
  transform gestures start FROM the selection and must keep SelectionModel valid through every
  diff). WP6 landing notes: SelectionModel lives on LevelDocument (selection_model.{h,cpp});
  SelectTool (select_tool.{h,cpp}) owns screen-space element resolution privately; highlights
  are per-block persistent meshes (selection_highlight_overlay.{h,cpp}) rebuilt dirty-block-only;
  triangle-tier elements are keyed by parent-face handle + face-local triangle/corner identity
  (no kernel triangle slots); a marked WP7 handoff seam exists for cross-tool selection
  fallback — BlockTool keeps exclusive ownership of its modal drag input, so route transform
  drags through the Select tool / the seam, not BlockTool.

Deliverables:
1. Kernel transform ops (modules/level_kernel, on LevelMesh alongside existing API):
   preview/commit lifecycle per tools/03 — `begin_transform_preview(PackedInt32Array vertex_ids)`,
   `preview_transform_vertices(PackedVector3Array new_positions)` (repeatable, cheap, geometry-only:
   updates positions + marks BVH dirty, no diff), `commit_transform_preview()` → exactly ONE
   LevelMeshDiff for the whole gesture, `cancel_transform_preview()` → restores pre-gesture
   positions exactly. Face UVs follow texture-lock: solve the per-face affine UV update in the
   face tangent basis (rigid closed-form for translations/rigid motions, least-squares for the
   general case) per tools/03 — NOT a dominant-world-axis approximation. Expose a face-level
   `texture_lock` toggle default ON.
2. Kernel extrude family (modules/level_kernel), all transactional (one diff each):
   - `extrude_faces(PackedInt32Array face_ids)`: consume adjacency.classify_region_rim; reject the
     whole op on its ambiguous/non-manifold signal; rim edges get side walls whose winding is
     derived purely locally from the owning face's loop order (kernel loops stay CCW-outward; the
     baker already flips at the bake seam — do not double-flip); side walls inherit
     material/UV attributes from the single rim-owning face and receive FRESH polygroup ids;
     interior edges move with the cap. Zero-distance extrude then position updates via the
     preview lifecycle is the intended gesture composition.
   - `push_pull_faces(PackedInt32Array face_ids, real_t distance)`: topology-inert — pure vertex
     moves along the selected faces' normals (average at shared verts), per tools/04.
   - `extrude_boundary_edges(PackedInt32Array edge_ids)`: boundary edges only (edge with exactly
     one alive adjacent face); reject interior edges; new quad per edge, winding from the owning
     face, attributes inherited from it.
3. Editor-side move tool + gestures (editor/level, extending the WP6 Select tool per PLAN §2):
   - Drag-move of the current selection at every tier (verts/edges/faces/objects). Motion solved
     in world space; SNAP THE DELTA, never absolute points, never coupled to the Local/World
     basis toggle (tools/03's UE5 bug class). Grid-snap uses the level editor's existing grid
     step setting; Ctrl temporarily inverts snap on/off during the drag. Preview via
     preview_transform_vertices each frame; mouse-up commits → one undo step; Escape mid-drag
     cancels cleanly (cancel_transform_preview, no diff, no undo entry).
   - Object mode move = LevelBlock node transform change through the standard editor undo/redo.
   - Shift+drag on a face selection = extrude_faces then drive the new cap through the preview
     lifecycle along the face normal; Shift+Ctrl+drag = push_pull_faces preview. Shift+drag from
     a boundary-edge selection = extrude_boundary_edges + preview.
   - Nudge: arrow keys (and PgUp/PgDn for the view-plane normal axis) move the selection by one
     grid step per press; each press = one committed diff/undo step.
   - Vertices-to-grid command: snap every selected vertex to the nearest grid point (one diff).
   - Duplicate: Ctrl+D — object tier duplicates the LevelBlock node(s) (standard editor undo);
     sub-object tiers may defer to LE2 as a wart if kernel face duplication is out of scope —
     note it if so.
   - SelectionModel stays valid across every commit/undo/redo (revalidate via diff handle arrays);
     newly created extrude walls become the selection where tools/04 says so (cap faces stay
     selected after extrude).
4. Headless smoke `transform_smoke` (module smoke extension AND/OR a run_smoke.sh case following
   the selection_smoke pattern — match whichever WP6 landed): kernel level — preview/commit
   produces exactly one diff and cancel restores positions bit-exact; extrude of one box face
   adds 4 walls with correct local winding (assert each wall normal points away from the box
   volume via kernel CCW convention), fresh polygroup ids, cap still selected handle-wise;
   extrude of an ambiguous non-manifold set rejects atomically (no partial mutation);
   push_pull moves only positions (topology hash unchanged); boundary-edge extrude on an interior
   edge rejects. Editor level (if the WP6 smoke harness supports synthetic input): drag-move a
   vertex with snap → position lands on grid; Escape mid-drag leaves mesh untouched and no undo
   entry; undo after extrude restores topology and SelectionModel has no stale handles. Print one
   OK marker; no error-class output beyond the known Vulkan loader warning.
</task>

<action_safety>
Allowed files: modules/level_kernel/** (new + edits alongside, no changes to existing public
signatures), editor/level/** (new + edits), smoke files under level-editor-planning/testbed/ and
modules/level_kernel/tests/smoke_project/ plus workspace-editor-planning/smoke/run_smoke.sh
wiring, DIVERGENCE-LEDGER.md append for any shared-file touch outside those trees (there should
be none). NEVER touch editor/scene/3d/node_3d_editor_viewport.* — the gizmo BVH entry points in
editor/scene/3d/node_3d_editor_plugin.cpp are consumed as-is. No git commits. Match existing
module style (columnar arrays, defensive bounds discipline) and editor/level style. Kernel loops
are CCW-outward; the baker flips winding at the bake seam — kernel-side geometry code must reason
in kernel order only.
</action_safety>

<verification_loop>
Build `scons platform=windows target=editor dev_build=yes`; fix all errors/new warnings. If the
final link fails with "Access is denied" on bin/godot.windows.editor.dev.x86_64.exe, the USER'S
INTERACTIVE EDITOR holds the lock — NEVER kill or Stop-Process ANY godot process for ANY reason
(process metadata visible from the sandbox is unreliable; a "windowless" process may be the
user's editor). Wait ~60s and retry a few times; if still locked, build with
`scons platform=windows target=editor dev_build=yes extra_suffix=wp7 -j4` and run all
verification against bin/godot.windows.editor.dev.x86_64.wp7.console.exe instead (WP5/WP6 both
proved this works; .wp5/.wp6-suffixed binary pairs already exist in bin/). Run the module
smoke (bin/godot.windows.editor.dev.x86_64.console.exe --headless --path
modules/level_kernel/tests/smoke_project --script smoke.gd) and transform_smoke until green,
then confirm zero regressions: selection_smoke, block_tool_smoke, level_tab_smoke via
workspace-editor-planning/smoke/run_smoke.sh (console binary, absolute path as arg 1). Done =
clean build + transform_smoke green + no regressions.
</verification_loop>

<compact_output_contract>
Final report: files created/modified (flag shared-file touches); the exact script-visible API
added (method signatures); build result; verbatim transform_smoke output tail; deferred warts
(e.g. sub-object duplicate); deviations from tools/03 / tools/04 / TOOL-FOUNDATIONS with one-line
rationale each.
</compact_output_contract>
