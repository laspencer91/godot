# CSG Edit and Extensible View Architecture — Complete Frozen Plan

This is the implementation-ready design target. It deliberately separates engine data, evaluation, editor interaction, and presentation.

## 1. Goals

The work should make Godot CSG suitable for fast architectural blocking and iteration:

- Select and hover semantic CSG surfaces.
- Push or pull faces along their normals without scaling the node.
- Extrude box faces with Shift-drag.
- Assign materials per semantic surface.
- Provide focused planar UV tools with texture locking.
- Draw additive and subtractive shapes directly in the viewport.
- Keep boolean rebuilding interactive on substantial CSG trees.
- Preserve native viewport navigation and node editing.
- Support multiple panes without leaking state or overlays between them.
- Establish repeatable patterns for both new document surfaces and new edit domains.

This remains parametric CSG editing. It is not intended to become a full mesh modeler.

## 2. Frozen architectural decisions

- CSG Edit is an explicit edit domain inside the normal 3D view, not a new main screen.
- The hierarchy is: Document kind → Document surface → Edit domain → Tool → Gesture.
- CSG Edit owns face-level interaction only while active.
- The tool rail contains Surface, Draw, Paint, and Operand.
- Extrusion is a Shift-drag gesture in Surface, not a separate tool.
- Semantic surfaces are authoring identities; rendered triangles are transient results.
- runOriginalID transports semantic provenance through Manifold.
- faceID identifies a planar result facet but is never durable selection state.
- Face selection remains separate from EditorSelection.
- Materials and UV settings live on CSG nodes and work outside the editor.
- Cached Manifold expressions preserve unchanged subtree identity.
- Boolean evaluation is asynchronous and single-flight per CSG root.
- Gesture feedback is view-local and immediate, even when the boolean result lags.
- MVP extrusion adds a union child beneath the source shape and does not reparent existing nodes.
- Generic hosts mount domain-provided controls without interpreting their contents.
- New document surface types and new edit domains use separate registries.

## 3. Architectural layers

| Layer | Responsibility |
|---|---|
| CSG module | Surface schemas, provenance, caches, boolean expressions, evaluation scheduling, output metadata, node properties |
| Generic editor | Document surface registry, edit-domain registry, per-view host, input arbitration, chrome mounting |
| CSG editor | Mode lifecycle, tools, gestures, surface selection, overlays, ghosts, contextual controls |
| View presentation | Toolbar toggle, left rail, hint line, contextual accordion panel, view-local scope cues |

The CSG module must not depend on editor types. Editor code consumes resolved semantic surface information rather than Manifold IDs directly.

## 4. Repeatable document-surface pattern

A document surface is the concrete pane content used to view a document: 2D, 3D, Script, Shader, Help, and so forth.

The current construction switch in `editor/gui/document_view.cpp:286` should move behind:

### EditorDocumentSurfaceRegistry

Stores registered surface providers by StringName identifier.

Responsibilities:
- Resolve an explicitly requested surface ID.
- Resolve a default surface for a document.
- Safely handle provider registration and removal.
- Avoid hard-coded surface construction in DocumentView.

### EditorDocumentSurfaceProvider

Stateless factory with only proven responsibilities:
- Surface ID and registration metadata.
- supports(EditorDocument *).
- create(context) -> EditorDocumentSurfaceInstance.

Do not add a general capabilities dictionary.

### EditorDocumentSurfaceInstance

Owned per pane/document view. It provides:
- Its complete root Control.
- Context activation/deactivation.
- Pre-delete cleanup for shared or parked controls.
- Any view-specific state restoration it actually requires.

The provider returns the complete surface composite it needs. DocumentView does not need a semantic dock or panel model.

Existing 2D, 3D, Script, Shader, Resource, Help, and Screen Host surfaces become real providers. Adding a new view then requires a provider, instance, and registration — no DocumentView switch edit.

## 5. Repeatable edit-domain pattern

An edit domain changes what interaction means inside an existing surface. CSG Edit, terrain sculpting, and a future mesh-edit mode are domains, not surfaces.

### EditorEditDomainRegistry

Stores stateless providers and owns no pane state.

### EditorEditDomainProvider

Initial interface:
- Registered ID, label, and icon.
- is_available(context).
- can_activate_from_double_click(context, hit).
- create_session(context).

Add further capabilities only when two real domains need them.

### EditorEditDomainHost

