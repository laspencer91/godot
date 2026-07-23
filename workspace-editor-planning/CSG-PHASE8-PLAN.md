# CSG Phase 8 — Long Tail (Finishing / Hardening) — Implementation Plan

Status: implementation-ready. Execute steps in order; each step is independently green (dev build + all five doctest filters pass after each). This is the FINAL phase — a cleanup/hardening pass, not a feature phase.

## 0. Scope and hard constraints

Phase 8 finishes the CSG level-editing feature. It adds **no new tools, no new UX surfaces, no new activation paths, and no new provider capabilities.**

- **FROZEN LAYER — DO NOT MODIFY:** `editor/gui/editor_edit_domain.{h,cpp}` (the generic edit-domain Registry/Provider/Host/Session/Context). Its test suite `*EditorEditDomain* 6/6 (85)` MUST NOT grow or change. Every change in this plan lives in the CSG module TU (`modules/csg/**`) or the CSG editor TU (`modules/csg/editor/**`) or the module tests — never the generic layer.
- **Established invariants that every step must preserve** (see the risks table, §7): Manifold handle collapse-on-evaluate (always evaluate a *copy* of a subtree/expression handle); `update_shape()` stays synchronous and unconditional; overrides never bump `surface_schema_generation`; brush-order picking (never pick against the render `ArrayMesh`); one undo action per gesture; ghost = view-local overlay only (no node mutation during drag).
- Baseline (current tree, Phase 7 committed): `*CSG* 49/49 (1411)`, `*CSGEditDomain* 14/14 (242)`, `*EditorEditDomain* 6/6 (85)`, `*EditorDocumentSurface* 2/2 (33)`, `*ResponsiveLayout* 2/2 (26)`.

## 1. Disposition of the frozen §28 long-tail list

§28 Phase 8 reads: "Cylinder and polygon surface editing, source/cutter visualization, diagnostics, linked baking, workplanes, advanced extrusion cases, performance-driven wide-tree optimizations, additional CSG primitives." Every one of these demands a new tool, a new UX surface, or benchmark-gated speculative work. All are **OUT OF SCOPE** for this finishing phase:

| §28 item | Ruling | Rationale |
|---|---|---|
| Cylinder & polygon surface (push/pull) editing | OUT | New per-primitive semantic push/pull tool behavior + gesture math beyond the box MVP (§17 is box-only). New tool surface. Provenance already supports it (Phase 2 cylinder/polygon faceIDs); the *editing* is a feature, deferred (§27). |
| Source/cutter visualization | OUT | New view-local overlay + contextual toggle = new UX surface (§27 "editable cutter/source visualization"). |
| Diagnostics (empty-result, invalid-manifold, coincident-surface warnings) | OUT | New UI/notification surface. Note: the DEV-only rebuild counters from Phase 0 (`csg_debug_counters`) already satisfy the §27 "boolean rebuild statistics in development builds" sliver; no further work in this phase. |
| Linked baking / one-click static-mesh bake | OUT | New workflow + new command/UX. `bake_static_mesh()` already exists as the runtime primitive; a linked-bake feature is deferred. |
| Workplanes (reusable) | OUT | New persistent editor object + UX (§23 "later additions"). |
| Advanced extrusion cases (inward/carve, inset, bevel, visible-fragment) | OUT | Explicitly deferred by §18 "Unsupported cases" and §32. Inward extrusion is a would-carve feature, not a finish. |
| Performance-driven wide-tree optimizations (balanced boolean-reduction trees) | OUT | §32 defers these "without benchmark evidence." No reference scene/benchmark exists yet (§30 leaves ms targets unset). Includes candidate (ii) below. |
| Additional CSG primitives | OUT | New node types = new feature, not a finish. |

What Phase 8 **does** deliver is the deferred cleanup/hardening backlog from the phase reviews — the six candidates (i)–(vi) below.

## 2. Rulings on candidates (i)–(vi)

