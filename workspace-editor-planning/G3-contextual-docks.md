# G3 — Per-pane contextual docks (scene-tree+inspector, resource-as-inspector-tab, script methods)

**Effort:** L→XL (phased: Phase 1 ships at L, de-singleton clean-up is XL debt) · **Depends on:** G1 (hard), G2 (soft for build / hard for meaningful ship), G4 (soft)

## Approach

Both plans converge on the only viable end-state: **each open document owns its own dock instances** (`EditorSelection`, `SceneTreeDock`, `InspectorDock`, plus a new `ScriptMethodsDock` for scripts), because two scene panes can be co-visible and cannot share one global dock. They diverge only on HOW to reach the 215 singleton call sites.

Adopt a **phased hybrid:** ship Plan A's pragmatic accessor-repointing first — keep the `get_singleton()`/`get_editor_selection()` names, make them return the **active** document's instances with a legacy/bootstrap fallback — so the tree keeps compiling with near-zero call-site churn. Bake in Plan B's discipline from day one: **per-document instance ownership** (NOT a single rebound instance), an explicit audit of the ~6 call sites that target a non-active document, and a planned rename to `get_active()`/`get_active_inspector()` once the path is stable.

New polymorphic `Document`/`SceneDocument`/`ScriptDocument`/`ResourceDocument` types and the per-pane dock host live in the user's `modules/` workspace module; edits to the stock dock classes stay in `editor/`. Everything is **hard-gated on G1** (DocumentContext) which does not exist yet; build behind a single-default-document stub until G1 lands.

## Ordered steps

