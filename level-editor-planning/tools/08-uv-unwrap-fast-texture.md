# Fast Texture Tool (Shift+Q) + the unwrap-mode library

**Status:** planning. **Prereq reading:** `../PLAN.md` — especially §1 kernel schema
(`LevelMeshData` per-face `uv_transform`/per-loop UV), §2 UX contract (modifier grammar, texture
lock ON default), §3 Hotspot system (the fitter's "Automatic (Square → Conforming)" mapping-mode
line reuses this same library), §4 phase LE2 (Fast Texturing) which this spec fleshes out.

**One sentence:** Shift+Q opens a modal 2D overlay over the selected faces; an unwrap-mode
library (Use Existing / Conforming / Square / Follow Quads / Planar) computes the base UV, the
user nudges it with box-transform gizmo tools, and Enter commits one kernel diff — Escape
discards everything, including any live preview.

Local reference used throughout: Cyclops clone (`gui/docks/uv_editor/` + `tools/uv_editor/`) —
a working, if UX-flawed, `Transform2D`-driven 2D sub-editor with box-transform gizmo, sticky-UV
selection, grid/subdiv drawing, and `ConvexVolume.generate_uv_triplanar` (dominant-axis planar
projection). It is mined for mechanism, not adopted (D2) — its per-frame-drag semantics and lack
of a mode library are exactly what this spec improves on.

---

## 1. Storage contract — when a face uses `uv_transform` vs. explicit per-loop UVs

`LevelMeshData` already carries both fields per PLAN §1: per-face `uv_transform : Transform2D`
and per-face-vertex (loop) `uv : Vector2`. This is not redundancy — it is two different
*authorities* for two different kinds of mapping, and every kernel op and the baker must agree on
which one is live for a given face.

**The rule:** loop UV is *always* present and is the only field the baker/runtime ever reads
(D7 — standard `ArrayMesh` output, no `uv_transform` evaluation at runtime). `uv_transform` is an
optional, editor-side **re-projection recipe**: it is present exactly when a face's mapping can be
described by one affine frame (world-space planar projection + offset/scale/rotation), and when
present it is the *source of truth* — every kernel op that moves the face plane (extrude,
push/pull, bevel, vertex drag) re-evaluates loop UV from it immediately (`loop.uv = uv_transform *
face_frame.project(vertex.position)`), which is exactly what "texture lock ON by default" (PLAN
§2) means mechanically. When `uv_transform` is null for a face, loop UV is authored directly and
geometry ops fall back to best-effort UV transfer (barycentric copy on split, translate-with-face
centroid on rigid moves) rather than regeneration — texture lock degrades gracefully instead of
silently corrupting a hand-unfolded island.

| Unwrap mode | `uv_transform` after commit | Loop UV role | Why |
|---|---|---|---|
| **Use Existing (1)** | unchanged (whatever the face already had) | authoritative if no transform | No-op mode — opens the overlay to hand-edit the current state, doesn't recompute. |
| **Square (3)** | set (per-face independent box frame) | derived from transform | A single flat rectangle per face is exactly what `Transform2D` encodes. |
| **Planar (5)** | set (one frame shared by the whole island) | derived from transform | Sharing one world-space frame across an island is *why* neighboring faces stay texture-continuous when one is transformed — the whole point of a planar frame vs. per-face box mapping. |
| **Conforming (2)** | cleared (null) | authoritative, per-loop | Region-growing unfold applies a *different* local rotation to every face in the island (hinge-unfold, §3.1) — that bends UV space non-affinely and cannot be captured by one `Transform2D`. |
| **Follow Quads (4)** | cleared (null) | authoritative, per-loop | Grid-walk spacing (Length/Even/Length-Average, §3.2) assigns UV per grid step, not per one frame. |

**Consequence every other tool must respect** (this is difficulty §3.3, stated here as the
contract, elaborated there): mode switch inside one overlay session is **all-or-nothing** for the
selection — accepting a mode always regenerates *both* fields for *every* selected face, so a
selection that entered the overlay in a mixed state (some faces framed, some not — e.g. after a
boolean merges two islands) never leaves it mixed. Partial-mix state can persist only *between*
overlay sessions, on faces nobody has re-touched yet; every kernel op that reads a face must check
`has_uv_transform` per-face, never assume mesh-wide uniformity.

