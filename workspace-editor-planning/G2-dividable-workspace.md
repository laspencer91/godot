# G2 — Dividable tabbed workspace replacing the 2D|3D|Script main-screen switcher

**Effort:** XL (first usable slice L→XL) · **Depends on:** G1 (DocumentContext), G3 (per-document viewports) · **Feeds:** G3, G4

## Approach

Build a new **`WorkspaceManager`** (a `Control`) under a self-contained `editor/workspace/` subtree that **replaces `EditorMainScreen` as the content host**: a tree of reused `DockSplitContainer` H/V nodes whose leaves are `WorkspacePane` `TabContainer`s, every open scene/script/resource being a tab backed by a `DocumentContext`. Reuse the existing `EditorDockDragHint` drop machinery and the existing `type=="tab"`/`tab_type` drag discriminator (editor_dock_manager.cpp:145-150) rather than inventing a parallel payload.

Plan A was empty, so Plan B's architecture is the target; the reconciliation is a **pragmatic-first build order**. **Verified fact:** this is a fork (branch feature/workspace-editor) and editor/ already carries custom code (custom docks under editor/docks/), so the "no edits under editor/" posture does **not** hold for this subsystem — Plan B's biggest open question is answered.

Ship in thin vertical slices: (1) tree + panes + 5-zone drop hosting the existing singleton plugins via reparent-on-activate with docks rebound as singletons; (2) scene 2D/3D toggle and persistence; (3) script tabs. Defer per-pane dock containers, per-document World3D/SubViewport simultaneous rendering, and floating panes. Keep EditorData accessors as a thin compat shim delegating to the active DocumentContext so the ~120 `_edit_current`/`_set_current_scene` call sites migrate incrementally.

## Ordered steps

1. **Confirm fork posture and scaffold `editor/workspace/`.** Confirm core editor/ edits are acceptable (they already are). Create `editor/workspace/` with its own SCsub wired into `editor/SCsub`. All new code lands here; the only stock-file edits are surgical, well-commented hooks in `editor_node.cpp`, `editor_main_screen.*`, `editor_data.*`.
2. **Define a minimal `DocumentContext` base (align with G1).** `editor/workspace/document_context.{h,cpp}`: RefCounted base owning identity (path/uid = stable persistence + drag-payload key), dirty state, `get_document_view()->Control*` factory, `get_contextual_docks()`, `get_title()/get_icon()`, `bind()/unbind()`. Subclasses Scene/Script/Resource. **If G1 owns this base, consume its interface — coordinate before writing.** SceneDocumentContext starts as a thin wrapper POINTING at the existing `EditorData::EditedScene` entry.
3. **Build the layout-tree primitives reusing dock prior art.** `editor/workspace/workspace_pane.{h,cpp}`: `WorkspacePane` as a TabContainer tree-leaf with `set_tabs_rearrange_group`/`drag_to_rearrange_enabled` + per-tab context menu (close, split). Reuse `DockSplitContainer` (editor_dock_manager.h:48, auto-collapses on hidden children) as the H/V node. Literal Control nesting (Splits contain Panes/Splits) so layout == hierarchy; assign each pane a stable id.
4. **Implement the 5-zone drop overlay on existing drag plumbing.** `editor/workspace/workspace_drop_overlay.{h,cpp}` modeled on `EditorDockDragHint` (dock_tab_container.h:42). `_draw` center square + 4 edge arrows; `zone_at(Point2)->{CENTER,LEFT,RIGHT,UP,DOWN}`; can_drop_data/drop_data dispatch to `WorkspaceManager::handle_tab_drop`. CENTER moves the tab; an edge wraps the target leaf in a new DockSplitContainer. Add a `tab_type` discriminator (`"workspace_document_tab"`) and reject mismatched drops on BOTH overlays (reuses negotiation at editor_dock_manager.cpp:145-164).
5. **Create `WorkspaceManager` and host the existing singleton plugins (slice 1 vertical).** `editor/workspace/workspace_manager.{h,cpp}`: a Control with root_split, document registry, active_document, document→pane map. API: open/close/activate/split_pane/handle_tab_drop. Host singleton plugins via reparent-on-activate: `SceneDocumentContext::get_document_view()` reparents the CanvasItemEditor/Node3DEditor main control (via `EditorPlugin::make_visible(true)`+`get_control()`) into the active scene tab. Only the active scene tab renders live; inactive tabs show placeholder/last-frame (explicit G3 boundary). Construct in EditorNode (~8814) into the same srt VBox where editor_main_screen is added today.
6. **Demote `EditorMainScreen` to a plugin registry and redirect chokepoints.** Keep `add/remove_main_plugin` + editor_table but stop owning visible-selection. Replace `editor_main_screen->select(...)` (editor_node.cpp:415-429, 3290-3300, 4659) with WorkspaceManager activation/auto-switch. Redirect `_set_current_scene/_nocheck` (4690-4698); `_edit_current` (3110) cooperates with WorkspaceManager rebinding. Keep EditorData accessors as a thin compat shim.
7. **Rebind contextual docks as singletons on activation.** On active-tab change, rebind dock singletons to the active DocumentContext (reusing `_edit_current` + save/restore_edited_scene_state wiring): SceneTreeDock::set_edited_scene + selection/history, InspectorDock edit, Signals/Groups/Import. **Pragmatic shortcut** — one shared set rebound on switch, not per-pane containers (record tech debt; end-state owned by G3).
8. **Add the scene 2D/3D per-tab toggle.** SceneDocumentContext gets an `active_view_mode` enum (2D|3D) exposed in its tab when the scene has both; drives which singleton plugin control is reparented. Replaces the global 2D|3D main-screen buttons.
9. **Persist and restore the layout tree.** Hook `WorkspaceManager::save/load_layout_to_config` into `_save_editor_layout`/`_load_editor_layout`, replacing `editor_main_screen->save/load_layout_from_config` (editor_node.cpp:6343/6360). Serialize a versioned `[Workspace]` section: split tree (orientation + split_offset), ordered open-document list (type, path/uid, owning pane id, tab index, active flag, scene 2D/3D mode). On load: validate ids, drop unresolved tabs gracefully, fall back to a default single-pane layout. Decide whether EditorSceneTabs feeds the workspace or is subsumed.
10. **Migrate scripts to first-class tabs (largest sub-refactor, staged LAST).** Stop treating ScriptEditor as a single full-screen main-screen plugin with an internal side-list. Opening a script creates a ScriptDocumentContext whose view is a CodeTextEditor; the left function list becomes a per-pane Methods/outline dock. **KEEP** ScriptEditor's project-global services (debugger breakpoints, find-in-files) — move only presentation. Migrate feature-profile gating (editor_node.cpp:7819-7839) from button enable/disable to document-type creation gating.

