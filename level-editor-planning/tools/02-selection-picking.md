# Tool 02 — Selection system & sub-object picking (Scythe-class)

**Status:** planning. **Prereq:** `../PLAN.md` (§1 Architecture, §Picking; §5 risk 2). This tool is
the whole of milestone **LE1** minus the transform operators — it is what makes every later tool
(move, extrude, texture, hotspot, boolean) have something to act on. It is also, per the Cyclops
assessment, the single highest crash-risk subsystem (stale sub-object handles). Design accordingly.

Binding recap (from the task brief): `1/2/3` = polygroup vert/edge/face, `4` = objects,
`6` toggles polygroup↔triangle tier, `Shift+Ctrl+1/2/3` = triangle-tier vert/edge/face;
double-click = flood fill (contiguous/coplanar); `CapsLock`+double-click = all faces on plane;
LMB-drag = path (paint) select; `L` = edge loop, `X` = edge ring; `Ctrl+A` all, `Ctrl+I` invert,
`Alt+KP +/-` grow/shrink; marquee box select. Note Scythe's `X`=ring differs from Alyx's `G`=ring;
we keep the brief's `L`/`X` and cite Alyx `L`/`G` only as behavioral reference.

---

## 1. End-to-end operation

A pick is a pipeline: **input → ray/region → broad phase → narrow phase → element resolve →
selection delta → highlight invalidate → tool handoff.** Selection is DOCUMENT state, never render
state, never an undo step.

1. **Input routing.** `LevelEditorView` (VIEW STATE, one per pane) receives the `InputEvent`, builds
   `ray_origin/ray_dir` from its own camera (`Camera3D::project_ray_*`), and hands it to the active
   tool. Selection is a *ubiquitous* handler the Select tool owns but every tool falls back to
   (Scythe/Hammer: you can always click-select). Modifier grammar: plain = replace, `Shift` = add,
   `Ctrl` = toggle, `Shift+Ctrl` = subtract (matches the fork's `_select_clicked` conventions).

2. **Broad phase — which blocks.** Reuse the fork's per-world gizmo BVH:
   `Node3DEditor::gizmo_bvh_ray_query(start, end, view->get_world_3d())`
   (`editor/scene/3d/node_3d_editor_plugin.cpp:4418`). Each `LevelBlock` registers its world AABB
   via `insert_gizmo_bvh_node` on enter-world and `update_gizmo_bvh_node` on bake. The world-scoping
   fix (`630785ab`) guarantees only *this document's* blocks come back — no cross-pane leak. Returns
   an unordered `Vector<Node3D*>` of candidate blocks.

3. **Narrow phase — element inside a block.** For each candidate we go into the KERNEL, never
   physics. Each `LevelMesh` owns a **per-block element BVH** over its baked triangles (built from
   the columnar `face_vertex_indices` triangulation, each leaf carrying `tri → (polygroup_face_id,
   material)`), queried in block-local space (transform the ray by `block.global_transform.affine_inverse()`).
   `intersect_ray_closest` returns `{hit, t_world, tri_id, face_id, bary}`. This gives the solid
   surface depth used for occlusion (step 5).

4. **Element resolve — mode + tier.** From the hit face:
   - **Face mode (`3`):** element = `face_id`. Polygroup tier → the face's `polygroup_id` expands to
     all faces sharing it (expansion at SELECTION level, not pick — PLAN §Picking). Triangle tier
     (`Shift+Ctrl+3`) → the raw `tri_id`.
   - **Edge mode (`2`):** gather the hit face's boundary edges (polygroup tier: the polygroup's
     outer boundary edges; triangle tier: the tri's 3 edges), project each to screen, pick nearest
     within `edge_tol_px` (default 8 px). No candidate within tolerance → miss (deselect on plain).
   - **Vertex mode (`1`):** gather the hit face's corner verts (+ verts of blocks whose screen-space
     bbox is within `vert_tol_px` of the cursor, from the broad-phase set), project, pick nearest
     within `vert_tol_px` (default 10 px).
   - **Object mode (`4`):** element = the block node; selection routes to the document's
     `EditorSelection` like any Node3D (interop with the scene tree).

5. **Occlusion.** Default **front-facing / depth-tested**: reject a vert/edge candidate whose
   projected depth is behind the solid surface depth from step 3 by more than `depth_bias`
   (`1e-3 * dist`). **X-ray toggle** (`Z`, per-tier) skips the reject. When ≥2 candidates survive
   within tolerance, sort by depth and **cycle on Spacebar / repeated same-spot click** (Alyx
   "Select cycles with Spacebar"); the active candidate is remembered per cursor cell so a second
   click advances rather than re-picks.

