# CSG Edit — Implementation Progress Ledger

Orchestration state for the phased implementation of `CSG-EDIT-PLAN.md`. Updated by the orchestrator after every phase step. Read this first when resuming.

## Orchestration rules

- Implementation is delegated to Codex CLI (`codex exec`) per phase; the orchestrator reviews the diff after each phase.
- After each phase: run an Opus agent with the /simplify pass over the phase's changes, then re-verify.
- Build only when necessary (phase verification or compile-sanity after large changes). Full builds are expensive.
- Git: user approved committing. Baseline workspace WIP committed at d399876524; plan docs at 7bf59608a7. Commit each phase after verification + simplify pass (one commit per phase, Co-Authored-By trailer). Never stash/revert user work.
- Any new editor surface/context must reuse an established fork pattern (chrome registry, provider/registry factories, per-view state) or establish one that later systems can reuse.

## Build commands (verified 2026-07)

- Production editor: `scons platform=windows target=editor dev_build=no d3d12=yes winrt=no -j24`
- Dev + unit tests: `scons platform=windows target=editor dev_build=yes tests=yes d3d12=yes winrt=no -j24`
  - Test run: `bin/godot.windows.editor.dev.x86_64.exe --test --test-case="*CSG*"` (doctest filters)
- `winrt=no` is REQUIRED — MSVC 14.51 hard-errors (STL1011) on `/await` + `<experimental/coroutine>` in the WinRT TTS driver.
- A running editor holds the exe lock; scons may exit 0 despite a failed final link — check binary timestamp.
- Long builds: run in background and poll; single Bash calls time out at 10 minutes.

## Phase status

| Phase | Description | Status |
|---|---|---|
| 0 | Characterization tests + dev counters (no behavior change) | implemented+verified; simplify pass running |
| 1 | Persistent Manifold cache graph | pending |
| 2 | Semantic provenance (schemas, origin tokens, faceID) | pending |
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
- Opus /simplify pass over the four files: launched, pending.
