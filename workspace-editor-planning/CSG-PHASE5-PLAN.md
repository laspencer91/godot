# CSG Phase 5 — Shift-Drag Box Extrusion

**Scope (authoritative — plan §28 Phase 5, lines 804–818).** Add a Shift-drag extrusion gesture to the **already-landed** `CSGSurfaceSession` (Phase 4, committed `b5e820fbda`) so that dragging a selected box face **with Shift held** creates a *new* `CSGBox3D` union operand — its footprint is the selected face, its depth is the drag displacement — instead of resizing the source box. The new child is added **beneath the source shape** through **one** `EditorUndoRedoManager` action (add child + set owner + configured size/transform/operation), is selected as the active cap after publication, gets its provenance tokens on the next evaluation automatically, and supports subsequent push/pull. This is an **extension** of the Phase 4 session and its scheduler, not a redesign. The frozen plan (§2 line 37, §16, §18, §19, §25, §28 Phase 5) is the design target.

**Frozen scope anchors (cite in code comments):**
- §2 line 37: *"MVP extrusion adds a union child beneath the source shape and does not reparent existing nodes."*
- §18 lines 489–533: outward extrusion of a box semantic face; `source subtree := source subtree UNION extrusion box`; the source node keeps parent/name/owner/operation/NodePath/scripts; the new child gets union op, deterministic local transform+dims, unique name, correct owner, per-surface settings from the source face; **new cap becomes the active surface after publication**; *"Because the source subtree is evaluated before the parent applies the source node's operation, this also expands a subtractive cutter correctly."*
- §28 Phase 5 acceptance: additive **and** subtractive *operands* extrude correctly; source NodePaths+operation unchanged; undo/redo restores geometry, ownership, selection; no existing node reparented; read-only/inherited nodes reject cleanly.

**Negative / inward extrusion is explicitly OUT OF SCOPE (positive-only MVP).** §18 is titled and specified as *outward* extrusion, and §18 "Unsupported cases" (lines 522–526) states: *"If a case cannot be represented as a union child without changing boolean meaning, it is unavailable in the MVP."* An inward drag cannot be a union child (it would carve, i.e. change boolean meaning), so an inward/zero drag commits nothing (no-op, no history). Note: "additive and subtractive operands extrude correctly" in §28 refers to the *source operand's operation mode* (a subtractive cutter grows its hole because the union child joins the source subtree **before** the parent applies the source's SUBTRACTION) — **not** to inward dragging.

**Hard constraints (enforce at review):**
- Files: `modules/csg/**` and `tests/editor/**` **only**. Do **not** modify `editor/gui/editor_edit_domain.*` (the 3B contract is frozen and sufficient) or any other `editor/**` file. All new work lands in `modules/csg/editor/csg_edit_domain.{h,cpp}`; tests in `modules/csg/tests/test_csg.h` and `tests/editor/test_csg_edit_domain.cpp`.
- The CSG **module core** (`modules/csg/*.cpp`) must not include editor headers (§3). Nothing in module core changes this phase — extrusion is entirely in `modules/csg/editor/`.
- Dev build: check `Get-Process python` (shared tree) then `scons platform=windows target=editor dev_build=yes tests=yes d3d12=yes winrt=no -j24`. `winrt=no` mandatory (STL1011). A running editor holds the exe lock — **rename, don't kill**. Check `$LASTEXITCODE` directly. Long builds → background + poll.
- Stage files explicitly; **never `git add -A`**; leave changes uncommitted. `workspace-editor-planning/` is read-only.
- Green baselines that must stay green: `*CSG*` **29/29, 990 assertions** (baseline; only additive new cases per §5); `*CSGEditDomain*` **4/4, 33 assertions** baseline + new; `*EditorEditDomain*` **6/6, 85**; plus `*EditorDocumentSurface*` / `*ResponsiveLayout*` unaffected. The 11+4 debug-counter pins in `test_csg.h` stay unmodified.

---

## 1. Current-state map (verified against the landed Phase 4 tree, `b5e820fbda`, 2026-07-22)

