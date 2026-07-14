# Tool spec: Extrude family (Face Extrude, Push/Pull, Boundary-Edge Extrude, Inset, Outset)

**Status:** planning. **Prereq reading:** `level-editor-planning/PLAN.md` (esp. D2/D3 kernel decisions,
§1 KERNEL layer, §4 phase LE1). This spec covers the ops LE1 lists as "face extrude (`Shift`-drag) +
push/pull (`Shift+Ctrl`); boundary-edge extrude" plus LE4's Inset/Outset, pulled forward here because
all five share one foundation (region-rim detection, adjacency, winding) and should be designed as one
family rather than five one-off ops.

Research base: Blender BMesh operator source (`bmo_extrude.c`, `bmo_inset.c` — edge-tag boundary
counting, per-vertex angle-bisector inset with "even" toggle, `mat_nr`/customdata copy-from-source
on new faces); geometry3Sharp / gradientspace `MeshExtrudeFaces` + `MeshRegionBoundaryLoops`
(DMesh3-style indexed mesh, closest kernel-shape analog to ours); CMU 15-462 / Cardinal3D `meshedit`
assignment (halfedge extrude, boundary-halfedge handling); ryg (Fabian Giesen)'s half-edge blog posts
(imaginary/null boundary face framing); Cyclops `MeshVectorData` (mined per D2 — its columnar
`face_data`/`face_vertex_data` dictionaries are the direct ancestor of `LevelMeshData`'s schema, but
Cyclops itself never implements extrude — its convex-hull-from-planes model can't add topology, which
is exactly why D2 rejected it as the kernel base).

---

## 1. End-to-end op specs

Shared vocabulary: a **rim edge** is an edge of the selection whose adjacency, per the rule in §2,
identifies it as the outer boundary of the selected region. A **wall/skirt face** is new geometry
created along a rim edge. All ops below are methods on `LevelMesh`, taking a selection + parameters,
returning a diff (or a rejection).

### 1.1 Face Region Extrude (`Shift`-drag on selected faces)

- **Selection in:** one or more selected faces (polygroup tier), any mix of orientation, may share
  interior edges (multi-face region) or touch existing mesh boundaries.
- **Topology created:**
  1. Detect rim edges (§2).
  2. Duplicate every vertex referenced by a selected face — one new vertex per old vertex,
     shared across all selected faces that reference it (not per-face).
  3. Re-point each selected face's loop (`face_vertex_indices` span) at the new duplicated
     vertices — this is the moving "cap."
  4. For each rim edge `(v0, v1)` as it appears in its **owning** selected face's loop order,
     emit one new quad `(v0, v1, v1', v0')` where `v0'/v1'` are the duplicates. Original rim
     vertices stay put, becoming the stationary "waist" ring, still connected to whatever
     unselected geometry they bordered before.
  5. Drag displacement is applied to the duplicated cap vertices only, along the averaged
     normal of the selected region (single face normal in the single-face case).
  6. Interior edges (both adjacent faces selected) get **no** wall — this is the "rim only"
     requirement from the task brief, guaranteed by the §2 rule rather than by post-hoc pruning.
- **Attribute inheritance:**
  - Cap faces: unchanged `material_index` / `uv_transform` / `polygroup` / `smoothing` /
    `texture_lock`. This is the payoff of storing `uv_transform` as a face-local `Transform2D`
    (per PLAN.md §1) rather than baked vertex UVs — translating a plane along its own normal
    doesn't invalidate the projection, so texture lock "just works" for the cap with zero
    special-case code.
  - Wall faces: `material_index`/`smoothing`/`texture_lock` copied from the **single** selected
    face that owns that rim edge (always well-defined, §2). `uv_transform` is **not** copied
    verbatim — the wall's plane differs from the source face's plane — it is freshly computed
    via the kernel's standard auto-planar-projection for new faces (same code path new faces
    get anywhere else; hotspot-fit-on-creation is deferred to LE3). Polygroup: a **new** id,
    not merged into the source face's polygroup (Blender's extrude does not fold new faces into
    the source face-map either — keeps polygroup-tier selection predictable).
