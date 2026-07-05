# G2 Main-Screen Replacement — Codex Implementation Plan

**Goal.** Replace the stock one-visible-plugin main-screen switcher (`2D|3D|Script|Game|AssetLib`) with a dividable workspace of panes + tabs, where each pane independently shows a view of any open document (scene, script, help), scene tabs carry their own embedded scene-tree + inspector + node dock with a live per-document selection, and the surviving singleton screens (Game, AssetLib, third-party main screens) live in a screen-host tab. The Services/View substrate (Node3DEditorView, CanvasItemEditorView, EditorDocument/SceneDocument, EditorWorkspace, DocumentView, TabbedDocumentHost) already exists and works; this plan wires it into the product: focus, intent API, scripts-as-tabs, per-scene docks, persistence, and finally retirement of the legacy strip.

## How to use this doc (codex working agreement — non-negotiable)

- **Build** (from repo root, use the git-bash Bash tool):
  `py -m SCons platform=windows target=editor redirect_build_objects=yes winrt=no -j24`
  (`winrt=no` is REQUIRED.)
- **Smoke:** `bash workspace-editor-planning/smoke/run_smoke.sh` — must exit 0 with **zero** error-class lines. It copies the fixture to temp; **never** run the editor directly on the committed `smoke/` fixture (it pollutes `project.godot` / writes `.godot/`). The harness only exercises the MAIN-view path; the manual checks listed per step are therefore mandatory, not optional. **Milestone-S exception (owner decision 2026-07-04):** for S5.5–S7 the per-step manual checks are batched into ONE end-of-milestone interactive pass by the owner; each commit still gates on build + smoke green.
- **Commit per green step.** Each sub-step below is one commit: build clean + smoke green + manual check done → commit with the given message stub. Never batch two sub-steps into one commit.
- **Remove all temp probes/prints before committing.** If you add a temporary print, `fflush(stdout)` after it (block buffering misorders crash output).
- **Run `/simplify`** after landing each milestone (and after any sub-step whose diff exceeded ~500 lines).
- **Update `workspace-editor-planning/DIVERGENCE-LEDGER.md` in the same commit** whenever a stock-file divergence is added or materially changed. Keep in-code `// G2` tags on every diverged hunk (use the step id, e.g. `// G2 S6a`).
- **Rebase policy:** never rebase onto upstream mid-milestone; milestone boundaries only (ledger policy).
- **The taxonomy tiebreaker** (ARCHITECTURE.md): shared editing SERVICES stay singleton behind `get_singleton()`; per-pane/per-document VIEWS are instanceable. Litmus test: *"If I opened a second pane, would this value be the same? Yes → service; No → view."* When any classification question comes up mid-step, answer it with this test — do not invent a third category.
- Line anchors below were verified against the current branch; if a file has drifted a few lines, locate the construct by name, not by number.

## Locked-decisions digest (do not re-litigate)

