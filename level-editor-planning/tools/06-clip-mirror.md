# Tool 06 — Clip (Shift+X) & Mirror (Shift+Z)

**Status:** planning. **Belongs to:** phase **LE5 — Booleans** in `PLAN.md`. **Prereq reading:**
`PLAN.md` §1 (kernel architecture), §5 risk 1 (boolean robustness). **Layer:** the interactive
plane widget + preview live in `LevelEditorView` (VIEW STATE); the split/mirror themselves are
`LevelMesh` operators in `modules/level_kernel` (KERNEL, no editor deps, GDScript-scriptable).

**Why this doc exists / prior-art warning.** Scythe shipped clip/mirror ~1 year in (v0.6) and it
*crashed on triangle vertex/edge selections* — the single worst reliability event in its history.
That crash is a geometry-robustness failure at the plane/mesh boundary, not a UX bug. This plan
scopes both ops to **faces / polygroups / objects first** (never raw tri-vert/edge selections),
and every operator **rejects illegal input rather than corrupting the mesh** (`PLAN.md` D-decisions).
The Cyclops `cut_with_plane()` reference (`math/convex_volume.gd:1156`) is *convex-only* — it appends
the cut plane to the brush's plane set and recomputes a convex hull. It cannot split a concave block,
an n-gon with a hole, or produce a proper cap. We need a real half-edge-free indexed plane-split.

---

## 1. End-to-end spec

### 1a. Plane definition UX (TrenchBroom / Hammer 3-point model)
- **Entry:** `Shift+X` (clip) / `Shift+Z` (mirror) enters a modal tool. `Enter`=apply, `Esc`=cancel
  (universal modal grammar, `PLAN.md` §2). Requires a non-empty selection at face/polygroup/object
  tier; **rejects with a status-bar message** if the active tier is vertex/edge (Scythe's crash class,
  refused up front).
- **Two definition modes, one tool:**
  1. **3-point plane** — click up to 3 points, each snapped to grid / vertex / edge-midpoint / face
     (the picking BVH from `PLAN.md` §Picking supplies the snap candidate). 1 point + view = a
     view-aligned plane through the point; 2 points = a plane containing the line and perpendicular to
     the current viewport; 3 points = the fully-specified plane. Matches TrenchBroom's clip tool.
  2. **Drag-a-line** — LMB-drag in a viewport draws a screen-space line; the plane is that line swept
     along the camera forward (Hammer 2D-viewport clip). Only offered in an axis/ortho view to keep it
     unambiguous.
- **Handles after definition:** the plane shows a translucent quad + normal arrow + an in-plane
  rotation ring; points remain draggable to re-fit. Plane normal side = "front". `Ctrl` flips normal.
- **Keep mode (clip only):** cycles **front / back / both** with `Tab` (Hammer/TrenchBroom keep-cycle).
  Default = keep-front. `both` = split in place into two blocks sharing the cap.
- **Mirror options:** `weld seam` toggle (default ON), `keep original` toggle (mirror = copy+reflect vs
  reflect-in-place). Mirror has no keep-front/back — it always duplicates+reflects the selection.

### 1b. Preview (live, no kernel commit)
- Live overlay = the **cutting plane quad** + the **intersection polyline** (plane ∩ selected-mesh
  edges — cheap, no retopology, no capping). Both halves are hinted by tinting front faces vs back
  faces of the *unmodified* mesh against the plane sign; `both` mode adds a small explode offset so the
  two halves visually separate. Mirror preview = a ghost (wireframe + 40% alpha) reflected instance of
  the selection, regenerated only when the plane moves. **No full split runs during drag** — see §4.4.

### 1c. Kernel plane-split op (`LevelMesh.split_with_plane`)
Signature (scriptable): `split_with_plane(plane: Plane, face_ids: PackedInt32Array, keep: int,
cap: bool, cap_material: int) -> LevelMeshDiff`. Steps:
1. **Classify** every vertex referenced by the operand faces as `ABOVE / ON / BELOW` (thick-plane, §4.1).
2. **Split faces** the plane straddles: walk each face loop; where an edge crosses ABOVE↔BELOW, insert
   a new split vertex at the exact edge/plane intersection (interpolating loop attrs); ON-vertices are
   reused directly (no new vertex). Emit the front sub-loop and back sub-loop as new faces, each
   inheriting the parent face's `material_index`, `uv_transform`, polygroup, smoothing, texture-lock.
   Coplanar faces (all-ON) are assigned wholly to front or back by their normal vs plane normal.
