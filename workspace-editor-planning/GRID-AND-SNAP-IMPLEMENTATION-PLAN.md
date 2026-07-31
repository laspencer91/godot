# First-Class 3D Grid and Snap Tooling - Revised Implementation Plan

Status: implemented on `fork-master` in dependency order on 2026-07-31; focused and full CSG regression suites are green. The production editor object graph compiles, and a test-enabled dev editor has been rebuilt and linked.

This document covers the 3D editor grid, translate-snap presentation, per-viewport isolation, generic edit-domain work grids, and CSG integration. It is retained as the implementation and acceptance record for the landed feature.

Implementation note: `origin/archive/document-grid-phase0-1a04382f05` contains the original Phase 0 math and tests. It is not merged into current `master` and is superseded where this revision changes LOD bounds, orthographic projected-density calculation, rebuild-key stability, and invalid-setting behavior. Reuse it as a starting point only; do not cherry-pick it as an accepted implementation without those corrections.

## 1. Executive verdict

The grid problem is real and is larger than a missing toolbar label:

- `Node3DEditorViewport::_process()` calls `Node3DEditor::update_grid()`, which always forwards to `main_view`.
- `Node3DEditorView::update_grid()` and `_init_grid()` always read viewport 0's camera.
- Each `Node3DEditorView` owns grid RIDs, but every grid instance uses shared cull bit 25 in the document's scenario. Cameras showing the same `World3D` can therefore render another view's grid.
- `MENU_VIEW_GRID` mutates `main_view`, while each viewport's `VIEW_GRID` independently toggles the camera's bit 25. The two controls are not aliases of one state.
- `grid_size` is not the snap step. It is effectively a half-extent measured in generated grid cells, while visible cell spacing is selected by an unrelated camera-distance LOD.
- `Node3DEditor::get_translate_snap()` polls Shift globally, and `is_snap_enabled()` includes a globally polled Ctrl inversion. CSG uses the same modifiers for extrusion and cut.
- CSG declares Local, Root, and World snap spaces, but only Local is implemented and no UI changes the enum.

The correct foundation is:

1. One checked private editor-layer lease per visible `Node3DEditorViewport`.
2. One `EditorGrid3DRenderer` per `Node3DEditorViewport`, using that same lease.
3. One global snap profile on `Node3DEditor`, queried with explicit modifier effects.
4. One reusable `EditorGridFrame3D` containing an orthonormal basis, an anchor, and a separate plane coordinate.
5. One optional working-grid-frame query on `EditorEditDomainSession`.
6. Per-subviewport grid visibility; editor-global snap enable and step.

This is the repeatable pattern for another 3D view or edit domain: acquire a private viewport layer, own render resources at the viewport, obtain presentation state from the owning view, and optionally obtain a working frame from the active domain.

## 2. Review rulings

### Accepted from the Opus review

- The bug is not merely main-view routing; viewport 0 is hard-coded inside the selected view as well.
- Same-world isolation cannot be fixed by toggling visibility on the current per-view instances while they all use bit 25.
- Arbitrary work planes should use transformed line geometry and a simple backend-safe fade shader, not a procedural derivative-based plane shader.
- The existing `editors/3d/grid_size` key should not be renamed. Its documentation and local variable names should be corrected instead.
- The generic edit-domain API should gain only one optional frame query, with a dummy-domain test. Do not add a capability system.
- Local/Root/World must be defined from the actual operand and nearest active CSG root, not from saved editor side tables.

### Corrections to the Opus review

1. **Reject one renderer per `Node3DEditorView`.** A view may contain four cameras. One mesh cannot be centered, projected, and LOD-selected for four cameras at once. It is the reason viewport 0 is currently privileged. The renderer belongs to each `Node3DEditorViewport`.

2. **Reuse and generalize the existing private viewport layer.** Do not create a second grid-specific layer pool. Grid, origin, and transform-gizmo instances owned by one subviewport use the same private bit. Per-instance visibility is safe once the instance is private to that viewport.

3. **Never degrade by aliasing a live lease.** The existing `allocate_gizmo_layer()` fallback returns bit 27 without recording an additional owner; a later free can clear another viewport's live allocation. The new pool fails closed with an invalid lease and retries when capacity becomes available.

4. **Keep adaptive LOD, but base it on the snap lattice for every grid.** The world grid should not remain semantically unrelated to snapping. Its visible minor spacing is `base_snap_step * primary_grid_steps^level`; the existing smooth small/large transition remains.

5. **Do not use `EditorDocument::contextual_editor_state` for grid state.** It is document-scoped. This fork already has `EditorDocumentView::editor_states`, explicitly intended for state that differs between two panes showing one document. Add minimal surface capture/apply hooks and use that dictionary. Disk serialization remains the workspace-session milestone.

6. **Do not use a global modifier-suppression lease.** Snap queries accept an explicit modifier-effect mask. Native tools request Ctrl inversion and Shift fine-snap; CSG requests neither. There is no acquire/release lifecycle to leak.