All line refs are to the committed files.

### 1.1 The session state machine you extend — `modules/csg/editor/csg_edit_domain.{h,cpp}`

- **Header members** (`csg_edit_domain.h:45–125`): `GestureState { IDLE, HOVER, PRESSED, DRAGGING, COMMIT, CANCEL }` (`:46–53`); `SnapSpace { LOCAL, ROOT, WORLD }` (`:54–58`); `active_root_id`/`active_box_id`/`active_viewport_id` ObjectIDs; picking accel `pick_mesh`/`pick_faces`/`pick_mesh_generation`; `hover_hit`/`selected_hit`+`has_*` flags; the drag capture block `press_position`, `symmetric_drag`, `start_size`, `start_transform`, `start_global_transform`, `drag_axis`/`drag_axis_sign`, `start_plane_coordinate`/`target_plane_coordinate`, `drag_line_origin_world`/`drag_line_direction_world`/`drag_axis_world_scale`/`drag_start_parameter`, `drag_displacement`, `ghost_result`/`has_ghost` (`:75–90`); context-panel control ids (`:92–93`).
- **The free push/pull math** `csg_push_pull_apply(start_size, start_transform, semantic_surface, displacement, symmetric) → CSGPushPullResult{size, transform}` (`.cpp:82–102`) and the file-static `_get_box_surface_axis(surface, &axis, &sign, &outward)` (`.cpp:48–80`) are already headlessly pinned. **`csg_extrude_box_face` (this phase) sits beside `csg_push_pull_apply` and reuses `_get_box_surface_axis`.**
- **Gesture forking today:** the mode (push/pull vs symmetric) is decided **at press** in `_begin_gesture` (`.cpp:243–293`): `symmetric_drag = p_event->is_alt_pressed()` (`:266`), captured alongside `start_size`/`start_transform`/`start_global_transform` and the drag-line geometry. **This is exactly where Shift capture for extrude belongs** (§16 line 438: *"Shift-drag starts extrusion after crossing the drag threshold"*; §18 line 496: *"Shift-press selects/captures the face. Crossing the drag threshold enters extrusion."*). No new `GestureState` is required — extrude is a captured-at-press boolean flag that changes the DRAGGING ghost and the COMMIT action, mirroring `symmetric_drag`.
- **Drag update** `_update_drag` (`.cpp:295–327`): threshold `4.0 * EDSCALE` (`:299`); projects the ray onto the world drag line, converts to outward displacement, snaps the **absolute plane coordinate** (`:311–317`, LOCAL only today), then computes `ghost_result = csg_push_pull_apply(...)` and re-derives `drag_displacement`/`target_plane_coordinate` post-clamp (`:319–323`). `drag_displacement > 0` ⇒ outward.
- **Commit** `_commit_gesture` (`.cpp:347–387`): builds **one** undo action `TTR("CSG Push/Pull Face")` with `add_do/undo_property(box,"size"/"transform")` and `add_do/undo_method(root,"_request_final_async_evaluation")` (`:366–375`), then resets capture. **This is the template the extrusion transaction parallels** — swap property edits for the add-child transaction, keep the `_request_final_async_evaluation` do/undo pair verbatim.
- **Cancel / finish-without-commit** `_cancel_gesture` (`.cpp:329–337`), `_finish_without_commit` (`.cpp:339–345`): clear ghost, return to HOVER/IDLE, no history. Escape → `handle_escape` (`.cpp:740–746`) calls `_cancel_gesture`. **No node is created until commit** — extrusion inherits this for free (nothing is mutated during drag).
- **Numeric entry** `_numeric_coordinate_submitted` (`.cpp:410–443`): re-captures if no active gesture, sets `symmetric_drag=false`, recomputes ghost (duplicating `:319–323` at `:433–437`), sets `DRAGGING`, calls `_commit_gesture`.
- **Ghost draw** `_draw_ghost` (`.cpp:497–586`): projects the (possibly resized) box face polygon + normal handle + full 8-corner wireframe + plane label into the viewport `surface` control in screen space. The 8-corner/12-edge box wireframe loop (`:546–574`) is directly reusable to draw the extrusion prism. Helpers `_get_box_face_corners` (`.cpp:445–490`) and `_local_to_world_transform` (`.cpp:492–495`) are reused as-is.
- **Input routing** `handle_input` (`.cpp:667–738`): LMB press → `_begin_gesture` (`:707–709`); LMB motion with LEFT mask + PRESSED/DRAGGING → `_update_drag` (`:722–726`); LMB release → `_finish_without_commit` (PRESSED) or `_commit_gesture` (DRAGGING) (`:711–718`); Enter handled at `:685–694`. Shift+LMB press on a face is already CONSUMED by the LMB-press branch (it does not special-case modifiers). **No routing change is needed** — Shift is read off the captured mouse event inside `_begin_gesture`.
- **`_get_active_box()`** (`.cpp:142–144`) resolves `active_box_id` — an ObjectID pattern reused for the new-box selection.