- **(i) World-space UV refinalize on ancestor-only moves — DO.** Verified real correctness gap. World-space planar UVs project through the **root's global transform** only (`modules/csg/csg_shape.cpp:662`, `projection_to_root = p_root->get_global_transform().affine_inverse()`). The refinalize is currently hooked only on `NOTIFICATION_LOCAL_TRANSFORM_CHANGED` (`:1106–1113`), which does **not** fire when an *ancestor* of the CSG root moves (the root's local transform is unchanged, only its global). Result: move a parent Node3D of a CSG root that has world-space UVs and the UVs silently go stale until an unrelated edit. Correctness outweighs the perf note. Cheap: the walk `_has_world_surface_uv_settings()` already exists (`:492`), and `VisualInstance3D` already enables global-transform notifications (`scene/3d/visual_instance_3d.cpp:210` `set_notify_transform(true)`), so `NOTIFICATION_TRANSFORM_CHANGED` already reaches the node. Headlessly pinnable (Step 5).

- **(ii) Cached "has world-space settings" early-out — DEFER.** Consistent with the prior verdict ("profile first; invalidation surface too broad to be clearly-safe") and §32 ("no persistent optimization without benchmark evidence"). The `_has_world_surface_uv_settings()` walk does **zero booleans and zero repacks** — it only reads sparse surface settings over the visible CSG subtree and, at most, sets `materialization_dirty` (`_invalidate_materialization_and_ancestors`, `:440`). It therefore violates none of the §30 gates. Doing (i) increases how often the walk runs (now on ancestor moves too), but only where the world-space UV feature is actually authored under a moving ancestor, and self-move frequency is unchanged. A cached boolean would need invalidation hooks on every descendant surface-setting change, add/remove, reparent, and visibility toggle — a broad, error-prone surface for no measured benefit. Revisit only with a reference scene and profile data (§30 groundwork not yet laid).

- **(iii) Duplicated planar UV-axis math — DO (cross-TU refactor).** The axis-basis + rotation + meters-per-tile computation is byte-duplicated between the core evaluator (`modules/csg/csg_shape.cpp:642–669`) and the editor texture-lock helper (`modules/csg/editor/csg_edit_domain.cpp:115–125`). Extract one concrete method on `CSGPrimitive3D` returning the rotated, meters-scaled operand-local axes; both sites then apply their own projection basis. Behavior-preserving and already double-pinned by existing tests, so it is clearly-safe. DO (Step 4).

- **(iv) `GestureState::CANCEL` dead enumerator — DO.** Verified: defined at `modules/csg/editor/csg_edit_domain.h:92`, referenced nowhere (no writes since Phase 5, no reads ever; tests do not reference gesture states). Trivial, compile-only removal (Step 1).

- **(v) 3-/4-site snap-step fetch — DO (small dedup).** The `Node3DEditor::is_snap_enabled()/get_translate_snap()` fetch is repeated at four sites in `modules/csg/editor/csg_edit_domain.cpp` (`:905, :1026, :1097, :1195`). Collapse to one private session helper. Behavior-preserving; flagged by the Phase 7 review. Not independently headless-testable (depends on the `Node3DEditor` singleton, which is null in tests → helper returns 0), so verified by no-regression + review (Step 2).

- **(vi) Draw-plane box-face normal uses `basis.xform` not inverse-transpose — DO (one-line consistency fix).** At `modules/csg/editor/csg_edit_domain.cpp:938` the authored-box-face branch computes the world normal as `box->get_global_transform().basis.xform(outward)`, while the sibling generic-pick branch 8 lines above (`:930`) already correctly uses `basis.inverse().transposed().xform(...)`. For a planar face normal under non-uniform scale, inverse-transpose is the correct transform. Making `:938` match `:930` removes the imprecision and the inconsistency. Requires a live viewport, so not headless-testable without adding a viewport dependency — verified by inspection against the already-correct sibling line (Step 3). One line.

## 3. Ordered migration steps

Steps are ordered safest→highest-nuance; they are largely independent (touch disjoint code) but must each land green before the next. After **every** step run all five filters (§5) and confirm no unexpected deltas.

