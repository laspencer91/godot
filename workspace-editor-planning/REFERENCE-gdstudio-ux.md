# GDStudio UX Reference (observed)

Reference notes for the workspace-editor project, reverse-engineered **from observed behavior in GDStudio screenshots** (https://gdstudio.dev/) — not from its (closed) source. Used to ground our own open implementation on the `feature/workspace-editor` branch of this Godot 4.7 fork.

> Legal note: ideas/behavior/layout are not protected. We implement from observation only; we do not decompile or copy GDStudio binaries/assets. Godot's editor is MIT, so this fork is legitimate.

This file is hand-maintained context. The dual-agent reconciled plans (INDEX.md + G1..G5) are written into this same folder by the planning workflow.

---

## 1. Global chrome

- **Wordmark + menu bar:** "GDstudio" at top-left, then menus: `Project`, `View`, `Help` (a `File` menu also appears in script-editing context). The stock Godot top-row scene tabs + main-screen (2D/3D/Script/AssetLib) buttons are **gone**.
- **Centered project/run control:** the current project name + a green **Play** button live centered in the title bar (e.g. "Starter Kit 3D Platformer ▶"), not in a left toolbar.
- **Persistent bottom status bar** (always visible, full width):
  - Left: **`Filesystem` | `Log` | `Audio`** toggle buttons. These summon bottom panels — i.e. the "bottom drawer" hosts more than FileSystem; Log and Audio are peer panels.
  - Center/left: a **live status text area** (observed: `Instantiating: (-6.265, 1.5, -2.768)`).
  - Right: **status icons** (error / warning counts, etc.).
- **Theme:** dark, rounded tabs, document-type icons on each tab (scene icon, script icon), an accent dot on the active/unsaved tab.

## 2. Workspace = dividable tabbed panes (Goal 2)

- The main area is a **binary tree of horizontal/vertical splits**; each leaf is a **tab group** (its own tab bar). Each tab is one **document pane**: a scene, a script, or a resource.
- **5-zone drag-drop split overlay:** while dragging a tab, hovering a pane shows an overlay = a **center square + 4 edge arrows** (up/down/left/right). 
  - Center square → **add as a tab** to the hovered group.
  - Left/Right arrow → **wrap the leaf in a new horizontal split**, dropped tab becomes the side sibling.
  - Up/Down arrow → **new vertical split**.
  - On drop: mutate the layout tree and re-layout. The tree must **persist/restore** across sessions.
- **Godot primitives to use:** `TabContainer`/`TabBar` rearrange groups (`set_tabs_rearrange_group`, `drag_to_rearrange_enabled`), `SplitContainer`, `Control._get_drag_data`/`_can_drop_data`/`_drop_data`, `_draw` for the overlay. Prior art already in-tree: `EditorDockManager`'s `DockSplitContainer` / `DockTabContainer` / floating `WindowWrapper`.

## 3. Per-pane contextual docks (Goal 3)

Docks depend on the **active tab's document type** and are scoped **per document**:

| Tab type | Pane contents |
|---|---|
| **Scene** | Viewport (with its own toolbar) + **Scene Tree** dock (top) and **Inspector** dock (below) on the side, scoped to that document. A **2D/3D toggle** ("2D" button at top-left of the viewport toolbar) appears if the scene has both. |
| **Script** | Script editor body (with `File/Edit/Search/Go To/Online Docs/Search Help` row, line numbers, code) + side docks **Inspector** (top) and **Methods** (function-outline list) below. The Methods dock **replaces the legacy ScriptEditor left-hand function list**; scripts are first-class regular tabs, not the single-view side-list. |
| **Resource** | Opens in its own tab rendered **as an Inspector** (e.g. `GradientTexture1D`, `StandardMaterial3D`), with full-pane resource sub-editors (gradient editor, color picker, etc.). |

- The per-scene viewport toolbar is the standard Godot transform toolbar (snap, transform tools, Add, Instance, lock, group, node dropdown `(none)`, `res://...tscn` path, a layout button).
- Implementation: per-document instances of `SceneTreeDock`, `InspectorDock`, `EditorSelection` (route the 76 / 43 / 59 singleton call sites to an active-document accessor). Depends on Goal 1 `DocumentContext`.

## 4. Multiple live scenes at once (Goal 1)

- Two (or more) **3D scenes render simultaneously** in side-by-side panes, each with independent selection + gizmos (observed: `main.tscn` and `coin.tscn`/`player.tscn` both live, each with own gizmo manipulator).
- **The wall:** stock Godot renders all 3D into one root-window `World3D` scenario with gizmos hard-wired to it → each document must own a `World3D` and route gizmos to its own scenario. De-risk with the two-own-World3D viewport spike before the big refactor.

## 5. Bottom drawer (Goal 4)

- A panel that **slides up over the workspace**, summoned by the bottom-bar `Filesystem`/`Log`/`Audio` toggles.
- The FileSystem drawer header: back/forward nav, **`Filter Files`** search, sort, **list/grid toggle**, current path (`res://objects/`), a layout button. Body = Favorites + `res://` tree on the left, **file grid with thumbnails** in the middle, **Import** panel on the right.
- **Drag-drop** files from the drawer directly into a tab/scene/inspector (observed instantiating a dragged object into a scene).
- `FileSystemDock` stays project-global; host it in a slide-up overlay layer above the split workspace.

## 6. Faster load (Goal 5)

- GDStudio reportedly cut editor/scene open from ~7s → ~3s; mechanism unknown. Investigate startup + scene/resource load path (plugin init order, `EditorFileSystem` scan/import, resource & thumbnail preloading, dock construction, `ResourceLoader` threading, shader/compile caches). Measure with the user's `objectdb_profiler` module.

---

## Screenshot inventory (source observations)

- **A** — Resource-as-tab: a scene pane (left) split beside a `GradientTexture1D` resource tab + `StandardMaterial3D` tab (right), each rendered as a full-pane inspector with color picker.
- **B** — Two live 3D scene panes (`main`, `coin`) side-by-side, each own Scene Tree + Inspector; bottom drawer open with FileSystem + Import.
- **C** — The 5-zone split overlay (center square + 4 arrows) shown mid-drag over a pane.
- **D** — Full layout: `main` scene pane (Scene Tree + Inspector) beside a `player` **script** tab whose side docks are **Inspector + Methods**.
- **E** — Bottom bar with `Filesystem | Log | Audio` toggles + live status text + bottom-right status icons; drawer hosting FileSystem (thumbnail grid) + Import.
