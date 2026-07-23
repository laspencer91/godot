# CSG Phase 6 — Surface Materials, Planar UVs, and the Paint Tool

## 0. Scope authority and freeze status

The authoritative scope is `workspace-editor-planning/CSG-EDIT-PLAN.md` **§28 Phase 6** (lines 820-834):

> - Add node-resident indexed settings.
> - Add output material regrouping.
> - Add planar UV generation.
> - Add Paint tool and contextual panel.
> - Add copy/lift/apply, alignment, fit/reset, and texture lock.
> - Regenerate tangents without rerunning booleans.
>
> Acceptance: .tscn round-trip retains all settings; old scenes remain unchanged; material and planar UV edits cause zero boolean evaluations; cut faces receive the cutter surface's material/UV settings; push/pull and extrusion preserve locked texture alignment.

Elaborated by §20 (per-surface material model), §21 (focused UV toolset), §22 (Paint tool), §18 surface-inheritance (lines 514-521), §13 invalidation rules (lines 358-363), §14 finalization split, §26 serialization/compat. The plan is **FROZEN** — implement it; do not redesign it. Where this document proposes a concrete mechanism (property encoding, UV hook point, tool-enum shape), verify the mechanism satisfies the frozen text and flag any conflict rather than silently diverging.

Two MUST-INCLUDE deferred decisions from `CSG-EDIT-PROGRESS.md` are folded in as explicit early steps:
1. **get_material() virtual on CSGPrimitive3D** replacing the 7-way cast chain in `_resolve_manifold_material` — behavior-preserving, landed and green *before* any override logic (Step 1).
2. **EditorNode-edited-root capture** (Phase 5 note): sessions should capture the doc root from the context document, not the `EditorNode` singleton, wherever practical. Paint/extrusion commits adopt this (Step 7).

---

## 1. Current-state map (verified against Phase 5 commit `ec7a15870a`)

**Material resolution path today**
- `CSGShape3D::_resolve_manifold_material(source)` — `modules/csg/csg_shape.cpp:421-442` — a 7-way `Object::cast_to<>` chain. For `CSGMesh3D` it returns `override.is_valid() ? override : source`; for the five other primitives it returns `primitive->get_material()` directly; the fallthrough (Combiner) returns `source`.
- Each primitive already has a non-virtual `get_material() const` (e.g. `CSGBox3D::get_material` at `csg_shape.cpp:1854`). The five non-mesh primitives build their brush with `get_material()` as the face material in `_build_brush`, so for them **`source_material == get_material()`** always. This is the key equivalence enabling the behavior-preserving refactor in Step 1.
- `_gather_manifold_surface_records` (`csg_shape.cpp:542-555`) walks the subtree; for each `CSGManifoldSurfaceRecord` inserts `origin_token → _resolve_manifold_material(record.source_material)` into `inputs.mesh_materials` and `origin_token → record.surface` into `inputs.surface_keys`. **The semantic surface index is available here** — the natural resolution point for overrides.

**Where surface_settings would live on each primitive**
- Every primitive defines a stable `enum Surface { …, SURFACE_COUNT }` and overrides `_get_surface_schema_size()`: Box 6 (`csg_shape.h:362-370`), Cylinder 3 (`:397-402`), Sphere/Torus 1 (`:323-326`, `:441-444`), Polygon fixed 3 (`:532-537`), Mesh dynamic, Combiner 0.
- Property exposure today: each primitive binds `material` via `ADD_PROPERTY` in `_bind_methods`. There is **no** `_get_property_list` in the CSG module; the only dynamic `_set` is `CSGBox3D::_set` (deprecated 3.x compat, `csg_shape.cpp:1826`). `_validate_property` exists on `CSGShape3D` (`:1041`) and `CSGPolygon3D` (`:2729`). Fork convention for indexed serialized data: use `_get_property_list` / `_set` / `_get` / `_property_can_revert` / `_property_get_revert` on `CSGPrimitive3D`, matching the mainline indexed-override pattern (e.g. `MeshInstance3D` surface overrides).

**How material grouping flows (csg_materialize_brush → render surfaces)**
- `csg_pack_manifold` (`csg_evaluation.cpp:175`) groups local brush faces into one MeshGL64 run per non-empty semantic surface, stamps `runOriginalID = origin_base + surface_i`, records `source_material` per surface.
- `csg_materialize_brush` (`csg_evaluation.cpp:47-107`) maps `runOriginalID → material` via `p_mesh_materials`, interns into `brush->materials`, stamps `face.material`. Reads UVs from interpolated vertex properties `MANIFOLD_PROPERTY_UV_X_0/Y_0` (`:89-91`). **Result brush faces do NOT carry `semantic_surface`/`face_id`** — result provenance lives in the parallel `r_result_triangles` vector.
- `csg_build_render_surfaces` (`csg_evaluation.cpp:558`) groups brush faces by `face.material` into `CSGRenderSurface` buckets and generates tangents. It has **no access to origin token / semantic surface** per face today.
- `_publish_snapshot` (`csg_shape.cpp:770-844`) turns render surfaces into `ArrayMesh` surfaces with `surface_set_material`.

