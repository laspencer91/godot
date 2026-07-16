# WP22 — LE2 document material drawer + contextual texture tools

## Outcome

Replace the Level Editor's tall, global right-side material dock with a Level-document-owned
bottom drawer, and separate material selection from face texture manipulation. The resulting
layout must obey the workspace's multiple-tab/multiple-pane contract: a control shown inside
one Level document always reads and mutates that document's state.

This work package supersedes the UI-location decision in `PLAN.md` §2 and WP14/WP15 that put
the Material Browser and Modify Texture controls together in a global right dock. The material
indexing and texture-operation behavior from those work packages remains binding.

## Binding layout

```text
┌──────────────── Level document surface ────────────────┬─ right column ─┐
│ selection / grid / snap                         [Mats]  │ Scene Tree     │
├──────┬────────────────┬─────────────────────────────────┤ Inspector      │
│ tool │ contextual     │                                 │ Signals        │
│ rail │ options        │            viewport             │ Groups         │
│      │                │                                 │                │
├──────┴────────────────┴─────────────────────────────────┤                │
│ Materials [source] [search...........] [M_*] [− zoom +] │                │
│ [tile][tile][tile][tile][tile][tile] →                  │                │
│ [tile][tile][tile][tile][tile][tile] →                  │                │
└─────────────────────────────────────────────────────────┴────────────────┘
```

- Register **Materials** with `DocumentBottomDockHost` for `TYPE_LEVEL` documents.
- The drawer occupies only the document surface column. The right Scene Tree/Inspector
  accordion keeps its full height and never participates in drawer sizing.
- A persistent toolbar icon toggles the drawer. `M` toggles it while a Level document owns
  keyboard context. Opening explicitly focuses/selects the search field; programmatic reveal
  (Lift/active-material synchronization) does not steal viewport focus.
- The permanent left rail contains tool modes only. A user-collapsible contextual panel sits
  between the rail and viewport. Selection changes replace its contents without changing its
  open/closed width.
- Move the active material swatch to the bottom of the left rail. It is clickable and opens the
  Materials drawer to the active material.

## Ownership contract

### Project-shared services

These remain on `LevelEditor` and may be consumed by every document view:

- `MaterialIndex` and hidden-path metadata;
- `TexelDensityScanner`;
- `MaterialBrowserPreviewQueue` / thumbnail memory and disk cache;
- `BlockoutMaterialRegistry`;
- project-wide editor settings and shortcut registration.

### Level-document context

These must not remain as a single global interaction state:

- active material Ref, resource path, and stable binding path;
- captured Lift mapping;
- hotspot mapping-mode override;
- drawer open state, source filter, search text, convention filter, zoom, and scroll position;
- contextual-options panel open state.

Store the semantic material state on `LevelDocument` (or a document-owned context object).
Store presentation state through `EditorDocument::contextual_editor_states`. Legacy script
methods on `LevelEditor` remain compatible by resolving the focused Level document; internal
view/tool paths use explicit `LevelDocument *` overloads so a background pane can never mutate
the focused pane by accident.

The existing `active_material_changed(material, path)` signal remains for compatibility, but
document views must either receive an explicit document-qualified signal or reject updates not
addressed to their bound document.

## Materials drawer

### Gallery geometry

- Default logical cell size: **108 px** (25% below the existing 144 px).
- Zoom range: 80–160 logical px, step 8; toolbar minus/slider/plus and Ctrl+wheel over gallery.
- Layout is column-major and horizontally virtualized. Derive row count from available drawer
  height (normally two rows), then extend columns to the right.
- Enable horizontal scrolling and disable vertical scrolling. Ordinary wheel input scrolls the
  shelf horizontally; Shift+wheel remains valid platform behavior.
- Virtualize visible columns plus one overscan column on each side. Never instantiate one
  Control per indexed project material.
- Reflow immediately on drawer resize or zoom without changing selection.

For item index `i`, row count `R`, and cell size `C`:

