# WP8 — LE1 cleanup: reuse/simplification/efficiency/altitude fixes (implementation brief)

<task>
Apply the itemized cleanup below to the level editor code of this Godot 4.8 fork (repo root =
this workspace). This is a BEHAVIOR-PRESERVING refactor pass over
modules/level_kernel/** and editor/level/** — no new features, no gesture changes. Every item
was pre-reviewed; do NOT hunt for additional issues. If an item turns out to be wrong about the
code, skip it and note why. The existing headless smokes are the behavior contract: they must
pass UNMODIFIED (if a fix seems to require editing a smoke assertion, the fix changed behavior —
reconsider or skip; the ONLY permitted smoke edits are additive assertions).

Items, in execution order (kernel first):

K1. Shared bakeable predicate: `_face_is_bakeable` is duplicated verbatim in
level_mesh_baker.cpp:12 and level_mesh_element_bvh.cpp:14. Move to a single
`bool LevelMeshData::face_is_bakeable(int p_face_id) const` (non-emitting); both consume it.

K2. Face-local triangle identity: the element BVH leaf carries (tri_id, face_id) where tri_id is
a GLOBAL fan-triangle counter; the editor (SelectionModel::global_to_local_triangle,
selection_model.cpp:564, and get_face_triangle_count:521) re-derives the global→local mapping by
re-scanning all faces with a DIFFERENT validity predicate — a latent divergence. Fix at the
kernel: store the face-local fan index in the BVH leaf, add `"local_tri"` to the Dictionary
returned by ray_closest (keep "tri_id" for compatibility), thread it through the editor's
SurfaceHit, and delete SelectionModel::global_to_local_triangle plus every editor consumer of
the global decode. Triangle-tier picking resolves via local_tri.

K3. Kernel-owned fan triangulation for the editor: add
`int LevelMesh::get_face_triangle_count(int p_face_id) const` and
`PackedInt32Array LevelMesh::get_face_triangle_vertex_ids(int p_face_id, int p_local_tri) const`
(kernel loop order fan {0, t+1, t+2}, face validated ONCE). Reimplement
SelectionModel::get_face_triangle_count / get_triangle_vertex_ids on top of these (or replace
their call sites outright), and make SelectionHighlightOverlay::_append_face_triangles fetch the
needed arrays once per face instead of per triangle (it currently re-validates every corner per
triangle and re-fetches vertex_positions per triangle — selection_highlight_overlay.cpp:78-102).

K4. Handle packing single-sourced: the (slot, generation)→int64 encoding exists in
LevelMesh::_pack_handle (level_mesh.cpp:68), level_mesh_diff.cpp:13 (pack_element_handle +
slot_generation), and the resolve side. Define one static inline pack/resolve pair in
level_mesh_data.h next to the generation columns; all three sites consume it.

K5. Delete LevelMesh::_append_face (level_mesh.cpp:127): create_box calls
`_append_quad_face(face_vertices, p_material_index, polygroup_id++, LevelMeshData::FACE_FLAG_TEXTURE_LOCK)`
(level_mesh_operations.cpp:117) — byte-identical behavior.

K6. Created-element contract made explicit: select_tool_transform.cpp:274 and :522 interpret
LevelMeshDiff::get_revert_removed_*_handles() as "elements the op created" by convention. Add
first-class `get_created_vertex_handles/get_created_edge_handles/get_created_face_handles()` to
LevelMeshDiff (today forwarding to the revert_removed arrays, with a comment that the diff
representation may narrow later and this accessor is the stable contract). Editor uses them.

K7. Polygroup membership as a kernel query: add
`PackedInt32Array LevelMeshAdjacency::get_polygroup_faces(int p_seed_face_id) const`; replace the
inline alive/polygroup-id scans in select_tool.cpp:_resolve_face (~line 237) and
_apply_marquee face tier (~line 1051), and use it inside get_polygroup_boundary_edges. While
there: classify_region_rim currently iterates EVERY edge of the mesh (level_mesh_adjacency.cpp
around :469); make it iterate only the union of face_edges of the region faces — identical
results, O(region) not O(mesh).

K8. LevelMeshAdjacency::_to_packed (level_mesh_adjacency.cpp:18) element-copies Vector<int> →
PackedInt32Array; these are the same underlying type, so `return p_values;` is an O(1) COW share.

K9. UV transform pack/unpack exists twice (LevelMesh::_read/_write_uv_transform level_mesh.cpp:35
vs LevelMeshData::get/set_face_uv_transform level_mesh_data.cpp:256). Keep one static
non-emitting pair on LevelMeshData; the emitting public accessors and LevelMesh both call it.

K10. level_mesh.cpp apply_diff/revert_diff (:358-384) are the same function modulo
after_data/before_data and the emitted bool → one private `_restore_diff_state(diff, reverted)`.

K11. level_mesh_operations.cpp: (a) remove the redundant second `transaction_changed = true`
in extrude_faces (:635) and extrude_boundary_edges (:787) — begin already set it; (b) in
begin_transform_preview (:426-433) restructure so packed-contains is tested once:
`if (contains) continue; if (invalid) return false;`.

K12. Kernel face normal for the editor: select_tool_transform.cpp:245-251 derives the extrude
axis from the first three corners inline (fails on a degenerate first triple; the adjacency's
_get_face_plane already fan-scans robustly). Expose
`Vector3 LevelMesh::get_face_normal(int p_face_id) const` (backed by the adjacency plane logic)
and use it for the extrude/push-pull axis.

E1. Canonical LevelMesh per block: SelectionModel::resolve (selection_model.cpp:462) creates a
throwaway LevelMesh (3 heap allocations + signal wiring + generation-column pass) PER ELEMENT PER
OVERLAY REBUILD PER DRAG FRAME, and SelectTool::_query_surface_hits + _resolve_face/_resolve_edge
/_resolve_vertex do the same per click, so the BVH/adjacency caches are rebuilt from scratch on
every press AND release. Add a lazily-created persistent `Ref<LevelMesh> LevelBlock::get_level_mesh()`
(bound to the block's current data; rebind if the block's data ref is swapped) and use it
everywhere the editor currently instantiates a temp LevelMesh over block->get_data()
(selection_model.cpp, select_tool.cpp, select_tool_transform.cpp). The mesh's dirty-flagged
caches then persist across queries. Keep kernel semantics unchanged.

E2. Debug metadata off the per-frame path: LevelEditorView::_sync_selection_metadata
(level_editor_view.cpp:330) rebuilds get_debug_entries() (a Dictionary per selected element) on
EVERY selection_changed, which also fires per dirty-block bake during drags. Add a monotonically
increasing `get_revision()` to SelectionModel bumped only when selection CONTENT actually changes
(apply/revalidate mutations, not mark_block_dirty); the view caches the last revision and skips
the debug-entry rebuild when unchanged.

E3. Single-pass baker: LevelMeshBaker::bake (level_mesh_baker.cpp:52-96) scans all faces once to
collect material indices then re-scans ALL faces per material, re-running the bakeable check per
(material, face) pair — and this runs per mouse-motion during preview drags. Rewrite as one pass
over faces bucketing by material (preserve the current first-seen material→surface ORDER exactly;
the smoke counts triangles per surface), validating each face once.

S1. _select_edge_walk (select_tool.cpp:532): the walk results are already unique (kernel visited
set), and the polygroup lift is gone — delete the Vector+dedup-lambda copy; iterate `walked`
directly.

S2. Delete dead code: SelectionModel::clear_feature (selection_model.cpp:395, zero callers,
unbound) and the SelectTool::_activate override (select_tool.cpp:1065 — set_mode_and_tier
early-returns on unchanged mode/tier, so it is a guaranteed no-op).

S3. Rollback/capture helpers in select_tool_transform.cpp: five near-identical
revert-topology-diffs-in-reverse loops (:266, :283, :320, :494, :561) → one
`_revert_topology_diffs()` doing a full-range reverse walk (null topology_diff entries are
skipped, which covers all five variants); three identical capture-original-positions loops
(:312, :633, :658) → `MeshDragState::capture_original_positions()`.

S4. Replace hand-rolled membership loops with Vector::has()/find() at: level_mesh_operations.cpp
packed_contains (:17, keep the helper name or inline), select_tool_transform.cpp packed_has (:24)
and element_is_selected (:33 → p_selected.has(p_element)), selection_model.cpp _contains (:18 →
find) and _append_dirty (:30), level_mesh_data.cpp _append_free_id (:131), level_mesh_baker.cpp
:58 (find()==-1 → !has()). Replace the six identical append-unique lambdas in select_tool.cpp
(append_unique :481/:588/:894, append_edge_id :549/:628/:931) with one file-local helper.

S5. LevelSnapService::snap_delta and snap_point_absolute (level_snap_service.cpp:25-41) are
byte-identical hand-rolled per-component snapping → each body becomes
`return p_v.snappedf(p_step);` (keep both methods — the names carry the delta-vs-point semantic).
Also delete make_affine (level_mesh_operations.cpp:26) — it wraps the existing
`Transform2D(xx, xy, yx, yy, ox, oy)` constructor (mind the argument order).

A1. Remove the second winding flip: SelectionHighlightOverlay::_append_triangle
(selection_highlight_overlay.cpp:89-93) reverses triangle order "to match the baker", but the
overlay materials are CULL_DISABLED so the flip is dead policy AND violates the invariant that
ONLY the baker converts winding at the bake seam. Emit kernel CCW order.

A2. Fix the misleading comments on the `is_empty()` fallback branches in _collect_all
(select_tool.cpp:~647) and _apply_marquee (:~950): their PRIMARY live role is the TIER_TRIANGLE
path (the polygroup block above is skipped for that tier); the closed-polygroup case is
unreachable today. Reword; do not change the code.

A3. Centralize editor policy constants: default snap step fallback (`LevelEditor::get_singleton()
? ...get_snap_step() : 1.0` appears 4× across select_tool_transform.cpp/block_tool.cpp), default
block height 3.0 (block_tool.cpp:29 and :252), and major-grid multiple 4 (level_editor_view.cpp
:60, :109-111, and the "Grid: 1 m / major 4 m" label :431). Define once (LevelEditor statics or
LevelSnapService), add `LevelEditor::snap_step_or_default()`, derive the grid label from the
constants.
</task>

<action_safety>
Allowed files: modules/level_kernel/** and editor/level/** only (plus additive-only smoke
assertions if you choose to lock in local_tri behavior). NEVER touch
editor/scene/3d/node_3d_editor_viewport.*. No git commits. Match existing style. Public API may
GROW (new methods listed above) but existing bound signatures must not change or disappear from
ClassDB (keep "tri_id" in ray_closest results). Explicitly out of scope (pre-decided skips, do
not do): swapping the element BVH for core TriangleMesh; HashSet-backed SelectionModel::apply /
_collect_all rewrites; caching texture-lock before-bases across preview frames; sharing the
BOX_* tables with tool_overlay; a kernel closed-vs-invalid polygroup status enum.
</action_safety>

<verification_loop>
Build `scons platform=windows target=editor dev_build=yes -j4`; fix all errors/new warnings. If
the final link fails with "Access is denied" on bin/godot.windows.editor.dev.x86_64.exe, the
USER'S INTERACTIVE EDITOR holds the lock — NEVER kill or Stop-Process ANY godot process for ANY
reason (process metadata visible from the sandbox is unreliable). Wait ~60s and retry a few
times; if still locked, build with `scons platform=windows target=editor dev_build=yes
extra_suffix=wp8 -j4` and verify against bin/godot.windows.editor.dev.x86_64.wp8.console.exe.
Then run, all green with UNMODIFIED assertions: the module smokes
(--headless --path modules/level_kernel/tests/smoke_project --script smoke.gd, and --script
transform_smoke.gd) and the editor cases (block_tool_smoke, level_tab_smoke, selection_smoke,
transform_smoke) via workspace-editor-planning/smoke/run_smoke.sh or the equivalent direct
staging you used in WP6/WP7. A smoke that needs an assertion EDIT to pass means behavior changed
— revert that item and note it.
</verification_loop>

<compact_output_contract>
Final report: per item K1..A3 — done / skipped(reason); files touched; build result; verbatim
tails of the module smoke and transform_smoke; any behavior deltas you believe exist (should be
none beyond E2's debug-metadata gating).
</compact_output_contract>
