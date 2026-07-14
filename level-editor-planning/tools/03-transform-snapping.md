# Transform tool + snapping service

**Status:** planning. **Prereq:** `level-editor-planning/PLAN.md` (§1 architecture, §2 UX contract,
D3/D5). This is the Scythe-class Move/Rotate/Scale tool operating on every selection granularity
the kernel exposes, plus the shared snapping service every other tool (block-create, kit placement,
vertices-to-grid, hotspot fitter's manual nudge) calls through.

Research base (July 2026): TrenchBroom 2026.1 manual (grid + vertex snapping, rotate-tool center
handle), Blender manual (pivot-point modes, "Absolute Grid Snap" vs. relative increment, "Align
Rotation to Target"), UE5 forum reports of Local-mode grid-snap producing off-axis results, Source 2
/ Hammer docs on Texture Lock, the fork's `editor/scene/3d/node_3d_editor_viewport.cpp` transform-
gizmo code (hit test, drag-plane construction, `_compute_transform`, the `_edit` state machine),
and the Cyclops Level Builder snapping stack (`SnappingManager`/`SnappingQuery`/
`CyclopsSnappingSystem`, MIT, cloned + reviewed) including `ConvexVolume.transform_uvs`.

---

## 1. End-to-end spec

One state machine per pane, owned by a new `TransformTool` (engine-side C++), generalizing the
fork's `Node3DEditorViewport::_edit` machine from "list of `Node3D*`" to "a `SelectionSet` at any
tier." Six stages, bookended by mouse-down/mouse-up:

**0 — Begin (mouse-down on a gizmo handle).** `TransformTool::begin(mode, plane, selection)`
snapshots the `SelectionFrame` (pivot + basis, §3.1) computed once at grab time, the pre-transform
positions (`original`/`original_local`, stock naming), and opens `LevelMesh::begin_transform_preview()`.
No undo step yet — undo steps are for commits, not drags (TrenchBroom/Cyclops lesson).

**1 — Gizmo hit test.** `hit_test(screen_pos) -> TransformPlane` reuses
`_transform_gizmo_select`'s screen-space handle geometry unchanged — the gizmo mesh doesn't care
what it drives. What changes is what `TRANSFORM_X_AXIS` etc. *means* downstream: the axis basis
comes from the current `SelectionFrame`, not `spatial_editor->get_gizmo_transform()`.

**2 — Drag-plane construction (per frame).** Identical math to stock `update_transform`'s
per-`TransformPlane` switch: single axis → `Plane(axis.cross(axis.cross(cam_normal)), center)`
(view-tilted plane containing the axis); dual axis → `Plane(third_axis, center)`; view/free →
`Plane(cam_normal, center)`. `motion = plane∩current_ray − plane∩click_ray`, masked onto the
constrained axis/axes.

**3 — Snap the motion, not the point.** `motion = SnapService::snap_delta(motion, ctx)` — §3.2
covers why the delta is snapped, never the absolute point or per-vertex. Angle/scale-factor
snapping go through parallel entry points at the same call site.

**4 — Kernel move/rotate/scale diff (preview, per frame).** Stock only ever moves `Node3D`
transforms; this is the generalization. Object tier: unchanged, `set_global_transform` on
`LevelBlock` nodes. Vertex/Edge/Face/Polygroup tier: compute one rigid-or-affine `Transform3D` from
`(mode, motion, extra, pivot, basis)` — same shape as stock `_compute_transform`'s three branches —
and call `LevelMesh::preview_transform_vertices(ids, xform, pivot)`. The kernel applies `xform`
about `pivot` to the selected vertices' columnar positions and recomputes affected face
planes/normals. Preview: no undo journal entry, no full `LevelMeshBaker` rebuild — only a
vertex-buffer-only push to existing bake surfaces (collision/BVH stay stale until commit; the
drag itself doesn't need accurate collision).

**5 — Texture-lock compensation (same kernel call, per dirty face).** Before stage 4 mutates a
face's vertices, snapshot its current derived loop UVs. After the motion, every touched face with
`texture_lock` on gets a new `uv_transform` solved so derived UVs are unchanged (§3.4). Faces with
the flag off are untouched — that flag *is* the entire difference between "texture rides the face"
and "texture stays anchored to the default world-aligned projection" (§3.4); no separate code path.

**6 — Commit (mouse-up).** `commit()` → `LevelMesh::commit_transform_preview()` diffs preview vs.
stage-0 snapshot, returns one `LevelMeshDiff` (vertex positions, `uv_transform`s, normals/planes) —
a single undo step regardless of frame count, then triggers the real baker pass + BVH/collision
rebuild for dirty blocks. `Escape`/right-click calls `cancel_transform_preview()` instead, restoring
the snapshot with no diff.

**Auxiliary entries** (skip 0–3, start at 4): `Alt+Arrows` nudge (motion = grid step along the
arrow's screen-projected axis, instant commit); **Vertices-To-Grid** (per vertex, `motion =
snap_point(v) − v`, N single-vertex translations in one kernel call, one commit); **CapsLock
drag-alignment** (§3.3) replaces stages 2–3 with a raycast-derived transform but still flows
through 4–6 unchanged.

---

## 2. Foundation services required

| Service | Owner | Notes |
|---|---|---|
| `TransformTool` | `LevelEditorView` (per pane) | C++; generalizes `Node3DEditorViewport`'s edit machine over `SelectionSet`. |
| `SnapService` | `LevelEditor` singleton | Owns grid step (power-of-two + 4 m preset), angle step, vertex/edge toggles, global on/off. Scriptable for headless checks. |
| `SnapContext` | ephemeral, per call | Camera (view-dependent vertex-snap radius), `exclude_face_ids`/`exclude_block_ids` (§3.3), pivot/basis. Direct port of Cyclops's `SnappingQuery`, generalized block-id → face-id since our selection can be sub-object of one block. |
| `SelectionFrame` | stateless utility | `centroid()`, `active_element_origin()`, `basis_for(Local\|World)`. Local resolves to the *owning block's* basis for sub-object tiers (a lone vertex borrows its parent's). |
| `PivotState` | `LevelEditorView` (per pane) | Ephemeral MMB temp-pivot override; reset by the *document's* selection-changed signal (§3.1). |
| Kernel preview/commit lifecycle | `LevelMesh` | `begin_transform_preview()` / `preview_transform_vertices(ids, xform, pivot)` / `commit_transform_preview() -> LevelMeshDiff` / `cancel_transform_preview()`. Kernel stays undo-agnostic; the diff is handed to the document's undo history by the editor layer. |
| Partial-bake preview path | `LevelMeshBaker` | Vertex-buffer-only update during preview vs. full dirty-block rebake + collision/BVH rebuild on commit — without this split every drag frame re-trimeshes collision. |
| Face tangent-basis utility | kernel math | `(origin, u, v) = face_canonical_basis(face)` — shared by the hotspot fitter's per-island OBB (PLAN.md §3) and texture-lock (§3.4). One implementation, two callers. |
| Raycast/BVH query + exclusion | reuses per-block BVH | `DynamicBVH` via `insert/update/remove_gizmo_bvh_node` (`node_3d_editor_plugin.h`), scoped per-`World3D` via `allocate/free_gizmo_layer` + the world-scoping fix (`630785ab`). PLAN.md's Picking already designates this for click-selection; §3.3 is a second consumer needing exclusion. |

---

## 3. Core difficulties

### 3.1 Gizmo placement/orientation for sub-object selections + pivot/axis + temp-pivot lifecycle

**Problem.** A selection can be one vertex, a scattered multi-face selection across several
blocks, or mixed tiers via polygroup expansion. Object tier gets a pivot+basis for free from its
`Transform3D`; Vertex/Edge/Face tiers have no inherent basis, and rotate/scale need a well-defined
axis set, not just a point.

**Chosen solution.** Blender's pivot-point taxonomy, restricted to what's needed without a mode
dropdown (Scythe has none): **Median/centroid** (default — average of selected vertices, tier-
agnostic), **Active element** (last-clicked vertex/edge/face — needed because a symmetric
selection's centroid often sits *on* a symmetry plane, exactly where you don't want to rotate
about), and a **temp pivot** overriding both. Basis: World by default; Local resolves to the
*owning block's* basis for sub-object tiers; multi-block sub-object selections fall back to World
(documented, never silently wrong — see the UE5 lesson in §3.2).

Temp-pivot lifecycle (MMB-drag): `PivotState::relocate(point)` sets the override; cleared only by
the *document's* selection-changed signal — not tool switch, not drag end. A pivot set for one
drag should survive a whole multi-step workflow (move, then rotate about the same corner) until
you actually pick something else.

**Algorithm sketch.**
```
SelectionFrame.centroid(sel):
    if pivot_state.has_override(): return pivot_state.override_point
    if sel.active_element_valid() and pivot_mode == ACTIVE: return position_of(sel.active_element)
    return mean(position_of(v) for v in sel.resolve_to_vertices())

SelectionFrame.basis_for(World|Local, sel):
    if World: return Basis.IDENTITY
    if sel.tier == Object: return sel.single_transform().basis        # existing stock path
    blocks = sel.distinct_owning_blocks()
    return blocks[0].global_transform.basis if blocks.size()==1 else Basis.IDENTITY  # documented fallback
```
Rotate/scale reuse `_compute_transform`'s formulas verbatim once `pivot`/`basis` come from here
instead of `spatial_editor->get_gizmo_transform()`.

**References:** Blender "Pivot Point" (Median/Individual/Active/3D Cursor) —
https://docs.blender.org/manual/en/2.80/scene_layout/object/editing/transform/control/pivot_point/index.html
· TrenchBroom rotate-tool center handle (draggable, persists, live coordinates) —
https://trenchbroom.github.io/manual/latest/

---

### 3.2 Snapping semantics that are never state-dependent

**Problem.** "Snap" can mean the grabbed point, the motion delta, or each vertex's final position
independently — picking differently per context is the UE5 failure mode: grid snap rounds
world-axis position components, but Local-mode drag direction follows the object's rotated local
axes, so a drag constrained to one local axis snaps against a grid it isn't aligned to, producing
skewed final positions. The bug is that *what gets snapped* silently changes with a UI toggle
(Local/World) that has nothing to do with snapping.

**Chosen solution.** Always snap the **motion delta**, in world space, period — exactly what the
fork's stock `update_transform` already does (`motion.snapf(get_translate_snap())` before
`apply_transform`), so LE0 only needs generalizing this to kernel-vertex motion, not reinventing
it. This is Blender's default Relative/Incremental snap: an imaginary grid re-seeded at the
selection's starting position, moving off-grid geometry in clean increments without forcing
absolute realignment mid-drag. Layer TrenchBroom's second rule on top for **Vertices-To-Grid**
only (a separate command): snap absolute vertex positions to the nearest grid plane — the two
rules coexist because they're bound to different commands, never a hidden mode flag on the same
one. Grid frame is always World, fully decoupled from the Local/World *basis* choice in §3.1 — the
one-line fix for the UE5 bug class: snap-grid frame and gizmo drag-axis frame are independent,
never coupled.

**Algorithm sketch.**
```
SnapService.snap_delta(motion, ctx):
    if !enabled: return motion
    return motion.snapf(active_grid_step())     # world axes, regardless of gizmo Local/World mode

SnapService.snap_point_absolute(point, ctx):    # Vertices-To-Grid only
    if !enabled: return point
    return point.snapped(Vector3.ONE * active_grid_step())
```
Headless-testable invariant: `snap_delta`'s output depends only on `motion` and grid step — never
on `ctx.basis` or selection rotation.

**References:** TrenchBroom's dual rule (distance-snapped delta + grid-plane vertex snap) —
https://trenchbroom.github.io/manual/latest/ · Blender Relative/Incremental vs. Absolute Grid Snap
— https://docs.blender.org/manual/en/4.0/editors/3dview/controls/snapping.html · UE5 Local-mode +
grid-snap producing unexpected/off-grid results —
https://forums.unrealengine.com/t/static-meshes-keep-snapping-on-the-grid-where-i-dont-want-it-to-go/482725

---

### 3.3 Drag-alignment (CapsLock + gizmo-drag)

**Problem.** Flush-mounting a dragged face selection onto another surface needs a per-frame
raycast under the cursor during the drag — against the same per-block BVH the dragged geometry
lives in. The dragged geometry's *preview* position (already moved this frame, stage 4) can
self-intersect the ray, aligning the selection onto itself instead of the target surface.

**Chosen solution.** Exclude the dragged selection's own faces (not just owning blocks — one block
can hold both dragged and static faces) from the raycast, via `SnapContext.exclude_face_ids`
populated once at stage 0 and reused every frame — the same shape as Cyclops's
`SnappingQuery.exclude_blocks`, generalized to face-id granularity. On a hit: align the selection's
reference normal (active-face normal, §3.1) to the hit surface's normal, and translate the
reference point onto the hit point — both recomputed fresh from the current raycast every frame
(no accumulation), so releasing CapsLock or committing never leaves drift. This ports Blender's
"Align Rotation to Target" face-snap mode, which likewise only activates while Snap-To-Face is
hitting a valid target; otherwise stage 2/3's normal drag-plane math continues uninterrupted.

**Algorithm sketch.**
```
on mouse_move while caps_lock_held and dragging:
    hit = bvh_raycast(camera_ray(cursor), exclude_face_ids = drag_selection.face_ids)
    if !hit: fall through to stage 2/3 plane-drag motion
    rot = Basis.from_to(selection.active_face().normal, hit.face.normal)   # shortest-arc about pivot
    xform = Transform3D(rot, hit.point - rot.xform(selection.reference_point() - pivot) - pivot)
    preview_transform_vertices(selection.vertex_ids, xform, pivot)          # stage 4, unchanged
```

**References:** Blender "Align Rotation to Target" (Snap To: Face) —
https://docs.blender.org/manual/en/3.4/editors/3dview/controls/snapping.html (corroborated:
https://blenderartists.org/t/understanding-of-snapping-with-align-rotation-to-target/1557317) ·
Cyclops `SnappingQuery.exclude_blocks` — `cyclops_level_builder/snapping/snapping_query.gd`, the
exclusion-set shape being generalized here.

---

### 3.4 Texture-lock math — exact affine solve

**Problem.** PLAN.md stores a per-face `uv_transform : Transform2D` and wants "UVs survive
geometry transforms" (lock ON by default). Confirmed Source/Hammer behavior: **lock ON** = texture
rides rigidly with the face; **lock OFF** = texture stays anchored to a fixed world-space
projection and appears to slide across the face as it moves, because Hammer's no-lock default is a
world-axis-aligned planar projection, not a face-relative one. Cyclops's own
`ConvexVolume.transform_uvs` (reviewed) compensates via a **dominant-world-axis** projection —
correct for axis-aligned box faces, but visibly stretches/shears UVs on faces diagonal to all three
world axes, since that basis isn't actually the face's plane.

**Chosen solution.** Use the face's own tangent-plane basis, not a world-axis approximation:

- **Canonical basis** `(O, U, V)`: `O` = face centroid, `U` = normalized first boundary edge,
  `V = normal × U`. Shared with the hotspot fitter's per-island OBB (PLAN.md §3) — one
  implementation, no divergence.
- **Canonical local coords:** `p(v) = ((v−O)·U, (v−O)·V)` — orthographic projection onto the
  face's own plane, world units.
- **Derived UV:** `uv(v) = uv_transform * p(v)`. Lock OFF is simply: `uv_transform` never changes,
  but `(O,U,V)` are recomputed from new vertex positions every op, so the projection re-seats in
  world space and the texture visibly swims — Hammer's no-lock behavior, for free.
- **Lock ON:** before mutating a locked face's vertices, snapshot `uv_i = uv_transform_old *
  p_old(v_i)` for every loop vertex. After motion (re-fitting a best plane via Newell's method
  first if the op left the face non-planar), solve `uv_transform_new` such that
  `uv_transform_new * p_new(v_i') ≈ uv_i`, preserving the old derived UVs.
  - **Rigid/similarity case** (whole-face translate/rotate/uniform-scale, the common gizmo-drag
    case): exact closed form, O(1) per face — the 2D similarity `D` mapping old basis to new basis
    is derivable directly from how `(O,U,V)` moved; `uv_transform_new = uv_transform_old * D⁻¹`.
  - **General case** (individually-dragged vertices, ops touching a subset of a face's vertices):
    solve the 2×3 affine `T` minimizing `Σᵢ‖T·p_new(v_i′) − uv_i‖²` (linear least squares, closed
    form via normal equations, needs ≥3 non-collinear loop vertices; degenerate faces fall back to
    offset-only). Generalizes Blender's "Correct Face Attributes" (per-vertex, connectivity-aware)
    to a single face-wide affine fit — appropriate since our faces are planar n-gons authored as
    one texture-space unit.
- Per-loop UV is a derived/cached value the baker recomputes at bake time, not authoritative data
  — except "Follow Active Quads" mode (PLAN.md §3), where this whole recompute is skipped by
  design.

**References:** Hammer/Source Texture Lock semantics — https://twhl.info/wiki/page/Texture_Lock ·
Cyclops `ConvexVolume.transform_uvs`/`translate_face_plane(lock_uvs)` —
`cyclops_level_builder/math/convex_volume.gd` (~1229–1340), read as the cautionary example
(dominant-world-axis, not face-tangent) to *not* copy verbatim · Blender "Correct Face Attributes"
— https://developer.blender.org/T78416, the closest prior art, generalized to a face-affine solve.

---

## 4. Headless test ideas

Runnable via `godot.windows.editor.x86_64.console.exe --headless --path . --script
tools/checks/<check>.gd` in the game repo, matching the existing `tools/checks/` pattern:

1. **Snap-delta frame-independence** (§3.2): identical `motion` through `snap_delta` with World and
   several non-axis-aligned Local bases → bit-identical output. Direct regression test for the UE5
   bug class.
2. **Grid-step round-trip:** every power-of-two level + the 4 m preset: `snap(snap(p)) == snap(p)`,
   and N composed `snap_delta` calls along one axis sum to exactly `N * step` (no drag-frame drift).
3. **Rigid texture-lock exactness:** n-gon face (3–8 verts, non-axis-aligned normal), random rigid
   transform with lock ON → every loop UV unchanged to epsilon. Must include a deliberately diagonal
   face (regression test for the Cyclops axial-stretch class — an axis-projected implementation
   fails this case, a face-tangent one passes).
4. **General-case texture-lock stability:** non-uniform per-vertex perturbation on a locked face →
   least-squares residual stays under tolerance for mild deformation, solve degrades gracefully
   (no NaN/singular) near collinear/degenerate.
5. **Pivot lifecycle:** set a temp pivot directly, run two preview/commit cycles with no selection
   change → pivot unchanged; fire the document's selection-changed signal → next `centroid()` call
   falls back to median.
6. **Preview/commit undo granularity:** `begin` → N `preview_transform_vertices` calls → `commit` →
   exactly one `LevelMeshDiff`; undo restores pre-drag positions and `uv_transform`s byte-identical,
   regardless of N.
7. **Cancel restores exactly:** same drive as #6 but `cancel_transform_preview()` → live arrays match
   the pre-`begin` snapshot exactly, no undo step pushed.
8. **Drag-align exclusion:** dragged face's preview position overlaps the ray before the alignment
   target → excluded query hits the far target, non-excluded query self-hits (proves the exclusion
   fixes a real, reproducible bug).
9. **Multi-block Local-frame fallback:** selection spanning two differently-rotated blocks,
   `basis_for(Local)` → `Basis.IDENTITY`, never either block's basis or an averaged one.
10. **Vertices-To-Grid vs. live-drag separation:** a live translate-drag through `snap_delta` stays
    off-grid (`start + N*step`); the Vertices-To-Grid command on the same selection lands every
    vertex on an absolute grid plane — proves §3.2's two rules are command-bound, not a shared toggle.

---

## 5. Open questions (non-blocking for LE0/LE1, revisit at LE1 exit)

- Blender's "Individual Origins" pivot mode (each object rotates about its own center) for
  Object-tier multi-block rotation — deferred until LE6 kit-placement; today's shared-pivot
  `_compute_transform` path already covers the common case.
- Angle-snap step presets (5°/15°/45°) vs. freeform field, and a distortion-warning overlay when
  the general-case texture-lock solve's residual exceeds a threshold — both UX polish for the LE2
  Modify Texture panel, not architectural difficulties.
