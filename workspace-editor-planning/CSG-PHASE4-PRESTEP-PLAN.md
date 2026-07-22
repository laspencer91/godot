# CSG Phase 4 Pre-Step — Evaluation Extraction & Detachable Snapshot

**Scope:** Restructure `modules/csg/csg_shape.cpp` so the boolean-evaluation machinery lives in dedicated translation units and the completed evaluation becomes a self-contained value object (`CSGEvaluationSnapshot`) built from immutable inputs and published atomically onto the root. **No threads, no scheduler, no interactive/final split, no editor code.** This creates the seams Phase 4 lands on. **Zero behavior change:** the 18 cases / 912 assertions in `modules/csg/tests/test_csg.h` must pass unmodified.

The guiding principle that keeps this low-risk: **move only pure (node-free, server-free) code into new files; keep every node method, every dirty flag, and every DEV counter call site in `csg_shape.cpp`.** The snapshot/input structs are simply the data that crosses the new pure boundary.

---

## 1. Current architecture (what exists after Phases 1–2)

All in `modules/csg/csg_shape.cpp` unless noted.

**Cache & record types (file-local, will move):**
- `struct CSGManifoldSurfaceRecord` (`:57-61`) — `origin_token`, `surface` (`CSGSurfaceKey`), `source_material`.
- `struct CSGManifoldResultTriangle` (`:63-66`) — `origin_token`, `face_id`.
- `struct CSGShape3D::ManifoldCache` (`:68-99`) — the pimpl: `local_brush`; `local_manifold`/`transformed_manifold`/`subtree_manifold` handles; `origin_base`/`origin_count`/`origin_schema_generation`; `surface_records`; `result_surface_keys` (HashMap token→key); `result_triangles`; six dirty/emptiness flags; destructor frees `local_brush`.

**Pure helpers (file-local statics, node-free, will move):**
- `_unpack_manifold` (`:477-537`) — `manifold::Manifold` + material map → `CSGBrush` faces (+ optional per-triangle records). Reads `GetMeshGL64()` (collapses its receiver — must be a copy).
- `_export_meshgl_as_json` (`:540-602`, DEV only) — used by `_pack_manifold`.
- `_pack_manifold` (`:605-686`) — `CSGBrush` → `manifold::Manifold` + `surface_records`; groups faces into one run per semantic surface.
- `_convert_csg_operation` (`:688-697`), `_to_manifold_transform` (`:699-710`).
- `_combine_manifolds` (`:712-723`) — **contains `count_batch_boolean_call()` at `:720`**.
- `enum ManifoldProperty` (`:466-475`) — shared by pack/unpack.
- `_generate_tangents_unindexed` (`:925-961`) — tangent generation (meshoptimizer).

**Node methods that interleave compute + apply (stay in `csg_shape.cpp`, get refactored to delegate):**
- `_ensure_local_manifold` (`:725-769`) — calls virtual `_build_brush()`, `ReserveIDs`, `_pack_manifold`. Counters `:752`, `:765`.
- `_ensure_subtree_manifold` (`:771-808`) — walks visible children in scene order, composes expression. Counters `:789`, `:804`; `_combine_manifolds` at `:791`,`:798`.
- `_ensure_transformed_manifold` (`:810-821`) — `subtree.Transform(get_transform())`. Counter `:819`.
- `_gather_manifold_surface_records` (`:823-836`) — recursive; builds token→material and token→key maps; `_resolve_manifold_material` (`:443-464`) does per-type `get_material()`.
- `_update_cached_aabb_from_manifold` (`:838-856`), `_update_child_manifold_aabbs` (`:858-868`).
- `_get_brush` (`:870-923`) — the fulcrum: early-out on clean cache (`:871`); ensure subtree; gather maps; `_unpack_manifold` into `brush`; **root only:** publish `result_surface_keys`/`result_triangles` and bump `result_generation` (`:897-904`); compute `node_aabb`; set emptiness; `_update_child_manifold_aabbs`; warnings. Counters `:887`/`:889`.
- `update_shape` (`:963-1036`) — root only: `set_base(RID())` + `root_mesh.unref()` (`:968-969`); `_get_brush()`; `_build_surfaces_smoothed/_default`; UV/tangent counters (`:987`,`:1000`); build `ArrayMesh` (`:991-1027`); `set_base(root_mesh->get_rid())` (`:1029`); `update_gizmos()` (`:1031`); `_update_collision_faces()` (`:1034`).
- `_build_surfaces_smoothed` (`:1038-1160`), `_build_surfaces_default` (`:1162-1261`) — brush → `ShapeUpdateSurface` arrays. Read node fields `calculate_tangents`, `smoothing_angle`.
- `_get_brush_collision_faces` (`:1272-1292`), `_update_collision_faces` (`:1294-1305`, counter `:1297`).

