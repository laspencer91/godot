# Step ⑤b.4 → ⑤c: CanvasItemEditor services/view split — implementation plan

Status: **executing** (2026-07-02). Authored by an independent planning pass.
This is the plan for extracting `CanvasItemEditor` (monolith singleton) into a
services singleton + instanceable `CanvasItemEditorView`, mirroring the
`Node3DEditor`/`Node3DEditorView` split. Replaces the duplicated manipulation in
`canvas_view_2d.{h,cpp}` with the one real editing implementation, instanced N
times ("view-many, edit-active"). See STEP5b3-SELECTION-SCOPE.md for the #6
selection model (Model A) this preserves.

## End state & class names
- **`CanvasItemEditor`** — the 2D SERVICES singleton (tool, snap engine+config,
  grid config, toolbar/menus/dialogs, undo helpers, canvas-space queries,
  `editor_selection`). Keeps `get_singleton()` + byte-identical API by forwarding
  to `main_view`.
- **`CanvasItemEditorView : Control`** (new, same header/cpp, after
  `CanvasItemEditor`) — instanceable per-pane view: display stack, transform/
  zoom/view_offset, the whole `_draw_viewport` overlay path, the
  `_gui_input_viewport` drag machinery, and (Phase 4) an own SubViewport bound to
  a document's World2D. `CanvasItemEditor` owns one as `main_view`; mints pane
  views via `create_view_bound_to(EditorDocument*)`.
- **`CanvasView2D` is superseded and deleted** (Phase 4): its ~120-line render
  core (own SubViewport + set_world_2d + per-viewport global_canvas_transform)
  ports into `CanvasItemEditorView`; its reimplemented select/move/rotate dies.
  `DocumentView` ends up routing 2D → `create_view_bound_to`, 3D →
  `Node3DEditorView`, symmetric.
- **`_display_scene_root` + `scene_view_container` retired** (Phase 6);
  scene_root permanently parked under `documents_holder`.

Every phase: build (`py -m SCons platform=windows target=editor
redirect_build_objects=yes winrt=no -j24`) → smoke
(`bash workspace-editor-planning/smoke/run_smoke.sh`) → commit `G2 Step⑤b.4x…`.
Update the DIVERGENCE-LEDGER canvas_item_editor row (SHIM→STRUCTURAL) in Phase 1.

## Member disposition (decided once)
**SERVICE (stays on CanvasItemEditor):** `tool` + toolbar buttons/menus +
`_button_*` handlers; all `snap_*` config, `smart_snap_active`,
`grid_snap_active`, `use_local_space`; snap engine (`snap_point`, `snap_angle`,
`_snap_if_closer_*`, `_snap_other_nodes`, `snap_target[2]`, `snap_transform`);
grid config (`grid_offset/step/primary_grid_step/grid_step_multiplier`, grid
menu); keying (`key_pos/rot/scale`); bones (`pose_clipboard`, `bone_list`,
`bone_last_frame`, `skeleton_menu`); dialogs (`snap_dialog`, `selection_menu`,
`add_node_menu`, `selection_results_menu`, `selection_menu_additive_selection`);
shared textures (`select_sb/select_handle/anchor_handle`), colors, shortcuts;
panels/splits; undo helpers (`_save/_restore/_commit_canvas_item_state`);
canvas-space queries (`find_canvas_items_at_pos`, `_find_canvas_items_in_rect`,
`_get_encompassing_rect*`, `_get_edited_canvas_items`, `_is_node_locked`,
`_is_node_movable`, `_anchor_to_position`, `_position_to_anchor`); `message`,
`node_create_position`, `editor_selection`, `grab_distance`, `simple_panning`,
`tree_signals_connected`, `selected_from_canvas`, `had_visible_selection`, etc.

