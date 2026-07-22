# Phase 3B scout — Node3DEditorViewport input pipeline map

Read-only reconnaissance (2026-07-22) for the EditorEditDomainHost insertion (CSG-EDIT-PLAN.md §5/§8). Refs are `editor/scene/3d/node_3d_editor_viewport.cpp` unless prefixed.

## Two input entry points

- `_sinput(...)` — line 2624, connected to the surface control's `gui_input` at 4477. Primary chain; the domain hook belongs here.
- `input(...)` — line 2614, only live during instant (Blender-style) transforms via `set_process_input` (6864-6865; off by default 7342); captures outside-rect motion via `view_3d_controller->get_warped_mouse_motion` (2619). This is the existing pointer-capture-during-drag mechanism a domain gesture should mirror; coordinate with `_edit.instant`.
- `_redirect_freelook_input` (2608, invoked 2672) re-emits gui_input to another viewport during freelook — domain host must ignore redirected events on non-focused panes.

## Ordered `_sinput` trace

1. Numpad emulation 2627-2632.
2. DnD guard: `gui_get_drag_data()` → return, 2634-2637.
3. Vertex-snap mode (fork feature): own Escape (2658, 2683) and LMB/RMB handling with `return`s, 2642-2766.
4. Plain-RMB → shared context-menu focus grab 2770-2780.
5. Global plugin `forward_3d_gui_input` 2782-2816 (force-input list 2789, plugins-over 2803). STOP → return; CUSTOM → local `after` flag; PASS → continue.
6. Plain-RMB context menu popup 2818-2831 (`_popup_context_menu`, return).
7. Navigation: `view_3d_controller->gui_input(p_event, surface rect)` 2833-2838 (orbit/pan/zoom/wheel/freelook; return if gesture ended).
8. Mouse button switch 2840-3232:
   - RIGHT 2850 (freelook, alt-RMB `_list_select` 2870, transform cancel).
   - MIDDLE 2902 (axis-plane cycling).
   - LEFT press 2941: instant/collision commit 2945; nav-modifier guards 2949; ruler/list-select 2957/2963; gizmo HANDLE pick 2983-3011 (`can_select_gizmos` 2973); transform-gizmo select 3013-3016; SUBGIZMO pick 3018-3062; object click-selection `_select_ray` 3064-3158 (gated on `after != CUSTOM` at 3068; sets `clicked`, `selection_in_progress`; region-select init 3111-3116; `begin_transform` 3105-3108/3154-3157).
   - LEFT release 3161: gizmo handle commit 3174; `_select_clicked(false)` 3192 (gated `after != CUSTOM` at 3188); `_select_region()` 3194; transform commit 3202-3225.
9. Mouse motion 3234-3358: gizmo hover 3280-3316; handle drag 3318; region drag 3324-3337; transform drag start 3339-3356.
10. Keyboard 3360-3643: instant numeric entry 3365-3409; Escape branches (below); axis locks 3438-3452; instant TRS 3603-3622; freelook Escape 3628-3630; Space maximize 3632; `if (freelook) accept_event();` 3641-3643.

## Recommended insertion point

**Single host-call site immediately after line 2816** (after global-plugin forwarding; before RMB context menu 2818 and navigation 2833). Domain returns PASS for MMB/wheel/nav-modified and plain-RMB events so navigation and the context menu stay native — matches contract §8.

Tri-state mapping:
- CONSUMED → `accept_event(); return;` (like vertex-snap early returns).
- BLOCK_NATIVE_EDIT → reuse `after = AFTER_GUI_INPUT_CUSTOM` gate for selection (3068/3188) PLUS a new per-viewport bool (e.g. `domain_blocks_native`) guarding: gizmo handle pick 2983-3011, transform-gizmo select 3013-3016, subgizmo pick 3018-3062, `begin_transform` 3105/3154, and optionally motion-side hover 3280-3356. Must NOT suppress navigation (2835), RMB menu (2829), freelook, Space.
- PASS_TO_VIEWPORT → fall through unchanged.

## AfterGUIInput sufficiency verdict

Enum editor/plugins/editor_plugin.h:124-127; aggregation editor/plugins/editor_plugin_list.cpp:45-63 (STOP > CUSTOM > PASS across ALL plugins). Conceptual match for the tri-state but INSUFFICIENT as-is: (a) global-plugin-scoped, not per-view; (b) CUSTOM only blocks click-selection, not gizmo handles/transform. Host needs its own per-view tri-state at ~2817.