7. **A frame needs a separate plane coordinate.** An anchor alone cannot both define an absolute Root/World lattice and lie on an arbitrary hovered face. The anchor defines coordinate zero; `plane_coordinate` selects the parallel plane currently drawn or edited.

## 3. Frozen UX

### Toolbar

Add one compact Grid group to the shared 3D toolbar, separated from transform-space controls:

`[grid visibility] [magnet snap] [1 m v]`

- Grid visibility toggles the active subviewport.
- Magnet toggles the editor-global snap-enabled state.
- The small text button shows the configured base translate snap step, for example `0.25 m`.
- Its popup offers smaller/larger step, common metric steps, `Configure Snap...`, and `3D Grid Settings...`.
- The tooltip may additionally report the active renderer's current visible minor spacing, for example `Snap: 0.25 m; visible grid: 2 m`.
- Reuse the existing grid, magnet, and settings icon vocabulary. Do not add a wide text label reading "Grid" to the crowded row.

The existing controls remain aliases:

- `View > View Grid` and the active viewport's `View > Grid` item toggle the same active-subviewport boolean.
- `Transform > Configure Snap...` opens the same snap dialog.
- Every alias refreshes after pane focus, subviewport click, state restore, or a change from another alias.

### Shortcuts

- `#`: toggle grid in the active subviewport; keep the existing shortcut.
- `Y`: toggle snap; keep the existing shortcut.
- `[`: halve the base translate snap step.
- `]`: double the base translate snap step.

Brackets are unused by the 3D editor. Animation-editor bracket bindings are in a different shortcut context. All four shortcuts are inert while a `LineEdit`, `TextEdit`, `CodeEdit`, spin-box editor, or IME text control owns focus. The existing transform-in-progress `Y` axis lock remains context-specific and unchanged.

### Grid behavior

- Grid visibility is per `Node3DEditorViewport`.
- Origin visibility remains per `Node3DEditorView` and is applied to all of its viewport renderers.
- Snap enabled and translate/rotate/scale steps remain editor-global services state.
- The configured translate step remains visible even when snapping is disabled.
- Temporary native fine-snap does not rebuild or relabel the grid; the grid represents the configured base lattice.
- In normal editing, canonical XY/YZ/XZ planes follow existing editor settings and orthographic behavior.
- In an edit domain, an available working frame replaces the canonical planes with one domain plane.
- Hover may preview a domain plane. Pointer press locks its anchor, basis, and starting plane coordinate for the gesture. A drag may move only the current plane coordinate; it may not rotate or re-anchor the lattice mid-gesture.

### CSG terminology

Rename the private CSG concept from `SnapSpace` to `GridSpace`, because it controls rendering, pointer projection, numeric entry, ghost geometry, and commit:

- Local: lattice anchor/orientation from the acting operand.
- CSG Root: lattice anchor/orientation from the nearest active CSG root.
- World: lattice anchor/orientation from identity/world origin.

All three use the actual hovered face normal and plane. World does not mean "always XZ" when editing a wall.

Expose `Grid Space: Local | CSG Root | World` once in the common CSG contextual panel. Disable it during a gesture. Default to Local.

## 4. Ownership and invariants

| Concern | Owner | Lifetime |
| --- | --- | --- |
| Base snap enable and translate/rotate/scale values | `Node3DEditor` | Editor service |
| Raw event-sourced Ctrl/Shift state and snap query policy | `Node3DEditor` | Editor service |
| Shared toolbar controls and target routing | `Node3DEditor` | Editor service |
| Bound document/world, viewport layout, last-used viewport, origin preference, edit-domain host | `Node3DEditorView` | Pane surface |
| Camera, grid visibility, private editor-layer lease, grid/origin renderer | `Node3DEditorViewport` | Subviewport |
| Optional preview/locked working frame | `EditorEditDomainSession` | Pane-local domain session |
| CSG grid-space selection and gesture frame | `CSGSurfaceSession` | CSG session |
| Camera/layout/grid visibility state | `EditorDocumentView::editor_states` | Per pane/document view |

Invariants:

1. `Node3DEditor` owns no grid/origin RIDs or camera-dependent grid state.
2. A render instance intended for one subviewport uses only that subviewport's leased bit.
3. A lease is unique within one scenario, is never aliased, and is freed only by its owner.
4. Hidden or detached subviewports do not consume a lease.
5. Layer masks use `uint32_t(1) << layer`; bit 31 must never use signed shift behavior.
6. Grid RIDs are created once and freed once. A semantic rebuild uses `mesh_clear()` on the existing mesh RID.
7. Camera movement alone never creates an instance or mesh RID.
8. Grid visibility has one source of truth per subviewport. Menus and toolbar buttons are projections of it.
9. Snap profile mutation goes through one setter that validates, persists project metadata, updates the dialog/readout, and invalidates all live renderers.
10. Snap helpers do not decide which modifier meanings apply. Callers supply the policy.
11. `EditorGridFrame3D` is orthonormal and finite. Source scale is never a grid unit or skew.
12. Working-frame snapping is absolute about the selected space anchor, never accumulated from the previous drag sample.
13. Per-pane state never enters document-scoped contextual state.

