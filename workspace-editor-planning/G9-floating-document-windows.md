# G9 — Floating document windows

**Status:** Planned; investigation-gated  
**Date:** 2026-08-05  
**Effort:** M–L  
**Depends on:** G2 document workspace, G3 document surfaces, G6 live tab transfer  
**Recommended product:** Native, modeless document windows; embedded overlay only as a later fallback

## Summary

Allow a tabbed document pane to leave the main editor workspace and live in its own native window.
The primary use case is a large scene viewport on one monitor with scripts, shaders, resources, or a
second scene pane on another monitor. Closing the native window redocks its documents without closing
them. Closing a tab retains the existing document-close semantics.

This is worth building for a multi-monitor workflow. An overlay inside the main editor is not the
primary product: existing splits already provide most of its value, while a native window adds screen
real estate, OS window snapping, independent maximization, and a useful second-monitor workflow.

The rendering side is not the hard part. The current fork already has instanceable `DocumentView`
surfaces, live `detach_tab()` / `adopt_tab()` transfer, and 2D/3D view resources hardened for
reparenting. The real work is making all windows participate in one focused-document contract:
focus, shortcuts, Save, undo/redo, shared toolbars, reveal/close lookup, dialogs, and persistence.

The recommended MVP is deliberately narrow:

- A context-menu command, **Move Pane to New Window**.
- One tabbed pane per native window. The pane may contain multiple document tabs.
- A **Move Pane Back to Main Window** command.
- Closing the native window redocks the pane.
- Native window position, size, screen, tabs, and active tab persist.
- Cross-window drag/drop and nested splits inside floating windows are deferred.

## What this buys

- A maximized 3D/2D scene viewport without sacrificing space to an editor split.
- Scripts, shaders, resources, or a reference scene on a second monitor.
- Persistent task contexts with normal OS positioning, snapping, Alt+Tab, and maximization.
- Large side-by-side comparisons that remain useful at ordinary editor zoom levels.
- A natural completion of the document/pane model: pane placement stops at the editor-window
  boundary today even though the view model itself is already portable.

The key workflow is:

> Main monitor: large scene viewport. Secondary monitor: tabbed script/shader/reference-scene pane.

This does **not** introduce simultaneous global editing contexts. There remains one globally active
workspace/pane/document. Other visible views render and retain their state, but editing commands and
shared chrome follow the active native window exactly as they follow the focused pane today.

## Product decisions

### Native window is the target

Use a native `Window` through the existing `WindowWrapper` infrastructure. This is the only variant
that creates substantial value beyond current splits.

### Do not make it modal

A true modal blocks interaction with the main editor and defeats the point of a second document
surface. If an in-editor floating variant is added, it must be a **modeless, draggable/resizable
overlay**, backed by the same workspace manager as native windows.

### Undock a logical pane, not a physical split-tree node

Do not reparent a live `WorkspacePane` out of its `SplitContainer`. The split tree assumes physical
parentage when collapsing and persisting geometry. Instead:

1. Create a destination floating workspace and tab host.
2. Move each live `DocumentView` through `TabbedDocumentHost::detach_tab()` / `adopt_tab()`.
3. Collapse the now-empty source pane when it is not the main root; leave an empty root tab host
   otherwise.

This preserves surface identity and editor state without corrupting the source tree.

### Window close means redock

The native title-bar close button is a placement operation, not a document-close operation. It moves
the live tabs back to the remembered origin pane, falling back to the active main-window tab host if
that pane no longer exists. Explicit tab close continues through the existing save/prompt pipeline.

### Keep one-document-one-host for G9

The existing no-duplicate-tab rule remains. Undocking moves a view; it does not clone another view of
the same document. Duplicate views can be considered separately after ordinary per-view restart state
and the human multi-pane audit are complete.

## Findings against the current code

### Already in place

