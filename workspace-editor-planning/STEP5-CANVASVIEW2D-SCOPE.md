# Step ⑤ scope: CanvasView2D (per-pane 2D)

Status: **scoped, not started** (2026-07-01). This is the design for the per-pane
2D view — the 2D parallel of `Node3DEditorView`. It is the critical path that
unblocks **panes replacing the stock 2D|3D|Script main screen** (a `DocumentView`
is 3D-only today, so a 2D scene shows an empty 3D viewport).

Grounded in two code-mapping passes (file:line below). Read alongside
[ARCHITECTURE.md](./ARCHITECTURE.md) (service / view / document taxonomy).

---

## The load-bearing finding (why this is feasible)

2D CanvasItems render into a **World2D canvas RID**, not into "the viewport they
are children of." A `World2D` owns one `canvas` RID and a *set* of viewports
(`scene/resources/world_2d.h:46,54,71-74`); each viewport attaches that canvas
(`viewport.cpp:602-604,1350-1353`) with its **own** per-viewport
`global_canvas_transform` (`viewport.cpp:1266,1277`). CanvasItems attach to the
canvas of their ancestor viewport's World2D (`canvas_item.cpp:243-294`, esp.
`:275,278`).

**Therefore** a per-pane `CanvasView2D` can own its **own** `SubViewport`, call
`set_world_2d(document->get_world_2d())` and set its own pan/zoom transform, and
render the exact same 2D scene as any other view of that document — **without
reparenting `scene_root`**. This mirrors 3D precisely: 3D shares a `World3D`
scenario via `viewport->set_world_3d()` (`node_3d_editor_plugin.cpp:4808`); 2D
shares a `World2D` canvas via `viewport->set_world_2d()`. Each `EditorDocument`
already exposes `get_world_2d()` (`editor_document.h`, captured at
`editor_document.cpp:36`).

This is what the current v1 shim **cannot** do: `_display_scene_root`
(`editor_node.cpp:4603-4632`) *reparents* the single active `scene_root`
SubViewport into the one shared `scene_view_container`, evicting all others — a
`Node` has one parent, and `SubViewportContainer` only draws its own SubViewport
children (`subviewport_container.cpp:135-148`), so exactly one document's 2D can
be mounted at a time.

## What's NOT free (the real work)

The fork's 2D display is a `SubViewport` (rendering) + a transparent overlay
`Control` named `viewport` drawn on top (`canvas_item_editor_plugin.cpp:5655-5732`).
Rendering is share-able (above). But the **overlay + pan/zoom + input** are
singletons on `CanvasItemEditor` (a hard singleton, ~100 `get_singleton()` refs):

- Pan/zoom is a plain `Transform2D transform` from `zoom` + `view_offset`
  (`.h:221,233,234`), rebuilt each frame in `_draw_viewport` (`:4268-4273`) and
  pushed via `get_scene_root()->set_global_canvas_transform(transform)` (`:4273`).
  **No Camera2D exists** (0 occurrences) — good, we replicate the transform per view.
- The editing overlay is `_draw_viewport` (`:4268-4300`) + `_draw_grid`,
  `_draw_rulers`, `_draw_guides`, `_draw_selection` (boxes/handles/anchors),
  `_draw_axis`, `_draw_locks_and_groups`, `_draw_smart_snapping`, `_draw_hover`,
  `_draw_message`, plus plugin `forward_canvas_draw_over_viewport`. All drawn onto
  the single `viewport` Control using the single `transform`.
- Input is `_gui_input_viewport` on that same `viewport` Control (`:5732`).

## Member classification (from the mapping pass)

- **SERVICE (stays on the `CanvasItemEditor` singleton):** `tool`; snap config
  (`snap_*`, `smart_snap_active`, `grid_snap_active`, `snap_rotation_step`,
  `use_local_space`); grid config (`grid_offset/step/primary_grid_step`); guides
  (persisted on scene meta); key-insert toggles; colors. Editing *policy*,
  pane-independent.