The box-transform gizmo (§4, ported from Cyclops `ToolUvBoxTransform`) always manipulates one
final composed `Transform2D` layered *on top of* whatever the unwrap mode produced. If the base
was Square/Planar, the nudge folds directly into `uv_transform` (matrix multiply, still one
frame). If the base was Conforming/Follow Quads, there is no slot to fold into — the nudge is
baked directly into every affected loop UV at accept time. Same gizmo code path, different write
target, decided by the same per-face flag.

---

## 2. Foundation services (kernel, headless-testable, no editor dependencies)

These live in `modules/level_kernel` alongside `LevelMesh`'s other operators (PLAN §1), exposed to
GDScript/traits per the "fully scriptable kernel" rule.

- **`FaceFrame` — per-face 3D→2D projection.** An orthonormal in-plane basis (`origin`, `right`,
  `up`) plus the raw projection `project(point: Vector3) -> Vector2`. Three construction
  strategies, selected by mode:
  - *Dominant-axis triplanar* (box/Square mapping): pick the axis `X/Y/Z` most parallel to the
    face normal, project the other two — the Cyclops `generate_uv_triplanar` approach
    (`math/convex_volume.gd:2030`), cheap, discontinuous at ~45° face-normal boundaries (acceptable
    for architectural blockout geometry, not for organic surfaces — a documented non-goal per
    PLAN §6).
  - *World-aligned planar* (Planar mapping, Hammer/Source "world" texture axes): `right =
    normalize(cross(world_up, normal))`, falling back to world-right when `normal` is
    near-parallel to world-up; `up = cross(normal, right)`. One frame shared verbatim across an
    island keeps neighbors seamless under independent per-face transforms.
  - *Align-to-longest-edge* (face mapping / "align to face"): `right` = the longest boundary edge
    direction, `up = cross(normal, right)` — for faces where triplanar/world alignment misaligns
    badly (angled roof faces, splayed fan geometry).
  - Composition: `final_uv(vertex) = uv_transform * face_frame.project(vertex.position)`.
    `uv_transform.affine_inverse()` recovers the raw frame-space point for editing round-trips
    (numpad nudge/justify in the persistent Modify Texture panel, PLAN §2).

- **Unfold-across-edge (hinge) operator.** Pure function, no mesh mutation: given face A with UV
  already assigned on all its loops, a shared edge, and face B (unassigned) — rotate B's plane
  about the shared-edge axis by the dihedral angle between the two face normals until B is
  coplanar with A, then project B's non-shared vertices through A's *established* 2D basis
  (continued, not re-derived); the shared-edge vertices copy A's exact UV values rather than being
  re-projected, so the seam is bit-exact rather than floating-point-close. Returns a UV-value map
  for B's loops; the caller (region growing) decides whether to accept it.