6. **Selection delta.** Resolve produces a `SelectionOp{feature, tier, handles[], mode}` applied to
   the per-document `SelectionModel` (§2). It mutates three keyed sets + an `active` element,
   emits `selection_changed`, and **does not** touch the undo stack (TrenchBroom rule; PLAN §Undo).

7. **Highlight invalidate.** `selection_changed` marks the view's overlay dirty per affected block.
   The overlay renderer (§2) rebuilds only dirty blocks' highlight meshes and reconciles
   `RenderingServer` instances on the per-world gizmo cull layer.

8. **Tool handoff.** On drag/operator start, the active tool takes a **snapshot** of the selection as
   stable handles (§ difficulty C) — e.g. Move resolves handles → current vertex positions;
   Extrude resolves the selected face set. The operator emits a kernel data-diff; the diff's
   removed-element list drives selection revalidation (§ difficulty C) transactionally, so the
   post-op selection and highlight are always consistent with the new topology.

---

## 2. Foundation services required

- **Per-block element BVH (kernel, `LevelMesh::element_bvh`).** `DynamicBVH` (same primitive the
  fork gizmo BVH uses) or a static AABB tree over baked triangles; leaf → `(tri_id, face_id)`.
  Provides `ray_closest`, `verts_near_screen`, `edges_near_screen`. Built lazily, rebuilt for a
  block only when its kernel diff touches positions/topology (dirty-flag, mirrors
  `update_gizmo_bvh_node` incremental use). This is the narrow phase; the gizmo BVH stays coarse.
- **Screen-space hit tester (view service).** Projects kernel elements through the pane camera,
  does px-tolerance nearest tests, front-face/depth reject vs the narrow-phase surface depth, and
  cursor-cell cycle state. Lives on `LevelEditorView` (needs camera + viewport size); takes kernel
  geometry by const ref (seam rule 1: view holds no kernel mutation).
- **Adjacency query service (kernel, `LevelMesh::Adjacency`).** Derived caches from the columnar
  arrays: `vert→edges`, `edge→(faceA,faceB)` (from `edge_face_indices`), `face→(edges,verts,loops)`,
  and the loop/ring step primitives (`edge_across_quad`, `edge_parallel_in_quad`). Invalidated
  wholesale on any topology diff, rebuilt lazily. Required by flood fill, loop/ring, grow/shrink,
  polygroup boundary extraction.
- **Selection storage per document (`SelectionModel`).** Owned by `LevelDocument`. Holds, per
  feature type, an ordered set of **stable handles** (not free-list ids — see C), plus `active`
  handle and current `(mode, tier)`. Provides `apply(SelectionOp)`, `snapshot()/restore()`,
  `revalidate(removed_ids)`, `for_each_resolved(cb)`. Snapshot/restore hooks into the existing
  per-scene selection save/restore path (`EditorData::save/restore_edited_scene_state`,
  `editor_data.cpp:983`) so multi-doc tab switches preserve sub-object selection.
- **Overlay renderer (view).** Owns `RenderingServer` instances on the document scenario, per-world
  gizmo cull layer (`allocate/free_gizmo_layer`), lifecycle create-detached→reconcile→free-in-dtor
  (copied from `Node3DEditorView`). Three persistent meshes per document — vertex handles
  (MultiMesh billboards), edge lines (`PRIMITIVE_LINES`), face fill (translucent tris) — plus the
  marquee screen-rect (2D overlay draw, cf. Cyclops `draw_screen_rect`).

---

## 3. Core difficulties (chosen solution + sketch + references)

### A. Screen-space vertex/edge picking with tolerance + occlusion

