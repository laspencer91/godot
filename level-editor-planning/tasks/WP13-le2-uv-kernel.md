# WP13 — LE2 kernel UV foundation (frame algebra, reconcile_face_uv, texture lock)

<task>
Implement the S6 "UV frame algebra" service in modules/level_kernel of this Godot 4.8 fork
(repo root = this workspace) — the kernel substrate every LE2 texturing tool builds on. Prereq
reading IN ORDER: level-editor-planning/PLAN.md §1 (kernel schema) + §4 LE2,
level-editor-planning/TOOL-FOUNDATIONS.md §2 (the ★ UV storage contract — it SUPERSEDES the
storage sketches in the tool plans) + §1 S6 + §3 pattern 4, then
level-editor-planning/tools/09-face-texturing-ops.md §1/§2/§4.1/§4.3 (frame convention, hinge
solver, texture lock) and tools/08-uv-unwrap-fast-texture.md §1/§4.3 (dual-storage contract,
reconcile ordering). Then read the existing kernel: modules/level_kernel (LevelMeshData columns,
LevelMesh operators — especially extrude_faces, push_pull_faces, the transform
preview/commit lifecycle — LevelMeshDiff, LevelMeshBaker) and how editor/level/
select_tool_transform.cpp drives commit_transform_preview.

SCHEMA (TOOL-FOUNDATIONS §2, binding)
Extend LevelMeshData's per-face columns with: uv_mode (enum PROJECTED/EXPLICIT), uv_origin
(Vector3), uv_tangent (Vector3), uv_transform (Transform2D — meaningful only when PROJECTED).
Per-loop uv0: if a loop UV column already exists, keep it; otherwise add it. Loop uv0 is ALWAYS
materialized and is the ONLY UV field LevelMeshBaker reads — the baker must gain zero knowledge
of uv_transform/frames. New faces (create_box, extrude walls) default to PROJECTED with an
Align-to-Grid frame (below) and identity uv_transform. All new columns flow through
LevelMeshDiff (undo must restore them byte-identical), serialization (however LevelBlock
persists mesh data today), and the copy/clone paths.

FRAME ALGEBRA (kernel, pure functions where possible)
- project_native(face, p) = Vector2((p - O).dot(T), (p - O).dot(B)) where B =
  normalize(N.cross(T)) derived LIVE from the CURRENT face normal — only T is stored/frozen
  (09 §1.1: continuous under normal drift; no dominant-axis argmax at evaluation time).
- get_uv(face, p): PROJECTED → uv_transform * project_native(face, p); EXPLICIT → the loop's
  stored uv0. One read adapter, both modes (09 §4.5).
- Align-to-Grid frame builder: T from a dominant-axis table lookup on argmax(|N.x|,|N.y|,|N.z|)
  — X-dominant → T = -Z axis, Y- and Z-dominant → T = -X axis (the Cyclops-proven sign
  convention, 09 §1.2); O = Vector3.ZERO always (world origin — cross-block continuity).