## 5. Core data structures

### 5.1 Private viewport layer pool

Generalize the existing gizmo pool into a private 3D viewport-layer pool owned by `Node3DEditor`.

Available bits are exactly:

`20, 21, 22, 23, 27, 28, 29, 30, 31`

Bits 0-19 remain user-visible render layers. Bit 24 remains `MISC_TOOL_LAYER`; bit 25 remains reserved as the legacy grid layer during migration and for downstream compatibility; bit 26 remains `GIZMO_EDIT_LAYER`.

The built-in renderer stops placing instances on bit 25. A viewport may continue to include or clear bit 25 with its grid-visible state during the compatibility window so downstream legacy grid overlays still follow the toggle, but built-in grid isolation never depends on that bit.

The pool is keyed by scenario RID id, as the current gizmo allocator is. It returns a small lease value containing the scenario key and layer index, or invalid on exhaustion. It tracks nine slots, not a contiguous bit range.

Lease lifecycle in `Node3DEditorViewport::_sync_private_editor_layer()`:

- acquire when the viewport is inside the tree, visible in the tree, and bound to a valid world;
- release before world change, on visibility loss, and during destruction;
- after acquire/release, rewrite camera cull mask plus all private gizmo/grid/origin instance masks;
- on exhaustion, set private instance masks to zero, leave the scene camera usable, warn once per scenario, and retry on a later visibility/world synchronization;
- while a visible viewport has no lease, show a small non-modal viewport warning that editor overlays are unavailable; remove it immediately after acquisition succeeds;
- a failed acquire is never passed to release.

This also fixes the current allocator's untracked bit-27 fallback.

### 5.2 Snap query policy

Add a small bitmask, not a mode-specific class hierarchy:

```cpp
enum class EditorSnapModifierEffect : uint8_t {
    NONE = 0,
    CTRL_INVERT_ENABLED = 1 << 0,
    SHIFT_FINE_STEP = 1 << 1,
    NATIVE = (1 << 0) | (1 << 1),
};
```

`Node3DEditor` stores raw Ctrl/Shift state updated from real `InputEventWithModifiers` events. The active `Node3DEditorView::input()` and viewport input path feed it before GUI controls consume the event. Focus loss clears it.

APIs:

```cpp
bool is_snap_enabled(EditorSnapModifierEffect p_effects) const;
real_t get_translate_snap(EditorSnapModifierEffect p_effects) const;
real_t get_rotate_snap(EditorSnapModifierEffect p_effects) const;
real_t get_scale_snap(EditorSnapModifierEffect p_effects) const;
```

Keep compatibility overloads/defaults only at public boundaries; internal call sites pass `NATIVE` or `NONE` explicitly.

- Native transform/gizmo tools: `NATIVE`.
- CSG surface and draw gestures: `NONE`.
- Grid renderer: configured base translate value, equivalent to `NONE`, independent of snap enabled.

Centralize translate-step mutation. The supported minimum is `0.001 m`; values loaded as zero or negative are normalized to the minimum because snap-off already has an explicit toggle. Half/double shortcuts use the same setter and allow values above the current dialog's soft maximum.

### 5.3 `EditorGridFrame3D`

Place the value and pure grid-layout math in `editor/scene/3d/editor_grid_3d.{h,cpp}` so renderer, domain, CSG, and tests share one implementation.

```cpp
struct EditorGridFrame3D {
    Vector3 u;
    Vector3 v;
    Vector3 n;
    Vector3 anchor;
    real_t plane_coordinate = 0.0;

    static EditorGridFrame3D world_xz();
    static bool from_plane_in_space(
            const Vector3 &p_point_on_plane,
            const Vector3 &p_outward_normal,
            const Transform3D &p_space_to_world,
            const Vector3 &p_tangent_hint,
            EditorGridFrame3D &r_frame);

    Vector3 to_coordinates(const Vector3 &p_world) const;
    Vector3 to_world(const Vector3 &p_coordinates) const;
    Vector3 plane_origin() const;
    Plane plane() const;
    Vector2 snap_uv(const Vector3 &p_world, real_t p_step) const;
    real_t snap_n(const Vector3 &p_world, real_t p_step) const;
};
```

Rules:

