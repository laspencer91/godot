# CSG Phase 4 — Evaluation Scheduler + Box Surface Tool (push/pull)

**Scope.** Two decoupled deliverables landing on committed seams: (A) a **module-side single-flight async evaluation scheduler** that drives root re-evaluation off-thread through the Phase-4-pre-step seam (`csg_build_snapshot` / `_gather_evaluation_inputs` / `_publish_snapshot`), with generation coalescing, interactive-vs-final quality, stale-result rejection, and a preserved synchronous path for tests/headless; and (B) the **first REAL edit domain** — a `CSGEditDomainProvider`/`Session` registered on the landed 3B registry, delivering the box Surface tool MVP (hover-highlight, click-select face, push/pull drag along the face normal with grid snapping, a view-local ghost, commit-on-release through undo/redo, Escape cancel). **Box only.** The frozen plan (§12–§19, §28 Phase 4, §29, §30) is the design target; this plans its implementation, it does not redesign it.

**Hard constraints (state to Codex, enforce at review):**
- Module work only under `modules/csg/**`; editor/tool work under `modules/csg/editor/**` (globbed by `SCsub:49` under `if env.editor_build`) and `editor/**`; new tests under `modules/csg/tests/**` and `tests/editor/**`.
- The CSG **module core** (`modules/csg/*.cpp`) must not include editor headers (plan §3). The scheduler is module-core. The domain provider/session live under `modules/csg/editor/` (which already depends on editor types — see `csg_gizmos.h`).
- Dev build: check `Get-Process python` (shared tree — another SCons must not be running) then `scons platform=windows target=editor dev_build=yes tests=yes d3d12=yes winrt=no -j24`. `winrt=no` is mandatory (STL1011). A running editor holds the exe lock — rename, don't kill.
- Stage files explicitly; **never `git add -A`**; leave changes uncommitted. `workspace-editor-planning/` is read-only.
- The `*CSG*` suite (18 cases / 912 assertions) plus the counter pins in `modules/csg/tests/test_csg.h` must stay green **unmodified** except for the additive new cases described in §6. `*EditorEditDomain*`, `*EditorDocumentSurface*`, `*ResponsiveLayout*` remain green.

---

## 1. Current-state map (verified against the live tree, 2026-07-22)

### 1.1 Module seam (Phase 4 pre-step, committed `ea4759315f`)

All refs `modules/csg/` unless noted.

- **`csg_evaluation.h`** — the detachable value types are defined and the pure free functions declared:
  - `CSGEvaluationSettings` (`:77-83`): `autosmooth`, `smoothing_angle`, `calculate_tangents`, `want_collision`, `want_render`.
  - `CSGEvaluationInputs` (`:85-100`): `manifold::Manifold subtree` (a **copy** of the cached handle — evaluation collapses its receiver, comment `:86-88`); `mesh_materials` (token→`Ref<Material>`), `surface_keys` (token→`CSGSurfaceKey`); `settings`; `root_id`; `schema_generation`; `request_generation`; `want_result_metadata`.
  - `CSGEvaluationSnapshot` (`:102-155`): owns `CSGBrush *brush`; `result_surface_keys`; `result_triangles`; `node_aabb`; `subtree_empty`; `render_surfaces`; `collision_faces`; `built_render`; `built_tangents`; `root_id`/`schema_generation`/`request_generation`. **Non-copyable, movable** (move-assign transfers brush ownership, frees prior). Dtor frees an unpublished brush. **This is the object that crosses the thread boundary.**
  - `CSGEvaluationSnapshot csg_build_snapshot(const CSGEvaluationInputs &)` (`:164`) — the pure, node-free orchestrator (stages 3–8 + collision extraction). **Currently defined but unused by the synchronous path** — it is the exact worker entry Phase 4 calls off-thread.
- **`csg_shape.cpp`** — the split methods:
  - `_gather_evaluation_inputs(bool want_render, bool want_collision)` (`:533-551`): main-thread; `_ensure_subtree_manifold()`, `_gather_manifold_surface_records(...)`, **copies** `subtree_manifold`, fills settings from node fields, sets `root_id = get_instance_id()`, `schema_generation`, `request_generation = result_generation`, `want_result_metadata = is_root_shape()`. **This is the "touches nodes" boundary — everything after it is thread-safe.**
  - `_get_brush()` (`:585-632`): dirty early-out (`:586-589`, must stay zero-work on clean cache); gather; materialize via `csg_materialize_brush`; **root-only** metadata publish + `result_generation` bump (`:606-613`); AABB; emptiness; `_update_child_manifold_aabbs`; warnings. Counters `:598-602`.
  - `update_shape()` (`:634-665`): root-only; `_get_brush()`; assemble `CSGEvaluationSettings` (note: re-assembles what gather already built — the pre-step /simplify flagged this as a Phase 4 seam change); `count_uv_finalization`; `csg_build_render_surfaces`; conditional `csg_extract_collision_faces`; `_publish_snapshot(snapshot)`. **Fully synchronous today.**
  - `_publish_snapshot(CSGEvaluationSnapshot &)` (`:667-741`): if `snapshot.brush` is non-null, swaps brush + metadata onto the node and bumps generation (dormant on the sync path, which leaves `snapshot.brush == nullptr`); if `built_render`, does `set_base(RID())` → rebuild `ArrayMesh` → `set_base(rid)` → `update_gizmos()`; if collision, `root_collision_shape->set_faces(...)` + `count_collision_rebuild`. **This is the main-thread landing Phase 4 reuses verbatim.**
  - `_queue_root_update(bool force)` (`:321-331`): walks to root, `callable_mp(root, &CSGShape3D::update_shape).call_deferred()` guarded by `root->dirty`. **This is the single funnel for all model-driven root updates** — the coalescing hook point.
  - `resolve_result_triangle(triangle, generation, ...)` (`:282-304`): validates `is_root_shape() && generation == result_generation && triangle < result_triangles.size()`, then token→key. **The picking backbone (Phase 2).**