- Align-to-Face frame builder: if the face has a nonzero prior T, re-orthogonalize it against
  the current normal (T' = normalize(T - N * T.dot(N))); else fall back to the direction of the
  longest boundary edge (deterministic). O = face centroid.
- Edge-hinge similarity solver (kernel primitive for LE2's wrap/flow, no editor caller yet):
  given two point→UV correspondences, solve the unique similarity Transform2D via the
  complex-number closed form in 09 §4.1. Guard the degenerate p1 == p0 case (reject).
- Scale contract: project_native is in world units; identity uv_transform means 1 UV unit per
  world unit. Document this in the header — the texel-density mapping arrives with the material
  browser WP, not here.

reconcile_face_uv (08 §4.3 — the ONE shared post-step)
A single kernel function every operator that changes a face's plane, moves its vertices, or
splits/merges it calls before returning its diff:
- PROJECTED: recompute all the face's loop uv0 = uv_transform * project_native against the NEW
  geometry (this IS texture lock).
- EXPLICIT: if the op split the face, transfer old loop UVs by barycentric interpolation from
  the pre-op face shape; rigid whole-face motion needs no UV change; never invent UVs, never
  silently write (0,0).
Wire it into EVERY existing mutating operator: transform commit (vertex positions), extrude_faces
/ push_pull_faces / extrude_boundary_edges (moved cap faces reconciled; NEW wall faces get
attribute-inheritance per TOOL-FOUNDATIONS §3.4: material/smoothing from the rim-owning face,
FRESH Align-to-Grid frame re-projected, never copied), create_box, and any other position/topology
op you find in LevelMesh. Ordering guarantee: reconcile runs inside the op, so the baker never
sees stale loop UVs.

TEXTURE LOCK (09 §4.3, default ON — a standing kernel behavior, not a toggle yet)
Inside the transform commit path, distinguish:
1. Rigid/affine whole-selection transform where every vertex of a face moved under one
   Transform3D M (the move/rotate drags): exact path — O' = M.xform(O), T' re-orthonormalized
   from M.basis * T; loop UVs then re-materialize via reconcile (they should come out unchanged
   in appearance; for pure translation with a world-origin grid frame the TEXTURE ANCHORS TO THE
   WORLD projection — uv_transform unchanged means the face slides under the texture. That is
   the documented Hammer no-lock-style default for grid-framed faces; LOCK semantics = transform
   the frame with the geometry, which is what this path does. Follow 09 §4.3 case 1 exactly.)
2. Partial/asymmetric vertex motion (subset of a face's vertices moved): least-squares refit of
   uv_transform (6 DOF affine, closed form via normal equations) from (old_uv, new
   project_native) correspondences over all the face's vertices, with a degeneracy guard —
   near-zero area or ill-conditioned normal matrix → freeze uv_transform unchanged, never NaN
   (reuse the kernel's existing face-area/degeneracy check style).
The mesh-tier transform preview does NOT need live UV updates this WP — preview remains
positions-only; the lock resolves at commit via reconcile. Note this in the ledger entry.

API EXPOSURE
Bind to GDScript whatever the headless checks need (get_uv, the frame builders as callable ops
on LevelMesh — e.g. align_faces_to_grid(face_ids)/align_faces_to_face(face_ids) returning one
diff each — uv_mode/frame accessors, the hinge solver as a static utility). Follow the module's
existing binding patterns and the "fully scriptable kernel" rule.

TESTS (modules/level_kernel/tests/smoke_project — new uv_smoke.gd, additive; do not modify
existing smoke scripts)
  a. Round-trip: uv_transform.affine_inverse() * get_uv == project_native for arbitrary points
     on an arbitrarily oriented face.
  b. Grid-frame continuity: two separate boxes sharing a coplanar face plane, both
     Align-to-Grid + identity transform → get_uv agrees at shared world positions (cross-block
     tiling).
  c. Texture lock, rigid: random rigid transform of a whole block via the transform
     preview/commit path → every vertex's get_uv unchanged within epsilon.
  d. Texture lock, partial: move one vertex of a quad face → other three loops' uv0
     bit-identical, moved loop matches the LSQ refit prediction; collapse the face toward zero
     area → no NaN/throw, uv_transform bitwise unchanged.
  e. Extrude inheritance: extrude a face → cap face reconciled (lock holds), wall faces are
     PROJECTED with fresh grid frames and sane loop UVs (no (0,0) fill, no copied cap
     transform).
  f. Hinge solver: two quads at a dihedral angle, arbitrary uv_transform on A → solve B →
     get_uv equal along the shared edge at t = 0, 0.25, 0.5, 0.75, 1.0 (not just endpoints).
  g. Undo: any of the above ops undone → all UV columns byte-identical to pre-op.
Wire uv_smoke.gd into whatever harness runs the existing kernel smokes if one exists; otherwise
it runs standalone like smoke.gd/transform_smoke.gd.

FILES YOU MAY TOUCH: modules/level_kernel/** (schema, operators, baker read-path if it doesn't
already consume loop UV, bindings, tests), editor/level/select_tool_transform.cpp ONLY if the
commit path needs to pass whole-selection-transform info (M) to the kernel for the rigid lock
path, level-editor-planning/PLAN.md ONLY to append the schema-delta note §5 of TOOL-FOUNDATIONS
already anticipates (one short bullet). Append one entry to
workspace-editor-planning/DIVERGENCE-LEDGER.md under the G-Level log (note: preview-time UV
updates deferred; texel-density scale mapping arrives with the material browser WP).
</task>

<action_safety>
NEVER touch editor/scene/3d/node_3d_editor_viewport.* (user's uncommitted WIP). No git commits.
No changes outside the files listed above. Existing bound signatures must not change (additive
bindings only). Existing smoke assertions — kernel AND editor suite — must pass UNMODIFIED; if
one fails, your change altered behavior; fix the change, not the smoke.
</action_safety>

<verification_loop>
Build from repo root: `scons platform=windows target=editor dev_build=yes -j4`; fix all errors
and new warnings. If the FINAL LINK fails with "Access is denied" on
bin/godot.windows.editor.dev.x86_64.exe, the USER'S LIVE EDITOR may hold the lock — NEVER kill
or Stop-Process ANY godot process for ANY reason (sandbox-visible process metadata is
unreliable). Retry a few times over ~2 minutes; if still locked, build with extra_suffix=wp13
and verify against bin/godot.windows.editor.dev.x86_64.wp13.console.exe, and say so in the
report. Then run, all green: the kernel module smokes (--headless --path
modules/level_kernel/tests/smoke_project --script smoke.gd, --script transform_smoke.gd, and
your new --script uv_smoke.gd) and the editor suite via
`bash workspace-editor-planning/smoke/run_smoke.sh "$(pwd)/bin/<console binary>"` (if Bash cannot
start under your sandbox, run the suite's cases one-for-one from PowerShell as prior WPs did and
say so). Known pre-existing failure that is NOT yours: floating_camera_preview (sub_viewport
null). Rare pre-existing flakes (rerun once before investigating): selection_smoke exit-139 at
teardown, block_tool extrude-height 2.0-vs-3.0.
</verification_loop>

<compact_output_contract>
Final report: the final schema (columns added, defaults, diff/serialization coverage); where
reconcile_face_uv is called from (every op listed); how the rigid-vs-LSQ texture-lock split is
detected at commit; the GDScript surface added; files touched; build result (standard or
suffixed + why); verbatim tails of all three kernel smokes and the editor suite; any behavior
deltas beyond the new feature (should be none).
</compact_output_contract>