**Invalidation seams**
- `_make_material_dirty()` → `_invalidate_materialization_and_ancestors()` (`csg_shape.cpp:369-385`) sets only `materialization_dirty`. Re-materialization runs `GetMeshGL64()` on the *clean* cached subtree copy = **zero `batch_boolean_calls`** (pinned by "Phase 1 material changes retain the root expression", `test_csg.h:410`). Exactly the property §20/§13 require; both new features must reuse it.
- `surface_schema_generation` only bumps in `_synchronize_surface_schema()` (`csg_shape.cpp:404-419`) when `_get_surface_schema_size()` **changes**. Adding/removing a surface override does not change schema size → **no generation bump → no picking/selection invalidation.** A test must pin this.

**Tool rail placeholders & session state (edit domain)**
- `CSGSurfaceSession::build_tool_rail()` (`csg_edit_domain.cpp:877-892`): enabled "Surface" toggle + three **disabled** placeholders: Draw, Paint, Operand.
- Tool state is a single bool `surface_tool_active` (`csg_edit_domain.h:72`). `handle_tool_toggle()` (`:858`) flips it. No tool enum; no Paint state.
- `_pick` (`csg_edit_domain.cpp:194-244`): TriangleMesh from `root->get_brush_faces()`, `resolve_result_triangle`, stores `hover_hit`; `selected_hit`/`has_selection`. Reusable by Paint.
- Contextual panel (`build_contextual_panel`, `:894`): PanelContainer→VBoxContainer, distance label + coordinate LineEdit, refreshed by `_update_context_panel()`. Paint extends this host.
- Commit/undo idiom (`_commit_gesture`, `:387-492`): `create_action` + `add_do/undo_property` + trailing `_request_final_async_evaluation` do/undo pair. Ownership gate uses `EditorNode::get_singleton()->get_edited_scene()` (`:402-404`) — replace with `p_context.document->get_root()` (`editor/editor_document.h:93`) with null-safe singleton fallback.

**Threading contract (§15)** — worker jobs must not touch scene nodes, transforms, or Resource contents. Every new per-surface datum (override material, planar UV frame) MUST be fully resolved into plain values / Refs inside `_gather_evaluation_inputs` on the main thread and carried in `CSGEvaluationInputs`.

---

## 2. Migration order with per-step gates

Filters and baselines: `*CSG*` **31/31 (1013)**, `*CSGEditDomain*` **5/5 (54)**, `*EditorEditDomain*` **6/6 (85)** — the last must stay exactly (do **not** modify `editor/gui/editor_edit_domain.*`).

