# 09 — Face texturing ops (LE2)

**Status:** planning. **Prereq reading:** `PLAN.md` §1 (`LevelMeshData` schema), §2 (UX contract —
this is the panel called out as needing first-class design), §4 LE2 scope line.

**Scope of this doc:** the full Scythe-class face-texturing tool set — Apply, Lift, Wrap,
Wrap-to-selection, Flow, Align to Grid/Face, the Modify Texture panel, blockout quick slots, and
texture lock — specified as algebra on `LevelMeshData`'s per-face `uv_transform : Transform2D`
plus a projection-frame extension this doc proposes. Local reference: the Cyclops clone
(`cyclops_level_builder`), which already stores `face_uv_transform : Array[Transform2D]`
(`resources/convex_block_data.gd:40`) composed on top of a world-axis dominant-axis planar
projection (`math/convex_volume.gd:520-528`, `math/math_util.gd:617-636`) — confirmed by reading
its source, mined throughout §1 and §4.

---

## 1. Data model & the projection-frame convention

### 1.1 Why this needs to be nailed down first

Wrap, Flow, Align-to-Face, and Justify are all *defined in terms of* "the face's 2D coordinate
system." If that system is recomputed from the live face normal on every evaluation, two failure
modes appear, both attested in the reference material:

- **Discontinuous flips.** A dominant-axis argmax (`|N.x|` vs `|N.y|` vs `|N.z|`) is exactly what
  Cyclops uses for its default planar projection (`math_util.gd:617` `get_axis_aligned_tangent_and_binormal`,
  built on `get_longest_axis`, `math_util.gd:285`). Fine as a *one-shot* rule, but re-run live on
  every geometry edit, a face whose normal drifts across the 45° boundary between two dominant
  axes (routine during bevel/tilt) would see its whole texture basis swap discontinuously. This is
  the exact failure the Quake `.map` format's **220 extension** fixed: the original format derived
  texture axes from the face plane on load (implicit, recomputed, warped/skewed under rotation);
  Valve's 220 format stores the texture axis vectors **explicitly** per face instead, immune to how
  the plane got there (canonical description: TrenchBroom's brush-format docs,
  `trenchbroom.github.io` — "Standard/Valve/Quake3" face-attribute formats).
- **World-lock as a feature, not a bug.** Cyclops's default projection uses **world position**,
  not face-local position (`fv.uv0 = Vector2(-v.point.z, -v.point.y)` for X-dominant faces — raw
  `v.point`). That's why two separate blocks with the same dominant axis and `uv_transform` tile
  seamlessly with zero alignment work — the desired default for trims/floors/walls, and exactly
  what Source 2 Hammer's grid-projected texturing gives before you touch anything.

**Decision: explicit per-face frame, two independent re-projection ops.** Extend
`LevelMeshData`'s per-face columns (beyond the already-planned `uv_transform : Transform2D`) with:

```
uv_origin  : Vector3   # O — world-space anchor point
uv_tangent : Vector3   # T — world-space "U" direction, unit length
```

`uv_bitangent` is **not** stored — it's derived live as `B = normalize(N.cross(T))` using the
*current* face normal `N`. This is the key refinement over naive 220-style full-frame storage:
deriving `B` from a stored `T` and the live `N` is a **continuous** function (no argmax, no
branch), so it tracks small normal drift smoothly with no flip — only `T` itself needs to be
frozen, because `T` is the thing that historically came from a discontinuous argmax decision.
`T` becomes near-parallel to `N` only in the genuine degeneracy case (face rotated ~90° from its
last alignment), which is the same degeneracy §4.3 already has to guard.

Per-face `get_uv(vertex)`:

```
project_native(face, p) = Vector2( (p - O).dot(T), (p - O).dot(B) )      where B = normalize(N × T)
get_uv(face, p)          = face.uv_transform * project_native(face, p)    [PROJECTED faces]
get_uv(face, p)          = per_loop_uv[face, p]                           [UNFOLDED faces]
```

`uv_mode` (`PROJECTED` / `UNFOLDED`) is a per-face flag. `UNFOLDED` is written by the hotspot
fitter's Conforming/Follow-Active-Quads modes (PLAN.md §3) and by manual edits in the Fast
Texture unwrap overlay; everything in this doc defaults to and mostly targets `PROJECTED`. §4.5
covers the interop case explicitly.