### Step 1 — Remove dead `GestureState::CANCEL` (item iv)

- **File/where:** `modules/csg/editor/csg_edit_domain.h:86–93`.
- **Change:** Delete the `CANCEL,` enumerator at `:92`. Leave `IDLE, HOVER, PRESSED, DRAGGING, COMMIT`.
- **Test:** None added; compile + existing `*CSGEditDomain* 14/14 (242)` unchanged.
- **Risk:** None. Confirmed zero references module-wide and in `tests/editor/test_csg_edit_domain.cpp`. (Note: `COMMIT` is written at `:1285, :1328` but never read; leave it — it is a meaningful transient and removing a written state is out of the clearly-safe bar for this pass. Do not touch it.)

### Step 2 — Extract translate-snap-step helper (item v)

- **Files/where:** `modules/csg/editor/csg_edit_domain.h` (declare private helper on `CSGSurfaceSession`, near the other private helpers); `modules/csg/editor/csg_edit_domain.cpp`.
- **Change:** Add
  ```cpp
  real_t CSGSurfaceSession::_active_translate_snap_step() const {
      Node3DEditor *node_3d_editor = Node3DEditor::get_singleton();
      if (node_3d_editor && node_3d_editor->is_snap_enabled()) {
          return node_3d_editor->get_translate_snap();
      }
      return 0.0;
  }
  ```
  Replace the four fetch sites, preserving each site's own downstream guard:
  - `:905–910` (`_project_draw_point`): `const real_t snap_step = _active_translate_snap_step(); if (snap_step > 0.0) { snap x and y }`.
  - `:1026–1032` (`_update_draw_height`): `const real_t snap_step = _active_translate_snap_step(); if (snap_step > 0.0) { snap draw_height }` — keep the CSG-7 comment.
  - `:1097–1100` (`_get_draw_min_extent`): `const real_t snap_step = _active_translate_snap_step(); if (snap_step > 0.0) { minimum_extent = MAX(snap_step, minimum_extent); }`.
  - `:1195–1201` (`_update_drag`): `const real_t snap_step = _active_translate_snap_step(); if (snap_space == SnapSpace::LOCAL && snap_step > 0.0) { snap target_plane_coordinate }` — **keep the `snap_space == SnapSpace::LOCAL` guard**; only Local space snaps.
- **Test:** None (helper returns 0 headlessly; pure refactor). Verified by no-regression on all suites.
- **Risk:** Low. Watch the site-4 `SnapSpace::LOCAL` condition — it must remain; do not fold it into the helper.

### Step 3 — Box-face draw-plane normal uses inverse-transpose (item vi)

- **File/where:** `modules/csg/editor/csg_edit_domain.cpp:938`, inside `CSGSurfaceSession::_resolve_draw_plane`.
- **Change:** Replace
  `draw_plane_normal_world = box->get_global_transform().basis.xform(outward).normalized();`
  with
  `draw_plane_normal_world = box->get_global_transform().basis.inverse().transposed().xform(outward).normalized();`
  to match the already-correct generic branch at `:930`.
- **Test:** None (requires a live `Node3DEditorViewport`; not headless-testable without adding a viewport dependency, which is out of scope). Verified by inspection against `:930`.
- **Risk:** Very low. `outward` is a unit axis in box-local space; under uniform scale/rotation inverse-transpose equals the plain basis up to normalization, so already-passing scenes are unaffected; only non-uniformly-scaled boxes change (and become correct). The subsequent `is_zero_approx` guard (`:941`) and `.normalized()` remain.

### Step 4 — Share the planar UV-axis math across TUs (item iii)