Build (serialize with other agents): `Get-Process python` first, then `scons platform=windows target=editor dev_build=yes tests=yes d3d12=yes winrt=no -j24`; test via the dev exe `--test --test-case=<filter>`. If a running editor holds the exe lock, rename (don't kill). Never `git add -A`; leave uncommitted. `workspace-editor-planning/` read-only.

### Step 1 — `get_material()` virtual refactor (behavior-preserving)

- Add to `CSGPrimitive3D` (public): `virtual Ref<Material> get_material() const { return Ref<Material>(); }`. Mark the six existing primitive `get_material()` methods `override`. Bindings unchanged.
- Rewrite `_resolve_manifold_material` to a single cast:
  ```cpp
  Ref<Material> CSGShape3D::_resolve_manifold_material(const Ref<Material> &p_source_material) const {
      const CSGPrimitive3D *prim = Object::cast_to<CSGPrimitive3D>(this);
      if (!prim) { return p_source_material; }          // Combiner: no local material
      const Ref<Material> node_material = prim->get_material();
      return node_material.is_valid() ? node_material : p_source_material;
  }
  ```
- **Behavior-preservation argument to confirm:** for the five non-mesh primitives `source_material == get_material()` (verify by reading each `_build_brush`), so the new expression equals both the old direct-return branch and the old mesh-override branch.

**Gate:** `*CSG*` 31/31 (1013) unchanged (material-propagation pin `test_csg.h:199`, mesh-override pin `:442` load-bearing).

### Step 2 — Per-surface settings data model + serialization (inert)

- Typed record in `csg_shape.h`:
  ```cpp
  struct CSGSurfaceSetting {
      Ref<Material> material;
      int uv_mode = 0;                 // 0 = legacy/interpolated, 1 = planar
      int uv_space = 0;                // 0 = operand local, 1 = CSG root, 2 = world
      Vector2 meters_per_tile = Vector2(1, 1);
      Vector2 offset;
      real_t rotation = 0.0;
      bool texture_lock = false;
      // Presence in the map == authored override. Absent slot == inherit node/default.
  };
  ```
  Sparse `HashMap<uint32_t, CSGSurfaceSetting> surface_settings;` on `CSGPrimitive3D`. `has_surface_setting(uint32_t)`, `get_surface_setting(uint32_t)`, granular setters calling `_make_material_dirty()` (material/space/mode) — and route pure UV param changes through `_make_material_dirty` too in this MVP (planar UVs are applied at materialize; still zero booleans; document + pin with a counter test).
- **Serialization via dynamic properties on `CSGPrimitive3D`:** `_get_property_list` emits, per surface `s` in schema range: `surface_settings/<s>/material` (OBJECT), `uv_mode` (INT enum Legacy,Planar), `uv_space` (INT enum Local,Root,World), `meters_per_tile` (VECTOR2), `offset` (VECTOR2), `rotation` (FLOAT), `texture_lock` (BOOL). **Compat rule (§26):** STORAGE usage only when the value differs from default → untouched primitives write nothing (old scenes byte-identical). `_property_can_revert`/`_property_get_revert` expose defaults. `_set`/`_get` parse the path, create the slot lazily on first non-default write, delete the slot when fully default. `_validate_property` hides out-of-range surfaces and planar-only fields when `uv_mode == Legacy`.
- `surface_schema_version` stored scalar (default = current constant; serialize only when non-default) for future migration (§26).
- Duplicate/instancing free via ordinary property machinery; confirm.

**Gate:** new serialization round-trip cases; all existing pins unchanged (settings inert).

### Step 3 — Material resolution order (override > node > inherited)

- `_resolve_manifold_material(source, semantic_surface)`: override (if slot present and material valid) > node material > source. Update the one call site in `_gather_manifold_surface_records` to pass `record.surface.semantic_surface`.
- Grouping happens post-boolean in materialize/render-build → material-override edits re-run only materialization+finalization (zero `batch_boolean_calls`).
- **Cut-face acceptance:** a subtractive cutter's output triangles carry the cutter's `origin_token`, so its per-surface override resolves naturally. Test proves it.

**Gate:** new cases — override-wins, cut-face material, zero-boolean-on-override. Existing pins unchanged.

### Step 4 — Planar UV projection

- **Resolve on the main thread** in `_gather_evaluation_inputs`. Add to `CSGEvaluationInputs` a `HashMap<CSGOriginToken, CSGSurfaceUVResolved> mesh_uv_settings;`:
  ```cpp
  struct CSGSurfaceUVResolved {
      bool planar = false;             // false = keep interpolated brush UVs (legacy)
      Vector3 origin;                  // ROOT-LOCAL space
      Vector3 axis_u, axis_v;         // ROOT-LOCAL, pre-scaled by 1/meters_per_tile and rotated
      Vector2 offset;
  };
  ```
  Populate next to `mesh_materials`, keyed by `origin_token`. Frame construction: schema-deterministic default basis per semantic surface (§21; e.g. box +Z face → U=+X, V=+Y operand-local), transformed to root-local per `uv_space` (Local via operand→root chain; Root directly; World via `root->get_global_transform().affine_inverse()`). Apply meters_per_tile/rotation/offset while building. All node/transform reads on the main thread → worker sees plain vectors only.
- **Apply in `csg_materialize_brush`:** per run (one `origin_token`), if resolved entry is planar, overwrite `face.uvs[i] = Vector2(axis_u.dot(p - origin) + offset.x, axis_v.dot(p - origin) + offset.y)` with `p` the root-local vertex; else keep legacy interpolated UV. Downstream (render surfaces, tangents) unchanged — tangents regenerate automatically without booleans.
- **Hook rationale (comment it):** materialize is the one stage with run→origin_token in hand; UV-only edits route through `_make_material_dirty` (re-materialize = zero booleans).
- Triplanar remains a shader concern; untouched.

**Gate:** planar UVs deterministic and distinct from legacy; UV-only edit keeps `batch_boolean_calls == 0`; the legacy 36-corner-UV box pin (`test_csg.h:235-258`) stays green untouched.

### Step 5 — Texture lock on push/pull and extrusion

- **Root/World-space projection is inherently locked** (frame anchored in root/world coords) — the MVP's primary lock mechanism and what the acceptance test exercises: project a face in Root space with `texture_lock`, push/pull, assert unchanged faces' output UVs identical and the dragged face stays root-anchored.
- **Local space + `texture_lock`:** compensate the stored `offset` for the local-frame shift at commit (one-sided push moves the local center by `d/2` along the drag axis); fold the offset write into the same undo action. Trickiest sub-item: if the math cannot be byte-stable for MVP, gate Local-space texture_lock behind Root/World (which fully satisfy acceptance) and record the deferral — do **not** silently drop the property.

**Gate:** Root-space lock headless case (UV arrays identical across a size edit); a push/pull-lock `*CSGEditDomain*` case if the offset compensation lands.

### Step 6 — Extrusion surface inheritance (§18)

- In the extrusion branch of `_commit_gesture`, after creating `new_box`: outward cap inherits the source face's material AND planar UV alignment (mode/space/meters/offset/rotation/lock) via `surface_settings/<cap>/…` inside the existing undo action (one atomic action). Side faces: source material default; Root-space planar with inherited meters_per_tile per §18. The internal joining face needs no setting. Keep node material as fallback (the existing `set_material` line becomes a fallback, overrides win).

**Gate:** extended extrusion cases — cap settings equal source face's; undo removes child + settings atomically.

### Step 7 — Paint tool: state machine, contextual controls, undo

- **Tool enum** replacing `surface_tool_active`:
  ```cpp
  enum class ToolMode { SURFACE, PAINT, OPERAND };
  ```
  Tab toggles **SURFACE↔OPERAND only** (§24); Paint entered via the rail button. Cancel any active gesture on tool switch.
- **Tool rail:** enable Paint (toggle-mode); `_set_tool_mode(ToolMode)` updates pressed states, refreshes panel, cancels gestures, queues redraw. Draw stays disabled (Phase 7).
- **Paint input** (`handle_input`, PAINT mode): motion → `_pick` hover preview, `BLOCK_NATIVE_EDIT` over a face; LMB press → apply active well settings to the surface (one undo action); Alt+click → eyedropper lift into the well (plus an explicit toggle button); Shift+click → add to multi-surface selection (`Vector<CSGSurfaceKey>`, view-owned, not EditorSelection); MMB/RMB/wheel → PASS.
- **Contextual panel (Paint mode):** material well `EditorResourcePicker` (base `Material`), uv_mode/uv_space options, meters_per_tile/offset/rotation spins, buttons Assign / Eyedropper / Align to Face / Align to Root Grid / Fit / Reset / Apply to Selected (§21/§22). Panel content varies by tool_mode; host stays dumb.
- **Paint apply = one undo action per click** on the SOURCE node (resolve via `ObjectDB::get_instance<CSGPrimitive3D>(key.source_shape)`): `add_do/undo_property` on `surface_settings/<s>/…` + the `_request_final_async_evaluation` do/undo pair. Multi-surface apply = one action. Panel slider drags merge via `UndoRedo::MERGE_ENDS`. **Edited-root gate from `p_context.document->get_root()`** with null-safe EditorNode fallback (deferred-decision adoption).
- **Token-range risk:** overrides don't change schema size → `surface_schema_generation` stable → picking/selection valid across paint + undo. Material edits bump `result_generation` → `_pick` rebuilds `pick_mesh` (already keyed on generation). Selection is semantic (`CSGSurfaceKey`) and survives. Pin with a test.

**Gate:** new `*CSGEditDomain*` cases (below). `*EditorEditDomain*` still exactly 6/6 (85).

### Step 8 — Documentation

- `doc_classes` XML for any newly bound methods under `modules/csg/doc_classes/CSG*.xml`; if settings are purely `_get_property_list`-driven, document the property-name scheme in the affected primitive XML descriptions. Stage only CSG XML changes.

---

## 3. Property schema + serialization decision (summary)

- **Encoding:** dynamic indexed properties (`_get_property_list`/`_set`/`_get` on `CSGPrimitive3D`), sparse HashMap backing store. §20 mandates the exact `surface_settings/<i>/<field>` names.
- **Compat:** default-valued sub-properties carry no STORAGE usage → untouched scenes byte-identical (§26). `surface_schema_version` scalar reserved. ObjectIDs/tokens/generations never serialize.

---

## 4. Test plan

**Headless module tests (`*CSG*`):**
1. Resolution order: override > node > default on box surfaces.
2. Cut-face material: subtractive cutter's per-surface override appears on the cut face.
3. Zero-boolean guarantee: set override material + planar UV param after update_shape → `batch_boolean_calls == 0`.
4. Planar UV: deterministic, distinct from legacy; Root-space invariant to operand edits; legacy-default box still emits the 36 corner UVs (existing pin untouched).
5. Texture lock: Root-space lock — output UVs of unchanged faces identical across a box size edit.
6. Serialization round-trip: authored overrides survive save/load; untouched primitive serializes no `surface_settings/*`.
7. Schema-generation stability: constant across override add/remove and paint-undo cycle.

**Headless editor tests (`*CSGEditDomain*`):**
8. Tool-mode state machine: Tab toggles SURFACE↔OPERAND; Paint not in Tab cycle; gesture cancels on switch.
9. Paint apply: pick + apply changes the source node's setting; one undo restores.
10. Eyedropper/lift + Apply-to-Selected in one action.
11. Extrusion inheritance: cap setting equals source face's; undo atomic.

**Manual checklist:** Paint hover across cut/subtractive operands; material well drag/assign; eyedropper; align/fit/reset; multi-apply; locked texture visibly preserved across push/pull + extrusion on Root/World-projected faces; two panes don't leak hover/selection/panel state.

---

## 5. Risks

- **Schema-generation bump on override add/remove:** mitigated — generation bumps only on schema-size change; overrides don't change size. Pinned by test 7. Do not let override presence feed `_synchronize_surface_schema`.
- **Material Ref thread-safety:** resolve all materials + UV frames on the main thread in `_gather_evaluation_inputs`; worker stores/assigns Refs and reads plain vectors only. Never call `get_material()` or read transforms inside `csg_build_snapshot`/`csg_materialize_brush`.
- **Undo of paint + token regeneration:** material edits bump `result_generation` → repick on generation mismatch (pattern exists at `csg_edit_domain.cpp:269`); selection is semantic and survives; assert.
- **UV hook regressing legacy output:** planar off by default; the 36-corner pin is the tripwire.
- **Local-space texture_lock math:** hardest; restrict to Root/World if not byte-stable, record deferral.
- **UV-edit invalidation routing:** UV param edits must route through the materialization path (else no-op). One route, one counter pin.
- **Shared-tree build races:** Get-Process python; rename-don't-kill; check `$LASTEXITCODE` directly.

---

## 6. Out of scope (Phase 7+)

- Draw tool (§23) — rail button stays disabled.
- Asset drag-and-drop onto faces (§22 deferral).
- Cylinder/polygon semantic push/pull, visible-fragment extrusion, inset/bevel (§27, §32).
- Seam marking, island editing, packing, relaxation, arbitrary unwrap (§21 non-goals).
- Full UV unwrap editor, bake-to-mesh, diagnostics (§27, §32).
- Any change to `editor/gui/editor_edit_domain.*` or new base-session virtuals.
- Reparenting / hierarchy transactions (§19).

---

## 7. Constraints (restate for the implementer)

- Touch only: `modules/csg/**`, `tests/editor/**`, `modules/csg/doc_classes/**` (docs only if needed). Do **not** modify `editor/gui/editor_edit_domain.*`.
- Build: `scons platform=windows target=editor dev_build=yes tests=yes d3d12=yes winrt=no -j24` after `Get-Process python` shows no running scons.
- Filters: `*CSG*` (31/31, 1013 baseline; grows), `*CSGEditDomain*` (5/5, 54 baseline; grows), `*EditorEditDomain*` (exactly 6/6, 85), no regressions.
- Never `git add -A`; leave uncommitted. `workspace-editor-planning/` read-only.

---

### Critical files
- `modules/csg/csg_shape.cpp` (`_resolve_manifold_material` :421, `_gather_manifold_surface_records` :542, `_gather_evaluation_inputs` :557, `_make_material_dirty` :383, `_synchronize_surface_schema` :404, `_validate_property` :1041; add `_get_property_list`/`_set`/`_get` on `CSGPrimitive3D`)
- `modules/csg/csg_shape.h` (`get_material()` virtual, `CSGSurfaceSetting` + `surface_settings` map, accessors)
- `modules/csg/csg_evaluation.cpp` (`csg_materialize_brush` :47 — planar UV application) and `csg_evaluation.h` (`CSGEvaluationInputs` :85 — `mesh_uv_settings`; `CSGSurfaceUVResolved`)
- `modules/csg/editor/csg_edit_domain.cpp` / `.h` (ToolMode enum, Paint input/panel/undo, extrusion inheritance, doc-root capture)
- `modules/csg/tests/test_csg.h` + `tests/editor/test_csg_edit_domain.cpp` (new cases)