## Files to touch

| Path | Change |
|---|---|
| `editor/workspace/SCsub` | NEW: build the editor/workspace/*.cpp subtree (wired into editor/SCsub). |
| `editor/workspace/document_context.h` | NEW: DocumentContext base + Scene/Script/Resource subclasses; view factory, contextual dock list, bind/unbind, stable id. Align with G1 if it owns the base. |
| `editor/workspace/document_context.cpp` | NEW: implementations; SceneDocumentContext initially wraps/points at the existing EditedScene entry. |
| `editor/workspace/workspace_manager.h` | NEW: WorkspaceManager Control replacing EditorMainScreen as content host; tree root, registry, active doc, persistence, drag-drop tree mutation API, active_document_changed signal. |
| `editor/workspace/workspace_manager.cpp` | NEW: open/close/activate/split/handle_tab_drop, save/load_layout_to_config, singleton dock rebinding, reparent-on-activate plugin hosting. |
| `editor/workspace/workspace_pane.{h,cpp}` | NEW: WorkspacePane tree-leaf TabContainer hosting document tabs; rearrange groups, per-tab context menu, split request. |
| `editor/workspace/workspace_drop_overlay.{h,cpp}` | NEW: 5-zone hit-test + overlay draw; can_drop_data/drop_data with tab_type discriminator; modeled on EditorDockDragHint. |
| `editor/editor_main_screen.h` | Demote switcher→registry: keep add/remove_main_plugin + editor_table + get_control; guard select/save/load behind the workspace transition. |
| `editor/editor_main_screen.cpp` | Stop owning visible-plugin selection; expose plugin controls for embedding in scene/asset tabs. |
| `editor/editor_node.h` | Add WorkspaceManager* member + accessor; keep get_editor_main_screen during transition; declare workspace-aware scene/document routing. |
| `editor/editor_node.cpp` | Construct WorkspaceManager (~8814) in srt VBox; redirect _set_current_scene/_nocheck (4690-4698) and _edit_current (3110); replace editor_main_screen->select calls (415-429,3290-3300,4659); workspace layout save/load (6343/6360); feature-profile gating (7819-7839). |
| `editor/editor_data.h` | Keep EditedScene (113-126) as source of truth in slice 1; expose accessors used by SceneDocumentContext; plan eventual field migration (compat shim). |
| `editor/editor_data.cpp` | Make save/restore_edited_scene_state and edited-scene accessors cooperate with the active DocumentContext. |
| `editor/docks/dock_tab_container.h` | Optionally factor a shared zone-hit-test reused by WorkspaceDropOverlay; ensure document-tab payloads are distinguishable from dock-tab payloads. |
| `editor/docks/editor_dock_manager.cpp` | Allow DockSplitContainer reuse for document splits; ensure drag payload type/tab_type negotiation distinguishes dock tabs from document tabs (145-164). |
| `editor/scene/editor_scene_tabs.h` | Repurpose to feed WorkspaceManager open-document set, or subsume into per-pane tab bars. |
| `editor/plugins/script_editor_plugin.cpp` | STAGED LAST: open scripts as ScriptDocumentContext tabs; function list → per-pane Methods dock; keep global debugger/find services. |
| `editor/scene/canvas_item_editor_plugin.cpp` + `editor/scene/3d/node_3d_editor_plugin.h` | Make make_visible/edit cooperate with the active SceneDocumentContext; G2 hosts a single live control + hooks reserved for per-document World2D/World3D (G3). |

## Conflicts & resolutions

- **Core editor/ edits vs module-only injection.** **Resolved by codebase evidence:** the fork already carries custom code under editor/docks/, so the posture doesn't hold. Core editor/ edits acceptable; mitigate by isolating new code under editor/workspace/ and keeping stock-file hooks surgical and commented.
- **Contextual docks: rebound singletons vs per-pane containers.** **Resolution:** ship rebound singletons for G2. Tech debt: split panes can't show two scene trees at once. Migration: per-pane dock containers owned by DocumentContext, scoped to G3; keep all rebind logic in one method.
- **EditedScene: full absorption now vs compat shim.** **Resolution:** start with the shim (SceneDocumentContext points at the existing entry), migrate fields incrementally behind the active-document accessor. Full ownership is the end-state but risks inspector/selection desync if done up front.
- **Simultaneous live rendering of multiple scene panes.** **Resolution:** G2 ships single-live-render via reparent-on-activate; define the World3D/SubViewport fields + a single view-host seam method but don't implement multi-viewport. Full simultaneous rendering is **G3's** responsibility.
- **ScriptEditor side-list retirement timing.** **Resolution:** sequence it LAST (deepest sub-refactor; risks debugger/breakpoint/find-in-files). Keep global services intact, move only presentation.
- **Floating panes (WindowWrapper).** **Resolution:** defer out of G2; additive follow-up reusing existing prior art with no architectural blocker.

## Risks

- Multiple visible scene panes drive the single live scene_root + root-window World3D → non-active tabs render blank/garbage. *Mitigation:* G2 renders only the active tab live; make the view-host seam a single method so G3 per-document World3D drops in cleanly.
- Rebound singleton docks → split panes can't show two SceneTrees/Inspectors. *Mitigation:* accept; keep rebind logic in one method for later per-pane replacement.
- Redirecting `_edit_current`/`_set_current_scene` (~120 call sites) can desync inspector/selection. *Mitigation:* thin compat shim delegating to the active DocumentContext; migrate incrementally.
- Layout-tree persistence can desync from the open-document set. *Mitigation:* version the section, validate ids, drop unresolved tabs, default-single-pane fallback.
- Drag payload collisions between dock tabs and document tabs. *Mitigation:* distinct workspace tab_type, reject mismatches on both overlays.
- Retiring ScriptEditor's side-list can regress debugger/find. *Mitigation:* stage LAST, keep global services intact.
- Literal Control nesting complicates stable pane-id serialization. *Mitigation:* assign each Pane/Split a persistent generated id in metadata.

## Cross-goal dependencies

- **G1 (DocumentContext):** consume the base + per-document selection/history/inspector binding. If G1 owns the base, align — coordinate before writing document_context.h.
- **G3 (2D/3D singleton wall):** true simultaneous per-pane viewports require each SceneDocumentContext to own World3D/World2D + SubViewport and de-singletonized editors. G2 reserves the fields + view-host seam but ships single-live-render.
- **Per-pane docks goal (G3):** G2's rebind method is replaced by per-pane dock containers.
- **ScriptEditor retirement** may be its own goal; G2's script slice needs scripts openable as standalone CodeTextEditor views with global services preserved.

## Unresolved issues

1. Does G1 own the DocumentContext base, or does G2 define it? Settle before writing document_context.h.
2. Are inactive scene tabs allowed to render placeholder/last-frame in G2 (deferring true multi-viewport to G3)? (Rec: placeholder.)
3. Per-pane dock containers in scope for G2, or rebound-singleton-on-activate acceptable? (Rec: singletons.)
4. How do AssetLib and the Game main-screen plugin map to the document model — singleton utility tabs, or excluded?
5. Migration story for existing saved layouts + the EditorSceneTabs row: auto-convert to default single-pane; does EditorSceneTabs feed the workspace or get subsumed?
6. Floating panes in G2 scope? (Rec: deferred.)