- **Diff out:** append rows to `vertex` arrays (new positions), append rows to `face`/
  `face_vertex` columnar arrays for the new walls, rewrite the `face_vertex_indices` span for
  each cap face (old vertex ids → new ids) — everything else byte-identical. Undo frees the new
  vertex/face ids back to the free-list and restores the cap's old index span.

### 1.2 Push/Pull (`Shift+Ctrl`-drag on selected faces)

- **Selection in:** one or more selected faces.
- **Topology created:** **none.** Each vertex referenced by a selected face moves along its own
  vertex normal (angle-weighted average of the *selected* faces touching it). If that vertex
  also belongs to an unselected neighbor face, the neighbor's shared edge moves with it — the
  neighbor face itself is not otherwise touched. This is the direct boundary-rep equivalent of
  Hammer's brush-plane push (which re-clips neighboring brush faces against the moved plane);
  here there are no infinite planes to re-clip, so the shared vertex simply translates and the
  neighbor's shape changes exactly as much as sharing that vertex implies.
- **Attribute inheritance:** none needed — no new faces/verts/loops. `uv_transform` of the pushed
  face is unchanged (same planar footprint, only depth changed). Loops on distorted unselected
  neighbors are left as-is, same behavior a modeler gets in Blender without correcting UVs.
- **Diff out:** vertex-position span diff only. Face/loop arrays are bit-identical before/after —
  this is the assertion the headless test in §4 checks, to guard against push/pull accidentally
  routing through the extrude code path.

### 1.3 Boundary-Edge Extrude (`Shift`-drag on selected boundary edges of an open mesh)

- **Selection in:** a connected chain (or closed loop) of **boundary edges** — each touches
  exactly one face. Mixing in any interior/manifold edge is rejected outright; that's a
  different tool (edge slide/loop-insert, LE4).
- **Topology created:** walk the chain in the order it appears in its owning faces' loops
  (always available directly from `face_vertex_indices`, no half-edge structure needed — see
  §3.5). Duplicate each chain vertex once, reusing the previous edge's duplicate for the shared
  vertex so consecutive quads weld. Emit one new quad per original edge, `(v0, v1, v1', v0')`,
  winding taken from the edge's single owning face (§3.5 — the "synthetic boundary face"
  argument). Original boundary vertices stay put as the new hinge; only the duplicates move
  with the drag.
- **Attribute inheritance:** unambiguous — every boundary edge has exactly one owning face, so
  material/smoothing/texture-lock copy from it directly (simpler than the region case, no
  rim-ownership computation needed). `uv_transform` re-derived via auto-planar-projection since
  the new quad's plane is generally a fold relative to the source face's plane.
- **Diff out:** same append/rewrite shape as §1.1's walls; no cap-reindex step since there's no
  cap (the chain has no interior to keep).

### 1.4 Inset (`I`, faces)