| Existing seam | Why it helps G9 |
|---|---|
| `TabbedDocumentHost::detach_tab()` / `adopt_tab()` | Moves the live `DocumentView` without firing close hooks or rebuilding the surface. |
| `DocumentView` + `EditorDocumentSurfaceInstance` | Scene, script, shader, help, screen, and resource UI are already view-owned rather than hard-coded into one central stack. |
| Per-view 2D/3D surfaces | Each scene pane owns its viewport binding and view resources. Reparenting does not require moving the scene root or recreating its world. |
| Reparent-tolerant 3D view lifecycle | Gizmo instances, private editor layers, grid/origin decorations, and toolbar mounting already survive pane moves. |
| `WindowWrapper` | Provides native-window creation, monitor placement, close signaling, shortcut propagation, progress-dialog hosting, and control reparenting. |
| Versioned workspace geometry and tab persistence | Gives G9 a migration precedent: unknown versions fail to a safe default instead of partially applying. |

### Gaps that block a safe implementation

1. **Only the main workspace is enumerable.** `EditorMainScreen` finds documents and the focused view
   through its one `EditorWorkspace`. A separate native host would be invisible to Save, reveal,
   close, and tab-destruction paths.
2. **Focus is local to one viewport.** `EditorWorkspace` listens to `gui_focus_changed` on its current
   viewport. A native `Window` owns another viewport, so focus must be coordinated across workspace
   roots.
3. **Multiple workspaces could remain context-active.** The current `set_focused_pane()` deactivates
   the old pane only inside the same workspace. G9 needs a single global active-workspace invariant.
4. **Host ownership is inferred from ancestry.** `TabbedDocumentHost::_owning_pane()` walks to a
   `WorkspacePane`, and pane close/move assumes one workspace. This works inside a floating
   `EditorWorkspace`, but cross-workspace transfer must live above either workspace.
5. **Current drag payload resolution is window-local in practice.** G6 resolves a source tab using a
   `from_path` from the target host. Whether Godot Control drag/drop crosses native Window viewports
   reliably is an investigation item, not an MVP assumption.
6. **Persistence stores one tree.** The current blob contains one geometry tree plus its pane-tab map.
   G9 must add window identity and placement while retaining a v1 migration path.
7. **Shared chrome must cross windows.** Script chrome and the 2D/3D toolbars are single service-owned
   Controls that reparent to the focused `DocumentView`. They must follow native-window focus and park
   safely when a window disappears.
8. **Dialogs are globally owned.** Save As, resource dialogs, warnings, and some plugin dialogs may
   appear on the main monitor even when invoked from a floating pane. Dialog transient-parent routing
   needs an explicit audit.
9. **Singleton screen documents are special.** Game, AssetLib, and arbitrary legacy/plugin screens
   carry assumptions about the main editor shell. They should remain in the main workspace for the
   MVP unless a screen explicitly opts into floating.

## Recommended architecture

```text
EditorWorkspaceManager (one global coordinator)
├── main EditorWorkspace
│   └── existing split tree and tab hosts
└── FloatingDocumentWindow [0..N]
    └── WindowWrapper
        └── EditorWorkspace (MVP: one leaf)
            └── WorkspacePane
                └── TabbedDocumentHost
                    └── live DocumentView tabs
```

### `EditorWorkspaceManager`

Add one editor-owned coordinator responsible for placement and focus across every workspace root.
It should not own document or render state. Its responsibilities are:

- Register/unregister the main workspace and floating workspaces.
- Track the one globally active workspace and focused pane.
- Enumerate every `TabbedDocumentHost`.
- Find the host/pane/window containing an `EditorDocument`.
- Return the globally focused `DocumentView`.
- Transfer tabs or panes between workspaces using the existing detach/adopt seam.
- Create, redock, and destroy `FloatingDocumentWindow` instances.
- Deactivate the old host before activating the new one, including ScriptEditor, ShaderEditor,
  scene activation, toolbar mounting, and document-surface context.

`EditorMainScreen` should delegate document lookup, reveal, close, drop, and focused-view queries to
this manager. No command should have a separate “if floating” branch.

### `EditorWorkspace`

Keep split-tree ownership local. Add an explicit active/inactive contract so pane focus inside a
background native window does not leave two hosts active. A workspace may remember its last focused
pane while inactive, but only the manager's active workspace may:

- set a surface context active;
- set ScriptEditor/ShaderEditor current surface;
- activate the global edited scene;
- own shared script or scene toolbar chrome;
- answer focused-document command routing.

Each floating window gets a real `EditorWorkspace`, even though the MVP exposes only its root leaf.
This keeps focus discovery inside that Window's viewport and provides a clean path to optional
floating split trees later.