- `anchor` is the chosen Local/Root/World coordinate origin.
- `plane_coordinate` is the absolute N coordinate of the displayed/edit plane.
- U is the tangent hint projected onto the plane. If degenerate, choose a deterministic space-basis axis least parallel to N.
- V is `N cross U` (or the consistently chosen equivalent); the result is right-handed and orthonormal.
- Transform scale and shear are removed by normalization/Gram-Schmidt. Units are world meters in every grid space.
- Mirrored inputs use the semantic outward normal and a deterministic tangent sign; they do not produce a left-handed or scaled lattice.
- Invalid/degenerate construction returns false. The caller falls back to `world_xz()`; do not silently manufacture a partly invalid frame.

### 5.4 Grid LOD/layout

Keep the current adaptive two-level fade, but make its base semantic:

```text
minor_step = base_translate_snap * primary_grid_steps ^ level
major_step = minor_step * primary_grid_steps
```

The base snap level (`level == 0`) must always be reachable. Negative levels are permitted when the configured physical minimum calls for visible subdivisions of a coarse snap lattice; the base snap lattice remains a coarser member of the same nested hierarchy.

Compute projected pixels per base step at a stable sample on the working plane. Use the full camera view/projection transform for both perspective and orthographic cameras: project `sample`, `sample + U * step`, and `sample + V * step`, then derive an isotropic density from both screen-space vectors. This is required for tilted and nearly edge-on edit-domain planes; orthographic size/viewport height alone is insufficient because it ignores foreshortening. Prefer a finite center-view-ray/plane intersection as the sample. Fall back deterministically and clamp invalid, behind-camera, and horizon cases.

The existing division-level settings retain their documented role as an absolute world-space spacing envelope, rather than being multiplied blindly by an arbitrary snap step. After applying the existing primary-step compatibility interpretation, derive absolute `configured_min_spacing` and `configured_max_spacing`, then convert that envelope to snap-lattice exponent bounds:

```text
level_min = min(0, floor(log_primary(configured_min_spacing / base_translate_snap)))
level_max = max(0, ceil (log_primary(configured_max_spacing / base_translate_snap)))
```

This deliberately rounds outward: the grid remains aligned to the snap lattice, includes the base level, can show useful subdivisions for a coarse snap value, and can still become physically coarse enough when the snap value is very small. For example, a `0.001 m` snap must not cap the far-view grid at `0.064 m` cells and a roughly `12.8 m` half-extent.

Choose a continuous level using projected density and division bias, then clamp it to the derived exponent bounds. Its floor selects the minor mesh step; its fraction drives the small/large color fade. Rebuild geometry only when one of these canonical semantic keys changes:

- frame basis, anchor, or plane coordinate;
- floor LOD;
- camera-centered major-cell coordinate in U/V;
- grid extent, plane settings, colors, or primary step count;
- base translate snap step.

Projection type, viewport dimensions, FOV/orthographic size, and camera motion are inputs to LOD and material telemetry, but they are not raw geometry-key fields. They cause a rebuild only when their derived floor LOD or centered major cell changes. Resizing a pane by one pixel while those decisions stay constant must not clear and rebuild the mesh.

Working frames must be canonicalized from stable semantic geometry whenever possible (for example, the authored CSG face plane rather than a raw ray-hit point). Rebuild-key comparison uses a documented finite tolerance for basis and plane values, scaled conservatively by the current minor step. Sub-pixel ray jitter on one semantic plane must not rebuild geometry; a real plane, anchor, or orientation change must.

Material-only fade changes do not rebuild geometry. `primary_grid_steps <= 1` gets a guarded fallback factor of 2 and a corrected editor-setting hint; no logarithm may divide by zero. Normalize non-finite bias/reference inputs, enforce ordered finite bounds, and clamp the fade radius to a finite non-negative value. `grid_half_extent_cells <= primary_grid_steps` is legal and must never produce a negative shader radius.

The existing `editors/3d/grid_size` key remains. Rename the local variable to `grid_half_extent_cells` and update `EditorSettings.xml` to explain that physical extent changes with visible cell spacing.

### 5.5 `EditorGrid3DRenderer`

One renderer is owned by each `Node3DEditorViewport`. It owns:

- persistent mesh/instance RIDs for up to three canonical planes or one working plane;
- persistent origin mesh/instance RIDs for that viewport;
- materials and the last semantic layout key;
- visibility, scenario, private layer, and current visible-spacing telemetry.

World mode supports the existing XY/YZ/XZ settings and orthographic behavior. Working mode draws one arbitrary frame plane and hides unused slots.

Use line geometry in frame-local U/V and set the instance transform from the frame. Generalize the current fade shader's plane math:

- derive the world plane normal through the model normal transform;
- derive plane origin from the instance transform;
- project camera position with `camera - normal * dot(camera - plane_origin, normal)`;
- use no derivatives or renderer-specific features.

Instances are created detached. The viewport's existing deferred-first-bind/material-registration protection must be preserved when ownership moves out of `Node3DEditorView`. World rebinding changes scenario only; it does not recreate resources.

## 6. Active-target and alias routing

The shared toolbar's shortcut context is not sufficient to identify the target object; it currently reparents controls but stores no view pointer.