| ID | Decision (locked) |
|----|-------------------|
| D1 | Two-layer intent API: `reveal(EditorDocument*, kind)` for document tabs + `focus_editor(StringName)` for singleton screens; `select(int)`/`select_by_name` stay forever as thin shims (platform/mono callers must NOT be edited); migrate the ~11 `EDITOR_SCRIPT` callers to `focus_editor("Script")`. |
| D2 | Staged rollout, **no feature flag**; 2D/3D already pane-native; others ride the legacy strip until retired (M7). |
| D3 | Render-visibility is already per-view (no work); interaction/main-screen visibility survives only for surviving singleton screens. Never touch the ~116 tool-plugin `make_visible` sites. |
| D4 | `EditorWorkspace` owns `focused_pane`; promote on mouse-down/focus-in. No Control-focus piggyback. Workspace owns UI focus; EditorNode/EditorData owns the model active-document. |
| D5 | Active-scene ↔ pane coupled lazily on interaction (generalize the 2D click-promote; add the 3D equivalent). Global active-document stays; `get_scene_root()` stays parameterized by active doc. |
| D6 | **Per-document docks (option A):** scene tree + inspector + node (signals/groups) dock are embedded per scene tab. Third services/view split of SceneTreeDock/InspectorDock. FileSystem + Import stay global. Requires **Model B** live per-document `EditorSelection` via a stable proxy for the ~5 cache-and-connect consumers; per-document `EditorSelectionHistory` likewise. |
| D7 | **Script-as-tab (option A):** ScriptEditor services/view split. Views = `ScriptTextEditor` / `EditorHelp` lifted into `TabbedDocumentHost`; **delete `script_list`** (pane tab bar is the script list); new script + help document kinds; debugger/"Edit Script" go through `reveal`. |
| D8 | Input/tool routing by focused pane; tool-mode/snap/gizmo-registry stay shared on the services singletons. Hover-for-nav deferred. |
| D9 | Persistence = the one one-way door. Versioned `[Workspace]` geometry (orientation/offset/stable pane-ids) in the editor layout; doc-set + per-pane tabs + per-view transforms in a per-project session store; transforms live on `EditorDocumentView::editor_states` keyed by pane-id. Version from the FIRST write. |
| D10 | Default = single root pane; migration shim reads legacy `selected_main_editor_idx`; GDStudio layout only as opt-in "reset layout" template (not in this plan's scope beyond the hook point). |
| D11 | Deprecate-and-parallel: `get_editor_main_screen()->get_control()` keeps returning `main_screen_vbox` byte-identical; `set_main_screen_editor(name)` routes through `focus_editor`. Never edit platform/JNI/mono/gridmap callers. |
| D12 | Bottom panel / distraction-free / expand-viewport stay global v1. Rewire only the single `update_distraction_free_mode()` coupling; expand-viewport maximizes the focused pane. |
| D13 | Game is a singleton pane; play focuses/creates its tab; stop restores a **guarded** previously-focused target handle. |
| D14 | Type-split identity: 2D/3D/Script/Help are document **view-kinds**; `EditorTable` shrinks to surviving singleton screens (Game, AssetLib) keyed by StringName. Legacy enumerators remain as deprecated aliases so un-editable callers (mono, macOS) keep compiling. |

## Shared vocabulary and seams (defined once, used by every milestone)

These are decisions this plan makes where the scoping notes left a detail open. They are now fixed; do not revisit.

1. **`DocumentViewKind`** — add to `editor/editor_document.h`:
   ```cpp
   enum class DocumentViewKind { DEFAULT, SCENE_2D, SCENE_3D, SCRIPT, HELP }; // G2 D14: view-kind, not screen.
   ```
   `DEFAULT` = derive from `EditorDocument::get_type()`.
2. **Intent API** (on `EditorMainScreen`):
   ```cpp
   void reveal(EditorDocument *p_document, DocumentViewKind p_kind = DocumentViewKind::DEFAULT);
   void focus_editor(const StringName &p_name); // Singleton screens by plugin name.
   ```
3. **Target-pane resolution rule** (for `reveal` of a document not yet in any tab): (a) the focused pane if its content is a `TabbedDocumentHost`; else (b) the most-recently-focused tabbed leaf (`EditorWorkspace::last_tabbed_pane`, updated whenever a tabbed pane gains focus); else (c) split the focused pane horizontally (new content on second/right) minting a new `TabbedDocumentHost`. If the document already has a tab in ANY pane, focus that existing tab instead (no duplicate tabs in v1; a second view of the same doc is the deferred multi-pane-editing milestone). **Post-S5.5 note (owner decision 2026-07-04):** the root pane hosts a `TabbedDocumentHost` from startup, so in the default layout case (a) always resolves and documents open as tabs **in the current pane — never a split-on-first-open**. Case (c) survives only as a fallback for panes holding non-tabbed content (debug scaffolding, transitional states).
4. **Pane-focus promotion mechanism** (assumption, chosen for determinism): leaf `WorkspacePane`s enable `set_process_input(true)` and override `Node::input()`; on `InputEventMouseButton` pressed whose position hits `get_global_rect()` (and the pane is visible in tree), promote via `workspace->set_focused_pane(this)`. `Node::input` runs in the pre-GUI input stage, so promotion happens before the click is consumed by any child control. Additionally `EditorWorkspace` connects to its `Viewport`'s `gui_focus_changed` and walks the focused Control's ancestry to the owning `WorkspacePane` (keyboard focus-in). Editor GUI lives in the root window with no canvas transform, so window coords == global rect coords.
5. **`ScreenHostDocument`** (lands early at **S5.5**, pulled forward from M5.1 — owner decision 2026-07-04: the tab system must be in place *before* scripts flip to tabs, so the first script open is a tab in the current pane, not a split): ONE special document (new `EditorDocument::TYPE_SCREEN_HOST`) whose `DocumentView` hosts `main_screen_vbox` itself. Focusing Game/AssetLib/any third-party main screen = `reveal(screen_host_doc)` + the legacy `_select_index` dance *inside* it. This keeps `get_editor_main_screen()->get_control()` returning the live `main_screen_vbox` (D11) and reuses the entire existing make_visible machinery inside one tab. M5.1 retains only the polish (retitle-per-screen, close/re-summon verification).
6. **Selection Model B implementation** (M4): each `SceneDocument` owns a real `EditorSelection` (the reserved member at `editor/editor_document.h:114` comes alive). The single `EditorNode::editor_selection` object becomes an `EditorActiveSelectionProxy : EditorSelection` — the public methods of `EditorSelection` (`editor/editor_data.h:291`) are virtualized and the proxy forwards every call to `EditorData::get_active_document()->get_selection()` (falling back to its own inherited storage when no scene document is active), relaying the active document selection's `selection_changed` signal as its own. The ~5 cache-and-connect consumers and the 48 call sites keep their pointer and their signal connection untouched.
7. **Selection history**: `EditorNode::editor_history` (`editor/editor_node.h:806`) keeps its identity (cached pointers exist, e.g. `EditorObjectSelector` ctor at `editor/docks/inspector_dock.cpp:804`) and keeps today's swap-on-activate behavior for global consumers; per-document dock views (M4) bind directly to `SceneDocument::selection_history` (`editor/editor_document.h:115`).
8. **Shared chrome follows focus** (assumption): the shared, singleton-owned toolbars/menus (ScriptEditor's file menu strip in M3; CanvasItemEditor/Node3DEditor main toolbars in M7) are single Controls **reparented into the focused pane's header** when that pane's active tab is of the matching kind, and parked in a hidden holder under their services singleton otherwise. Per-script edit menus already travel with `ScriptTextEditor` (`ScriptEditorBase::get_edit_menu`), so only the truly-global chrome moves.
9. **Session store path** (assumption): `res://.godot/editor/workspace_session.cfg` via `ConfigFile` (resolved through `EditorPaths`/project-settings dir like the existing per-project editor metadata), written/read by the `EditorNode::_save/_load_editor_layout` orchestrator hooks.

---

## Milestone 1 — Foundational focus + intent API (D1, D4, D5)

**Goal:** the workspace feels live — clicking a pane focuses it, clicking an inactive document's 3D/2D view promotes its document, and all "switch main screen" intent flows through `reveal`/`focus_editor` with `select()` preserved as a shim. No selection/dock/persistence changes.
**Services/view split introduced:** none (wiring only).

### M1.1 — Promote pane focus on mouse-down / focus-in

- **Intent:** `EditorWorkspace::focused_pane` is actually promoted by interaction, with a visible focus ring and a signal others can subscribe to.
- **Files:** `editor/gui/editor_workspace.{h,cpp}` (`focused_pane` + `get/set_focused_pane` at `editor_workspace.h:96,121-122`; `WorkspacePane` at `:50`).
- **Add:**
  - `WorkspacePane::input(const Ref<InputEvent>&)` override implementing seam #4 (leaf-only: enable input processing in `set_content`, disable in `split`).
  - Make `EditorWorkspace::set_focused_pane` out-of-line: update member, `queue_redraw()` old+new pane, update `last_tabbed_pane` if the new pane's content is a `TabbedDocumentHost`, emit new signal `focused_pane_changed` (register in `_bind_methods`).
  - `EditorWorkspace::_notification(NOTIFICATION_READY)`: connect `get_viewport()->gui_focus_changed` → `_on_gui_focus_changed(Control*)` walking ancestry to an owned `WorkspacePane`.
  - Focus ring: `WorkspacePane` draws a 2px accent border in `NOTIFICATION_DRAW` when `workspace->get_focused_pane() == this` and the workspace has >1 leaf (no ring in the default single-pane layout — UX-neutral for stock use).
- **Keep green:** `get_focused_pane()` already falls back to `root_pane`; nothing consumes the signal yet; single-pane default draws no ring, so stock UX is pixel-identical.
- **Verify:** build + smoke; manual: launch editor on a scratch project (never the smoke fixture), use the existing debug split (`_debug_split_focused_with_tabs`), click between panes → ring follows; Tab-focus into a pane's control → ring follows.
- **Commit:** `G2 M1.1: promote workspace pane focus on mouse-down/focus-in`

### M1.2 — 3D view promotes its document on click (generalize the 2D gate)

- **Intent:** clicking a non-active pane's 3D view makes its document the active edited scene, mirroring the 2D `_ensure_active` pattern.
- **Files:** `editor/scene/3d/node_3d_editor_plugin.{h,cpp}` (`Node3DEditorView` at `node_3d_editor_plugin.h:1099`, `bound_world` `:1112`, `set_active_world` `:1153`, `Node3DEditor::set_active_world` decl `:965`, impls at `node_3d_editor_plugin.cpp:8288/10015`); `editor/gui/document_view.cpp:53-62`. **Template to mirror:** `CanvasItemEditorView::_is_active_document()/_edits_gated()/_ensure_active()` at `editor/scene/canvas_item_editor_plugin.cpp:4317-4351`.
- **Add:**
  - `EditorDocument *document = nullptr` member on `Node3DEditorView`; change `Node3DEditor::create_view_bound_to(Ref<World3D>)` to take `EditorDocument *` (derive the world inside; `document_view.cpp:60` is the only caller — update it to pass `p_document`).
  - `Node3DEditorView::_is_active_document()` and `_ensure_active()` (resolve index via `EditorData::find_document_index(document)` → `EditorNode::set_edited_scene_index`), verbatim ports of the 2D bodies.
  - Promotion hook: `Node3DEditorView::input()` override (same mechanism as M1.1): mouse-button pressed inside the view's global rect, `document` non-null and not active → `_ensure_active()`. (Do NOT thread this through `Node3DEditorViewport::_sinput` — the pre-GUI input stage guarantees the same click then lands on the already-active editor.)
- **Keep green:** `document == nullptr` (the main shim view) never promotes; 2D path untouched.
- **Verify:** build + smoke; manual: two panes, two 3D scenes; click the inactive pane's viewport → its doc becomes active (gizmos/current-scene tab follow), the click's camera navigation still works.
- **Commit:** `G2 M1.2: 3D view promotes its document on click (mirror 2D _ensure_active)`

### M1.3 — Intent API `reveal()`/`focus_editor()`; `select()`/`select_by_name` become shims

- **Intent:** introduce the two-layer intent API with the legacy entry points delegating to it, behavior byte-identical.
- **Files:** `editor/editor_main_screen.{h,cpp}` (`select` at `editor_main_screen.cpp:171-209`, `select_by_name` `:158-169`, `main_editor_plugins` map at `editor_main_screen.h:62`); `editor/editor_document.h` (add `DocumentViewKind`, seam #1).
- **Add:**
  - Rename the body of `select(int)` to private `_select_index(int)` (keep the `is_changing_scene` guard, hidden-button early-return, `update_distraction_free_mode()` call — all inside `_select_index`). Public `select(int)` = `{ _select_index(p_index); }` tagged `// G2 D1 shim: platform/mono callers must keep working — do not remove.` Preserve the `select(-1)` semantics from `_notification(NOTIFICATION_READY)` exactly (same guard order).
  - `void focus_editor(const StringName &p_name)`: look up `main_editor_plugins`, `_select_index(get_plugin_index(plugin))`; `ERR_FAIL_MSG` on unknown name (same message shape as `select_by_name`).
  - Rewrite `select_by_name` = `{ focus_editor(p_name); }` (plugin name == button text today, so matching is unchanged).
  - `void reveal(EditorDocument*, DocumentViewKind = DEFAULT)`: v1 shim — for scene documents: `EditorNode::set_edited_scene_index(find_document_index(doc))` then `focus_editor("2D")`/`("3D")` chosen by kind (or by `classify_scene_type` for DEFAULT); for other types: `WARN_PRINT_ONCE` + no-op (real body lands in S5).
- **Keep green:** every existing caller flows through unchanged code paths; `EditorInterface::set_main_screen_editor` (`editor/editor_interface.cpp:431`) now transits `focus_editor` via the `select_by_name` shim — D11 satisfied with zero edits outside this file pair.
- **Verify:** build + smoke; manual: all five strip buttons switch; `EditorInterface.set_main_screen_editor("Script")` from a tool script works.
- **Commit:** `G2 M1.3: intent API reveal()/focus_editor(); select()/select_by_name become shims`

### M1.4 — Reroute the `EDITOR_SCRIPT` callers to `focus_editor("Script")`

- **Intent:** the "focus the Script editor" intent sites stop naming an enum index.
- **Files (exact 12 sites; leave `editor_main_screen.cpp:299` internal, and do NOT edit `modules/mono/editor/editor_internal_calls.cpp:171` or anything under `platform/`):**
  `editor/inspector/editor_inspector.cpp:1547,1991` · `editor/editor_node.cpp:422` · `editor/doc/editor_help_search.cpp:192` · `editor/docks/scene_tree_dock.cpp:1323` · `editor/docks/inspector_dock.cpp:106` · `editor/doc/editor_help.cpp:2349,4273` · `editor/scene/connections_dialog.cpp:1302,1310,1340` · `editor/script/script_editor_plugin.cpp:951`.
- **Change:** `...->get_editor_main_screen()->select(EditorMainScreen::EDITOR_SCRIPT)` → `...->get_editor_main_screen()->focus_editor(SNAME("Script"))`.
- **Keep green:** identical behavior through `focus_editor` → `_select_index`.
- **Verify:** build + smoke; manual: "Open in Script Editor" from inspector, connect-signal dialog "Open method" — both land on Script screen.
- **Commit:** `G2 M1.4: reroute EDITOR_SCRIPT callers to focus_editor("Script")`

---

## Milestone 2 — Visibility-contract scoping (D3)

**Goal:** pin down and enforce the split between per-view render visibility (already done, needs nothing) and singleton-screen interaction visibility, so later milestones can't regress panes by driving `make_visible`.
**Services/view split introduced:** none (contract + audit).

### M2.1 — Audit + contract: main-screen `make_visible` never affects pane views

- **Intent:** prove (and fix if needed) that switching the legacy strip cannot blank or disturb a pane-hosted view, and document the contract at the seam.
- **Files:** `editor/plugins/editor_plugin.cpp` (`make_visible` at `:361`, `has_main_screen` at `:355`) — comment only; `editor/scene/canvas_item_editor_plugin.cpp` + `editor/scene/3d/node_3d_editor_plugin.cpp` — the plugin `make_visible` bodies (`CanvasItemEditorPlugin::make_visible`, `Node3DEditorPlugin::make_visible`); `editor/editor_main_screen.cpp:193-199` (the only driver).
- **Do:**
  - Add a `// G2 D3` doc-comment on `EditorPlugin::make_visible` stating the contract: *"main-screen visibility applies to the singleton screen surface only; per-pane document views own their render visibility (SubViewport UPDATE_WHEN_VISIBLE) and must never be driven by this call."*
  - Read both plugin `make_visible` bodies; confirm they only show/hide the singleton's main container / toolbars and singleton-owned overlays. If any code path hides state shared with pane views (e.g. a services-owned control a `CanvasItemEditorView`/`Node3DEditorView` renders through), scope the hide to the main view container and tag `// G2 D3`.
  - Do NOT touch any of the ~116 tool-plugin `make_visible` sites.
- **Keep green:** comment-plus-surgical change; strip switching unchanged.
- **Verify:** build + smoke; manual regression: split a pane showing a 3D doc and one showing a 2D doc; cycle the legacy strip through all five screens; both pane views keep rendering and stay clickable (M1.2 promotion still works).
- **Commit:** `G2 M2.1: D3 visibility contract — main-screen make_visible scoped to singleton screens (audit + tags)`

---

## Milestone S (3) — Script-as-tab (D7)

**Goal:** scripts and class docs become first-class document tabs. `ScriptEditor` keeps its singleton (45 external callers untouched) as the scripting SERVICES: menus, find-in-files, save-all, history routing, debugger goto. The per-script VIEW (`ScriptTextEditor`/`EditorHelp`) is lifted into `TabbedDocumentHost`; the internal `script_list` and hidden `TabContainer` hosting are deleted at the end.
**Services/view split introduced:** ScriptEditor (services) vs ScriptTextEditor/EditorHelp (views).

### S1 — Unified open-view registry inside ScriptEditor

- **Intent:** decouple "the set of open scripts" from `tab_container` children so views can live anywhere.
- **Files:** `editor/script/script_editor_plugin.{h,cpp}` (`script_list` at `script_editor_plugin.h:168`, `tab_container` `:181`, `_update_script_names` `:323`, `_go_to_tab` `:369`).
- **Add:** `Vector<ScriptEditorBase *> registered_views;` + `void _register_view(ScriptEditorBase*)` / `void _unregister_view(ScriptEditorBase*)` (also auto-unregister via `tree_exiting` connection). The internal open path registers on creation; `_close_tab` unregisters.
- **Repoint** every site whose semantics are "all open script views" from `tab_container->get_child(...)` loops to `registered_views`: `_update_script_names`, `save_all_scripts`, `get_open_scripts`, `get_open_script_editors`, `apply_scripts`, `reload_scripts`, breakpoint/debugger sync loops, find-in-files "is it open" checks. **Leave** genuinely-index-based sites (`_go_to_tab`, history stacks, `_get_current_editor`) on `tab_container` for now — they are dispositioned in S6/S7. Build the disposition by grepping `tab_container->` and classifying each hit (`all-open` vs `current/index`); keep the classification as a checklist in the commit message body.
- **Keep green:** registry contents == tab children today; iteration order = registration order == tab order.
- **Verify:** build + smoke; manual: open 3 scripts, Save All, close one, find-in-files across them.
- **Commit:** `G2 S1: ScriptEditor view registry decouples open scripts from the internal TabContainer`

### S2 — Extract `ScriptEditor::create_editor_view()` factory

- **Intent:** the "create + fully wire a ScriptEditorBase for this resource" block becomes callable without parenting into `tab_container`.
- **Files:** `editor/script/script_editor_plugin.{h,cpp}` — the creation block inside the open path (`_open_script` / `edit`): the `script_editor_func` selection, `memnew`, `set_edited_resource`, syntax highlighter registration, and the full signal wiring (`name_changed`, `edited_script_changed`, `request_help`, `request_open_script_at_line`, `go_to_help`, `search_in_files_requested`, `replace_in_files_requested`, `request_save_history`, etc. — move the block verbatim).
- **Add:** `ScriptEditorBase *create_editor_view(const Ref<Resource> &p_resource);` (public; calls `_register_view` internally). Internal path becomes `se = create_editor_view(script); tab_container->add_child(se); ...`.
- **Keep green:** pure extraction; internal path identical.
- **Verify:** build + smoke; manual: open scripts of each flavor (GDScript, .txt/text resource, doc page) — identical behavior.
- **Commit:** `G2 S2: extract ScriptEditor::create_editor_view() factory (wire without parenting)`

### S3 — Script/Help document kinds + EditorData aux-document registry

- **Intent:** the model learns about script and help documents.
- **Files:** `editor/editor_document.{h,cpp}`; `editor/editor_data.{h,cpp}` (near `get_active_document()` at `editor_data.cpp:855`).
- **Add:**
  - `TYPE_HELP` to `EditorDocument::Type` (append; do not reorder).
  - `class ScriptDocument : public EditorDocument { Ref<Resource> script; ... }` (type `TYPE_SCRIPT`, `path` = resource path; getter `get_script_resource()`).
  - `class HelpDocument : public EditorDocument { String class_name; ... }` (type `TYPE_HELP`, `path` = `"help://" + class_name`).
  - `EditorData`: `Vector<EditorDocument *> aux_documents;` + `ScriptDocument *get_or_create_script_document(const Ref<Resource>&)`, `HelpDocument *get_or_create_help_document(const String &p_class)`, `void close_aux_document(EditorDocument*)`, lookup-by-path helper; `memdelete` all in EditorData teardown. (Scene documents stay where they are, on `EditedScene` — do not unify storage now.)
- **Keep green:** additive, nothing calls it yet.
- **Verify:** build + smoke.
- **Commit:** `G2 S3: ScriptDocument/HelpDocument kinds + EditorData aux-document registry`

### S4 — DocumentView/TabbedDocumentHost host ScriptTextEditor/EditorHelp

- **Intent:** a workspace tab can present a script or help document, fully wired to the services.
- **Files:** `editor/gui/document_view.{h,cpp}` (ctor kind-routing at `document_view.cpp:53-62`); `editor/gui/tabbed_document_host.cpp` (tab titles); `editor/gui/editor_workspace.cpp` (`_debug_split_focused_with_tabs`, decl `editor_workspace.h:115`).
- **Add:**
  - `document_view.cpp` routing: `TYPE_SCRIPT` → `ScriptEditor::get_singleton()->create_editor_view(script_doc->get_script_resource())` (registration happens inside the factory); `TYPE_HELP` → `memnew(EditorHelp)` + `go_to_help("class_name:" + name)`. `~DocumentView` calls `ScriptEditor::_unregister_view` for script surfaces (expose a public `release_editor_view(ScriptEditorBase*)` on ScriptEditor for this — the dtor must not reach into privates).
  - Tab titles in `TabbedDocumentHost::add_document`: `doc->get_path().get_file()` fallback to type name; connect the view's `name_changed` (script views) to a title refresh.
  - Extend `_debug_split_focused_with_tabs` (existing TEMPORARY scaffolding — keep, no prints) to also add a `ScriptDocument` tab for the first open script, so this step is manually exercisable.
- **Keep green:** unreachable except via the debug split.
- **Verify:** build + smoke; manual: debug-split → script tab: edit text, Ctrl+S saves, Save All (services) saves it, breakpoints toggle margin works; help tab renders a class page.
- **Commit:** `G2 S4: DocumentView/TabbedDocumentHost host ScriptTextEditor/EditorHelp views`

### S5 — `reveal()` opens script/help documents into a tabbed pane

- **Intent:** implement the real `reveal` path with the target-pane resolution rule (seam #3).
- **Files:** `editor/editor_main_screen.{h,cpp}`; `editor/gui/editor_workspace.{h,cpp}`; `editor/gui/tabbed_document_host.{h,cpp}`.
- **Add:**
  - `TabbedDocumentHost::focus_document(EditorDocument*)` — find in `documents` (`tabbed_document_host.h:57`) → `set_current`, else `add_document` + select. Also `bool has_document(EditorDocument*) const`.
  - `EditorWorkspace::resolve_target_pane_for_documents()` implementing seam #3, plus `WorkspacePane *find_pane_showing(EditorDocument*)` (tree walk over leaf `TabbedDocumentHost`s) for the no-duplicate-tab rule.
  - `EditorMainScreen::reveal` (script/help arm): existing tab anywhere → focus that pane + tab; else target-pane resolution → `focus_document`. Set workspace focused pane to the target.
- **Keep green:** still no production caller; verify with a temporary trigger (e.g. temporarily point one `focus_editor("Script")` site at `reveal`) and **remove the probe before committing**.
- **Verify:** build + smoke; manual (with probe, then removed): reveal opens a tab in the focused tabbed pane; revealing the same script twice focuses, not duplicates; reveal with only the legacy root pane splits it.
- **Commit:** `G2 S5: reveal() opens script/help documents into a workspace tab pane`

### S5.5 — ScreenHostDocument: the main area becomes a tab host (pulled forward from M5.1)

- **Intent:** the root pane hosts a `TabbedDocumentHost` from startup, with the legacy main-screen stack living inside tab 0 as a `ScreenHostDocument` view. This is the prerequisite for S6a: with it, `reveal` of the first script resolves to case (a) of seam #3 and opens **a tab in the current pane** — no split. (Owner decision 2026-07-04: "It should be a tab on the current pane. So we need to make sure tab system in place first.")
- **Files:** `editor/editor_document.h` (add `TYPE_SCREEN_HOST` — append, do not reorder); `editor/editor_main_screen.{h,cpp}` (ctor: root-pane content; `get_control()`; `focus_editor`); `editor/gui/document_view.{h,cpp}` (`TYPE_SCREEN_HOST` arm); `editor/gui/tabbed_document_host.cpp` (title fallback for the screen-host doc).
- **Do:**
  - `ScreenHostDocument : EditorDocument` (type `TYPE_SCREEN_HOST`, path `"screens://"`, no world). `EditorMainScreen` owns the single instance (member, freed in dtor if never adopted by EditorData — it is NOT registered as a scene or aux document; it is workspace-only).
  - `DocumentView` `TYPE_SCREEN_HOST` arm: reparent `main_screen_vbox` into the view (`reparent()`, keep size flags EXPAND_FILL). `~DocumentView`: if the vbox is still our child, reparent it back to a hidden compat holder (`Control` child of `EditorMainScreen`, `set_visible(false)`) — `get_control()` returns the live `main_screen_vbox` at every instant (D11, byte-identical pointer).
  - `EditorMainScreen` ctor: root pane content = a `TabbedDocumentHost` whose tab 0 is the `ScreenHostDocument` (title `"Editor"` v1 — `"screens://".get_file()` is unusable; per-screen retitle is M5.1 polish). The tab is created current, so at startup `main_screen_vbox` is visible exactly as today and `_select_index(-1)`/READY default-select behave unchanged.
  - `focus_editor(name)`: prepend `reveal(screen_host_doc)` before `_select_index`, so strip buttons refocus the screen-host tab when a script tab is current (2 lines; the rest of M5.1's dance already works because `_select_index` runs inside the hosted vbox).
  - `TabbedDocumentHost::_activate_document`: screen-host doc has no scene index (`find_document_index` returns -1) → already a no-op; verify, do not special-case.
- **Keep green:** startup layout is pixel-identical except a one-row tab bar above the main screen; `get_control()` callers (third-party plugins parent into it) untouched; smoke exercises this on every scene open — this step has REAL harness coverage, unlike S6a+.
- **Verify:** build + smoke; manual: editor boots showing the Editor tab with 3D screen inside; strip buttons still switch screens inside the tab; debug-split still works.
- **Commit:** `G2 S5.5: ScreenHostDocument — main area becomes a tab host; legacy screens live in tab 0`

### S6a — Flip: `ScriptEditor::edit()` + goto/debugger routing go through `reveal`

- **Intent:** the canonical open-script entry (all 45 external `get_singleton()` callers funnel here) now summons a workspace tab; "current script" re-sources from the focused pane.
- **Files:** `editor/script/script_editor_plugin.{h,cpp}` (`edit(...)`, `goto_line`, `script_goto_method`, the debugger stack-frame goto path `_goto_script_line`); `editor/gui/tabbed_document_host.cpp` / `editor/gui/document_view.cpp` (current-view notification).
- **Do:**
  - `edit(p_script, line, col, grab_focus)`: honor the external-editor setting first (unchanged early-return), then `EditorData::get_or_create_script_document` → `EditorMainScreen::reveal(doc, SCRIPT)` → resolve the now-existing view from `registered_views` (match by edited resource) → apply `goto_line(line, col)` and focus.
  - Add `ScriptEditor::set_current_view(ScriptEditorBase*)` + re-implement `_get_current_editor()` as: the explicitly-set current view if alive, else `tab_container` current (legacy fallback). `TabbedDocumentHost::_on_tab_selected`/`DocumentView` calls `set_current_view` whenever a script tab becomes the active tab of the focused pane; `EditorWorkspace::focused_pane_changed` re-derives it. All "act on current script" operations (save, soft-reload, toggle breakpoint, run) now follow pane focus.
  - `goto_line`/`script_goto_method`/debugger goto: resolve the view via the registry; if the script isn't open, they already route through `edit()` — now workspace-native.
- **Keep green:** the internal `tab_container` path still exists (old tabs keep working until S7); external-editor setting unchanged; `EditorHelp` opens still internal until S6b. With S5.5 in place, the first script open lands as a tab beside the Editor tab in the root pane — verify NO split occurs in the default layout.
- **Verify:** build + smoke; manual: attach-script from Scene dock opens a tab; double-click script in FileSystem opens a tab; debugger breakpoint hit jumps to tab + line; Ctrl+S with a script pane focused saves that script.
- **Commit:** `G2 S6a: ScriptEditor::edit()/goto/debugger route through reveal(); current-script follows pane focus`

### S6b — Help documents + script navigation history over the workspace

- **Intent:** class-reference opens become help tabs; back/forward history survives the lift.
- **Files:** `editor/script/script_editor_plugin.{h,cpp}` (`_help_class_open`, `_help_class_goto`, `_history_forward/_history_back` and their `ScriptHistory` records); the M1.4 help callers need no edits (they call `focus_editor("Script")` → see below).
- **Do:**
  - `_help_class_open/goto` → `get_or_create_help_document(class)` + `reveal(doc, HELP)` + `go_to_help` on the view.
  - Rework `ScriptHistory` records from tab-index-based to `{ObjectID view, Variant state}`; forward/back call `reveal` on the record's document (guard dead ObjectIDs by skipping records).
  - Special-case inside `focus_editor`: `p_name == "Script"` → if any script/help doc is open, `reveal` the most-recent one (track most-recent revealed script doc on ScriptEditor); else fall through to `_select_index` (empty legacy screen). Tag `// G2 S6b`.
- **Keep green:** legacy internal help tabs (pre-existing) still close normally; strip Script button now lands you on the last script tab.
- **Verify:** build + smoke; manual: F1/class link opens help tab; Alt+Left/Right walks history across script and help tabs; `focus_editor("Script")` sites (connect dialog, inspector "Open method") land on tabs.
- **Commit:** `G2 S6b: help documents as tabs; script history + focus_editor("Script") over the workspace`

### S7 — Delete `script_list` + internal tab hosting; shared chrome follows focus

- **Intent:** the pane tab bar IS the script list; the ScriptEditor panel stops hosting content and survives as services + dialog owner.
- **Files:** `editor/script/script_editor_plugin.{h,cpp}` (`script_list` `:168`, `_script_list_clicked` `:348`, `_make_script_list_context_menu` `:349`, `tab_container` `:181`, `window_wrapper` `:201/:470`, `members_overview`).
- **Do (grep `tab_container->` and `script_list->`, disposition every hit — the S1 checklist is the starting point):**
  - Delete `script_list`, its context menu, its sort-mode settings plumbing; `_update_script_names` shrinks to "refresh registry metadata + notify tab-title refresh".
  - Stop adding children to `tab_container`; remaining `current`-based logic already re-sourced in S6a. Remove the container (or leave an empty stub only if a dialog anchors to it — prefer removal).
  - `members_overview` (methods panel) is REMOVED with the left panel — **explicitly deferred** to G3's ScriptMethodsDock; note it in the commit message and in G3-contextual-docks.md's backlog if present.
  - Shared chrome (seam #8): the ScriptEditor menu strip (File menu, Search/Debug menus not owned per-view) moves into a single `HBoxContainer` that `DocumentView` mounts into a slim header of the focused script tab's view (reparent-on-focus; parked hidden under ScriptEditor otherwise). Per-view `get_edit_menu()` chrome mounts beside it.
  - Disable the Script "Make Floating" affordance (`window_wrapper`) — floating an empty services shell is meaningless now; tag `// G2 S7`, keep the WindowWrapper plumbing for the singleton screens.
- **Keep green:** every services API (`get_open_scripts`, save-all, breakpoints, find-in-files) already iterates the registry (S1); the Script screen in the legacy strip is now an empty shell reached only if zero docs are open (S6b special-case) — acceptable until M7 removes it.
- **Verify:** build + smoke; manual sweep: open/close/save/save-all/soft-reload/find-in-files/replace-in-files/breakpoints/debugger goto/help/history — all tab-native; no dangling `script_list` UI.
- **Commit:** `G2 S7: retire ScriptEditor internal script_list/tab hosting; workspace tabs are the script list`
- Run `/simplify` over the milestone-S diff after this lands.

### S8 — Pane management UX (split/close without debug keys)

- **Intent:** users can split and close panes deliberately; scaffolding stops being the only entry.
- **Files:** `editor/gui/editor_workspace.{h,cpp}`, `editor/gui/tabbed_document_host.{h,cpp}`.
- **Add:**
  - `EditorWorkspace::close_pane(WorkspacePane*)`: collapse the parent split (surviving sibling's content takes the parent leaf slot; fix up `focused_pane`/`last_tabbed_pane`; root pane never closes).
  - Tab-bar context menu (right-click) on `TabbedDocumentHost`: *Split Right*, *Split Down* (splits the owning pane, new `TabbedDocumentHost` on second, moves the right-clicked tab into it), *Close Tab*, *Close Pane*.
  - Tab close (`_on_tab_close`, `tabbed_document_host.h:66`): closing the last tab of a non-root pane closes the pane.
- **Keep green:** `_debug_split_focused*` stays until M7.4; no default layout change.
- **Verify:** build + smoke; manual: split via context menu, move a script tab into the new pane, close tabs/panes back down to one.
- **Commit:** `G2 S8: pane split/close UX (tab-bar context menu, close_pane collapse)`

---

## Milestone D (4) — Per-scene-tab docks + Model B selection (D6)

**Goal:** each scene tab is a composite view — embedded scene tree, inspector, node (signals/groups) dock — each bound to a live per-document `EditorSelection`/history, with the global selection accessor becoming a retargeting proxy. FileSystem + Import (+ History) docks stay global.
**Services/view split introduced:** SceneTreeDock/InspectorDock/NodeDock (per-document views) vs their shared dialogs/registries (services); EditorSelection (per-document) vs the active-selection proxy (global facade).

### D1 — SceneDocument owns a live EditorSelection (mirrored, not yet authoritative)

- **Intent:** bring the reserved `SceneDocument::selection` (`editor/editor_document.h:114`) alive without changing who is authoritative.
- **Files:** `editor/editor_document.{h,cpp}`; `editor/editor_data.cpp` (`save/restore_edited_scene_state` at `:997-1019`); `editor/editor_node.cpp` (`_set_current_scene_nocheck` — locate by name; the switch path that calls save/restore).
- **Do:** `SceneDocument` ctor `memnew(EditorSelection)`, dtor `memdelete`; in `save_edited_scene_state`, additionally copy the global selection's node set into the outgoing doc's selection; in `restore_edited_scene_state`, prefer restoring the global from the incoming doc's live selection when non-null (Dictionary snapshot remains the fallback for docs that predate this).
- **Keep green:** global selection remains the single authority; the doc copy is write-behind.
- **Verify:** build + smoke; manual: select nodes in scene A, switch A→B→A, selection intact (same as today).
- **Commit:** `G2 D1: SceneDocument owns a live EditorSelection (mirrored on scene switch)`

### D2 — Model B: the global selection becomes a retargeting proxy

- **Intent:** the highest-risk flip — `EditorNode::editor_selection` becomes `EditorActiveSelectionProxy` (seam #6); per-document selections become authoritative.
- **Files:** `editor/editor_data.{h,cpp}` (`EditorSelection` at `editor_data.h:291`); `editor/editor_node.{h,cpp}` (member `editor_node.h:274`, `get_editor_selection()` `:805`); `editor/editor_document.cpp` (activate/deactivate hooks).
- **Do:**
  - **First** enumerate the consumer surface: grep every method called on `get_editor_selection()` results and on the ~5 cached pointers (scene dock, scene-tree editor, 3D, 2D, control toolbar — fwd-decl sites listed at `scene_tree_dock.h:40`, `scene_tree_editor.h:38`, `canvas_item_editor_plugin.h:44`, `control_editor_plugin.h:41`, `node_3d_editor_plugin.h:52`). Virtualize exactly that method set on `EditorSelection` (`virtual` + `override` in the proxy); leave unused methods non-virtual.
  - `EditorActiveSelectionProxy : EditorSelection`: forwards each virtualized call to the active `SceneDocument`'s selection (fallback: inherited own storage when no scene doc — empty project, script-only session); on active-document change (hook `SceneDocument::activate/deactivate`, driven from the scene-switch path), moves a relay connection of the doc selection's `selection_changed` → proxy re-emit, and emits once to refresh consumers.
  - `EditorNode` allocates the proxy; delete the selection half of `save/restore_edited_scene_state` (the D1 mirror becomes the real store; keep the history half untouched).
  - Debug-build consistency probe allowed during development (`DEV_ASSERT` comparing proxy list vs doc list) — remove before commit.
- **Keep green:** all 48 call sites and 5 cached pointers compile untouched; signal identity preserved (same Object, same signal name).
- **Verify:** build + smoke (all three scenarios); manual (thorough — this is the risk gate): select/deselect in tree + 2D + 3D; scene switch preserves per-scene selection; delete a selected node; undo/redo of selection-affecting ops; multi-select drag in 2D; gizmo drag in 3D; control toolbar anchors UI follows selection.
- **Commit:** `G2 D2: Model B — global EditorSelection is a retargeting proxy over per-document selections`

### D3 — Per-document selection history wiring

- **Intent:** history follows the same per-document shape without breaking cached pointers.
- **Files:** `editor/editor_node.{h,cpp}` (`editor_history` member, `get_editor_selection_history()` at `editor_node.h:806`); `editor/editor_data.cpp` (history half of save/restore at `:997-1019`); `editor/editor_document.h:115`.
- **Do:** the existing snapshot/swap of `EditorSelectionHistory` re-sources to `SceneDocument::selection_history` as the backing store (save = copy global→doc member, restore = copy doc→global), replacing the `EditedScene`-Dictionary snapshot. `EditorNode::editor_history` keeps its identity (cached at `inspector_dock.cpp:804` and the `animation_track_editor.cpp` sites resolve fresh each call — safe).
- **Keep green:** behavior identical to today's snapshot; per-doc views (D6/D7) will bind `SceneDocument::get_selection_history()` directly.
- **Verify:** build + smoke; manual: inspector back/forward per scene survives scene switching.
- **Commit:** `G2 D3: selection history backed by SceneDocument (global accessor unchanged)`

### D4 — SceneTreeDock reads through a bound-document resolver

- **Intent:** mechanical parameterization — every ambient global read inside SceneTreeDock/SceneTreeEditor resolves through "my bound document, else the active one".
- **Files:** `editor/docks/scene_tree_dock.{h,cpp}`; `editor/scene/scene_tree_editor.{h,cpp}`.
- **Add:** `EditorDocument *bound_document = nullptr;` + private inline resolvers on SceneTreeDock: `EditorSelection *_doc_selection() const`, `Node *_doc_scene_root() const` (doc root else `EditorNode::get_edited_scene()`), `EditorSelectionHistory *_doc_history() const`. Replace member/ambient reads (`editor_selection` cached member, `EditorNode::get_singleton()->get_edited_scene()`, `get_editor_selection_history()` at `scene_tree_dock.cpp:115,1840,2946`) with resolver calls. Same treatment for the selection pointer SceneTreeEditor holds (make it a settable member fed by its owning dock).
- **Keep green:** `bound_document == nullptr` everywhere → resolves to exactly today's globals (and the proxy).
- **Verify:** build + smoke; manual: full scene-dock sweep (add/rename/reparent/instantiate/attach script/cut-paste).
- **Commit:** `G2 D4: SceneTreeDock/SceneTreeEditor read scene/selection/history through a bound-document resolver`

### D5 — SceneTreeDock shared dialogs become instance-independent services

- **Intent:** the heavyweight dialogs are shared once, safe for N dock instances.
- **Files:** `editor/docks/scene_tree_dock.{h,cpp}`.
- **Do:** move the owned dialogs (`create_dialog`, `script_create_dialog`, `shader_create_dialog`, rename dialog, quick-open, and any other `memnew`-in-ctor dialog) behind lazily-created static/shared accessors (`static CreateDialog *_shared_create_dialog()` pattern, parented to EditorNode's GUI base). Every invocation binds its completion callbacks to `this` (the invoking instance) at request time, not at construction — audit each `connect` in the ctor and move per-request connects to the open call (`connect(..., CONNECT_ONE_SHOT)` where the dialog is confirm-style).
- **Keep green:** single instance today → identical behavior; the change is only *when* callbacks bind.
- **Verify:** build + smoke; manual: create node, attach script, change type, rename via dialog — twice in a row (ONE_SHOT rebinding check).
- **Commit:** `G2 D5: SceneTreeDock shared dialogs are instance-independent services`

### D6 — InspectorDock + NodeDock through the bound-document resolver

- **Intent:** same parameterization for the inspector and signals/groups docks.
- **Files:** `editor/docks/inspector_dock.{h,cpp}` (history reads at `:316,322,373,410,420,430,552`; `EditorObjectSelector` at `:804`); `editor/docks/node_dock.{h,cpp}`.
- **Do:** `bound_document` + resolvers as in D4 (`_doc_selection/_doc_history`); the ctor-cached `EditorObjectSelector(history)` takes the resolver result at construction (a bound instance is constructed with its doc's history; the global instance keeps the EditorNode history). The property-editor plugin registry (`EditorInspector::add_inspector_plugin`, static) is already a service — no change. NodeDock's connections/groups panels re-source their target node from `_doc_selection()`.
- **Keep green:** null-bound instances == today.
- **Verify:** build + smoke; manual: inspect properties, sub-resource drill-in, back/forward, signals connect dialog, group add/remove.
- **Commit:** `G2 D6: InspectorDock/NodeDock read through a bound-document resolver`

### D7a — Scene DocumentView embeds a per-document scene tree

- **Intent:** the composite scene tab, first slice: tree on the left of the editor surface.
- **Files:** `editor/gui/document_view.{h,cpp}` (scene arm of the ctor, `document_view.cpp:53-62`).
- **Do:** for scene documents, build `HSplitContainer`: left = a new `SceneTreeDock` instance with `bound_document = doc` (collapsible, initial width ~250px * EDSCALE), right = the editor surface. Tree binds doc root + doc selection (D4 resolvers make this automatic). Guard: the dock instance must tolerate being created before the doc's scene finishes loading (bind on `NOTIFICATION_READY` + doc-root-changed refresh).
- **Keep green:** the legacy main screen and global docks are untouched; only pane-hosted scene tabs gain the tree.
- **Verify:** build + smoke; manual: two panes, two scenes — each tree shows its own scene; selecting in pane A's tree does not disturb pane B's tree; selecting in the ACTIVE doc's tree drives gizmos (proxy).
- **Commit:** `G2 D7a: scene DocumentView embeds a per-document scene tree`

### D7b — Composite completes: per-document inspector + node dock

- **Intent:** the scene tab becomes self-sufficient: tree | surface | (Inspector ‖ Node).
- **Files:** `editor/gui/document_view.{h,cpp}`.
- **Do:** right side of a second `HSplitContainer`: a `TabContainer` with an `InspectorDock` instance (bound) and a `NodeDock` instance (bound), initial width ~350px * EDSCALE. Selection flow: the doc selection's `selection_changed` → the bound inspector edits the selection front (mirror the global scene-dock→inspector push path, but sourced from the doc selection).
- **Keep green:** global docks still exist and mirror the active doc — redundancy is expected until D8.
- **Verify:** build + smoke; manual: two visible scene tabs, select different nodes in each — each inspector shows its own selection **simultaneously** (this is THE Model B acceptance test); edit a property in the inactive doc's inspector → undo/redo lands in that doc's history.
- **Commit:** `G2 D7b: scene DocumentView embeds per-document inspector + node dock`

### D8 — Retire the global Scene/Inspector/Node docks; accessors route to the active document

- **Intent:** end the redundancy: the per-tab docks are the only scene-context docks; FileSystem/Import/History stay global.
- **Files:** `editor/editor_node.cpp` (dock registration block in the ctor — locate the `EditorDockManager`/dock-slot setup; also `push_item`/`edit_item`/`push_node_item`); `editor/docks/inspector_dock.{h,cpp}` (`get_singleton`/`get_inspector_singleton`); `editor/docks/scene_tree_dock.{h,cpp}` (`get_singleton`).
- **Do:**
  - Remove Scene/Inspector/Node from the dock-manager registration; keep one **hidden fallback instance** of each (unbound, parented under EditorNode, never shown) so no accessor ever returns null.
  - `InspectorDock::get_singleton()` / `get_inspector_singleton()` / `SceneTreeDock::get_singleton()` become routing accessors: active doc's bound instance if one exists (resolve via the doc's composite DocumentView; add an `EditorNode`-side registry mapping `SceneDocument* → its dock instances`, populated by DocumentView ctor/dtor), else the hidden fallback. Tag `// G2 D8 routing shim`.
  - `EditorNode::push_item` family routes to the active doc's inspector instance.
  - Check the feature-profile dock gating and layout save/restore for the removed docks (grep the dock names in `editor_node.cpp` / dock manager) and neutralize gracefully (absent docks are skipped, not errors).
- **Keep green:** the hidden fallbacks guarantee the ~dozens of `get_singleton()` consumers stay non-null even with zero scenes open.
- **Verify:** build + smoke (all scenarios — restore-3-scenes especially); manual: fresh project open, empty project (no scene), create-root-node flow, inspect from FileSystem (resource → active doc inspector), signals dialog, quick-open-scene.
- **Commit:** `G2 D8: retire global scene-tree/inspector/node docks; singleton accessors route to the active document`
- Run `/simplify` over the milestone-D diff.

---

## Milestone 5 — Singleton screens as tabs; remaining select() rewires (D13, D12)

**Goal:** Game/AssetLib/third-party main screens live in one screen-host tab; play/stop uses a guarded focus handle; distraction-free/expand-viewport follow pane focus.

### M5.1 — Screen-host tab polish (core landed early at S5.5)

- **Intent:** seam #5's core (TYPE_SCREEN_HOST, DocumentView reparent arm, root-as-tab-host ctor, `focus_editor` → `reveal(screen_host_doc)`) **already landed at S5.5** — do NOT re-implement. This step is the remaining polish + the singleton-screen verification sweep.
- **Files:** `editor/editor_main_screen.cpp` (`_select_index` — retitle hook); `editor/gui/tabbed_document_host.{h,cpp}` (tab retitle API, close/re-summon path).
- **Do:**
  - Tab title follows the selected screen's plugin name (`_select_index` notifies the hosting tab; add a `TabbedDocumentHost::set_document_title(EditorDocument*, String)` or refresh hook).
  - Closing the screen-host tab parks `main_screen_vbox` in the compat holder (S5.5 dtor path); a strip button press re-summons it via `focus_editor` → `reveal`. Verify the round-trip is leak- and crash-free.
- **Keep green:** third-party plugins parenting into `get_editor_main_screen()->get_control()` keep working wherever the vbox lives; `select()` shims land in the same place.
- **Verify:** build + smoke; manual: press AssetLib strip button → screen-host tab focuses showing AssetLib; switch to Game → same tab retitles; close the tab → strip button re-summons it.
- **Commit:** `G2 M5.1: screen-host tab retitles per screen; close/re-summon round-trip`

### M5.2 — Play-mode focus handle (guarded restore)

- **Intent:** reframe `screen_index_before_start` as a saved focus-target handle.
- **Files:** `editor/plugins/game_view_plugin.cpp` (record at `:558`, restore at `:597-602`, embed re-focus at `:621` — locate by the member name if drifted).
- **Do:** replace the int with `struct { ObjectID pane_id; String doc_path; bool was_screen; StringName screen_name; }` captured before focusing Game (from `EditorWorkspace::get_focused_pane()` + its active tab / current screen). Restore on stop: if the pane ObjectID resolves and (doc still open / screen still registered) → `reveal`/`focus_editor` accordingly; otherwise no-op (never crash into a deleted pane). Embed re-focus (`:621`) → `focus_editor("Game")`.
- **Keep green:** stock flow (play from a scene, stop) restores exactly as before.
- **Verify:** build + smoke; manual: focus a script tab → play (Game tab focuses) → stop → script tab refocused; play → close the originating pane → stop → no error, focus stays on Game.
- **Commit:** `G2 M5.2: play-mode focus restore via guarded workspace focus handle`

### M5.3 — Distraction-free follows pane focus; expand-viewport maximizes the focused pane

- **Intent:** rewire the one coupling (D12).
- **Files:** `editor/editor_main_screen.cpp:208` (the `update_distraction_free_mode()` call inside `_select_index`); `editor/editor_node.cpp` (`update_distraction_free_mode` impl + the expand/distraction actions — grep `distraction_free` and the expanded-viewport toggle); `editor/gui/editor_workspace.{h,cpp}`.
- **Do:**
  - `EditorNode` connects `EditorWorkspace::focused_pane_changed` → `update_distraction_free_mode()` (keep the `_select_index` call too; screens still exist inside the host tab).
  - `EditorWorkspace::set_pane_maximized(WorkspacePane *p_pane, bool)`: walk ancestors hiding the sibling subtree of each split (store/restore hidden set + split offsets); `is_pane_maximized()`.
  - The expand-viewport action calls `set_pane_maximized(get_focused_pane(), toggle)` in addition to its existing dock-collapse behavior.
- **Keep green:** with a single root pane, maximize is a no-op; distraction-free unchanged.
- **Verify:** build + smoke; manual: 2×2 split, expand-viewport shortcut on a pane → it fills the workspace; toggle back → layout restored (offsets intact).
- **Commit:** `G2 M5.3: distraction-free follows pane focus; expand-viewport maximizes the focused pane`

### M5.4 — Sweep the remaining editor-tree select() callers

- **Intent:** inside `editor/`, no caller names a screen index anymore.
- **Files:** grep `->select(EditorMainScreen::` and `->select_by_name(` under `editor/` (excluding `editor_main_screen.cpp` internals). Known remainders include `editor/editor_node.cpp` NOTIFICATION_READY-adjacent paths and any 2D/3D switchers. **Do not touch** `platform/**`, `modules/mono/**`, `modules/gridmap/**`.
- **Do:** each site → `focus_editor(<name>)` or `reveal(<doc>, <kind>)` per its intent (2D/3D switch intents on the active scene → `reveal(active_doc, SCENE_2D/3D)`). `select_next/select_prev` (Ctrl+F1.. cycling) keep working over the surviving table — leave them, note as v1-acceptable.
- **Keep green:** shims still back every untouched caller.
- **Verify:** build + smoke; manual: keyboard screen-cycling shortcuts still do something sane.
- **Commit:** `G2 M5.4: migrate remaining editor-tree main-screen select() callers to the intent API`

---

## Milestone 6 — Persistence (D9, D10) — THE one-way door

**Goal:** versioned `[Workspace]` split-tree geometry in the editor layout; open-doc set + per-pane tabs + per-view transforms in the per-project session store; legacy migration. **Schema is versioned from the very first write; unknown version ⇒ ignore + default layout, never partial-parse.**

### M6.1 — Stable pane ids + geometry (de)serialization

- **Intent:** the workspace tree can round-trip itself.
- **Files:** `editor/gui/editor_workspace.{h,cpp}`.
- **Add:** `uint32_t pane_id` on `WorkspacePane` (assigned by `EditorWorkspace::make_pane` from a persisted `next_pane_id` counter; never reused within a session file); `Dictionary EditorWorkspace::save_geometry() const` / `bool EditorWorkspace::load_geometry(const Dictionary&)`. Schema v1 (recursive node): `{ "v": 1 (root only), "t": "split"|"leaf", "id": int (leaf), "vert": bool, "off": int (SplitContainer split_offset), "a": <node>, "b": <node> }`. `load_geometry` rebuilds leaves as empty `TabbedDocumentHost` panes (content assignment comes from the session, M6.3) and returns false on ANY structural/version anomaly (caller falls back to default single pane; log one WARN).
- **Keep green:** nothing calls it yet.
- **Verify:** build + smoke; unit-style manual probe (temporary, removed): save→load→save produces identical Dictionaries for a 3-split layout.
- **Commit:** `G2 M6.1: workspace split-tree geometry serialization (schema v1, stable pane ids)`

### M6.2 — Wire geometry into the layout orchestrator + legacy migration

- **Intent:** geometry persists through `EditorNode::_save/_load_editor_layout`; legacy configs seed the default pane.
- **Files:** `editor/editor_main_screen.cpp` (`save_layout_to_config` `:85-98`, `load_layout_from_config` `:100-105`); `editor/editor_node.cpp` (orchestrator `:6371-6397` — verify EditorMainScreen fan-out is already present, extend only if the workspace needs a separate hook).
- **Do:** `save_layout_to_config` additionally writes `p_config_file->set_value(p_section, "workspace", workspace->save_geometry())` **and keeps writing** `selected_main_editor_idx` (legacy parallel until M7). `load_layout_from_config`: if `workspace` present and `load_geometry` succeeds → done; else legacy path (existing deferred `select(idx)` — the D10 migration shim: old configs behave exactly as today, seeding the single default pane).
- **Keep green:** old config files load identically; new key ignored by old builds (ConfigFile tolerates unknown keys).
- **Verify:** build + smoke; manual: split 3 panes, quit, relaunch → geometry restored (empty tab hosts OK at this step); delete the `workspace` key from the layout cfg → clean legacy startup.
- **Commit:** `G2 M6.2: workspace geometry persists via the editor-layout orchestrator (legacy fallback intact)`

### M6.3a — Session store: document set + per-pane tab assignment

- **Intent:** which documents are open, and in which pane's tabs, survives restart.
- **Files:** new `editor/gui/editor_workspace_session.{h,cpp}` (register in the editor SCsub automatically via glob — verify `editor/gui` builds by glob; if not, add to SCsub); `editor/editor_node.cpp` (save/restore hook points in `_save_editor_layout`/`_load_editor_layout` + after-project-scenes-loaded deferred restore); `editor/gui/tabbed_document_host.h` (`documents` `:57`, `current` `:59` — add read accessors).
- **Do:** write `res://.godot/editor/workspace_session.cfg` (seam #9), schema v1: `[session] version=1`; `[documents] list = [{path, kind}]` (scene paths, script paths, `help://Class`); `[panes] p<id> = { tabs: [doc indices], current: int }`. Restore (deferred one frame after scene restore completes): resolve docs — scenes match already-restored `EditedScene`s by path (do NOT re-open scenes; the existing scene-restore owns that), scripts/help via `get_or_create_*`; missing files skipped silently; unknown pane_id → tabs appended to the first tabbed leaf; version mismatch → skip session entirely.
- **Keep green:** absent session file ⇒ today's behavior; scene-restore path untouched.
- **Verify:** build + smoke (restore-3-scenes scenario must stay green); manual: layout with scene + script + help tabs across 2 panes → restart → identical tab sets and current tabs.
- **Commit:** `G2 M6.3a: per-project workspace session — document set + per-pane tab assignment (schema v1)`

### M6.3b — Per-view transforms in `EditorDocumentView::editor_states`

- **Intent:** camera/canvas transforms per (pane, document) survive restart.
- **Files:** `editor/editor_document.h:145` (`editor_states`); `editor/gui/document_view.{h,cpp}`; `editor/scene/canvas_item_editor_plugin.{h,cpp}` (`CanvasItemEditorView` — expose `get_view_state()/set_view_state(Dictionary)` covering `view_offset`/`zoom`); `editor/scene/3d/node_3d_editor_plugin.{h,cpp}` (`Node3DEditorView` — same for camera transform/ortho/fov per viewport, reusing the existing per-viewport state get/set machinery where present); `editor_workspace_session.cpp` (`[views] p<id>/<doc path> = states-dict`).
- **Do:** `DocumentView::capture_view_state()` (surface → `doc_view->get_editor_states()`) called at session-save; `apply_view_state()` after the surface binds on restore. Script views: store caret line/col + scroll.
- **Keep green:** absent state ⇒ default framing (today's behavior).
- **Verify:** build + smoke; manual: pan/zoom a 2D tab + orbit a 3D tab in two panes → restart → both framings restored per pane.
- **Commit:** `G2 M6.3b: per-view camera/canvas transforms persist on EditorDocumentView::editor_states`
- Run `/simplify`; **update DIVERGENCE-LEDGER** with the persistence rows (schema documented in the ledger note).

---

## Milestone 7 — Enum shrink + retire the legacy strip (D14, D2 finalize)

**Goal:** scenes open as workspace tabs by default; 2D/3D/Script leave the strip; `EditorTable` shrinks to Game + AssetLib; scaffolding removed.

### M7.1 — Scenes open as workspace tabs by default

- **Intent:** the real switchover: opening/creating a scene reveals a composite scene tab.
- **Files:** `editor/editor_node.cpp` (the scene-open path around `register_document_context` — locate by name; hook after the document is registered and typed); `editor/editor_main_screen.cpp` (`reveal` scene arm drops the M1.3 legacy fallback and uses the S5 tab path).
- **Do:** on document registration (open, new, and restore-on-startup — restore coexists with M6.3a session assignment: session wins when it names the doc; plain registration reveals into the target pane otherwise), call `reveal(scene_doc)`. The stock top scene-tab strip (`EditorSceneTabs`) **stays** as a global doc switcher for v1 — its clicks activate the doc; add a `reveal` on click so activation also focuses the tab's pane. Note its eventual removal as out-of-scope.
- **Keep green:** legacy strip 2D/3D buttons still work (they focus the active doc's view kind via M5.4 rewire); the legacy main 2D/3D stack still exists inside the screen-host until M7.2.
- **Verify:** build + smoke; manual: open project → restored scenes appear as tabs; New Scene → tab; double-click scene in FileSystem → tab (or focuses existing).
- **Commit:** `G2 M7.1: scenes open as workspace tabs by default (composite scene view)`

### M7.2a — Shared 2D/3D toolbars follow the focused scene pane

- **Intent:** editing chrome survives the strip's death: the singleton toolbars mount into the focused scene tab.
- **Files:** `editor/scene/canvas_item_editor_plugin.{h,cpp}` + `editor/scene/3d/node_3d_editor_plugin.{h,cpp}` (identify the main toolbar HBox each singleton owns and expose `Control *get_shared_toolbar()`); `editor/gui/document_view.{h,cpp}` (header slot, seam #8).
- **Do:** scene DocumentViews get a header slot; on focused-pane change / active-tab change, the matching singleton toolbar (2D toolbar for a 2D surface, 3D for 3D) reparents into the focused scene tab's header; parked hidden under its singleton otherwise. Tool-mode/snap state stays on the services (D8) — the toolbar is one Control moving, its handlers untouched.
- **Keep green:** the legacy main screen still shows its toolbar when the screen-host tab is focused and no scene pane is (park logic must handle that case: toolbar returns to its stock position inside the legacy stack).
- **Verify:** build + smoke; manual: focus 3D tab → 3D toolbar present, tool shortcuts (Q/W/E/R) act on that doc; focus 2D tab → 2D toolbar swaps in; snap dialog opens from either.
- **Commit:** `G2 M7.2a: shared 2D/3D toolbars follow the focused scene pane`

### M7.2b — Remove 2D/3D/Script from the strip; root pane defaults to a tab host

- **Intent:** the strip carries only singleton screens; the workspace is the primary surface.
- **Files:** `editor/editor_node.cpp` (the `add_main_plugin`/plugin-registration block — grep `add_main_plugin`; register CanvasItemEditorPlugin/Node3DEditorPlugin/ScriptEditorPlugin WITHOUT a strip entry: split `add_main_plugin` into strip registration vs plugin bookkeeping, or simply skip the strip add for these three and keep them as ordinary plugins); `editor/editor_main_screen.cpp` (`_notification(NOTIFICATION_READY)` default-select logic `:43-64` → default = focus the root tab host; ctor already hosts a `TabbedDocumentHost` since S5.5 — this step only changes whether the screen-host tab exists by default when no singleton screen is summoned); `editor/editor_node.cpp:7841-7861` (feature-profile gating: 2D/3D/Script profile flags now gate view-kind minting — 3D disabled ⇒ `DocumentView` never mints a 3D surface (falls back to 2D), Script disabled ⇒ `ScriptEditor::edit` external/no-op path — keep the existing profile checks' observable messages).
- **Keep green:** `focus_editor("2D"/"3D")` now maps to `reveal(active_doc, kind)`; `focus_editor("Script")` = S6b behavior; `select(EDITOR_2D/3D/SCRIPT)` (mono/macos, un-edited) maps through the same — implement the mapping INSIDE `select(int)`/`focus_editor` shims, tagged `// G2 D14`.
- **Verify:** build + smoke (all scenarios); manual: fresh project boots to an empty tab host + docks; strip shows only Game/AssetLib; mono-style `select(EDITOR_SCRIPT)` path (invoke via `EditorInterface.set_main_screen_editor("Script")`) still lands on scripts.
- **Commit:** `G2 M7.2b: retire 2D/3D/Script from the main-screen strip; workspace tab host is the default surface`

### M7.3 — EditorTable shrink (type-split identity)

- **Intent:** the enum tells the truth; un-editable callers keep compiling.
- **Files:** `editor/editor_main_screen.{h,cpp}` (`EditorTable` `editor_main_screen.h:46-52`).
- **Do:** keep the enumerators `EDITOR_2D/EDITOR_3D/EDITOR_SCRIPT` as `[[deprecated]]`-commented legacy aliases (`// G2 D14: view-kinds now; kept only so platform/mono callers compile — select() maps them`), reorder nothing; `editor_table`/`buttons` now only ever contain Game/AssetLib (+ third-party); internal index-based logic (`set_button_enabled`, `can_auto_switch_screens` `:240-257`, `remove_main_plugin` `:293`) re-audited against the shrunken table (in particular `set_button_enabled(EDITOR_2D...)` callers — grep and reroute to the profile gating from M7.2b; `can_auto_switch_screens`'s Script-button scan now returns true when no Script button exists — preserve its intent: auto-switch allowed unless a singleton screen right of the old Script position is active; simplify to "selected screen is null or Game/AssetLib ⇒ policy by name" with a `// G2 D14` note).
- **Keep green:** shims map legacy indices by name; nothing outside `editor/` edited.
- **Verify:** build + smoke; manual: feature profile disabling 3D still yields a working 2D-only editor.
- **Commit:** `G2 M7.3: EditorTable shrinks to singleton screens; legacy indices map through the intent shims`

### M7.4 — Scaffolding removal + final cleanup

- **Intent:** delete the debug rig; the ledger and docs reflect the end state.
- **Files:** `editor/gui/editor_workspace.{h,cpp}` (`_debug_split_focused` `:109`, `_debug_split_focused_with_tabs` `:115`, `debug_pane_counter` `:99`, the `unhandled_key_input` trigger `:104`); `workspace-editor-planning/DIVERGENCE-LEDGER.md`; sweep for stray temp probes/prints.
- **Do:** remove the debug split methods + key hooks (S8's context menu is the real affordance); final ledger pass re-verified against `git diff --name-status master...HEAD`; run `/simplify` across the milestone-7 diff; extend `workspace-editor-planning/smoke/run_smoke.sh` fixtures/assertions if M7 changed any output the harness keys on (harness edits are non-engine, safe).
- **Verify:** build + smoke; full manual regression sweep (the union of all milestone manual checks, one pass).
- **Commit:** `G2 M7.4: remove workspace debug scaffolding; G2 main-screen replacement complete`

---

## Risk register / one-way doors

1. **D9 persistence schema (THE one-way door).** Once a `[Workspace]` geometry or session file is written to a user project, every future build must read it or deliberately ignore it. **Mitigation (mandatory, built into M6):** `"v": 1` / `version=1` written from the FIRST write; loaders hard-fail-to-default on any unknown version or structural anomaly (never partial-apply); the legacy `selected_main_editor_idx` keeps being written in parallel until M7 so a rollback build still restores sanely; session and geometry are separate stores so corrupting one never poisons the other.
2. **Model B selection proxy (highest behavioral risk).** Failure modes: a consumer calls a method that wasn't virtualized (silently operates on the proxy's empty base storage); double/missed `selection_changed` emissions during retarget; ordering during scene switch (proxy retargets before/after consumers observe the switch). **Mitigation:** D1 lands the per-doc mirror while the global stays authoritative (one green step of soak); D2 begins by *enumerating* the consumer method surface via grep before virtualizing; a debug-only consistency assert during development; retarget emits exactly one `selection_changed` after reconnecting the relay; the D2 manual verification list is the gate — do not proceed to D3 with any anomaly.
3. **ScriptEditor long tail.** `ScriptEditor` is ~4k lines with dozens of `tab_container`/`script_list` touchpoints. **Mitigation:** the S1 grep-disposition checklist is produced up front and re-run at S7; `_get_current_editor()` keeps a legacy fallback until S7; the 45 `get_singleton()` callers are never edited — only the internals behind them.
4. **`make_visible` regression surface.** ~116 tool-plugin sites must not be touched; the danger is indirect (a services singleton hiding state a pane view renders through). **Mitigation:** M2.1 audit + the standing manual regression (strip-cycle with live panes) repeated in every milestone's verification.
5. **Reparent-follows-focus chrome (S7, M7.2a).** Reparenting themed Controls can drop theme lookups or focus. **Mitigation:** one hidden holder per singleton, reparent via `reparent()` (keeps ownership), re-apply size flags on mount, and the park-to-stock fallback path so the legacy screen never loses its toolbar while it exists.
6. **Hidden `main_screen_vbox` (new with S5.5).** Stock never fully hides the main-screen stack; once it lives in tab 0, selecting a script tab hides the whole vbox. Per-view rendering is safe (D3 audit: SubViewports are UPDATE_WHEN_VISIBLE — stopping is the point), but any singleton code gating on `is_visible_in_tree()` of a main screen could behave differently while a script tab is current. **Mitigation:** the M2.1 strip-cycle regression is extended: with a script tab current, verify play/stop, docks, bottom panel, and re-focusing the Editor tab all behave; anomalies get scoped fixes tagged `// G2 S5.5`.
7. **Un-editable callers (platform/mono/gridmap).** They compile against `EditorMainScreen::select` and the `EditorTable` enumerators. **Mitigation:** shims are permanent (D1/D11); M7.3 keeps legacy enumerators as aliases; a grep for `EDITOR_SCRIPT|select(` under `platform/ modules/mono modules/gridmap` is part of M7.3 verification (expect zero edits, all compiling).

## Definition of done — per milestone

**M1:** ☐ pane focus ring follows mouse-down/focus-in ☐ 3D + 2D views promote their doc on click ☐ `reveal`/`focus_editor` exist; `select`/`select_by_name` are shims ☐ 12 EDITOR_SCRIPT sites migrated; platform/mono untouched ☐ build + smoke green at every commit ☐ ledger updated (editor_main_screen, node_3d_editor rows).

**M2:** ☐ D3 contract documented at `EditorPlugin::make_visible` ☐ strip-cycle with two live panes shows zero render/interaction regressions ☐ no tool-plugin `make_visible` site touched.

**S:** ☑ root pane is a tab host from startup; legacy screens live in the screen-host tab (S5.5); `get_control()` returns the live `main_screen_vbox` at every instant ☑ first script open is a tab in the CURRENT pane — no split ☑ scripts and class docs open as workspace tabs via `reveal` ☑ debugger goto / attach-script / find-in-files / save-all / history all workspace-native ☑ `script_list` and internal tab hosting deleted ☑ shared script chrome follows the focused script tab ☑ members-overview deferral recorded ☑ pane split/close UX exists (S8, `415c93dfeb`) ☑ 45 `ScriptEditor::get_singleton()` callers un-edited and working ☑ smoke green; `/simplify` run; ledger row for `script_editor_plugin` added. *(Owner interactive verification pass still owed — batched per 2026-07-04 decision.)*

**D:** ☐ every `SceneDocument` owns live selection + history ☐ global accessor is the retargeting proxy; 48 sites / 5 cached consumers un-edited ☐ two visible scene tabs show two live selections + inspectors **simultaneously** ☐ scene tabs embed tree + inspector + node dock ☐ global Scene/Inspector/Node docks removed; routing shims non-null in all states (incl. zero scenes) ☐ FileSystem/Import/History still global ☐ undo/redo lands in the correct per-doc history ☐ smoke (incl. restore-3-scenes) green; `/simplify`; ledger rows (editor_data, editor_node, docks).

**M5:** ☐ Game/AssetLib live in the screen-host tab; `get_editor_main_screen()->get_control()` returns `main_screen_vbox` byte-identically ☐ play/stop focus round-trip incl. deleted-pane guard ☐ distraction-free reacts to pane focus; expand-viewport maximizes the focused pane ☐ zero `select(EditorMainScreen::` callers left under `editor/` (except shim internals).

**M6:** ☐ geometry schema v1 round-trips; unknown version ⇒ default layout ☐ legacy configs (no `workspace` key) behave exactly as before ☐ doc-set + per-pane tabs + per-view transforms restore across restart ☐ missing-file / dead-pane-id / version-mismatch degrade silently ☐ smoke restore scenario green ☐ schema documented in the ledger.

**M7:** ☐ scenes open as composite tabs by default ☐ 2D/3D toolbars follow focus ☐ strip shows only Game/AssetLib ☐ `EditorTable` shrunk with compiling legacy aliases; platform/mono/gridmap un-edited ☐ feature-profile gating works in the new model ☐ debug scaffolding deleted ☐ full manual regression pass ☐ final ledger reconciliation against `git diff --name-status master...HEAD`.

## Explicitly out of scope (do not build, do not "fix in passing")

- **D8b** per-view tool/snap state (tool mode/snap stay on the services singletons).
- **D12b** per-pane bottom panels (bottom panel stays global).
- Floating panes (WindowWrapper workspaces).
- Full simultaneous multi-pane EDITING (two panes editing 2D and 3D at once) — render/navigate-many + edit-active is the banked milestone; the no-duplicate-tab rule in seam #3 is part of this deferral.
- Members-overview / ScriptMethodsDock (G3), per-pane contextual dock layout beyond the fixed tree|surface|inspector composite (G3), bottom drawer (G4), load-time work (G5).
- Removing `EditorSceneTabs` (the top scene-tab strip) — kept as a global switcher in v1.
- Hover-to-navigate input affordance.
- The opt-in "GDStudio reset layout" template (D10) — only the hook point (default layout builder) exists after M6; the template itself is future work.