```text
column = i / R
row    = i % R
position = (column * C, row * C)
content = (ceil(count / R) * C, R * C)
```

### Header and tile behavior

- Header: source (`Project`, `In Level`, `Hidden`), expanding search, `M_*` convention filter,
  and compact zoom controls. Collapse source into a dropdown when width is constrained.
- Tile: image-dominant thumbnail, selected accent border, one-line material name, compact
  dimensions/status line. Resource class and full path belong in the tooltip, not permanent
  overlay text.
- Left click commits active material on **mouse release**. A press that becomes a drag must not
  also change active material.
- Drag begins a material resource payload suitable for dropping onto the Level viewport. V1 may
  expose the payload before viewport drop-to-apply is wired, but must preserve release timing.
- Double click opens the material resource document/Inspector. Right click retains Set Active,
  Hide/Unhide, Open in Inspector, and Reveal in FileSystem.
- When Lift or another explicit action changes the active material, reveal its tile without
  focusing search or changing the user's filter unless the item is already in that filter.

## Left tool rail and contextual panel

### Permanent rail

- Compact icon buttons (approximately 42 logical px) for Select and Block, with room for future
  placement/light modes. Text moves to tooltips and accessibility names.
- Group mode buttons at the top. Place the active material swatch at the bottom.
- Reuse theme `EditorIcons` wherever semantics already exist. Add only missing UV-specific
  tintable SVG icons; do not add raster UI artwork.

### Context resolution

The panel never automatically changes its expanded width. Its contents follow this priority:

1. active modal Fast Texture session — session status/accept/cancel only;
2. Block tool — block creation and snap/material settings;
3. Select tool + face selection — full Face Material / UV Transform / Hotspot controls;
4. Select tool + selected `LevelBlock` objects — same operations with an explicit
   “All faces of N objects” scope label;
5. vertex/edge/no actionable selection — selection help/options; no disabled wall of UV buttons.

Selection mode and counts come from the bound `LevelDocument::SelectionModel`. Object scope
comes from that document's `EditorSelection`; never query the global selection singleton.

### Texture-control organization

Remove `ModifyTexturePanel` from the browser. Rebuild its same command handlers in semantic
groups inside the contextual panel:

- **Material:** active swatch/name, Apply, Lift status, and captured-mapping indicator.
- **Nudge:** true 3×3 directional pad using arrow icons and the existing numpad shortcuts.
- **Scale:** U−, U+, V−, V+.
- **Rotate / Flip:** CCW, CW, Flip U, Flip V.
- **Align:** Left/Center/Right and Top/Middle/Bottom, plus a distinct Fit action.
- **Hotspot:** show only when actionable; mapping-mode override plus Individual/Grouped fit
  actions and debug/preview affordances.

Every button calls the existing `LevelEditorView`/`SelectTool` operation path, producing the
same single undo diff as its shortcut. The UI must not duplicate kernel behavior.

## Implementation seams

1. Refactor `MaterialBrowserDock` from a registered singleton `EditorDock` into an embeddable,
   document-bound view (rename to `MaterialBrowserView` if practical; retain compatibility
   accessors for smoke/addons during migration).
2. `LevelEditor` owns shared services and creates/releases one browser view per live
   `LevelDocument`; it no longer registers a global Material Browser dock.
3. `DocumentView` asks `LevelEditor` for the bound browser view when constructing a Level
   document, registers it with `DocumentBottomDockHost`, restores drawer state, and releases it
   before child teardown.
4. `LevelEditorView` owns the rail/context panel and exposes explicit document-bound material
   operation/reveal methods used by the browser.
5. Route all active-material and captured-mapping reads in `SelectTool` and
   `FastTextureOverlay` through the bound document context.
6. Preserve old public script methods by resolving the current Level document; add explicit
   document APIs for internal C++ paths and focused regression tests.

## Acceptance criteria

