# G7 - Contextual bottom drawers in the workspace model

**Status:** Animation and the Level Editor's Materials browser are document-owned. AnimationTree
has the hardened global binding from the earlier G7 pass but has not yet moved into the document
host. TileMap / TileSet, Theme,
SpriteFrames, Polygon2D, ResourcePreloader, and MeshLibrary still follow their stock loose binding.

## Decision (owner, 2026-07-15)

Contextual editors for normal 2D/3D scene documents live in a **document-owned bottom drawer**,
not in the full-width global bottom panel. The right Scene Tree / Inspector accordion remains full
height; the drawer is nested under only the viewport side of the scene split. This makes ownership
unambiguous when multiple panes are visible and naturally moves viewport-bottom overlays, including
the floating Camera Preview, above an open drawer.

Each available contextual editor gets a compact icon toggle at the right side of that document's
scene toolbar. The toggle remains visible and pressed while its drawer is open, so the same target
both opens and closes it. The drawer header also has an explicit close button. This avoids a second
floating button row and leaves one discoverable, stable control in both states.

Animation is the first implementation of the contract; the Level Editor's Materials browser is the
second. The generic host is deliberately capable of holding more contextual editors, but shows only
one drawer at a time.

## Document drawer contract

1. **The view owns the host.** `DocumentView` creates one `DocumentBottomDockHost` for a normal
   2D/3D scene. The host's split contains the concrete 2D/3D surface and one drawer stack. The outer
   scene split still owns the right accordion as its other child, so opening a drawer cannot shorten
   or cover that column.

2. **Toggles are document-local chrome.** The host adds a named toggle to that view's
   `toolbar_host`. The shared 2D/3D toolbar may travel in and out at child index zero; document
   toggles remain with their view. Every drawer also exposes a close button in its header.

3. **One contextual drawer is open per view.** Opening a different registered drawer closes the
   previous one and synchronizes both toggle states. Hidden drawers remain alive so editor state is
   retained without reconstructing their controls.

4. **State belongs to the document.** Open/closed state and editor-specific state are mirrored into
   `EditorDocument::contextual_editor_states`. The normal `EditorPlugin::get_state()` /
   `set_state()` path still round-trips the same dictionary for scene persistence, including when
   restore state arrives before the `DocumentView` has been constructed.

5. **Services are global; views are not.** A contextual editor plugin remains the global services
   and routing object, but mints one editor view for its owning `EditorDocument`. The old global dock
   is retired and disabled after it supplies legacy command registration/fallback services.

6. **All context is explicit.** A document editor binds to that document's root, selection,
   selection history, and paired Inspector. Child animation controls resolve their owning
   `AnimationPlayerEditor` through their node ancestry rather than reading a process-global editor
   singleton. This is required when two scene panes and two timelines are simultaneously visible.

7. **Focus changes routing, not ownership.** `DocumentView::set_context_active()` selects the editor
   instance that plugin callbacks and 3D key requests should use. Deactivation snapshots state.
   `NOTIFICATION_PREDELETE` unregisters the view before Node frees its children.

## Animation implementation notes

- Selecting an `AnimationPlayer`, `AnimationTree`, or `AnimationMixer` routes editing and the open
  request to the active document's Animation drawer.
- Dummy `AnimationPlayer` instances used for `AnimationMixer` editing are owned per drawer, including
  their signal connections and cleanup.
- Track, bezier, and library editors resolve the owning player, scene root, selection, and Inspector
  history from their ancestor drawer. Undo actions therefore retain the correct concrete editor even
  after focus moves to another pane.
- The drawer toggle is named `AnimationBottomDockToggle`; the body is named
  `AnimationBottomDockPanel`. These stable names are used by the workspace smoke coverage.

## Level Materials implementation notes (REMOVED 2026-07-16)

The Materials drawer was the second document-owned drawer, bound to the G-Level level editor's
`LevelDocument`/`MaterialBrowserDock`. The level editor was retired in favor of a Blender-based
workflow and stripped from master; the Materials drawer went with it. Its implementation (a useful
reference for future `DocumentBottomDockHost` migrations: per-document view minting/release,
`contextual_editor_states` round-tripping, virtualized horizontal shelf) is preserved on branch
`archive/g-level-editor`. Animation remains the sole in-tree reference implementation.

## Remaining rollout

AnimationTree is the next natural migration because it shares the animation selection machinery.
After that, TileMap / TileSet, Theme, SpriteFrames, Polygon2D, ResourcePreloader, and MeshLibrary can
follow the same factory, explicit-context, state, and release pattern. Until each moves, its hardened
global binding remains a transitional behavior rather than the target UX.

## Known limitation

Onion skinning captures viewport state through the `Node3DEditor` / `CanvasItemEditor` singleton
main views, which are unbound for pane-hosted scenes. Pane views render through their own
document-bound `SubViewport`s, and the pane 2D views do not forward the force-overlay. Both onion
controls therefore remain disabled with an explanatory tooltip until capture is made
document-view-aware.