- **Selection in:** selected faces; `I` again toggles **Individual** vs **Region** mode
  (matches Blender's inset modal); `Alt` toggles **Even** offset (§3.4).
- **Topology created — Region mode:** identical rim detection to §1.1, but the new ring is
  offset **inward and coplanar**, not displaced along the normal. Each rim vertex is duplicated
  and moved inward along its per-vertex bisector (§3.4's corner math); a flat quad skirt
  `(v0, v1, v1', v0')` connects old to new per rim edge, and each selected face's loop is
  re-pointed at the inset (shrunk) vertex copies exactly like the cap step in §1.1. Interior
  edges between two selected faces get no skirt — same rim rule as extrude, so a donut/annulus
  selection produces two independent skirt loops (outer + inner hole boundary).
- **Individual mode:** every selected face is treated as its own 1-face region *even where two
  selected faces share an edge* — that shared edge gets two independent skirts and is
  duplicated/split at that point. This is the actual topology-creating difference between the
  two modes, not just a cosmetic offset difference.
- **Attribute inheritance:** the shrunk face copy keeps its **entire** original `face_data` row
  unchanged (material/uv_transform/polygroup/smoothing) — conceptually it's still "the same
  face," just smaller, matching Blender's inset. Skirt faces follow the same rule as extrude
  walls (§1.1): inherit from the single owning face per rim edge.
- **Diff out:** same shape as §1.1.

### 1.5 Outset (`O`, faces)

Same operator as §1.4 with the offset distance negated (skirt points outward, face boundary
grows instead of shrinks). No separate implementation. Because growing outward has no guarantee
against overrunning neighboring unselected geometry, Outset only guarantees local silhouette
validity (§3.4's winding/self-intersection check) — it does not attempt neighbor-collision
detection, matching Blender's Inset-with-negative-thickness, which has the same limitation.

---

## 2. Foundation services

- **Adjacency index** (built lazily from the columnar arrays, cached on `LevelMesh`, invalidated
  transactionally on every diff apply/undo — same cache-invalidation discipline PLAN.md's risk
  #2 already requires for bake/selection caches): edge→face(s) keyed by unordered vertex pair
  (hashmap, O(Σ face size) to build); vertex→faces and vertex→edges (needed for push/pull normal
  averaging, flood-fill, picking); face→edges in **loop order** (needed to walk a rim/boundary
  chain as a connected pass instead of an unordered edge soup, so consecutive wall/skirt quads
  reuse the correct shared-vertex duplicate rather than re-duplicating per edge).
- **Region-boundary (rim) detection rule**, unifying all of §1.1/1.3/1.4: for selection `S`, for
  each face `f ∈ S`, for each edge `e` in `f`'s loop, let `A` = faces adjacent to `e`,
  `A_sel = A ∩ S`.
  - `|A_sel| == 2` → interior edge, no wall/skirt.
  - `|A_sel| == 1` → rim edge, **owned by `f`** (covers both "closed region on a manifold mesh"
    and "region touching an existing hole/mesh boundary," since `A_sel == A` there too — same
    branch, no special case). This single rule also correctly produces two loops for a
    donut/annulus selection.
  - Otherwise (`|A| > 2` and more than one of those faces is selected — a non-manifold edge with
    an ambiguous split) → **reject the whole op**, mesh unchanged. No guessing.
- **Winding rule:** a wall/skirt's winding is derived **purely locally** from its one owning
  face's loop order — `(v0, v1)` as that edge appears in `f`'s own loop, never from a
  region-averaged or global normal. Because every rim edge has exactly one owner by
  construction, this is well-defined even when the selection mixes a correctly-wound face with
  an accidentally flipped one — each wall is still locally consistent with its own owner, and no
  aggregate orientation vote is ever needed. This is the crux of difficulty #1 below.

---

## 3. Core difficulties

### 3.1 Region-rim detection and correct winding/normals for new wall faces (mixed-orientation selections, non-manifold rims)

**Chosen solution:** the per-edge "selected-adjacent-face count" rule + purely local per-face
winding derivation from §2, with an explicit reject branch for the >2-adjacent-faces case.

**Algorithm sketch:**
1. Build (or reuse cached) edge→faces adjacency.
2. For every face in the selection, walk its loop; classify each edge via `|A_sel|` as above.
3. Walk rim edges per connected boundary loop (via face→edges ordering) rather than an
   unordered pass, so consecutive quads share duplicate vertices correctly.
4. Emit one wall per rim edge, winding from step 2's owning face; reject the whole op (no
   partial mutation) if any edge falls in the ambiguous non-manifold case.

**References:** Blender `bmo_extrude.c`'s `bmo_extrude_face_region_exec`, which tags boundary
edges of the selected face region by adjacent-selected-face count — the same trick under a
different name; geometry3Sharp's `MeshExtrudeFaces` + `MeshRegionBoundaryLoops` (gradientspace),
which computes region boundary loops explicitly before extrude and documents the multi-loop
(donut) case — the closest published analog to an indexed-mesh (DMesh3-style) kernel like ours.

### 3.2 Repeated-extrude UX: gesture threshold, and push/pull vs extrude distinction

**Chosen solution:** two independent, orthogonal decisions instead of one fuzzy heuristic.
*Which operator* is decided by the modifier chord **latched at mouse-down** (`Shift` = extrude,
`Shift+Ctrl` = push/pull) — no inference, no ambiguity. *Whether anything commits at all* is
decided by a plain screen-space drag-distance epsilon (the same kind of threshold the fork's
existing `Node3DEditorView`-style gizmo drags already use) applied uniformly regardless of which
operator was latched; below it, the gesture is treated as a plain reselect-click — no diff, no
undo entry (consistent with PLAN.md's "selection changes are never undo steps"). Stacking
(re-extruding an already-extruded cap) needs no special-cased "was this just extruded" state at
all: the tool returns to idle on mouse-up, and the next mouse-down re-evaluates the modifier
chord fresh against the **current** selection (now the moved cap) — each op is a pure
`(mesh, selection, params) -> (mesh', new_selection)` transform, so chaining is just running the
next op on the previous op's output selection.

**References:** Blender's Extrude Region / Inset are independent, `OPTYPE_REGISTER|OPTYPE_UNDO`
modal operators — "extrude again" in the manual is documented as literally re-invoking the
operator on the resulting selection, not a stateful continuation; the CMU 15-462 / Cardinal3D
`meshedit` assignment frames every local operator as exactly this pure-function shape, which is
what makes chaining ops on their own output the natural (and only) composition model rather than
something requiring bespoke state.

### 3.3 Attribute inheritance rules for new geometry (material/UV/polygroup on walls)

**Chosen solution:** new wall/skirt faces always inherit `material_index`/`smoothing`/
`texture_lock` from the **single** rim-owning face identified in §2 — never a global "active
material" default, never an average, since §2 guarantees exactly one candidate always exists.
`uv_transform` is *not* copied byte-for-byte (the wall's plane differs from the owner's plane) —
it's recomputed via the same auto-planar-projection new faces already need elsewhere in the
kernel. Polygroup: walls/skirts get a **fresh** polygroup id rather than folding into the
source's, so polygroup-tier selection stays predictable after the op. The one exception is
Inset's shrunk face copy (§1.4), which keeps its *entire* original row unchanged since it is
still conceptually the same face.

**References:** Blender BMesh's `bmo_inset.c` (`inset_region_exec`) explicitly copies `mat_nr`
from the originating face onto each new boundary face it creates (`BM_elem_attrs_copy`-style),
which is the direct precedent for "inherit from the one well-defined source, never a global
default"; Source-engine/Hammer community documentation of vertex-manipulation face extrude
(brush side faces initialize from whichever adjacent plane exists) motivates the same
one-well-defined-source principle even though Source's brush model differs structurally from
ours (no live planes to inherit from — hence recomputing rather than copying the UV transform).

### 3.4 Inset/outset corner math on n-gons (non-convex faces, spikes at acute corners, even offset)

**Chosen solution:** classic 2D polygon-offset per-vertex miter join, computed in the face's own
best-fit-plane local basis (project n-gon to 2D, offset, project back), with a miter-limit clamp
for acute corners.

**Algorithm sketch:**
1. Project the face's loop into its own 2D plane basis.
2. For vertex `v` with incoming/outgoing edges, compute the unit bisector of their inward
   normals; **Even** offset scales the bisector step by `d / sin(θ/2)` (`θ` = interior angle at
   `v`, signed using the face's own winding so reflex/concave corners offset correctly) so all
   edges move inward by the same perpendicular distance `d` regardless of corner angle — without
   this compensation, acute corners get visibly under-inset walls (the naive "just move along
   bisector by `d`" case, which Blender exposes as the non-Even alternative).
3. **Acute-corner spike guard:** `1/sin(θ/2)` diverges as `θ→0`; clamp the miter length at a
   configurable multiple of `d` (the standard miter-limit technique from 2D vector-graphics
   stroke joins / straight-skeleton implementations). For v1, exceeding the limit **rejects**
   the op with a message rather than emitting a degenerate/overshooting vertex (bevel-join
   fallback, which flattens the tip with an extra vertex instead of rejecting, is a plausible
   later polish pass but adds a corner case not needed for the slice).
4. **Self-intersection / winding-flip guard:** after computing all offset corners, check each
   corner's local turn direction still matches the pre-offset winding (cheap per-corner cross
   product against the face normal); reject the whole op if any corner would flip — deliberately
   not attempting general robust polygon clipping (Vatti/Weiler-Atherton-class algorithms), which
   is overkill for a level-editor face op and is exactly the class of problem D4 already
   delegates to vendored Manifold if it's ever truly needed.

**References:** Blender's `bmo_inset.c` implements precisely this — per-vertex edge-pair angle
bisector offset with an explicit "even offset" boolean option, and Outset is the same code path
with negated thickness; miter-limit-then-fallback is the standard SVG/vector-stroke tessellation
technique (also documented in CGAL's straight-skeleton material) for handling acute joins without
degenerate spikes.

### 3.5 Boundary-edge extrude on open meshes (consistent orientation without a face normal)

**Chosen solution:** synthetic/virtual boundary face framing — there is no "outside" face to take
a normal from, but every boundary edge has **exactly one** real adjacent face, so borrow that
face's loop-winding convention directly instead of needing an aggregate normal at all. No virtual
face object is actually materialized in the kernel; it's purely the mental model that justifies
why "use the one real neighbor's loop direction" is always sufficient and always consistent —
even across a chain that visits several different owning faces (e.g. an L-shaped open sheet's
boundary), because each segment's winding is derived locally from its own owner exactly as in
§3.1, never from a chain-wide vote.

**Algorithm sketch:** walk the selected boundary-edge chain in the order each edge appears within
its one owning face's loop (directly available from `face_vertex_indices` — no half-edge
structure needed, consistent with D3's indexed-mesh-not-pointer-half-edge decision); duplicate
each chain vertex once, reusing the prior edge's duplicate for a shared vertex so the strip welds
continuously; emit one quad per edge with winding from its owner.

**References:** ryg (Fabian Giesen)'s half-edge mesh blog posts, which frame a mesh boundary as
though bounded by an imaginary null-material face specifically to keep half-edge orientation
invariants uniform without special-casing; the CMU 15-462 / Cardinal3D `meshedit` assignment's
treatment of boundary halfedges (`Halfedge::is_boundary()` paired with an "imaginary" outside
halfedge), which is the same technique applied to make `extrude`/`bevel` code branch-free across
interior vs. boundary edges.

---

## 4. Headless test ideas (`tools/checks/` pattern, e.g. `level_kernel_extrude_check.gd`)

1. **Donut-region extrude:** flat grid mesh, select an annulus of faces around one unselected
   central face; run Face Region Extrude; assert two rim loops are detected (outer + inner hole
   boundary), wall count == combined loop length, and every edge with exactly 2 selected
   neighbors (the ring's own interior edges) produced **no** wall.
2. **Euler characteristic invariance:** on a closed manifold (cube, χ = V−E+F = 2), extrude one
   face and assert χ unchanged (still topologically a sphere); repeat for Inset (χ-preserving)
   and for Boundary-Edge Extrude on an open disk (χ = 1, unchanged — still simply connected).
3. **Mixed-orientation selection:** construct a 2-face region where one face's winding was
   manually flipped (simulated authoring mistake); run extrude; assert each wall's winding is
   locally consistent with **its own** owning face (per-face dot-product/cross-product check)
   rather than asserting one global answer — this is the direct regression guard for §3.1.
4. **Non-manifold rejection:** an edge shared by 3 faces, 2 selected; assert the op returns a
   rejection and V/E/F counts (and every array) are byte-identical before/after — "reject, don't
   corrupt" must hold even under a partial-mutation attempt.
5. **Gesture stacking:** call the extrude operator twice in sequence on the evolving selection
   (simulating two separate Shift-drag mouse-down/up gestures); assert two distinct wall rings
   exist, each is its own undo step, and undoing once removes only the second ring.
6. **Push/pull topology-inert:** select N faces, push/pull by a nonzero offset; assert
   `face_vertex_count`/`face_vertex_indices`/edge arrays are bit-identical before/after and only
   vertex-position spans changed — guards against push/pull silently reusing the extrude path.
7. **Inset even-offset / acute-corner guard:** build a face with one very acute corner; request
   an inset distance that would flip winding there without the miter clamp; assert the op
   rejects (v1 behavior) and that no emitted face has inverted winding relative to its stored
   normal.
8. **Attribute inheritance:** single face with known `material_index`/`polygroup`/`uv_transform`;
   extrude; assert the cap keeps all three unchanged, every new wall inherits the same
   `material_index`, no wall's polygroup equals the source's (fresh id), and `uv_transform` on
   the walls is recomputed rather than a byte-copy of the source's.
9. **Undo round-trip:** snapshot `LevelMeshData` before an op, run it, undo, assert field-by-field
   equality with the pre-op snapshot including free-list state (redo must not leak or duplicate
   ids).