3. **Collect cut edges** — every new edge lying on the plane (ON→ON) is a boundary edge of the cut.
4. **Cap** (if `cap`): build closed loops from cut edges and triangulate/n-gon them (§4.2).
5. **Partition** faces into front / back connected sets; honor `keep` (drop the unwanted set, or keep
   both as two polygroups / two `LevelBlock`s for `both`).
6. **Validate → reject:** if any produced face is degenerate (area < ε²), non-planar beyond tolerance,
   or the result is non-manifold where the input was manifold, **abort and return an empty diff** with a
   reason code. Never commit a partial edit.

### 1d. Mirror op (`LevelMesh.mirror_across_plane`)
`mirror_across_plane(plane, face_ids, weld, keep_original) -> LevelMeshDiff`. Duplicate operand
topology; reflect each vertex `p' = p - 2*plane.distance_to(p)*plane.normal`; **reverse every face loop**
(reflection inverts winding — negative-determinant basis, cf. `convex_volume.gd:1296` `reverse()`);
recompute normals; **mirror UVs** (§4.3); if `weld`, merge mirrored verts that land within ε of an
original vertex *on the seam plane* into shared vertices and drop the now-coincident seam edges. New
faces get fresh polygroup ids (§4.3 duplication policy). If `!keep_original`, the original operand is
also removed (pure reflect).

### 1e. Attribute handling summary
| Attribute | Split | Mirror |
|---|---|---|
| `material_index` (per face) | inherited by both sub-faces; cap gets `cap_material` (active material default) | copied |
| `uv_transform : Transform2D` (per face) | inherited unchanged (texture-lock preserves world UVs across the cut) | reflected: negate one basis axis + adjust origin (§4.3) |
| loop `uv/color/normal` | split verts interpolate by edge parameter `t`; cap loops get planar-projected UV from `cap_material` | loop uv mirrored with the face; normal reflected then loop-reversed |
| polygroup id | sub-faces keep parent id | new ids = `max_id + 1 + original_id` offset |
| smoothing / texture-lock flags | inherited | copied |

### 1f. Diff / undo
Both ops emit a `LevelMeshDiff` = changed spans of the columnar arrays (removed face/vertex ids →
free-list, added ids, mutated attribute spans) exactly as `PLAN.md` §Undo prescribes. The **kernel stays
undo-agnostic** — it only produces the diff; the document's undo history applies/reverts it, and derived
state (bake, BVH, selection caches, overlays) invalidates transactionally against that diff. Selection
change from the op is *not* itself an undo step (TrenchBroom lesson).

---

## 2. Foundation services (build these once; clip, mirror, bevel, and Manifold booleans all reuse them)

- **`GeomPredicates`** — robust orientation & sign: `orient3d(a,b,c,d)`, `signed_dist(plane,p)`,
  `classify(plane, p, eps) -> {ABOVE,ON,BELOW}`. Fast float path with an exact (adaptive) fallback used
  only as a sign tie-breaker (§4.1). Header-only, kernel-internal, headless-testable in isolation.
- **`PolyTriangulator`** — polygon-with-holes triangulation in a 2D plane basis: ear-clipping with
  hole-bridging (Eberly/FIST), returns triangle indices; also an `is_convex_simple()` fast path that
  emits a single n-gon face when there are no holes and the loop is convex. Reused by every cap and by
  the baker's face triangulation.
- **`PlaneFit`** — best-fit plane from 1/2/3 snapped points + camera basis, and plane↔2D-basis
  transforms (`to_plane_uv`, `from_plane_uv`) so all in-plane work (loop nesting, triangulation, cap UV)
  happens in stable 2D.
