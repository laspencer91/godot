# CSG Edit — Phase 7 (Draw Tool) — Implementation Plan

**Scope authority:** `workspace-editor-planning/CSG-EDIT-PLAN.md` §28 Phase 7 (feature list), §23 (Draw workflow), plus §7 (lifecycle), §16 (face selection), §24 (Operand/Tab), §25 (undo). This plan is subordinate to that frozen text; where it interprets or fills silence it says so explicitly.
**Base commit:** Phase 6 landed (`71a7759b35`). Baselines: `*CSG*` 45/45 (1326), `*CSGEditDomain*` 10/10 (157), `*EditorEditDomain*` 6/6 (85).

---

## 1. Authoritative scope (verified against frozen text)

§28 Phase 7 lists exactly five deliverables:
- Add workplane/face rectangle picking.
- Add Box drawing.
- Add explicit Add/Cut and temporary Ctrl inversion.
- Add height drag and numeric entry.
- Add node creation/undo.

§23 pins the workflow: 1. Choose Box. 2. Choose Add or Cut explicitly. 3. Drag a snapped rectangle on the current workplane or a visible planar face. 4. Drag or type height. 5. Commit one `CSGBox3D`. Modifiers: Add is default; **holding Ctrl temporarily inverts** Add/Cut (explicit state stays visible); **Escape cancels the current phase**; **RMB remains native**. §23 "Later additions" defers: rectangle-on-face extrusion, cylinder drawing, polygon/path drawing, reusable workplanes, duplicate-and-drag — all **out of scope**.

Interpretations (recorded):
- **Drawing on an existing CSG face creates a child under that face's operand.** The frozen text says "union child" — that is the Add case. The Add/Cut choice selects the child's operation (Add→`OPERATION_UNION`, Cut→`OPERATION_SUBTRACTION`) placed as a child of the hit operand; both are structurally sound because a child subtree is evaluated before the parent applies the source node's own operation (§18's reasoning). Orchestrator interpretation of the "union child" wording.
- **Draw cannot activate without an existing selection.** §7 governs activation; the provider requires a selected CSGShape3D. Phase 7 introduces no new activation path — Draw always runs inside an already-active session with a valid root. Drawing on the ground plane adds a *new sibling root* while a session is active on some root. The first CSG object in an empty scene still comes from the native "Create CSG Box" context menu (`node_3d_editor_viewport.cpp:2564-2611`). Intentional boundary.

---

## 2. Current-state map (verified refs, `71a7759b35`)

All refs `modules/csg/editor/csg_edit_domain.{h,cpp}` unless noted.

