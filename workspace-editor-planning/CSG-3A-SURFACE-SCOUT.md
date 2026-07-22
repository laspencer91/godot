# Phase 3A scout — DocumentView surface construction/lifecycle map

Read-only reconnaissance (2026-07-22) preparing the EditorDocumentSurfaceRegistry refactor (CSG-EDIT-PLAN.md §4, Phase 3A). All file:line refs verified at scout time.

## Orientation

- Model: `editor/editor_document.h` (`EditorDocument` + subclasses, C++-only, not `Object`). View state: `EditorDocumentView` (same file, :306).
- Per-pane presentation node: `DocumentView` (`editor/gui/document_view.cpp/.h`, a `MarginContainer`).
- Tab container that mints/frees DocumentViews: `TabbedDocumentHost` (`editor/gui/tabbed_document_host.cpp`).
- Existing factory/registry to imitate: `EditorViewportChromeRegistry` (`editor/gui/editor_viewport_chrome.{h,cpp}`).
- The construction switch: `DocumentView::DocumentView` at `document_view.cpp:287` (`switch (type)`), scene-composite second phase at :360. Pre-delete cleanup: `_notification(NOTIFICATION_PREDELETE)` at :637.

## Per-surface-type breakdown

### TYPE_RESOURCE — document_view.cpp:288
- Constructs `memnew(InspectorDock(ed, false))` (bound instance, is_global=false), `set_bound_document`, `edit_resource_document(rd->get_resource())`. `editor_surface = inspector_dock`.
- InspectorDock is owned per-view. Only shared dep: `EditorNode::get_editor_data()`.
- Activation: `set_context_active` → `inspector_dock->set_context_active(p_active)` (:606).
- No special parking/cleanup; freed by Node teardown.
- `ResourceDocument` keeps the exact `Ref<Resource>` (editor_document.h:201); `scene_context_document` (:203) keeps a scene context-active while the resource tab is focused (used by `_activate_document`, tabbed host :231).

### TYPE_SCRIPT — document_view.cpp:296
- `ScriptEditor::get_singleton()->create_editor_view(sd->get_script_resource())` → `ScriptEditorBase`.
- Factory: `script_editor_plugin.cpp:2058`; mints via `script_editor_funcs[]`, `_register_view(seb)` (:2077) → `registered_views` + `tree_exiting → _unregister_view` (:762). Wires ~10 signals onto the singleton. Restores caret/scroll from `script_editor_cache` by path (:2130).
- View is per-tab owned; ScriptEditor singleton holds registration + shared chrome (`menu_hb`, `find_replace_bar`).
- Activation: `set_current_surface(view)` from `TabbedDocumentHost::_sync_current_script_view` (:252) → `script_editor_plugin.cpp:429`; dedups by `current_surface_id`, then `_mount_chrome` (:482, reparents chrome into `p_view->get_chrome_host()` = DocumentView `content_vbox`, document_view.cpp:542) vs `_park_chrome` (:497, homes `menu_home`/`find_bar_home`). Shortcut contexts re-pointed via `_set_chrome_shortcut_context`.
- Cleanup (PREDELETE, document_view.cpp:654/:678): `se->park_chrome_if_hosted_by(this)` then `se->release_editor_view(seb)`.
- User-close side effects: `TabbedDocumentHost::_drop_tab_at` (:356) calls `se->notify_surface_closing(surface)` (script_editor_plugin.cpp:810) BEFORE memdelete (state cache/previous scripts). NOT called on editor-shutdown teardown.

### TYPE_SHADER — document_view.cpp:304
- `ShaderEditorPlugin::get_singleton()->create_editor_view(shd->get_shader_resource())` → `ShaderEditor`.
- Factory: `shader_editor_plugin.cpp:77`; language plugin picks text vs visual widget; tracks in `edited_shaders` (:124); zoom signals onto plugin singleton.
- Shared traveling control: plugin's File menu (`file_menu`). Activation: `set_current_surface` (:160) dedups by `current_shader_editor_id`, mounts via `se->use_menu_bar(file_menu)` or parks.
- Cleanup (PREDELETE, document_view.cpp:685): `sep->release_editor_view(she)` (:128) — `_park_file_menu` if hosted here (:146), clears current id, drops `edited_shaders` entry.