One host per pane-level editor surface. Responsibilities:
- Own the active domain session.
- Enter, exit, and retarget domains.
- Route input before native gizmo and object-selection handling.
- Manage pointer capture for active gestures.
- Dispatch view-local drawing.
- Notify chrome that the active domain or tool changed.
- Mount the session's optional raw contextual Control.
- Suppress or restore native selected-object plugins according to domain policy.
- Exit safely if the provider, view, scene, or active root disappears.

### EditorEditDomainSession

One session per view. Responsibilities:
- Domain-local selection.
- Active tool.
- Gesture state.
- Input handling.
- Overlays and ghosts.
- Context panel construction.
- Tool descriptors for the rail.
- Mode lifecycle callbacks.

Session state is pane-local. It must not live in the CSG editor plugin singleton.

### Input result

Domain handling needs a tri-state result:

- `PASS_TO_VIEWPORT`: native viewport handling may continue.
- `BLOCK_NATIVE_EDIT`: the domain did not perform an action, but native gizmos or object selection must not react.
- `CONSUMED`: the domain handled the event.

An active gesture has pointer capture until release or cancellation.

## 6. Viewport chrome

Use the existing viewport chrome registry (`editor/gui/editor_viewport_chrome.h`) rather than manually parenting controls into Node3DEditorViewport.

Add:
- SLOT_CENTER_LEFT
- SLOT_CENTER_RIGHT

Center slots need vertically centered layout and viewport safe-area handling.

CSG chrome consists of:
- Top toolbar: CSG Edit toggle and active-root label.
- Center-left: Surface, Draw, Paint, Operand rail.
- Bottom-center: current gesture/modifier hint.
- Right accordion: session-provided contextual panel.

Chrome registrations are SCOPE_VIEW. Factories receive the view/domain host, never a raw session pointer. Controls persist for the view and hide when CSG Edit is inactive.

The contextual host is intentionally simple: session supplies Control → host mounts Control → host removes/replaces Control when session changes. It does not model fields, properties, or semantic sections.

## 7. CSG mode lifecycle

### Entering

CSG Edit can be entered by:
- Clicking the top toolbar toggle while a CSG node is selected.
- Double-clicking a visible CSG result in Scene mode.
- A CSG-aware viewport context-menu entry.

Entering from any CSG child resolves its nearest CSG root. The active root is displayed in the toolbar and becomes the editing scope.

### While active

- Selecting another operand under the active root keeps the mode active.
- Selecting a CSG node under another root retargets the session after canceling any active gesture and clearing face selection.
- Selecting a non-CSG node in the Scene Tree exits CSG Edit.
- Losing pane focus suspends input but does not destroy the session.
- Deleting or removing the root exits safely.
- Switching the pane to a non-3D surface exits the domain.
- Only the active pane accepts editing input.

### Exiting and cancellation

- Escape during a gesture restores its starting state.
- Escape with no active gesture exits CSG Edit.
- The toolbar toggle explicitly exits.
- Scene close, provider removal, or view destruction exits automatically.

The last-used CSG tool may be remembered per document view. Reopening a scene should still default to Scene mode for safety.

## 8. Native viewport interaction contract

| Interaction | CSG Surface/Draw/Paint | CSG Operand |
|---|---|---|
| Mouse hover | Domain face/tool hover | Native |
| LMB click | Domain selection/action | Native node selection |
| LMB drag | Domain gesture | Native gizmos |
| Shift-drag | Surface extrusion | Native behavior |
| RMB | Native context menu with CSG entries | Native |
| MMB/orbit/pan/zoom | Native navigation | Native |
| Wheel | Native navigation unless an explicit domain modifier is later established | Native |
| Gizmos | Selected-object gizmos suppressed | Native |
| Global force-draw plugins | Remain visible | Remain visible |
| Scene Tree selection | Domain lifecycle rules apply | Native |
| Escape | Cancel gesture, then exit | Exit or native cancellation |
| Tab | Toggle Surface ↔ Operand when viewport has focus | Toggle back |

Unmodified viewport navigation remains native. CSG interaction is inserted before gizmo and object-selection handling, not as scattered `if (csg_mode)` branches.

## 9. View-local visual language

While CSG Edit is active:
- The active root receives a clear view-local outline or tint.
- Non-member geometry may be faded with view-local overlays.
- Subtractive operands can be shown as colored wireframes or translucent source shapes.
- Hovered result fragments receive a light fill and outline.
- Selected semantic surfaces receive a stronger highlight.
- A selected box face displays a normal handle and plane-coordinate feedback.
- An active push/pull or extrusion shows a ghost immediately.
- If the boolean result is behind, a subtle "Updating CSG…" state appears.

