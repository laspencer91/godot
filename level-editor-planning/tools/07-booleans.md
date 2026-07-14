# Tool 07 — Booleans (subtract / intersect / union)

**Status:** planning. **Phase:** LE5 (booleans land LAST, hardened — see `PLAN.md` §4).
**Prereq reading:** `PLAN.md` (kernel + document seams, D4), `modules/csg/csg_shape.cpp`
(`_pack_manifold`/`_unpack_manifold` — the single most important local reference),
`thirdparty/manifold/include/manifold/manifold.h` (MeshGL contract),
`thirdparty/manifold/src/shared.h` (`TriRef`).

**One sentence:** user picks the Boolean tool + a target block and one or more tool blocks; the
kernel triangulates each operand's n-gon mesh into a watertight `MeshGL64`, runs one Manifold
`BatchBoolean`, then reconstructs n-gon faces from the result while recovering `material_index`,
`uv_transform`, and polygroup for every surviving surface region and projecting fresh UVs onto the
new cut surfaces — result replaces the inputs as edited block(s).

Hammer analogue: *Subtract* (carve). Scythe analogue: carve-style clip ops (their v0.6 crash class —
`PLAN.md` risk 1). Reject-illegal-input, never corrupt (D4). This is the documented graveyard of
level editors; the whole design is defensive.

---

## 0. Decisions (booleans-specific, extend `PLAN.md` D4)

| # | Decision | Rationale |
|---|----------|-----------|
| B1 | **One-shot op through vendored Manifold `BatchBoolean`, never live CSG.** | Matches D4. Live CSG re-evaluation is the open-bug prototyping path; we run once, validate, commit a data-diff. |
| B2 | **`(runOriginalID, faceID)` is the attribute primary key**, not vertex-property UVs. | Manifold passes `TriRef.faceID` and `.originalID` through booleans *unchanged* (`shared.h:143-145`). Godot's CSG only carries UV+material as interpolated vertex props and loses per-face `uv_transform`/polygroup — we key off face identity instead and recover the full attribute tuple exactly. |
| B3 | **Surviving faces copy source `uv_transform` verbatim; only genuinely-new cut faces get generated UVs.** | `uv_transform` is a *planar* projection defined in the face plane; any surviving sub-region lies in the same plane, so the same transform still maps it correctly. No UV-recovery math for survivors — exact by construction. |
| B4 | **Detect-guard-report open meshes; never auto-close.** | Blocks are closed by construction; an edit that opened one is a bug to surface, not paper over. Auto-capping is a corruption vector. A separate explicit "Cap Boundaries" command exists, but the Boolean op *rejects* non-manifold input. |
| B5 | **Result stays ONE edited block by default (multi-shell allowed); "Split to Blocks" is an explicit follow-up using `Decompose()`.** | Our `LevelMeshData` is just indexed n-gons — disjoint shells are legal. Keeping one block keeps undo one diff; most subtracts want one block anyway. |
| B6 | **Tool-block-originated faces in a Subtract are reassigned to the active material + projected UV.** | The carved cavity walls come from the tool block's surface (they carry the tool's `originalID`); Hammer-carve convention gives cut faces a fresh material. We make that override explicit and controllable. |

---

## 1. End-to-end pipeline

```
LevelMeshData (n-gon, columnar)   ── per operand ──►  [1] validate closed 2-manifold
        │                                                     │ (reject + report if open)
        ▼                                                     ▼
[2] triangulate faces in-plane      ──►  [3] pack MeshGL64:  vertProperties = position only
   (ear-clip, all tris of a face             triVerts (CCW), faceID = source face key,
    share one faceID)                        runOriginalID = per-block reserved ID,
        │                                     mergeFromVert/mergeToVert from shared verts
        ▼                                             │
[4] Manifold(meshGL) → Status()==NoError?  ◄──────────┘   (guard: NotManifold ⇒ abort op)
        │
        ▼
[5] BatchBoolean({A, B...}, OpType)   (Subtract / Intersect / Add)
        │
        ▼
[6] result.Status() ok?  result.NumTri() within budget?  (ResultTooLarge / runaway guard)
        │
        ▼
[7] GetMeshGL64():  triVerts, vertProperties, runIndex/runOriginalID (per-run), faceID (per-tri)
        │
        ▼
[8] group result tris by (runOriginalID, faceID)  ──►  coplanar face groups
        │
        ▼
[9] per group: boundary-trace tri union → n-gon loop(s); drop collinear/T-junction verts
        │
        ▼
[10] classify group:  survivor (key ∈ source table)  →  copy material_index/uv_transform/polygroup
                       tool-cut / new-coplanar        →  active material + world-planar UV projection
        │
        ▼
[11] weld shared verts (mergeTo/From already welded topology) → rebuild LevelMeshData
        │
        ▼
[12] emit data-diff (replace input blocks' spans) → document undo history → bake dirty
```