**Node-resident types in `csg_shape.h`:** `CSGOriginToken` (`:47`), `CSGSurfaceKey` (`:49-59`), `CSGSurfaceHit` (`:61-67`), `struct ShapeUpdateSurface` (`:116-128`, private nested).

**Consumers of the brush outside `update_shape`:** `get_brush_faces()` (`:1359`, public — used by `editor/csg_gizmos.cpp:386` and 20+ test call sites), `_get_brush_collision_faces` (collision + `bake_collision_shape`), and non-root `_get_brush` (AABB/gizmos). **These call `_get_brush` directly and rely on its generation bump + metadata publish** (Phase 2 tests call `root->get_brush_faces()` then `get_result_generation()`/`resolve_result_triangle()` — e.g. `test_csg.h:537-547`). This coupling is the single most important invariant to preserve.

**Build system:** `SCsub:46` globs `*.cpp` — new `.cpp` files are picked up automatically, **no SCsub edit**. Editor sources are globbed separately under `if env.editor_build` (`:48-49`); the new files are module-core, not editor. Module-test registry auto-discovers `tests/test_csg.h` (per progress log). The module already depends only on `core/`, `scene/`, `servers/`, and `thirdparty/manifold` — **no editor headers**, which the new files must respect.

---

## 2. Target file layout

Three new files; `csg_shape.{h,cpp}` keep node lifecycle, properties, notifications, and thin delegation.

### `modules/csg/csg_manifold_cache.h` (new, header-only)
Moves the persistent per-node cache out of the `.cpp`. Included only by `csg_shape.cpp` and `csg_evaluation.cpp` (keeps `<manifold/manifold.h>` out of `csg_shape.h`, preserving its current forward-declaration of `ManifoldCache` at `csg_shape.h:84`).

Contents (verbatim moves from `csg_shape.cpp:57-99`):
- `struct CSGManifoldSurfaceRecord`
- `struct CSGShape3D::ManifoldCache` — **stays a member type of `CSGShape3D`.** Because it is declared `struct ManifoldCache;` inside the class (`csg_shape.h:84`), its definition may live in a header included by the `.cpp`; define it as `struct CSGShape3D::ManifoldCache { … }` here. Include `csg.h` (for `CSGBrush`) and `<manifold/manifold.h>`.

Rationale for keeping the cache separate from evaluation: the cache is **node-resident mutable state** (dirty flags, retained handles, token ranges) that stays behind on the node in Phase 4; the evaluation types are **detachable values** that will cross to a worker. Different lifetimes, different files.

### `modules/csg/csg_evaluation.h` (new)
Pure evaluation contract — no `CSGShape3D` method bodies, no `Node`, no `RenderingServer`/`PhysicsServer`. May include `csg.h`, `csg_shape.h` (for `CSGSurfaceKey`/`CSGOriginToken`/`CSGShape3D::Operation` enum), `core/templates/*`, `scene/resources/material.h`, `<manifold/manifold.h>`.

Contents:
- `enum ManifoldProperty` (moved from `csg_shape.cpp:466-475`).
- `struct CSGManifoldResultTriangle` (moved from `:63-66`).
- `struct CSGRenderSurface` — relocation of `CSGShape3D::ShapeUpdateSurface` (`csg_shape.h:116-128`) as a standalone struct (see §3, note on transient `…w` pointers).
- `struct CSGEvaluationSettings` — the plain settings snapshot (see §4).
- `struct CSGEvaluationInputs` — immutable job inputs (see §4).
- `struct CSGEvaluationSnapshot` — the detachable result (see §4).
- Free-function declarations (all pure):
  - `manifold::Manifold csg_combine_manifolds(const std::vector<manifold::Manifold>&, manifold::OpType)`
  - `manifold::OpType csg_convert_operation(CSGShape3D::Operation)`
  - `manifold::mat3x4 csg_to_manifold_transform(const Transform3D&)`
  - `void csg_pack_manifold(const CSGBrush*, manifold::Manifold&, CSGOriginToken base, uint32_t schema_size, ObjectID source, uint32_t schema_gen, Vector<CSGManifoldSurfaceRecord>&)`
  - `void csg_materialize_brush(const manifold::Manifold&, const HashMap<CSGOriginToken, Ref<Material>>&, CSGBrush* out, Vector<CSGManifoldResultTriangle>* out_tris)` (== `_unpack_manifold`)
  - `void csg_build_render_surfaces(const CSGBrush*, const CSGEvaluationSettings&, Vector<CSGRenderSurface>&, bool& r_built_tangents)`
  - `void csg_extract_collision_faces(const CSGBrush*, Vector<Vector3>&)`
  - `CSGEvaluationSnapshot csg_build_snapshot(const CSGEvaluationInputs&)` — orchestrates stages 3–8 + collision extraction into one value.

