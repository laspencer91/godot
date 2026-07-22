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
| 2 | Semantic provenance (schemas, origin tokens, faceID) | DONE — committed 9c24baed23 (simplify: tagging helper + hidden-constraint comments) |
| 3A | Document surface registry | DONE — committed (simplify pass clean: no changes needed) |
| 3B | Edit-domain host + chrome (center slots, dummy domain) | DONE — committed (simplify: minimal, 2 constraint comments) |
| 4 | Evaluation scheduler + box Surface tool (push/pull) | pre-step DONE (committed; simplify: minimal, zero edits); scheduler pending |
| 5 | Shift-drag box extrusion | pending |
| 6 | Surface materials + planar UVs + Paint tool | pending |
| 7 | Draw tool | pending |
| 8 | Long tail | pending |

## LIVE STATE (update before any compaction; read first on resume)

As of 2026-07-22 ~19:30Z:
- Phase 3A COMMITTED (simplify pass green, zero source changes; all milestone tags preserved; migration fidelity verified vs HEAD).
- Phase 4 pre-step plan SAVED at CSG-PHASE4-PRESTEP-PLAN.md (Opus planner done). Handed to Codex (module tree modules/csg/** disjoint from 3B editor work — interleaving approved by orchestrator). Codex must serialize dev builds: check `Get-Process python` for a running scons before every build.
- Phase 3B plan SAVED at CSG-PHASE3B-PLAN.md (Opus planner done; line refs re-verified against live tree; typed EditorEditDomainContext mirrors 3A idiom; two-scope suppression: per-viewport domain_blocks_native + global is_edit_domain_active_anywhere; DEV-only dummy domain; headless test plan).
- Phase 4 pre-step COMMITTED ea4759315f (Codex task-mrwfaikf-69b7f9; simplify minimal/zero edits; 18/18, 912).
- Phase 3B COMMITTED (Codex task-mrwgvqk4-2me7zl; simplify minimal + 2 comments; all four suites green). MANUAL FOLLOW-UP for user: live two-pane visual checklist via DEV dummy domain (double-click enter, Tab, Escape, chrome mount, gizmo suppression across panes) — not automatable headlessly.
- NEXT: Phase 4 proper (scheduler + box Surface tool). Flow: Opus Plan agent drafts CSG-PHASE4-PLAN.md (inputs: plan §14/§15/§16/§28 Phase 4, the pre-step seam in modules/csg/csg_evaluation.h, the 3B domain seam in editor/gui/editor_edit_domain.h, deferred-decisions notes incl. collision-payload-built flag) → Codex implements → review → simplify → verify → commit.
- Orchestration flow (user-directed): Opus agents plan structural goals → Codex agents implement → Opus /simplify after each phase → verify → commit per phase. Codex launch = Agent(subagent_type codex:codex-rescue); job state JSON at C:\Users\laspe\.claude\plugins\data\codex-openai-codex\state\godot-683f4ee0b87eee85\jobs\<task-id>.json; watcher pattern = background bash until-loop on '"status": "running"'.
- Remaining phases after 3A: 3B (edit-domain host + chrome, scout: CSG-3B-INPUT-SCOUT.md; gizmo-suppression decision = suppress globally while domain active, MVP), 4 (pre-step then scheduler + Surface tool), 5 (extrusion), 6 (materials/UVs/Paint), 7 (Draw), 8 (long tail).

## Deferred decisions / phase-prompt notes

- Phase 4 pre-step (user-approved flow: Opus plans → Codex implements): split evaluation machinery out of csg_shape.cpp (csg_manifold_cache/csg_evaluation files) + detachable evaluation-snapshot object built by workers, published by main thread. Plan DRAFTED → CSG-PHASE4-PRESTEP-PLAN.md (6-step migration, each step green; zero behavior change; counter-pin table; …w scratch-pointer hazard; subtree-copy invariant).
- Phase 6 prompt: add a get_material() virtual on CSGPrimitive3D to replace the 7-way cast chain in _resolve_manifold_material (flagged by Phase 1 simplify) before layering surface-override resolution on top.
- Phase 3B prompt + 3A review: ONE context carrier — surface instance context, domain session context, and chrome context should hand around the same object (view or domain host; 3A introduced typed EditorDocumentSurfaceContext). No parallel divergent Dictionary shapes.
- Phase 7 prompt: decide use_collision default for Draw-tool-created standalone roots (new roots default off today; drawing under an existing root inherits root collision automatically). Also: mid-drag collision intentionally lags until final publish (§14/§30) — expected, documented.
- Phase 4 (scheduler) prompt: CSGEvaluationSnapshot needs an explicit "collision payload built" flag (or quality tag) — an empty collision_faces Vector cannot distinguish skipped collision work from a valid empty result once interactive snapshots skip collision (Codex pre-step finding; harmless on the synchronous path).

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
- Opus /simplify pass: DONE — _tag_faces_single_surface helper (sphere/torus dedup), box-loop hoist, hidden-constraint comments (cylinder face layout dependency; polygon fixed 3-slot rationale). Re-verified 18/18, 912/912. Committed 9c24baed23.

### Phase 4 pre-step — started 2026-07-22

- Scope: per CSG-PHASE4-PRESTEP-PLAN.md — extract pure evaluation from csg_shape.cpp into csg_manifold_cache.h + csg_evaluation.{h,cpp}; introduce CSGEvaluationSettings/Inputs/Snapshot, _gather_evaluation_inputs, _publish_snapshot. Zero behavior change; the seam Phase 4's scheduler lands on. modules/csg/** only.
- Codex result (task-mrwfaikf-69b7f9): DONE, verified. All 6 migration gates + final rerun: 18/18 cases, 912/912 assertions each time.
  - Files: csg_shape.cpp (2927 lines, -672/+113 net), csg_shape.h (579), csg_manifold_cache.h (new, 76), csg_evaluation.h (new, 164), csg_evaluation.cpp (new, 643). No SCsub/test edits.
  - Invariants audited by Codex + orchestrator review: subtree-copy-before-evaluate (with comments), _get_brush early-out + metadata/generation responsibilities intact, counter sites per plan §6, …w scratch pointers cleared, clear-then-rebuild deferred to _publish_snapshot.
  - Noted coupling: csg_manifold_cache.h includes csg_evaluation.h (cache retains Vector<CSGManifoldResultTriangle>). Phase 4 finding recorded in deferred decisions (collision-payload-built flag).
- Opus /simplify pass: DONE — verdict MINIMAL, zero edits. Every moved function diffed vs HEAD (only permitted renames/settings-parameterization); no double generation bump (sync path leaves snapshot.brush null so _publish_snapshot's brush branch is dormant); csg_build_snapshot coherent though unused by sync path (intentional Phase 4 seam); subtree-copy comments at all four sites; no dangling symbols. Noted-not-applied: update_shape re-assembles CSGEvaluationSettings that _get_brush's gather already built — fixing requires returning inputs from _get_brush, a seam change deferred to Phase 4. Re-verified 18/18 (912). Committed with this ledger update.

### Phase 3B — started 2026-07-22

- Scope: per CSG-PHASE3B-PLAN.md — generic edit-domain layer (Registry/Provider/Session/Host, tri-state input, typed context), single _sinput arbitration hook, five suppression guards (two scopes), SLOT_CENTER_LEFT/RIGHT chrome, DEV dummy domain, headless tests. editor/** + tests/editor/** only.
- Codex result (task-mrwgvqk4-2me7zl): DONE, verified. All 6 steps built; *EditorEditDomain* 6/6 (85), *EditorDocumentSurface* 2/2 (33), *ResponsiveLayout* 2/2 (26), *CSG* 18/18 (912).
  - New: editor_edit_domain.h (152), editor_edit_domain.cpp (378), test_editor_edit_domain.cpp (392). Modified: viewport_chrome (+8), plugin (+32), viewport (+70/-7), register_editor_types (+9).
  - Hook at _sinput:2843 (after global plugins, before RMB/nav); guards at :3040 (gizmo pick), :3071 (transform select), :3076 (subgizmo), :3162/:3211 (begin_transform), :3337 (gizmo hover), :3371 (transform hover); helpers _domain_pane_accepts_input:2231, _neutralize_click_state:2248; draw forward :4712.
  - Deviations: dummy passes all RMB/modifier-LMB (superset of required pass list); manual two-pane visual checklist deferred (no interactive driver — dummy ready for manual verification); freelook bookkeeping still main_view-backed (future per-view ownership coupling noted); provider hot-unregistration requires owner to call notify_provider_unregistered (registry does not enumerate hosts; shutdown order safe).
- Opus /simplify pass: DONE — verdict minimal; 2 constraint comments added (hazard-8 rationale on _neutralize_click_state; global-vs-per-view suppression scope on is_edit_domain_active_anywhere). Inert-when-inactive PASS at all 8 sites (domain_blocks_native reset per event, cannot leak); _select_ray is const, no viewport-state mutation — declined double-click activation falls through cleanly (benign redundant BVH query per focused-pane LMB double-click; guarding it away would need speculative registry API, left as-is). Idiom matches 3A; chrome corner slots byte-identical; DEV gating symmetric. Re-verified all four suites green. Committed with this ledger update.

### Phase 3A — started 2026-07-22

- Scope: EditorDocumentSurfaceRegistry/Provider/Instance per plan §4/§28 Phase 3A, migrating the DocumentView construction switch. Strict behavior preservation of the six lifecycle seams documented in CSG-3A-SURFACE-SCOUT.md. New files editor/gui/editor_document_surface.{h,cpp}; registry test in tests/editor/.
- Codex result (task-mrwdx47m-j0nn86): DONE, verified. *CSG* 18/18 (912), *EditorDocumentSurface* 2/2 (33), *ResponsiveLayout* 2/2 (26).
  - Files: editor/gui/editor_document_surface.{h,cpp} (new: Context/Provider/Instance/Registry + six built-in providers), document_view.{h,cpp} (construction switch removed, ~-650 lines, delegates to instance), tabbed_document_host.cpp (user-close → instance hook), register_editor_types.cpp (registry create/register before EditorViewportChromeRegistry; symmetric shutdown), tests/editor/test_editor_document_surface.cpp (new, 2 cases/33 asserts).
  - Context is a typed 4-field struct (document, document_view, host_view, chrome_host) — no capabilities Dictionary. Default resolution: resource/script/shader/help/screen_host/scene; unknown → scene (selection present: composite; else bare-3D fallback). Providers borrowed, never retained by instances.
- Opus /simplify pass: DONE — verdict "already minimal and faithful", zero source changes. Migration fidelity diffed block-by-block vs HEAD (only receiver renames/enum qualifications/context renames); all six lifecycle seams verified (set_context_active guard, screen-host re-park+null, PREDELETE ordering incl. unconditional _park_script_chrome, two close paths); all milestone comment tags preserved, new seams tagged CSG-3A; registry idiom matches EditorViewportChromeRegistry; scene virtuals defaulting on base judged the minimal option. Re-verified all three filters green. Committed with this ledger update.
