# WP1 — LE0 kernel module (implementation brief)

<task>
Implement the LE0 scope of a new engine module `modules/level_kernel` in this Godot 4.8 fork
(repo root = this workspace, Windows, MSVC, scons). READ FIRST, in order:
level-editor-planning/PLAN.md (§0 decisions, §1 kernel), level-editor-planning/TOOL-FOUNDATIONS.md
(§2 UV storage contract — BINDING), level-editor-planning/tools/01-block-tool.md (kernel-facing
API expectations). Model SCsub/config.py/register_types structure on the existing modules/horde_sim.

Deliverables:
1. `LevelMeshData : Resource` — columnar topology storage: vertex positions; edge tuples (v0,v1);
   face table (loop start/count, material_index, uv_mode enum PROJECTED/EXPLICIT, uv_origin Vector3,
   uv_tangent Vector3, uv_transform Transform2D, polygroup id, flags); loop table (vertex index,
   uv0 ALWAYS materialized, color, normal). Stable ids with free-list semantics in mind (design
   arrays so element removal can later leave holes). Serializes via standard Resource properties
   (packed arrays).
2. `LevelMesh : RefCounted` — live edit object wrapping a LevelMeshData. Transaction API:
   begin_transaction() / commit() returning a `LevelMeshDiff` (RefCounted, holds before/after spans
   sufficient to revert/reapply) / rollback(). One operator for LE0:
   create_box(Transform3D frame, Vector3 size, int material_index) building 6 quad faces, outward
   winding, each face PROJECTED with a world-axis planar UV frame (dominant-axis rule). Shared
   post-step reconcile_face_uv() that materializes loop uv0 from frame+uv_transform for PROJECTED
   faces (called at end of every op). apply_diff(diff)/revert_diff(diff) for undo integration.
3. `LevelMeshBaker` — helper: bake ArrayMesh with one surface per material (triangulate n-gon
   faces; fan is fine for convex LE0 quads), and produce trimesh collision faces
   (PackedVector3Array triplets) for ConcavePolygonShape3D.
4. `LevelBlock : Node3D` — registered node, exports a LevelMeshData; on READY and on data change
   rebuilds internal (non-persisted, internal-mode) MeshInstance3D + StaticBody3D/CollisionShape3D
   children from the baker. Runtime-safe: zero editor dependencies, works exported and headless.
5. Full ClassDB bindings for every class/method above so headless GDScript can drive
   create_box → bake → asserts.
6. A self-contained smoke project at modules/level_kernel/tests/smoke_project/ (minimal
   project.godot + smoke.gd) whose script: creates a LevelMesh, begin_transaction,
   create_box(identity, (4,4,4), 0), commit; asserts 8 verts / 12 edges / 6 faces / 24 loops;
   asserts every loop uv0 materialized per the dominant-axis projection; bakes and asserts
   12 triangles and 36 collision vertices; revert_diff → asserts empty; apply_diff → asserts arrays
   identical to post-commit; prints PASS/FAIL lines and exits nonzero on failure.

NOTE: a parallel task created level-editor-planning/testbed/checks/level_kernel_check.gd which
guesses this API via reflection — after your API is final, align that check's calls (it lists its
assumed names in a top-of-file comment). Keep the class/method names from this brief where possible.
</task>

<action_safety>
Create/modify files ONLY inside modules/level_kernel/ plus the single alignment edit to
level-editor-planning/testbed/checks/level_kernel_check.gd. Godot modules are self-registering —
no engine-file edits are needed or allowed. Never touch editor/scene/3d/node_3d_editor_viewport.*
(unrelated uncommitted work in tree). No git commits. Match Godot's C++ style (.clang-format);
follow existing module idioms (see modules/horde_sim).
</action_safety>

<verification_loop>
Build: `scons platform=windows target=editor dev_build=yes` from the repo root (warm incremental
cache from today — expect incremental, not full). Fix every error and every new warning you
introduced; rebuild until clean. Then run the smoke:
`bin/godot.windows.editor.dev.x86_64.console.exe --headless --path modules/level_kernel/tests/smoke_project --script res://smoke.gd`
and iterate until all asserts PASS. Also run
`bin/godot.windows.editor.dev.x86_64.console.exe --headless --path level-editor-planning/testbed --script res://checks/level_kernel_check.gd`
and make it PASS (not SKIP). Done = clean build + both scripts green.
</verification_loop>

<compact_output_contract>
Final report: list of files created with one-line purpose each; build result (config + wall time);
verbatim tail of both script outputs; any deviations from the planning docs with a one-line
rationale per deviation.
</compact_output_contract>