### `modules/csg/csg_evaluation.cpp` (new)
Definitions of everything declared in `csg_evaluation.h`, including the DEV-only `_export_meshgl_as_json` (moved from `:540-602`, keep `#ifdef DEV_ENABLED` + `core/io/json.h`). **The `count_batch_boolean_call()` call stays inside `csg_combine_manifolds`** — the only counter that changes file. All other counter call sites remain in `csg_shape.cpp` (see §6).

### `csg_shape.h` (edited)
- Include `csg_evaluation.h` (or forward-declare its structs) so signatures can reference `CSGEvaluationInputs`/`CSGEvaluationSnapshot`/`CSGRenderSurface`.
- Remove `struct ShapeUpdateSurface` (`:116-128`); replace the two `_build_surfaces_*` member declarations (`:139-140`) — see §3 for whether they stay members.
- Add private method declarations: `CSGEvaluationInputs _gather_evaluation_inputs(bool p_want_render, bool p_want_collision);` and `void _publish_snapshot(CSGEvaluationSnapshot &p_snapshot);` (see §5).
- Keep `CSGSurfaceKey`/`CSGSurfaceHit`/`CSGOriginToken` here (they are public node API used by `resolve_result_triangle` etc.); `csg_evaluation.h` includes `csg_shape.h` to see them. (Include cycle avoided: `csg_evaluation.h` needs only the small POD types + the `Operation` enum from `csg_shape.h`, and `csg_shape.h` does **not** need snapshot bodies — a forward declaration of the three evaluation structs in `csg_shape.h` suffices, with the include pulled into `csg_shape.cpp`.)

### `csg_shape.cpp` (edited)
Include `csg_manifold_cache.h` + `csg_evaluation.h`. Delete the moved definitions (`:57-99`, `:466-475`, `:477-537`, `:540-602`, `:605-723`, `:925-961`, `:1038-1261`). Node methods now delegate to the free functions (§5). Counter call sites stay (§6).

---

## 3. `CSGRenderSurface` and the transient-pointer hazard

`ShapeUpdateSurface` (`csg_shape.h:116-128`) holds both the arrays (`vertices`, `normals`, `uvs`, `tans`, `material`, `last_added`) **and** raw write pointers (`verticesw`, `normalsw`, `uvsw`, `tansw`). Those pointers alias the struct's own `Vector` storage and are valid only within the single build scope; if a `CSGRenderSurface` is copied or moved (as it will be when returned inside `CSGEvaluationSnapshot`'s `Vector<CSGRenderSurface>`), the pointers dangle.

**Contract:** the `…w` fields are build-scratch only. `csg_build_render_surfaces` fills the arrays and uses the pointers internally; **publish (`update_shape`) must read the `Vector` members, never the `…w` pointers.** Recommend zeroing the pointers at the end of `csg_build_render_surfaces`. This is a genuine correctness trap for the implementer and for Phase 4 (the snapshot will be moved across the thread boundary).

Because `csg_build_render_surfaces` reads only `calculate_tangents` and `smoothing_angle`/`autosmooth` from the node, pass them via `CSGEvaluationSettings`. Convert `_build_surfaces_smoothed`/`_build_surfaces_default` into `csg_build_render_surfaces` (free, settings-driven). (Alternative lower-churn option: keep them as members reading node fields and defer their move to Phase 4. Not recommended — it leaves render-build node-coupled and defeats the seam. Move them now.)

---

## 4. Type definitions

### `CSGEvaluationSettings`
Plain copyable settings snapshot — the "plain settings snapshot" of plan §15. No pointers to nodes/resources except material `Ref`s carried by the inputs (see below).
```cpp
struct CSGEvaluationSettings {
    bool  autosmooth        = false;
    float smoothing_angle   = 50.0f;
    bool  calculate_tangents = true;
    bool  want_collision    = false;   // root + use_collision + inside tree
    bool  want_render       = false;   // build ArrayMesh surfaces
};
```