- **`LoopExtractor`** — given a set of undirected cut edges, assemble oriented closed loops and classify
  nesting (outer vs hole) by signed area + point-in-polygon in the plane basis. Reused by mirror-seam
  detection and future bridge/fill tools.
- **Plane-widget UX kit** (`LevelEditorView`) — the draggable 3-point/line gizmo, snap resolver
  (grid ∪ vertex ∪ edge-mid ∪ face), keep-mode cycler, overlay-mesh builders for plane quad +
  intersection polyline + reflected ghost. Detached-RID create→reconcile→free lifecycle like the grid
  decoration (`PLAN.md` VIEW STATE).
- **`TransactionScope`** — begin/rollback/commit around a kernel op so preview and reject-don't-crash
  both get a clean "try it, throw it away" path (§4.4).

---

## 3. Core difficulties — one chosen solution each

### Difficulty 1 — Robust plane–mesh splitting on n-gons (THE crash source)
Vertices exactly on the plane, edges grazing it, and fully-coplanar faces are where naive float
classification produces zero-area slivers and duplicate/near-duplicate verts that the cap and baker
later choke on — precisely Scythe's v0.6 failure.

**Chosen solution: thick-plane snap-to-plane classification with a grid-tied epsilon, exact predicate as
a sign tie-breaker only.** Classify with a signed-distance band: `|d| ≤ ε → ON` (and *snap the vertex
exactly onto the plane*), else sign of `d`. `ε` is derived from the editor grid quantum
(e.g. `grid/1024`), not a hardcoded 1e-6, because block coordinates are grid-quantized — snapping to the
plane there yields a genuinely watertight weld instead of a sliver. Only when a vertex is *outside* the
band but its float sign is numerically ambiguous do we consult an adaptive-exact `orient3d` to pin the
sign deterministically. We deliberately do **not** adopt full exact-predicate topology (Shewchuk) as the
primary path: exact predicates give a *correct* classification but still leave you with real sub-ε
slivers that downstream capping/baking must special-case; the thick-plane snap *removes* those slivers by
construction, which is what a grid editor actually wants.

*Algorithm sketch:*
```
for v in operand_verts: cls[v] = classify(plane, pos[v], eps); if cls[v]==ON: pos[v] = project(plane,pos[v])
for face in operand_faces:
  if all loop verts ON: assign whole face to front/back by dot(face.normal, plane.normal); continue
  front=[]; back=[]
  for (a,b) in face_edges:
    push a to its side (ON → push to BOTH sub-loops)
    if cls[a]!=cls[b] and neither is ON:
      t = signed_dist(a)/(signed_dist(a)-signed_dist(b))     # exact by construction, a,b straddle
      s = get_or_make_split_vertex(a,b,t)                    # dedup per edge → shared vertex
      push s to BOTH sub-loops
  reject if front or back has <3 verts after dedup; else emit both sub-faces (inherit attrs)
```
Dedup split vertices per undirected edge so the two faces sharing an edge produce the *same* split
vertex → watertight. **References:** Ericson, *Real-Time Collision Detection* §5.3.5 (thick planes) &
§8.3 (splitting polygons, keeping shared verts); geometry3Sharp `MeshPlaneCut` (ON-vertex reuse +
per-edge crossing dedup); Shewchuk, *Adaptive Precision Floating-Point Arithmetic and Fast Robust
Geometric Predicates* (the exact tie-breaker).

### Difficulty 2 — Capping the cut cross-section (loops, possibly nested / hole-in-face)
A single planar cut through a concave block or a block with a window can produce several disjoint
outer loops, and an outer loop may contain hole loops. The cap must be a polygon-with-holes, correctly
wound, with sane material/UV.

**Chosen solution: extract oriented loops in the plane's 2D basis, resolve nesting by containment, then
ear-clip-with-hole-bridging; emit a single n-gon cap when a loop is simple & convex, triangles otherwise.**
The cap plane and the split plane are identical, so cap UVs are a clean planar projection.