### 1.2 The module async seam you rely on (unchanged this phase) — `modules/csg/csg_shape.{h,cpp}`

- `request_async_evaluation(CSGEvalQuality)` (`.cpp:698–717`): editor-gated; gathers inputs, lazily creates the per-root `CSGEvaluationScheduler`, `request(...)`, sets `async_suppressed_deferred_count = root_update_deferred_count`, queues the poll. `request_final_async_evaluation()` (`.cpp:719–721`) is the FINAL wrapper, **bound as `_request_final_async_evaluation`** (`.cpp:1107`) — the exact method the commit adds to the undo action.
- `_queue_root_update(bool)` (`.cpp:322–341`): when a scheduler exists it **`invalidate_requests()` + resets `async_suppressed_deferred_count = 0`** (`:332–333`) — the Phase-4 simplify fix. **This is load-bearing for the extrusion transaction:** `add_child` fires the new box's `NOTIFICATION_PARENTED` → `_make_dirty` → `_queue_root_update`, which resets suppression and queues a sync deferred update; the *subsequent* `_request_final_async_evaluation` do-method re-requests async and re-suppresses that queued sync update. Because gather runs synchronously at method-call time, **the async job already includes the freshly-added child** (provenance token range allocated on that evaluation — Phase 2, automatic per node).
- `resolve_result_triangle(triangle, generation, &key, &face_id)` (`.cpp:282–304`) and `get_result_generation()` — picking backbone; unchanged. The new box's faces resolve to `key.source_shape == new_box_id` on the first evaluation after commit → subsequent push/pull works.
- `CSGBox3D::Surface { SURFACE_POSITIVE_X=0 … SURFACE_NEGATIVE_Z, SURFACE_COUNT }` (`csg_shape.h:362–370`); default operation is `OPERATION_UNION`.

### 1.3 The scene-tree transaction idiom (verified editor example to cite)

`Node3DEditor::_add_sun_to_scene` (`editor/scene/3d/node_3d_editor_plugin.cpp:2426–2435`) and `_add_environment_to_scene` (`:2460–2469`) are the canonical add-node-with-owner undo pattern:

```cpp
undo_redo->create_action(TTR("Add Preview Sun to Scene"));
undo_redo->add_do_method(base, "add_child", new_sun, true);   // true = force readable/unique name
undo_redo->add_do_method(new_sun, "set_owner", base);         // owner = edited scene root → serialization
undo_redo->add_undo_method(base, "remove_child", new_sun);
undo_redo->add_do_reference(new_sun);                         // keeps node alive across undo, frees on purge
undo_redo->commit_action();
```

Do-methods execute in registration order (add_child → set_owner); undo-methods execute in registration order (remove_child). `add_do_reference` retains the node while it lives in undo history and frees it when the history entry is purged. **This phase parents under the source box, not the scene root** (§2 line 37 / §18), and configures `size`/`transform`/`operation` on the C++ instance *before* the action (those persist because `add_do_reference` retains the object).

