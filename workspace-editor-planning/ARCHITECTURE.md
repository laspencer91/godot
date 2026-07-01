# Workspace-editor architecture: the three-layer taxonomy

**Purpose.** Decide *once* what kind of thing every piece of editor state is, so
every extraction — the 3D editor (done), the 2D editor (next), the docks, the
inspector — sorts its members the same way. When you are mid-refactor and unsure
where a member belongs, this file is the tiebreaker.

The whole workspace-editor effort is one move: Godot's *runtime* scenes are
already instanceable, but the *editor* was built as a single structure that
assumes **one** current scene, **one** of each editor surface (via
`get_singleton()`), **one** selection, **one** history — and switches scenes by
*mutating those singletons*. We are un-conflating that single structure into
three layers with different cardinalities.

---

## The three layers

### 1. SERVICE — single / global (one per editor, keep behind `get_singleton()`)

State and behavior that is genuinely shared across every open document and every
pane. There is exactly one, it never holds render state for a specific document,
and it stays reachable via its singleton accessor so the ~120 external call
sites (gizmo plugins, tool code) don't churn.

- **Belongs here:** tool mode (select/move/rotate/scale), snap settings, the
  gizmo-plugin registry, the gizmo *meshes* (shared geometry), menus and their
  handlers, dialog owners, editor-settings-driven config.
- **Test:** "If I opened a second document in a second pane, would this be the
  same value for both?" If yes → service.
- **Example:** `Node3DEditor` is becoming the 3D *services* singleton. It owns
  tool mode, the gizmo registry, snap, menus. It must eventually hold **no**
  render state for any specific document.

### 2. VIEW STATE — many / instanceable (one per pane, `memnew` into any pane)

State bound to *a rendered view of a document in a pane*. There are N of these,
one per pane showing that kind of document. It owns render resources and their
lifecycle. This is the layer that makes multi-pane possible.

- **Belongs here:** the viewport(s) / quad, the camera(s), per-view grid/origin
  decoration and its RIDs, the world *binding* (which world this view renders),
  per-view gizmo cull-mask layer, view-layout (1/2/3/4-viewport split),
  maximize state, freelook state, per-view camera position/zoom/pan.
- **Test:** "Would two panes showing the same document each need their own copy?"
  If yes → view state. (Two panes on one scene each have their own camera angle.)
- **Example:** `Node3DEditorView` (done). It owns `viewports[]`, `viewport_base`,
  `bound_world`, the grid/origin decoration + its resource lifecycle
  (create-detached → reconcile-on-settled-frame → free-in-dtor), `last_used`,
  `freelook`. Its 2D sibling will be `CanvasView2D`.

### 3. DOCUMENT STATE — one per open document (the model)

State that *is* the document: its identity and its edit model, independent of how
many panes are looking at it (including zero).

- **Belongs here:** path, root node, scene/script/resource type, dirty flag,
  undo/redo history id, **and — once split — the render/physics world** (a scene
  document owns a World3D + World2D; a script document owns neither), and
  **selection** (per-document, so an inactive pane's gizmos aren't stale).
- **Test:** "Does this survive when the document has no open pane, and is it the
  same regardless of which pane is active?" If yes → document state.
- **Current reality / known strain:** `EditorDocumentContext` is today a single
  class that unconditionally owns a `World3D` *and* carries a `TYPE_SCRIPT` /
  `TYPE_RESOURCE` enum (a script document owning a 3D world) *and* holds
  `editor_states` + `active` — which its own header annotates as view state
  living on the model. **Planned split (Step ④ opener):**
  - `EditorDocument` — slim: identity, path, root, history id, dirty.
  - `SceneDocument : EditorDocument` — owns the worlds (SubViewport/World3D/World2D).
  - `EditorDocumentView` — per-pane: `editor_states`, `active`, bound pane.
    (This mirrors `Node3DEditorView` at the document layer — do it while the
    class is ~120 lines, not 1200.)

---

## The seam rules (how the layers talk)

1. **Services never hold render state.** Any render/world/decoration state on a
   services singleton is a temporary parking spot to be moved to the view. When
   `Node3DEditor` is fully drained of render state it can be renamed/documented
   as `Node3DEditorServices` (cosmetic, optional).

2. **`friend` is a temporary bridge, not the boundary.** Where a service reaches
   into a view's private state via `friend class` (e.g. `Node3DEditor` reading
   `Node3DEditorView::bound_world`) that access is a *promissory note*: the
   endgame is a per-view public API, and every such friend read is a site to
   migrate to it. Friend works at 2 views and rots at N. Don't add new friend
   reaches without noting them here.

3. **"Which document?" is resolved from the acting UI, not from ambient global
   state.** `EditorNode::get_scene_root()` → "the active document" is fine for
   v1, but breaks the moment two panes are live. The endgame is a single choke
   point — `EditorContext::for_control(Control *)` walks up to the owning
   `WorkspacePane` and returns its document/view, falling back to the active
   document. Migrate call sites to it opportunistically during work you're
   already touching, so the fallback shrinks instead of a big-bang migration.

4. **Isolation across documents is free; only disambiguate within a world.**
   Each document has its own scenario, so cross-document render state cannot
   collide. Anything that uses a *global* budget to disambiguate views should be
   keyed **per-world** — the budget becomes "N views of the *same* document"
   (never hit) instead of "N panes total" (a dual-monitor user hits it).
   **DONE for the gizmo cull-mask freelist** (Step④ commit): `Node3DEditor`
   now holds a `HashMap<scenario-id, mask>` and `allocate/free_gizmo_layer` take
   the world; the layer is claimed in the world-binding lifecycle
   (`set_editor_world`: free the old world's layer, claim from the new world's,
   rewrite the camera cull mask + gizmo-instance layer masks) rather than at
   viewport construction. Apply the same per-world keying to any future
   global-budget disambiguation.

---

## Applying this to the 2D editor (next)

`CanvasItemEditor` gets the same surgery: sort its members into service (tool
mode, snap, guides config, menus), view state (the viewport, pan/zoom, the
`CanvasView2D` to extract), and document state (which lands on the
document/`SceneDocument`). The 2D extraction is harder — there is no clean world
seam and the viewport is monolithic — but the taxonomy is identical, decided
here once.

---

## Open decisions to record (not yet resolved)

- **Inactive-document cost policy.** "All docs live at once" means every open
  scene ticks physics/processing forever (tension with G5). On `deactivate()`,
  candidates — all cheap and reversible on `activate()`, preserving "in-tree and
  warm, not simulating": `PhysicsServer3D::space_set_active(false)` on the doc's
  space, render-target update mode → `UPDATE_DISABLED` on its SubViewport,
  optional process-mode pause on the root. Decide the policy before it becomes a
  per-bug argument.
- **Deferred-first-bind invariant.** `Node3DEditorView` defers its first
  decoration reconcile one frame so freshly-created materials register with the
  RenderingServer before render. This currently encodes "usually works" timing.
  Either find the synchronous registration guarantee, or document exactly which
  RenderingServer command-queue behavior the one-frame wait relies on, so it is
  a load-bearing invariant rather than folklore.