Stages **[2][8][9][10]** are new kernel services (§2); everything else is thin glue over Manifold and
the existing baker/undo seams.

---

## 2. Foundation services (kernel, headless-testable, no editor deps)

1. **`FaceTriangulator`** — planar n-gon → triangles. Project face loop to 2D using a plane basis
   (dominant-axis drop for speed, full basis for robustness), ear-clip (handles concave + hole loops
   from prior booleans), emit triangles in the face's CCW winding. **Invariant:** every triangle of one
   source face is tagged with that face's `faceID`. Convex blocks fast-path to a fan. Reuses the math
   already needed by the baker; the baker and packer call the same routine so triangulation is
   identical on both sides of the round-trip.

2. **`ManifoldPacker` / `ManifoldUnpacker`** — mirror of `csg_shape.cpp:398` `_pack_manifold` /
   `:280` `_unpack_manifold`, but:
   - `numProp = 3` (position only). We do **not** carry UV/smooth as vertex props (B2/B3) — smaller
     buffers, no interpolation garbage on cut verts.
   - `runOriginalID`: `Manifold::ReserveIDs(1)` per **block** (target = A, each tool = B, C…), stored
     in a `HashMap<uint32, BlockTag>`. (Godot reserves per *material*; we reserve per *block* so we can
     tell target-survivor from tool-cut.)
   - `faceID` per triangle = the source n-gon face index within its block.
   - `mergeFromVert`/`mergeToVert`: built from the mesh's own shared-vertex index (our indexed mesh
     already knows which loop-verts coincide) so Manifold gets watertight topology without float
     compares (`manifold.h:64-73`). Fall back to `MeshGL::Merge()` only as a diagnostic.

3. **`AttributeRoundtripTable`** — forward: `{block_tag → {face_index → (material_index, uv_transform,
   polygroup, smoothing)}}`, captured at pack time. Reverse: `(runOriginalID, faceID) → tuple`.
   Single source of truth for stage [10]. What Godot's CSG solves: material via `runOriginalID`
   (`csg_shape.cpp:294-303`). What we add beyond it: per-face `uv_transform`, polygroup, smoothing,
   and the survivor-vs-new classification.

4. **`CoplanarFaceMerger`** — result-tri group → n-gon. Build an edge-use multiset over the group;
   boundary edges are used exactly once. Trace boundary loops (CCW outer, CW holes), then run a
   collinear-vertex pass with an angular epsilon to delete Manifold's T-junction verts introduced along
   cut lines. **Epsilon policy:** merge tolerance keyed to `manifold.tolerance` (bbox-relative,
   `manifold.h:164-168`), not an absolute constant — this is the crack/T-junction knob.

5. **`PlanarUVProjector`** — new cut faces → `uv_transform`. Per coplanar group: plane normal →
   world-axis-aligned tangent frame (Hammer "World" alignment: pick the axis pair least parallel to the
   normal), scale to the active material's texel-density target, write a `Transform2D`. Deterministic,
   seed-free.

---

## 3. Core difficulties (chosen solution + sketch + refs)

### Difficulty A — Watertightness requirement (Manifold demands closed 2-manifold input)

**Chosen: detect + guard + report; never auto-close (B4).**

Manifold's `Manifold(const MeshGL64&)` constructor sets `Status()` to `Error::NotManifold` when input
half-edges don't pair (`manifold.h:311-325`, `:59-62`). We don't want to *discover* breakage there — we
want a first-class precondition check that produces a good editor message and highlights the offending
edges.

*Sketch:* (1) On each operand, walk the indexed mesh's directed edges; every undirected edge must be
used exactly twice with opposite orientation (closed, orientable 2-manifold). Collect boundary edges
(used once) and non-manifold edges (used >2). (2) If any: **abort the op**, flash the boundary/non-
manifold edges in the viewport, toast "Block ‹name› is not closed (N open edges) — booleans need a
solid." Offer the separate **Cap Boundaries** command as a suggested fix, but do not run it implicitly.
(3) Belt-and-suspenders: after pack, assert `Manifold(mesh).Status()==NoError` before the op; a
mismatch between our check and Manifold's is itself a bug (headless assert, §4). Blocks are closed by
construction (LE0 box topology, LE4 ops reject boundary flips), so this fires only on genuine breakage.