- **Region growing.** BFS across the *selection's* dual graph (faces = nodes, shared manifold
  edges = arcs; selection boundary edges are automatic UV seams — never grown across). Seed choice
  is the lowest stable face id in the selection (deterministic, matches the Hotspot Fitter's
  seeded-determinism requirement, PLAN §3). Each newly visited face is hinge-unfolded relative to
  its BFS parent; an accumulated-distortion metric (chain of dihedral angles from seed) is tracked
  per face so callers (Conforming, and the Hotspot Fitter's "Automatic" mode) can decide when to
  stop growing and cut a new island instead of continuing to unfold. Reused verbatim by
  Follow-Quads' grid-walk (§3.2), which is region growing restricted to quad-only continuations in
  two principal directions instead of an unrestricted BFS.

---

## 3. End-to-end pipeline

1. **Selection.** Existing polygroup/triangle-tier face selection (LE1) — Fast Texture operates on
   whatever is currently selected; no separate selection step.
2. **Shift+Q opens `FastTextureOverlay`.** Snapshots the current UV/`uv_transform` state of every
   selected face (for exact cancel-restore), pushes a scoped shortcut context so `[`/`]`, `F`,
   `Q`/`E`, flips, `B`, `G`, `C`, `T`, `1`–`5`, `Ctrl+1/2/3` resolve inside the overlay and nothing
   leaks to the 3D tool underneath, and opens a working copy of the affected UV data (not yet
   written into `LevelMeshData`).
3. **Unwrap mode selection (1–5) recomputes the working copy.** Picking a mode runs the
   corresponding foundation-service algorithm (§2) against the *original* snapshot (not against
   whatever the previous mode left behind — modes are mutually exclusive per session, not
   compounded) and writes the result into the working copy. The overlay's 2D view and the 3D
   viewport's material preview (when `T`/use-original-material is off) re-render immediately.
4. **Interactive 2D manipulation.** The box-transform gizmo (ported from Cyclops
   `ToolUvBoxTransform` — corner scale/rotate handles, free/axis/uniform scale variants via
   Shift-modifier, a movable pivot, MMB temp-pivot) composes one additional `Transform2D` on top of
   the working copy per §1's rule (folds into `uv_transform` for Square/Planar, bakes into loop UVs
   for Conforming/Follow Quads). Grid resize (`[`/`]`), frame (`F`), rotate (`Q`/`E`), flips,
   repeat-background (`B`), world-scale display (`G`), show-coordinates (`C`) are view/HUD
   affordances on the same working copy — none of them touch `LevelMeshData` until accept.
