# CSG Edit — Implementation Progress Ledger

Orchestration state for the phased implementation of `CSG-EDIT-PLAN.md`. Updated by the orchestrator after every phase step. Read this first when resuming.

## Orchestration rules

- Implementation is delegated to Codex CLI (`codex exec`) per phase; the orchestrator reviews the diff after each phase.
- After each phase: run an Opus agent with the /simplify pass over the phase's changes, then re-verify.
- Build only when necessary (phase verification or compile-sanity after large changes). Full builds are expensive.
- Git: user approved committing. Baseline workspace WIP committed at d399876524; plan docs at 7bf59608a7. Commit each phase after verification + simplify pass (one commit per phase, Co-Authored-By trailer). Never stash/revert user work.
- Any new editor surface/context must reuse an established fork pattern (chrome registry, provider/registry factories, per-view state) or establish one that later systems can reuse.

## Build commands (updated 2026-07-22 — see repo-root CLAUDE.md, shared-tree agreements)

- The tree is shared by MULTIPLE concurrent agent sessions. Before ANY build, check for a running scons: `Get-CimInstance Win32_Process -Filter "Name='python.exe'"` (look for SCons in command line). Never run two builds of the same object namespace concurrently (phantom LNK1120s).
- Production editor: `.\build_editor.ps1` ONLY (serializes builds, pins flags, fixes hardlinks). Never raw scons for production.
- Dev + unit tests (separate `.dev.` namespace, safe alongside production but not alongside another dev build): `scons platform=windows target=editor dev_build=yes tests=yes winrt=no -j24`
  - Test run: `bin/godot.windows.editor.dev.x86_64.exe --test --test-case="*CSG*"` (doctest filters)