- **`csg_manifold_cache.h`** — node-resident mutable cache: retained `local/transformed/subtree_manifold` handles, origin-token range, `result_surface_keys`/`result_triangles`, dirty flags. Stays on the node.
- **`csg_debug_counters.h`** (`:37-64`) — 11 counters. **No scheduler counters exist yet** (plan §28 Phase 0 lists "scheduler requests, completions, coalesces, stale drops" — Phase 4 adds them).
- **Synchronous consumers that must not change semantics:** `get_brush_faces()` (public; used by gizmos + 20+ tests), `_get_brush_collision_faces()`/`bake_collision_shape()` (`:752-770`), non-root `_get_brush`. Tests call `root->update_shape()` then **immediately** read `get_result_generation()` / `resolve_result_triangle()` (e.g. `test_csg.h:537-547`). **Therefore `update_shape()` must remain synchronously complete.**

### 1.2 Editor seam (3B, committed `ef5aa45776`)

- **`editor/gui/editor_edit_domain.{h,cpp}`** — `EditorEditDomainRegistry` (singleton, `register_provider`/`unregister_provider`/`get_provider`/`find_double_click_provider`/`get_available_providers`), `EditorEditDomainProvider` (pure: `get_domain_id`/`is_available`/`can_activate_from_double_click`/`create_session`), `EditorEditDomainSession : Object` (virtuals: `enter`/`exit`/`retarget`/`handle_input`→tri-state/`handle_escape`/`handle_tool_toggle`/`draw_overlay`/`build_tool_rail`→SLOT_CENTER_LEFT/`build_contextual_panel`→SLOT_CENTER_RIGHT), `EditorEditDomainHost` (per-pane, owns session, `enter_domain`/`exit_domain`/`try_activate_from_double_click`/`route_input`/`route_draw`/`notify_provider_unregistered`). Tri-state enum `EditorEditDomainInput { PASS_TO_VIEWPORT, BLOCK_NATIVE_EDIT, CONSUMED }`. Registry created at `editor/register_editor_types.cpp:163`, freed `:371`. DEV dummy domain proves the seam.
- **`editor/scene/3d/node_3d_editor_viewport.cpp`** — arbitration hook at `:2846-2872` (after global-plugin forwarding, before RMB/nav): resets `domain_blocks_native`, routes to `editor_view->get_edit_domain_host()`, CONSUMED → `_neutralize_click_state()`+`accept_event()`+return, BLOCK → `domain_blocks_native = true` + `after = AFTER_GUI_INPUT_CUSTOM`, and the not-active double-click activation path (`:2865-2872`). Suppression guards at `:3043`/`:3079` (node-attached gizmo pick, global `is_edit_domain_active_anywhere()`), `:3074`/`:3165`/`:3214`/`:3374` (per-viewport transform via `domain_blocks_native`), `:3340` (hover). Overlay forward at `:4718`. Helpers `_domain_pane_accepts_input()` (`:2231`), `_neutralize_click_state()` (`:2251`).
- **`editor/scene/3d/node_3d_editor_plugin.{h,cpp}`** — `Node3DEditorView` owns `edit_domain_host`; `Node3DEditor::is_edit_domain_active_anywhere()` (`plugin.cpp:2735`). Snap source: `Node3DEditor::get_singleton()->is_snap_enabled()` (`plugin.h:481`, = `snap_enabled ^ snap_key_enabled`) / `get_translate_snap()` (`plugin.cpp:4430`).
- **`editor/gui/editor_viewport_chrome.h`** — `SLOT_CENTER_LEFT`/`SLOT_CENTER_RIGHT` present; `add_control`/`remove_control` public.

### 1.3 Picking primitives (verified)

- `Node3DEditorViewport::get_ray_pos(pos)` (`:950`) + `get_ray(pos)` (`:958`) give world ray origin + direction.
- `TriangleMesh::intersect_ray(begin, dir, r_point, r_normal, r_surf_index=nullptr, r_face_index=nullptr)` (`core/math/triangle_mesh.h:87`) — **returns the triangle ordinal `r_face_index`.**
- `CSGShape3D::get_brush_faces()` returns the materialized brush faces **in brush-face order, 1:1 with `result_triangles`** (both stamped in the same `csg_materialize_brush` loop). **A `TriangleMesh` built from `get_brush_faces()` therefore yields `r_face_index == result-triangle index`, which feeds `resolve_result_triangle(face_index, generation, ...)` directly.** This is the Phase 4 picking accel — see §3.4. (Do **not** use `generate_triangle_mesh()` / `root_mesh`: that TriangleMesh is built from the render `ArrayMesh`, whose triangles are regrouped/split by material and are **not** index-aligned to `result_triangles`.)
- `WorkerThreadPool::add_native_task(void(*)(void*), void*, high_priority, desc)` + `is_task_completed(id)` + `wait_for_task_completion(id)` (`core/object/worker_thread_pool.h:245-250`) — the off-thread job mechanism.

---

## 2. Scheduler design (module core)

### 2.1 Ownership — per-root, module-owned (plan §15)

