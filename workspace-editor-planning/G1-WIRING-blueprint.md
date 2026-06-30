# G1 wiring blueprint (verified against source)

Concrete, line-grounded transformation plan for wiring `EditorDocumentContext`
into `EditorData` / `EditorNode` / the 2D & 3D editors. Every line number below
was confirmed against the tree at commit `59b1ad3f62` on `feature/workspace-editor`.
This supersedes the higher-level steps in `G1-multiple-scenes.md` with exact edits.

## Verified current architecture

- **One global `scene_root` SubViewport** (`editor_node.cpp:8820`), `disable_3d=true`,
  `disable_input=true`, `audio_listener_2d=true`. No own/explicit World3D.
- **Scene switching reparents the scene's root node** in/out of that one SubViewport:
  - `_set_current_scene_nocheck` (`editor_node.cpp:4698`): removes old root from
    `scene_root` (4717-4719), adds new root (4730-4734).
  - `set_edited_scene_root` (`editor_node.cpp:4584`): same remove/add (4588-4602).
  - Inactive scenes' roots are **detached** (no parent) → not live.
- **2D render:** the 2D editor parents the single `scene_root` under its viewport
  container — `canvas_item_editor_plugin.cpp:5654`
  `scene_tree->add_child(EditorNode::get_singleton()->get_scene_root())`. It renders
  `scene_root`'s World2D.
- **3D render:** because stock `scene_root` has no own world, scene 3D nodes'
  `find_world_3d()` resolves up to the **root-window World3D**. Every editor gizmo /
  grid / origin instance + both picks use that one world:
  - scenario: `node_3d_editor_plugin.cpp` 4712, 4721, 4730, 4739, 4751, 4762, 4773
    (transform gizmos), 8327 (origin), 9066 (grid).
  - space-state picks: 5356, 9357 (`get_tree()->get_root()->get_world_3d()->get_direct_space_state()`).
  - per-node selection gizmos already use `sp->get_world_3d()` (7398-7421) →
    **doc-correct for free** once the doc's scene_root owns its world. Verify only.

## Target v1 model

- `EditorData::EditedScene` **owns an `EditorDocumentContext*`** (composition). Each
  doc's scene_root SubViewport carries its **own explicit World3D** (already done in
  the DocumentContext ctor) + its own World2D.
- **All docs live at once:** every doc's scene_root stays parented under a persistent
  (hidden) container in EditorNode, so all keep processing/physics. The scene's root
  node stays under its own doc scene_root permanently — **no reparenting on switch**.
- **Switch = activate/deactivate:** (1) make active doc's scene_root visible, others
  hidden; (2) rebind docks (SceneTreeDock/InspectorDock) to active doc's root +
  selection; (3) bind 3D editor to active doc's world/scenario/space; (4) bind 2D
  editor to active doc's World2D. `get_tree()->set_edited_scene_root()` still points
  at the **focused** doc only.
- `EditorNode::{scene_root, editor_selection, editor_history}` demote to delegating
  accessors → active doc.

## Edit sequence (each step keeps the tree compiling + testable)

### Step A — EditorData owns a DocumentContext per scene
- `editor_data.h:113-126`: add `EditorDocumentContext *document = nullptr;` to
  `EditedScene` (keep existing fields for v1; migrate later).
- `editor_data.cpp:613 add_edited_scene`: `es.document = memnew(EditorDocumentContext);`
  set `history_id`, `time_opened`, push. (Parent its scene_root in EditorNode — see B.)
- `editor_data.cpp:644 remove_scene`: `memdelete(es.document)` (frees SubViewport +
  selection; releases worlds) after the existing root teardown.
- `editor_data.cpp:973 clear_edited_scenes`: free all documents.
- Add accessors: `EditorDocumentContext *get_document(int idx=-1)`,
  `get_active_document()`, `document_count()`.
- **Build + run after A:** documents constructed but unused → editor behaves exactly
  as before (regression-free checkpoint).