Add an explicit toolbar target on `Node3DEditor`:

- `EditorNode::update_scene_pane_toolbar()` passes the focused `DocumentView`'s scene `SubViewport` alongside the shortcut context.
- `Node3DEditor` resolves that `SubViewport` to its `Node3DEditorViewport` by scanning live views, then stores an ObjectID.
- `_viewport_clicked(Node3DEditorView *, int)` updates the same target immediately.
- a dead/hidden target falls back to the focused view's last-used viewport, then `main_view` only as legacy fallback.

Every grid command resolves the target at invocation time. No grid handler reads or writes `main_view` directly.

`Node3DEditorViewport::set_grid_visible(bool)` is the single state mutation. It updates its renderer and local View-menu checkmark, then asks services to refresh the shared toolbar/View menu if it is the active target.

Snap commands remain global, but one central refresh updates the magnet, text readout, snap dialog, menus, and all renderer invalidation flags.

## 7. Generic per-pane state seam

Do not implement the full workspace session store as part of grid work. Add only the already-planned surface seam:

```cpp
class EditorDocumentSurfaceInstance {
public:
    virtual Dictionary capture_view_state() const { return Dictionary(); }
    virtual void apply_view_state(const Dictionary &p_state) {}
};
```

`DocumentView` exposes `capture_view_state()` and `apply_view_state()`:

- capture writes the surface dictionary into its own `EditorDocumentView::editor_states`;
- apply runs after the concrete surface is ready/in-tree;
- `EditorSceneDocumentSurfaceInstance` captures the outgoing 2D/3D surface before switching and applies the incoming surface's saved subdictionary;
- pre-delete performs a final capture.

Add `Node3DEditorView::get_view_state()` / `set_view_state()` around existing viewport state APIs. Store:

- layout mode and split state;
- last-used viewport index;
- per-viewport camera/display state, including its grid-visible boolean;
- view-wide origin preference.

Do not store snap enabled/step (service-global), private layer bits, RIDs, LOD caches, or domain working frames.

Fix preview-camera state while touching this path: `Node3DEditorViewport::get_state()` and `set_state()` must serialize a `NodePath` relative to the owning view's bound document root, not `EditorNode::get_edited_scene()`.

`EditorDocumentView::editor_states` is in-memory scaffolding today. Cross-restart persistence lands later with the planned `editor_workspace_session` schema; this feature must not create a grid-only session file or use document-scoped contextual state as a shortcut.

## 8. Edit-domain seam

Forward-declare `EditorGridFrame3D` in `editor/gui/editor_edit_domain.h` and add one optional query:

```cpp
virtual bool get_working_grid_frame(EditorGridFrame3D &r_frame) const {
    return false;
}
```

`Node3DEditorView::update_grid(Node3DEditorViewport *p_viewport)` asks its per-view active session. False means canonical world grid. The query is const and side-effect free. A viewport argument is unnecessary because each `Node3DEditorView` owns an independent domain host/session; the renderer target remains the explicit argument to `update_grid()`. There is no capability dictionary, registration metadata, or CSG type check.

Extend the existing dummy session in `tests/editor/test_editor_edit_domain.cpp` to return a rotated frame. Test true, false/fallback, two independent hosts, and session destruction. This is the second client that keeps the seam generic.

## 9. CSG integration

### Frame selection

Add session members for preview and locked frames:

- `preview_grid_frame` plus validity;
- `locked_grid_frame` plus validity;
- `GridSpace grid_space`;
- frame lock begins on pointer press and ends on commit/cancel/exit/retarget.

Build a face frame from:

- point and outward normal from the semantic hit;
- Local space transform = acting operand global transform;
- CSG Root transform = nearest active root global transform;
- World transform = identity;
- Local tangent hint = semantic face tangent; Root/World tangent hint = a deterministic projected axis from that space.

The transform contributes translation and orientation, never scale/shear. The selected face supplies N in all spaces. `plane_coordinate = N dot (face_point - anchor)`.

When no face is available:

- Draw uses world XZ at coordinate zero.
- Surface falls back to canonical world grid.
- Paint and Operand do not supply a working frame.

### Surface push/pull and extrusion

At press, lock the frame and the starting absolute N coordinate. During drag:

1. Intersect/solve the existing world drag line.
2. Convert the candidate face point to the locked frame's absolute N coordinate.
3. If base snapping is enabled under `EditorSnapModifierEffect::NONE`, snap that coordinate.
4. Convert the snapped world displacement back to the operand-local box size/transform math.
5. Use the same value for the distance label, numeric field, ghost, and commit.

Shift remains extrusion and Ctrl remains their CSG meanings; neither changes snap enable or step. Alt symmetry remains as implemented. Numeric input is an absolute N coordinate in the selected Grid Space and displays the signed delta as secondary information.

During drag, the renderer may move `plane_coordinate` to the snapped ghost face, but U/V/N and anchor remain locked.