### `CSGEvaluationInputs`
Everything a worker needs to build a snapshot **without touching scene nodes** (plan §15 threading contract). Assembled on the main thread by `_gather_evaluation_inputs`.
```cpp
struct CSGEvaluationInputs {
    // Immutable Manifold value — a COPY of manifold_cache->subtree_manifold.
    // GetMeshGL64()/IsEmpty()/BoundingBox() collapse their receiver into an
    // evaluated leaf; building from a copy preserves the cached handle's
    // operation-node identity (Phase 1 gotcha, csg_shape.cpp:892-894, :840-844).
    manifold::Manifold subtree;

    // Provenance gathered from the live tree (touches nodes/materials) up front,
    // so the build step is node-free.
    HashMap<CSGOriginToken, Ref<Material>> mesh_materials;
    HashMap<CSGOriginToken, CSGSurfaceKey>  surface_keys;

    CSGEvaluationSettings settings;

    // Validation identity for publish (Phase 4 checks these before swapping).
    ObjectID root_id;
    uint32_t schema_generation = 0;
    uint64_t request_generation = 0;   // pre-step: current gen; Phase 4: scheduler request id
    bool     want_result_metadata = false;  // true only for root (mirrors is_root_shape())
};
```
Note on `Ref<Material>`: the build step stores these handles into `CSGBrush::materials` and groups by pointer identity — it does **not** read Resource contents, satisfying "jobs do not access Resource contents." Assembling them into an `ArrayMesh` is publish-side.

### `CSGEvaluationSnapshot`
Everything a completed evaluation publishes. Owns its geometry; safe to construct on any thread, move to the main thread, and swap onto the root.
```cpp
struct CSGEvaluationSnapshot {
    CSGBrush *brush = nullptr;                 // owned; materialized geometry
    HashMap<CSGOriginToken, CSGSurfaceKey> result_surface_keys;
    Vector<CSGManifoldResultTriangle>      result_triangles;
    AABB node_aabb;
    bool subtree_empty = true;

    Vector<CSGRenderSurface> render_surfaces;  // empty for brush-only builds
    Vector<Vector3>          collision_faces;  // empty unless settings.want_collision
    bool built_render   = false;
    bool built_tangents = false;               // publish-side tangent-counter decision

    ObjectID root_id;                          // carried from inputs for publish validation
    uint32_t schema_generation = 0;
    uint64_t request_generation = 0;

    ~CSGEvaluationSnapshot() { if (brush) memdelete(brush); }  // if unpublished
    // Non-copyable; movable (transfers brush ownership).
};
```
`result_generation` is **not** in the snapshot — it is assigned at publish by bumping the node's counter (preserving current semantics where the number is a property of the publishing node, `csg_shape.cpp:900-903`). Phase 4 will instead validate `request_generation` against the root's requested/published generation before the bump.

**Contracts:**
- (a) *Worker-buildable:* `csg_build_snapshot(inputs)` reads only `inputs` — pure Manifold/CSGBrush/Vector math. No `Object`, `Node`, `RenderingServer`, `PhysicsServer`, `EditorSelection`.
- (b) *Atomic publish:* `_publish_snapshot` swaps `result_surface_keys`/`result_triangles`/`node_aabb`/`subtree_empty` onto the node, bumps `result_generation`, then pushes render + collision to servers. In the pre-step this is a plain call; in Phase 4 it is the `call_deferred` main-thread landing.
- (c) *Same flow both ways:* the synchronous path (`update_shape`) is exactly `inputs = gather(); snap = csg_build_snapshot(inputs); _publish_snapshot(snap);`. Phase 4 inserts only the thread hop between `gather` and `build`, and the scheduler around it.

---

## 5. Stage assignment (plan §14's 11 stages) → build vs publish, with current code