5. **Accept (Enter) → one kernel diff.** The working copy's UV/`uv_transform` values are written
   into `LevelMeshData`'s columnar arrays for every affected face/loop and immediately resolved
   (loop UV recomputed from `uv_transform` right now, not deferred to bake — §1). This produces a
   single data-diff span, pushed through the document's undo history exactly once per accept, never
   per drag-frame (matches the kernel's diff-based undo contract, PLAN §1). The overlay closes and
   pops its shortcut context.
6. **Cancel (Escape) → discard.** The working copy is dropped, the pre-session snapshot is
   restored verbatim (already-committed state is untouched since nothing was written until step
   5), no diff is emitted, no undo entry is created.
7. **Bake.** The baker (PLAN §1 `LevelMeshBaker`) only ever reads loop UV — it has no knowledge of
   `uv_transform` at all. This is what keeps D7 ("standard output," plain `ArrayMesh`) true: the
   frame recipe is 100% an editor-side convenience, resolved into ordinary UVs the instant any
   kernel op could have invalidated them.

---

## 4. Core difficulties

### 4.1 Conforming unwrap — region-growing unfold with a distortion-driven split rule

**Chosen solution:** BFS region growing (§2) from a deterministic seed face, hinge-unfolding
(§2) each newly visited face relative to its BFS parent, with two termination conditions that
force a new island: (a) accumulated distortion since the seed exceeds a threshold, (b) a
**cycle-closure conflict** — a face reachable from the seed via two different BFS paths (e.g. a
selection that wraps a cylindrical or toroidal cross-section) is visited twice with two candidate
UVs; if they disagree beyond epsilon, that mesh region is not developable within the selection and
the second-arriving path's edge becomes a forced seam instead of silently overwriting the
first-arriving result. Because BFS visitation order is fully determined by the seed rule and a
stable neighbor-iteration order (face-loop order, not spatial), "first path wins" is deterministic
and reproducible — required for the headless determinism check (§5) and consistent with how the
Hotspot Fitter's own seeded-determinism rule works (PLAN §3).

Algorithm sketch:
```
seed = min(face_id for face in selection)
queue = [seed]; visited = {seed}
uv[seed] = FaceFrame.for(seed).project(seed.vertices)   # world-aligned planar seed frame
while queue:
    a = queue.pop()
    for edge in a.shared_edges_within_selection():
        b = edge.other_face(a)
        if b in visited:
            check_cycle_closure(a, b, edge, uv)   # conflict -> cut seam, do not overwrite
            continue
        candidate_uv = hinge_unfold(a, edge, b, uv)         # §2
        distortion[b] = distortion[a] + dihedral_angle(a, b)
        if distortion[b] > SPLIT_THRESHOLD:
            start_new_island(b); continue          # new BFS root, uv_transform cleared for both
        uv[b] = candidate_uv
        visited.add(b); queue.push(b)
```
Selection boundary edges are never grown across — they are automatic seams by construction, so
island shape is entirely a function of the selection, matching "grouped apply" scoping to
coplanar/collinear islands the Hotspot Fitter already assumes (PLAN §3 step 1).

**References:** DreamUV (github.com/leukbaars/DreamUV — MIT, small readable unwrap/hotspot Python,
studied for the shape of a mode-library UI, not the flattening math); Blender manual's Unwrap
algorithms page (Angle Based Flattening vs. Conformal, background for why we deliberately do *not*
implement LSCM — hinge-unfold is cheap, deterministic, and sufficient for near-developable
architectural surfaces, where LSCM's iterative solve buys accuracy we don't need); local Cyclops
`gui/docks/uv_editor/uv_editor.gd` (`rebuild_block_handles`, `ConvexVolume` face/edge/vertex
iteration) for the mesh-walking shape this reuses.

### 4.2 Follow Active Quads on an n-gon mesh

**Chosen solution:** grid-walk region growing (§2, the quad-restricted variant of Conforming's
BFS) from a seed quad in two principal directions. From the seed face, "opposite edge" is the edge
two steps around the loop from the entry edge (standard quad-strip definition); stepping across
that opposite edge onto the next face continues the same logical row (or column); the other two
edges of the seed step into the perpendicular direction. A face only continues a direction if it is
itself a quad (4-sided) **and** the edge crossed to reach it has a well-defined opposite edge on
the far side too — i.e., both faces agree on being part of the same quad ribbon. Any non-quad
neighbor (a tri, pentagon, or n-gon left over from a boolean) terminates that direction of the
walk at that edge; the island is the maximal `(row, col)` grid reachable this way, and faces beyond
the dead end are left completely untouched (existing UV/`uv_transform` unchanged) rather than
falling back to a different mode implicitly — the user re-selects and re-runs a different mode for
the leftover geometry, matching Scythe's explicit-mode-per-run model.

Once every face has a `(row, col)` grid coordinate, each of the three spacing sub-modes computes a
1D parametrization per row and per column independently, then combines them into per-face UV rects:
- **Length:** `u_i = cumulative_sum(edge_length_0..i-1)` in absolute texel-density units (no
  0–1 normalization) — exact real-world spacing, so a tiling trim texture reads at uniform texel
  density along the strip even when individual segments differ in length.
- **Even:** `u_i = i / N` — uniform grid spacing regardless of real length; correct for
  gradient/blend-mask textures where visual continuity matters more than metric accuracy.
- **Length-Average:** `avg = total_length / N; u_i = i * avg` — every step gets the *same* size
  (the strip's mean edge length) rather than its own measured length, smoothing out small
  per-segment irregularities (a slightly uneven wall-panel row) while still tracking the strip's
  aggregate real-world length, unlike Even which ignores length entirely.

**References:** Blender manual, Follow Active Quads (`docs.blender.org` UV mapping section) — the
row/column grid-walk and Length vs. Even spacing distinction this design generalizes to an n-gon
mesh (Blender's version assumes an all-quad mesh; the "terminate at non-quad neighbor" rule here is
this project's addition, needed because the level kernel is an indexed n-gon mesh, not BMesh); Zen
UV and rmKit UV documentation (both surveyed for "quad flow"/UV grid nomenclature and as prior art
for a spacing-mode picker as a persistent sub-tool, not a one-shot operator).

### 4.3 The dual-storage problem — what every other tool must respect, bake ordering

Covered as the storage *contract* in §1; the difficulty is keeping every kernel op honest about it
once other operators (extrude, bevel, mirror, subtract) start touching faces that carry a live
`uv_transform`. **Chosen solution:** push the invariant into the kernel op layer itself rather than
trusting each op's author to remember it — every operator that changes a face's plane or splits a
face calls a single shared `LevelMesh.reconcile_face_uv(face)` post-step before returning its diff:
if `has_uv_transform`, recompute all of that face's loop UVs from `uv_transform * face_frame`
against the *new* geometry (texture lock); if not, and the op split or merged the face, best-effort
transfer old loop UV via barycentric interpolation from the pre-op face shape (never invent new
UVs from thin air, never silently drop to `(0,0)`). This makes "does this op respect texture lock"
a property of one shared function instead of N operator implementations, and keeps LE4/LE5's new
operators (bevel, boolean subtract) automatically correct without their authors re-deriving the
rule.

Bake ordering: the baker never evaluates `uv_transform` (§1, §3 step 7) — by the time a block
reaches `LevelMeshBaker`, every dirty face has already had `reconcile_face_uv` run at the end of
whatever kernel op touched it, so loop UV is always current. This ordering guarantee (reconcile
happens inside the kernel op, not as a separate bake-time pass) is what lets the baker stay a pure
"columnar data → `ArrayMesh` surfaces" function with no knowledge of the texturing model at all —
consistent with D7 and with keeping `modules/level_kernel` the only place that understands
`uv_transform`.

**References:** Source engine face texturing model — `.vmf` texinfo (texture axis U/V + shift/
scale/rotation stored per face, recomputed against current geometry, exactly the `uv_transform`
role) as documented across the Source SDK community wiki and Hammer/Source 2 docs already in the
research base; TrenchBroom manual's Texture Lock section (the behavior this reconcile step
implements, and TrenchBroom's own documented edge cases — e.g. texture lock breaking down across
non-affine ops — for what "graceful degradation" should look like); local Cyclops
`tools/uv_editor/tool_uv_box_transform.gd` (`transform_uvs`/`transform_uvs_command`) for the
shape of "apply a `Transform2D` across a sticky-UV selection and emit one command," reused for the
gizmo's final-nudge step in §1.

### 4.4 The modal overlay editor — a 2D sub-editor living inside a 3D tab

**Chosen solution:** a tool-owned overlay `Control` (`FastTextureOverlay`), not a second
`WorkspacePane`/document. Per `workspace-editor-planning/ARCHITECTURE.md`'s three-layer taxonomy,
this is **view state** (§2 of that doc — "would two panes showing the same document each need
their own copy?" yes: two panes could have independent Fast Texture sessions open on different
face selections of the same level). `LevelEditorView` instantiates it when the tool activates and
frees it on deactivate, exactly like `Node3DEditorView`'s grid/origin decoration lifecycle
(create → reconcile → free-in-dtor). It is emphatically *not* a `ResourceDocument`-style tab the
way the Hotspot Editor is (PLAN §3) — that resource-editing surface is genuinely a separate
document users switch between; Fast Texture is a transient, modal, per-tool-invocation overlay
that should never appear in the tab strip.

Mechanically: the overlay owns its own pan/zoom `Transform2D` (the Cyclops `proj_transform`
pattern — `uv_to_viewport_xform = view_xform * proj_transform`, view_xform flips Y and centers),
renders via `_draw()` (grid + subdiv grid + face polygons + selection, ported near-verbatim from
`uv_editor.gd`'s `draw_grid`/`draw_subdiv_grid`/`draw_uv_mesh`), and while active captures
`_gui_input` for the whole pane (modal — Scythe/Hammer both fully swallow 3D-viewport input during
Fast Texture) through a pushed shortcut context that owns `1`–`5`, `Ctrl+1/2/3`, `Q`/`E`, `[`/`]`,
`F`, `B`, `G`, `C`, `T`, `Enter`/`Escape`, popped on accept/cancel so those keys resolve to the 3D
tool's bindings again immediately after. The 3D viewport keeps rendering behind/beside the overlay
(never hidden) — selection-highlight sync uses the *same* per-world gizmo overlay instances the 3D
tool already draws with (reuse the existing per-scenario `allocate_gizmo_layer` infrastructure
PLAN §1 calls out for picking) rather than a parallel highlight channel, so a face picked/hovered in
the 2D overlay recolors in the live 3D view for free and vice versa.

**References:** Scythe Editor's own Fast Texture Tool description (scytheeditor.com/guide — the
modal-overlay-with-its-own-hotkeys behavior this spec is implementing); local Cyclops `ToolUv`
base class + `view_uv_editor`/`UvEditor : Node2D` (`gui/docks/uv_editor/view_uv_editor.gd`,
`uv_editor.gd`) — the closest working precedent for "a `_gui_input`-driven 2D sub-editor with its
own projection transform embedded inside a 3D-editor plugin," ported into the view-state layer
instead of Cyclops' `EditorPlugin`-owned singleton; `workspace-editor-planning/ARCHITECTURE.md`
itself for the service/view-state/document-state test this decision is scored against.

---

## 5. Headless test ideas (`tools/checks/` in the game repo, or engine-side `modules/level_kernel/tests`)

- **Unwrap determinism.** Same mesh + same face selection + same mode, run twice → byte-identical
  `uv_transform` and loop-UV arrays. Conforming/Follow-Quads must have *zero* randomness (unlike
  the Hotspot Fitter's tie-break) — the deterministic seed rule (lowest face id, stable neighbor
  order) is what this asserts.
- **Hinge-unfold seam continuity.** For a chain of faces unfolded via Conforming across shared
  edges: assert both faces' loop UV at the two shared-edge vertices are bit-exact equal (no seam
  crack — the hinge operator copies rather than re-projects shared-edge UVs, §2), and assert the
  unfolded UV-space edge length equals the original 3D edge length within float epsilon (hinge is
  an isometry per step).
- **Region-growing cycle closure.** Construct a selection whose dual graph has a cycle (e.g. faces
  around a cylindrical cross-section) so BFS reaches one face via two paths with disagreeing
  candidate UVs; assert the algorithm cuts a seam at the second-arriving edge rather than
  overwriting, and assert which path "won" matches the documented first-visited-wins rule.
- **Follow-Quads grid-walk termination.** Build an n-gon mesh where a quad strip dead-ends into a
  triangle or pentagon; assert the walk stops cleanly at that edge (no crash, no infinite loop),
  and assert faces past the dead end retain their pre-op UV/`uv_transform` untouched.
- **Texel-density preservation.** For Planar/Square and Follow-Quads' Length mode, sample known
  world-space edge lengths against the resulting UV-space delta × the atlas texture's pixel
  dimensions; assert the ratio matches the `HotspotAtlas` texel-density target within tolerance
  (ties this mode library directly to PLAN §3's fitter, which shares it).
- **`FaceFrame`/`uv_transform` round-trip.** `uv_transform.affine_inverse() *
  (uv_transform * face_frame.project(v))  ==  face_frame.project(v)` for arbitrary points —
  algebra correctness independent of mesh content.
- **`reconcile_face_uv` texture-lock invariant.** Apply a geometry op (push/pull, bevel) to a face
  with a live `uv_transform`; assert loop UV after the op equals `uv_transform * face_frame` against
  the *post-op* geometry (not the pre-op cached values) — this is the fuzzable "every operator
  respects texture lock" property from §4.3, worth running across every op × having/lacking
  `uv_transform` combination the same way LE5's boolean fuzzing sweeps op × selection-mode (PLAN
  §4 LE5).
- **Overlay accept/cancel diff count.** Drive `FastTextureOverlay`'s scriptable API headlessly
  (no editor UI, per the kernel's "fully scriptable" rule): select faces, pick a mode, accept →
  assert exactly one undo diff was pushed and `LevelMeshData` reflects the working copy; cancel →
  assert zero diffs and the mesh is byte-identical to the pre-session snapshot.
- **Mixed-state regeneration.** Build a selection where some faces already carry a `uv_transform`
  and others don't (simulating a post-boolean merge); run any single mode; assert every selected
  face ends the session in the same state-class (all-transform or all-loop, per §1's mode table) —
  no persisted partial mix.