- **Files/where:** `modules/csg/csg_shape.h` (declare on `CSGPrimitive3D`, adjacent to `get_surface_uv_basis` at `:319`); `modules/csg/csg_shape.cpp` (define + call at `:642–669`); `modules/csg/editor/csg_edit_domain.cpp` (call at `:115–125`).
- **Change:** Add a concrete (non-virtual) method that returns the rotated, meters-scaled operand-local axes — the exact shared prefix of both sites:
  ```cpp
  // csg_shape.h, public on CSGPrimitive3D:
  void get_surface_planar_uv_axes(uint32_t p_surface, const CSGSurfaceSetting &p_setting,
          Vector3 &r_axis_u, Vector3 &r_axis_v) const;
  ```
  ```cpp
  // csg_shape.cpp:
  void CSGPrimitive3D::get_surface_planar_uv_axes(uint32_t p_surface, const CSGSurfaceSetting &p_setting,
          Vector3 &r_axis_u, Vector3 &r_axis_v) const {
      Vector3 axis_u, axis_v;
      get_surface_uv_basis(p_surface, axis_u, axis_v);
      const real_t cos_rotation = Math::cos(p_setting.rotation);
      const real_t sin_rotation = Math::sin(p_setting.rotation);
      const Vector3 rotated_u = axis_u * cos_rotation + axis_v * sin_rotation;
      const Vector3 rotated_v = axis_v * cos_rotation - axis_u * sin_rotation;
      const real_t meters_u = Math::is_zero_approx(p_setting.meters_per_tile.x) ? 1.0 : p_setting.meters_per_tile.x;
      const real_t meters_v = Math::is_zero_approx(p_setting.meters_per_tile.y) ? 1.0 : p_setting.meters_per_tile.y;
      r_axis_u = rotated_u / meters_u;
      r_axis_v = rotated_v / meters_v;
  }
  ```
  - In `_gather_manifold_surface_records` (`:642–669`): replace the axis/rotation/meters block with a call, then keep the per-space `projection_to_root` switch (`:653–664`) unchanged and set `resolved_uv.axis_u = projection_to_root.basis.xform(scaled_u); resolved_uv.axis_v = projection_to_root.basis.xform(scaled_v);` (`:668–669`). `resolved_uv.origin/offset` unchanged.
  - In `csg_texture_lock_compensate_offset` (`:115–125`): replace the block with a call, then `const Vector3 resolved_u = p_operand_to_root.basis.xform(scaled_u); const Vector3 resolved_v = p_operand_to_root.basis.xform(scaled_v);` (`:124–125`). The gate at `:111` and the dot-product offset at `:126` are unchanged.