| # | Stage | Side | Current location | Destination |
|---|-------|------|------------------|-------------|
| 1 | Dirty-state propagation | **Main / node-resident** (pre-gather) | `_make_dirty`/`_invalidate_*`/`_make_transform_dirty` (`:400-424`), `_notification` (`:1381-1464`) | Unchanged in `csg_shape.cpp`. Never crosses the boundary. |
| 2 | Cache/expression construction | **Main / gather** (reads scene graph) | `_ensure_local/subtree/transformed_manifold` (`:725-821`), `_combine_manifolds` | Stays node-side; `_combine_manifolds` body → `csg_evaluation.cpp` but **called during gather**. Produces the immutable `subtree` handle. |
| 3 | Manifold evaluation | **Build** (pure) | `GetMeshGL64()` inside `_unpack_manifold` (`:482`) | `csg_materialize_brush` in `csg_evaluation.cpp`, on the `inputs.subtree` **copy**. |
| 4 | Raw result extraction | **Build** (pure) | `_unpack_manifold` (`:477-537`) | `csg_materialize_brush`. |
| 5 | Provenance resolution | **Split** | gather map: `_gather_manifold_surface_records` (`:823-836`) touches nodes → **main/gather**; per-triangle stamping in `_unpack_manifold` (`:524-531`) → **build** | Map gathered into `inputs`; stamping in `csg_materialize_brush`. |
| 6 | Material grouping | **Build** (pure) | material-id assignment in `_unpack_manifold` (`:492-501`) using the map | `csg_materialize_brush`. |
| 7 | UV generation | **Build** (pure) | UVs baked into brush (`_pack_manifold`), copied in `_build_surfaces_*` | `csg_build_render_surfaces`. (Planar UV is Phase 6.) |
| 8 | Normals/smoothing/tangents | **Build** (pure) | `_build_surfaces_smoothed/_default` (`:1038-1261`), `_generate_tangents_unindexed` (`:925-961`) | `csg_build_render_surfaces`. |
| 9 | Rendering publication | **Publish** (RenderingServer) — cannot move | `set_base(RID())`/`root_mesh.unref()`/`ArrayMesh`/`set_base`/`update_gizmos` (`:968-1031`) | `_publish_snapshot`. |
| 10 | Picking acceleration update | **Publish** (node-resident swap) | `result_surface_keys`/`result_triangles`/`result_generation` store in `_get_brush` (`:897-904`) | Data built in stages 3–5; **swap + generation bump** in `_publish_snapshot`. |
| 11 | Collision publication | **Split** | face extraction `_get_brush_collision_faces` (`:1272-1292`) → **build (pure)**; `set_faces` + debug instance (`:1294-1305`) → **publish** (Physics/RenderingServer) — cannot move | Extraction → `csg_extract_collision_faces`; `set_faces` → `_publish_snapshot`. |

**Cannot cleanly move (publish by definition):** `set_base` (VisualInstance/RenderingServer), `ArrayMesh::add_surface_from_arrays` (RenderingServer), `update_gizmos`, `root_collision_shape->set_faces` + `PhysicsServer` body-transform + debug `RenderingServer` instance (`:1323-1352`), `update_configuration_warnings`, `_update_child_manifold_aabbs` (walks child nodes and evaluates their cached handles). These stay on the main thread in `_publish_snapshot`.

**Hidden ordering dependencies (call these out to the implementer):**
1. **Clear-then-rebuild order.** `update_shape` currently does `set_base(RID())` + `root_mesh.unref()` **before** building (`:968-969`). Move this into `_publish_snapshot` **after** the build completes. Synchronously identical; required so Phase 4 keeps the prior published mesh visible until the new snapshot is ready (plan §14 interactive). Do not clear in `gather`.
2. **Generation bump is coupled to brush materialization, not to `update_shape`.** `get_brush_faces()` (tests, gizmos) reaches `_get_brush` without `update_shape` and expects `result_generation` to advance and `resolve_result_triangle` to work (`test_csg.h:537-547`, `:596-618`, `:703-733`). Therefore the root `_get_brush` path must continue to publish metadata + bump generation. See §5.1.
3. **Dirty early-out.** `_get_brush` returns the cached brush with **no materialization and no generation bump** when `!materialization_dirty && brush` (`:871-874`). `update_shape` on a clean cache must produce `root_materializations == 0` (`test_csg.h:342-348`). `gather` must reproduce this early-out (skip build, reuse cached brush) — see §5.1.
4. **`_ensure_subtree_manifold` sets `transformed_manifold_dirty = true` / `materialization_dirty = true`** as a side effect (`:800-802`). Keep this in the node method; it is dirty-bookkeeping, not build work.

### 5.1 How `_get_brush` and `update_shape` are refactored (preserving all of the above)

Keep `_get_brush()` as the **brush-materialization + metadata-publish** entry (unchanged responsibilities), but express its body through the shared pieces:

