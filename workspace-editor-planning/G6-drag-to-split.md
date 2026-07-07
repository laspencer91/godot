# G6 — Drag a tab to split a pane (compass drop-zones)

**Effort:** S–M · **Depends on:** S8 split engine (landed) · **Do after G4**

## Summary

Drag a document tab over a pane and a 5-zone **compass** appears (center +
N/S/E/W). Dropping on an arrow splits the pane in that direction and moves the
dragged tab's live `DocumentView` into the new side; dropping on center just
moves the tab into that pane's tab bar. This is the direct-manipulation
equivalent of S8's tab-bar context menu (Split Right/Down), which already works.

## What already exists (the split engine — no changes needed)

S8 shipped every primitive the drop handler calls:

- `WorkspacePane::split(bool vertical, Control *content, bool new_on_second)` —
  the two bools already address all four directions:
  | Zone  | vertical | new_on_second |
  |-------|----------|---------------|
  | Right | false    | true          |
  | Left  | false    | false         |
  | Down  | true     | true          |
  | Up    | true     | false         |
- `TabbedDocumentHost::detach_tab(idx)` → returns the live `DocumentView` with
  **no close side effects**; `adopt_tab(doc, view)` re-homes it and selects it.
- `EditorWorkspace::split_pane_with_tab(pane, tab, vertical)` — already wires
  `detach → split → adopt`. Only gap: it hard-codes `new_on_second=true`
  (reaches Right/Down but not Left/Up).

## What's new (the only real work)

### 1. Drag source — tab payload
Override `_get_drag_data` on the workspace tab bar (the `TabBar` inside
`TabbedDocumentHost`, or a thin subclass) to emit a typed payload, e.g.
`{ "type": "workspace_tab", "source_host": ObjectID, "tab_idx": int }`, plus a
drag preview built from the tab's label/icon. Native `set_drag_to_rearrange_enabled`
only does *intra-bar* reordering, so we supply our own cross-pane payload.
Keep the native reorder for same-bar drops (or reimplement it in `_drop_data`).

### 2. Drop target + compass overlay — `PaneDropOverlay`
A lightweight transparent `Control` over each pane's content area (owned by
`TabbedDocumentHost`, sized to fill the content host). Normally
`MOUSE_FILTER_IGNORE`; becomes active on `NOTIFICATION_DRAG_BEGIN`, inert again
on `NOTIFICATION_DRAG_END` (so it never eats clicks outside a drag).

- `_can_drop_data(pos, data)` → true only for a `workspace_tab` payload;
  while hovering, `queue_redraw()`.
- `_draw()` → the 5-zone cross (center square + 4 arrows), highlight the zone
  under the cursor, and paint a translucent **preview rectangle** of the
  half-pane the split will occupy (whole-rect for center).
- `_drop_data(pos, data)` → hit-test `pos` against the 5 zone rects → resolve
  `(vertical, new_on_second)` (or center) → call the split path below.

### 3. Zone → action
Resolve the source host + tab from the payload's `ObjectID`/index, then:
- **N/S/E/W** → `EditorWorkspace::split_pane_with_tab(target_pane, tab, vertical, new_on_second)`
  (add the `new_on_second` param — one-line change; default `true` keeps the S8
  context-menu callers working).
- **Center** → `source_host->detach_tab(idx)` + `target_host->adopt_tab(doc, view)`
  (a plain cross-pane tab move; no split). If source == target, fall back to
  native reorder.
- **Same-pane single-tab guard:** dropping a pane's only tab back onto itself is
  a no-op (mirrors the `documents.size() > 1` guard on the S8 context menu).

## Ordered steps

1. Add `new_on_second` param to `EditorWorkspace::split_pane_with_tab` (default
   `true`); thread it into the `split()` call. Build+smoke (no behavior change).
2. `_get_drag_data` on the tab bar → typed `workspace_tab` payload + preview.
3. New `PaneDropOverlay` control: fill the content host, drag-gated activity,
   `_can_drop_data`/`_draw` compass hit-testing + preview rect.
4. `_drop_data` → zone→action table calling the split/adopt path.
5. Center-zone cross-pane move + same-pane reorder fallback.
6. Polish: preview-rect styling to match the dock-card theme; arrow icons;
   snap-to-zone thresholds. Owner visual pass.

## Files to touch

| Path | Change |
|---|---|
| `editor/gui/editor_workspace.{h,cpp}` | Add `new_on_second` to `split_pane_with_tab`. |
| `editor/gui/tabbed_document_host.{h,cpp}` | `_get_drag_data` payload + preview; own a `PaneDropOverlay` over the content host; center-zone move. |
| `editor/gui/pane_drop_overlay.{h,cpp}` | NEW: the compass overlay Control (`_can_drop_data`/`_drop_data`/`_draw`, drag-gated). |

## Risks

- Overlay eats input outside a drag → strict `MOUSE_FILTER_IGNORE` except
  between DRAG_BEGIN/END.
- Preview-rect vs actual split mismatch → derive the preview rect from the same
  `(vertical, new_on_second)` the drop will use (single source of truth).
- Nested-split target ambiguity (which leaf is under the cursor) → overlay is
  per-`TabbedDocumentHost`, so the hovered host *is* the target leaf; no tree
  walk needed.
