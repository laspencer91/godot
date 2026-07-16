# WP16 — LE2 unwrap-mode kernel library (Conforming / Square / Planar / Follow Quads)

<task>
Implement the unwrap-mode library in modules/level_kernel of this Godot 4.8 fork (repo root =
this workspace) — the kernel half of LE2's Fast Texture tool; the modal overlay UI is the NEXT
WP and must not be built here. Prereq reading IN ORDER: level-editor-planning/PLAN.md §2 + §4
LE2, level-editor-planning/TOOL-FOUNDATIONS.md §2 (UV storage contract) + §4 (unwrap row),
then level-editor-planning/tools/08-uv-unwrap-fast-texture.md IN FULL — §1 (mode → storage
table, BINDING), §2 (foundation services), §3 pipeline steps 3/5/6, §4.1 (conforming), §4.2
(follow quads), §5 (test list). Then the existing kernel: WP13's frame algebra
(project_native/get_uv, frame builders, solve_edge_hinge_similarity, reconcile_face_uv,
uv_mode PROJECTED/EXPLICIT), WP15's wrap/flow propagation if landed (reuse its BFS shape),
LevelMeshAdjacency (shared-edge walks), LevelMeshDiff.

MODES (each: input = a face selection on one LevelMesh; output = ONE LevelMeshDiff; obeys the
§1 storage table exactly)
1. SQUARE: per-face independent box frame — dominant-axis frame per face (WP13's
   _dominant_axis_tangent convention), O = face centroid, uv_transform set so the face's UV
   bbox is normalized per texel-density-consistent scale (identity linear part is acceptable
   v1 — document); uv_mode = PROJECTED.
2. PLANAR: ONE world-aligned frame shared verbatim by the whole selection island (08 §1 —
   sharing the frame is why neighbors stay continuous): world-aligned planar frame from the
   AREA-DOMINANT face normal of the island (deterministic tie → lowest face id), same O/T
   written to every face; uv_mode = PROJECTED, per-face uv_transform = identity.
3. CONFORMING: region-growing hinge-unfold per 08 §4.1 — BFS from the lowest-face-id seed
   across the selection's internal manifold shared edges (selection boundary = automatic
   seam), each face hinge-unfolded relative to its BFS parent: rotate coplanar about the
   shared edge by the dihedral angle, project through the parent's ESTABLISHED 2D basis, and
   COPY shared-edge vertex UVs bit-exact (never re-project — TOOL-FOUNDATIONS S6). Accumulated
   dihedral distortion above a threshold starts a new island (new BFS root); cycle-closure
   conflicts (face reachable via two paths with disagreeing UVs beyond epsilon) cut a seam at
   the second-arriving edge, first-visited-wins, deterministic (stable neighbor iteration =
   face-loop order). Result: uv_mode = EXPLICIT, per-loop UVs authoritative, uv_transform
   cleared/ignored per the storage contract.
4. FOLLOW QUADS: quad-restricted grid walk per 08 §4.2 — from a seed quad (the selection's
   active/lowest quad; reject with a typed error if the seed is not a quad), walk opposite
   edges (loop.next.next) in both row directions and the perpendicular pair for columns; a
   non-quad neighbor or missing opposite edge terminates that direction; faces beyond a dead
   end are left COMPLETELY untouched. Spacing sub-modes computed per row/column 1D
   parametrization: LENGTH (cumulative real edge lengths), EVEN (i/N), LENGTH_AVERAGE
   (i * total/N). Result: uv_mode = EXPLICIT.
5. USE EXISTING is a no-op at kernel level (the overlay WP consumes it) — do not implement.

API SHAPE
LevelMesh methods (bound to GDScript, one diff each):
  unwrap_square(face_ids), unwrap_planar(face_ids),
  unwrap_conforming(face_ids, distortion_threshold = default),
  unwrap_follow_quads(face_ids, spacing_mode) — plus an enum for spacing.