### Step B — Parent doc scene_roots; demote EditorNode globals
- `editor_node.cpp:8820`: stop creating the single `scene_root`. Add a persistent
  hidden `Node` (e.g. `documents_root`) under `gui_base`/main as the live home for all
  doc scene_roots. On `add_edited_scene`, `documents_root->add_child(doc->scene_root)`.
- `editor_node.h:462/274/262`: `get_scene_root()` / `get_editor_selection()` /
  `get_editor_selection_history()` (h:860/802/803) delegate to
  `editor_data.get_active_document()` — no cached members.
- `editor_node.cpp:4698 _set_current_scene_nocheck` → `_activate_document(idx)`:
  drop the reparent churn (4717-4719, 4730-4734). Instead: deactivate old doc, activate
  new (visibility), `editor_data.set_edited_scene(idx)`, rebind docks, set
  `get_tree()->set_edited_scene_root(new_root)`, keep state save/restore but route the
  selection/history args through the doc (4705/4744 use the **per-doc** selection).
- `editor_node.cpp:4584 set_edited_scene_root`: drop the auto_add reparent (4588-4602);
  the root is created under its doc's scene_root by the open path instead.
- **Build + run after B:** multi-scene tabs switch via activate/deactivate; 2D may
  still bind only one scene_root (fixed in C). This is the highest-risk step.

### Step C — Bind 2D editor to the active document
- `canvas_item_editor_plugin.cpp:5654`: instead of adding the single scene_root once,
  add/swap the **active** doc's scene_root into `scene_tree` on activation (or bind the
  container viewport's World2D to the active doc). Re-point on switch.
- Audit `get_scene_root()` uses at 4273/4370/4663/5600/6139-6146/6244 — they now hit
  the active doc via the delegating accessor; verify correctness.

### Step D — Bind 3D editor to the active document (one helper)
- Add to `Node3DEditorViewport`: `Ref<World3D> bound_world;` + `bind_world(Ref<World3D>)`.
- Add a single resolver, e.g. `Node3DEditor::_active_scenario()` /`_active_space()` →
  active doc's `get_scenario()` / `get_space()`.
- Replace the 11 root-window scenario sites (4712,4721,4730,4739,4751,4762,4773,8327,9066)
  and the two space-state picks (5356,9357) with the resolver.
- On the viewport: `viewport->set_world_3d(doc world)` where the viewport is set up
  (~6663) and re-point on edit/make_visible. Re-instance gizmo/grid/origin into the new
  scenario on bind (`Node3DEditor::set_active_world`).
- 7398-7421 verify-only.
- **Build + run after D:** the focused 3D pane renders + picks the active doc's world;
  two scenes are independently editable on tab-switch (full simultaneous side-by-side
  3D remains G2).

### Step E — Selection/history/undo/title/session
- `editor_history`/`editor_selection` now per-doc; verify `save/restore_edited_scene_state`
  (`editor_data.cpp`) operate on the doc's selection+history.
- Undo/redo keyed by doc `history_id` (already stable). Title/tabs reflect active doc.
- Session restore instantiates a DocumentContext per saved scene.

## Build invocation (this machine)

`py -m SCons platform=windows target=editor redirect_build_objects=yes winrt=no -j24`

- `redirect_build_objects=yes` — matches the existing `bin/obj/` layout (incremental).
- `winrt=no` — **required workaround:** VS 18's toolchain rejects cppwinrt's
  `<experimental/coroutine>` (`STL1011`) in `tts_driver_onecore.cpp`/`tts_windows.cpp`.
  Drops only OneCore TTS (SAPI TTS unaffected). The permanent fix (restore VS17 toolset,
  or patch the coroutine define) is a separate decision — see status notes.
- Single-TU check (fast): build just the target `.obj`, e.g.
  `py -m SCons ... bin/obj/editor/editor_document_context.windows.editor.x86_64.obj`.