**Options weighed.** (i) GPU ID/back-buffer pick (Blender's `ED_view3d_backbuf` / `drw_select`):
render element ids to an offscreen buffer, read back the pixel. Pixel-exact occlusion for free, but
needs a GPU readback stall per click, a bespoke id-render pass, and **does not work headless** — a
non-starter given the kernel must be script-drivable for `tools/checks`. (ii) Pure geometric
project-and-measure with BVH depth for occlusion.

**Chosen: (ii) geometric BVH + depth reject.** Deterministic, headless-testable, no readback, reuses
the kernel BVH we already need for face picking.

Sketch (edge mode; vertex mode is the degenerate 1-endpoint case):
```
ray = camera.project_ray(mouse)
blocks = gizmo_bvh_ray_query(ray, view.world)          # broad
surf = {}                                               # nearest solid depth per block hit
for b in blocks:
    lray = to_local(ray, b)
    surf[b] = b.mesh.element_bvh.ray_closest(lray)      # narrow: tri, face, t
hitface = argmin_t(surf)                                # the face under the cursor
cands = boundary_edges(hitface, tier) ∪ edges_near_screen(mouse, vert_tol_px)
best = null; bestpx = tol
for e in cands:
    p2 = project_segment(e); d = dist_point_seg_px(mouse, p2)
    if d > tol: continue
    if not xray and depth(e) > surf_depth_at(mouse) + bias: continue   # occluded
    if d < bestpx: bestpx = d; best = e
# ties within tol → push to cycle list, Spacebar advances
```
Tolerance is in **pixels** (screen-constant handle size — the fork's `_transform_gizmo_select`
picks handles by screen radius the same way). Depth bias scales with distance to avoid z-fighting on
coplanar trim.

**References:** fork `Node3DEditorViewport::_transform_gizmo_select`
(`editor/scene/3d/node_3d_editor_viewport.cpp:1779`) for screen-radius handle tolerance;
Blender `view3d_select.cc` `unified_findnearest` + `ED_view3d_backbuf` (the GPU approach we reject,
[dfelinto/blender editmesh source](https://github.com/dfelinto/blender)); TrenchBroom `PickResult`
depth-ordered `Hit` list (kduske/TrenchBroom, `Model/PickResult.h`) for the "nearest wins, keep the
rest for cycling" pattern.

### B. Edge loop / ring traversal on an n-gon mesh

**Problem.** Real loop/ring algorithms assume quads; our kernel is n-gon with polygroups. Undefined
walk rules here are a correctness and crash source. **Chosen: quad-gated walk, n-gons and
non-4-valence are hard terminators** — exactly Blender/Alyx observed behavior ("stops at n-gons,
stops at vertices with >3 edges" / boundary).

Definitions on the indexed mesh: an edge `e` has faces `(fA,fB)` from `edge_face_indices`
(one may be −1 = boundary). "Quad" = face with `face_vertex_count==4`.

**Edge LOOP walk (`L`) — walks *along* the edge's direction, vertex to vertex:**
```
step(e, v):                      # leave edge e through vertex v
    f = the quad in (fA,fB) containing v   # must be a quad, else STOP
    e2 = the edge of f incident to v that is NOT e and NOT sharing f's opposite corner
         (i.e. the edge across the quad from e at v)
    if valence(v) != 4: STOP     # loop only continues through 4-way junctions
    return e2
```
Walk both directions from the seed edge; **terminate** on: STOP above, boundary edge, leaving into a
non-quad (n-gon/tri), or returning to the seed edge (closed loop → whole loop selected).

**Edge RING walk (`X`) — walks *across* quads to the parallel edge:**
```
step(e, f):                      # cross face f
    if f is not a quad: STOP
    e2 = the unique edge of f sharing NO vertex with e   # opposite side of the quad
    return e2, other_face(e2, f)
```
Walk through `fA` then `fB`; terminate on non-quad, boundary (`other_face == -1`), or return to seed.
Both walks are O(loop length), guarded by a visited-set to make the closed case terminate and to
make a malformed mesh (should never happen post-validation) fail safe rather than spin.

Polygroup-tier interaction: loops/rings compute on the raw edge graph, then the *result* is
lifted to the active tier (polygroup edges the loop passes through get whole-polygroup-boundary
selected if the tier is polygroup). Double-click on an edge = run the loop walk (Alyx: double-click
edge = loop).

**References:** Blender `bmesh_walkers_impl.c` `BMW_EdgeLoopWalker` / `BMW_EdgeRingWalker`
([dfelinto/blender bmesh_walkers_impl.c](https://github.com/dfelinto/blender/blob/master/source/blender/bmesh/intern/bmesh_walkers_impl.c),
[walkers overview](https://developer.blender.org/docs/features/objects/mesh/bmesh/)); Alyx/Source 2
Hammer *Mesh Editing 2* loop=`L`/double-click, ring=`G`
([Valve Developer Community — Mesh Editing 2](https://developer.valvesoftware.com/wiki/Dota_2_Workshop_Tools/Level_Design/Basic_Construction/Mesh_Editing_2)).

### C. Selection validity across topology edits (the Cyclops crash class)

**Problem.** A face id is a free-list slot; after an undo/redo or an operator, that slot may be dead
or reused. Cyclops' top crash class was selection holding stale indices. **Chosen: generation-stamped
stable handles + diff-driven revalidation, use-time guarded.** This is the exact pattern the fork
already adopted for `clicked` ObjectID (`630785ab`: "Clear the pending clicked ObjectID when
set_editor_world rebinds… keeping the use-time guard as defense in depth").

Mechanics:
- Every kernel element slot carries a `generation:u32`. A handle is `{slot, generation}`. Freeing a
  slot bumps its generation; re-allocating the slot uses the new generation. `resolve(handle)`
  returns null unless `slots[slot].alive && slots[slot].generation == handle.generation`.
- The `SelectionModel` stores **handles**, never bare slots. Every read goes through `resolve` — a
  stale handle silently yields nothing (no dereference, no crash). Defense in depth even if
  revalidation is skipped.
- Each kernel operator diff exposes `removed_element_ids` per feature type. On diff
  apply/revert (including undo/redo), `SelectionModel::revalidate(removed)` drops exactly those
  handles — **O(changed), not O(selection)** — because ids are stable, surviving elements keep their
  handle. This is transactional with the diff (PLAN risk 2): bake, adjacency cache, and selection all
  invalidate against the same diff so they can never disagree.
- Selection is never on the undo stack (TrenchBroom), but undo/redo of a *geometry* diff still runs
  revalidation, so undoing an extrude that created faces drops the now-nonexistent face handles.

Sketch:
```
struct Handle { uint32 slot; uint32 gen; };
resolve(h): return slots[h.slot].gen == h.gen && slots[h.slot].alive ? h.slot : INVALID;
on_diff(diff):                       # apply or revert
    for id in diff.removed[feature]: sel[feature].erase(id)   # id is stable+gen-stamped
    adjacency.invalidate(); baker.mark_dirty(diff.blocks); overlay.mark_dirty(diff.blocks)
```

**References:** fork commit `630785ab` (stale-handle use-time guard + clear-on-rebind);
geometry3Sharp `DMesh3` refcount/generation model (`RefCountVector` guarding vert/edge/tri slots,
[gradientspace/geometry3Sharp](https://github.com/gradientspace/geometry3Sharp)); TrenchBroom
`NotifyBeforeRemove` selection-drop-on-delete (kduske/TrenchBroom).

### D. Marquee correctness + per-tier semantics (secondary)

Frustum from the drag rect via `MathUtil.calc_frustum_camera_rect`-style planes (Cyclops
`math_util.gd:640`), broad phase `gizmo_bvh_frustum_query(planes, view.world)` (`node_3d_editor_plugin.cpp:4436`).
Per element: vertex = center-in-frustum; edge = both endpoints (enclose) or segment-clip (touch);
face = all corners (enclose) or any (touch). **Enclose vs touch** toggle (Blender box vs drag).
Marquee is **x-ray by default** for sub-objects (predictable, avoids per-element occlusion rays);
optional occlude mode depth-tests element centers against the element BVH. Polygroup tier: a
polygroup qualifies if *any* of its faces qualify (default) or *all* (strict toggle).
Reference: Blender `view3d_select.cc` box-select; Cyclops `math_util.calc_frustum_camera_rect`.

### E. Performance — many blocks × elements, highlight regen (secondary)

Highlight meshes are **persistent per document**, updated **per dirty block only** (a coarse
"block has selection" bit skips untouched blocks entirely on rebuild). All selected faces batch into
one translucent surface; vertex handles are one MultiMesh (per-instance transform, screen-scaled in
shader); edges one line surface. Element BVHs are lazy + dirty-rebuilt (mirrors the gizmo BVH's
`optimize_incremental(1)` incremental cost model). Pick cost is broad-phase-bounded: the gizmo BVH
prunes to a handful of blocks before any per-element work.

---

## 4. Headless test ideas (`one-more-house/tools/checks/`)

Kernel is script-exposed (PLAN §Kernel), so all of picking is driveable without the editor by
feeding a fixed camera basis + synthetic screen coords.

- **`selection_pick_check.gd`** — build a known box and a known n-gon prism; with a fixed camera,
  cast rays at precomputed screen points; assert returned `(face_id, edge, vert)` per mode and tier.
  Include a grazing-angle and a coplanar-trim case (depth bias).
- **`selection_adjacency_check.gd`** — on a quad grid: assert `edge→faces`, `vert→edges`; assert an
  `L` loop returns the full ring of edges and terminates at the grid boundary; assert an `X` ring
  returns the parallel band; assert both **stop at an inserted n-gon** and at a valence-3 vertex.
- **`selection_stale_check.gd`** (PLAN risk 2 fuzz) — select a random element set, run a randomized
  operator sequence (extrude/inset/weld/delete) with interleaved undo/redo; after each step assert
  every stored handle either resolves to a live element or was dropped by revalidation, and that
  highlight element count == resolved selection count. Zero dereferences of dead slots.
- **`selection_marquee_check.gd`** — frustum from a known rect; assert the enclosed vs touched sets
  on a grid; assert x-ray vs occlude difference on two stacked coplanar faces.
- **`selection_cycle_check.gd`** — two overlapping faces under one cursor point; assert Spacebar
  cycles front→back→front deterministically and that plain-click on a new cell resets the cycle.
- **`selection_flood_check.gd`** — double-click flood fill returns the contiguous coplanar face
  patch and stops at a normal-angle break; `CapsLock`+double-click returns all faces on the infinite
  plane regardless of contiguity.

All comparisons seed any random tie-break (cycle order, flood ordering) so checks are deterministic —
same discipline as the hotspot fitter's `Disallow Random` seeded mode (PLAN §3).
