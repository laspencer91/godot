# Block tool (`Shift+B` Add Block / `Shift+R` Add Rectangle)

**Status:** planning, targets LE0 exit criteria in `../PLAN.md` (block drag-create, grid snap,
undo, save/reload, plays in-game with collision). First tool built against the modal-tool
framework and `modules/level_kernel`; its foundation services are shared by every tool after it
(extrude, clip, prop placement).

References used throughout: Cyclops Level Builder clone (MIT) at
`…/scratchpad/cyclops/godot/addons/cyclops_level_builder/` — `tools/tool_block.gd`,
`math/math_util.gd`, `snapping/snapping_system_grid.gd`, `snapping/snapping_query.gd`;
TrenchBroom (`github.com/TrenchBroom/TrenchBroom`, `common/src/View/CreateSimpleBrushToolController{2D,3D}.cpp`);
Blender's Interactive Add tool (`source/blender/editors/space_view3d/view3d_placement.cc`,
operator `VIEW3D_OT_interactive_add`, docs.blender.org manual "Interactive Add"); O3DE White Box
Gem (`github.com/o3de/o3de`, `Gems/WhiteBox/Code/Editor/EditorWhiteBoxComponentModeCommon.cpp` —
Ctrl+drag-along-normal extrude, the closest analogue to our height stage).

---

## 1. End-to-end operation spec

### 1.1 Input events (viewport-routed, per §"VIEW STATE" in PLAN.md)

| Event | Meaning |
|---|---|
| `Shift+B` / `Shift+R` | Activate tool (`LevelEditor` tool-mode switch; view routes input to it) |
| `LMB down` | Arm the gesture — does not start a drag yet (`PENDING`, §1.2) |
| Motion (button held) | Advance the active stage's preview |
| `Ctrl` at drag start | Flip creation-grid orientation to face the camera (§3.1) |
| `Shift` at gesture start | Surface-snap: raycast for a hit face, orient grid to it (§3.2) |
| `LMB up` | Advance stage: `BASE_DRAG`→`HEIGHT_DRAG` or commit; `HEIGHT_DRAG`→commit |
| `Enter` | Force-commit the current preview immediately |
| `Escape` | Cancel gesture, discard preview, return to `IDLE` — never touches undo |

### 1.2 Tool state machine

```
IDLE ──LMB down──> PENDING ──drag > threshold──> BASE_DRAG
                     │                               │ LMB up, rect has area
                     │ LMB up (click, no drag)       ▼
                     └──> IDLE                   HEIGHT_DRAG ──LMB up / Enter──> COMMIT ──> IDLE
                                                       │ Escape
BASE_DRAG ──Escape──> IDLE                            └──> IDLE (discard, no undo entry)
BASE_DRAG ──LMB up, rect ~zero-area──> PENDING (re-arm, §3.4)
```

`PENDING` mirrors Cyclops's `READY` state (`tool_block.gd:225-234`, drag-start radius check at
line 310): a bare click must not create a degenerate block or eat a click meant for selection.
The radius check is a foundation service (`ToolBase::drag_started(...)`), not per-tool code.

State lives on the C++ tool object (`BlockTool : public LevelTool`, engine-side per D1), one
instance per `LevelEditorView` pane, so two panes can run independent gestures on one document.

### 1.3 Preview (ghost)

Every motion event recomputes `BoxSpec { Vector3 p0, p1, p2; Basis grid_basis; }` and pushes it
to a `RenderingServer` instance on the document's scenario (reusing the per-world gizmo-layer
allocation the picking BVH already needs). No kernel call, no `LevelMeshData`, no undo touched
during the drag (§3.3). Wireframe box + corner markers mirror Cyclops's
`draw_cube`/`draw_line_strip`/`draw_vertices` (`tool_block.gd:145-153`).

### 1.4 Kernel op (commit)