- **ToolMode** `{ SURFACE, PAINT, OPERAND }` at `.h:59-63`. Rail in `build_tool_rail()` (`.cpp:1489-1519`): Surface toggle, **Draw (created disabled, no signal, `:1499-1502`)**, Paint toggle, Operand toggle. `_update_tool_buttons()` (`:363-373`); `_set_tool_mode()` (`:350-361`) cancels in-flight push/pull gesture, refreshes. The rail test (`test_csg_edit_domain.cpp:100-132`) asserts 4 children and `draw_button->is_disabled()` — **must be updated when Draw is enabled**.
- **Input routing** `handle_input()` (`:1347-1456`): retarget on selection change (`:1352-1356`); early-out PASS if no active root (`:1357-1359`) or OPERAND (`:1361-1363`); Enter commit for Surface (`:1366-1375`); MIDDLE/RIGHT/WHEEL pass (`:1385-1387`); LMB/motion branch by mode; Paint block (`:1389-1440`). `handle_escape()` (`:1458-1469`); `handle_tool_toggle()` (`:1471-1474`) = Surface↔Operand only; `draw_overlay()` (`:1476-1487`) dispatches per mode.
- **Gesture template**: `_begin_gesture()` (`:681-736`) captures the world drag line; `_closest_parameter_on_line_to_ray()` (`:663-679`); `_update_drag()` (`:738-767`) snaps the absolute coordinate.
- **Extrusion transaction** (`_commit_gesture` extrusion branch `:811-875`): the one-undo-action shape (`:845-854`) that under-face creation reuses; editability gate `_is_source_editable` (`:328-330`); post-commit retarget of `active_box_id`/`selected_hit`.
- **`_pick()`** (`:611-661`): TriangleMesh from `get_brush_faces()` (brush order == result order; never the render mesh). Currently **discards `hit_position`/`hit_normal`** — the Draw pick variant must retain them.
- **Native "Create CSG Box"** (`node_3d_editor_viewport.cpp:2564-2611`): the new-root transaction template — `instantiate_object_properties`, `validate_child_name` + casing, `snap_point`, `create_action_for_history`, add_child/set_owner/set_global_position/add_do_reference/remove_child, select-if-document-root (`:2606-2610`). Does **not** set `use_collision`.
- **`use_collision`**: default `false` (`csg_shape.h:129`); `set_use_collision` (`:211`); root-only property — children inherit the root's collision automatically.
- **Box schema**: `CSGBox3D::Surface` (`csg_shape.h:419-426`); `_get_box_surface_axis` (`.cpp:52-84`); `_get_box_face_corners` (`:1070-1115`); ghost polyline machinery in `_draw_ghost` (`:1122-1213`).
- Snapping: `Node3DEditor::snap_point` (`node_3d_editor_plugin.h:468`), `is_snap_enabled()` (`:481`), `get_translate_snap()` (`:484`). Rays: `Node3DEditorViewport::get_ray_pos`/`get_ray` (`node_3d_editor_viewport.h:661-662`). No ray-plane helper in the session yet — add one via `Plane::intersects_ray`.

---

## 3. Two-phase gesture state machine (minimal extension)

Draw is a **distinct tool mode** (rail button, like Paint) — do **not** overload `GestureState`.

- Add `ToolMode::DRAW` → `{ SURFACE, DRAW, PAINT, OPERAND }`.
- `enum class DrawPhase { IDLE, RECTANGLE, HEIGHT };` + `DrawPhase draw_phase = DrawPhase::IDLE;`
- Draw state (session-local, cleared on every exit/retarget/cancel/tool-switch path): `bool draw_cut_mode = false;` plane frame `draw_plane_origin_world`/`draw_plane_normal_world`/`draw_plane_u_world`/`draw_plane_v_world`; `ObjectID draw_parent_operand_id;` (null = ground plane → new standalone root); `Vector2 draw_rect_min/draw_rect_max/draw_first_corner_uv;` `real_t draw_height = 0.0;` height drag line; contextual control ids (draw tool button, Add/Cut buttons, height edit).

**Transitions** (inside `tool_mode == DRAW`):
```
IDLE --LMB press on resolved plane--> RECTANGLE   (capture first corner; parent operand resolved here)
RECTANGLE --LMB drag--> RECTANGLE                 (update corner + rect ghost; snap)
RECTANGLE --LMB release, non-degenerate--> HEIGHT (build height drag-line at rect center)
RECTANGLE --LMB release, degenerate--> IDLE       (no history)
RECTANGLE --Escape--> IDLE                        (cancel whole draw)
HEIGHT --motion--> HEIGHT                          (update height + box ghost; snap; Ctrl live-inverts color)
HEIGHT --LMB click OR numeric Enter--> IDLE        (COMMIT; zero height => no history)
HEIGHT --Escape--> RECTANGLE                       (§23 "cancels the current phase")
```
Extend `_set_tool_mode()` to reset draw state; clear draw state in `exit()`/`retarget()`.

---

## 4. Reference-plane resolution

New `bool _resolve_draw_plane(Node3DEditorViewport *p_viewport, const Vector2 &p_position)` at RECTANGLE start:

1. **Face hit first.** Draw-variant pick retaining `hit_position`/`hit_normal`. On a resolved hit: `draw_parent_operand_id` = the hit surface's `source_shape`; plane = hit triangle's plane in world space (origin = `root_global.xform(hit_position)`; for a box face prefer the exact face normal via `_get_box_surface_axis` × box global basis; else triangle normal). Orthonormal (u,v): seed = world axis least aligned with normal, `u = seed.cross(normal).normalized()`, `v = normal.cross(u)`.
2. **No hit → world ground plane** `Plane(Vector3(0,1,0), 0)`, u = +X, v = +Z (matches native create placement space and `snap_point` coherence). `draw_parent_operand_id` null → new standalone root.
3. Project the press ray with `Plane::intersects_ray`; reject parallel/miss (PASS, no phase change).

`draw_first_corner_uv` = projected point in (u,v) scalars, snapped.

---

## 5. Rectangle math and box construction (pure, headless-pinnable)

Free functions beside `csg_push_pull_apply`/`csg_extrude_box_face`:

```cpp
struct CSGDrawRect { Vector2 min; Vector2 max; bool degenerate; };
CSGDrawRect csg_draw_rectangle_bounds(const Vector2 &a, const Vector2 &b, real_t min_extent);

struct CSGDrawBoxResult { Vector3 size; Transform3D world_transform; };
CSGDrawBoxResult csg_draw_box_from_rect(
    const CSGDrawRect &rect, real_t height,
    const Vector3 &plane_origin, const Vector3 &plane_u,
    const Vector3 &plane_normal, const Vector3 &plane_v);
```

- Bounds: componentwise min/max (corner-order independent); `degenerate` when either extent < `min_extent` (= `MAX(translate_snap, 0.001)` when snapping, else 0.001).
- Box: `size = (width_u, MAX(height,0.001), depth_v)`; rect center world = `origin + u*mid_u + v*mid_v`; box center = rect center + `normal*(height/2)`; basis columns `(u, normal, v)`; height always along `+normal` (box sits on the picked face / on the grid).
- Operation is resolved at commit, not in the math: `effective_cut = draw_cut_mode ^ ctrl_held`.

---

## 6. Snapping (both phases)

- Rectangle corners: snap the **plane (u,v) scalars** with `Math::snapped(coord, translate_snap)` under `is_snap_enabled()` (drift-free on tilted planes; equals world X/Z snap on the ground plane). Snap first corner at press and the moving corner each motion.
- Height: snap the absolute height scalar (same as push/pull `:754-760`).
- Follow the existing snap-enable convention (includes the temporary inversion key). No hard-coded snap modifier.

---

## 7. Tri-state input mapping for Draw mode

New `tool_mode == DRAW` block in `handle_input()` parallel to the Paint block. Navigation/region passthrough and MIDDLE/RIGHT/WHEEL passthrough stay ahead of it (RMB/MMB/wheel native, §23). At the top of the branch: if `_get_active_root()` is null → reset `draw_phase`, clear state, PASS.

| Event | Phase | Result |
|---|---|---|
| LMB press | IDLE | `_resolve_draw_plane` → success: first corner, →RECTANGLE, CONSUMED; else PASS |
| Motion (LMB held) | RECTANGLE | project/snap/update rect, CONSUMED |
| LMB release | RECTANGLE | degenerate → IDLE (no history), CONSUMED; else build height line, →HEIGHT, CONSUMED |
| Motion (no button) | HEIGHT | height from `_closest_parameter_on_line_to_ray`, snap, ghost + Ctrl recolor, BLOCK_NATIVE_EDIT |
| LMB press | HEIGHT | `_commit_draw()`, CONSUMED |
| Enter | HEIGHT | `_commit_draw()` (mirror Surface Enter `:1366-1375`) |
| Numeric height submit | HEIGHT | set height → `_commit_draw()` |
| MMB/RMB/wheel/nav | any | PASS |

`handle_escape()`: before the existing gesture check — DRAW+HEIGHT → back to RECTANGLE, return true; DRAW+RECTANGLE → IDLE, return true (Escape during a draw never exits the domain). `draw_overlay()`: DRAW case draws the rect outline (RECTANGLE) and box wireframe + top-cap fill + height label (HEIGHT), green Add / red Cut reflecting live Ctrl inversion; reuse `_draw_ghost` polyline machinery.