### TYPE_HELP — document_view.cpp:313
- `ScriptEditor::get_singleton()->create_help_view(hd->get_class_name())` → `EditorHelp`; then `go_to_class` via `call_deferred` (needs in-tree for theme+doc data).
- Factory: script_editor_plugin.cpp:2147; separate registry `registered_help_views` (:2155), self-heals via `tree_exiting → _unregister_help_view`.
- Shares the SAME ScriptEditor chrome as scripts (`is_ours`, :433). PREDELETE `release_editor_view` branch is script-only (cast fails for EditorHelp) — help relies on tree_exiting.

### TYPE_SCREEN_HOST — document_view.cpp:324 (biggest special case)
- Mints NOTHING. Adopts `shd->get_screen_stack()` (= `EditorMainScreen::main_screen_vbox`, set at editor_main_screen.cpp:663) by removing from its current parent (:331).
- The stack is a process-wide singleton container (all legacy main screens).
- Parking: hidden in-tree `screen_park_holder` Control created in EditorMainScreen ctor (:645), ObjectID stored on doc via `set_park_holder_id` (:664). Invariant "D11": `EditorMainScreen::get_control()` always returns a live vbox.
- Cleanup (PREDELETE, document_view.cpp:694): re-park to holder via ObjectDB::get_instance, then NULL `document_surface` and `editor_surface` (:701-702) so Node teardown does not free the shared stack. If holder already gone (whole-editor teardown), deliberately leaves stack to die with the view. Holder is added BEFORE the workspace (editor_main_screen.cpp:642-648) to guarantee teardown ordering — load-bearing.

### TYPE_SCENE_2D / 3D / MIXED — document_view.cpp:337/:341/default :346
- `_create_scene_surface(p_2d)` (:248) → `CanvasItemEditor::get_singleton()->create_view_bound_to(document)` (canvas_item_editor_plugin.cpp:5959) or `Node3DEditor::get_singleton()->create_view_bound_to(document)` (node_3d_editor_plugin.cpp:2834). Views bind to the document's isolated World3D/World2D; 3D forces VIEW_USE_1_VIEWPORT (:2845); each view owns an `EditorViewportChrome` (:2799).
- Scene-composite second phase (:360, gated on `p_document->get_selection()`):
  - `bound_scene_document`; `scene_surface_2d`/`scene_surface_3d` retained lazily once minted (each keeps its camera/pan state — `set_scene_view_2d` :557).
  - Right dock column, all owned per-view bound instances in FoldableContainer accordion: SceneTreeDock (:378), InspectorDock (:383), SignalsDock (:408), GroupsDock (:411); inspector lock + target label (:388-406).
  - `toolbar_host` HBox (:425) — mount slot for the shared 2D/3D toolbar.
  - `scene_surface_stack` MarginContainer (:434).
  - Optional Animation drawer: `AnimationPlayerEditorPlugin::get_singleton()->create_editor_view(p_document, inspector_dock)` (:442; factory animation_player_editor_plugin.cpp:2745), in `DocumentBottomDockHost`; state from `document->get_contextual_editor_state("Animation")` (:451).
  - `editor_surface = scene_split`.
- Shared 2D/3D toolbar: ONE control (`get_shared_toolbar()` = `main_flow`, node_3d_editor_plugin.cpp:2733 / canvas_item_editor_plugin.cpp:5980). Mounted by `EditorNode::update_scene_pane_toolbar` (editor_node.cpp:4783, the ONLY reparent site), parked via `park_shared_toolbar` (:2737/:5984, re-points shortcut contexts). Driven by tab-select (`_sync_current_script_view` → update_scene_pane_toolbar, tabbed host :260) and pane focus.
- Activation: `set_context_active` (document_view.cpp:595) fans to doc_view set_active, CanvasItemEditorView set_context_active, inspector_dock, animation editor (stores drawer state on deactivate).
- Cleanup (PREDELETE): `set_context_active(false)` (:640); `scene_tree_dock->set_bound_inspector(nullptr)`; animation `_store_animation_drawer_state()` + `release_editor_view` (:646); toolbar-park block (:659-672). Owned *EditorView freed by Node teardown; its dtor (node_3d_editor_plugin.cpp:2803) erases itself from `editor_views`.
- Default branch (:346): unclassified SceneDocument with selection still gets scene surface; truly unknown doc → bare 3D view. NOTIFICATION_READY (:628) re-points embedded scene tree when root loads late.

