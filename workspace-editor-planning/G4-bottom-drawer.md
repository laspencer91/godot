# G4 — Bottom drawer slideout for file exploration with drag-drop into panes

**Effort:** M · **Depends on:** none hard (degrades gracefully without G1/G2) · **Lowest-risk goal**

## Approach

Build a single self-contained **`WorkspaceFileDrawer`** overlay (a `PanelContainer` hosting a `DockTabContainer`) anchored to the bottom edge of `center_vb`, animated with a `Tween`, defaulting closed and partial-height (~40%) when open. Host the project-global `FileSystemDock` inside it.

This is the lowest-risk goal because the hard part — drag-drop — **already works end to end in stock Godot:** `FileSystemDock::get_drag_data_fw` → `EditorNode::drag_files_and_dirs` (editor_node.cpp:6904/6860) emits the standard `{type:files/files_and_dirs/resource}` dict, and existing receivers (`Node3DEditorViewport::can_drop_data_fw` at node_3d_editor_plugin.cpp:5856, CanvasItemEditorViewport, SceneTreeDock, EditorInspector) already consume it. **No drag/drop producer or consumer code changes for v1.**

Favor pragmatic-first delivery: for v1, **REPARENT** the single `FileSystemDock` pointer out of `editor_dock_manager->add_dock` (editor_node.cpp:9132) into the drawer's DockTabContainer at startup, and drive its overridden `save_layout_to_config/load_layout_from_config` (filesystem_dock.h:402-403) directly from EditorNode's layout hooks. This avoids touching the shared `EditorDockManager` and the persisted `DockSlot` enum. Use the DockTabContainer host so adding Import/Log/Audio tabs later and the eventual clean migration are cheap.