Do not mutate node materials or per-instance parameters to create scope cues. That would leak between panes showing the same world. Full-scene desaturation remains a later view-local compositor feature.

## 10. Semantic surface model

### CSGSurfaceSchema

Each primitive type defines stable named semantic surfaces independent of triangle order.

Initial schemas:
- Box: six named axis faces.
- Cylinder: side, top, bottom.
- Sphere: body.
- Torus: body.
- Polygon: front, back, side, with path/cap extensions only where stable.
- Mesh: source mesh surface indices; arbitrary triangle-face editing is not initially supported.
- Combiner: no local surfaces.

Numeric indices are stable per primitive class and append-only where possible. Code should use named constants rather than current brush face order.

### CSGSurfaceKey

Transient durable authoring identity:

```cpp
struct CSGSurfaceKey {
    ObjectID source_shape;
    uint32_t semantic_surface;
    uint32_t schema_generation;
};
```

Properties:
- Stable across boolean evaluation and ordinary geometry edits.
- Deliberately process-local and nonserializable.
- Invalidated when its source node disappears or its semantic schema changes.
- Never contains faceID, result triangle index, or a saved NodePath.

The eventual header should explicitly state that ObjectID is intentional.

### CSGOriginToken

A nonserialized uint32_t transported as Manifold runOriginalID.

Each primitive reserves one contiguous token range for its current surface schema: `token = operand_origin_base + semantic_surface`.

- Allocate the range once and retain it across geometry rebuilds.
- Allocate a new range only when the semantic schema changes.
- Duplicated nodes receive independent ranges.
- Tokens are never scene data.
- Each evaluation snapshot owns a token-to-CSGSurfaceKey map.

### faceID

faceID represents a planar input/output facet:
- A box semantic face normally has one faceID.
- A cylinder side has one semantic token but multiple planar faceID values.
- Boolean splitting may produce multiple disconnected fragments sharing provenance.
- faceID is valid only for the result generation that produced it.

The Manifold contract explicitly preserves input faceID and reconstructs planar faces from run plus faceID (`thirdparty/manifold/include/manifold/manifold.h:100`).

### CSGSurfaceHit

A transient pick result:

```cpp
struct CSGSurfaceHit {
    CSGSurfaceKey surface;
    uint64_t result_generation;
    uint32_t face_id;
    uint32_t triangle;
    uint32_t connected_fragment;
};
```

Hover can highlight the connected fragment under the pointer. Selection stores the semantic CSGSurfaceKey, not the hit fields.

Visible-fragment editing can later use the richer hit without weakening semantic selection.

## 11. Manifold packing and provenance

Current material-only packing is replaced with semantic-surface runs.

For each local primitive:
- Generate its local brush or MeshGL64.
- Group triangles into runs by semantic surface.
- Set each run's stable origin token.
- Set meaningful input faceID values.
- Keep material lookup separate from runOriginalID.

The root evaluation snapshot holds: origin token → SurfaceKey; SurfaceKey → resolved surface settings. Output triangles resolve directly through their run token.

Nested CSG must remain as Manifold expressions. Intermediate children must no longer be unpacked into CSGBrush, transformed, and repacked. Only the final root result is materialized.

This replaces the current rebuild path around `modules/csg/csg_shape.cpp:484`.

## 12. Persistent cache graph

Each CSG node owns cached values at distinct levels:

primitive parameters → local brush / MeshGL → local Manifold + origin records → transformed child handle → subtree Manifold expression → root evaluated MeshGL → finalized render/pick/collision outputs

### Required identity invariant

Every clean authored subtree retains a strong `manifold::Manifold` handle. An unchanged transformed wrapper is also retained.

Parents reference those exact handles. They do not recreate clean subtree expressions during a rebuild.

This matters because:
- Transform() creates a wrapper with its own lazy cache (`thirdparty/manifold/src/csg_tree.cpp:485`).
- Manifold avoids collapsing reused operation nodes but may collapse uniquely owned ones (`thirdparty/manifold/src/csg_tree.cpp:670`).
- A new BatchBoolean parent retains supplied child CSG nodes (`thirdparty/manifold/src/manifold.cpp:919`).

### Expression construction

Expression construction must preserve current operation ordering exactly:
- Start from the node's own local manifold, if any.
- Visit visible CSG children in scene order.
- Apply their operation modes with the same grouping semantics as current CSG.
- Rebuild only dirty expression nodes and their ancestors.
- Reuse unchanged child subtree handles.