```cpp
CSGBrush *CSGShape3D::_get_brush() {
    if (!manifold_cache->materialization_dirty && brush) { dirty = false; return brush; }   // early-out (:871)
    CSGEvaluationInputs inputs = _gather_evaluation_inputs(/*render*/false, /*collision*/false);
    // counter: root_/non_root_materialization stays HERE (main thread), wrapping the pure call
    #ifdef DEV_ENABLED … count_root/non_root … #endif
    CSGBrush *out = memnew(CSGBrush);
    Vector<CSGManifoldResultTriangle> tris;
    csg_materialize_brush(inputs.subtree /*copy already*/, inputs.mesh_materials, out,
                          inputs.want_result_metadata ? &tris : nullptr);
    // publish metadata + generation + aabb + emptiness + child aabbs + warnings  (== current :897-921)
    …
    return brush;
}
```

`update_shape()` remains the render/collision publisher and continues to call `_get_brush()` first (so materialization, generation, and the dirty early-out are shared exactly once):

```cpp
void CSGShape3D::update_shape() {
    if (!is_root_shape()) return;
    CSGBrush *n = _get_brush();                      // stages 1-5,10 (+ early-out, generation)
    ERR_FAIL_NULL(n);
    CSGEvaluationSettings s = { autosmooth, smoothing_angle, calculate_tangents,
                                use_collision && root_collision_shape.is_valid(), /*render*/true };
    CSGEvaluationSnapshot snap;                        // brush not owned here; render payload only
    #ifdef DEV_ENABLED count_uv_finalization(); #endif
    csg_build_render_surfaces(n, s, snap.render_surfaces, snap.built_tangents);   // stages 6-8
    if (s.want_collision) csg_extract_collision_faces(n, snap.collision_faces);   // stage 11 build
    _publish_snapshot(snap);                           // stages 9,11-publish
}
```

`_publish_snapshot` does: `set_base(RID())` + `root_mesh.unref()`; build `ArrayMesh` from `render_surfaces` (firing `count_tangent_finalization` once when `built_tangents`, matching `:998-1002`); `set_base(root_mesh rid)`; `update_gizmos()`; if collision, `root_collision_shape->set_faces(collision_faces)` + `count_collision_rebuild` + debug shape (== `_update_collision_faces` body, `:1294-1305`).

This keeps `update_shape` synchronously identical while giving Phase 4 a clean split: `_get_brush`'s materialization body is exactly what moves to the worker (it is already `csg_materialize_brush` + a copy of `inputs`), and `_publish_snapshot` is the main-thread landing. The full "single snapshot" object materializes at the seam — for the pre-step the snapshot carries render+collision; Phase 4 folds the brush+metadata (currently published inside `_get_brush`) into the same snapshot built by the worker.

> **Design note for the implementer:** the pre-step deliberately leaves the brush+metadata publish inside `_get_brush` rather than the snapshot, because `get_brush_faces()`/collision baking need brush+generation without render surfaces, and folding render-build into every `_get_brush` call would fire UV/tangent counters and hit `RenderingServer` on paths that must not (breaking pins and correctness). Phase 4 unifies them by making the *scheduler* (not `get_brush_faces`) the sole driver of root re-evaluation; at that point `_get_brush`'s root branch becomes "read last published snapshot." Do not attempt that unification now.

---

## 6. Counter call-site moves (enumerated; counts must stay identical)

Of eleven counters (`csg_debug_counters.h:37-63`), **only one call site changes file**, and it fires identically:

| Counter | Current site | After |
|---------|-------------|-------|
| `count_batch_boolean_call` | inside `_combine_manifolds` (`:720`) | **moves to `csg_evaluation.cpp`** inside `csg_combine_manifolds`; still called only from `_ensure_subtree_manifold` (`:791`,`:798`). Count unchanged. |
| `count_local_primitive_brush_pack` | `_ensure_local_manifold` (`:752`) | stays (node method). |
| `count_leaf_manifold_repack` | `_ensure_local_manifold` (`:765`) | stays. |
| `count_operation_switch_flush` | `_ensure_subtree_manifold` (`:789`) | stays. |
| `count_expression_node_reconstruction` | `_ensure_subtree_manifold` (`:804`) | stays. |
| `count_transformed_wrapper_construction` | `_ensure_transformed_manifold` (`:819`) | stays. |
| `count_root_materialization` / `count_non_root_materialization` | `_get_brush` (`:887`/`:889`) | **stays in `_get_brush`**, wrapping the `csg_materialize_brush` call on the main thread. Not moved into the pure function (keeps DEV code out of the future worker path). |
| `count_uv_finalization` | `update_shape` (`:987`) | stays in `update_shape`, before `csg_build_render_surfaces`. |
| `count_tangent_finalization` | `update_shape` (`:1000`, once per shape when tangents built) | moves to `_publish_snapshot` (ArrayMesh assembly), gated on `snap.built_tangents`, fired once. Same value. |
| `count_collision_rebuild` | `_update_collision_faces` (`:1297`) | moves to `_publish_snapshot` collision branch. Same value (one per root `update_shape` with collision). |