1. **Pin the G1 DocumentContext / active-document contract.** Fix the exact accessor signature (`WorkspaceEditor::get_active_document() -> Document*`), its null-safety during startup/teardown, and a polymorphic `Document` base with `DocumentType get_type() {SCENE,SCRIPT,RESOURCE}`. Stub `get_active_document()` to return one default document so accessor plumbing and ScriptMethodsDock can be built/tested independently.
2. **Give each Document type its own dock + selection instances (per-document ownership).** SceneDocument owns EditorSelection + EditorSelectionHistory + SceneTreeDock + InspectorDock; ScriptDocument owns InspectorDock + ScriptMethodsDock; ResourceDocument owns an InspectorDock (v1, see C4). Construct lazily on first activation via the injection-ready ctors: `memnew(SceneTreeDock(doc_scene_root, doc->selection, editor_data))` and `memnew(InspectorDock(editor_data))`; call `scene_tree_dock->set_edited_scene(doc_root)`. Reuse `EditorData::save/restore_edited_scene_state` + the EditedScene bucket (editor_data.h:113-126) as the per-document selection/history snapshot.
3. **Repoint the three hot accessors to the active document (keep names; legacy fallback).** Make `SceneTreeDock::get_singleton()` (scene_tree_dock.h:317), `InspectorDock::get_singleton()/get_inspector_singleton()` (inspector_dock.h:142-143), and `EditorNode::get_editor_selection()` (editor_node.h:802) return the active document's instances, falling back to a legacy/bootstrap instance when no document is active. **CRITICAL:** `get_inspector_singleton()` dereferences `singleton->inspector` and will null-crash — guard it. Keeping names means the 215 sites compile unchanged; semantics shift from "the dock" to "active dock." Retain the static singleton members as the fallback target.
4. **Audit and fix the call sites that need a NON-active document.** A blind redirect mis-targets: `editor_debugger_node`/`editor_debugger_tree` (remote scene tree, 5+ sites), `filesystem_dock` instantiate-into-scene, and any captured scene tree. Convert those ~6 sites to take an explicit `Document*`/dock instance parameter. Land step 3's redirect first (tree compiles), then migrate these file-by-file.
5. **Route `_edit_current` and scene-switch chokepoints through the active Document.** `_edit_current` (editor_node.cpp:3110) mostly works unchanged with step 3, but audit its `editor_history`/`get_edited_scene()` reads (:3117 foreign-resource guard, :3175 get_edited_scene) to use the active document's history/root. `_set_current_scene_nocheck` (4698) `selection->clear()`+`set_edited_scene()` becomes operations on the activated SceneDocument; its scene_root reparenting (:4717-4734) is superseded by G2 — coordinate, don't duplicate. Connect each document's EditorSelection `selection_changed` to that document's inspector update.
6. **Create `ScriptMethodsDock` (function outline) as a reusable EditorDock.** Filter LineEdit + ItemList + sort button, extracted from ScriptEditor's `_update_members_overview`/`_members_overview_selected`, populated from the script tab's `CodeTextEditor`/`get_functions()`. Bound to a ScriptDocument's editor instance, NOT the global ScriptEditor's `_get_current_editor()`. Leave the legacy ScriptEditor side-list intact for v1; make the dock reusable so the legacy list can later delegate. Cross-reference the upstream source for rebase tracking.
7. **Build the per-pane WorkspaceDockHost / contextual dock area.** In the pane leaf (TabContainer from G2; temporary VBox host if G2 not landed) add a contextual dock region. On active-tab change: hide/remove the previous document's docks, re-parent (not free) the active document's docks in, flip the accessors (step 3). Per-type layout: SCENE = VSplit SceneTreeDock(top)+InspectorDock(bottom); SCRIPT = InspectorDock + ScriptMethodsDock; RESOURCE = InspectorDock editing the resource. Reuse EditorDockManager DockSplitContainer/EditorDock prior art; a plain VSplitContainer is fine for v1. Stub (don't implement) the scene-tab 2D/3D toggle hook.
8. **Lifecycle, persistence, incremental clean-up.** On document close, free its docks/selection/history; on shutdown avoid double-free with the legacy fallback. Provide get/set state hooks so G2/G4 layout persistence can restore active document per pane (only document identity + split ratios persist) via EditorDock `_save/_load_layout_to_config` virtuals. **Tech-debt milestone:** once stable, rename `get_singleton()`→`get_active()`, remove the static members (Plan B end-state), revisit resource-tab bare-inspector and per-document SignalsDock/GroupsDock.

## Files to touch

| Path | Change |
|---|---|
| `modules/<workspace-module>/document.{h,cpp}` | NEW (G1-owned, extended here): Document base + Scene/Script/Resource subclasses owning their docks, EditorSelection, EditorSelectionHistory; lazy construction on activation; get_type(). |
| `modules/<workspace-module>/workspace_dock_host.{h,cpp}` | NEW: per-pane controller that swaps visible docks on active-tab change and flips the active-document accessors. |
| `modules/<workspace-module>/script_methods_dock.{h,cpp}` | NEW: ScriptMethodsDock (filter LineEdit + ItemList + sort) bound to a ScriptDocument's CodeTextEditor; logic lifted from ScriptEditor members_overview. In modules/ to minimize rebase cost (subclasses editor/ EditorDock). |
| `editor/docks/scene_tree_dock.h` | Repoint get_singleton() (317) to active-document delegate with legacy fallback; keep static singleton as fallback. Ctor (368) injection-ready. Audit for hidden static/shortcut state. |
| `editor/docks/scene_tree_dock.cpp` | Ensure multi-instance safety: per-instance shortcut/theme registration, no shared static state; set_edited_scene rebind (3435) used per document. |
| `editor/docks/inspector_dock.h` | Repoint get_singleton()/get_inspector_singleton() (142-143) to active-document instance; ADD null guard (get_inspector_singleton dereferences singleton->inspector). Ctor takes EditorData& (164). |
| `editor/docks/inspector_dock.cpp` | Per-instance history wiring to the owning document's EditorSelectionHistory; verify back/forward (h:128-129) follow active document. |
| `editor/editor_node.h` | get_editor_selection() (802) + get_editor_selection_history() (803) become active-document forwarders; keep editor_selection (274) + editor_history (262) as legacy/bootstrap fallback. |
| `editor/editor_node.cpp` | Audit _edit_current (3110) + _set_current_scene_nocheck (4698) for editor_history/get_edited_scene reads that must become per-document; keep ctor dock+selection creation (8575/9121/9134) as legacy fallback (move into Document factory later). |
| `editor/editor_data.{h,cpp}` | Reference EditedScene (113-126) + save/restore_edited_scene_state (240-241) as the per-document snapshot mechanism reused by SceneDocument; adjust set_edited_scene/remove_scene to drive per-document selection/history when a workspace document is active. |
| `editor/script/script_editor_plugin.{h,cpp}` | Source of extraction: members_overview ItemList + _update_members_overview/_members_overview_selected. v1: leave intact, copy minimal logic into ScriptMethodsDock; later delegate. |
| `editor/debugger/editor_debugger_node.cpp`, `editor/docks/filesystem_dock.cpp` | Convert the ~6 non-active-document call sites (remote scene tree, instantiate-into-scene) to take an explicit Document*/dock. |
| `editor/scene/3d/node_3d_editor_plugin.cpp`, `editor/scene/canvas_item_editor_plugin.cpp` | Selection/scene-tree routing reads through active document; overlaps G2 world isolation — coordinate so each editor reads its own document's selection. |

## Conflicts & resolutions

- **De-singleton entirely vs repoint the existing accessor (core methodological split).** Plan A: keep static singleton members + names, repoint with fallback (smallest diff, 215 sites compile unchanged). Plan B: remove static fields, rename to `get_active()`, migrate all 215. **Resolution:** adopt Plan A's mechanism — Plan B's "deadlock" argument is a strawman since **both plans create per-document instances**, so co-visible panes are already safe; the only real difference is naming + churn. Ship with names kept + bootstrap fallback (also covers the 215 startup/teardown calls). Tech debt: `get_singleton()` now lies. Clean path: rename to `get_active()` + delete static members as a mechanical follow-up.
- **Where non-active-document call sites are handled.** **Resolution:** take Plan B's discipline on Plan A's mechanism — audit and convert editor_debugger_node/tree + filesystem_dock instantiate to explicit instances (step 4). Low cost, prevents subtle selection/tree corruption.
- **EditorSelectionHistory per-document vs shared.** **Resolution:** per-document (Plan B) — the multi-pane vision requires independent breadcrumb/history, and EditedScene already snapshots per scene. Forward `get_editor_selection_history()` to the active document; verify HistoryDock + inspector back/forward on tab switch.
- **Resource tab body: bare EditorInspector vs full per-document InspectorDock.** **Resolution:** Plan A for v1 — use a per-document InspectorDock (or make the active-inspector resolver return the resource tab's inspector) so consumers reaching the inspector via `get_inspector_singleton()` (ImportDock, _edit_current) hit the right object. Tech debt: extra dock chrome on resource tabs. Clean path: bare EditorInspector once `get_active_inspector()` reliably resolves to it.
- **Where new code lives: editor/ vs modules/.** **Resolution:** hybrid — Document hierarchy, WorkspaceEditor, WorkspaceDockHost, and new ScriptMethodsDock live in `modules/<workspace-module>`; edits to stock dock classes stay in editor/. ScriptMethodsDock subclasses editor/'s EditorDock but its files sit in modules/.
- **Effort sizing.** **Resolution:** both right about different phases. Phase 1 (per-pane docks via accessor-repoint) is L. Phase 2 (de-singleton rename + Document factory + bare resource inspector) is XL clean-up debt. Plan as **L→XL phased**.

## Risks

- Active-document indirection on `get_singleton()`/`get_editor_selection()` is hit 215 times across 30+ files, including during construction/teardown and before any document exists → null-deref / wrong document. *Mitigation:* always provide a legacy/bootstrap fallback; ERR_FAIL_NULL guards (esp. get_inspector_singleton); exercise no-document, startup/teardown, switch-mid-drag paths.
- Blind redirect silently changes semantics and mis-targets the ~6 non-active sites. *Mitigation:* categorize + convert those sites to explicit instances; land redirect first, migrate file-by-file.
- Per-document EditorSelectionHistory can desync the inspector breadcrumb/back-forward + the foreign-resource guard (editor_node.cpp:3117). *Mitigation:* drive history off the active document's EditedScene bucket; verify HistoryDock + back/forward on tab switch.
- Multi-instance docks may rely on hidden static/per-class state (theme caches, shortcut registration, undo-redo wiring). *Mitigation:* audit ctors; register shortcuts/undo-redo per-instance.
- ScriptMethodsDock duplicates ScriptEditor's members_overview → drift. *Mitigation:* port minimal logic, depend only on `CodeTextEditor::get_functions()`, keep legacy list working, add cross-reference comment.
- Resource tab inspector reachability via `get_inspector_singleton()`. *Mitigation:* route resource tabs through a per-document InspectorDock so the accessor stays the single source of truth.
- Hard dependency on G1 DocumentContext which does not exist. *Mitigation:* gate behind G1; stub `get_active_document()` to return one default document.

## Cross-goal dependencies

- **G1 DocumentContext (HARD):** provides `get_active_document()` + per-document scene_root/resource/script that every routed call site dereferences. Build behind a single-default-document stub until it lands.
- **G2 (SOFT build / HARD ship):** provides the pane leaf + active-tab-change event; owns the per-document live scene_root reparenting that supersedes `_set_current_scene_nocheck:4717-4734` — coordinate.
- **G2/G-3D per-document World3D (SOFT):** 3D/2D editor selection routing must read each document's own selection; the 2D/3D toggle is stubbed here.
- **G4 layout persistence (SOFT):** per-pane dock layout (split ratios, MethodsDock state, active document identity) serializes via EditorDock save/load virtuals.

## Unresolved issues

1. Confirm G1's exact active-document API and whether it guarantees a usable value during startup/teardown (the fallback depends on that boundary).
2. Do SignalsDock, GroupsDock, ImportDock (~37 combined call sites, also pushed in `_edit_current`) become per-document or stay global-rebound for v1? (Rec: global for v1; flag as debt.)
3. Multi-window active-document semantics: per-floating-window "active" document or one global? Affects accessor resolution across windows.
4. Is EditedScene fully absorbed into SceneDocument (cleaner, bigger diff) or kept in EditorData with SceneDocument holding an index? (Rec: keep in EditorData for Phase 1, absorb in Phase 2.)
5. For a scene tab with both 2D and 3D content, does the per-pane Inspector/Scene-Tree stay shared while only the viewport toggles, or do 2D/3D modes carry distinct dock sets?
