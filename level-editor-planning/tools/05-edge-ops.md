# 05 — Edge / Topology Operator Set (Scythe-class)

**Scope:** the LE4 "modeling depth" cluster of `LevelMesh` operators that *add and rewire
topology*: Connect/Cut (`V`), edge-loop insert (`X`→`V`), Bevel (`B`), Bridge (`Shift+C`),
Merge Faces (`Backspace`), Weld Overlapping (`Shift+Ctrl+Alt+M`), Subdivide, Normals
(Soft `J` / Hard `H` / Dominant Face `K` / Invert `Shift+N`), Detach (`N`), Extract (`Alt+N`).

**Prereq reading:** `PLAN.md` (§1 kernel, D3 indexed-mesh, undo=diffs, reject-don't-corrupt).
This is the hardest operator cluster in the whole editor — these are the ops that can produce
non-manifold or self-intersecting garbage, so the two-tier BMesh discipline (atomic Euler ops →
composed operators, every composed op validates preconditions and **rejects**) is mandatory here,
not optional. All ops are pure `modules/level_kernel` C++, GDScript-exposed, headless-drivable.

---

## 0. Kernel model this cluster assumes

`LevelMesh` is an **indexed** structure (D3), not pointer half-edge, but it carries enough
adjacency to walk cycles in O(1):

- **Vertex**: `position`, free-list id, `disk` = unordered list of incident edge ids.
- **Edge**: ordered pair `(v0,v1)`, `radial` = list of incident face-loop ids (2 for manifold
  interior, 1 for boundary, 3+ = non-manifold and normally illegal here).
- **Face**: ordered loop-id ring (n-gon, CCW seen from front), `material_index`,
  `uv_transform:Transform2D`, `polygroup`, `smooth_flag`, `texture_lock`, `hotspot_ref`.
- **Loop** (face-vertex): `(face, vertex, edge)` + per-corner `uv`, `color`, `normal`.
  This is the BMesh "loop" and is where all per-corner attribute interpolation happens.

Adjacency (`disk`/`radial`) is *derived*: rebuilt from the columnar arrays on load and maintained
incrementally by atomic ops. This mirrors BMesh's disk/radial cycles (D3 rationale) while staying
serializable as Cyclops-style `MeshVectorData` columns (see `mesh_vector_data.gd`:
`edge_vertex_indices`, `edge_face_indices`, `face_vertex_count`, `face_vertex_indices`, plus
per-feature attribute dictionaries).

**Atomic Euler-ish ops** (each keeps `V−E+F` bookkeeping honest and emits a diff span):
`vert_add`, `vert_kill`, `edge_add`, `edge_kill`, `face_add`, `face_kill`, `edge_split`
(insert vertex, split radial faces' loops), `face_split` (SEMV/SFME — split n-gon by a chord
between two of its loops), `loop_rotate`. Composed operators call ONLY these + validation; they
never poke columns directly. This is the single most important invariant: it makes rejection and
undo-diffs uniform.

---

## 1. Foundation services required (build before any operator)

| Service | Responsibility | Consumers |
|---|---|---|
| **Adjacency cache** | disk (vert→edges), radial (edge→loops), loop-next/prev/radial walks in O(1); invalidated transactionally with each diff | every op |
| **Selection topology** | verts/edges/faces at polygroup vs triangle tier (PLAN §1); ordered *edge-loop* and *edge-ring* extraction; boundary-loop extraction (walk `radial.size()==1` edges) | Connect, Loop, Bridge, Bevel |
| **Manifold/precondition validator** | `is_manifold_edge`, `link_condition(edge)`, `would_flip_normal`, `is_degenerate(face)`, `boundary_merge_check`; returns a typed rejection reason, never mutates | every composed op |
| **Attribute interpolator** | interpolate loop uv/color/normal along a split edge (t-param), and copy-with-inheritance when a face is subdivided; per-face attr (uv_transform, polygroup, material) inheritance policy | Connect, Loop, Bevel, Subdivide, Bridge |
| **Spatial hash** | grid-bucket vertex lookup at a tolerance ε for Weld and Bridge auto-snap; deterministic bucket iteration order | Weld, Bridge |
| **Grid service** | current grid step (`[`/`]`), quantize-to-grid; Bevel width = grid step (Hammer convention) | Bevel |
| **Diff journal** | records changed column spans + free-list allocs/frees per atomic op, coalesced per composed op into one undo transaction; replayable against any pane's document (PLAN §5.5) | undo, multi-pane |
| **Transaction guard** | RAII scope: snapshot free-list heads, run composed op, on rejection *roll back to snapshot* so a half-applied op never escapes | every composed op |

The transaction guard is what makes "reject rather than corrupt" cheap: composed ops run
optimistically through atomic ops, and any precondition failure mid-op throws → guard restores the
pre-op free-list state and discards the partial diff. No operator ever needs a bespoke unwinder.

---

## 2. Per-operator specifications

Each: **preconditions → topology transform → attribute handling → diff**.

### 2.1 Connect / Cut (`V`)
- **Preconditions:** either (a) ≥2 selected verts that lie on a common face's boundary, or
  (b) 2 selected verts on the same face interior-connectable without crossing an existing edge.
  Reject if the two verts are already adjacent, if a proposed chord would exit the face, or if
  they belong to no shared face.
- **Topology:** for each ordered pair spanning a face, `face_split` (SFME): insert a new edge
  between the two loops and split the n-gon into two n-gons. Multi-vert selection cuts a fan/path
  of chords across successive shared faces (Scythe "connect selected verts").
- **Attributes:** both halves inherit the parent face's `material_index`, `uv_transform`,
  `polygroup`, `smooth_flag`, `hotspot_ref` **verbatim** (a straight cut must not reflow UVs —
  texture-lock invariant, PLAN §2). New loop corners on the cut edge get uv/color/normal
  **interpolated** along the cut endpoints from the parent face's loop attributes.
- **Diff:** +1 edge, +2 loops, −1 face +2 faces (per chord); columns: `edge_*`, `face_vertex_*`,
  face attr rows duplicated.

### 2.2 Edge-loop insert (`X` select ring → `V` connect)
- **Preconditions:** an ordered edge-**ring** (opposite edges of a quad strip). `X` builds the
  ring by walking `radial→loop.next.next→radial` across quads; the walk defines validity.
- **Topology:** `edge_split` each ring edge at t (default 0.5, slidable), then `face_split` each
  crossed quad between the two new verts → strip of quads becomes two strips. See §3.2 for the
  non-quad fallback (the hard part).
- **Attributes:** per §2.1; UVs interpolated at t so the inserted loop lands at the correct UV
  parameter (texture lock preserved). Face attrs inherited by both child quads.
- **Diff:** +k verts, +2k edges, +k faces for a ring of length k.

### 2.3 Bevel (`B`) — level-design bevel (edges only, grid-quantized)
- **Scope decision (see §3.1):** v1 is **edge bevel only, single segment, width = current grid
  step**. No vertex bevel, no profile superellipse, no multi-segment. This is the deliberate
  Scythe-vs-Blender scope cut.
- **Preconditions:** selected edges are manifold interior edges (`radial==2`); reject boundary
  and non-manifold edges. Width must be < half the shortest adjacent edge (else reject — would
  self-intersect).
- **Topology:** each beveled edge → new quad face; each affected vertex's incident faces shrink
  inward by width along their edges; corners where 2+ beveled edges meet get a corner patch
  (§3.1). Uses atomic `edge_split`/`vert_add`/`face_add`.
- **Attributes:** the new bevel quad takes the `material_index`/`polygroup` of the **dominant**
  adjacent face (larger area; tie → lower id, deterministic). Its `uv_transform` is projected
  from that dominant face so trim stays continuous. Corner patch inherits likewise. New edges
  marked hard or soft per current normal mode (§2.8).
- **Diff:** per edge +2 verts, +~5 edges, +1 face; corner patches add a face + fan.

### 2.4 Bridge (`Shift+C`)
- **Preconditions:** exactly two selected edge sets, each a single closed loop OR each a single
  open boundary strip; both on the same object; neither already directly shared. See §3.3.
- **Topology:** pair vertices between loops and emit one quad (or triangle where counts differ)
  per segment. Loops of unequal count fall back to §3.3's matching. If either "loop" is actually a
  filled face selection, first `face_kill` it to open the hole, then bridge the boundary.
- **Attributes:** bridge faces get `material_index`/`polygroup` from a chosen source loop
  (active-selection loop wins; else lower object id). `uv_transform` seeded to project along the
  bridge axis; corner uvs interpolated between the two ring's loop uvs by arc-length param.
- **Diff:** +N faces, +N edges (N = matched segment count); if holes were filled, −2 faces first.

### 2.5 Merge Faces (`Backspace`)
- **Preconditions:** ≥2 selected faces that are **edge-connected** and coplanar within an angle
  tolerance (default = hard-normal threshold). Reject if merging would create a non-simple
  (self-touching / hole-bearing) face or a non-manifold edge.
- **Topology:** dissolve the shared interior edges (`edge_kill` + loop splice) → single n-gon.
  Any vertex left with valence 2 and collinear neighbors is dissolved too (removes needless verts,
  Scythe behavior), guarded by the valence-2 precondition (P1 from CMU/link-condition work).
- **Attributes:** result face keeps the **active/dominant** face's `material_index`,
  `uv_transform`, `polygroup`, `hotspot_ref`. Surviving loop corner attrs are kept as-is; dropped
  interior corners are discarded.
- **Diff:** −(k−1) faces, −(shared edges), −(dissolved verts) for k inputs.

### 2.6 Weld Overlapping (`Shift+Ctrl+Alt+M`)
- **Preconditions:** operates on selected verts (or whole object). Candidate pairs are verts
  within ε (grid-fraction, default grid/16). Reject any weld that would produce a non-manifold
  edge (`radial>2`) or a zero-area/degenerate face (§3.4).
- **Topology:** spatial-hash cluster verts within ε; for each cluster elect a representative,
  `vert_merge` the rest onto it (retargets edges' endpoints, splices disks), then dedup edges that
  became identical (`edge_kill` extras, splice radials) and kill faces that collapsed to <3
  distinct verts. See §3.4.
- **Attributes:** merged vertex position = cluster average (or snap-to-grid if all near a grid
  node). Merged edge/loop attrs: keep representative's; when two loops of the same face fuse, that
  face is killed (it degenerated). UV seams (`E_UV_SEAM`) OR-combined.
- **Diff:** −(merged verts), −(dup edges), possibly −(degenerate faces).

### 2.7 Subdivide
- **Preconditions:** selected edges and/or faces; none. (Always valid — purely additive.)
- **Topology:** simple midpoint scheme (NOT Catmull-Clark — non-goal, PLAN §6): `edge_split` each
  selected edge at 0.5; for a fully-ringed quad, add a center vert and fan → 4 quads; n-gons fan
  from a new centroid vert to each edge midpoint (produces quads for even n, quad+tri cap for odd).
- **Attributes:** all new corner attrs bilinearly/linearly interpolated; face attrs inherited by
  all children; `polygroup`/`material` unchanged.
- **Diff:** additive only; +verts/+edges/+faces, no frees → cleanest diff of the set.

### 2.8 Normals — Soft (`J`) / Hard (`H`) / Dominant Face (`K`) / Invert (`Shift+N`)
- **Model (see §3.5):** normals are **per-loop**, driven by per-edge `hard` flags + per-face
  `smooth_flag`. There are no separate "smoothing groups"; a hard edge is a normal-split seam.
- **Soft (`J`):** clear `hard` on selected edges → adjacent loops share an area/angle-weighted
  averaged vertex normal. **Hard (`H`):** set `hard` → each face keeps its geometric normal at
  that edge (split). **Dominant Face (`K`):** for the selected verts/edges, snap all incident
  loop normals to the normal of the **dominant** adjacent face (largest projected area; tie →
  lowest face id) — used to kill shading artifacts on trim where one big face should "win".
  **Invert (`Shift+N`):** reverse winding of selected faces (reverse loop ring order) and negate
  their loop normals; reject if it would make an edge non-manifold-consistent with a neighbor
  (mixed winding across a shared manifold edge is flagged, not silently kept).
- **Attributes:** only `edge.hard`, `face.smooth_flag`, and loop `normal` change (Invert also
  reorders loop ring + flips face `uv_transform` handedness so texture stays put).
- **Diff:** attribute-only for J/H/K (no topology change); Invert reorders loop columns.

### 2.9 Detach (`N`) / Extract (`Alt+N`)
- **Detach (`N`):** split selected faces away from the mesh along their shared boundary —
  duplicate the boundary verts/edges so the selection becomes a separate connected component
  **within the same `LevelBlock`**. Precondition: selection is a face set with a well-defined
  boundary. Topology: `edge_split`-style rip along the boundary loop (new verts, radial detaches).
  Attributes: copied verbatim to the new corners. Diff: +boundary verts/edges.
- **Extract (`Alt+N`):** Detach **into a new `LevelBlock`** (new `LevelMeshData` node). Same rip,
  then move the component's faces/verts into a freshly-created kernel mesh and remove them from the
  source. Attributes carried over; new block inherits transform. Diff: two coupled diffs (source
  shrinks, new block created) in one undo transaction.

---

## 3. Core difficulties (chosen solution + algorithm sketch + references)

### 3.1 Bevel — corner topology where 3+ beveled edges meet (THE hard one)
**Problem:** beveling a single edge is a trivial quad. The pain is the *vertex corner*: when 2, 3,
or N beveled edges share a vertex, the shrunk-back faces leave a gap that must be filled with a
consistent patch, and the naive "just connect the new points" produces pinches, overlaps, or
non-planar n-gons — this is exactly why Blender's `bmesh_bevel.cc` grew EdgeHalf/Profile/miter
machinery.

**Chosen solution — a *restricted* level-design bevel + a fixed corner-patch rule.** Because v1 is
single-segment, grid-quantized, edges-only, we can define an *exact* corner topology instead of
Blender's general solver:
1. At each vertex, collect the ordered fan of incident edges (from the disk cycle, sorted
   angularly around the vertex normal). Mark which are beveled.
2. Each beveled edge contributes **one new vertex per adjacent face**, offset inward by the grid
   width along that face's boundary edges (offset point = along-edge march, not plane-intersect —
   keeps it grid-clean and cheap).
3. **Corner fill by beveled-edge count at the vertex:**
   - **1 beveled edge:** no corner patch (the two new verts just cap the bevel quad).
   - **2 beveled edges:** connect the two new-vert pairs → single quad corner (the common "flat
     miter"). This is Blender's "inner/outer miter = none" case made the default.
   - **3+ beveled edges (N):** build an **N-gon corner cap** from the ring of new verts around the
     vertex (one or two per incident face), triangulated to the corner centroid only if the N-gon
     is non-planar beyond tolerance. This is the single, predictable topology — no
     profile-dependent patch explosion.
4. Reject if grid width ≥ ½ shortest incident edge (would swallow a neighbor vert) → the user
   drops grid size and retries. Rejection here is a feature: it's the guardrail that keeps corners
   solvable.

**Why this scopes out the graveyard:** Blender's corner complexity is dominated by multi-segment
profiles and arbitrary widths per edge. Fixing width to one grid step and segments to one collapses
the corner to a bounded, enumerable set of cases. If artists later need rounded trim, that's a
post-slice multi-segment extension, not v1.

*References:* Blender Bevel manual (inner/outer miter definitions for 3+ edge corners)
<https://docs.blender.org/manual/en/latest/modeling/meshes/editing/edge/bevel.html>;
BMesh editing kernel / `bmesh_bevel.cc` EdgeHalf + Profile design
<https://deepwiki.com/blender/blender/11.2-bmesh-editing-kernel>;
BMesh developer docs (disk cycle for the angular edge fan)
<https://developer.blender.org/docs/features/objects/mesh/bmesh/>.

### 3.2 Loop insert across n-gon faces — ring walk when the ring hits a non-quad
**Problem:** the loop-cut ring walk assumes quads: enter an edge, exit the topologically-opposite
edge, repeat. An n-gon (or triangle) has no unambiguous "opposite" edge, so the classic algorithm
just stops — but our meshes are n-gon-first, so "stop" is common and must be graceful, not a dead
tool.

**Chosen solution — quad-walk with an explicit "terminate at pole" rule + partial-loop commit
(Blender's actual behavior, made a hard contract).**
1. Ring extraction (`X`): from the seed edge, at each face take `loop.next.next.edge` (the opposite
   edge) **only if that face is a quad**. If the face is a tri or n-gon, the ring **terminates**
   at that face — do not turn a corner (turning would spawn a tri or 5+-gon, defeating the point).
2. Walk both directions; the ring is the union. It is a full loop iff it closes; otherwise it's an
   open partial loop bounded by poles.
3. Insert (`V`): `edge_split` every ring edge, `face_split` every quad the ring crosses. The n-gon
   poles at the ends are **left intact** — the inserted loop simply dead-ends into them (their
   boundary gains one vertex from the terminal `edge_split`, splitting that pole n-gon into an
   (n+1)-gon and a small face only if the user's chord is committed there; default: dead-end
   without cutting the pole).
4. UX contract: highlight the previewed ring; if it dead-ends, show the partial ring — never
   silently do nothing. This is the single behavior that makes n-gon meshes tolerable to edit.

*References:* Blender Loop Cut manual (loops travel through quads, stop at tris/n-gons)
<https://docs.blender.org/manual/en/latest/modeling/meshes/tools/loop.html>;
"How the loop cut tool works" (opposite-edge rule + pole termination)
<https://artisticrender.com/how-the-loop-cut-tool-works-in-blender/>.

### 3.3 Bridge — orientation/matching under vertex-count mismatch + twist
**Problem:** two loops rarely have equal counts or aligned starts; naive index pairing produces
crossed faces and twisted tubes.
**Chosen solution — arc-length correspondence + best-rotation search + explicit twist offset.**
1. **Orientation:** compute each loop's winding order relative to its average normal; if the two
   loops face "the same way" the bridge would fold — reverse one loop's traversal so the faces come
   out consistently wound (checked by the manifold validator: every bridge edge must end
   `radial==2` with consistent winding, else reject).
2. **Start alignment (twist minimization):** parameterize both loops by normalized arc length
   [0,1). For each candidate rotational offset of the smaller loop's start against the larger,
   score Σ chord length of paired segments; pick the min-sum offset (this is the discrete twist
   search — it's what Blender's Twist param nudges manually). Expose the resulting offset as the
   `Twist` knob so users can ±1 it.
3. **Count mismatch (M vs N, M<N):** walk both by arc length; emit a quad when both advance one
   step, a **triangle** when the longer loop must advance without the shorter (fan the extra
   vertices). No new verts are inserted on either loop — keeps it predictable and reversible.
   (Blender's alternative is to subdivide the sparse loop; we choose triangulation to avoid
   mutating the input loops.)
4. Reject if either loop isn't a single simple cycle/strip, or loops are on different objects
   without an explicit cross-object bridge flag.

*References:* Blender Bridge Edge Loops manual (pairing by sequential order, Twist offset)
<https://docs.blender.org/manual/en/latest/modeling/meshes/editing/edge/bridge_edge_loops.html>;
Bridge Edge Loops overview (twist = rotational pairing offset; mismatch handling)
<https://grokipedia.com/page/Bridge_Edge_Loops>.

### 3.4 Weld correctness — tolerance, non-manifold prevention, attribute merge on loop collision
**Problem:** welding within ε can (a) fuse verts that shouldn't, (b) create `radial>2`
(non-manifold) edges, (c) collapse faces to degenerate, (d) leave duplicate coincident edges.
**Chosen solution — spatial-hash cluster → validated vert_merge → edge/face dedup, with the CMU
link-condition as the reject gate.**
1. **Cluster:** insert selected verts into a uniform spatial hash (cell = ε). A cluster = verts
   mutually within ε via union-find over neighboring cells (deterministic cell iteration order for
   reproducible representative election — lowest vert id wins, avoiding Scythe's name-collision
   class of nondeterminism).
2. **Validate before merging:** for each pair to be fused, check the **link condition** — the two
   verts' one-ring links must intersect in exactly the shared-edge endpoints; violating it means
   the merge would pinch the surface non-manifold → reject that pair (or the whole cluster,
   configurable) rather than corrupt.
3. **Merge:** retarget every incident edge of the losers onto the representative; splice disks.
4. **Dedup:** edges that now have identical endpoint pairs are merged — splice their radials;
   reject if the spliced radial exceeds 2 (non-manifold). Faces whose loop ring now has <3 distinct
   verts are `face_kill`ed (degenerate). Faces that became "figure-8" (a vertex appears twice) are
   rejected → whole cluster rolled back.
5. **Attributes:** representative position = cluster centroid, snapped to grid if all within ε of a
   grid node. Colliding loops on a surviving face: keep the representative loop's uv/color/normal;
   `uv_seam` flags OR-combined; `hard` edge flags OR-combined (a seam survives a weld).

*References:* CMU MeshEdit — link condition guaranteeing 2-manifold after collapse/merge
<https://462cmu.github.io/asst2_meshedit/>; CGAL edge-collapse link-condition precondition
<https://doc.cgal.org/4.3/Surface_mesh_simplification/classEdgeCollapsableMesh.html>.

### 3.5 Connect/cut & normals on an n-gon mesh (polygroup/UV split + Dominant Face semantics)
Bundled because both are "how do attributes behave on n-gons" questions.
- **Face split (Connect):** when SFME splits an n-gon into two, both children **inherit the parent
  row verbatim** (`material_index`, `uv_transform`, `polygroup`, `hotspot_ref`) — a cut never
  reflows texture (texture-lock invariant). New corners on the cut edge interpolate loop uv/color
  from the parent's two endpoints by the cut's t-param. Polygroup stays single unless the user
  later re-groups. This keeps trim continuous across a cut, the property Scythe users care about.
- **Normals model:** per-loop normals; a **hard edge** is a normal seam, a **soft edge** shares an
  angle-weighted average across the edge. No smoothing-group integer — the hard-edge boolean *is*
  the smoothing group boundary (equivalent expressiveness, simpler diff). **Dominant Face (`K`)**
  means: for the selected verts, override all incident loop normals with the largest-area adjacent
  face's normal — the level-design idiom for "make this corner shade like the big wall, ignore the
  little chamfer." Baked into per-loop `normal` columns at edit time so the bake path (`LevelMeshBaker`)
  is a pure copy — no runtime normal solve.

*References:* Blender Bevel manual miter/normal notes (dominant-face style corner shading)
<https://docs.blender.org/manual/en/latest/modeling/meshes/editing/edge/bevel.html>;
ryg / half-edge attribute-on-loop model & BMesh loop attributes
<https://developer.blender.org/docs/features/objects/mesh/bmesh/>.

---

## 4. Headless test ideas (`one-more-house/tools/checks/`)

Every check drives the GDScript-exposed kernel — no editor. Seeded RNG for determinism.

1. **Euler characteristic invariants.** After each op on a closed manifold block assert
   `V − E + F == 2·(components) − 2·(genus)` matches the op's *declared* delta (subdivide/connect
   keep χ; bridge/detach change it predictably). Any drift = topology corruption.
2. **Manifold assertion.** After every op, assert no edge has `radial>2` and no vertex has a
   non-single-fan disk (except at declared boundaries). This is the primary corruption tripwire.
3. **Idempotence / no-op stability.** Subdivide-then-merge back, weld with ε=0, bevel width 0,
   soft-then-hard-then-soft an edge → assert the mesh is byte-identical (or attribute-identical) to
   start. Catches accumulation bugs.
4. **Round-trip diff replay.** Apply op → capture diff → undo (revert diff) → assert byte-identical
   to pre-op snapshot; redo → assert identical to post-op. Run interleaved across two documents to
   cover multi-pane replay (PLAN §5.5).
5. **Reject-not-crash fuzzing.** Generate random selections × every op × pathological inputs
   (mismatched bridge loops, coincident verts, boundary-edge bevel, n-gon loop-cut, self-touching
   weld). Assert the op either produces a valid manifold result **or** returns a typed rejection
   and leaves the mesh byte-identical — never a crash, never a corrupt in-between state (this is
   the LE5 Scythe-v0.6 crash-class guard applied early to LE4).
6. **Attribute continuity.** After Connect/Loop/Subdivide with texture-lock on, sample UV at fixed
   world points before/after → assert unchanged within ε (texture-lock invariant). After Merge,
   assert result carries the active face's material/polygroup.
7. **Corner enumeration (bevel).** Procedurally build vertices with 1/2/3/4/5 incident beveled
   edges; assert each produces the §3.1 declared corner topology and stays manifold + planar within
   tolerance.
8. **Bake/selection cache consistency.** After a randomized op sequence, rebuild bake + selection
   caches from scratch and assert they equal the transactionally-invalidated caches (PLAN §5.2
   derived-state-desync guard).

---

## 5. Build order within LE4

1. Foundation services §1 (adjacency, validator, interpolator, spatial hash, diff journal,
   transaction guard) + atomic Euler ops + tests 1/2/4/5 scaffolding.
2. Subdivide + Connect/Cut (purely-additive & single-face-split — safest, exercise the whole
   diff/undo path).
3. Loop insert (§3.2 ring walk) → Merge Faces → Weld (§3.4).
4. Normals J/H/K/Invert (§3.5) — attribute-only, low risk.
5. Bevel (§3.1) — last and hardest; land with the §4.7 corner enumeration check.
6. Bridge (§3.3) + Detach/Extract — cross-component ops, need the multi-diff transaction.

Each lands with (a) its headless checks, (b) an interactive smoke scenario, (c) a DIVERGENCE
ledger entry for any upstream touch, (d) a `DECISIONS.md` note game-side (PLAN §4).