*Algorithm sketch:*
```
edges = all ON→ON edges created by the split
loops = LoopExtractor.assemble(edges)                 # oriented via consistent edge direction
project each loop to 2D via PlaneFit.to_plane_uv
for L in loops: signed_area(L) > 0 ? outers.append(L) : holes.append(L)   # CCW outer, CW hole
for hole H: assign to the outer whose polygon contains H.any_point (point-in-poly, innermost wins)
for outer O with its holes:
  if O convex and no holes: emit one cap face (n-gon), winding = plane.normal
  else: tris = PolyTriangulator.earclip_with_holes(O, holes); emit cap faces from tris
cap face: material_index = cap_material; uv = from_plane_uv scaled by texel density; normal = plane.normal
for `both` keep: emit the cap twice with opposite winding (one per half) so both blocks stay closed
```
Nesting via innermost-containing-outer handles concentric rings (frame-in-frame). **References:**
geometry3Sharp `MeshPlaneCut`/`MeshBoundaryLoops` (loop assembly + fill); Eberly, *Triangulation by Ear
Clipping* (hole-bridging: cut a bridge edge from each hole's rightmost vertex to the outer loop);
Blender `mesh_bisect` fill option (its "Fill" produces exactly this planar cap); Held, *FIST: Fast
Industrial-Strength Triangulation* (robust ear clipping used in production).

### Difficulty 3 — Mirror seam welding + attribute mirroring
Reflection inverts winding, mirrors texture handedness, and duplicates polygroups; welding must fuse the
seam without leaving a double-sided crack or T-junctions.

**Chosen solution: reflect + reverse-loops + mirror-UV, then weld seam verts by exact grid-snapped
coincidence (not radius search).** Because seam verts sit *on* the mirror plane and are grid-quantized,
a mirrored seam vertex lands on the *same* snapped coordinate as its original — weld by hashing snapped
positions, not by an O(n²) distance search (avoids the accidental over-weld that plagues radius welds).

*Algorithm sketch:*
```
for v in operand: p' = reflect(plane, pos[v]); if |signed_dist(plane,v)| ≤ eps: p' = project(plane, v)  # seam
copy faces; reverse each loop; normal' = reflect_dir(plane, normal)
uv mirror: for a face, replace uv_transform T with M∘T where M flips the plane-aligned axis
           (Transform2D basis: negate one column, offset compensates) → texture reads mirror-correct
if weld:
  key(v) = snap_to_grid(pos)                         # hash
  seam originals register their key; mirrored seam verts that hit an existing key are REMAPPED to it
  drop edges/faces that become zero-area after remap; assert manifold or REJECT
polygroup ids: new_id = original_id + (max_polygroup_id + 1)   # duplicate, never reuse
```
Winding-reversal is the same operation Cyclops already does on negative-determinant transforms
(`convex_volume.gd:1296-1298`, `FaceInfo.reverse()`), so we mirror that behavior deliberately.
**References:** Cyclops `ConvexVolume.transform()` negative-determinant `reverse()` path
(`math/convex_volume.gd:1296`); Ericson RTCD §12 / general note that an odd number of axis reflections
flips triangle winding and must be corrected; standard UV-handedness note (a reflected surface needs its
UV parameterization mirrored or the texture appears back-to-front).

### Difficulty 4 — Clip preview performance (live both-halves without committing kernel state)
Running the full split (classify → retopo → cap → repartition → bake) every mouse-move frame while
dragging the plane is too expensive and would thrash the undo/derived-state machinery.

**Chosen solution: cheap live preview (plane quad + intersection polyline + sign-tint), full split
deferred to commit through `TransactionScope` (try/rollback).** During drag we compute *only* plane∩edge
intersection points (a few dozen dot products) to draw the cut line, and tint existing faces by their
centroid's plane sign to hint front/back — **no topology is created**. Only on `Enter` do we run the real
`split_with_plane` inside a transaction; if the user is in `both`-preview and then cancels, we roll the
transaction back and nothing reaches the undo stack. This also gives reject-don't-crash for free: a split
that fails validation rolls back to a pristine mesh. This matches `PLAN.md`'s explicit non-goal "Live CSG
preview of pending booleans (cheap wireframe preview only)".

*Algorithm sketch:*
```
on_drag(plane):
  poly = []
  for e in selection_boundary_edges: if straddles(plane,e): poly.append(intersect(plane,e))
  overlay.set_line(poly); overlay.set_plane_quad(plane); tint_faces_by_sign(plane)   # no kernel call
on_commit(plane, keep, cap):
  tx = mesh.begin_transaction()
  diff = mesh.split_with_plane(plane, sel, keep, cap, active_material)
  if diff.is_empty(): tx.rollback(); status("clip rejected: <reason>"); return
  tx.commit(diff)   # → document undo history applies it, derived state invalidates against diff
```
**References:** geometry3Sharp change-tracking / `MeshEditor` transactional edits (build-then-keep-or-toss);
Ericson RTCD §8.3 (splitting is a pure function of plane+mesh, so it's trivially deferrable/replayable);
`PLAN.md` §Undo (kernel emits diffs, editor owns the stack) — the transaction is the editor-side wrapper.

---

## 4. Headless test ideas (`one-more-house/tools/checks/`, seeded-deterministic)

All kernel classes are GDScript-exposed (`PLAN.md`), so these run without the editor.

- **`clip_fuzz_check.gd` — plane fuzz against primitives.** For box / L-shape (concave) / ring
  (hole-in-face) / stairs blocks, generate N seeded random planes AND a curated adversarial set:
  plane exactly through a vertex, exactly along an edge, exactly coincident with a face (coplanar),
  grazing tangent, and just-inside-ε near-misses on both sides. Assert: op returns a diff or a *clean
  rejection* — never an exception, never a partial mutation. This is the direct regression guard for
  Scythe's v0.6 crash.
- **Volume conservation.** For `keep=both` on a *closed* block:
  `|vol(front) + vol(back) − vol(original)| < ε`. Use the signed-tetrahedron volume sum over triangulated
  faces (Cyclops already exposes `face_area_x2`/triangulation to build on). Catches cap orientation bugs
  (a mis-wound cap flips a volume sign) and dropped-sliver leaks.
- **Watertightness after cap.** After any split with `cap=true`, assert every edge of each resulting
  block is shared by exactly 2 faces (closed 2-manifold) and no boundary edges remain. A missing/holed
  cap shows up here immediately.
- **Split-vertex sharing.** Assert each on-plane split vertex is referenced by both adjacent sub-faces
  (dedup worked → no crack). Count new verts ≤ number of straddled edges.
- **Idempotent re-clip.** Clipping by a plane, then clipping the front half by the *same* plane, changes
  nothing (empty diff) — proves ON-classification stability across passes.
- **Mirror round-trip.** `mirror(keep_original=false)` twice across the same plane ≈ identity mesh
  (positions within ε, same face count after weld). `mirror` then check winding: every face normal of the
  reflected half points consistently outward (dot with outward test > 0).
- **Mirror weld correctness.** With `weld=on`, seam vertex count == original seam vertex count (no
  doubles); with `weld=off`, exactly doubled. Assert no over-weld (interior verts untouched).
- **UV mirror handedness.** After mirror, sample a face's UV winding sign — must be inverted relative to
  the source face (confirms `uv_transform` mirror, not just geometry).
- **Reject-tier guard.** Calling `split_with_plane` with a vertex/edge-tier selection returns a rejection
  code, asserting the Scythe-crash surface is closed at the API level, not just the UI.

---

## 5. Build order within LE5
1. Foundation services (§2) + `GeomPredicates`/`PolyTriangulator` unit checks.
2. `split_with_plane` faces-only, `keep=front`, **no cap** + `clip_fuzz_check` (rejection contract first).
3. Capping + watertightness/volume checks. 4. `keep=back/both`. 5. Plane widget + live preview (§4.4).
6. `mirror_across_plane` + weld + mirror checks. 7. Interactive smoke scenario (headless misses editor
bugs — HammerForge lesson): clip and mirror the greybox house, save, `launch_two.ps1`, walk it co-op.
Each step lands with a `DECISIONS.md` note game-side and a DIVERGENCE-LEDGER entry for any shared-file
touch (`PLAN.md` §4).