- **Test:** No new test required. The refactor is byte-pinned in both TUs by existing coverage: module `*CSG*` Phase 6 planar-UV cases (`test_csg.h:1370, :1423, :1461, :1495`) and editor `*CSGEditDomain*` "Local planar texture lock compensates a one-sided push pull" (`test_csg_edit_domain.cpp:470–496`). Any drift breaks them. (Optional: one extra `CHECK` in an existing editor case asserting the helper's axes match; not required and would change the 242 assertion count — prefer to leave counts fixed.)
- **Risk:** Low. Output must be byte-identical; the existing exact-value UV tests are the tripwire. Ensure `get_surface_planar_uv_axes` is const and reachable from the editor TU (it already includes `csg_shape.h`).

### Step 5 — World-space UV refinalize on ancestor moves (item i)

- **Files/where:** `modules/csg/csg_shape.cpp` `_notification` (`:1064–1149`); `modules/csg/tests/test_csg.h` (new case).
- **Change (primary — consolidate onto the global transform notification):**
  1. In `NOTIFICATION_LOCAL_TRANSFORM_CHANGED` (`:1106–1113`), keep only `_make_transform_dirty();` (the wrapper invalidation, which correctly depends on the *local* transform only). Remove the `is_root_shape() && _has_world_surface_uv_settings()` world-UV block from here.
  2. Move `case NOTIFICATION_TRANSFORM_CHANGED:` **out of** the `#ifndef PHYSICS_3D_DISABLED` guard (it is currently inside it at `:1142–1147`) so it compiles regardless of physics, and add the world-UV refinalize there, keeping the physics body/debug work physics-guarded:
     ```cpp
     case NOTIFICATION_TRANSFORM_CHANGED: {
         // World-space planar UVs project through the root's GLOBAL transform, so an
         // ancestor-only move (which never fires LOCAL_TRANSFORM_CHANGED) must still
         // refinalize them. Zero booleans/repacks: only materialization is invalidated.
         if (is_root_shape() && _has_world_surface_uv_settings()) {
             _make_material_dirty();
         }
     #ifndef PHYSICS_3D_DISABLED
         if (use_collision && is_root_shape() && root_collision_body.is_valid()) {
             PhysicsServer3D::get_singleton()->body_set_state(root_collision_body, PhysicsServer3D::BODY_STATE_TRANSFORM, get_global_transform());
         }
         _on_transform_changed();
     #endif // PHYSICS_3D_DISABLED
     } break;
     ```
     Keep `NOTIFICATION_ENTER_TREE`/`NOTIFICATION_EXIT_TREE` inside the physics guard as they are.
  - Rationale for consolidation: a self local-transform change also changes the global transform, so `NOTIFICATION_TRANSFORM_CHANGED` covers both self-moves and ancestor-moves; handling world UVs there once avoids double-invalidation. The global notification is delivered on `SceneTree::flush_transform_notifications()` (one frame later than the synchronous LOCAL notification); this latency is immaterial because CSG output finalization is already deferred/async (§14/§15) and `_make_material_dirty` only queues a root update.
  - **Conservative fallback (only if Step-5 verification surfaces any test depending on *synchronous* self-move refinalize):** instead of removing the block from LOCAL, keep it in LOCAL *and* add the global handler. Double-fire is idempotent — `_queue_root_update` (`:407`) only queues a new deferred when `!dirty`, so the second call coalesces. Prefer the primary consolidation; document if the fallback is used.
- **Test (new case in `test_csg.h`), `[SceneTree][CSG] Phase 8 world-space planar UVs refinalize on ancestor move`:** Model on the existing `:1495` case but drive the notification path instead of calling `update_shape()` directly:
  1. `Node3D *parent = memnew(Node3D);` add to `SceneTree::get_singleton()->get_root()`.
  2. `CSGBox3D *box` with size (2,2,2), `SURFACE_UV_MODE_PLANAR` + `SURFACE_UV_SPACE_WORLD` on `SURFACE_POSITIVE_Z`, distinct planar material; `parent->add_child(box)`. Confirm `box->is_root_shape()` (parent is not a CSGShape3D).
  3. `MessageQueue::get_singleton()->flush();` then `box->update_shape();` to reach a clean, materialized baseline; verify world mapping with a `bake_static_mesh()` + `_get_surface_arrays_for_material` helper as at `:1516`.
  4. `_reset_csg_counters();`
  5. `parent->set_position(Vector3(...));` — **do not** touch `box`.
  6. `SceneTree::get_singleton()->flush_transform_notifications();` (delivers `NOTIFICATION_TRANSFORM_CHANGED` to `box`, which has `notify_transform` via `VisualInstance3D`) then `MessageQueue::get_singleton()->flush();` (runs the queued `_update_shape_deferred`).
  7. Assert `#ifdef DEV_ENABLED` `counters.uv_finalizations == 1` and the +Z-face UVs now equal the box vertex XY plus the **new global** origin (i.e. UVs tracked the ancestor move). `queue_free()` both.
  - Discriminator check: with the fix reverted this case fails at step 7 (`uv_finalizations == 0`, stale UVs) because the ancestor move never queues a rebuild; with the fix it passes.
  - The existing `:1495` case stays green: it moves the box itself and calls `update_shape()` explicitly (which is unconditional and does not depend on notification timing), and it does not flush the message queue, so its `root_materializations == 1`/`batch_boolean_calls == 0` assertions are unaffected by moving the trigger from LOCAL to global.
- **Risk:** Moderate-low.
  - `_make_material_dirty` runs zero booleans (only sets `materialization_dirty`, `:440–446`) — preserves §30 "material/UV changes run zero booleans." Confirm the new case shows `batch_boolean_calls == 0`.
  - Verify the physics-disabled build still compiles (the `NOTIFICATION_TRANSFORM_CHANGED` label now lives outside the guard; only its body is guarded). `_on_transform_changed` stays inside `#ifndef PHYSICS_3D_DISABLED` (`:1030–1036`) — do not call it from the unguarded region.
  - Gate correctness: `_has_world_surface_uv_settings()` (`:492`) checks only `PLANAR + WORLD`; Local/Root spaces are invariant under whole-tree moves and correctly excluded — do not broaden it.

## 4. Files touched (summary)

- `modules/csg/csg_shape.h` — Step 4 (helper decl).
- `modules/csg/csg_shape.cpp` — Step 4 (helper def + call site), Step 5 (notification restructure).
- `modules/csg/editor/csg_edit_domain.h` — Step 1 (enum), Step 2 (helper decl).
- `modules/csg/editor/csg_edit_domain.cpp` — Step 2 (helper def + 4 call sites), Step 3 (normal), Step 4 (call site).
- `modules/csg/tests/test_csg.h` — Step 5 (one new case).
- **Not touched:** `editor/gui/editor_edit_domain.{h,cpp}` (frozen), `csg_evaluation.{h,cpp}`, `csg_evaluation_scheduler.{h,cpp}`, `tests/editor/test_csg_edit_domain.cpp` (no new editor cases needed).

## 5. Test plan and expected counts

Run after each step; final expected state:

| Filter | Baseline | After Phase 8 | Delta |
|---|---|---|---|
| `*CSG*` | 49/49 (1411) | **50/50 (1411 + N)** | +1 case (Step 5). N = the new case's assertions (report observed; ~a dozen, dominated by the per-vertex UV loop). |
| `*CSGEditDomain*` | 14/14 (242) | **14/14 (242)** | 0 (Steps 1–4 are behavior-preserving refactors pinned by existing cases). |
| `*EditorEditDomain*` | 6/6 (85) | **6/6 (85)** | 0 — MUST NOT grow (frozen layer untouched). |
| `*EditorDocumentSurface*` | 2/2 (33) | **2/2 (33)** | 0. |
| `*ResponsiveLayout*` | 2/2 (26) | **2/2 (26)** | 0. |

Only `*CSG*` changes: exactly one new case, no change to the 1411 baseline assertions (existing cases untouched). If any other filter's count moves, stop and investigate before continuing.

Test run command (per filter): `bin/godot.windows.editor.dev.x86_64.exe --test --test-case="*CSG*"` (and each of `*CSGEditDomain*`, `*EditorEditDomain*`, `*EditorDocumentSurface*`, `*ResponsiveLayout*`).

## 6. Build commands

Shared tree, multiple concurrent agent sessions — obey the ledger rules:

- **Before ANY build**, check for a running SCons: `Get-CimInstance Win32_Process -Filter "Name='python.exe'"` (look for SCons in the command line). Never run two builds of the same object namespace concurrently (phantom LNK1120s). The dev namespace (`.dev.`) is safe alongside a production build but not alongside another dev build.
- **Dev + unit tests build:**
  `scons platform=windows target=editor dev_build=yes tests=yes d3d12=yes winrt=no -j24`
  - `winrt=no` is REQUIRED (MSVC 14.51 hard-errors STL1011 on `/await` + `<experimental/coroutine>`).
  - Long builds: run in background and poll; a single foreground call times out at 10 minutes.
  - Shell pipelines mask SCons's exit code — check `$LASTEXITCODE` directly.
  - A running editor holds the exe lock; the final link fails while output looks fine — check the binary timestamp; **rename** (don't kill) a locking editor exe. If a locked `bin\D3D12Core.dll`/`d3d12SDKLayers.dll` is renamed aside, note the `.locked-*` files for the user to delete once other editors exit.
- **Git:** stage files explicitly, **never `git add -A`** — other sessions' uncommitted work shares the tree. One commit for Phase 8 (the five files above + the ledger) with the `Co-Authored-By` trailer, after the simplify pass is green.

## 7. Risks and gotchas

| Risk / invariant | Where it bites | Mitigation |
|---|---|---|
| **Manifold handle collapse-on-evaluate** | Any code that evaluates a cached subtree/expression handle (`GetMeshGL64`, `IsEmpty`, `BoundingBox`) collapses it into a leaf, destroying expression identity. | Phase 8 touches none of the evaluation core; keep it that way. Do not add handle evaluation in the notification path — Step 5 only sets a dirty flag. |
| **`update_shape()` stays synchronous & unconditional** | Step 5. `update_shape()` (`:788`) always rebuilds when called; it is not dirty-gated. | Do not gate it on dirty. Step 5's test must drive the *notification/deferred* path (`flush_transform_notifications` + `MessageQueue::flush`), not call `update_shape()`, or the bug won't be observable. |
| **Overrides never bump `surface_schema_generation`** | Steps 4–5 touch surface-setting reads. | Neither step writes schema generation. Picking stability across paint/undo depends on this — do not add a generation bump. |
| **Brush-order picking, never the render ArrayMesh** | Step 3 (draw-plane normal). | Step 3 changes only the face-normal transform, not the pick source (`_pick_for_draw` still uses brush order). Do not reroute picking. |
| **One undo action per gesture** | None of Steps 1–5 alter transactions. | Confirm the `*CSGEditDomain*` Draw/Extrusion/Paint undo cases stay at 242 assertions. |
| **Ghost = view-local overlay only** | Step 2/3 (draw session). | No node mutation added mid-gesture; snap helper and normal fix are pure computation. |
| **Zero booleans on material/UV invalidation (§30)** | Step 5 (`_make_material_dirty`). | `_invalidate_materialization_and_ancestors` (`:440`) sets only `materialization_dirty`; assert `batch_boolean_calls == 0` in the new case. |
| **Physics-disabled build** | Step 5 moves `NOTIFICATION_TRANSFORM_CHANGED` outside `#ifndef PHYSICS_3D_DISABLED`. | Keep the body's physics/`_on_transform_changed` work inside the guard; verify a `PHYSICS_3D_DISABLED` compile if feasible, or at minimum reason it through (label unguarded, body guarded). |
| **Over-invalidation via world-UV walk on animated ancestors (candidate ii)** | Step 5 increases walk frequency. | Accepted: walk is O(subtree) settings-read, zero booleans/repacks, only where world-space UVs are authored. Do NOT add a cached flag (candidate ii DEFER) — its invalidation surface is unproven and out of the clearly-safe bar. |
| **Byte-identical UV refactor (Step 4)** | Shared helper must reproduce both sites exactly. | Existing exact-value UV tests (`test_csg.h:1370/1423/1461/1495`, `test_csg_edit_domain.cpp:470–496`) are the tripwire; run both filters after Step 4. |
| **Frozen generic layer** | Any accidental edit to `editor/gui/editor_edit_domain.{h,cpp}`. | `*EditorEditDomain*` must stay exactly 6/6 (85). If it moves, revert. |
| **Shared tree / concurrent builds** | Build/link phantom failures, exe lock. | Check for running SCons first; rename locking editors; stage files explicitly; never `git add -A`. |

## 8. Done criteria

- Steps 1–5 landed, each independently green.
- `*CSG* 50/50 (1411 + N)`; `*CSGEditDomain* 14/14 (242)`; `*EditorEditDomain* 6/6 (85)`; `*EditorDocumentSurface* 2/2 (33)`; `*ResponsiveLayout* 2/2 (26)`.
- Candidate rulings recorded: (i) DONE, (ii) DEFER, (iii) DONE, (iv) DONE, (v) DONE, (vi) DONE; frozen §28 feature list marked OUT with rationale (§1).
- Opus `/simplify` pass over the Phase 8 diff, then single Phase 8 commit (five source files + ledger), `Co-Authored-By` trailer, explicit staging.
- Ledger updated: Phase 8 status DONE, candidate dispositions folded in, world-UV ancestor-move fix and the four cleanups noted; the CSG edit feature is complete.