A flat n-ary parent remains one cache boundary. Editing one child can make that parent recombine all cached child results. A persistent balanced reduction tree is not part of the frozen MVP; add it only if wide-root profiling justifies the complexity.

## 13. Invalidation rules

| Change | Invalidates |
|---|---|
| Primitive dimensions or geometry | Local brush/manifold, subtree, ancestors, root result |
| Operand transform | Transformed wrapper, parent subtree, ancestors; not local manifold |
| Child operation, order, or visibility | Parent expression and ancestors; not clean leaves |
| Semantic schema change | Local manifold, origin range, selection generation, ancestors |
| Surface material | Output material grouping only |
| Planar UV settings | Output UVs, tangents, render mesh; not boolean |
| Legacy/interpolated input UVs | Local manifold properties and ancestors |
| Smoothing or tangent settings | Output finalization only |
| Collision settings | Collision output only |
| Root transform with local/root UV space | Node transform only |
| Root transform with world-space UVs | UV/tangent finalization; not boolean |
| Navigation or editor overlay settings | View state only |

Material and planar UV editing must not run the boolean again.

## 14. Evaluation and finalization split

The monolithic update_shape() path around `modules/csg/csg_shape.cpp:583` should be divided conceptually into:

1. Dirty-state propagation.
2. Cache/expression construction.
3. Manifold evaluation.
4. Raw result extraction.
5. Provenance resolution.
6. Material grouping.
7. UV generation.
8. Normals, smoothing, and tangents.
9. Rendering publication.
10. Picking acceleration update.
11. Collision publication.

This creates an explicit interactive/final policy.

### Interactive result

May skip or defer: collision, expensive autosmoothing, tangent generation, other final-only postprocessing.

It still uses the accurate Manifold boolean. Until an interactive result completes, the previous published result remains visible behind the immediate ghost.

### Final result

On gesture release or normal scene update: complete material grouping, complete UV and tangent generation, update render output, update picking data, update collision and configuration warnings.

A synchronous final evaluation entry point remains available for headless tests, import/export, and code paths that require immediate results.

## 15. Evaluation scheduler

The scheduler belongs to the CSG module and is shared by every view of a root.

Per root it stores: requested generation, published generation, one running job, one latest pending snapshot, requested quality (interactive or final).

### Threading contract

- At most one job evaluates a root's shared lazy Manifold graph.
- New requests coalesce while a job is running.
- Final quality supersedes a queued interactive request.
- Stale job results are discarded.
- After completion, the newest pending snapshot launches immediately.
- Jobs contain immutable Manifold values and plain settings snapshots.
- Jobs do not access scene nodes, editor state, Resource contents, RenderingServer, or EditorSelection.
- Main-thread publication validates root ObjectID, schema generation, and request generation.
- Root deletion safely discards outstanding results.

Single-flight is required because the lazy operation cache is mutable and not an atomic public concurrency contract (`thirdparty/manifold/src/csg_tree.h:80`).

### Picking contract

- Hover uses the last published result.
- A gesture captures its starting CSGSurfaceHit.
- The active gesture does not repick the changing boolean result every frame.
- Its ghost is the visual authority.
- A new topology-dependent gesture cannot begin from an outdated published generation.
- Navigation and cancellation remain available while evaluation catches up.

## 16. Face selection

Face selection belongs to the CSG session.

Behavior:
- Hover maps a result triangle to a CSGSurfaceHit.
- Clicking selects its CSGSurfaceKey.
- Clicking a face may make its source operand the active node in EditorSelection, but the face itself never enters EditorSelection.
- Shift-click toggles semantic surface selection when no drag threshold is crossed.
- Shift-drag starts extrusion after crossing the drag threshold.
- Paint operations may apply to multiple selected surfaces.
- Push/pull initially operates on the active surface only.
- Invalid keys are pruned after schema or scene changes.
- Different panes may hold different face selections.

Selected semantic surfaces can highlight all currently visible fragments. Hover highlights only the connected fragment under the pointer.

## 17. Surface tool

Surface is the default CSG tool.

### Box push/pull

For a box face:
- Drag is constrained to the face normal.
- Modify box dimensions and position, not Node3D.scale.
- The opposite face remains fixed.
- Alt-drag performs symmetric resizing around the box center.
- Prevent zero or negative dimensions.
- Preserve the existing transform, including rotation and ordinary scale.