*Refs:* Manifold `MeshGL` manifold-input contract & `Merge()` best-effort caveat
(`manifold.h:57-73,172-186`); Godot's own "empty shape ⇒ not manifold" warning path
(`csg_shape.cpp:1127-1138`, `get_configuration_warnings`); glTF `EXT_mesh_manifold` (why merge vectors
beat float-compare welding).

### Difficulty B — Attribute round-trip (recover material_index / uv_transform / polygroup)

**Chosen: `(runOriginalID, faceID)` primary key + verbatim survivor copy (B2/B3).**

`TriRef` (`shared.h:135-153`) stores `originalID` ("ideal for reapplying properties") and `faceID`
("the original triangle this was part of… pass it along unchanged"), and boolean output preserves both
via `MapTriRef` (`boolean_result.cpp:517-519`). On output, `MeshGL64` exposes `runOriginalID` per run
and `faceID` per triangle (`manifold.h:147,153-158`). So each result triangle answers *which block* and
*which source face* it came from — everything else is a table lookup.

*Sketch:* (1) Pack: reserve one `originalID` per block, set `faceID = source_face_index`, fill
`AttributeRoundtripTable`. (2) Unpack: for each run, `runOriginalID → block_tag`; for each triangle,
`(block_tag, faceID) → tuple`. (3) A surviving face is a subset of an original planar face, so its
`uv_transform` is **copied verbatim** — exact, no fitting (B3). (4) Classification: key present in
target's table ⇒ survivor; key from a *tool* block (or a fresh coplanarID Manifold minted for a
new cut surface) ⇒ send to Difficulty D. **Open spike (must confirm in LE5):** verify `GetMeshGL64()`
actually emits our input `faceID`s unchanged and what ID new cut faces receive; add the assert in §4
before trusting the classifier. Fallback if `faceID` plumbing proves lossy: encode
`block<<20 | face` into a dedicated float property channel (Godot-style, `csg_shape.cpp:437-450`) and
read the majority key per reconstructed face.

*Refs:* `shared.h:135-153` `TriRef`; `manifold.h:85-102,134-158` runs/faceID/OriginalID doc;
Godot `_pack_manifold`/`_unpack_manifold` material-run pattern (`csg_shape.cpp:280-463`).

### Difficulty C — N-gon reconstruction without T-junctions or cracks

**Chosen: group by `(originalID, faceID)`, boundary-trace, collinear-collapse with tolerance-keyed
epsilon (B3, service §2.4).**

A boolean shreds each surviving face into many triangles and injects new verts along cut lines. Naively
keeping triangles bloats the mesh and defeats face-level editing/texturing; naively re-merging risks
cracks (over-merge across a real edge) or T-junctions (under-merge, a vert on one side of an edge but
not the other → z-fighting seams).

*Sketch:* (1) Bucket result tris by `(originalID, faceID)` — guaranteed coplanar & co-facial
(`SameFace`, `shared.h:149-152`). (2) Edge-use multiset over the bucket; boundary = edges used once.
(3) Trace boundary into loops; classify outer (CCW vs face normal) vs hole (CW). (4) Collinear pass:
drop a vertex when its two incident boundary edges are collinear within an angular epsilon derived from
`manifold.tolerance` — this removes T-junction verts Manifold left mid-edge. (5) Keep genuine corners.
(6) If a face ends with hole loops, emit an n-gon-with-holes (our `LevelMeshData` supports multi-loop
faces; the triangulator §2.1 handles them symmetrically). **Crack avoidance:** never merge across a
`(originalID, faceID)` boundary — the key *is* the merge partition, so two truly-different source faces
that happen to be coplanar stay separate (correct: they may carry different materials).

*Refs:* Zhou, Grinspun, Zorin & Panozzo, *Mesh Arrangements for Solid Geometry* (SIGGRAPH 2016) — the
libigl/Blender exact-boolean coplanar-resolution reference; Manifold coplanar-face reconstruction doc
(`manifold.h:100-103`); gradientspace "Mesh Booleans" posts (T-junction/seam discussion); Blender exact
boolean's post-merge dissolve as prior art for step (4).

### Difficulty D — Interface-face UV generation (new cut surfaces)

**Chosen: world-axis-aligned planar projection per coplanar region, texel-density scaled (service
§2.5).**

New cut walls have no source `uv_transform`. Hammer's default for carved faces is a World-aligned
planar projection; we replicate it so cut surfaces look intentional and tile with neighbors.