```cpp
Ref<LevelMeshDiff> diff = level_mesh->create_box(bounds_local, grid_basis, material_index, uv_transform);
document->get_undo_history()->push(diff, "Add Block");
```

`create_box` (`modules/level_kernel/level_mesh.cpp`) composes the atomic Euler ops: 8 vertices,
6 quad faces, correct winding, one polygroup. It validates volume and basis orthonormality and
**rejects** (null diff, no-op) rather than throwing — the same contract every kernel op follows.

### 1.5 Undo

The diff is a columnar-array insert (new vertex/edge/face spans + free-list bookkeeping) applied
through the document's existing undo history id — the tool has no undo-specific code beyond the
one `push` call. Redo re-applies the same diff; transient drag state is never serialized.

---

## 2. Foundation services required

1. **Modal tool framework** — `LevelTool` base (activate/deactivate, `_gui_input` routing,
   drag-start-radius helper, `ED_SHORTCUT` registration, options-panel mount into `toolbar_host`).
   Cyclops's `CyclopsTool`/`_gui_input` dispatch is the structural model (portable pattern, code
   itself not ported per D2).
2. **Ray/plane math** — engine-side equivalents of `math_util.gd`: `intersect_plane`,
   `closest_point_on_line`, `snap_to_best_axis_normal`, a `Basis`-from-normal builder
   (`math_util.gd:617-636`). Pure functions, headless-testable in isolation.
3. **Picking service** — per-block BVH ray cast against `LevelMesh` faces (PLAN.md §"Picking"),
   returning hit position/normal/face id/polygroup id. Needed here for surface-snap (§3.2), reused
   by every LE1 selection tool.
4. **Snapping service** — a `SnapQuery` object (camera + modifiers in, snapped point out),
   pluggable grid/vertex/edge sub-snappers behind one interface (mirrors
   `snapping_manager.gd`/`snapping_query.gd`/`snapping_system_grid.gd`). Must support an
   **arbitrary grid transform**, not just the world grid — surface-snap reorients the grid to the
   hit face (`grid_transform : Transform3D`, `snapping_system_grid.gd:36-53`).
5. **Preview/ghost render layer** — a shared `ToolOverlay` helper over `RenderingServer`
   (create-detached → reconcile → free-in-dtor lifecycle already used for grid/origin decoration).
   Every modal tool needs this; build once, not per-tool RID juggling.
6. **Kernel create-box op + diff/undo plumbing** — `LevelMesh::create_box`, `LevelMeshDiff`, and
   the document undo-history push path. First kernel op to exist at all, so it also proves the
   diff format and the baker's dirty-block invalidation.
7. **Bake trigger** — a document-level "dirty blocks changed" signal drives `LevelMeshBaker`
   after commit; the tool's job ends at "push diff," it must not call the baker directly (keeps
   the kernel/bake seam thin).

---

## 3. Core difficulties

### 3.1 Base-rectangle drag plane is camera-dependent

**Why hard.** A 2D drag defines a rectangle, but a rectangle needs a plane, and nothing in the
drag itself says which of infinitely many planes through the click point is meant. Wrong plane
choice means grazing-angle instability or the rectangle silently forming on the wrong world axis.
The plane must be fixed at drag start and held for the whole `BASE_DRAG` stage — never
re-derived per motion event, or the rect jitters whenever the cursor crosses other geometry.

**Chosen solution.** Adopt Cyclops's rule set (`tool_block.gd:120-135`, `BlockAlignment`):
default plane is the authored ground plane (XZ, +Y up, matching the 4 m kit grid) unless (a)
`Shift`-surface-snap found a hit (§3.2 supplies the plane), or (b) an active selection supplies
its own top-face plane. `Ctrl` does not change the plane — it only flips which screen direction
the grid's "toward camera" axis faces, computed once at drag start and frozen. Freezing the
choice at `PENDING`→`BASE_DRAG` is what keeps `intersect_plane` well-conditioned for the whole
gesture.