For a local face displacement d:

- one-sided: `size_axis += d; local center_axis += d / 2`
- symmetric: `size_axis += 2d; center unchanged`

The implementation must correctly transform the normal and displacement for rotated or nonuniformly scaled operands.

### Snapping

Snap the absolute face-plane coordinate rather than accumulated drag delta.

Supported spaces: Operand local, Active CSG root, World.

For an arbitrarily oriented plane, snap its scalar coordinate along the selected normal. This avoids incremental drift.

Use the existing viewport snap setting. Temporary snap inversion should follow Godot's configured convention rather than hard-coding a conflicting modifier.

### Precision entry

During a drag:
- Show signed distance and resulting plane coordinate.
- Allow direct numeric entry.
- Escape cancels.
- Enter or click release commits.

### Undo

A gesture captures starting values once. Live updates do not create per-frame undo entries.

Commit creates one undo action: Do = final values, Undo = starting values. Cancellation restores the starting values without creating history.

## 18. Shift-drag extrusion

MVP extrusion supports outward extrusion of a box semantic face.

### Gesture

- Shift-press selects/captures the face.
- Crossing the drag threshold enters extrusion.
- Drag depth is constrained to the outward face normal.
- Snapping and numeric entry match push/pull.
- A view-local prism ghost appears immediately.
- The scene tree is not structurally changed until commit.

### Structural representation

Commit creates one ordinary CSGBox3D union child under the source shape:

`source subtree := source subtree UNION extrusion box`

The source node keeps: its parent, name, owner, operation, NodePath, script and animation references.

Because the source subtree is evaluated before the parent applies the source node's operation, this also expands a subtractive cutter correctly.

The new child receives: union operation, deterministic local transform and dimensions, a unique readable name, the correct edited-scene owner, ordinary serialized node data, per-surface settings derived from the source face.

### Surface inheritance

- Outward cap inherits the source material and planar UV alignment.
- The joining face is internal after union.
- Side faces use the source material by default.
- Side planar UVs align to the active root grid and inherit meters-per-tile.
- The new cap becomes the active surface after publication.

### Unsupported cases

If a case cannot be represented as a union child without changing boolean meaning, it is unavailable in the MVP. Do not silently reparent existing nodes.

Visible-fragment extrusion, arbitrary polygons, inset, and combiner interposition are later work.

### Undo acceptance

One undo action covers: child creation/removal, owner assignment, naming, node selection refresh, CSG face-selection reconciliation.

Subtractive extrusion must undo with the source's operation, NodePaths, ownership, and node selection intact.

## 19. Future hierarchy transactions

If a future tool must insert a combiner or reparent nodes, first extract a narrow document-bound scene-tree transaction service shared with SceneTreeDock.

It must handle: read-only and inherited-scene validation, node creation, reparenting and ordering, owner restoration, transform preservation, NodePath property updates, animation track path updates, selection restoration, live-debug notifications, one atomic undo action.

The path-update algorithms already exist publicly, while _do_reparent() itself is private and dock-owned (`editor/docks/scene_tree_dock.h:215`, `editor/docks/scene_tree_dock.h:363`).

CSG code must not call the global SceneTreeDock singleton, especially in this fork's document-bound multi-pane model.

## 20. Per-surface material model

Each primitive stores surface overrides indexed by its semantic schema.

Resolution order: surface material override → node material → existing/default fallback.

Properties are node-resident and serialized, for example:

```
surface_settings/0/material
surface_settings/0/uv_mode
surface_settings/0/uv_space
surface_settings/0/meters_per_tile
surface_settings/0/offset
surface_settings/0/rotation
surface_settings/0/texture_lock
```

Internally, the node uses typed surface-setting records. Dynamic indexed properties provide serialization and Inspector integration.

Changing a surface material:
- Does not rebuild local Manifolds.
- Does not rerun the boolean.
- Regroups output triangles by resolved material.
- Updates the render mesh and relevant editor previews.

This naturally supports different materials on subtractive cutter faces because their output provenance points back to cutter surfaces.

Existing scenes with no overrides retain current single-material behavior.

## 21. Focused UV toolset

The CSG UV system should solve architectural projection, not arbitrary unwrapping.

### Supported projection modes

- Existing/legacy UV behavior.
- Planar projection.
- Material-driven triplanar remains available and unchanged.

### Planar spaces

- Operand Local: projection moves with the operand.
- CSG Root: stable across operand edits and moves with the root.
- World: fixed to world coordinates; root transform changes require UV finalization but not a boolean rebuild.