*Sketch:* Per new-face coplanar group: take the plane normal → choose the world axis pair whose plane is
least parallel to the normal (dominant-axis projection) → build a `Transform2D` mapping world position
to UV at the active material's texel density → assign `material_index = active` (B6). All deterministic
(no random tie-break — that's Hammer's *hotspot* behavior, LE3, not raw booleans). Leaves the surface
immediately re-texturable with the LE2 tools and texture-lock-safe (planar transform survives later
moves).

*Refs:* Hammer/Source 2 face projection & texture-lock docs; `PLAN.md` §3 texel-density fitter (shared
texel-density source); Manifold `CalculateNormals` (`manifold.h:404`) for consistent per-face normals
feeding the projection.

### Difficulty E — Result explosion (a boolean can yield many disjoint shells)

**Chosen: keep one multi-shell block by default; `Decompose()`-backed "Split to Blocks" is explicit
(B5).**

*Sketch:* `Manifold::Decompose()` (`manifold.h:284`) returns the connected components. Default: skip it,
rebuild all shells into one `LevelMeshData` (one undo diff, simplest history). "Split to Blocks" command:
`Decompose()` → one block per component, each a diff, parented under the original's node. **Runaway
guard:** before commit, reject if `result.NumTri()` exceeds a budget (× input tris) or
`Status()==ResultTooLarge` (`manifold.h:324`) — surface "degenerate/near-tangent inputs" rather than
freeze the editor. Empty result (fully-consumed subtract) ⇒ delete the block with an undoable diff, not
a crash.

*Refs:* `manifold.h:284` `Decompose`/`Compose`; `PLAN.md` risk 1 (scope clip/mirror before general
booleans); TrenchBroom subtract-produces-many-brushes UX (why "keep one" is the sane default).

---

## 4. Headless test ideas (`one-more-house/tools/checks/`, seeded-deterministic)

- **`boolean_roundtrip_check.gd`** — build a block, assign distinct `material_index`/`uv_transform`/
  polygroup per face, subtract a smaller block clear of every face interior. Assert: every *untouched*
  face survives with byte-identical `uv_transform`, material, polygroup (Difficulty B/C conservation).
- **`boolean_faceid_spike.gd`** — the B open-spike: assert `GetMeshGL64().faceID` returns our packed
  input IDs unchanged for survivor tris and a distinguishable value for cut tris. Gates trusting the
  classifier; run first.
- **Volume identities** — for random A,B: `vol(A) − vol(A∖B) == vol(A∩B)` and
  `vol(A∪B) == vol(A) + vol(B) − vol(A∩B)`, within `manifold.tolerance` (`Volume()`, `manifold.h:347`).
  Also `Genus()`/`Status()==NoError` on every result (watertight-out assert).
- **Watertight-in guard** — feed a deliberately-opened mesh (delete a face); assert the op is *rejected*
  with the boundary-edge report and the mesh is left untouched (Difficulty A, B4).
- **Fuzz the pathological cases** — seeded random over: coplanar/touching faces (shared wall subtract),
  exactly-tangent (face-on-face), fully-nested (B ⊂ A ⇒ hollow shell + hole loop), B ⊃ A ⇒ empty
  result, and slivers near `tolerance`. Each must either commit a valid manifold or reject cleanly —
  **never crash, never emit non-manifold** (Scythe v0.6 crash class, `PLAN.md` risk 1). Cross every
  `OpType` with every selection tier.
- **Reconstruction sanity** — after subtract, assert no T-junctions (every boundary vertex is shared by
  its neighbors' edges) and tri-count stays within budget (Difficulty C/E).

---

## 5. Scope, sequencing, open items

- **Order within LE5:** ship `Clip`/`Mirror` (plane ops — `SplitByPlane`/`TrimByPlane`,
  `manifold.h:391-393`) first; they're the constrained, lower-risk subset and let the round-trip
  machinery (§2) bake before general N-operand `BatchBoolean`. Scythe's revealed lesson (`PLAN.md`
  risk 3): general booleans took ~1 year solo — respect the data point, keep this post-slice.
- **Preview:** cheap wireframe of the pending op only (D4 / `PLAN.md` §6 non-goal: no live CSG preview).
- **Must-confirm spikes before building the classifier:** (1) `faceID` output plumbing survives
  `BatchBoolean` (§4 `boolean_faceid_spike`); (2) new cut faces' `originalID` assignment (which operand
  Manifold attributes them to) — determines the B6 override rule's exact predicate.
- **Divergence ledger:** the op is pure `modules/level_kernel` + Manifold (already vendored, already
  built for CSG — zero new thirdparty). Only editor-tab wiring touches shared files; log per
  `workspace-editor-planning/DIVERGENCE-LEDGER.md`.