### Draw tool

Replace `draw_plane_origin_world`, `draw_plane_normal_world`, `draw_plane_u_world`, and `draw_plane_v_world` as competing authorities with the locked frame.

- Rectangle phase intersects `frame.plane()` and stores absolute U/V coordinates.
- U/V values snap through the frame, not relative to the first corner or current plane origin.
- Height phase computes an absolute N coordinate; height is target N minus the base `plane_coordinate`.
- Ghost and `csg_draw_box_from_rect()` consume the same frame.
- Cut inversion reads event-sourced raw Ctrl state but calls snap APIs with `NONE`.
- Escape, tool switch, root loss, retarget, exit, and successful commit clear the locked frame.

`get_working_grid_frame()` returns the locked frame during a gesture, otherwise a valid hover preview, otherwise false.

## 10. Dependency-ordered implementation phases

### Phase 0 - Characterize and extract pure policy

Files:

- new `editor/scene/3d/editor_grid_3d.{h,cpp}`;
- new `tests/editor/test_editor_grid_3d.cpp`.

Implement and test `EditorGridFrame3D`, projected-density/LOD selection, semantic rebuild keys, finite/degenerate handling, and snap-lattice centering. Rework, rather than accepting unchanged, the archived Phase 0 implementation identified at the top of this document. No renderer or UI change.

Gate: rotated, translated, nonuniform, mirrored, arbitrary-plane, absolute U/V/N, perspective/ortho layout including oblique orthographic planes, snap steps from `0.001 m` through large values, `primary_grid_steps == 1`, invalid setting combinations, stable jitter/resize rebuild keys, and LOD boundary tests pass.

### Phase 1 - Harden the private viewport layer lease

Files:

- `editor/scene/3d/node_3d_editor_plugin.{h,cpp}`;
- `editor/scene/3d/node_3d_editor_viewport.{h,cpp}`;
- `tests/editor/test_editor_grid_3d.cpp` or a focused layer-pool test file.

Replace `allocate_gizmo_layer/free_gizmo_layer` with the noncontiguous checked pool, rename viewport members from gizmo-layer to private-editor-layer, make leases visibility/world aware, migrate transform-gizmo masks, use unsigned shifts, and remove alias fallback.

Gate: nine unique leases per scenario; independent reuse across scenarios; tenth invalid; no double-free; hidden viewport releases; reacquire restores masks and clears the viewport warning; bit 31 safe.

### Phase 2 - Move rendering to each subviewport

Files:

- `editor/scene/3d/editor_grid_3d.{h,cpp}`;
- `editor/scene/3d/node_3d_editor_plugin.{h,cpp}`;
- `editor/scene/3d/node_3d_editor_viewport.{h,cpp}`.

Implement `EditorGrid3DRenderer`, move grid/origin RIDs out of `Node3DEditorView`, use the viewport's private layer and camera, preserve deferred scenario binding, route the viewport process call through its owning `editor_view`, fan editor-setting changes to all live renderers, and reuse RIDs with `mesh_clear()`.

The renderer reads the configured raw translate step through a narrow accessor that does not apply temporary modifier effects. Existing snap-change paths invalidate all live renderers; Phase 3 centralizes those paths without changing this contract.

Remove every built-in grid-instance assignment to `GIZMO_GRID_LAYER`; bit 25 remains only a temporary compatibility mask as described above.

Gate: non-main pane grid appears; quad cameras each get correct centering/LOD; two same-world panes show exactly one private grid each; toggle independence; world rebind; hidden cleanup; no RID churn; tiny and large snap steps retain useful near/far grids.

### Phase 3 - Make snap policy explicit

Files:

- `editor/scene/3d/node_3d_editor_plugin.{h,cpp}`;
- `editor/scene/3d/node_3d_editor_viewport.cpp`;
- `modules/csg/editor/csg_edit_domain.{h,cpp}`;
- `tests/editor/test_csg_edit_domain.cpp` and snap policy tests.

Add event-sourced raw modifiers, explicit effect masks, central positive-step setter, and migrate internal call sites. CSG's active step uses `NONE`; cut uses raw Ctrl state without changing snap. The central setter retains the renderer invalidation introduced in Phase 2.

Gate: native Ctrl inversion and Shift fine-snap remain; CSG Ctrl/Shift do not affect enable/step; focus loss clears modifiers; zero state normalizes safely; every step mutation invalidates all live renderer telemetry/layout exactly once.

### Phase 4 - First-class Grid toolbar and coherent aliases

Files:

- `editor/scene/3d/node_3d_editor_plugin.{h,cpp}`;
- `editor/scene/3d/node_3d_editor_viewport.{h,cpp}`;
- `editor/editor_node.{h,cpp}`;
- `editor/settings/editor_settings.cpp`;
- `doc/classes/EditorSettings.xml`.