### Stored settings

- Meters per tile on both planar axes.
- Offset.
- Rotation.
- Optional axis/basis override where needed.
- Texture lock.
- Schema-defined deterministic default basis.

### Paint/UV operations

- Assign material.
- Planar project in Local, Root, or World space.
- Set meters per tile.
- Offset, rotate, and scale.
- Align to face.
- Align to root/world grid.
- Fit.
- Reset.
- Copy/lift settings from a surface.
- Apply settings to selected surfaces.
- Texture lock during push/pull and extrusion.

World/root-space mappings are naturally locked. Local mappings adjust their stored offset during geometry edits when texture lock is enabled.

UV changes invalidate output UVs and tangents only. They do not invalidate the boolean unless the user explicitly chooses legacy interpolated input UV behavior.

### Explicit non-goals

Seam marking, island editing, packing, relaxation, arbitrary unwraps. Those remain post-bake mesh workflows.

## 22. Paint tool

Paint owns material and UV application.

Interactions:
- Hover previews the target semantic surface.
- Click applies the active material/settings.
- Eyedropper/lift reads settings from the hovered surface.
- Apply writes lifted settings to all selected surfaces.
- Multi-selection supports batch alignment and material assignment.
- A contextual panel exposes material and UV controls.
- Continuous property changes merge into sensible undo actions.

Asset drag-and-drop directly onto faces is useful but deferred until its interaction with existing viewport drag/drop is reviewed.

## 23. Draw tool

Draw is delivered after Surface, extrusion, and Paint are stable.

Initial workflow:
1. Choose Box.
2. Choose Add or Cut explicitly.
3. Drag a snapped rectangle on the current workplane or visible planar face.
4. Drag or type height.
5. Commit one CSGBox3D.

Modifier behavior:
- Add is the default.
- Holding Ctrl temporarily inverts Add/Cut.
- The explicit Add/Cut state remains visible; the modifier is only a temporary inversion.
- Escape cancels the current phase.
- RMB remains native navigation/context behavior.

Later additions: rectangle-on-face extrusion, cylinder drawing, polygon/path drawing, reusable workplanes, duplicate-and-drag workflows.

## 24. Operand tool

Operand temporarily returns node-level editing to the native viewport: native node selection, native transform gizmos, operation editing through Inspector/context controls, Scene Tree reordering, visibility and source/cutter inspection.

Tab toggles Surface and Operand when the viewport has keyboard focus and no text field is active. The shortcut remains configurable.

## 25. Undo, multi-pane synchronization, and scene changes

### Undo/redo

All model changes use ordinary node properties and editor undo actions.

- Push/pull: one property action.
- Material/UV edits: property actions.
- Extrusion: one structural action.
- Draw: one node-creation action.
- Cancellation creates no history.
- Undo/redo requests a new CSG generation.
- Stale asynchronous results are discarded normally.

### Multiple panes

- CSG root evaluation and caches are model-owned and shared.
- Mode, active tool, face selection, hover, ghost, and contextual UI are view-owned.
- Only the active pane mutates the scene.
- Other panes receive the published model result and reconcile their transient keys.
- Scope cues and ghosts never appear in another pane unless that pane independently enables them.

### Scene edits from elsewhere

If scripts, the Inspector, undo, or another editor operation changes the CSG tree:
- Invalidation uses the same module cache graph and scheduler.
- Sessions observe the new generation.
- Invalid face keys are pruned.
- An active gesture is canceled if its source node/schema becomes invalid.

## 26. Serialization and compatibility

- Surface settings serialize on primitive nodes.
- Stable semantic surface constants are independent of triangle ordering.
- A serialized surface-schema version supports future migration.
- ObjectID, origin token ranges, result generations, faceID, and editor selections never serialize.
- Generated extrusion nodes are normal visible scene nodes.
- Existing scenes without new properties produce unchanged behavior.
- Existing single-material scenes continue to use the node material.
- Headless and runtime CSG builds understand surface materials and UVs without editor code.
- Old scenes should not be rewritten merely by opening them.

## 27. Diagnostics and secondary improvements