Read-only / inherited validation reference: `SceneTreeDock` uses `Node::is_editable_instance(...)` and `Node::get_scene_inherited_state()` (`editor/docks/scene_tree_dock.cpp:626, 1131, 1405`). The MVP gate (§4 below) mirrors that logic locally without calling the dock singleton (§19 line 542: *"CSG code must not call the global SceneTreeDock singleton"*).

---

## 2. State-machine extension: forking extrude vs push/pull

**Decision point: at PRESSED, in `_begin_gesture`** (§16 line 438 / §18 line 496). Add a captured flag; do **not** add a new `GestureState`.

### 2.1 New header members (`csg_edit_domain.h`)

- `bool extrude_gesture = false;` — captured at press from `p_event->is_shift_pressed()`, parallel to `symmetric_drag`.
- `CSGExtrusionResult extrude_ghost;` (mirrors `ghost_result`) — the computed new-box `{size, local_transform}` for the ghost. A distinct field keeps the push/pull path byte-identical.
- `ObjectID pending_cap_box_id;` + `uint32_t pending_cap_surface = 0;` — set at commit so the new cap becomes the active selected surface after the next publication (§18).
- New struct beside `CSGPushPullResult`:
  ```cpp
  struct CSGExtrusionResult {
      Vector3 size;                 // new box size, in source-box-local meters
      Transform3D local_transform;  // relative to the SOURCE box (identity basis)
  };
  CSGExtrusionResult csg_extrude_box_face(const Vector3 &p_source_size, uint32_t p_semantic_surface, real_t p_depth);
  ```

### 2.2 Fork points

| Stage | push/pull (Shift **not** held) | extrude (Shift held at press) |
|---|---|---|
| `_begin_gesture` | `symmetric_drag = is_alt_pressed()` | additionally `extrude_gesture = is_shift_pressed()`. If both Shift+Alt: **extrude wins, symmetric ignored**. Capture otherwise identical (same drag-line geometry, same `start_plane_coordinate`). |
| `_update_drag` | `ghost_result = csg_push_pull_apply(...)` | if `extrude_gesture`: `extrude_ghost = csg_extrude_box_face(start_size, surface, MAX(drag_displacement, 0))`; the source box is drawn unchanged and a prism ghost is drawn from the face outward. Snapping/numeric machinery shared (snap the absolute cap plane coordinate; depth = coord − start_plane_coord along outward normal). |
| `_commit_gesture` | property edit action | if `extrude_gesture` **and** `drag_displacement > CMP_EPSILON` **and** the read-only/inherited gate passes: build the **add-child transaction** (§3). Inward/zero depth ⇒ `_finish_without_commit()` (no-op, no history). |
| `draw_overlay` / `_draw_ghost` | resized-box wireframe | prism wireframe from the swept face (reuse the 8-corner loop `.cpp:546–574` on `extrude_ghost`), distinct color to read as "new operand." |
| `handle_escape` / cancel | clears ghost | identical — nothing mutated during drag; **no node ever created until commit** (§18, §28). |

`handle_input` needs **no change** — Shift+LMB press already lands in the LMB-press → `_begin_gesture` branch (`.cpp:707`), and `_begin_gesture` now reads Shift off the event. `_update_drag`/release routing is unchanged.

---

## 3. The scene-tree transaction (add union child under the source shape)

### 3.1 Footprint / transform math — `csg_extrude_box_face` (free, headlessly testable)

Given the source box local `size`, the selected `semantic_surface`, and outward `depth > 0`:

```cpp
CSGExtrusionResult csg_extrude_box_face(const Vector3 &source_size, uint32_t surface, real_t depth) {
    int axis; real_t sign; Vector3 outward;
    _get_box_surface_axis(surface, axis, sign, outward);   // outward has only the axis component = sign
    CSGExtrusionResult r;
    r.size = source_size;
    r.size[axis] = MAX(depth, (real_t)0.001);              // depth along the face normal; footprint = other two axes unchanged
    Vector3 center_local;                                   // in SOURCE-box-local space
    center_local[axis] = sign * (source_size[axis] * 0.5 + r.size[axis] * 0.5); // inner cap flush with source face, extends outward
    r.local_transform = Transform3D(Basis(), center_local); // identity basis: the child inherits source rotation/scale via parenting
    return r;
}
```