- `winrt=no` is REQUIRED for dev builds — MSVC 14.51 hard-errors (STL1011) on `/await` + `<experimental/coroutine>`.
- A running editor holds the exe lock; the final link fails while output looks fine — check binary timestamp; RENAME (don't kill) a locking editor exe.
- Shell pipelines mask scons's exit code — check `$LASTEXITCODE`/exit status directly.
- Long builds: run in background and poll; single foreground calls time out at 10 minutes.
- Git: stage files explicitly, never `git add -A` — other sessions' uncommitted work shares the tree.

## Phase status

| Phase | Description | Status |
|---|---|---|
| 0 | Characterization tests + dev counters (no behavior change) | DONE — committed 4261ae84e1 (simplify pass: one test-helper consolidation) |
| 1 | Persistent Manifold cache graph | DONE — committed 95e5716225 (simplify: one hidden-constraint comment added; code judged minimal) |
| 2 | Semantic provenance (schemas, origin tokens, faceID) | implemented+verified (18/18 cases, 912/912 asserts); simplify pass running |
| 3A | Document surface registry | pending |
| 3B | Edit-domain host + chrome (center slots, dummy domain) | pending |
| 4 | Evaluation scheduler + box Surface tool (push/pull) | pending |
| 5 | Shift-drag box extrusion | pending |
| 6 | Surface materials + planar UVs + Paint tool | pending |
| 7 | Draw tool | pending |
| 8 | Long tail | pending |

## Phase log

### Phase 0 — started 2026-07-22

- Scope: dev-only counters in modules/csg + characterization tests per plan §28 Phase 0 / §29 module tests. No behavior changes.
- Codex result (task-mrw9sebe-k94ig3): DONE, verified.
  - Files: csg_debug_counters.{h,cpp} (new, DEV_ENABLED-only, SafeNumeric<uint64_t>), csg_shape.cpp (+35 instrumentation lines), tests/test_csg.h (+237, six characterization cases). No test-registration change needed (generated module-test registry auto-discovers).
  - Counters: local_primitive_brush_packs, transformed_wrapper_constructions, batch_boolean_calls, operation_switch_flushes, root_materializations, uv_finalizations, tangent_finalizations, collision_rebuilds; reset()/get().
  - Tests: 6 new cases 46/46 assertions; full *CSG* filter 7/7 cases 48/48 (pre-existing polygon suite green). Dev build succeeded.
  - BASELINE (Phase 1 must beat): 3-leaf mixed-op tree = 6 BatchBoolean calls (one per child materialization + 3 root op groups), 2 op-switch flushes, each leaf packs once per rebuild; nested single-leaf tree = 3 BatchBoolean calls (one per authored level); 2 transformed wrappers for nested combiner case. Pinned behaviors: subtract-then-union vol 57 vs union-then-subtract vol 56; box = 36 corner UVs / 144 tangent floats; empty combiner → empty brush, default AABB, valid zero-surface mesh; hidden children fully excluded; collision 36 verts, 1 rebuild.
- Opus /simplify pass: DONE — collapsed repeated DEV_ENABLED reset boilerplate in test_csg.h into _reset_csg_counters() helper; rejected no-op-stub call sites (guard logic must compile out of release). Re-verified 7/7, 48/48. Committed 4261ae84e1.

### Phase 1 — started 2026-07-22

- Scope: persistent Manifold cache graph per plan §12/§13/§28 Phase 1. Recursive expression composition, subtree handle identity, root-only materialization, once-per-pack material ID records, granular invalidation, _get_brush caller audit, acceptance tests vs Phase 0 baseline.
- Codex result (task-mrwbao0m-bpe6zz): DONE, verified. 13/13 cases, 135/135 assertions; all Phase 0 output pins unchanged.
  - CSGShape3D::ManifoldCache (opaque, csg_shape.cpp): local brush, local Manifold, transformed wrapper, subtree expression handle, persistent originalID/material records, per-level dirty flags, cached subtree emptiness.
  - Invalidation: geometry → _make_dirty (local + ancestors); LOCAL_TRANSFORM_CHANGED → _make_transform_dirty (wrapper + parent chain only); op/order/visibility → parent expression + ancestors; material setters → _make_material_dirty (materialization only).
  - Acceptance deltas: transform edit = 0 leaf repacks/1 wrapper/1 parent expr; resize = 1 repack/2 exprs; deep branch = 1 repack/4 exprs on chain; material-only = zero boolean work; clean update = zero cache work. Mixed 3-leaf tree BatchBoolean 6→3.
  - _get_brush audit: parent composition no longer calls child _get_brush; gizmos/tests use explicit get_brush_faces() materialization; non-root AABBs from Manifold bounds; config warnings use cached emptiness. get_brush_faces not script-bound.
  - CRITICAL GOTCHA (documented for later phases): manifold::Manifold::GetMeshGL64() REPLACES the receiving handle with an evaluated leaf — root materialization must evaluate a COPY of the subtree handle or expression identity is destroyed.
  - Grouping subtlety preserved: node's own operation starts the grouping before child op switches; singleton BatchBoolean calls elided (same handle) with identical output.
- Opus /simplify pass: DONE — verdict "already close to minimal"; added one comment documenting that IsEmpty()/BoundingBox() also collapse the receiving Manifold handle (same trap as GetMeshGL64), so cached handles are always evaluated via a copy. Re-verified 13/13, 135/135. Committed 95e5716225.
- Scout docs committed 03e49a3d94 (CSG-3A-SURFACE-SCOUT.md, CSG-3B-INPUT-SCOUT.md).

### Phase 2 — started 2026-07-22

- Scope: semantic provenance per plan §10/§11/§28 Phase 2. Per-primitive surface schemas + named constants, CSGSurfaceKey/CSGOriginToken with once-per-schema ReserveIDs ranges retained in ManifoldCache, semantic-surface MeshGL runs + meaningful input faceIDs, material lookup moved off runOriginalID, evaluation snapshot (token→SurfaceKey) + generation-bound triangle resolution API, provenance tests. Module-only.
- Codex result (task-mrwcu7gj-if6fov): DONE, verified. 18/18 cases, 912/912 assertions; all Phase 0/1 pins green.
  - Files: csg.h (Face semantic metadata), csg_shape.h (CSGSurfaceKey/CSGSurfaceHit/CSGOriginToken, per-primitive Surface enums, resolution API), csg_shape.cpp (token ranges, semantic packing, snapshot), test_csg.h (+5 cases).
  - Schemas: Box +X/-X/+Y/-Y/+Z/-Z=0..5 (brush emits +X,+Y,+Z,-X,-Y,-Z → map 0,2,4,1,3,5); Cylinder SIDE/TOP/BOTTOM (cone has no top slot); Sphere/Torus BODY=0 with per-triangle faceIDs (curvature); Polygon FRONT/BACK/SIDE fixed 3-slot schema even when caps absent (avoids generation churn); path-extrusion side quads get per-triangle faceIDs (non-planar under twist); Mesh semantic index = source surface index, faceID = triangle ordinal.
  - Token ranges: ManifoldCache stores origin_base/count/schema_generation; ReserveIDs(schema_size) once per schema generation; retained across geometry rebuilds; new range on mesh surface-count change.
  - Snapshot per root: HashMap<token, CSGSurfaceKey> + Vector<{token,faceID}> per output triangle (2×u32/tri) + result_generation u64. resolve_result_triangle(triangle, generation, &key, &face_id) validates generation, live ObjectID, schema generation, range. connected_fragment deferred to editor phase.
  - Inter-surface triangle order changes for interleaved primitives (cylinder) — render-surface grouping and geometry identical, pins passed.
- Opus /simplify pass over the four files: launched, pending.