**VIEW (moves to CanvasItemEditorView):** display stack (`viewport`,
`viewport_scrollable`, `scene_view_container` [until P6], `h_scroll`, `v_scroll`,
`controls_vb`, `button_center_view`, `zoom_widget`); transform (`transform`,
`zoom`, `view_offset`, `previous_update_view_offset`, `updating_scroll`);
`panner`, `pan_pressed`, `resample_timer`; per-view visibility (`grid_visibility`,
all nine `show_*`); ruler tool (`ruler_tool_active`, `ruler_tool_origin`,
`ruler_width_scaled`, `ruler_font_size`); drag machinery (`drag_type`,
`drag_from`, `drag_to`, `drag_start_origin`, `drag_rotation_center`,
`drag_selection`, `dragged_guide_index/pos`, `is_hovering_h/v_guide`,
`box_selecting_to`, `temp_pivot`, `cursor_shape_override`, `original_transform`);
caches (`selection_results`, `hovering_results`); P4: `document`, `view_viewport`.

**Dead code:** `top_ruler`/`left_ruler` (h:383-384) — delete in P1.
**Temp bridges:** mutual `friend` (tagged `// Step⑤b.4 promissory note`).

## Phases (each an independently green commit)
1. **⑤b.4a — extract CanvasItemEditorView (display stack + pan/zoom).** The ③a.1
   analog. Forward-decl view; add `main_view` + `get_main_view()` +
   `_get_active_view()` choke point. Move display-stack + transform members +
   `_update_scrollbars/_update_scroll/_zoom_on_position/_update_zoom/
   _update_oversampling/_shortcut_zoom_set/_pan_callback/_zoom_callback`. View
   ctor builds subtree (editor ctor lines 5647-5745). Forwarders:
   `get_canvas_transform/get_viewport_control/get_scene_view_container/
   get_controls_container/update_viewport`. Compiler-driven reroute of ~208
   `viewport->`/`transform`/`zoom` refs as `main_view->X`. RISK: ordering (view
   ctor calls get_scene_root — safe, editor already does); editor_node.cpp needs
   ZERO changes (forwarder). **START HERE.**
2. **⑤b.4b — move the overlay draw path onto the view.** `_draw_viewport` + all
   `_draw_*`. Move `show_*`/grid_visibility/ruler_width. Menu handlers read via
   friend. **Introduce the transform-sink seam:** `_get_transform_sink()` returns
   scene_root (shim main view) or (P4) own view_viewport — only the displaying
   view writes the global_canvas_transform. Plugin overlay gated on
   `this==_get_active_view()`.
3. **⑤b.4c — move input/drag machinery onto the view.** `_gui_input_viewport` +
   full chain + `_commit_drag/_reset_drag/_update_cursor/get_cursor_shape` +
   `_get_canvas_items_at_pos/_select_click_on_item`. Snap/undo/menus stay on
   editor, called via `editor->`. Selection stays Model A (only view=main_view).
4. **⑤b.4d — document-bound views; CanvasView2D superseded.** Add
   `document`/`view_viewport`/`bind_document`/`_is_active_document`/
   `_ensure_active`; edit-gate on active-document; `create_view_bound_to`;
   `DocumentView` routes 2D→it; **delete canvas_view_2d.{h,cpp}**. COORDINATE
   AUDIT: every hit-test/drag uses `transform * get_global_transform_with_canvas`,
   NEVER `get_screen_transform` (routes through the doc's scene_root sink);
   re-express `_snap_other_nodes`/`snap_point` screen-transform reads against the
   acting view. Behavior-identical for main_view (its transform == sink).
5. **⑤c.1 — CanvasLayer fidelity for world-bound views.**
   `_reconcile_canvas_layers()`: attach each CanvasLayer's canvas to the view's
   viewport at RS level (create-detached/reconcile/free-in-dtor). Guard double-
   attach with a per-view HashSet<RID>.
6. **⑤c.2 — main view world-binds; retire the shim.**
   `set_active_document()`→`main_view->bind_document`. `_activate_scene_views`
   calls it instead of `_display_scene_root`; **delete `_display_scene_root`** +
   `get_scene_view_container` + shim branch. scene_root parks under
   documents_holder forever. restore_3_scenes is the critical smoke case.

## Post-plan (non-blocking): drain friend bridges; per-doc 2D editor_states;
Model B selection. All deferred per STEP5b3 decision 2.