### 1.2 Two ways to (re-)populate the frame

Both are user-invoked re-projection ops, not continuous recomputation — the frame only changes
when one of these runs:

- **Align to Grid** (`Shift+Ctrl+G`, world-aligned dominant-axis frame — the Quake-220 /
  Cyclops-precedent case): `T` = table lookup on `argmax(|N.x|,|N.y|,|N.z|)` (X-dominant →
  `T = -Z`, Y/Z-dominant → `T = -X`, matching the sign convention proven in the Cyclops reference
  so ported content doesn't visually rotate); `O = Vector3.ZERO` always (world origin, not
  block/face-local) — *why* grid mode gives cross-block texel continuity for free, called out in
  PLAN.md §2 as "essential."
- **Align to Face** (`Shift+Ctrl+F`, face-local frame): if the face already has a `uv_tangent`,
  re-orthogonalize it against the current normal (`T' = normalize(T - N*(T.dot(N)))`) — preserves
  the user's chosen orientation across a normal change, minimal-surprise. If the face has no prior
  tangent (freshly created), fall back to the direction of its longest boundary edge (deterministic,
  geometry-derived — same tie-break philosophy as the hotspot fitter's seeded determinism, PLAN.md
  §3). `O` = face centroid.

Storage is uniform; only the *population method* differs. This resolves the "world-aligned vs
face-local" question as a false dichotomy — both are reprojection operators writing the same
fields, exactly matching the two keybinds the spec already names.

---

## 2. Foundation services

| Service | Responsibility | Consumers |
|---|---|---|
| **Frame algebra** (`project_native`, `get_uv`, `Align to Grid`/`Align to Face` frame builders) | §1 formulas; single source of truth for "what does this face's UV look like right now" regardless of mode | every op below |
| **Edge-hinge solver** (2-point similarity fit, §4.1) | given two world-space point→UV correspondences, solve the unique similarity `Transform2D` (rotation + uniform scale + translation) satisfying them | Wrap, Wrap-to-selection, Flow, hotspot fitter's **Conforming** and **Follow Active Quads** modes (PLAN.md §3) — this is the "unfold-across-edge operator shared with the unwrap library" the task calls for; build it once in `modules/level_kernel/uv/edge_hinge_solver.*` |
| **World-grid projection** | Align-to-Grid frame builder + the fixed grid `uv_transform` scale (texel density → world units) | Align to Grid, blockout quick slots (grid-snapped trims must tile without wrap) |
| **Texture-lock resolver** (§4.3) | per-face anchor cache, least-squares affine refit, degeneracy guard | any vertex-moving op (move/push-pull/extrude/bevel) when texture lock is on (default) |
| **Native-UV read adapter** (§4.5) | `get_uv(face, vertex)` works identically whether `face.uv_mode` is `PROJECTED` or `UNFOLDED`; only *writing* a solved transform requires `PROJECTED` | Wrap/Flow/Lift when the neighbor/source face is unfolded |

All four non-trivial services are kernel-side (`modules/level_kernel`), no editor dependencies,
per PLAN.md's "fully scriptable" requirement — every op here must be callable from a headless
GDScript check with no viewport.

---

## 3. Operation specs

Each op is defined as a transformation of one or more faces' `(uv_mode, O, T, uv_transform)` (and,
for Lift, the "active material + captured mapping" tool state).

### 3.1 Apply (`Shift+T`) / Lift (`Shift+RMB`)

- **Apply**: for each face in selection, `face.material_index = active_material`. If the face has
  no valid frame yet (`uv_tangent` is zero/uninitialized), lazily run Align-to-Grid on it first —
  applying a material should never leave a face without a usable projection.
- **Lift**: reads the picked face's `material_index` **and** its full UV state
  (`uv_mode, O, T, uv_transform`) into the tool's "active material + captured mapping" slot. This
  is the thing Scythe's docs are explicit about (lift ≠ just eyedrop material) — copying the UV
  state is what makes a later Apply on a different face *not* look different. If the source face
  is `UNFOLDED` (§4.5), the captured mapping degrades to a similarity-fittable local UV template
  rather than a `Transform2D`.