---

## 8. The two creation transactions

Factor into a testable helper:

```cpp
CSGBox3D *csg_draw_commit_box(
    EditorUndoRedoManager *undo_redo,
    CSGShape3D *root, Node *edited_root,
    CSGPrimitive3D *parent_operand /* null => new standalone root under edited_root */,
    const CSGDrawBoxResult &box, bool cut, bool use_collision_for_new_root);
```

**A. Under-face child** (`parent_operand != null`): editability gate (reject cleanly, no history); local transform = `parent_global.affine_inverse() * box.world_transform`; one undo action mirroring extrusion (`create_action("CSG Draw Box")`, add_child(new_box,true), set_owner(edited_root), undo remove_child, add_do_reference, `_request_final_async_evaluation` do+undo on `root`); `set_operation(cut ? SUBTRACTION : UNION)`, size/transform, unique readable name. No use_collision (inherited from root).

**B. New standalone root** (`parent_operand == null`): parent = `edited_root`; `instantiate_object_properties` + `validate_child_name` + casing (match native create); one undo action: add_child(new_box,true) → set_owner → `set_global_transform(box.world_transform)` → **`set_use_collision(use_collision_for_new_root)`** → add_do_reference → undo remove_child; evaluation driven by `add_do_method(new_box,"_request_final_async_evaluation")` (a standalone root evaluates itself); `set_operation(UNION)`. Post-commit: select the new box in EditorSelection (native create's select-if-document-root rule), set `active_root_id` immediately, `_clear_pick_state()`.

Both paths: reset `draw_phase`, clear transient state, redraw. The new box participates in provenance/settings automatically (plain `CSGBox3D`).

### `use_collision` decision (RESOLVED)

The frozen plan is silent (§23/§26). **Decision: new standalone roots created by Draw default `use_collision = true`** — Draw is an explicit level-blocking tool; solid geometry is the expected default. Recorded as an **orchestrator decision, user-overridable** (Inspector toggle; project-setting gate out of scope). Under-face children unaffected (root property). **Divergence note:** native context-menu "Create CSG Box" leaves collision off; the Draw-only default (true) is intentional — recorded in CSG-EDIT-PROGRESS.md (do NOT edit DIVERGENCE-LEDGER.md; it carries another session's uncommitted work). Pass as the `use_collision_for_new_root` parameter (default true), not hard-coded.

---

## 9. Migration order with per-step gates

Build per step (`Get-Process python` first; rename a locked exe aside, never kill). `modules/csg/**` + `tests/editor/**` only; never `git add -A`; leave uncommitted.

- **Step 1 — Pure math + data model.** `ToolMode::DRAW`, `DrawPhase`, state members, `csg_draw_rectangle_bounds`/`csg_draw_box_from_rect` + structs. Gate: compiles; new headless pins pass; baselines unchanged.
- **Step 2 — Rail + contextual panel.** Enable Draw button (toggle → `_set_tool_mode(DRAW)`); Add/Cut segmented toggle; height LineEdit; hint label; panel visibility. **Update the rail test** (`test_csg_edit_domain.cpp:100-132`): Draw enabled/selectable; Tab still excludes Draw. Gate: `*CSGEditDomain*` green at new count.
- **Step 3 — Plane resolution + rectangle phase.** Draw-variant pick retaining hit point/normal; `_resolve_draw_plane`; RECTANGLE routing + rect ghost. Gate: builds; pins green.
- **Step 4 — Height phase.** Height drag line, motion→height, numeric entry, box ghost with Add/Cut color + Ctrl inversion. Gate: builds.
- **Step 5 — Commit transactions.** `csg_draw_commit_box` (both paths) + retarget/selection + collision decision. Gate: new commit-config pins green; `*CSG*` baseline unchanged.
- **Step 6 — Escape/cancel + lifecycle.** Per-phase Escape; state clearing in `_set_tool_mode`/`exit`/`retarget`; null-root guard. Gate: all filters green.
- **Step 7 — Final verify.** Five filters at exact counts.

---

## 10. Test plan

**Headless (`test_csg_edit_domain.cpp`):**
- Rectangle bounds: corner-order independence; degeneracy across `min_extent`; zero-area rejects.
- Box-from-rect: ground plane (identity basis, origin = rect center + (0,h/2,0)); tilted plane (orthonormal columns (u,n,v)); height clamp.
- Commit-config (mirror the extrusion transaction test `:380-451`, `CSGEditDomainSynchronousSchedulerScope`):
  - Under-face: one undo action; Add→UNION / Cut→SUBTRACTION child with correct size/local transform; undo removes, redo restores; editability gate rejects non-owned source with no history.
  - New root: child of edited_root, owner set, UNION, **use_collision == true**, world transform applied; one action; undo/redo; a second subcase pins `use_collision_for_new_root=false` (flag honored).
- Mode/rail: Draw enabled/selectable → `ToolMode::DRAW`; Add/Cut toggle flips `draw_cut_mode`; Tab excludes Draw.

**Manual checklist:** enter on existing root → Draw; ground-plane rect with visible snapping; pull/type height → new standalone root (selected, collision on, session retargets); rect on an existing face → child under that operand (source NodePath/operation unchanged); Cut carves; Ctrl live-inverts during height; Escape: height→rectangle, rectangle→abandoned, idle→exit; single undo removes the whole box; new box accepts Paint/UV settings; RMB/MMB/wheel native throughout.

---

## 11. Risks and mitigations

- **No-active-root entry:** no new activation path (§7); DRAW branch early-outs and resets when the root is gone. First box in an empty scene = native context menu.
- **Retarget-after-create:** `active_root_id` set immediately + EditorSelection select + `_clear_pick_state()`; next `handle_input` retarget reconciles.
- **Ground-plane picking without a pick mesh:** `Plane::intersects_ray` on the raw ray, independent of `pick_mesh`; reject parallel/behind.
- **Degenerate rects:** `min_extent` tied to snap step (or 0.001); degenerate release = IDLE, no history; height clamps; zero height commits nothing.
- **Undo of a root creation while the session targets it:** ObjectID-based root resolution → null after undo → DRAW resets and passes; re-resolve from EditorSelection. No dangling pointers.
- **Face-plane basis stability:** box face normal preferred over triangle normal; least-aligned-axis seed for the cross products.
- **Async collision lag:** ghost is view-local; collision builds at final publish (§14/§30) — expected.

---

## 12. Out of scope

Rectangle-on-face extrusion variant; cylinder drawing; polygon/path drawing; reusable/named workplanes; duplicate-and-drag; non-box primitives; asset drag-drop onto the drawn face; multi-box repeat draw; project-setting gate for the collision default; changing the native "Create CSG Box" collision default; any change to `editor/gui/editor_edit_domain.*`.

---

## 13. Constraints (restate)

- `modules/csg/**` + `tests/editor/**` only; do **not** modify `editor/gui/editor_edit_domain.*` or DIVERGENCE-LEDGER.md.
- Build: `scons platform=windows target=editor dev_build=yes tests=yes d3d12=yes winrt=no -j24` after `Get-Process python`; rename a locked exe aside, never kill.
- Filters exact: `*CSG*` (45/45, 1326 baseline + new pins), `*CSGEditDomain*` (10/10, 157 + new), `*EditorEditDomain*` **exactly 6/6 (85)**, `*EditorDocumentSurface*` 2/2 (33), `*ResponsiveLayout*` 2/2 (26).
- Never `git add -A`; leave uncommitted. `workspace-editor-planning/` read-only. Tag new seams `CSG-7:`.

---

### Critical files
- `modules/csg/editor/csg_edit_domain.cpp` / `.h`
- `tests/editor/test_csg_edit_domain.cpp`
- `modules/csg/csg_shape.h` (Box surface enum :419-426, `set_use_collision` :211, default :129)
- `editor/scene/3d/node_3d_editor_viewport.cpp` (native create template :2564-2611; rays via node_3d_editor_viewport.h:661-662) — read-only reference