```
on PENDING -> BASE_DRAG (start_pos, camera):
    if shift_held: plane = surface_snap_plane(camera, start_pos)          # §3.2
    elif match_to_selection: plane = active_block_top_plane()
    else: plane = Plane(WORLD_UP, ground_grid_origin)
    grid_basis = orthonormal_basis_from_normal(plane.normal, camera.forward)
    if ctrl_held: grid_basis = flip_toward_camera(grid_basis, camera.global_position)
    p0 = snap(intersect_plane(ray_o, ray_d, plane), grid_basis)           # frozen for whole drag
on motion in BASE_DRAG:
    p_cur = snap(intersect_plane(ray_o, ray_d, plane), grid_basis)        # same plane, same basis
```

**References.** Cyclops `tools/tool_block.gd:82-135` (`start_block_drag`) and
`math/math_util.gd:39-41` (`intersect_plane`); Blender `view3d_placement.cc`
(`VIEW3D_OT_interactive_add` freezes its base plane from the first-click hit, docs.blender.org
manual "Interactive Add").

### 3.2 Surface-snap orientation — "which grid am I on?"

**Why hard.** `Shift`-snap must reorient the creation grid to the hit face's tangent frame, but
a normal alone under-determines that frame (any rotation about it is valid) — naive code picks
an unstable tangent that spins between adjacent, nearly-coplanar faces (grid flickers near an
edge). And grid-snap increments must still line up with the kit grid once reoriented.

**Chosen solution.** Snap the hit normal to the nearest world axis first
(`snap_to_best_axis_normal`, `math_util.gd:277-283`) whenever within a small angular tolerance —
true for the vast majority of kit surfaces, since the 4 m modular grid is axis-aligned — then
look up tangent/binormal from a fixed axis-keyed table (`math_util.gd:617-636`), never an
arbitrary cross product. Fall back to an OBB-fit tangent frame only for genuinely sloped
surfaces, and disable power-of-two snap in that case (status-line notice) rather than fake an
alignment that isn't real. This matches the project's actual content instead of solving the
general arbitrary-normal problem, and reuses the same table §3.1 needs.

```
surface_snap_plane(camera, screen_pos):
    hit = bvh_raycast(camera.ray(screen_pos))
    if not hit: return ground_plane_fallback()
    n_axis = snap_to_best_axis_normal(hit.normal)
    if angle(hit.normal, n_axis) < AXIS_SNAP_TOLERANCE:      # ~2 deg
        basis = axis_aligned_tangent_binormal(n_axis)         # table lookup, stable, no flicker
        grid_step = kit_grid_step
    else:
        basis = obb_tangent_frame(hit.face_points)             # sloped-surface fallback
        grid_step = null                                       # snap disabled, notice shown
    return Plane(n_axis, hit.position), basis, grid_step
```

**References.** Cyclops `math/math_util.gd:277-291` (`snap_to_best_axis_normal`) and `:617-636`
(axis-keyed tangent table); TrenchBroom resolves face-drag planes the same way — snap the picked
face's normal to a canonical axis before building a UV/drag frame
(`common/src/View/CreateSimpleBrushToolController3D.cpp`).

### 3.3 Preview must never pollute undo history or the live document

**Why hard.** The highest-blast-radius bug class PLAN.md's own risk list names ("derived-state
desync … Cyclops crash class"): if the ghost box is even a transient kernel mutation, every
mouse-move during `BASE_DRAG` becomes an undo-able edit unless explicitly suppressed, multi-pane
use races two previews against one document, and `Escape` becomes "undo N times" instead of
"discard." Overlay RIDs can also leak if cleanup isn't unconditional on every exit path (commit,
cancel, and tool deactivation mid-drag alike).

