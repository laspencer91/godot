# G7 — Contextual bottom docks in the workspace model

**Status:** Animation and AnimationTree docks migrated. TileMap / Theme / SpriteFrames /
Polygon2D / ResourcePreloader / MeshLibrary still follow stock behavior.

## Decision (owner, 2026-07-09)

Selection-driven bottom editors stay in the **full-width global bottom panel** — they are not
per-pane and not "traveling chrome." Rationale: the animation timeline (and tilemap palette,
theme editor, …) need horizontal space; a per-pane drawer gets pane-width minus the ~400px
dock accordion, which is unusable in splits. The trade-off (a global surface showing state
for one specific pane) is solved by hardening the *binding* instead of moving the *host*.

## The binding contract

A selection-driven bottom dock is correct under the workspace model when it satisfies all of:

1. **No reliance on `node_removed` for scene switches.** Stock Godot unbound the edited
   object when the previous scene left the tree; resident documents never leave the tree.
   The dock must detect a binding that points outside `EditorNode::get_edited_scene()` and
   unbind explicitly. Trigger: the `EditorNode::scene_changed` signal (fires at the end of
   every document switch) and/or `NOTIFICATION_VISIBILITY_CHANGED`.
   *Animation:* `AnimationPlayerEditor::_find_player()` → `_unbind_player()`.
   *AnimationTree:* `AnimationTreeEditorPlugin::edited_scene_changed()` → `_unbind_tree()`.

2. **Per-scene state round-trip via plugin `get_state()`/`set_state()`.** The machinery
   already runs on every switch (`EditorNode::_set_current_scene_nocheck` →
   `save_edited_scene_state`/`restore_edited_scene_state`). Include a `"dock_open"` bool so
   each scene remembers whether the dock was open (apply it *before* any visibility
   early-out so a scene that left it closed also closes it).
   *Animation:* `AnimationPlayerEditor::get_state()/set_state()`; `EditorDock::is_dock_open()`.
   *AnimationTree:* `AnimationTreeEditor::get_state()/set_state()`.

3. **Boot restore gets a switch tail.** During session restore every `_set_current_scene`
   skips `_set_main_scene_state` (gated on `restoring_scenes`), so `scene_changed` /
   `notify_edited_scene_changed` never fired for the boot scene. Fixed centrally:
   `EditorNode::_notify_restored_scene_current()` (deferred from
   `_load_open_scenes_from_config`) fires the notification tail once after restore — new
   docks get this for free, but must bind off `scene_changed`, not off assumptions about
   load order.

4. **Bound-scene indicator.** A global panel over multiple visible panes is ambiguous;
   show the owning scene's short name in the dock's toolbar. Derive it from the bound
   node's owning edited scene (scan `EditorData` edited scenes for the ancestor root),
   NOT from `get_edited_scene()` — a pinned binding may belong to a non-active scene.
   *Animation / AnimationTree:* `_update_bound_scene_label()`.

5. **Pin-style overrides survive switches deliberately.** If the dock supports pinning a
   binding, the stale-binding unbind (contract #1) must skip pinned bindings; the indicator
   (#4) then tells the user they're looking at another scene's object.

## Known limitation

Onion skinning captures viewport state through the `Node3DEditor`/`CanvasItemEditor`
singleton main views, which are **unbound** for pane-hosted scenes — expected broken under
the workspace model. Confirmed from the implemented routing: pane views render through their own
document-bound `SubViewport`s, pane 2D views do not forward the force-overlay, and
`EditorPlugin::update_overlays()` refreshes only the singleton main views. Both onion controls
are disabled with an explanatory tooltip until capture is made document-view-aware.