### 3.2 Wrap (`Alt+RMB`) / Wrap-to-selection (`Shift+Alt+RMB`) / Flow (`Ctrl+Alt+RMB`)

All three are the same primitive — solve the destination face(s)' `uv_transform` from the
edge-hinge solver (§4.1) — applied to a different face set:

- **Wrap**: single adjacent face across the last-clicked shared edge.
- **Wrap-to-selection**: BFS from the source face across the current selection's internal edges,
  each face solved against its already-solved neighbor (propagation, not all-against-source).
- **Flow**: same BFS, but along a drag path (edge loop/ring or click-drag across a curved strip)
  rather than a static selection — see §4.2 for why propagation (not "solve everyone against face
  0") is the right choice here too.

All three convert their destination face(s) to `PROJECTED` mode as a side effect (§4.5).

### 3.3 Align to Grid (`Shift+Ctrl+G`) / Align to Face (`Shift+Ctrl+F`)

Frame builders from §1.2, plus reset `uv_transform` to identity (Align to Grid) or preserve it
(Align to Face, since it's meant to fix orthogonality, not throw away alignment work).

### 3.4 Modify Texture panel

Persistent (not modal) dock, numpad-grammar nudges plus explicit buttons, operating on the
selected face(s)' `uv_transform` only — never `O`/`T` (those are the "structural" frame; the panel
edits the "alignment" layer on top, matching Hammer's Face Edit Sheet split between the projection
and the offset/rotation/scale fields):

| Control | Effect |
|---|---|
| Numpad nudge (8 dir. + large-step modifier) | `uv_transform.origin += step * direction`, direction in the face's **current** UV space — always "relative to the texture as seen on this face," independent of frame orientation |
| Scale X / Y | multiply a column of `uv_transform`'s linear part; pivot = UV-space bbox center, not texture origin (origin-pivoted scale is the classic "texture flies off the face" complaint) |
| Rotate CW / CCW | `uv_transform = Transform2D(±step).translated(pivot) * uv_transform`, pivot = UV-space bbox center |
| Fit | solve linear part so the UV bbox exactly covers the material's texture footprint (per-axis, unless "keep aspect" is on) |
| Justify L/R/T/B/Center | translation-only; see §4.4 |
| Flip, texture-level `Alt+E`/`Alt+D` | flips the **material's** texture sampling (flag on the material/quick-slot); touches zero faces' `uv_transform`, affects every face using it |
| Flip, face-UV-level `Alt+T`/`Alt+R` | negates one column of *this face's* linear part, re-centered on the UV bbox so it doesn't also translate off the face |

### 3.5 Blockout quick slots (`Shift+Alt+1..6`)

Each slot holds a material ref (persistent, `LevelEditor` singleton per PLAN.md §1). Distinct from
a plain material-browser click: the quick-slot chord sets the active material **and**
immediately runs Apply on the current selection if one exists — a one-chord "paint" action, the
detail that makes blockout-by-numberkey actually fast.

### 3.6 Texture lock (default ON)

Not a discrete op — a standing mode that intercepts every vertex-moving operator (move, push/pull,
extrude, bevel, scale) and re-solves affected faces' UV state so their *appearance* survives the
edit. Full design in §4.3.

---

## 4. Core difficulties

### 4.1 Wrap across the shared edge

**Difficulty:** solve the neighbor face's `uv_transform` so the texel field is continuous across
the hinge, for an arbitrary dihedral angle, without assuming anything about how the neighbor's own
frame was populated.

**Chosen solution — 2-point similarity solve in UV space, not a 3D fold.** Because `get_uv`
is affine in the projected 3D→2D sense (`project_native` is linear-affine in world position, and
`uv_transform` is a 2D affine map), the composition restricted to any straight 3D line — in
particular the shared edge — is itself affine in the edge's 1D parameter. That means: **matching
UV at the two shared vertices is necessary and sufficient for continuity along the entire edge**,
not just at those two points. This sidesteps doing any 3D dihedral-rotation math:

```
source points  p0, p1 = project_native(neighbor, v0), project_native(neighbor, v1)   # pre-transform
target points  q0, q1 = get_uv(source_face, v0),        get_uv(source_face, v1)       # already aligned

treat 2D vectors as complex numbers:
  z  = (q1 - q0) / (p1 - p0)     # complex division: gives scale = |z|, rotation = arg(z)
  t  = q0 - z * p0               # translation

neighbor.uv_transform = Transform2D(rotation=arg(z), scale=|z|, origin=t)
```

Closed-form, no iteration (2 points exactly determine a similarity transform's 4 DOF — compare
Umeyama's least-squares similarity derivation for the >2-point case). It deliberately leaves the
neighbor's own `(O, T)` untouched, so wrap never fights a neighbor's existing frame orientation,
only aligns the editable transform layer on top. If the neighbor has no frame yet (new face from
an extrude), lazily Align-to-Grid it first, then solve as above.

**Non-planar / multi-edge adjacency (T-junctions from booleans, an edge shared by >2 faces):**
deterministic tie-break — the already-`PROJECTED` neighbor with the lowest face index is the
reference; if more than one already-projected neighbor disagrees (would solve to different
transforms), don't silently pick one — flag it as a seam and surface it the same way the hotspot
atlas already visualizes island boundaries (PLAN.md §3), rather than crashing or guessing.

**References:** TrenchBroom brush-format docs (`trenchbroom.github.io`) on Standard/Valve-220
face attributes as the ancestral per-face-affine model; Umeyama, S., *Least-Squares Estimation of
Transformation Parameters Between Two Point Patterns*, IEEE PAMI 1991, for the closed-form
point-correspondence transform family this is a 2-point special case of.

### 4.2 Flow along a curved surface

**Difficulty:** propagate a hinge solve along a whole strip of faces (an arch, a barrel vault)
without drift or shear buildup.

**Chosen solution — pairwise propagation, not solve-everyone-against-the-source.** Flow reuses the
§4.1 solver face-by-face along the BFS/path order, each face solved against its immediate,
already-solved predecessor. Every step is *anchored exactly* at its two shared vertices, so there
is no positional drift along the seam. The risk that remains is **rotational twist**: a solver that
instead derived rotation from "this face's global frame orientation" would inherit any
dominant-axis-argmax flip between consecutive faces (§1.1's discontinuity) and accumulate visible
twist. The chosen solver derives rotation purely from the two actual corresponding UV points at
each step, never consulting the neighbor's frame convention — a dominant-axis flip between face
*i* and *i+1* is invisible to it. This is the discrete analogue of a rotation-minimizing frame
(compare Bishop, R. L., *There Is More Than One Way to Frame a Curve*, Amer. Math. Monthly, 1975,
on avoiding accumulated twist when propagating an orientation along a curve) and matches the direct
UX ancestor, Source 2 Hammer's **UV Peel** (select an edge loop spanning a curve, UV Peel "makes
the texture flow around the mesh, maintaining alignment at all points" — Valve's own Mesh
Texturing documentation) and DreamUV's viewport "Flow" operator.

Texel-density drift (as opposed to twist) is bounded, not eliminated: consecutive faces with very
different width-to-hinge-length ratios legitimately show different perpendicular stretch — that's
geometry, not solver error; the §5 headless test checks for compounding *rounding* specifically.

### 4.3 Texture lock: re-solving `uv_transform` on vertex motion

**Difficulty:** when vertices move, keep the *appearance* of the texture on the face instead of
letting it slide (Source 2's "Translate/Scale with texture lock," generalized here to any
vertex-level edit at any selection tier, not just whole-object transforms).

**Two cases, and why they need different handling:**

1. **Whole-face/whole-block rigid or affine transform** (move/rotate/scale a selection as one
   unit): there is an exact solution — apply the *same* 3D transform `M` to the frame:
   `O' = M.xform(O)`, `T' = M.basis * T` (re-orthonormalized against the new normal per §1.1's
   live-derivation of `B`). No fitting needed; this is the cheap, exact path and should be the
   common case (it's what the Cyclops reference does incrementally for its own move tools —
   `convex_volume.gd`'s move-face code composes `uv_transform` with the move's induced 2D shift
   rather than recomputing from scratch).
2. **Partial/asymmetric vertex drag** (one corner of a quad moved without the others — no single
   3D transform explains the motion): untouched vertices need no work at all (frame unchanged, so
   their UV is unchanged automatically — texture lock is free for most of what actually happens
   during editing). For the vertex(es) that moved, **least-squares refit** the face's
   `uv_transform`'s linear+translation part (6 DOF, closed form via normal equations) using
   `(old_uv_i, new_planar_position_i)` correspondences across all of the face's vertices —
   exactly determined at 3 non-collinear points, over-determined (smoothing, the desired behavior)
   for n-gons with n > 3.

   **Degeneracy guard (the case the task calls out explicitly):** if the drag pushes the face
   toward near-zero area — collapsing to a sliver, or anchor points going near-collinear — the
   normal-equations matrix becomes ill-conditioned. Detect via an area/condition-number threshold
   (reuse the kernel's `face_area_x2`-style check already used for topology-op precondition
   guards) and **freeze `uv_transform` unchanged** rather than let the fit blow up — the same
   reject-don't-crash philosophy PLAN.md applies to booleans. Re-solve resumes once the face is
   non-degenerate again.

**References:** Umeyama 1991 again, for the least-squares point-correspondence transform family
this affine refit generalizes (similarity → full affine, 6 DOF instead of 4); Valve's *Texture
alignment* documentation (`developer.valvesoftware.com/wiki/Texture_alignment`) for the target
behavior being reverse-engineered — "Translate with Texture Lock" and "Scale with Texture Lock"
apply specifically in the Meshes/Objects/Groups selection modes, confirming case 1 above is the
primary documented use case and case 2 is the harder generalization this project needs for
vertex/edge-tier edits that Source 2 doesn't expose the same way.

### 4.4 Justify/Fit semantics on an arbitrarily rotated face

**Difficulty:** what does "Justify Left" mean on a face whose `uv_transform` already has a
rotation baked in, or whose frame is face-local at some odd angle?

**Chosen solution — operate purely in UV space, never in world/screen space.** Justify/Fit read
the face's **current** `get_uv(vertex)` values (i.e., after both the frame projection and the
existing `uv_transform` are applied), take their bounding box in UV space, and only ever adjust
`uv_transform`'s *translation* component (Justify) or its *linear* component's scale (Fit) — they
never reason about world axes or the frame's `(O, T)` at all. Concretely: Justify Left computes
`u_min` of the current UV bbox and shifts `uv_transform.origin` so the new `u_min = 0` (or the
active material's left edge in texture space); Right/Top/Bottom/Center are the symmetric cases;
Fit scales so bbox width/height exactly cover the material's footprint (independently per axis
unless "keep aspect" is set). Because this only ever consumes *already-computed* UV coordinates,
it is correct regardless of how strange the face's frame is — a face rotated 37° via Align-to-Face
justifies exactly the same way a grid-aligned face does, because by the time Justify runs, "left"
already means "small U" in that face's own current parameterization, not any world direction.

**References:** Valve's *Hammer Face Edit Dialog* documentation
(`developer.valvesoftware.com/wiki/Hammer_Face_Edit_Dialog`) — the classic Source 1 Face Edit
Sheet's Justify Top/Bottom/Left/Right/Center/Fit buttons are the direct ancestor Source 2 and
Scythe inherited, and are documented as operating on the face's own texture-space bounds, never
world axes; TrenchBroom's face-attribute editor (`trenchbroom.github.io`), which exposes only the
primitive Offset/Rotation/Scale-X/Scale-Y fields with no "Justify" convenience at all — confirming
Justify is purely a derived UX layered on the same primitives this doc already defines in §3.4,
not a separate data concept.

### 4.5 Interop when the neighbor uses per-loop UVs instead of `uv_transform`

**Difficulty:** Lift/Wrap/Flow need to read from or write to a face that's in `UNFOLDED` mode
(hotspot-conforming bake, or a hand-authored unwrap in the Fast Texture overlay), which has no
single affine `uv_transform` to solve.

**Chosen solution — asymmetric read/write, not a unified representation.** The **read** side
(`get_uv(face, vertex)`) already works identically for both modes per §1.1 — the edge-hinge solver
only ever needs `get_uv` at two vertices, a trivial per-loop lookup in `UNFOLDED` mode. So
**wrap/flow sourcing from an unfolded neighbor needs zero special-case code.** The **write** side
is where the asymmetry lives: solving *into* a face always requires `PROJECTED` (a `Transform2D`
is meaningless on per-loop data), so Wrap/Flow **unconditionally convert their destination to
`PROJECTED`** as part of applying (§3.2) — matching Scythe/Hammer's actual behavior of always
establishing a fresh per-face alignment on the destination. Lift degrades gracefully from an
`UNFOLDED` source: it captures material + a *local UV template* that a later Apply can only
similarity-fit onto a topologically compatible destination (§4.1's solver, matching 2+ points
instead of exactly 2); on a topology mismatch it falls back to "material only, identity
`uv_transform`," with a status-bar notice — never a silent no-op or a crash.

**References:** this doc's own §1.1 `uv_mode` split as the internal source-of-truth; Blender's UV
system faces the identical problem whenever a rule-based unwrap (Follow Active Quads) coexists
with authored per-loop edits — the same read-cheap/write-normalizes pattern is how addons like
DreamUV and Zen UV avoid two codepaths per operator.

---

## 5. Headless test ideas

All driveable via `modules/level_kernel`'s scripted API with no viewport, per PLAN.md's "fully
scriptable" kernel requirement. Suggested check: `tools/checks/face_texture_ops_check.gd`
(game-side, mirroring the existing `tools/checks/` pattern).

1. **Wrap continuity, not just endpoint match.** Two quads sharing an edge at a non-trivial
   dihedral angle. Apply an arbitrary `uv_transform` to face A, run Wrap onto B, then sample
   `get_uv` at several parametric points *along* the shared edge (`t = 0.0, 0.25, 0.5, 0.75, 1.0`,
   not just the two vertices) via both faces' formulas and assert equality within epsilon. A
   naive implementation that matches vertices but gets the linear part wrong (e.g. via a full
   affine fit instead of a similarity fit) would pass an endpoints-only test and fail this one.
2. **Flow chain, twist/drift bound.** A 6-face curved strip (barrel-vault approximation). Flow
   from face 0, assert edge continuity (test 1's method) at every internal edge transitively, and
   assert face 5's `uv_transform` scale magnitude stays within tolerance of face 0's — catches
   compounding rounding error distinct from honest geometric stretch.
3. **Texture lock invariance, rigid case.** Build a face, engage texture lock, apply a randomized
   rigid transform (translate + rotate) to its vertices as a unit, assert every vertex's `get_uv`
   is unchanged within epsilon after the exact-recompute path (§4.3 case 1) runs.
4. **Texture lock, partial drag.** Move one vertex of a quad only. Assert the other three
   vertices' UVs are bit-for-bit unchanged (frame-untouched fast path), and assert the moved
   vertex's new UV matches the least-squares refit's prediction.
5. **Degenerate collapse guard.** Drive a face toward zero area (collapse two vertices together)
   mid-drag; assert the resolver does not NaN/throw and instead leaves `uv_transform` bitwise
   unchanged across the collapse attempt; un-collapse and assert normal re-solving resumes on the
   next edit.
6. **Justify is frame-rotation-invariant.** Take a face with an arbitrary rotation (e.g. 37°)
   baked into `uv_transform`. Run Justify Left; assert the resulting UV bbox's `u_min == 0`
   regardless of the baked rotation or the frame's `(O, T)`.
7. **Per-loop-UV neighbor wrap.** Construct a face in `UNFOLDED` mode with hand-authored per-loop
   UVs; Wrap *from* it onto a `PROJECTED` neighbor; assert the neighbor's solved `uv_transform`
   reproduces the source's exact per-loop UV values at the two shared vertices.

---

## 6. Open questions for whoever implements LE2

- Grid-mode texel-density-to-scale mapping should read from the same "texel density source"
  config PLAN.md §3 defines for the hotspot fitter (keyed by RID/path) — don't add a second one.
- Whether §4.3 case 2's refit runs every input tick during a drag (expensive, always-correct) or
  only on release (cheap, possibly-stale live preview) is a perf/feel call for the LE0 smoke test,
  not something to settle from first principles here.
- `uv_origin`/`uv_tangent` are new `LevelMeshData` columns beyond PLAN.md §1's sketch — flag this
  in that file's kernel bullet (or a DIVERGENCE-LEDGER-style note) when LE2 lands them, so PLAN.md
  doesn't silently drift out of sync with the real schema.