Add the compact toolbar group, active-target ObjectID routing, one per-viewport grid setter, menu synchronization, step popup, bracket shortcuts, text-focus guard, central snap refresh, snap-derived adaptive world spacing, and corrected grid-setting descriptions.

Gate: toolbar follows pane and subviewport focus; all aliases agree; step readout/settings update from every path; typing is unaffected; grid remains adaptive but aligned to snap.

### Phase 5 - Generic per-view state capture/apply

Files:

- `editor/gui/editor_document_surface.{h,cpp}`;
- `editor/gui/document_view.{h,cpp}`;
- `editor/scene/3d/node_3d_editor_plugin.{h,cpp}`;
- `editor/scene/3d/node_3d_editor_viewport.{h,cpp}`;
- relevant editor tests.

Add the two generic hooks, `DocumentView` capture/apply, Node3D view state, and bound-document preview-camera NodePaths. Store only in `EditorDocumentView::editor_states`; do not add disk persistence here.

Gate: capture/apply round-trip restores layout, cameras, per-viewport grid visibility, origin, and preview camera for the correct document; two views of one document retain different dictionaries.

### Phase 6 - Optional edit-domain working frame

Files:

- `editor/gui/editor_edit_domain.h`;
- `editor/scene/3d/node_3d_editor_plugin.cpp`;
- `tests/editor/test_editor_edit_domain.cpp`.

Add the single virtual query, world fallback, and dummy provider/session coverage.

Gate: false uses canonical grid; true supplies arbitrary frame; two pane hosts remain independent; exit immediately returns to canonical grid.

### Phase 7 - CSG Local/Root/World grid integration

Files:

- `modules/csg/editor/csg_edit_domain.{h,cpp}`;
- `tests/editor/test_csg_edit_domain.cpp`;
- `modules/csg/tests/test_csg.h` only if pure box/frame math belongs in module coverage;
- `workspace-editor-planning/CSG-EDIT-PROGRESS.md`.

Add Grid Space UI, hover/locked frame state, absolute Surface/Draw math, unified numeric/ghost/commit values, and cleanup on every lifecycle path.

Gate: Local/Root/World produce distinct expected lattices; rotated/nonuniform/mirrored operands; push/pull, symmetric, extrusion, add/cut draw; no Ctrl/Shift collision; exit restores canonical grid; existing CSG test counts remain green plus new cases.

### Phase 8 - Validation and production build

Run focused tests first, then the repository production build script. Do not start a second SCons process in an occupied namespace.

## 11. Test and acceptance matrix

### Pure/headless

- Frame basis is finite, orthonormal, right-handed, and scale-free.
- Anchor and plane coordinate reconstruct the exact source plane.
- Absolute U/V/N snapping is stable across repeated samples.
- Local/Root/World anchors and projected tangents are deterministic.
- Mirrored and nonuniform transforms do not skew cells.
- LOD remains on powers of `primary_grid_steps` relative to base snap.
- Tiny and large snap steps retain the base level while reaching a useful physical near/far spacing envelope.
- Perspective and orthographic projected density account for U/V foreshortening on arbitrary planes.
- Center-cell and LOD boundaries rebuild exactly once; fade-only, sub-tolerance hit jitter, and same-LOD viewport resizing do not rebuild.
- `grid_half_extent_cells <= primary_grid_steps`, reversed bounds, and non-finite inputs stay finite and never produce a negative fade radius.
- Invalid camera/frame inputs do not emit NaNs.
- Layer pool covers all nine bits, is per scenario, fails closed, and balances acquire/free.
- Snap policy native/none behavior and modifier focus reset.
- Generic domain frame true/false and two-host isolation.
- View-state dictionaries are independent per `EditorDocumentView`.

### CSG automated

- Hover frame, press lock, cancel, commit, retarget, and exit.
- Absolute rectangle U/V and height N.
- Absolute push/pull plane coordinate in all three spaces.
- Numeric, pointer, ghost, and committed result agree.
- Ctrl cut and Shift extrusion leave configured snap unchanged.
- Rotated, nonuniform, and mirrored operand/root transforms.

### Live smoke

- A pane-created `Node3DEditorView` draws a grid immediately.
- Main view and pane view can share one world without duplicate grids.
- Two visible panes on one world have independent visibility and cameras.
- Quad view has four correctly centered grids, not viewport-0 copies.
- Perspective and every orthographic direction.
- Oblique edit-domain planes in orthographic and perspective views, including nearly edge-on angle fade.
- Snap steps `0.001`, `0.01`, `0.1`, `1`, and `10 m` at near and far camera distances; the grid neither truncates near the camera nor loses snap alignment.
- Forward+, Mobile, and Compatibility rendering.
- Toggle grid from toolbar, shared View menu, and viewport View menu.
- `#`, `Y`, `[`, `]`; then type those characters in CSG numeric and ordinary text fields.
- Switch documents, move tabs/panes, hide/show panes, and return without stale layers or scenarios.
- Enter CSG, hover different faces, draw/push/extrude, cancel, and exit.
- RenderingServer RID counts remain stable while orbiting/panning within one semantic cell and across mesh rebuilds.

