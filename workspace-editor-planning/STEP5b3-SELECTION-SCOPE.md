# Step ⑤b.3 + #6 scope: per-pane 2D editing (selection + manipulation)

Status: **scoped, not started** (2026-07-01). This is the last and largest slice
of the 2D services/view split — the point where a 2D pane becomes *editable*
(click-select, move/rotate/scale/anchors), not just viewable. Grounded in two
code-mapping passes (file:line throughout).

Read alongside [STEP5-CANVASVIEW2D-SCOPE.md](./STEP5-CANVASVIEW2D-SCOPE.md) and
[ARCHITECTURE.md](./ARCHITECTURE.md).

---

## What the mapping found

### The manipulation code is big and member-coupled
`CanvasItemEditor::_gui_input_viewport` (`canvas_item_editor_plugin.cpp:2748`) is
an ordered chain of `_gui_input_*` sub-handlers (rulers/guides → plugins →
double-click → scale → pivot → resize → rotate → move → anchors → ruler-tool →
select), gated by a single `drag_type` state machine (`.h:156-186`). The whole
input block + `_commit_drag` is **~1,790 lines** (`:1186`→`:2977`); the selection
overlay (`_draw_selection` + `_draw_control_anchors`/`_helpers`) is another
**~450 lines** (`:3550`→`:4006`). Manipulation mutates nodes live during drag and
commits via `EditorUndoRedoManager` (`_commit_canvas_item_state`, `:930`).

### Three coupling points (what makes it single-pane today)
1. **The member view transform** — `transform`/`zoom`/`view_offset` (`.h:221,233,234`),
   used by every hit-test and every overlay draw, and pushed onto the shared
   scene via `get_scene_root()->set_global_canvas_transform(transform)`
   (`:4273`). *Our CanvasView2D already owns its own transform on its own
   viewport, so this is the piece we've been building.*
2. **The single edited-scene accessor** — `EditorNode::get_singleton()->get_edited_scene()`
   at hit-test (`:694,743`), select (`:2445,2528,2606`), and draw (`:4278…`).
3. **The global selection object** — member `editor_selection`, bound once
   (`:5614`) to `EditorNode::get_editor_selection()`; read/written throughout
   select/manipulate/draw, with per-node editor data
   (`CanvasItemEditorSelectedItem`) attached to it.

### #6 (per-document selection): Model A already effectively works
`EditorSelection` is one global object (`editor_node.h:274`), consumed by ~17
files / 59 sites. Crucially, **per-scene selection snapshot/restore already
exists**: `EditorData::save/restore_edited_scene_state` (`editor_data.cpp:983-1009`)
serialize the selection into the `EditedScene` struct, and
`_set_current_scene_nocheck` (`editor_node.cpp:4767`) saves→clears→restores on
every scene switch. Since our tab selection already drives the active edited
scene (④ commit fba087e49a), **the global selection already follows the active
document** — that is Model A, keyed on the active document. The reserved
`SceneDocument::selection` (`editor_document.h:114`) is still dead scaffolding.

**Model B** (each document owns a *live* `EditorSelection`, `get_editor_selection()`
returns the active doc's) is blocked by ~5 *cache-once-and-connect* consumers
(scene dock ctor injection `editor_node.cpp:9175`, scene-tree editor
`scene_tree_editor.cpp:1770`, 3D editor, 2D editor, control toolbar) that grab
the pointer once and connect to that object's `selection_changed`. Redirecting
the getter is trivial; keeping those cached pointers/connections valid across a
document switch is the work. ~10 fetch-fresh consumers would follow for free.

---

## The strategic finding

Fully independent per-pane 2D *editing* (two visible panes, each with its own
live selection and manipulation) is a **large extraction (~2,200 lines)** AND it
is the *same problem 3D has not fully solved either* — the 3D DocumentView panes
render and show gizmos but editing still drives the global selection through the
singleton. So this isn't a 2D-only gap; it's the general "per-pane editing"
frontier, and v1 deliberately deferred it (INDEX decision 2: "single live editing
pane; rebind-on-activate").

**The consistent v1 model is "view-many, edit-active":** every pane *renders* its
document (done for 2D: ⑤a/b.1/b.2), and the *active* pane edits. That matches
what tab-activation already does and what 3D does. Full per-pane simultaneous
editing (and Model B selection) is a post-v1 convergence step.

---

## Recommended architecture (mirrors the 3D split)

Give `CanvasItemEditor` a **primary `CanvasView2D` (`main_view`)** that owns the
transform + hosts the display + carries the editing overlay/input — exactly as
`Node3DEditor` owns `main_view` (a `Node3DEditorView`). Then:
- The **primary view edits** (input + overlay + selection), reading SERVICE
  (tool/snap/undo) from the `CanvasItemEditor` singleton and operating on the
  global selection (= active document's, via Model A) and the active scene.
- **Secondary panes render** (the CanvasView2D we already have: display +
  pan/zoom + grid).
- Selection stays Model A (already working); Model B is deferred.

This keeps one copy of the 2,200-line editing brain (moved onto the view, not
duplicated), and converges 2D onto the same shape as 3D.

### Phasing (each a green commit)
- **⑤b.3a** — hit-testing + click-select in CanvasView2D driving the *global*
  selection (reuse the public `find_canvas_items_at_pos`, port
  `_select_click_on_item`'s ~30-line resolver), + draw the selection rect/box.
  Only the active pane; smallest editable slice.
- **⑤b.3b** — move drag (the common case): `_save/_restore/_commit_canvas_item_state`
  via `EditorUndoRedoManager`, snap from the service.
- **⑤b.3c** — rotate/scale/resize/anchor handles + their overlay.
- **⑤b.3d** — fold this into `CanvasItemEditor::main_view` so the singleton's own
  editing routes through the view (the true services/view split), then ⑤c can
  retire the `_display_scene_root` reparent shim.

### #6 decision to make
- **Now:** rely on Model A (already works) — active pane edits the active
  document's selection. No new work.
- **Later (post-v1):** Model B for truly independent per-pane selection, via a
  stable selection *proxy* the docks hold while its backing swaps, so the ~5
  cache+connect consumers don't churn.

---

## The decision for you

⑤b.3 is the largest remaining piece of Step ⑤ and overlaps the general per-pane
editing frontier. Two reasonable paths:

- **(A) Push through ⑤b.3 now** — deliver an editable 2D pane (view-many /
  edit-active), then ⑤c (retire the shim) → main-screen replacement becomes
  possible. Big, multi-commit, but completes the 2D story.
- **(B) Bank the milestone** — 2D + 3D render/navigate in tabbed splittable panes
  today. Move to a higher-leverage goal (G3 per-pane scene-tree+inspector, G4
  bottom drawer, or the main-screen replacement using primary-view editing) and
  return to full per-pane 2D editing at the v1→per-pane convergence.

Recommendation: given the size and that it overlaps an already-deferred frontier,
**(B)** unless editable 2D panes are the immediate priority — the render/navigate
milestone is a clean stopping point, and G3/G4 deliver more visible product per
unit effort.