Plan §15: *"The scheduler belongs to the CSG module and is shared by every view of a root. Per root it stores: requested generation, published generation, one running job, one latest pending snapshot, requested quality."* This is **per-root-node state**. Implement it as a small owned struct instantiated lazily on the **root** `CSGShape3D` (the node that already owns `result_generation`, `manifold_cache`, `root_mesh`). Do not make it a module singleton (would need root→state mapping and defeats "shared by every view of a root" being naturally the root node). Do not put it on the session (session is view-owned; multiple panes share one root's scheduler).

New file `modules/csg/csg_evaluation_scheduler.h` (+ `.cpp`), module-core (auto-globbed by `SCsub:46`, no SCsub edit). It includes only `csg_evaluation.h`, `core/object/worker_thread_pool.h`, `core/os/mutex.h` — **no editor headers, no `csg_shape.h` method bodies.** The scheduler is a plain owner (not `Object`), held by pointer on `CSGShape3D` (mirrors `ManifoldCache *manifold_cache`), created on first async request when `is_root_shape()`, destroyed in the node dtor.

```cpp
enum class CSGEvalQuality { INTERACTIVE, FINAL };

class CSGEvaluationScheduler {
public:
    // Called on the main thread. Coalesces; launches at most one worker job.
    void request(CSGEvaluationInputs &&inputs, CSGEvalQuality quality);
    // Called on the main thread each idle/poll (or by the deferred landing).
    // Returns a ready snapshot to publish, or none. Relaunches pending if idle.
    bool try_take_completed(CSGEvaluationSnapshot &r_snapshot);
    bool has_work() const;            // running || pending
    void cancel_and_flush();          // node teardown: detach, discard results

private:
    // Per plan §15 state:
    uint64_t requested_generation = 0;
    uint64_t published_generation = 0;
    WorkerThreadPool::TaskID running_task = WorkerThreadPool::INVALID_TASK_ID;
    // Job payload (heap; owned by the running job until landing):
    struct Job { CSGEvaluationInputs inputs; CSGEvaluationSnapshot result; SafeFlag done; };
    Job *running_job = nullptr;
    bool has_pending = false;
    CSGEvaluationInputs pending_inputs;
    CSGEvalQuality pending_quality = CSGEvalQuality::INTERACTIVE;
    Mutex mutex; // guards the pending/running handshake against the deferred landing
    static void _run(void *p_job); // trampoline: p->result = csg_build_snapshot(p->inputs); p->done.set();
};
```

### 2.2 Request / publish generation protocol (vs the existing `result_generation`)

There are **two distinct generation counters — keep them separate and do not conflate:**
- **`request_generation`** (scheduler-owned, monotonic per root): assigned by the scheduler at each `request()` (`++requested_generation`). Stamped into `inputs.request_generation` before the job launches, carried through the snapshot. This is the *coalescing / stale-rejection* key.
- **`result_generation`** (node-owned, existing): the **published** picking generation, bumped in `_publish_snapshot` / `_get_brush` exactly as today. This is what `resolve_result_triangle` validates. It advances **only on the main thread at publish time**, never on the worker.

Publish acceptance test (main thread, in the landing): accept a completed snapshot **iff** `snapshot.request_generation == scheduler.requested_generation` (it is the newest request) **and** `snapshot.root_id == get_instance_id()` **and** `snapshot.schema_generation == surface_schema_generation`. Otherwise **discard** (stale — a newer request superseded it or the schema changed). On accept, `published_generation = snapshot.request_generation`, then the node bumps `result_generation` as part of the metadata swap.

Coalescing rule (plan §15): while a job runs, `request()` stores `pending_inputs`/`pending_quality` (overwriting any prior pending — latest-request-wins) and returns. **Final supersedes a queued interactive** (if a pending interactive exists and a FINAL request arrives, replace with FINAL; an interactive request does **not** downgrade a pending FINAL). When the running job completes and lands, if `has_pending`, immediately launch the pending as the next job.

### 2.3 Job lifecycle states

`IDLE → LAUNCHED(running_task valid) → COMPLETED(done flag set, awaiting main-thread landing) → LANDED(published or discarded) → (pending? LAUNCHED : IDLE)`.

- **Launch:** `request()` on an idle scheduler builds a heap `Job{ std::move(inputs) }`, `running_job = job`, `running_task = WorkerThreadPool::get_singleton()->add_native_task(&_run, job, /*high_priority*/ true, "csg_eval")`. `_run` calls `job->result = csg_build_snapshot(job->inputs); job->done.set();` — **pure, node-free** (see §2.4).
- **Landing:** driven from the main thread. **Use `call_deferred` on a node method, not raw MessageQueue** (matches the existing `_queue_root_update` idiom at `:328` and stays on the node's lifetime): the node registers a deferred `_poll_scheduler()` that, under the scheduler mutex, checks `running_job->done.is_set()`; if set, `WorkerThreadPool::wait_for_task_completion(running_task)` (guaranteed immediate — thread already finished; required to release the pool slot), moves `result` out, clears running state, launches pending if any, and returns the snapshot to `_publish_snapshot`. If not set yet, re-`call_deferred(_poll_scheduler)` (self-reschedule) so the poll continues next idle without blocking. **No busy-wait, no foreground sleep.** (Rationale for deferred-poll over a completion callback: WorkerThreadPool has no main-thread completion signal; the editor already pumps deferred calls every frame; and it keeps *all* server/node mutation on the main thread per plan §15.)

### 2.4 Thread-safety boundary (plan §15 threading contract — enforce strictly)

**The worker (`_run` → `csg_build_snapshot`) may touch only the `CSGEvaluationInputs` value and produce a `CSGEvaluationSnapshot` value.** It must not touch: scene nodes, `Object`, `Node3D` transforms, `RenderingServer`, `PhysicsServer`, `EditorSelection`, `Engine`, or any node method. This is already true of `csg_build_snapshot` today (it is pure) — Phase 4's job is to **not regress it**. Specific rules:
- **`manifold::Manifold subtree` is a copy** taken in `_gather_evaluation_inputs` (`:540`). The worker evaluates the copy (`GetMeshGL64`/`IsEmpty`/`BoundingBox` collapse the receiver — the cached node handle is never touched). This is the single most important invariant; it is already commented at four sites — do not remove those comments.
- **`Ref<Material>` is handle-only on the worker.** `csg_materialize_brush` stores the handles into `CSGBrush::materials` and groups by pointer identity; it never reads Resource contents. All material *resolution* (the `get_material()` walk) happens on the main thread in `_gather_manifold_surface_records` before launch. **Do not add any `Ref` refcount churn on the worker beyond what the pre-step already does** (Godot `Ref` refcounting is atomic, so copying handles is safe, but keep it to the gathered map).
- The single-flight guarantee (at most one job per root's shared lazy Manifold graph) is **required** because Manifold's lazy operation cache is mutable and not a concurrency contract (`thirdparty/manifold/src/csg_tree.h:80`). The scheduler enforces it; never launch a second job for the same root while `running_task` is valid.

### 2.5 Interactive vs final quality + the collision-payload-built flag (deferred-decision MUST-INCLUDE)

`request(inputs, quality)` sets, on a **copy** of settings inside `inputs`:
- **INTERACTIVE:** `settings.want_collision = false`, `settings.calculate_tangents = false` (skip tangents), `want_render = true`. Skips collision + tangents per §14/§29/§30.
- **FINAL:** `settings.want_collision = (use_collision && root_collision_shape.is_valid())`, `settings.calculate_tangents = calculate_tangents`, `want_render = true`.

**Add the explicit flag (deferred decision (a)):** extend `CSGEvaluationSnapshot` with `bool collision_built = false;` (and update the move-assign in `csg_evaluation.h:127-147` to carry it). `csg_build_snapshot` sets `collision_built = settings.want_collision` after (conditionally) filling `collision_faces`. **Reason:** once interactive snapshots skip collision, an empty `collision_faces` Vector cannot distinguish "collision intentionally skipped" from "valid empty collision result." `_publish_snapshot` must gate the collision `set_faces` + `count_collision_rebuild` on `snapshot.collision_built`, **not** on `!collision_faces.is_empty()`, so an interactive publish leaves the previously published collision shape untouched (satisfying §30 "collision does not rebuild during interactive drag"). Update `update_shape()` (sync path) to set `collision_built = settings.want_collision` so the synchronous behavior is byte-identical and the counter pins hold.

### 2.6 How the synchronous path stays exactly as-is

**`update_shape()` remains fully synchronous and unchanged in observable behavior.** The scheduler is a *new, additional* async entry point; it does **not** replace `update_shape()`. Concretely:
- Tests, headless, `get_brush_faces()`, `bake_*`, and the existing `_queue_root_update → call_deferred(update_shape)` path all continue to run `update_shape()`/`_get_brush()` synchronously. No test sees a thread.
- **Gating async:** the scheduler is used only when (i) running in the editor (`Engine::get_singleton()->is_editor_hint()`), and (ii) an explicit async request is made (by the CSG session, or by `_queue_root_update` electing the async route). For Phase 4 MVP the **only** async driver is the CSG session at gesture-commit and (optionally) interactive drag; ordinary model edits keep the synchronous deferred `update_shape`. This is the lowest-risk wiring and still satisfies §30 because the drag input handler never calls a synchronous boolean (it drives the ghost overlay; commit issues an async request).
- **Race safety between sync and async:** if a synchronous `update_shape()` runs while an async job is in flight (e.g. an Inspector edit during a drag), it bumps `result_generation`/`published_generation` on the main thread; the later async landing sees `snapshot.request_generation < requested_generation` **or** its schema/root check fails, and is **discarded**. Invariant: **`_publish_snapshot` and `update_shape` both run on the main thread and are mutually serialized by the deferred queue; only the pure build runs off-thread.**
- Provide a **synchronous fallback in the scheduler itself**: if `WorkerThreadPool` is unavailable or a global "force synchronous CSG" flag is set (headless/tests never construct a scheduler, but guard anyway), `request()` runs `csg_build_snapshot` inline and lands immediately. This keeps a deterministic path for the new headless scheduler tests (§6).

### 2.7 New counters (DEV only)

Add to `csg_debug_counters.h`: `scheduler_requests`, `scheduler_completions`, `scheduler_coalesces`, `scheduler_stale_drops` (+ `count_*` methods, `reset`/`get` fields). Increment in `request()` (requests; coalesces when a job was already running/pending), in the landing (completions), and on discard (stale_drops). These are the §28 Phase 0 scheduler counters; the new headless tests pin them. Do **not** touch the 11 existing counters' call sites.

---

## 3. Editor-side inventory (box Surface tool)

### 3.1 File placement (per fork convention)

CSG editor code lives in **`modules/csg/editor/`** (globbed by `SCsub:49`, already depends on editor types — `csg_gizmos.h` includes `editor/plugins/editor_plugin.h`). Add:
- `modules/csg/editor/csg_edit_domain.h` / `.cpp` — `CSGEditDomainProvider` (subclasses `EditorEditDomainProvider` from `editor/gui/editor_edit_domain.h`) and `CSGSurfaceSession` (subclasses `EditorEditDomainSession`), plus the tool state machine, picking accel, ghost, and undo commit.

Register the provider from the **module's** editor init (not from `editor/`, which must not depend on the module): in `modules/csg/register_types.cpp`, `initialize_csg_module(MODULE_INITIALIZATION_LEVEL_EDITOR)` (`:57-60`, where `EditorPlugins::add_by_type<EditorPluginCSG>()` runs) call `EditorEditDomainRegistry::get_singleton()->register_provider(memnew(CSGEditDomainProvider))` (store the pointer statically); unregister + `memdelete` in `uninitialize_csg_module` at the **EDITOR** level (extend it — it currently early-returns for non-SCENE). Ordering is safe: `register_editor_types()` creates the registry (main.cpp:838) **before** `initialize_modules(EDITOR)` (:840); teardown is reverse (`uninitialize_modules(EDITOR)` :900 → `unregister_editor_types()`/registry.free :901). The provider is registered under `#ifdef TOOLS_ENABLED` (the whole block already is).

### 3.2 Provider gating

`CSGEditDomainProvider`:
- `get_domain_id()` → `SNAME("csg_surface")`.
- `is_available(context)` → true when the current EditorSelection's single selected node resolves to a `CSGShape3D` (root or child → nearest CSG root). Query via `EditorNode::get_singleton()->get_editor_selection()` or the existing `Node3DEditor::get_single_selected_node()` reachable through the context's `view`. (Do not store selection — resolve on demand.)
- `can_activate_from_double_click(context, hit)` → true when `ObjectID hit` resolves to a `CSGShape3D` (the double-click hit a CSG result). The hook already passes `_select_ray(pos)` as `hit` (`:2868`).
- `create_session(context)` → `memnew(CSGSurfaceSession)`.

MVP entry paths: **double-click on a CSGBox3D result** (already wired through the 3B hook) and, optionally, the top-toolbar toggle (plan §7). MVP may ship double-click-only; add the toggle if time permits (it mounts through the same `enter_domain("csg_surface", viewport)` call). The tool rail (SLOT_CENTER_LEFT) shows Surface (only active tool this phase; Draw/Paint/Operand disabled placeholders); the contextual panel (SLOT_CENTER_RIGHT) shows the selected-face plane-coordinate readout + numeric entry.

### 3.3 Session state machine (plan §16/§17)

`CSGSurfaceSession` owns (pane-local; never in a plugin singleton — plan §5):
- `CSGShape3D *active_root` (resolved on `enter`/`retarget`; validated by `ObjectID` every use), `CSGBox3D *active_box` (the operand whose face is selected — box only this phase).
- `CSGSurfaceKey selected_surface` + `uint32_t selected_semantic_surface` (the picked box face 0–5); `CSGSurfaceHit hover_hit` (transient).
- Picking accel: `Ref<TriangleMesh> pick_mesh` + `uint64_t pick_mesh_generation` (see §3.4).
- Gesture state enum: **`IDLE → HOVER → PRESSED → DRAGGING → (COMMIT | CANCEL)`**.
  - `IDLE/HOVER`: mouse motion → repick, update `hover_hit`, `queue_redraw`. Returns `BLOCK_NATIVE_EDIT` when hovering a CSG face (suppresses native gizmo hover), else `PASS_TO_VIEWPORT`.
  - `PRESSED`: LMB press on a hovered face → capture `selected_surface`, capture the **starting** box `size`/`transform`, capture the face plane (origin + world normal), capture the drag anchor ray. `CONSUMED`. (Below drag threshold on release → treat as click-select only, no undo entry.)
  - `DRAGGING`: LMB motion past threshold → compute signed displacement `d` along the face normal (screen ray → plane math, §3.5), snap (§3.6), update the **ghost** (§3.7). **No node mutation, no boolean.** `CONSUMED`.
  - `COMMIT`: LMB release (or Enter, or numeric-entry confirm) → one undo action (§3.8) → clear gesture → new async FINAL evaluation lands via scheduler. `CONSUMED`.
  - `CANCEL`: Escape during a gesture → discard ghost, restore starting values (none were mutated, so this is just clearing gesture state), stay in the domain. `handle_escape()` returns true. Escape with no gesture → returns false → host exits the domain (3B contract, `editor_edit_domain.cpp:200-203`).
- `handle_tool_toggle()` (Tab): toggle Surface↔Operand (Operand = pass-through, native editing). MVP may keep Surface-only and return false, but wiring the toggle is cheap and matches §24.

### 3.4 Picking path (screen ray → result triangle → CSGSurfaceKey)

**Phase 4 adds a picking accel over the snapshot's triangle list** (the editor has `TriangleMesh::intersect_ray` with `r_face_index`, but no CSG-result-index raycast helper — this phase builds one). On hover, in the session:
1. If `active_root->get_result_generation() != pick_mesh_generation`, rebuild `pick_mesh`: `Vector<Vector3> faces = active_root->get_brush_faces();` (brush-face order == `result_triangles` order — verified §1.3), `pick_mesh.instantiate(); pick_mesh->create(faces);` and store the generation. (`get_brush_faces()` early-outs on a clean cache, so hover is cheap; it only re-materializes when the model actually changed.)
2. World ray: `Vector3 rp = viewport->get_ray_pos(mouse); Vector3 rd = viewport->get_ray(mouse);`. Transform into root-local: `Transform3D inv = active_root->get_global_transform().affine_inverse(); Vector3 lp = inv.xform(rp); Vector3 ld = inv.basis.xform(rd).normalized();`.
3. `int32_t face_index = -1; Vector3 hp, hn; if (pick_mesh->intersect_ray(lp, ld, hp, hn, nullptr, &face_index)) { ... }`.
4. `CSGSurfaceKey key; uint32_t face_id; if (active_root->resolve_result_triangle((uint32_t)face_index, active_root->get_result_generation(), key, face_id)) { hover_hit = {...}; }`.
5. The picked `key.source_shape` (ObjectID) + `key.semantic_surface` identify the box + which of the 6 faces. Only faces of a `CSGBox3D` are actionable this phase; ignore hits whose source is not a box (leave `active_box = nullptr`, hover highlight only).

**Gesture-start gating (plan §15 picking contract):** capture the `CSGSurfaceHit` (including `result_generation`) at PRESSED. A new topology-dependent gesture must not begin from an outdated published generation — if `hover_hit.result_generation != active_root->get_result_generation()` at press, repick once before capturing. During DRAGGING, **do not repick** — the captured face plane is the authority; the ghost is the visual authority.

### 3.5 Push/pull math (plan §17)

At PRESSED capture: the face's outward normal in box-local space is the axis unit vector for `selected_semantic_surface` (SURFACE_POSITIVE_X → +X, etc., from the `CSGBox3D::Surface` enum). The face plane in box-local coordinates sits at `+size[axis]/2` (positive faces) or `-size[axis]/2` (negative faces). Convert to the drag space (§3.6). During DRAGGING, project the current mouse ray onto the drag line (the face-normal line through the captured face-plane point, in world space) to get the new absolute plane coordinate; snap it; the signed displacement `d` (along the outward normal, box-local) is `new_coord - start_coord`.

Apply (one-sided, opposite face fixed — §17):
- `size[axis] += d`
- box **local center** shifts by `d/2` along the local axis → adjust the node transform origin: `new_origin = start_transform.origin + start_transform.basis.xform(axis_unit * (d/2))` where `axis_unit` points **outward** for the picked face (so a negative-face pull grows toward −axis and shifts center toward −axis). For a negative face the outward normal is −axis, and the same `size += d; center += (outward)*d/2` formula holds with `d` measured along the outward normal.
- **Alt-drag → symmetric** (§17): `size[axis] += 2d`, center unchanged.
- Clamp: `size[axis] = MAX(size[axis], 0.001)` (prevent zero/negative).
- **Rotated / non-uniformly scaled operands:** all normal/displacement transforms go through `start_transform.basis` (the box's local frame), never world axes. Because push/pull edits `size` + `transform.origin` and never `Node3D.scale`, existing rotation and scale are preserved.

### 3.6 Snapping (plan §17)

Snap the **absolute face-plane coordinate**, not the accumulated delta (avoids drift). Source: the existing viewport snap setting — `Node3DEditor::get_singleton()->is_snap_enabled()` (includes the temporary-inversion key `snap_key_enabled`, satisfying §17's "follow Godot's configured convention") and `get_translate_snap()`. MVP snaps in **Operand local** space (`new_coord = Math::snapped(new_coord, snap_step)` on the local plane coordinate). Root/World spaces (§17) are a small extension — wire the enum but MVP may ship Local-only and note Root/World as fast-follow. The contextual panel shows the signed distance + resulting plane coordinate and accepts numeric entry (§17 precision entry): typing a value sets the absolute coordinate; Enter commits; Escape cancels.

### 3.7 Ghost preview (plan §9/§18/§19 — view-local overlay)

The ghost is **view-local and immediate**, drawn per-viewport, never mutating the node or its materials (plan §9 — material mutation leaks between panes). **Use the overlay-draw path** for MVP (matches the 3B `draw_overlay` seam already forwarded at viewport `:4718`):
- `CSGSurfaceSession::draw_overlay(viewport)` draws the projected wireframe of the *target* box (start box with the dragged face moved to the snapped coordinate) using `viewport->get_surface()->draw_*` in screen space (project the 8 box corners through the camera). Show the moving face highlighted, the plane-coordinate label near the face. This is the "ghost immediately" of §9/§18 and is confined to the pane that owns the gesture (each pane's own `surface` draw signal).
- The heavier option (temporary async interactive evaluation with the node actually resized under INTERACTIVE quality while the ghost shows the target) is supported by the scheduler but **not required for MVP** — it mutates the node, complicating undo and multi-pane. Keep it out of Phase 4; the scheduler's interactive quality is exercised by the commit path and the headless tests. Note this as the §14 "interactive result" future path.

Selected-face visual (§9): draw the picked face fill + outline in `draw_overlay` on HOVER/selected; draw the normal handle + plane-coordinate feedback on selection. Scope cue for the active root: MVP may draw a simple root-AABB outline in the overlay; full non-member fade is later (§9 explicitly defers full-scene desaturation).

### 3.8 Undo/redo shape (plan §17 "Undo", §25)

One undo action per committed push/pull (captured starting values once at PRESSED, live updates create **no** history):
```cpp
EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton(); // as in csg_gizmos.cpp:104
ur->create_action(TTR("CSG Push/Pull Face"));
ur->add_do_property(active_box, "size", final_size);
ur->add_undo_property(active_box, "size", start_size);
ur->add_do_property(active_box, "transform", final_transform);      // origin compensation
ur->add_undo_property(active_box, "transform", start_transform);
ur->commit_action();
```
The property setters (`set_size` → `_make_dirty` → `_queue_root_update`) drive a synchronous deferred `update_shape` (or an async FINAL request if the session elects it). Cancellation restores starting values without creating history (nothing was mutated during drag). Undo/redo requests a fresh evaluation and stale async results are discarded normally (§25) — the generation protocol (§2.2) guarantees this.

### 3.9 Retarget / lifecycle (plan §7)

`retarget(context)` (selecting another CSG node under a different root): cancel any active gesture, clear face selection, re-resolve `active_root`. Deleting/removing the root, switching the pane to non-3D, or losing the provider → host exits safely (3B host dtor + `notify_provider_unregistered` already handle this). Validate `active_root`/`active_box` via `ObjectID` before every use (a node may vanish mid-session).

---

## 4. Domain session tri-state input mapping table

Returned by `CSGSurfaceSession::handle_input` (the host pre-consumes Escape/Tab per `editor_edit_domain.cpp:198-208`). "Nav-modified" = orbit/pan/zoom or freelook (session checks the controller state, mirroring the dummy).

| Event | IDLE / HOVER | PRESSED | DRAGGING |
|---|---|---|---|
| MMB / wheel / nav-modified | PASS | PASS | PASS |
| Plain RMB (context menu) | PASS | PASS | PASS |
| Nav (orbit/pan/zoom, freelook) | PASS | PASS | PASS |
| LMB press on a CSG box face | CONSUMED (→PRESSED, capture) | — | — |
| LMB press off any CSG face | PASS (let native select) | — | — |
| Modifier+LMB press (Alt = symmetric; captured at press) | CONSUMED (→PRESSED) | — | — |
| LMB motion (button held), no threshold yet | — | CONSUMED (stay PRESSED) | — |
| LMB motion past threshold | — | CONSUMED (→DRAGGING) | CONSUMED (update ghost) |
| Plain mouse motion (no button) | BLOCK_NATIVE_EDIT if over a face (repick + suppress native hover); else PASS | — | — |
| LMB release | — | CONSUMED (click-select, no undo) → HOVER | CONSUMED (→COMMIT, one undo) → HOVER |
| Escape (via `handle_escape`) | returns false → host exits domain | returns true (cancel) → HOVER | returns true (cancel ghost) → HOVER |
| Tab (via `handle_tool_toggle`) | returns true (Surface↔Operand) | returns true | returns true (after cancel) |
| Any event on a non-focused pane | (never routed — `_domain_pane_accepts_input()` gate) | — | — |

---

## 5. Migration order (each step compiles + is testable; module before editor)

Serialize dev builds (`Get-Process python` first). Filters: `*CSG*` (module + new scheduler cases), `*EditorEditDomain*`, `*EditorDocumentSurface*`, `*ResponsiveLayout*`.

**Step 0 — Baseline.** Build dev+tests; confirm `*CSG*` 18/18 (912), `*EditorEditDomain*` 6/6 (85), and the other two green. Record the 11 counter pins.

**Step 1 — Snapshot flag + scheduler counters (module, additive).** Add `collision_built` to `CSGEvaluationSnapshot` (+ move-assign), set it in `csg_build_snapshot` and in `update_shape()`; gate the `_publish_snapshot` collision branch on `collision_built`. Add the four scheduler counters to `csg_debug_counters.{h,cpp}`. Build + `*CSG*` — **all 18/18 unchanged** (sync path sets `collision_built = want_collision`, so the collision-rebuild pin at `test_csg.h:294-297` holds).

**Step 2 — `csg_evaluation_scheduler.{h,cpp}` (module core, unwired).** Implement the scheduler (§2) with the synchronous-fallback path. Lazily-created `CSGEvaluationScheduler *` member on `CSGShape3D` + destruction in the node dtor (`cancel_and_flush`). Add `request_async_evaluation(CSGEvalQuality)` on `CSGShape3D` (root-only; builds inputs via `_gather_evaluation_inputs`, applies quality settings, calls `scheduler->request(...)`) and `_poll_scheduler()` (deferred landing → `_publish_snapshot`). Build. Add `tests/test_csg.h` scheduler cases in **synchronous-fallback mode** (deterministic): single-flight, coalescing, stale rejection, interactive-skips-collision (`collision_built == false`), counter pins. Run `*CSG*` — 18 existing + new cases green.

**Step 3 — `csg_edit_domain.{h,cpp}` (module editor): provider + skeleton session.** Provider gating (§3.2), session enter/exit/retarget, resolve `active_root`. Register in `register_types.cpp` (EDITOR init) + unregister in `uninitialize_csg_module` (EDITOR). Session `handle_input` returns PASS everywhere for now; `build_tool_rail`/`build_contextual_panel` return placeholder controls. Build. **Manual:** double-click a CSGBox3D → domain enters, rail + panel mount, Escape exits. (Headless registry test for the provider — §6.)

**Step 4 — Picking + hover highlight.** Picking accel (§3.4), `hover_hit`, `draw_overlay` face highlight, tri-state hover mapping. Build. Manual: hovering box faces highlights; hover confined to the active pane.

**Step 5 — Push/pull drag + ghost + snapping + numeric entry.** State machine PRESSED/DRAGGING/COMMIT/CANCEL (§3.3), math (§3.5), snapping (§3.6), ghost overlay (§3.7), contextual panel readout/entry. **No undo yet — scratch copy for the ghost only.** Build. Manual: ghost moves along the normal, snapped; Escape cancels; no node change yet.

**Step 6 — Commit through undo/redo + async final.** COMMIT builds the one undo action (§3.8); commit drives evaluation via `request_async_evaluation(FINAL)` so §30 "no synchronous boolean in the drag handler" holds and the scheduler is exercised live. Build. Manual: push/pull commits, undo/redo restores geometry + transform; rotated/scaled box edits correctly; two panes don't share hover/selection/ghost.

**Step 7 — `/simplify` pass + commit.** Re-verify all four filters + counter pins; one commit (Co-Authored-By trailer). Stage explicitly.

Steps 1–2 are module-only and independently revertable (scheduler unwired until Step 6). Steps 3–5 are editor-only and inert until a session is entered. Step 6 is the behavior-live integration.

---

## 6. Test plan

### Headless (new, deterministic)

- **`modules/csg/tests/test_csg.h` — new scheduler cases** (synchronous-fallback mode so no thread nondeterminism):
  - *Single-flight:* two requests back-to-back → the second coalesces (`scheduler_coalesces == 1`), one completion.
  - *Coalescing / latest-wins:* 100 rapid `request()` calls → final published generation == last requested; `scheduler_requests == 100`.
  - *Stale rejection:* land a snapshot whose `request_generation < requested_generation` (or mismatched `schema_generation`) → discarded, `scheduler_stale_drops` bumped, `result_generation` unchanged.
  - *Interactive skips collision + tangents:* INTERACTIVE snapshot has `collision_built == false`; previously published collision untouched (`collision_rebuilds` does not advance on interactive publish). FINAL publish sets `collision_built == true` and rebuilds.
  - *Root deletion with queued evaluation* (plan §29): free the root while a job is pending — `cancel_and_flush` in dtor discards without publish-after-free.
  - *Undo during in-flight* (plan §29): a synchronous `update_shape` between request and landing → the async landing is discarded by generation check.
- **Existing `*CSG*` 18/18 (912) unmodified** — sync path byte-identical; `collision_built` defaults preserve every pin (esp. `test_csg.h:294-297` collision, `:340-349` clean-update-all-zero, `:530-582` provenance/generation coupling).
- **`tests/editor/test_csg_edit_domain.cpp`** (new, `TEST_FORCE_LINK`, `#ifdef TOOLS_ENABLED`; auto-discovered): local `EditorEditDomainRegistry`, register `CSGEditDomainProvider`, assert gating on `CSGBox3D` vs non-CSG node; `create_session` returns a `CSGSurfaceSession`; independent sessions per view; `notify_provider_unregistered` → exit. The push/pull math (§3.5) should be factored into a free function `csg_push_pull_apply(start_size, start_xform, semantic_surface, d, symmetric) → (size, xform)` so it can be pinned headlessly without a viewport.

### Manual (live drag checklist — editor-runtime)

Double-click entry on a CSGBox3D; hover highlight per face; click-select face (readout in panel); push/pull drag along normal with grid snapping (toggle snap, temporary-invert key); Alt-drag symmetric; numeric entry commits; Escape cancels gesture then (second Escape) exits; commit undo/redo restores size + transform; **rotated + non-uniformly scaled box edits correctly without changing node scale** (§28 acceptance); ghost immediate and confined to the active pane; two panes on one scene do not share hover/selection/ghost; pointer feedback never waits for a boolean (§30); collision does not rebuild during drag, rebuilds once on commit (§30).

---

## 7. Risks

| Risk | Mitigation |
|---|---|
| **Manifold lazy-cache race** — a worker evaluates a handle the main thread also mutates | Single-flight per root (§2.4); worker sees only `inputs.subtree` **copy**; at most one job; `csg_build_snapshot` is pure. Never launch a 2nd job while `running_task` valid. |
| **Copy invariant regression** — worker collapses the cached node handle | `_gather_evaluation_inputs` copies `subtree_manifold` (`:540`); worker touches only the copy. Keep the four subtree-copy comments. Review the job trampoline for any node access. |
| **Publish-after-free** — root deleted mid-job, landing dereferences it | Landing validates `snapshot.root_id == get_instance_id()` before touching the node; node dtor calls `scheduler->cancel_and_flush()` (detach the job; the trampoline frees an orphaned Job, or dtor waits then discards). Never publish onto a freed node. |
| **Stale generation between hover and click** | Gesture-start gating (§3.4): repick if `hover_hit.result_generation != get_result_generation()` at PRESSED; do not repick during DRAGGING (ghost is authority — §15). |
| **Undo/redo mid-async** | Generation protocol (§2.2): undo bumps `result_generation` on the main thread; the in-flight async landing fails the request/schema check and is discarded. Pinned by the "undo during in-flight" test. |
| **Collision skip during drag then final publish** (§30) | `collision_built` flag distinguishes skipped vs empty (§2.5); `_publish_snapshot` gates collision on `collision_built`, never on `collision_faces.is_empty()`; INTERACTIVE leaves prior collision untouched; FINAL on commit rebuilds once. Pinned headlessly. |
| **`update_shape` async regression breaks tests** | `update_shape()` stays fully synchronous; the scheduler is an *additional* path gated to editor + explicit request. Tests never construct a scheduler implicitly. |
| **Picking index mismatch** — using `root_mesh`/`generate_triangle_mesh` (material-regrouped) instead of brush order | Build the pick `TriangleMesh` from `get_brush_faces()` (brush order == `result_triangles` order); `r_face_index` feeds `resolve_result_triangle` directly. Never pick against the render ArrayMesh. |
| **Ghost/material leak across panes** (§9) | Ghost is a per-viewport `draw_overlay` in screen space; no node/material mutation; MVP does not mutate the node during drag. |
| **Rotated/scaled box math wrong** | All normal/displacement transforms go through `start_transform.basis`; edit `size` + `transform.origin`, never `scale`. Pin `csg_push_pull_apply` headlessly. |
| **WorkerThreadPool completion has no main signal → busy-wait** | Deferred self-rescheduling `_poll_scheduler` (call_deferred), not a foreground sleep; `wait_for_task_completion` only after `done.is_set()` (immediate). |
| **Provider registered before registry / after free** | Register in module EDITOR init (registry created earlier in `register_editor_types`, main.cpp:838); unregister in module EDITOR uninit (before registry.free, main.cpp:900-901). |

---

## 8. Explicitly out of scope (Phase 5+)

- **Shift-drag extrusion** (Phase 5) — no union-child creation, no scene-tree transaction.
- **Other primitives** — cylinder/sphere/torus/polygon/mesh push/pull; **box only** this phase.
- **Draw and Paint tools** (Phase 7 / Phase 6).
- **Surface materials, planar UVs, texture lock** (Phase 6) — no `surface_settings/*` properties, no output regrouping.
- **Root/World snap spaces beyond Local** — wire the enum, ship Local-only (Root/World fast-follow).
- **Per-frame interactive node mutation during drag** (§14 richer interactive result) — MVP drives the drag with a ghost overlay only; the node mutates once at commit.
- **Multi-surface selection, connected-fragment highlighting** (§16) — single active face only.
- **Full-scene scope dimming / non-member fade** (§9) — at most a root-AABB outline.
- **Persistent balanced reduction trees, wide-tree perf work** (§12/§32).
- **Module-core editor-header leak** — the scheduler includes no editor headers.

---

### Critical files for implementation
- `modules/csg/csg_evaluation.h` — seam types; add `collision_built`.
- `modules/csg/csg_shape.cpp` (+ `.h`) — `_gather_evaluation_inputs` (`:533`), `_publish_snapshot` (`:667`), `update_shape` (`:634`), `_queue_root_update` (`:321`), `resolve_result_triangle` (`:282`); add scheduler member + `request_async_evaluation`/`_poll_scheduler`.
- `modules/csg/csg_evaluation_scheduler.{h,cpp}` (new) — the per-root single-flight scheduler.
- `modules/csg/editor/csg_edit_domain.{h,cpp}` (new) — `CSGEditDomainProvider` + `CSGSurfaceSession`; registered from `modules/csg/register_types.cpp`.
- `editor/gui/editor_edit_domain.h` — the 3B contract the CSG domain implements (unchanged; the integration target).

Supporting: `modules/csg/csg_debug_counters.{h,cpp}` (scheduler counters), `modules/csg/tests/test_csg.h` + `tests/editor/test_csg_edit_domain.cpp` (new tests), `core/math/triangle_mesh.h` (`intersect_ray` + `r_face_index`), `editor/scene/3d/node_3d_editor_viewport.cpp` (`get_ray`/`get_ray_pos`, hook `:2846`, overlay `:4718`), `editor/scene/3d/node_3d_editor_plugin.h` (snap settings, `is_edit_domain_active_anywhere`).