### `FloatingDocumentWindow`

An editor GUI owner around `WindowWrapper` and a child `EditorWorkspace`. It stores:

- stable floating-window id;
- remembered main-workspace return pane id;
- native rect, screen, and mode;
- its workspace root;
- close/redock state and shutdown guard.

It listens for native window focus and close. Native focus activates its workspace through the
manager. User close redocks; editor shutdown serializes and tears down without a pointless redock.

### Active-context invariant

At every observable point:

```text
exactly one active workspace
  -> exactly one focused leaf
    -> at most one current tab/view
      -> exactly that view receives focused commands and shared chrome
```

This invariant is the acceptance gate for Ctrl+S, undo/redo, scene tool shortcuts, script commands,
and contextual docks. Window z-order alone is not sufficient; activation must be recorded when the
OS window gains focus and when a tab changes inside it.

## Transfer behavior

### Move Pane to New Window

1. Validate that the source is a tabbed leaf and contains at least one supported document.
2. Create a hidden `FloatingDocumentWindow` and its single-leaf workspace.
3. Record the source pane id as the preferred redock target.
4. Detach/adopt every tab in order, preserving the current document and all live view ObjectIDs.
5. Collapse an empty non-root source pane; retain an empty main root host.
6. Show and focus the native window only after transfer completes.
7. Queue a layout save.

### Move Pane Back to Main Window / native close

1. Resolve the remembered source pane if it still exists and hosts documents.
2. Otherwise use the active main-window tabbed pane, then the last main tabbed pane, then the main
   root host.
3. Detach/adopt tabs in order and restore the current tab.
4. Activate the destination pane.
5. Destroy the empty floating workspace/window after deferred signal completion.
6. Queue a layout save.

### Later: Move Tab to New Window

This is a small extension once the manager exists: detach one tab instead of draining the source
host. It is intentionally not required for the first pane-level MVP.

## Persistence design

Extend the workspace blob to a new version instead of adding loosely related keys. Conceptually:

```text
{
  "v": 2,
  "main": { "geometry": ..., "tabs": ... },
  "windows": [
    {
      "id": 1,
      "screen": 1,
      "rect": Rect2i(...),
      "mode": "windowed|maximized",
      "return_pane": 7,
      "workspace": { "geometry": ..., "tabs": ... }
    }
  ],
  "active": { "workspace": "main|window-id", "pane": pane_id }
}
```

Requirements:

- Read the existing unversioned/v1 main-workspace blob exactly as today and treat it as no floating
  windows.
- Unknown v2+ versions fall back to a main default workspace; never partially restore windows.
- Pane ids are scoped to their workspace id, not assumed globally unique.
- Restore window shells and geometry before document views, following the existing two-phase scene
  restore rule.
- Populate tabs only after scene documents exist; show native windows after successful population.
- Skip missing document paths and empty restored windows.
- Clamp an absent monitor's window rect to the main display through `WindowWrapper`'s screen restore
  behavior.
- Named workspace layouts should eventually include floating placement because they already store the
  workspace blob. Decide during the persistence spike whether session restore lands first and named
  layouts follow one milestone later.

Persistence is a one-way-door change. Do not write schema v2 until its loader, v1 migration, malformed
input fallback, and monitor-removal cases have automated coverage.

## Overlay option

An embedded overlay can reuse `EditorWorkspaceManager` and `FloatingDocumentWindow` semantics with a
different presentation adapter:

- draggable/resizable `PanelContainer` over the editor center area;
- modeless, never exclusive;
- same child `EditorWorkspace` and transfer rules;
- no OS screen placement;
- useful only as a fallback when native multi-window support is disabled.

Do not implement the overlay first as a disconnected shortcut. Without the manager it would repeat
the same focused-command bugs, and with the manager its incremental value is small.

## Investigation gates

No production implementation begins until the following bounded spikes are recorded in this file or
a linked `G9-*-SPIKE.md`.

### I0 — Native reparent and lifecycle spike

Create a temporary `WindowWrapper` host and move one live view of each supported kind through it:

- script/text;
- shader;
- generic resource/Inspector;
- scene 2D;
- scene 3D.