The foundation should later support: Add/Subtract/Intersect color coding; editable cutter/source visualization; isolate active root; show final result, source operands, or both; empty-result and invalid-manifold diagnostics; coincident-surface/tolerance warnings; identification of the operand involved in a failed build; boolean rebuild statistics in development builds; improved hard-edge and smoothing preservation using semantic surfaces and faceID; copy/paste surface settings between nodes; workplane management; linked bake-to-mesh workflow; a one-click final static mesh bake preserving materials and UVs; better operation/order affordances in the Scene Tree; cylinder and polygon semantic push/pull; bevel, inset, and visible-fragment extrusion only after the core model proves stable.

## 28. Implementation sequence

### Phase 0 — Characterization and counters

Add development-only counters for:
- Local primitive packs.
- Transformed-wrapper construction.
- Expression-node reconstruction.
- Lazy operation evaluations/cache misses.
- Root materializations.
- UV/tangent finalizations.
- Collision rebuilds.
- Scheduler requests, completions, coalesces, and stale drops.

Add behavior-characterization tests for current: operation ordering, nested transforms, Add/Subtract/Intersect, material propagation, empty combiners, visibility, existing UVs, collision/AABB behavior.

No behavior changes.

### Phase 1 — Persistent Manifold cache graph

- Cache local Manifolds.
- Cache transformed child wrappers.
- Retain strong subtree handles.
- Compose nested Manifold expressions recursively.
- Materialize only at the root.
- Preserve current boolean semantics exactly.
- Implement granular invalidation.

Acceptance:
- Transforming an operand repacks zero leaves.
- Resizing one primitive repacks one leaf.
- A deep-tree change rebuilds/evaluates only the changed authored node and its ancestors.
- Clean subtree handles remain identical.
- Ordinary non-editor CSG editing is faster or neutral.

### Phase 2 — Semantic provenance

- Define primitive surface schemas.
- Allocate stable per-schema origin ranges.
- Pack runs by semantic surface.
- Supply meaningful input faceID.
- Carry token maps through nested expressions.
- Resolve output triangles to CSGSurfaceKey.
- Build generation-bound result picking metadata.

Acceptance:
- Surface keys survive transforms, geometry edits, and nested booleans.
- Cylinder sides resolve to one semantic surface across multiple faceID values.
- Materials can be resolved from provenance without boolean repacking.
- No editor side table is required to reconstruct source identity.

### Phase 3A — Document surface registry

- Introduce provider/instance/registry.
- Migrate built-in surface creation and cleanup.
- Replace hard-coded construction routing.
- Retain existing behavior and ownership.

Acceptance:
- A test surface can be registered without editing DocumentView.
- Existing views retain activation, cleanup, toolbar, and parking behavior.
- Two panes can create independent instances.

### Phase 3B — Edit-domain host and chrome

- Introduce registry, provider, host, and session.
- Add tri-state input arbitration.
- Add center-left/right chrome slots.
- Add contextual Control mounting.
- Add a dummy test domain.
- Integrate one generic seam before gizmo/selection handling.

Acceptance:
- Dummy domain can claim LMB, draw an overlay, supply tools, and mount a panel without viewport-core changes.
- Navigation remains native.
- Two views hold independent sessions.
- Provider removal cannot leave stale session/chrome pointers.

### Phase 4 — Scheduler and box Surface tool

- Split evaluation/finalization.
- Add single-flight worker scheduling.
- Add generation coalescing and stale-result rejection.
- Add last-result picking.
- Implement box hover/select.
- Implement push/pull, normal handle, snapping, numeric input, cancellation, and undo.
- Add ghosts and mode scope cues.

Acceptance:
- Pointer feedback never waits for a boolean.
- No boolean executes synchronously in the drag input handler.
- One running job maximum per root.
- One hundred rapid requests eventually publish only the newest final state.
- Rotated and nonuniformly scaled boxes edit correctly without changing node scale.
- Two panes do not share hover, selection, or ghosts.

### Phase 5 — Shift-drag box extrusion

- Add extrusion gesture.
- Add immediate prism ghost.
- Commit a union child beneath the source shape.
- Inherit surface settings.
- Integrate node ownership and undo.
- Select the new cap after publication.

Acceptance:
- Additive and subtractive operands extrude correctly.
- Existing source NodePaths and operation remain unchanged.
- Undo/redo restores geometry, ownership, and selection.
- No existing node is reparented.
- Read-only/inherited nodes reject the operation cleanly.

### Phase 6 — Surface materials and planar UVs

- Add node-resident indexed settings.
- Add output material regrouping.
- Add planar UV generation.
- Add Paint tool and contextual panel.
- Add copy/lift/apply, alignment, fit/reset, and texture lock.
- Regenerate tangents without rerunning booleans.