## Per-view vs global state

Per-view: viewport instances (`_build_view_viewports` node_3d_editor_plugin.cpp:2818-2832; `create_view_bound_to` 2834-2851 forces single-viewport 2845); transform-manipulator render RIDs + `transform_gizmo_visible` per viewport (`update_transform_gizmo_view` 5697); `_edit` struct; surface/camera/chrome. Fork fans `update_transform_gizmo` across all `editor_views` (plugin 195-203).

Global (Node3DEditor singleton): `editor_selection`/`selected`; `gizmo.visible` (`is_gizmo_visible` plugin 4376-4381, mixed into per-view gate at 5782/5808); tool mode; local coords; hover.

**KEY GAP:** selected-object node-attached gizmos (`node->get_gizmos()`, used at 2984/3021/3281) render in the shared World3D and appear in EVERY pane showing that world; only the transform manipulator is per-view. Per-view "selected-object gizmos suppressed" (§8) needs either global suppression while a pane's domain is active, or new per-view plumbing. DESIGN DECISION NEEDED at Phase 3B.

## Context menu (commit 98577e2c84)

`_popup_context_menu` 2442-2480 → context dict `_build_context_menu_context` 2346 (carries scene_root, placement_position, physics_hit/hit_normal, document_history_id, 2521-2566) → `EditorContextMenuPluginManager::add_options_from_plugins(menu, CONTEXT_SLOT_3D_EDITOR, paths, 0, context)` 2470-2475. Slot enum editor/inspector/editor_context_menu_plugin.h:49-58. Custom activation `_context_menu_option` 2490-2497 → `activate_custom_option`. CSG entries: an EditorContextMenuPlugin on CONTEXT_SLOT_3D_EDITOR.

## Overlay drawing (view-local ghosts/highlights)

`_draw()` 4654 on surface `draw` signal (4476); forwards `forward_3d_draw_over_viewport` + force-draw first (4655-4656). Use `surface->draw_*`, `camera->unproject_position`, `_is_vertex_occluded`; trigger via `surface->queue_redraw()`. Per-viewport by construction → naturally view-local (plan §9 satisfied without material mutation). Examples: region box 4666-4678, messages 4682-4689, vertex-snap ghosts 4691-4722.

## Escape / Tab / focus

- Escape owners: vertex-snap 2658/2683; freelook 3420-3423, 3628-3630; gizmo-handle restore 3425-3430; DEFAULT `_clear_selected()` 3431-3434; shortcut `spatial_editor/cancel_transform` 7607; `ui_cancel` in rotation control 450. Host Escape (cancel gesture → exit) handled at the ~2817 hook consumes before all of these — placement works naturally.
- Tab: NO existing Key::TAB usage in the viewport — free. But Tab is Control focus traversal; host must `accept_event()` to keep focus on the surface. Keep shortcut configurable (plan §24).
- Focus: keyboard needs surface focus; `_surface_focus_enter/exit` toggles menu shortcut disable 2222-2228. Focused-pane gate: `_surface_mouse_enter` 2195-2214 only re-grabs focus within the already-focused pane (2204-2209) — the enforcement point for "only the active pane accepts editing input"; host uses the same check to suspend input on unfocused panes.

## Double-click

No `is_double_click()` usage anywhere in the viewport selection path — double-click activation must be ADDED at the host hook (read `b->is_double_click()` on LMB before native `_select_ray`). Matches provider API `can_activate_from_double_click(context, hit)`.

## Hazards

1. Node-attached selected-object gizmos are global (see KEY GAP above).
2. Two entry points; instant transforms use `set_process_input` as capture — domain gestures mirror this, coordinate with `_edit.instant`.
3. Freelook input redirection (2608).
4. Escape multiply-owned — consume at hook before `_clear_selected` 3431.
5. Tab = focus traversal; accept_event.
6. Hook must sit AFTER DnD guard (2634) and vertex-snap returns (2676-2766).
7. `after == CUSTOM` alone under-suppresses (no gizmo-pick gating).
8. Press/motion/release state (`clicked`, `selection_in_progress`, region-select 2974/3064/3113) persists across events — a domain that consumes the press must neutralize these so the release handler (3161-3226) doesn't act on stale state.
9. Chrome slots: current enum has only 6 corner slots (editor_viewport_chrome.h:95-103) — SLOT_CENTER_LEFT/RIGHT must be added (plan §6); registration via `register_control_factory` h:78; chrome created per view at plugin 2799.