Exercise at least 20 undock/redock cycles for a 3D view. Verify no crash, stale viewport, lost camera
state, leaked private gizmo layer, duplicate toolbar, or surface close hook. Record backend and OS.

**Exit gate:** all surface kinds survive with stable `DocumentView`/surface ObjectIDs, or the failing
surface is explicitly excluded from MVP with a scoped fix plan.

### I1 — Native focus, commands, and dialogs spike

With main and floating panes visible, verify:

- Ctrl+S and Save As target the floating focused document;
- undo/redo targets its document history;
- Q/W/E/R and snap commands target a floating scene;
- script find, completion, IME, and editor shortcuts work;
- switching OS windows moves shared script/scene chrome exactly once;
- Save As, confirmation, warning, and progress dialogs appear over or visibly relate to the invoking
  native window;
- play/stop and debugger focus round-trips remain sane.

**Exit gate:** define one global activation seam; no individual command gets a floating-window patch.

### I2 — Cross-window drag feasibility spike

Test whether Godot Control drag payloads and `TabBar` previews cross native Window viewports on Windows,
Linux, and macOS. Specifically audit G6's `from_path` source resolution.

**Exit gate:** if any platform is unreliable, cross-window drag remains deferred and the product uses
explicit Move commands. Do not build an OS-specific drag transport for the MVP.

### I3 — Persistence and monitor spike

Prototype schema-v2 round-tripping without showing windows. Test:

- v1 migration;
- two floating windows with colliding local pane ids;
- missing document paths;
- missing monitor / changed DPI;
- corrupt window entry;
- active floating window absent after filtering.

**Exit gate:** save-load-save is stable and every invalid case degrades to a usable main workspace.

### I4 — Resource/performance soak

Compare main-only and two-window sessions for idle CPU/GPU, render-target updates, private editor layer
allocation, and teardown. Background documents remain live by current architecture; G9 must not
accidentally render hidden tabs continuously or leak native window resources.

**Exit gate:** costs scale with visible views, not with undock/redock count.

## Implementation plan

### G9.1 — Workspace manager and global focus

**Goal:** make multiple workspace roots logically possible before any production window UI exists.

- Add `editor/gui/editor_workspace_manager.{h,cpp}`.
- Register the main `EditorWorkspace` and expose active workspace/pane/view queries.
- Move cross-workspace enumeration and document lookup behind the manager.
- Refactor `EditorMainScreen::{reveal,close_document,drop_document_tabs,get_document_view,
  get_focused_document_view}` to use it.
- Split `EditorWorkspace::set_focused_pane()` into local selection and globally active-context work,
  coordinated by the manager.
- Add tests with two in-tree workspace roots proving one active context and global document lookup.

**Commit boundary:** manager landed with one registered workspace and zero user-visible behavior change.

### G9.2 — Native single-pane MVP

**Goal:** explicit undock/redock with no persistence and no cross-window drag.

- Add `FloatingDocumentWindow` backed by `WindowWrapper`.
- Add **Move Pane to New Window** to the tab/pane context menu.
- Transfer all tabs through detach/adopt; preserve order, current tab, and live ObjectIDs.
- Connect native focus to manager activation.
- Implement explicit redock and title-bar-close redock.
- Disable the command with a clear tooltip when native multi-window is unavailable.
- Exclude `ScreenHostDocument`/legacy singleton screens from MVP floating.
- Add smoke coverage for transfer/redock ownership; complete the native manual matrix.

**Commit boundary:** a dependable, non-persistent native pane window.

### G9.3 — Session and layout persistence

**Goal:** windows survive restart safely.

- Implement versioned schema v2 and v1 migration.
- Save/restore rect, screen, mode, return pane, geometry, tabs, current tab, and active workspace.
- Preserve the existing two-phase geometry/document restore ordering.
- Restore native windows hidden and reveal only valid non-empty results.
- Add malformed-input and missing-monitor tests.
- Decide and document whether named layouts include floating windows in this milestone or G9.4.

**Commit boundary:** restart round-trip is deterministic and backwards compatible.

### G9.4 — Product polish

**Goal:** make the native workflow feel intentional rather than bolted on.