## TabbedDocumentHost lifecycle

- Parallel arrays `documents[]`/`views[]`. `_ensure_view` (:175) memnews DocumentView hidden; `ensure_document` (:149) mints even for background opens (registrations must exist before first show).
- `_show` (:187): deactivate outgoing (`set_context_active(false)`), show one, hide rest (hidden SubViewports stop rendering).
- `_on_tab_selected` (:263): gated on `is_inside_tree() && !suppress_activation` AND focused-pane check (:270) — background/programmatic tab changes must NOT steal shared toolbar/chrome. Critical seam.
- Destruction: `_drop_tab_at` (:353) — notify_surface_closing, memdelete(view), `_remove_tab_entry` under suppress_activation. `detach_tab`/`adopt_tab` (:391/:401) move a live view between hosts WITHOUT close side effects (drag-to-split).
- Registry touchpoints: `_ensure_view`, `_drop_tab_at`, `detach_tab`, `adopt_tab`.

## Registry pattern to imitate

- `EditorViewportChromeRegistry`: Registration object (RefCounted; editor_id/scope/slot/Callable factory/order; auto-unregister in dtor), manual singleton create/free/get_singleton, `register_control_factory` returns Ref<Registration> (.cpp:75). Late binding both directions (register attaches to live chromes :92; add_chrome pulls all registrations :162). Factory called via Callable with context Dictionary; validates returned Control has no parent (.cpp:108-127). Public entry: `EditorPlugin::add_control_to_viewport_chrome` (editor_plugin.h:190).
- Plan wants a typed `EditorDocumentSurfaceProvider` (supports() + create(context) → Instance) instead of raw Callable.
- Smaller per-document tracking pattern worth mirroring: animation plugin `create_editor_view`/`release_editor_view` + `document_editors` HashMap (animation_player_editor_plugin.cpp:2745/:2758).

## Naming/placement conventions

- Workspace/pane infra Controls → `editor/gui/` (document_view, tabbed_document_host, editor_workspace, editor_viewport_chrome, pane_drop_overlay, document_bottom_dock, editor_main_screen).
- Model/service classes → `editor/` root (editor_document.{h,cpp}).
- Reusable semantic docks → `editor/docks/`. Editor surfaces/plugins under their domain dirs.
- New registry fits `editor/gui/`; concrete providers live beside their editors.
- Comment convention: "why" comments tagged by milestone (G2 S7, M7.2a, G-Shader, D11) documenting seams/invariants — PRESERVE these tags in the refactor.

## Seams a registry MUST preserve exactly

1. Ownership split — surface owned, service singleton retained. Instance cleanup MUST call matching release/unregister at PREDELETE and keep the singleton alive.
2. Shared traveling controls have a home + park op: ScriptEditor chrome (`park_chrome_if_hosted_by`), Shader File menu (`release_editor_view`→`_park_file_menu`), 2D/3D toolbar (`park_shared_toolbar`), each guarded by ancestor/parent check. Instance needs a pre-delete/deactivate hook that runs while the surface is still alive (derived-first PREDELETE ordering, document_view.cpp:650 comment).
3. ScreenHost never-own + re-park + null-out (:701). Holder-before-workspace ordering is load-bearing.
4. Activation dedup + focused-pane gate (:270) — route activation through the same gate.
5. State restoration timing: script caret/scroll in factory; Animation drawer from contextual_editor_states (:451); help go_to_class deferred until in-tree (:320); scene-tree root re-point on READY (:628). Providers must reproduce the pre-tree vs post-tree split.
6. Two distinct close paths: user close (notify_surface_closing before memdelete) vs teardown (PREDELETE + tree_exiting). Both must remain.

## Hazards

- Global singleton assumptions null-checked but produce silent null surfaces — registry should decide availability via supports() instead.
- Order dependencies: derived-first PREDELETE; park-before-free; screen_park_holder before workspace; deferred calls requiring in-tree.
- `document_surface` vs `editor_surface` aliasing (document_view.h:74-78): keep "root Control" and "concrete surface for lifecycle/context" distinct, as today.
- Shared-toolbar dangling: documented crash (DIVERGENCE-LEDGER:108) if a focused scene pane's view frees without parking — Instance cleanup ordering is safety-critical.
- Selection/gizmos still global: scene surfaces render per-world but edit only while context-active.