Acceptance:
- .tscn round-trip retains all settings.
- Old scenes remain unchanged.
- Material and planar UV edits cause zero boolean evaluations.
- Cut faces receive the cutter surface's material/UV settings.
- Push/pull and extrusion preserve locked texture alignment.

### Phase 7 — Draw

- Add workplane/face rectangle picking.
- Add Box drawing.
- Add explicit Add/Cut and temporary Ctrl inversion.
- Add height drag and numeric entry.
- Add node creation/undo.

### Phase 8 — Long tail

Cylinder and polygon surface editing, source/cutter visualization, diagnostics, linked baking, workplanes, advanced extrusion cases, performance-driven wide-tree optimizations, additional CSG primitives.

## 29. Required test matrix

### Module tests

- Origin token and faceID round-trip through union, subtraction, and intersection.
- Nested transform provenance.
- Geometry change with stable semantic key.
- Schema-generation invalidation.
- Material-only and planar-UV-only invalidation.
- Deep-tree cache-hit behavior.
- Flat wide-tree benchmark baseline.
- Operation-order compatibility.
- Deterministic final output.
- Root deletion with queued evaluation.
- Undo during an in-flight evaluation.
- Latest-generation publication.
- Collision only on final quality.

### Editor tests

- Toolbar and double-click activation.
- Escape lifecycle.
- Tab Surface/Operand toggle with focus rules.
- Native RMB/MMB/navigation.
- Native gizmos suppressed or restored appropriately.
- Face selection separate from EditorSelection.
- Different roots and non-CSG Scene Tree selection.
- Two panes showing one scene.
- Two panes showing different scenes.
- Hover and ghost confinement.
- Push/pull snapping in Local, Root, and World spaces.
- Numeric commit and cancellation.
- Additive/subtractive extrusion.
- NodePath and selection preservation.
- Context panel lifecycle.
- Provider unregister and view destruction.

### Serialization tests

- New per-surface settings round-trip.
- Old scenes load without new stored defaults.
- Generated extrusion children save and reload.
- Headless CSG output matches editor final output.
- Surface schema migration tests when the schema version changes.

## 30. Performance acceptance

Exact millisecond targets should be set after Phase 0 establishes a reference scene and hardware baseline. The architectural gates are fixed:

- No synchronous boolean in pointer-motion handling.
- Transform edits repack zero local primitives.
- One primitive geometry edit repacks one local primitive.
- Clean authored subtrees retain their lazy caches.
- At most one evaluation job per root.
- Pending interactive requests coalesce.
- Final quality cannot be permanently starved by continuous edits.
- Material and planar UV changes run zero booleans.
- Collision does not rebuild during interactive drag.
- Cache memory is bounded to current state, the in-flight snapshot, and the latest pending state.
- Old snapshots are released after their jobs finish or are discarded.

## 31. Principal risks and mitigations

| Risk | Mitigation |
|---|---|
| Selection breaks after rebuild | Semantic CSGSurfaceKey; never store token/faceID as selection |
| Nested provenance disappears | Preserve Manifold expressions until root materialization |
| Cache silently stops helping | Strong subtree handles, dirty-path construction, development counters |
| Concurrent lazy-cache race | Single-flight evaluation per root |
| Continuous drag never catches up | Coalescing, ghost authority, final request priority |
| Stale face picked | Generation-bound hits and gesture-start gating |
| Extrusion breaks NodePaths | MVP union-child representation; no existing-node reparenting |
| Native viewport conflicts | Explicit domain boundary and tri-state arbitration |
| Two panes contaminate each other | Model-owned scheduler, view-owned session and overlays |
| Generic API grows speculatively | Add provider capabilities only when two clients require them |
| Material/UV edits remain expensive | Separate output finalization invalidation from boolean invalidation |
| Manifold upgrade changes behavior | Characterization and provenance/cache contract tests |

## 32. Explicitly deferred

- Full vertex/edge mesh editing.
- Arbitrary mesh-face push/pull.
- Full UV unwrap editor.
- Visible-fragment extrusion.
- Inset, bevel, and loop operations.
- Automatic combiner interposition.
- Speculative generic panel schemas.
- Persistent balanced boolean-reduction trees without benchmark evidence.
- Global material mutation for scope dimming.
- A separate CSG main screen.

This is the frozen plan: provenance and evaluation belong to the module; interaction belongs to per-view sessions; presentation belongs to host-bound chrome; document surfaces and edit domains use parallel provider/instance patterns.