**Chosen solution.** Preview lives entirely in `ToolOverlay` (RenderingServer instances on the
document's scenario) for the whole drag; the kernel is touched exactly once, at commit. Stronger
than an "undo-then-suppress" pattern: a tool architecturally incapable of calling the kernel
before commit cannot cause this bug class at all, which is cheaper than auditing every call site.
Cleanup (`overlay.clear()`) lives in one place — `exit_gesture()` — invoked identically from
commit, `Escape`, and `_deactivate()`, never duplicated per-transition.

```
enter_gesture(): overlay = ToolOverlay.create(document.scenario)
on motion: overlay.update_box(p0, p1, p2, grid_basis)                # no kernel call, ever
on commit: diff = level_mesh.create_box(...); undo_history.push(diff); exit_gesture()
on Escape / _deactivate() mid-drag: exit_gesture()                    # no undo call
exit_gesture(): overlay.clear(); state = IDLE
```

**References.** PLAN.md §"KERNEL" ("selection changes are never undo steps," "kernel stays
undo-agnostic … it emits diffs, the editor owns the stack") and §"Risks" item 2; Cyclops
`tools/tool_block.gd:137-153` (`_draw_tool`, draw-only reuse of drag state); O3DE White Box's
manipulator-preview vs. committed-mesh split (`EditorWhiteBoxComponentModeCommon.cpp`) — the
Ctrl-drag extrude preview is a manipulator overlay, the mesh mutates only on mouse-up.

### 3.4 Degenerate drags — zero-area rectangle, zero height, snap-collapsed corners

**Why hard.** Three degeneracies compound: (a) a drag that snaps two distinct corners into the
same grid cell, zeroing the rect after snapping even though raw mouse motion was nonzero; (b) a
`BASE_DRAG`→commit with no height-stage motion; (c) near-zero floating-point slivers that pass a
naive `> 0` check but produce an unusable box and a degenerate face normal in the baker. The
kernel's own reject-illegal-topology contract is the correct final backstop, but a silent no-op
with no feedback is bad UX if the tool doesn't pre-empt it.

**Chosen solution.** Two tool-layer guards, both cheaper than a kernel round-trip: (1)
`AABB.has_volume()` on snapped bounds before ever calling `create_box`, matching Cyclops's
commit-time guard (`tool_block.gd:164`); (2) a `default_block_height` fallback when the user
releases without a height-drag — the flat click-drag-release gesture should produce *a* box, not
nothing, since that's the common "one grid cell tall" case. Snap-collapsed corners re-arm
`PENDING` (not silent discard, not a bogus commit) so the user can simply redo the drag.

```
on LMB up in BASE_DRAG:
    if not AABB.from_points(p0, p1_snapped).has_volume():
        state = PENDING; return                       # snap collapsed it; re-arm, don't commit
    if angle_between(view_dir, plane_normal) < FLAT_VIEW_EPSILON:
        commit_box(p0, p1_snapped, p1_snapped + plane_normal * settings.default_block_height)
    else:
        state = HEIGHT_DRAG
on LMB up / Enter in HEIGHT_DRAG:
    if not AABB.from_points(p0, p1, p2_snapped).has_volume():
        return                                          # stay in HEIGHT_DRAG; no sliver commit
    commit_box(p0, p1, p2_snapped)
```

**References.** Cyclops `tools/tool_block.gd:160-164` (`bounds.has_volume()` guard) and
`:236-256` (flat-view-angle → default-height shortcut); PLAN.md §"KERNEL" (every op "rejects
illegal topology … rather than attempting it" — the tool guard is a latency optimization on top
of that contract, not a replacement for it).

### 3.5 View-axis-dependent height dragging

**Why hard.** `HEIGHT_DRAG` projects the mouse ray onto a line (the base-plane normal through the
drag-start point) via `closest_point_on_line`. That projection is well-conditioned only when the
view ray isn't nearly parallel to the line; as the camera looks closer to straight down (or up)
the height axis, `ray_dir.cross(line_dir)` shrinks toward zero and the solved point becomes
numerically unstable — tiny mouse movements produce huge height swings right when a player would
naturally look top-down to place a floor-to-ceiling block. Distinct from §3.1 (plane selection)
and §3.4 (degenerate *result*, not unstable *input math*).

**Chosen solution.** Reuse the angle test that already resolves §3.4's flat-click case
(`angle_with_base` vs `drag_angle_limit`), applied symmetrically at both ends: within
`drag_angle_limit` of 0 or of π, skip the live line-projection and fall back to
`default_block_height` on release, rather than attempting a regularized solve that would still
feel erratic even if numerically finite. One angle-threshold constant now governs both stages,
and it turns "the math blows up" into an accepted product limit ("use the default and nudge/type
afterward" — LE1 adds numeric entry anyway).

```
on motion in HEIGHT_DRAG:
    angle = acos(clamp(dot(camera.project_ray_normal(mouse_pos), height_axis), -1, 1))
    if angle < drag_angle_limit or angle > PI - drag_angle_limit:
        preview_height = settings.default_block_height    # frozen; ignore further motion
        overlay.mark_height_locked(true)
    else:
        p_cur = snap(closest_point_on_line(ray_o, ray_d, p1, height_axis), grid_basis)
        preview_height = (p_cur - p1).dot(height_axis)
        overlay.mark_height_locked(false)
```

**References.** Cyclops `tools/tool_block.gd:239-256` (`angle_with_base`, `drag_angle_limit`)
and `math/math_util.gd:85-88` (`closest_point_on_line`, the projection that degenerates); Blender
`view3d_placement.cc`'s interactive-add height stage similarly locks/disables free extrusion at
grazing view angles (docs.blender.org manual, "Interactive Add") — a converged-upon mitigation,
not a one-off hack.

---

## 4. Headless test ideas (kernel scriptable, deterministic)

Drive `LevelMesh`/`BlockTool`'s pure-logic pieces directly (traits/GDScript-exposed per PLAN.md
§"KERNEL"), living in `one-more-house/tools/checks/level_kernel_check.gd` and a
`block_tool_check.gd` once the state machine is scriptable:

1. **`create_box` topology assertion** — known AABB + identity basis; assert 8 vertices, 6
   quad faces, correct winding/material/`uv_transform`, and diff round-trips through
   serialize/deserialize/re-apply idempotently.
2. **Degenerate-input rejection matrix** — zero-volume AABB, non-orthonormal basis, NaN/Inf
   corners; assert null diff and unchanged vertex/face counts (no partial mutation) — the
   kernel-side backstop for §3.4/§3.5, testable with zero input events.
3. **Undo/redo round-trip** — N `create_box` calls with distinct bounds, undo all, assert mesh
   matches initial state, redo all, assert byte-for-byte match with pre-undo state (covers PLAN.md
   risk 5 in single-pane form).
4. **Angle-threshold sweep** — matrix of `(view_dir, height_axis)` pairs across 0..π; assert the
   height-lock branch fires exactly within `[0, drag_angle_limit] ∪ [π-drag_angle_limit, π]` and
   `preview_height` stays finite through the boundary (§3.5, no viewport needed).
5. **Surface-snap axis classifier** — synthetic face normals at fine angular steps around each
   axis; assert the axis-aligned tangent table is chosen exactly inside `AXIS_SNAP_TOLERANCE`
   with no orientation flicker for normals 0.01° apart across the boundary (§3.2 regression test).
6. **Fuzzed random-bounds stress** — seeded RNG over thousands of `(p0, p1, p2)` triples,
   weighted toward near-degenerate cases (near-zero extents, exact grid boundaries, large
   world-offset precision limits); assert no crash and every result is either a valid 6-face box
   or a clean null-diff rejection — the same fuzz pattern PLAN.md prescribes for LE5 booleans,
   applied here first since this is the first kernel-op consumer.