- Two visible Level documents may hold different active materials, captured mappings, hotspot
  overrides, filters, zoom levels, and drawer states without cross-talk.
- Opening Materials shrinks only the viewport-side surface and pushes its bottom-anchored HUDs;
  the right accordion height is unchanged.
- Default gallery tiles are 108 logical px and the drawer presents a horizontal two-row shelf at
  ordinary heights. Zoom/reflow does not lose active selection or request all thumbnails.
- Normal click commits on release; dragging a tile does not fire click selection.
- UV controls are absent outside actionable face/object contexts and immediately reflect the
  bound document's selection.
- Existing material Apply/Lift/Modify/Hotspot commands retain undo and shortcut behavior.
- The retired global Material Browser cannot be focused or reopened through dock commands/layout
  restoration.

## Verification sequence

During implementation, use only targeted validation:

1. compile affected object files or incremental editor target;
2. focused `material_browser_smoke`;
3. focused `face_texture_smoke` and `fast_texture_smoke` after routing changes;
4. a new two-Level-document context smoke covering independent active material/drawer state;
5. a focused drawer geometry smoke covering horizontal layout and unchanged right-column height.

Run the full workspace editor suite and complete kernel smoke set **once, at the end**, after all
tracks are integrated. Do not repeatedly pay the full-suite cost during development.

## Implementation status — complete (2026-07-15)

The document-owned Materials drawer, horizontal virtualized shelf, compact tool rail,
selection-aware texture context panel, and per-Level-document interaction/presentation state are
implemented. The global Material Browser registration and its mixed material/UV control surface are
retired. Drawer and tool commands resolve the focused pane's `LevelDocument`; two views of the same
or different Level documents do not share active material, drawer, filter, zoom, scroll, captured
mapping, hotspot override, context-panel, or tool-mode state.

Final validation:

- `scons platform=windows target=editor dev_build=yes -j8` — passed.
- All seven `modules/level_kernel/tests/smoke_project` scripts — exit 0, success marker present,
  zero matched errors.
- `SMOKE_HEADLESS=1 workspace-editor-planning/smoke/run_smoke.sh <dev console binary>` —
  `SMOKE: PASS`; every shared workspace and Level-editor case exited 0 with zero error-class lines.
- Claude Opus `/simplify` was run over the scoped implementation; its behavior-preserving
  refactors were audited and rebuilt before the final suites.

Final validation also closed three shared seams exposed by the integrated run: desired-size
isolation keeps bottom drawers from resizing the right document column; Scene Tree gesture coverage
is isolated to its owning document composite; and editor shutdown destroys scene `DocumentView`s
before their raw `EditorDocument` models.

The final UX-polish pass also made new Level geometry use the document's single selected node as
its structural parent. A transformed `Node3D` parent receives the inverse-composed local transform,
so the block remains exactly where it was drawn in world space; scene-root placement retains the
legacy world-baked representation. Focused coverage verifies parent/owner identity, world bounds,
undo/redo, surface-grid snapping with general snapping disabled, face-local edge picking, passive
edge/vertex previews, compact grid controls, and the contextual-panel separator. The concluding
headless workspace rerun reached `SMOKE: PASS` with every case at exit 0 and zero error-class lines.

## Likely files

- `editor/level/material_browser_dock.{h,cpp}` (or new `material_browser_view.*`)
- `editor/level/level_editor.{h,cpp}`
- `editor/level/level_editor_view.{h,cpp}`
- `editor/level/select_tool_texture.cpp`
- `editor/level/fast_texture_overlay.{h,cpp}`
- `editor/editor_document.h`
- `editor/gui/document_view.{h,cpp}`
- `editor/gui/document_bottom_dock.{h,cpp}` only for generic additive seams
- `editor/icons/*.svg` only for missing UV semantics
- focused smoke addons/testbed fixtures
- `level-editor-planning/PLAN.md`
- `workspace-editor-planning/DIVERGENCE-LEDGER.md`