All must: run against current geometry, call reconcile_face_uv / materialize loop UVs so the
baker sees final UVs immediately, cover every changed column in the diff (byte-identical
undo), and REJECT (typed error, mesh untouched) on empty selection, non-manifold internal
edges (radial > 2), or invalid seed — never crash, never partial-apply. Determinism: zero
randomness; same mesh + same selection → byte-identical output arrays (08 §5.1).
Also expose the hinge-unfold single-step (unfold face B across edge from A, returning the
UV map for B's loops without mutating) as a static/testable utility — the overlay's preview
and future hotspot fitter (LE3) both consume it.

MIXED-STATE RULE (08 §1 consequence): every mode regenerates BOTH storage fields for EVERY
selected face — a selection never leaves a session mixed (some PROJECTED, some EXPLICIT)
within the applied set. Assert this in tests.

TESTS (modules/level_kernel/tests/smoke_project — new unwrap_smoke.gd, additive; wire like
the existing kernel smokes)
  a. Determinism: same box + same selection through each mode twice → byte-identical
     uv columns (08 §5.1).
  b. Hinge seam continuity: an L-shaped 3-quad strip unwrapped Conforming → shared-edge
     vertex UVs bit-exact equal across faces; unfolded UV edge lengths equal 3D edge lengths
     within epsilon (isometry per step, 08 §5.2).
  c. Cycle closure: faces wrapping a 4-face closed band (box side ring) → BFS reaches one face
     via two paths; assert a seam is cut at the second-arriving edge (documented
     first-visited-wins winner), no overwrite, no crash.
  d. Follow-quads termination: quad strip dead-ending into a triangle → walk stops cleanly;
     the triangle and everything past it retain their prior UV state bitwise; all three
     spacing modes produce their documented 1D parametrizations on a known-length strip.
  e. Planar continuity: two coplanar neighbor faces → identical frames, get_uv agrees at the
     shared edge; Square on the same pair → independent centroid frames.
  f. Mixed-state regeneration: selection with one PROJECTED and one EXPLICIT face → after any
     single mode, every selected face is in the same state class (08 §5 "mixed-state").
  g. Undo: each mode's diff undone → all UV columns byte-identical to pre-op; redo →
     byte-identical to post-op.
  h. Reject paths: empty selection and non-quad follow-quads seed → typed rejection, mesh
     byte-identical.

FILES YOU MAY TOUCH: modules/level_kernel/** (new unwrap source files, bindings, tests; small
additive seams in LevelMesh/adjacency where the walks need accessors). NOTHING under
editor/ — this WP is kernel-only. Append one entry to
workspace-editor-planning/DIVERGENCE-LEDGER.md under the G-Level log (note the overlay-UI
deferral to the next WP and any v1 simplifications, e.g. Square's identity scale).
</task>

<action_safety>
NEVER touch editor/scene/3d/node_3d_editor_viewport.* (user's uncommitted WIP). No git
commits. No changes outside the files listed above. Existing bound signatures must not change
(additive only). Existing smoke assertions — kernel AND editor suite — must pass UNMODIFIED;
if one fails, your change altered behavior; fix the change, not the smoke.
</action_safety>

<verification_loop>
Build from repo root: `scons platform=windows target=editor dev_build=yes -j4`; fix all errors
and new warnings. If the FINAL LINK fails with "Access is denied" on
bin/godot.windows.editor.dev.x86_64.exe, the USER'S LIVE EDITOR may hold the lock — NEVER kill
or Stop-Process ANY godot process for ANY reason (sandbox-visible process metadata is
unreliable). Retry a few times over ~2 minutes; if still locked, build with extra_suffix=wp16
and verify against bin/godot.windows.editor.dev.x86_64.wp16.console.exe, and say so in the
report. Then run, all green: every kernel smoke in modules/level_kernel/tests/smoke_project
(smoke.gd, transform_smoke.gd, uv_smoke.gd, any WP15 smoke, plus your unwrap_smoke.gd) and the
editor suite via `bash workspace-editor-planning/smoke/run_smoke.sh "$(pwd)/bin/<console binary>"`
(if Bash cannot start under your sandbox, run the suite's cases one-for-one from PowerShell as
prior WPs did and say so). Rare pre-existing flakes (rerun once before investigating):
selection_smoke exit-139 at teardown, block_tool extrude-height 2.0-vs-3.0.
</verification_loop>

<compact_output_contract>
Final report: the BFS/walk data structures and where determinism comes from; the distortion
threshold default and cycle-closure epsilon chosen; the follow-quads opposite-edge rule
implementation on the n-gon loop rings; the GDScript surface; files touched; build result
(standard or suffixed + why); verbatim tails of ALL kernel smokes and the editor suite; any
behavior deltas beyond the new ops (should be none).
</compact_output_contract>