Because materialization, UV, and collision counters stay bound to the same node-method call sequences (`_get_brush`, `update_shape`/`_publish_snapshot`) and the dirty early-out is preserved, every pinned count is reproduced:
- `test_csg.h:131-142` (Add/Subtract/Intersect): 3 packs / 3 repacks / 3 wrappers / 4 exprs / 3 batch / 2 flushes / 1 root-mat — all sites unchanged, `batch` still fired inside the (relocated) combine.
- `:236-241` UV test: root-mat 1, uv 1, tangent 1 — uv stays in `update_shape`, tangent fires once in publish.
- `:340-349` clean `update_shape`: all zero — early-out in `_get_brush` still short-circuits before any counter, and `gather` is not reached because `_get_brush` returns early.
- `:294-297` collision: `collision_rebuilds == 1` — one publish with collision.
- Non-root materialization (`:474`, `:481`) via `nested->get_brush_faces()` — `_get_brush` non-root branch unchanged.

---

## 7. Migration order (each step compiles and passes 18/18)

**Step 0 — Baseline.** Build dev+tests, confirm 18/18 / 912 green (`scons platform=windows target=editor dev_build=yes tests=yes winrt=no -j24`; `--test --test-case="*CSG*"`). Record the counter pins.

**Step 1 — Extract the cache header.** Create `csg_manifold_cache.h`; move `CSGManifoldSurfaceRecord` + `CSGShape3D::ManifoldCache` (`csg_shape.cpp:57-99`) into it; include it from `csg_shape.cpp`. Pure relocation, no logic change. Build + test.

**Step 2 — Extract pure evaluation into `csg_evaluation.{h,cpp}`.** Move `enum ManifoldProperty`, `CSGManifoldResultTriangle`, `_pack_manifold`→`csg_pack_manifold`, `_unpack_manifold`→`csg_materialize_brush`, `_convert_csg_operation`→`csg_convert_operation`, `_to_manifold_transform`→`csg_to_manifold_transform`, `_combine_manifolds`→`csg_combine_manifolds` (carry the `count_batch_boolean_call`), `_export_meshgl_as_json`, `_generate_tangents_unindexed`. Make them non-static, declare in header. Node methods call the new names. **No counter count change.** Build + test (this is the biggest mechanical move; verify pins here).

**Step 3 — Relocate render-surface build.** Introduce `CSGRenderSurface` + `CSGEvaluationSettings` in `csg_evaluation.h`; convert `_build_surfaces_smoothed/_default` into `csg_build_render_surfaces` (free, settings-driven) and add `csg_extract_collision_faces` (wrapping current `_get_brush_collision_faces` logic). Remove `ShapeUpdateSurface` from `csg_shape.h`. Rewire `update_shape` to call the free functions; zero the `…w` scratch pointers post-build (§3). Build + test (UV/tangent/collision counts).

**Step 4 — Introduce `CSGEvaluationInputs` + `_gather_evaluation_inputs`.** Extract the gather logic currently inline in `_get_brush` (`_ensure_subtree_manifold` + `_gather_manifold_surface_records` + the `subtree_manifold` **copy** + settings read + `want_result_metadata = is_root_shape()`) into `_gather_evaluation_inputs`. `_get_brush` calls it, then `csg_materialize_brush` on `inputs.subtree`. Preserve the early-out and the materialization counters exactly (§5.1). Build + test.

**Step 5 — Introduce `CSGEvaluationSnapshot` + `_publish_snapshot`.** Move `set_base(RID())`/`root_mesh.unref()` to publish (after build); move ArrayMesh assembly, `set_base`, `update_gizmos`, and the collision `set_faces`/debug into `_publish_snapshot`; move the tangent + collision counters accordingly. `update_shape` becomes gather-via-`_get_brush` + `csg_build_render_surfaces` + `_publish_snapshot` (§5.1). Build + test — this is the behavior-sensitive step (clear-then-rebuild ordering); verify the full suite and the empty/hidden-child cases (`test_csg.h:247-276`).

**Step 6 — `/simplify` pass + commit.** One commit for the pre-step (Co-Authored-By trailer per progress-log convention), after re-verifying 18/18 / 912 and the counter pins.