- Dynamic native title/icon from the current document.
- Window menu actions: redock, move active tab to main, new window if later enabled.
- Correct dialog transient parent and progress-host behavior.
- Per-monitor DPI/theme refresh audit.
- Window close during async scene-save prompt.
- Main-window/project shutdown ordering.
- Optional cross-window drag only if I2 passes on all supported desktop platforms.
- Optional splits inside a floating workspace only after the single-pane lifecycle soaks cleanly.

### G9.5 — Optional embedded overlay

Only if requested after native windows ship:

- Add a modeless overlay presentation adapter using the same manager/workspace owner.
- Use it as a fallback for embedded single-window mode.
- Do not persist overlay coordinates in the native-window schema without an explicit type field.

## Expected effort

| Work | Estimate |
|---|---:|
| Investigation gates I0–I2 | 1–2 days |
| Workspace manager/global focus | 2–3 days |
| Native explicit undock/redock MVP | 2–3 days |
| Persistence and migration | 2–4 days |
| Dialog, platform, DPI, drag, and lifecycle polish | 3–7 days |

A useful native MVP is approximately one focused engineering week after the spikes. A polished,
cross-platform feature with restoration and optional drag is approximately two to four weeks. Nested
floating split trees are outside that estimate.

## Risk register

| Risk | Failure mode | Mitigation |
|---|---|---|
| Active-context split brain | Ctrl+S, undo, or tool shortcuts hit the last docked pane | One manager-owned active workspace; commands query only it. |
| Physical split-tree corruption | Reparented pane can no longer collapse or serialize | Move live views via detach/adopt; never extract a `WorkspacePane`. |
| Shared chrome stranded in a closing window | Toolbar disappears or is freed with the window | Deactivate/park chrome before deferred window teardown. |
| Scene render-resource leak | Repeated moves exhaust gizmo layers or retain SubViewports | Stable view transfer plus I0 cycle/teardown soak. |
| Wrong dialog monitor | Save As or confirmation appears behind the editor | Focused-window transient-parent routing audited in I1/G9.4. |
| Cross-window drag inconsistency | Drops work on one OS/backend only | Explicit Move commands are MVP; I2 is a hard gate. |
| Persistence one-way door | New layouts cannot be read or partially corrupt startup | Version from first write, v1 migration, fail-whole-blob fallback. |
| Missing monitor/DPI change | Restored window is invisible or incorrectly scaled | Use `WindowWrapper` screen restore/clamping and test changed topology. |
| Singleton screen assumptions | Game/AssetLib/plugin screen breaks outside editor shell | Exclude screen-host documents unless explicitly opted in. |

## Verification and acceptance criteria

### Automated

- Manager registers two workspace roots and enforces exactly one active root.
- Global find/reveal/close/drop sees documents in either root.
- Pane transfer preserves document order, current tab, `DocumentView` ObjectID, and surface ObjectID.
- Transfer does not call `notify_surface_closing()`.
- Closing a floating window redocks; closing a tab uses the ordinary close pipeline.
- Ctrl+S routing resolves the active floating view in a testable focus harness.
- Schema v1 migration and schema-v2 round-trip are stable.
- Unknown version, corrupt window, missing document, and missing monitor all fall back safely.
- Existing workspace smoke suite remains green.

### Required human pass

- Script typing, completion, find, IME, Save, Save As, undo/redo.
- 2D pan/zoom/select/transform and toolbar switching.
- 3D orbit/freelook/select/gizmos/transform and mouse capture.
- Shader and generic resource editing.
- Main ↔ floating focus switching with visible toolbar/context ownership.
- Twenty repeated undock/redock cycles and window close/reopen.
- Two monitors with different DPI; monitor disconnect before restore.
- Windows, Linux, and macOS native-window behavior.
- Play/stop/debugger/progress dialogs while a floating scene or script is active.

## Definition of done

- A user can move a supported tabbed document pane into a native window and back without recreating
  views or losing editor state.
- The OS-focused document window is the sole source of focused command routing.
- Native close redocks; tab close retains save/prompt semantics.
- Main and floating workspaces restore safely after restart, including monitor changes.
- No shared chrome, SubViewport, gizmo layer, or document-surface lifecycle leak is observed.
- Existing main-window-only workflows and workspace layout migration remain unchanged.
- Cross-window drag and floating splits are either verified cross-platform or remain explicitly
  deferred; they are not implicit requirements for declaring the MVP complete.