**Tech-debt flag:** reparenting FileSystemDock outside EditorDockManager loses float/move-between-slots and unified dock persistence, and the feature-profile `set_dock_enabled` calls (7815/7831) and docks menu must be special-cased. The clean migration is the **DOCK_SLOT_DRAWER** route (append enum before DOCK_SLOT_MAX, register the drawer's DockTabContainer via `register_dock_slot`, add serialization + layout-version migration), which restores all that machinery for free — and is why the DockTabContainer host is chosen now.

## Ordered steps

1. **Create `WorkspaceFileDrawer` overlay class.** `editor/gui/workspace_file_drawer.{h,cpp}`. PanelContainer owning a DockTabContainer content host (prior art: EditorBottomPanel) + a grab/header strip with a close button and a draggable top border for resize. Anchored to bottom edge, full width; closed state translates the panel fully below the visible rect. API: `set_open(bool animate)`, `is_open()`, `toggle()`, `get/set_drawer_height` (height fraction, default ~0.40), `add_panel(Control*, title)`, `save_state_to_config/load_state_from_config`. Mouse-filter discipline: only the panel rect is MOUSE_FILTER_STOP; non-panel overlay area is MOUSE_FILTER_IGNORE.
2. **Slide animation + toggle affordance + shortcut.** Animate panel offset with `create_tween()` between closed (off-screen) and open (height fraction). Persistent bottom-edge handle/tab + header close button. Register an ED_SHORTCUT next to `editor/toggle_last_opened_bottom_panel` (editor_node.cpp:8807) routed to `file_drawer->toggle()`; on open optionally call `FileSystemDock::focus_on_filter` (filesystem_dock.h:425). Never translate the panel to zero size while open, so the Tree stays hit-testable as a drag source.
3. **Mount the drawer as the last child of `center_vb`.** In EditorNode ctor, after center_vb is built (editor_node.cpp:8700-8709) and the workspace subtree is added, memnew the drawer and add it as the LAST child of center_vb so it paints above the split workspace but only over the central column (side docks stay usable), below modal Popups. Store `EditorNode::workspace_file_drawer` member + getter. center_vb is a local var, so capture the pointer at construction time.
4. **Reparent FileSystemDock into the drawer (pragmatic v1).** At editor_node.cpp:9127-9132, keep the existing memnew + signal connections (inherit/instantiate/display_mode_changed) and connect_filesystem_dock_signals, but replace `editor_dock_manager->add_dock(filesystem_dock)` (9132) with `workspace_file_drawer->add_panel(filesystem_dock, TTR("FileSystem"))`. Singleton is still set in the dock ctor, so the ~100 `get_singleton()` call sites keep working. Defer Import/Log/Audio.
5. **Drag-aware occlusion handling.** Combine two mechanisms so drops aimed at panes beneath always land: (a) keep the drawer partial-height so the upper workspace is always exposed; (b) on `NOTIFICATION_DRAG_BEGIN` (or `gui_is_dragging()`) set the drawer panel background mouse_filter to IGNORE (FileSystemDock is the SOURCE, needs no hits) and optionally auto-retract to a thin strip, restoring MOUSE_FILTER_STOP and height on `NOTIFICATION_DRAG_END`.
6. **Persist drawer geometry + FileSystemDock state.** In `_save_editor_layout/save_layout_to_config` (editor_node.cpp:6334) write drawer open bool + height fraction under EDITOR_NODE_CONFIG_SECTION (new keys `file_drawer_open`/`file_drawer_height`); restore in load path (6349). Because the dock no longer flows through `EditorDockManager::save_docks_to_config`, also invoke FileSystemDock's overridden `save/load_layout_to_config` (filesystem_dock.h:402-403) directly.
7. **Handle feature-profile enable/disable.** Replace the two `editor_dock_manager->set_dock_enabled(FileSystemDock::get_singleton(), ...)` calls (editor_node.cpp:7815, 7831) with showing/hiding the drawer FileSystem panel (or force-closing the drawer) when the dock is profile-disabled. Audit the docks menu for the now-unregistered dock.
8. **Verify drops land in the visible viewport (v1); design the per-pane contract (deferred).** Confirm a file dragged from the partially-open drawer over the visible main-screen viewport still drops via the existing global handlers. Design (don't implement) the LENS-B `DropTarget` contract: each future document pane Control's `_can_drop_data` accepts `{type:files}` and `_drop_data` resolves the file against THAT pane's DocumentContext (instance PackedScene into the document's own scene_root/World3D; assign Resource into the document-scoped inspector) instead of the global `EditorNode::scene_root` / Node3DEditor singleton. The drawer stays a single global overlay; only drop RESOLUTION becomes per-pane. Until DocumentContext lands, panes fall back to global viewports.

## Files to touch

| Path | Change |
|---|---|
| `editor/gui/workspace_file_drawer.h` | NEW: overlay declaration — PanelContainer hosting a DockTabContainer, slide/open state, toggle/resize API, mouse-filter discipline, save_state/load_state_to_config. |
| `editor/gui/workspace_file_drawer.cpp` | NEW: implementation — bottom-edge anchoring, Tween slide, drag-aware mouse_filter IGNORE + optional auto-retract on DRAG_BEGIN/END, add_panel, layout persistence. |
| `editor/gui/SCsub` | Verify the glob picks up the new source; add explicitly if needed. |
| `editor/editor_node.h` | Add forward decl + member `WorkspaceFileDrawer *workspace_file_drawer` and a getter (near other GUI members ~432). |
| `editor/editor_node.cpp` | Construct + mount drawer as last child of center_vb (~8700-8709); reparent FileSystemDock into drawer instead of add_dock (9132); add toggle button + ED_SHORTCUT (near 8807) and dispatch; persist drawer state + call FileSystemDock save/load_layout_to_config in layout hooks (6334/6349); replace feature-profile set_dock_enabled calls (7815/7831) with drawer panel show/hide. |
| `editor/docks/editor_dock.h` | (DEFERRED clean route only) Append DOCK_SLOT_DRAWER to the DockSlot enum before DOCK_SLOT_MAX (51-64). |
| `editor/docks/editor_dock_manager.cpp` | (DEFERRED clean route only) Register/serialize the drawer slot in register_dock_slot / save_docks_to_config / load_docks_from_config and DockSlotGrid/DockContextPopup; add a layout-version migration. |
| `editor/docks/filesystem_dock.cpp` | (DEFERRED clean route only) Change default_slot from DOCK_SLOT_LEFT_BR (~4318) to DOCK_SLOT_DRAWER. |

## Conflicts & resolutions

- **Host FileSystemDock: raw reparent vs new DOCK_SLOT_DRAWER registered slot.** **Resolution:** Plan A's raw reparent for v1 (smallest diff, no shared-enum churn that risks corrupting saved layouts). Flag tech debt: v1 loses float/move + unified persistence, must special-case set_dock_enabled + docks menu. Clean migration = Plan B's DOCK_SLOT_DRAWER route, done when dock float/move into the drawer is actually wanted or EditorDockManager is otherwise refactored.
- **Drawer content container: bare PanelContainer vs DockTabContainer host.** **Resolution:** adopt Plan B's DockTabContainer even in pragmatic v1 — costs little, gives free tabbing for deferred Import/Log/Audio, makes the DOCK_SLOT_DRAWER migration near-drop-in. Keep Plan A's simple `add_panel(Control*, title)` as the public API over it.
- **Occlusion handling.** **Resolution:** combine all three defensively — always partial-height, panel-background mouse_filter IGNORE during an active drag, optional auto-retract on DRAG_BEGIN. Belt-and-suspenders removes the single most likely failure mode.
- **v1 drawer contents: FileSystem only vs +Import/Log/Audio.** **Resolution:** FileSystem only in v1 to keep scope minimal and avoid disturbing the existing bottom_panel (already hosts Log/Audio). DockTabContainer makes extra tabs additive. User-facing scope decision — see unresolved.

## Risks

- Overlay occludes the very pane the user wants to drop onto. *Mitigation:* partial-height default ~40%; mouse_filter IGNORE + optional auto-retract on DRAG_BEGIN; only the panel rect is MOUSE_FILTER_STOP.
- Reparenting FileSystemDock out of EditorDockManager desyncs unified persistence, DockSlotGrid/context popup, docks menu, float/move; set_dock_enabled (7815/7831) assumes registration. *Mitigation:* singleton stays set so get_singleton() callers are fine; replace set_dock_enabled with drawer show/hide; call FileSystemDock save/load directly; audit docks menu; accept loss of float/move in v1; restore via deferred DOCK_SLOT_DRAWER.
- Drag fails to initiate from a Tree in an off-screen-translated overlay. *Mitigation:* only allow drags while open and laid out; never translate to zero size — use a position offset on a still-laid-out panel, keep the tree inside the visible rect.
- Per-pane drop routing impossible until DocumentContext exists → v1 drops only reach global viewports. *Mitigation:* ship v1 against existing global viewports (functional); design the DropTarget contract now; gate document-scoped routing behind the pane goals.
- (Deferred route) DOCK_SLOT_DRAWER shifts persisted DockSlot indices → corrupts old layouts. *Mitigation:* append before DOCK_SLOT_MAX, bump layout config version, add migration, test loading a pre-change layout.
- Z-order/parenting relative to dialogs/popups. *Mitigation:* parent to center_vb as last child, not gui_base; modal Popups are separate Windows and remain above.
- Upstream-merge friction from editing the EditorNode ctor + layout hooks. *Mitigation:* keep all logic inside WorkspaceFileDrawer; limit editor_node.cpp edits to a few clearly-bracketed insertion points.

## Cross-goal dependencies

- **Dividable/tabbed workspace (G2):** provides the panes that act as drop targets. G4 degrades to the stock single main screen without it, so it can ship first.
- **Per-document viewport (G1/G3):** each document viewport Control must be the drop receiver under the cursor for scene-targeted drops to land in the correct document; otherwise drops resolve to global scene_root / Node3DEditor singleton.
- **Contextual per-pane docks (G3):** supplies per-pane Inspector/SceneTree as additional drop targets; not required for file-into-viewport drops.
- **DocumentContext (G1):** required for the per-pane DropTarget contract. Design now; gate implementation behind it.
- **Workspace layout-tree persistence (G2):** drawer open/height should save/restore with the pane-tree state for coherent session restore.
- **EditorDockManager:** only touched if/when the deferred DOCK_SLOT_DRAWER route is taken — coordinate with any goal refactoring the dock manager.

## Unresolved issues

1. v1 drawer contents: FileSystem only, or also Import/Log/Audio tabbed? (Rec: FileSystem only; Log/Audio already live in the existing bottom_panel — decide whether the drawer coexists with or eventually replaces bottom_panel.)
2. Drawer scope: center workspace column only (center_vb) or the whole client area including side docks (main_vbox/gui_base)? GDStudio screenshots suggest center-only; confirm.
3. Toggle affordance: persistent bottom-edge handle, titlebar button, Ctrl+J-style shortcut, or combination — and how it coexists with toggle_last_opened_bottom_panel.
4. Occlusion UX: smooth auto-retract-on-drag vs fixed partial-height — which feels less surprising (affects whether auto-retract is on by default).
5. When to invest in the clean DOCK_SLOT_DRAWER migration: only if users need to float/move docks into/out of the drawer, or proactively before saved-layout formats stabilize (the enum change needs a migration regardless).

## Addendum — bottom-bar observations (screenshot E)

GDStudio keeps a **persistent full-width bottom status bar** (always visible, independent of the drawer):
- Left: **`Filesystem` | `Log` | `Audio`** toggle buttons that summon their respective bottom panels. This confirms the drawer **coexists with / extends** the existing `bottom_panel` (Log/Audio remain peers) rather than replacing it — resolving unresolved issue #1 toward *coexist*. Implication: the drawer's DockTabContainer should host **FileSystem** while Log/Audio stay on the stock `EditorBottomPanel`, and all three are summoned from a unified bottom-bar toggle row.
- Center-left: a **live status text area** (observed: `Instantiating: (-6.265, 1.5, -2.768)`) — maps to the stock editor status/progress line.
- Right: **status icons** (error / warning counts) — the stock bottom-bar indicators.

So Goal 4 v1 = mount FileSystem in the slide-up drawer, add a `Filesystem` toggle to the bottom bar beside the existing Log/Audio toggles, and leave the rest of the stock bottom bar intact. See `REFERENCE-gdstudio-ux.md` §1 and §5.