Focused commands after a tests-enabled dev build:

```powershell
bin\godot.windows.editor.dev.x86_64.exe --test '--test-case=*EditorGrid3D*'
bin\godot.windows.editor.dev.x86_64.exe --test '--test-case=*EditorEditDomain*'
bin\godot.windows.editor.dev.x86_64.exe --test '--test-case=*CSGEditDomain*'
bin\godot.windows.editor.dev.x86_64.exe --test '--test-case=*CSG*'
```

Final production verification uses only:

```powershell
.\build_editor.ps1
```

## 12. Edge and failure behavior

- More than nine simultaneously visible subviewports on one scenario: additional viewports render the scene but no private transform gizmo/grid/origin, warn once, show the non-modal unavailable-overlays indicator in each affected viewport, and retry when a lease frees. Never overlap another viewport's private layer.
- Hidden quad slots and background tabs release leases but retain renderer RIDs for cheap reactivation.
- Invalid world/scenario: detach instances and release the old lease.
- Invalid frame: canonical world fallback outside a locked gesture; cancel the gesture if its locked frame becomes invalid.
- Camera parallel to a grid plane: retain finite layout and let angle fade hide it; never explode LOD.
- Snap disabled: grid still represents configured spacing; snapping operations do nothing.
- Base step loaded as zero/negative: normalize to `0.001 m` and update every alias.
- CSG root/operand deleted or schema generation changes: clear preview/lock before the next render query.
- Renderer/material not registered on first frame: remain detached until deferred bind; never attach a null material to a live scenario.
- A state dictionary references a missing preview camera: ignore it and restore the normal editor camera.

## 13. Migration and compatibility

- Keep `editors/3d/grid_size`, `primary_grid_steps`, and division-level keys. Update documentation; do not migrate or duplicate keys.
- `grid_size` changes only in description/local naming. Existing numeric values load.
- Visible divisions remain powers of `primary_grid_steps` relative to the configured base snap lattice. The existing division-level settings continue to define an absolute physical spacing envelope, rounded outward to reachable snap-lattice levels while always including the base level. This is an intentional visible behavior refinement and must be noted in release notes.
- Existing `#` and `Y` shortcuts keep their paths and defaults. Bracket shortcuts are additive and user-remappable.
- Keep public no-argument snap accessors as native-policy compatibility wrappers where external editor modules require them; internal code uses explicit effects.
- Keep a no-argument `Node3DEditor::update_grid()` compatibility forwarder temporarily, but no internal viewport may call it.
- Existing viewport state key `grid` remains valid. Old state without new view-level keys receives defaults.
- Do not serialize ObjectIDs, RIDs, layer numbers, renderer caches, or working frames.

## 14. Deferred and non-goals

- Full workspace-session disk serialization (`editor_workspace_session`, planned M6.3a/b).
- 2D editor grid/snap parity.
- Rotation/scale grids or arbitrary-frame rotation/scale snapping.
- Named/reusable workplanes and workplane management UI.
- Vertex, surface, and floor-snap unification.
- Moving all shared bit-24/bit-26 third-party overlays to private viewport layers.
- A procedural infinite-grid shader.
- More than nine private 3D subviewports in one scenario; solve only if a real workflow reaches the fail-closed limit.
- CSG polygon/cylinder editing or new extrusion topology.

## 15. Commit sequence

Each implementation commit must build and pass its focused gate before the next:

1. `grid: add working-frame and snap-lattice layout math`
2. `editor: harden private 3D viewport layer leases`
3. `grid: render grid and origin per 3D subviewport`
4. `snap: make modifier effects explicit for native and domain tools`
5. `grid: add first-class toolbar controls and snap-aligned LOD`
6. `workspace: capture and restore generic per-view surface state`
7. `edit-domain: add optional working-grid-frame query`
8. `csg: integrate Local Root and World working grids`

Documentation and tests travel with the commit whose behavior they describe. There is no code-free validation commit.

## 16. Worktree prerequisite and staging discipline

The original freeze-time base (`146ae83ee0`) and its ahead-of-origin count are historical only. Before implementation, start from the then-current clean `master`/tracking branch, fetch its remote, record the actual base commit in the implementation handoff, and compare the archived Phase 0 files against that base before reusing them.

The tree is shared. Before every implementation phase:

- inspect `git status --short --branch` and the relevant diff;
- stop if a required file has unrelated concurrent changes;
- stage explicit paths only;
- never use `git add -A`, `git commit -a`, stash, reset, or revert another session's work;
- check for a running SCons process before any build;
- use the dev/test namespace for focused tests and `build_editor.ps1` for the final production build.

Commit this revised plan independently before implementation begins so the archived Phase 0 behavior cannot be mistaken for the accepted specification.