- The new box is a **child of the source box** (`active_box`), so it inherits the source's world rotation and scale automatically — the footprint matches the face even for rotated / non-uniformly scaled source boxes, with **no scale baking** and **no `Node3D.scale` writes** (§17 discipline carried over).
- The inner (joining) face coincides with the source face → union is seamless (§18 "The joining face is internal after union"). The outward cap is the same `semantic_surface` index as the dragged face (axes align) → that is the cap to select post-publication.

### 3.2 The undo action (parallels `_commit_gesture` and the sun/env pattern)

```cpp
// CSG-5: extrusion commit — one atomic transaction (plan §18 / §28 Phase 5).
CSGExtrusionResult ex = csg_extrude_box_face(start_size, selected_hit.surface.semantic_surface, drag_displacement);
CSGBox3D *new_box = memnew(CSGBox3D);
new_box->set_name("Extrusion");                       // add_child(..., true) makes it unique/readable
new_box->set_operation(CSGShape3D::OPERATION_UNION);  // §18: always union into the source subtree
new_box->set_size(ex.size);
new_box->set_transform(ex.local_transform);
new_box->set_material(source_box->get_material());    // MVP node-level material inheritance; per-surface settings are Phase 6

Node *scene_owner = source_box->get_owner() ? source_box->get_owner() : source_box; // edited-scene root; see §4 gate

EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
ur->create_action(TTR("CSG Extrude Face"));
ur->add_do_method(source_box, "add_child", new_box, true);
ur->add_do_method(new_box, "set_owner", scene_owner);
ur->add_undo_method(source_box, "remove_child", new_box);
ur->add_do_reference(new_box);
// Same supersede pattern as push/pull: add_child queued a sync deferred update; this snapshots the post-add tree and supersedes it.
ur->add_do_method(active_root, "_request_final_async_evaluation");
ur->add_undo_method(active_root, "_request_final_async_evaluation");
ur->commit_action();
```

