# WP5 — LE1 kernel foundation: adjacency, element BVH, stable handles (implementation brief)

<task>
Implement the LE1 kernel-side query foundation in modules/level_kernel of this Godot 4.8 fork
(repo root = this workspace). READ FIRST, in order: level-editor-planning/TOOL-FOUNDATIONS.md
(§1 services S3/S4 — their chosen solutions are BINDING), level-editor-planning/tools/02-selection-picking.md
(§2 foundation services, §3.B loop/ring walk rules, §3.C generation-stamped handles),
level-editor-planning/PLAN.md (§1 KERNEL layer, risk 2). Existing code:
modules/level_kernel (LevelMeshData columnar arrays, LevelMesh transaction API, LevelMeshDiff,
LevelMeshBaker — see modules/level_kernel/tests/smoke_project/smoke.gd for the exact API).
This WP is pure kernel + tests: NO editor/level/ changes, no UI. WP6 (selection tool) and WP7
(transform/extrude) build directly on these APIs, so signatures are the contract.

Deliverables:
1. Adjacency service (S3): a lazily-built, cached `LevelMeshAdjacency` owned by LevelMesh,
   invalidated wholesale on any diff apply/revert/commit that touches topology. Provides:
   edge→faces (keyed by unordered vertex pair), vert→edges, vert→faces, face→edges in LOOP ORDER.
   On top of it: quad-gated edge LOOP walk and edge RING walk exactly per tools/02 §3.B
   (n-gons, boundary, and non-4-valence vertices are hard terminators; visited-set so closed
   loops and malformed input terminate safely); coplanar flood fill from a seed face (contiguous
   faces within a normal-angle epsilon) and plane-wide query (all faces on the seed's infinite
   plane, contiguity ignored); region-rim classification for a face set per the per-edge
   selected-adjacent-face-count rule (|A_sel|==2 interior, ==1 rim owned by the selected face,
   ambiguous non-manifold → reject signal) — TOOL-FOUNDATIONS S3.
2. Element BVH (S4 narrow phase): per-LevelMesh AABB tree over baked triangles, leaf carrying
   (tri_id, face_id); built lazily, dirty-flagged on any geometry diff. Query:
   `ray_closest(local_origin, local_dir)` → {hit, t, face_id, tri_id, barycentric}. Geometric,
   NOT physics, NOT GPU — must work headless. Also expose bulk element accessors the view layer
   will need for screen-space tolerance picking: positions of a face's corner verts and boundary
   edges (polygroup tier: whole-polygroup boundary) so the editor can project them itself.
3. Generation-stamped stable handles (S4): every vert/edge/face slot carries a generation u32;
   freeing bumps it. Script-visible handle encoding (suggest packing slot+generation into an
   int64) with `resolve_vertex/edge/face(handle)` → slot or -1, and `make_*_handle(slot)`.
   LevelMeshDiff gains `removed_vertex/edge/face_handles` arrays (computable by diffing alive
   flags between before/after snapshots) so a SelectionModel can revalidate O(changed). The
   kernel has no delete ops yet (create_box only) — build the mechanism now anyway so WP6/WP7
   never retrofit it; exercise it in tests at the data layer.
4. Script exposure of every query above (ClassDB) so headless checks can drive picking with
   synthetic rays. Extend modules/level_kernel/tests/smoke_project/smoke.gd (or a sibling
   script wired the same way) with: adjacency correctness on a box (edge→faces counts,
   face→edges loop order), loop/ring walk on a box (each walk of a box edge terminates
   correctly — box faces are quads but every vertex is valence-3, so loops must STOP; rings
   must return the 4-edge band around the box), flood fill (one box face = itself; two coplanar
   touching boxes stay separate meshes so fill stays within one), ray_closest hitting a known
   face center returns that face_id, and handle round-trip + stale-handle resolve→-1 after a
   simulated free.
</task>

<action_safety>
Allowed files: modules/level_kernel/** only (new files + edits), plus DIVERGENCE-LEDGER.md append
if any shared file outside the module must be touched (it should not). NEVER touch
editor/scene/3d/node_3d_editor_viewport.*. No git commits. Match the module's existing style
(columnar arrays, defensive `_face_is_bakeable`-style bounds discipline). Do not change existing
public API signatures; add alongside. Note: the baker now emits reversed (clockwise) winding at
the bake seam — kernel loops remain CCW-outward; the element BVH triangulates from kernel loop
order, so face_id mapping must use the same fan (loop_start, corner+1, corner+2 by kernel order),
independent of the render flip.
</action_safety>

<verification_loop>
Build `scons platform=windows target=editor dev_build=yes`; fix all errors/new warnings. If the
final link fails with "Access is denied" on bin/godot.windows.editor.dev.x86_64.exe, the user is
running that editor binary — wait ~60s and retry the same scons command until it links (object
compiles are unaffected). Run the module smoke
(bin/godot.windows.editor.dev.x86_64.console.exe --headless --path modules/level_kernel/tests/smoke_project --script smoke.gd)
until green, then confirm no regression: run
workspace-editor-planning/smoke/run_smoke.sh with the console binary for block_tool_smoke and
level_tab_smoke. Done = clean build + new adjacency/BVH/handle assertions green + zero regressions.
</verification_loop>

<compact_output_contract>
Final report: files created/modified; the exact script-visible API added (method signatures);
build result; verbatim module smoke output tail; deviations from tools/02 / TOOL-FOUNDATIONS with
one-line rationale each.
</compact_output_contract>