- **VIEW state (becomes per-pane `CanvasView2D`):** `transform`, `zoom`,
  `view_offset`; the display Controls `viewport`, `viewport_scrollable`,
  `scene_view_container`; `h_scroll`/`v_scroll`; `zoom_widget`; per-view
  visibility toggles (`show_rulers/guides/grid/origin/...`); the entire
  `_draw_viewport` overlay path + `_gui_input_viewport`.
- **Active-scene coupling (thread explicitly, don't read the global):**
  `get_edited_scene()/_root()` and `get_scene_root()` reads inside the draw path.

---

## Phased plan (mirrors the Node3DEditor split playbook)

### ⑤a — RENDER-only `CanvasView2D` (tractable; the DocumentView 2D surface)
A new `editor/gui/canvas_view_2d.{h,cpp}` (or under editor/scene/2d): a Control
owning its own `SubViewport` bound to `document->get_world_2d()`, with its own
`global_canvas_transform` for pan/zoom (replicating the `zoom`/`view_offset` →
`Transform2D` math, no Camera2D). It renders the document's 2D scene per-pane,
with pan/zoom, but **no editing overlay yet** (view/navigate, not manipulate).

`DocumentView` becomes type-aware: `TYPE_SCENE_2D` → hosts a `CanvasView2D`;
`TYPE_SCENE_3D` → the existing `Node3DEditorView` path; `TYPE_SCENE_MIXED` → both
behind a per-view 2D/3D toggle. **This is the milestone that makes a 2D scene
render correctly in a pane** — the current blocker for main-screen replacement.

Verification: a 2D document renders in a pane (probe: its World2D canvas RID is
bound; scene visible), two panes show two different 2D docs simultaneously,
smoke green. Commit-per-green as usual.

### ⑤b — CanvasItemEditor services/view split (hard; the 3D-③ parallel)
Factor the VIEW-state members and the `_draw_viewport` overlay path + input into
`CanvasView2D`, leaving only SERVICE on the `CanvasItemEditor` singleton (keep
`get_singleton()` for the ~100 external callers, forward like Node3DEditor does).
Now each pane's 2D view manipulates (select/drag/handles), reading tool/snap from
the service. Per-pane selection overlay ties to **#6 (per-document selection)** —
the overlay draws the selection, which must be the pane's document's selection.
Use the compiler-driven ref-finding + reparent-tolerance patterns from ③.

### ⑤c — retire the scene_root reparent shim
Once `CanvasView2D` renders via the shared World2D, `_display_scene_root`'s
reparenting is dead: `scene_root` stays parked under `documents_holder` always
(exactly like 3D), and `_activate_scene_views` stops moving it. Removes the SHIM
divergence (`canvas_item_editor_plugin` `scene_view_container` +
`editor_node._display_scene_root`) from the ledger. **Unblocks main-screen
replacement** (panes now render 2D and 3D documents).

---

## Caveats to design around
- **CanvasLayers** attach to a CanvasLayer's own canvas registered against a
  specific viewport (`canvas_item.cpp:272-273`), not the World2D main canvas — so
  CanvasLayer content needs per-viewport handling distinct from the shared canvas.
- **Input routing:** a `CanvasView2D` whose SubViewport is not the scene node's
  parent still routes input (shared World2D + its own picking), but the editing
  code assumes the single `scene_view_container` — per-pane editing overlays/gizmos
  need the per-viewport treatment 3D gives gizmo instances.
- **Selection is scene-scoped** (`EditorSelection` via `editor_selection`), not
  pane-scoped — full per-pane editing overlay wants #6 first.
- **`get_scene_root()` global** (`editor_node.cpp:4593`) must be threaded as the
  pane's document, mirroring the 3D `set_active_world` refactor already done.

## Recommended entry point
Start with **⑤a** (render-only CanvasView2D + type-aware DocumentView). It is the
smallest slice that delivers the unblocking value (2D scenes render in panes),
proves the World2D-share design end to end, and defers the large overlay/input
extraction (⑤b) and the shim removal (⑤c) to follow-on green commits.