Each step is independently revertable. Steps 1–3 are pure relocations (lowest risk); 4–5 introduce the seam types; the only ordering-behavior change is the deferred clear in Step 5.

---

## 8. Test expectations

- **No test edits.** `tests/test_csg.h` compiles against unchanged public API (`get_brush_faces`, `get_result_generation`, `resolve_result_triangle`, `get_surface_*`, `bake_*`, `update_shape`). It includes `../csg_shape.h` only; it does **not** reference `ManifoldCache`, `ShapeUpdateSurface`, or the free functions, so the moves are invisible to it.
- All 18 cases / 912 assertions pass unmodified. Counter pins verified at Steps 2–5 as enumerated in §6.
- Watch especially: `Phase 1 transform resize and clean rebuild invalidation` (`:303-352`, the clean-`update_shape`-all-zero pin — depends on the preserved early-out) and `Phase 2 boolean surface provenance` (`:530-582`, depends on `get_brush_faces()`→generation coupling).
- The polygon suite (`:792-861`) exercises `get_brush_faces()` twice to force AABB updates; ensure `_get_brush`/`_gather` still refresh `node_aabb` for non-root and root (via the publish/metadata path preserved in §5.1).

---

## 9. Risks and mitigations

| Risk | Mitigation |
|------|-----------|
| Snapshot build accidentally evaluates the **cached** subtree handle, collapsing operation-node identity (Phase 1 gotcha) | `CSGEvaluationInputs.subtree` is a **copy** taken in `_gather_evaluation_inputs`; `csg_build_snapshot`/`csg_materialize_brush` only ever see the copy. Document the invariant on the field (§4) and mirror the existing comments at `csg_shape.cpp:892-894`,`:840-844`. |
| Counter drift when call sites move | Only `count_batch_boolean_call` changes file (inside the relocated combine); all materialization/UV/tangent/collision counters stay bound to their node methods. Verify at each step against recorded pins (§6). |
| Losing the `_get_brush` dirty early-out → clean `update_shape` re-materializes (breaks `:342-348`) | `_get_brush` keeps its `!materialization_dirty && brush` early-out verbatim; `update_shape` continues to route materialization through `_get_brush`, not a fresh build (§5.1). |
| Deferring `set_base(RID())`/`unref` changes visible behavior | Synchronous flow is byte-identical (clear then immediately republish within the same call). The move is required for Phase 4 and validated by the empty/hidden-child + collision cases. |
| Dangling `…w` write pointers after `CSGRenderSurface` is moved into the snapshot `Vector` | Contract: `…w` are build-scratch; publish reads `Vector` members only; zero the pointers post-build (§3). |
| `Ref<Material>` handled off the main thread in Phase 4 | Pre-step gathers/resolves all materials on the main thread into `inputs.mesh_materials`; build only stores handles + groups by identity, never reads Resource contents (§4). Establishes the Phase 4-safe boundary now. |
| Include cycle `csg_shape.h ↔ csg_evaluation.h` | `csg_shape.h` forward-declares the three evaluation structs; the full `csg_evaluation.h` include lives in `csg_shape.cpp`. `csg_manifold_cache.h` (with `<manifold/manifold.h>`) is included only by the two `.cpp` files, keeping Manifold out of the public node header. |
| New files not compiled / editor-dep leak | `SCsub:46` auto-globs `*.cpp` — no SCsub change; new files are module-core and include no editor headers, satisfying plan §3 ("CSG module must not depend on editor types"). |

---

## 10. Explicitly out of scope (seams only)

No worker threads, no `CSGEvaluationScheduler`, no per-root running/pending-job state, no generation coalescing or stale-result rejection, no interactive-vs-final quality policy, no picking/hover wiring, no editor/session/chrome code. This pre-step produces exactly: `CSGEvaluationInputs`, `CSGEvaluationSnapshot`, `csg_build_snapshot`, `_gather_evaluation_inputs`, `_publish_snapshot`, and the three new files — the clean seam onto which Phase 4's scheduler and thread hop attach between `gather` and `build`.

---

### Critical files
- `modules/csg/csg_shape.cpp`, `modules/csg/csg_shape.h`
- `modules/csg/csg_debug_counters.h`
- `modules/csg/tests/test_csg.h`
- `modules/csg/SCsub` (no edit expected — glob picks up new `.cpp`)

New files: `modules/csg/csg_manifold_cache.h`, `modules/csg/csg_evaluation.h`, `modules/csg/csg_evaluation.cpp`.