- `source_box` = `_get_active_box()` (the box whose face was picked). `active_root` = `_get_active_root()`.
- **Source node untouched** (§28 acceptance): its parent, name, owner, operation, NodePath are unchanged — only a child is added. No reparenting (§2 line 37).
- **Selection after publication (§18):** set `pending_cap_box_id = new_box->get_instance_id(); pending_cap_surface = selected_hit.surface.semantic_surface;` then, after commit, immediately set the session selection to the cap by **constructing the key directly** (no pick needed — the cap's `semantic_surface` equals the dragged surface, axes aligned): `active_box_id = new_box_id; selected_hit.surface = { new_box_id, pending_cap_surface, new_box->get_surface_schema_generation() }; has_selection = true;` The `pick_mesh` rebuilds on the next hover when `result_generation` advances; picking then resolves the cap and subsequent push/pull works. Reset gesture capture exactly as the tail of `_commit_gesture` does (`.cpp:377–386`). EditorSelection is **not** modified (root unchanged; face selection is separate from EditorSelection per §16).

### 3.3 Why provenance "works immediately" (next evaluation)

`add_child` → the new `CSGBox3D` enters the tree → `NOTIFICATION_PARENTED` sets `parent_shape` and marks the subtree dirty → root update queued; then `_request_final_async_evaluation` gathers inputs **synchronously** (rebuilding the subtree Manifold including the new child) and launches the FINAL job. During that evaluation the new box's `ManifoldCache` reserves its origin-token range (Phase 2, automatic). On publish, `resolve_result_triangle` maps the cap triangles to `{new_box_id, cap_surface}`. **No token bookkeeping in the session.**

---

## 4. Read-only / inherited-scene gate (§28 acceptance: "reject cleanly")

Before building the transaction, validate the source box is editable in the current edited scene. Reject → `_finish_without_commit()` (no history, no node), optionally set the context-panel label to a short reason. Local check (no SceneTreeDock singleton — §19):

- `Node *edited_root = source_box->get_owner();` (or `source_box` itself if it is the scene root). Reject if there is no edited scene root reachable.
- Reject if `source_box` belongs to a **foreign/instanced subscene** and is not an editable instance — mirror `Node::get_scene_inherited_state()` / `Node::is_editable_instance()` (`scene_tree_dock.cpp:626, 1405`).
- Safe MVP rule satisfying the acceptance test: **allow only when `source_box == edited_root` or `source_box->get_owner() == edited_root`.** Anything else (instanced/inherited internals, orphan nodes) rejects. This guarantees `set_owner(scene_owner)` targets the real edited-scene root and the child serializes into the current scene.

---

## 5. The three folded Phase-4 cleanups (explicit step — same file)

From `CSG-EDIT-PROGRESS.md` follow-up notes:

1. **Dead `gesture_state` intermediate writes.** `_cancel_gesture` (`.cpp:330`) writes `GestureState::CANCEL` then immediately overwrites it at `:334`; `_finish_without_commit` (`.cpp:340`) writes `GestureState::COMMIT` then overwrites at `:342`. Remove the two dead writes (the enumerators remain used by `_commit_gesture`). Re-verify no reader depends on the transient value.
2. **Factor `_apply_displacement()`.** The post-clamp recompute block is duplicated in `_update_drag` (`.cpp:319–323`) and `_numeric_coordinate_submitted` (`.cpp:433–437`). Extract a private `void _apply_displacement()` (operating on the already-set `target_plane_coordinate`/`start_*`/`symmetric_drag`) and call it from both sites. **Extend it to branch on `extrude_gesture`** so the extrusion ghost path reuses the same snap→clamp→derive pipeline (push/pull uses `csg_push_pull_apply`; extrude uses `csg_extrude_box_face`). This is the natural seam for the fork in §2.2.
3. **Milestone tags.** Tag all **new** Phase-5 code `CSG-5:` and retro-tag the Phase-4 session seams you touch with `CSG-4:` for consistency (cosmetic; keep it to lines you edit).

Do these **before** layering extrusion so the fork lands on a clean, de-duplicated base.

---

## 6. Migration order (each step compiles + is testable; module-editor only)

Serialize dev builds (`Get-Process python` first). Filters each build: `*CSG*`, `*CSGEditDomain*`, `*EditorEditDomain*`, plus a regression glance at `*EditorDocumentSurface*`/`*ResponsiveLayout*`.

**Step 0 — Baseline.** Build dev+tests; confirm `*CSG*` 29/29 (990), `*CSGEditDomain*` 4/4 (33), `*EditorEditDomain*` 6/6 (85). Record counter pins.

**Step 1 — Folded cleanups (§5).** Remove dead writes; extract `_apply_displacement()` (push/pull-only branch for now); add tags. Build → all filters unchanged (pure refactor).

**Step 2 — `csg_extrude_box_face` + headless test (math only).** Add the struct + free function beside `csg_push_pull_apply`; **no gesture wiring yet.** Add `tests/editor/test_csg_edit_domain.cpp` cases pinning footprint/transform for +X/−X/±Y/±Z faces and several depths; assert the returned `local_transform` basis is identity and size/center are correct. Build → `*CSGEditDomain*` = 4 baseline + new.

**Step 3 — Gesture fork (ghost only, no commit).** Add `extrude_gesture` capture in `_begin_gesture`; branch `_apply_displacement()`/`_draw_ghost` on it to draw the prism; extend the context panel readout ("Extrude depth"). **No transaction yet** (extrude release → `_finish_without_commit`). Build → manual: Shift-drag shows a growing prism ghost; Escape cancels; no node created.

**Step 4 — Commit transaction + read-only gate + post-commit cap selection.** Implement §3 + §4 in `_commit_gesture` (extrude branch) and the numeric-entry path (branch only when a Shift-initiated gesture is active — a fresh numeric entry with no gesture stays push/pull). Build → manual checklist per §7.

**Step 5 — Headless transaction/provenance test (§7).** Add the tree-level provenance test (force-synchronous evaluation). Build → `*CSG*` and `*CSGEditDomain*` green.

**Step 6 — `/simplify` pass + commit.** Re-verify all filters + counter pins; one commit (Co-Authored-By trailer). Stage explicitly.

Steps 1–2 are inert refactor/addition; Step 3 is ghost-only (no model change); Step 4 is behavior-live.

---

## 7. Test plan

### Headless (deterministic, additive)

- **`tests/editor/test_csg_edit_domain.cpp` — extrusion math** (Step 2): `csg_extrude_box_face` for each of the 6 faces; assert `size[axis] == depth`, footprint on the other two axes == source size, `local_transform.origin == outward*(source_size[axis]/2 + depth/2)`, `local_transform.basis == Basis()`; depth clamp at 0.001.
- **`modules/csg/tests/test_csg.h` — new-child provenance** (Step 5, force-synchronous): build a root `CSGBox3D`; `set_async_evaluation_force_synchronous(true)`; add a child `CSGBox3D` (union) via `add_child`; `root->update_shape()`; assert (a) `result_generation` advanced, (b) at least one result triangle resolves via `resolve_result_triangle` to `key.source_shape == child_id` with a valid cap `semantic_surface` — the headless proxy for "provenance works immediately" and "subsequent push/pull works."
- **Existing `*CSG*` 29/29 (990) and `*CSGEditDomain*` 4/4 (33) unmodified** — extrusion is additive; no module-core edit; counter pins untouched.
- **Transaction undo** is **not** unit-tested headlessly (`EditorUndoRedoManager` singleton + live scene tree unavailable in the harness, matching Phase 4's choice). The transaction is validated by the manual checklist; the *node configuration* it applies is fully pinned by `csg_extrude_box_face` + the provenance tree test.

### Manual (live editor checklist)

Double-click a `CSGBox3D` to enter the domain; Shift-drag a face outward → prism ghost appears immediately and is confined to the active pane; snapping toggles the cap plane coordinate; numeric entry during the Shift-drag sets depth and commits; release commits **one** undo action ("CSG Extrude Face"); the new box appears under the source box in the Scene Tree with the correct owner (persists on save/reload); the cap is the active surface and push/pull on it works; **undo removes the child cleanly and restores selection**; redo re-adds; a **subtractive** source operand's extrusion grows the cut (source operation unchanged); a **rotated + non-uniformly scaled** source box extrudes with a footprint that matches the face and no change to `Node3D.scale`; **Escape mid-drag creates no node**; extruding a **read-only/inherited** source rejects cleanly with no node and no history; inward/zero drag is a no-op; two panes on one scene do not share ghost/selection.

---

## 8. Risks

| Risk | Mitigation |
|---|---|
| **Transaction atomicity** — child add, owner, geometry not one undo step | Single `create_action`/`commit_action`; `add_child`+`set_owner`+`_request_final_async_evaluation` as do-methods, `remove_child`+`_request_final_async_evaluation` as undo-methods, `add_do_reference(new_box)` (exact sun/env pattern, `node_3d_editor_plugin.cpp:2426`). Size/transform/operation set on the instance before the action, retained by `add_do_reference`. |
| **Owner edge cases** — orphan / instanced / inherited source | §4 gate rejects anything where `source_box != edited_root && source_box->get_owner() != edited_root`; owner = `source_box->get_owner()` (or `source_box` if it is the root). Rejects cleanly (no node, no history). |
| **Provenance for the new node** — cap not selectable until tokens exist | Tokens auto-allocate on the next evaluation (Phase 2); commit issues `_request_final_async_evaluation` which gathers synchronously *after* `add_child`, so the job includes the child. Cap selection is set by constructing the key directly; `pick_mesh` rebuilds when `result_generation` advances. Pinned by the provenance tree test. |
| **Dedup interaction** — the queued sync update from `add_child` vs the async FINAL | `_queue_root_update` resets suppression + `invalidate_requests()` at mutation (`csg_shape.cpp:332–333`); the *later* `_request_final_async_evaluation` re-suppresses and re-requests — identical to the push/pull commit flow. **Keep `_request_final_async_evaluation` as the last do-method** (after `add_child`/`set_owner`) so it snapshots the post-add tree. |
| **Subtractive source grows the wrong thing** | New child is **always** `OPERATION_UNION` and parented **under the source box**; the source subtree unions the extrusion *before* the parent applies the source's operation (§18) → additive grows the shape, subtractive grows the cut. Manual checklist covers both. |
| **Inward/zero drag** (out of scope) | Extrude commit requires `drag_displacement > CMP_EPSILON`; otherwise `_finish_without_commit()`. |
| **Ghost / material leak across panes** (§9) | Ghost is per-viewport `draw_overlay` in screen space; no node/material mutation during drag; source box untouched until commit. |
| **Shift + Alt ambiguity** | Extrude wins; symmetric ignored. Documented in `_begin_gesture`. |
| **Rotated/scaled footprint wrong** | New box is a child with identity local basis → inherits source rotation/scale via parenting; `csg_extrude_box_face` never touches basis or scale. Pinned headlessly + manual. |
| **Numeric-entry commits the wrong gesture** | Numeric path commits extrusion only when a Shift-initiated gesture is already active (`extrude_gesture` set at press); a fresh numeric entry with no active gesture stays push/pull. |
| **Selection lost on undo** (§28 acceptance) | Face selection is session-local and reset on the commit/undo path; the active root is unchanged by the transaction, so undo restores the pre-extrude selection state naturally. EditorSelection is not part of the action. |

---

## 9. Explicitly out of scope (Phase 6+)

- **Inward / negative extrusion** (would carve — not a union child; §18). Positive-only MVP.
- **Reparenting existing nodes / combiner interposition** (§2 line 37, §18, §19).
- **Per-surface material / planar-UV inheritance on the cap and sides** (§18) — **Phase 6.** MVP inherits only the node-level material from the source box.
- **Non-box source faces** — box only.
- **Rectangle-on-face / inset / bevel / visible-fragment extrusion** (§18, §32).
- **Root/World snap spaces** — LOCAL only (matches Phase 4's deferral).
- **Multi-surface / Shift-click selection toggle** (§16) — Shift means extrude here.
- **Per-frame interactive node mutation during the extrude drag** — ghost only; node created once at commit.
- **EditorSelection sync / Scene-Tree focus of the new node as part of undo** — session-local cap selection only.
- Any change to `editor/gui/editor_edit_domain.*` or module core.

---

### Critical files for implementation
- `modules/csg/editor/csg_edit_domain.cpp` — extend `_begin_gesture` (`:243`), `_update_drag` (`:295`), `_commit_gesture` (`:347`), `_numeric_coordinate_submitted` (`:410`), `_draw_ghost` (`:497`); add `csg_extrude_box_face`, the read-only gate, `_apply_displacement()`; fold the three cleanups.
- `modules/csg/editor/csg_edit_domain.h` — add `CSGExtrusionResult` + `csg_extrude_box_face` decl, `extrude_gesture`/`extrude_ghost`/`pending_cap_*` members, `_apply_displacement()`.
- `modules/csg/csg_shape.cpp` — **no edits**; relied-upon seams `request_final_async_evaluation` (`:719`, bound `:1107`) and `_queue_root_update` dedup reset (`:322–341`).
- `tests/editor/test_csg_edit_domain.cpp` — extrusion-math cases (baseline 4/33 + new).
- `modules/csg/tests/test_csg.h` — new-child provenance case (force-synchronous).

Supporting refs: `editor/scene/3d/node_3d_editor_plugin.cpp:2426–2469` (add-child/set-owner/add_do_reference undo pattern), `editor/docks/scene_tree_dock.cpp:626,1405` (read-only/inherited validation to mirror), `modules/csg/register_types.cpp:63–88` (provider registration — unchanged), `modules/csg/csg_shape.h:362` (`CSGBox3D::Surface` enum).
