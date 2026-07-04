# Fork divergence ledger

**Why this exists.** `feature/workspace-editor` edits stock Godot editor files.
When we sync onto a newer upstream, the rebase conflicts land in exactly these
files — and `node_3d_editor_plugin.cpp` / `editor_node.cpp` are among *upstream's*
highest-churn files too. Knowing, per file, which hunks are **ours by design**
(structural, keep and re-apply) vs **incidental** (small, upstreamable, or soon
to be deleted) is what turns a rebase from days into hours.

Baseline: branch `feature/workspace-editor`, forked from `master` @ `895db87388`.
Regenerate the raw list with `git diff --name-status master...HEAD`.

**Rebase cadence policy.** Rebase onto upstream **at milestone boundaries only**
(e.g. after G2 lands), **never mid-surgery**. A mid-extraction rebase of
`node_3d_editor_plugin.cpp` — with half the services/view split applied — would
be miserable. Finish the current step to a green commit first, then rebase.

Legend — **Class**: `STRUCTURAL` = ours by design, will conflict, re-apply
deliberately · `SHIM` = intermediate scaffolding, will be rewritten at a later
step (don't over-invest in clean rebasing) · `UPSTREAMABLE` = a stock bug we
fixed; PR it and the hunk leaves our diff · `NEW` = file we added, no conflict
surface. **Risk** = expected rebase pain.

---

## Modified stock files (the conflict surface)

| File | Class | Risk | What diverged / rebase note |
|------|-------|------|------------------------------|
| `editor/scene/3d/node_3d_editor_plugin.{cpp,h}` | STRUCTURAL | **HIGHEST** | The services/view split (Step ③/①). New `Node3DEditorView` class owns viewports + `bound_world` + grid/origin decoration + resource lifecycle; `Node3DEditor` retains services and forwards accessors. Churn concentrates in: the ctor (`_build_view_viewports`, `main_view`), `_init_indicators`/`_finish_indicators`, `set_active_world`, `get_editor_world_3d`/`_scenario`/`_space_state`, and ~25 rerouted viewport accessors. Tagged `// Step③`/`// Step①`. On rebase: expect conflicts anywhere upstream touches viewport plumbing or indicator init; re-apply the *view ownership* intent, not line-by-line. |
| `editor/editor_node.{cpp,h}` | STRUCTURAL | **HIGH** | The multi-document spine. Added `documents_holder` (persistent in-tree parent keeping every open doc's `scene_root` live — including the placeholder), `register_document_context`, `_activate_scene_views`, and `get_scene_root()` delegating to the active document. `editor_node.cpp` is one of upstream's most-churned files. ⑤c RETIRED the `_display_scene_root` reparent shim: scene_roots stay parked under `documents_holder` for their whole lifetime; `_activate_scene_views` now binds the 2D main view to the active document via `CanvasItemEditor::activate_document()` (own world-bound viewport, no reparenting), mirroring the 3D `set_active_world`. `_activate_scene_views` is still SCAFFOLDING (retire when DocumentView owns activation) but the doc-context registration is STRUCTURAL. |
| `editor/editor_data.{cpp,h}` | STRUCTURAL | MEDIUM | `EditorData::EditedScene` gained an `EditorDocumentContext *document` member (memnew'd in the edited-scene path, ~line 630); added `get_document(idx)` / `get_active_document()`. Localized to the edited-scenes machinery. Note: this ownership model is exactly what the **Step ④ document split** revisits (EditorDocument/SceneDocument/EditorDocumentView) — these hunks will move, so don't polish them for rebase. |
| `editor/scene/canvas_item_editor_plugin.{cpp,h}` | STRUCTURAL | **HIGH** | The 2D services/view split (Step ⑤b.4). `CanvasItemEditor` is the 2D services singleton + a new instanceable `CanvasItemEditorView` (both in this file), mirroring the Node3DEditor split. Landed: display stack + pan/zoom (⑤b.4a), overlay (⑤b.4b), input/drag (⑤b.4c), document-binding (⑤b.4d — panes mint a view via `create_view_bound_to()`, each with its OWN `view_viewport` bound to the document's World2D; `CanvasView2D` deleted), and ⑤c (the MAIN view also renders through its own world-bound `view_viewport` via `activate_document()`, rebound per scene switch — the `_display_scene_root` reparent shim is gone). The SHIM surface (scene_root reparenting + `get_scene_view_container` forwarder) is fully retired; `scene_view_container` remains as the per-view host for `view_viewport`. High churn on both sides of a rebase; re-apply the *view ownership* intent. NEXT: this unblocks replacing the stock 2D\|3D\|Script main-screen switcher with the pane workspace. |
| `editor/editor_main_screen.{cpp,h}` | STRUCTURAL | LOW | Hosts the workspace: `EditorMainScreen` owns an `EditorWorkspace`; since G2 S5.5 the root pane hosts a `TabbedDocumentHost` whose tab 0 (the `ScreenHostDocument`) carries `main_screen_vbox`, parked under a hidden holder whenever its view dies so `get_control()` returns the live vbox at every instant (D11). `focus_editor()` reveals the screen-host tab before `_select_index`. UX delta: one tab bar above the main screen. M2.1 added a `// G2 D3` comment on the single make_visible driver in `_select_index` (comment-only). |
| `editor/plugins/editor_plugin.cpp` | STRUCTURAL | LOW | **Comment-only (M2.1).** A `// G2 D3` doc-comment on `EditorPlugin::make_visible` pins the visibility contract: main-screen `make_visible` toggles the plugin's own singleton screen surface only; per-pane document views own their render visibility and must never be driven by it. No behavior change; trivial to re-apply on rebase. |
| `editor/scene/scene_tree_editor.cpp` | **UPSTREAMABLE** | LOW | 9-line fix: `_selection_changed()` early-returns if `!is_inside_tree()` (the global `EditorSelection::selection_changed` signal can fire while the Scene dock is mid-reparent during layout restore → `_update_selection` resolves cached ABSOLUTE paths on an out-of-tree node → error). This is a **latent stock bug** (our multi-scene parking just exposes it), not flip-specific. **PR to `godotengine/godot`** (commit `637dba8048`); once merged upstream this hunk leaves our diff. Until then it's the smallest, safest divergence. |

## Added files (no conflict surface — additive only)

- `editor/editor_document.{cpp,h}` — the open-document model, split by state kind into `EditorDocument` (slim base) / `SceneDocument` (owns the world) / `EditorDocumentView` (per-pane view state). Renamed from `editor_document_context.{cpp,h}` in Step④ (2/n).
- `editor/gui/editor_workspace.{cpp,h}` — `EditorWorkspace` + `WorkspacePane` (the dividable pane tree).
- `editor/gui/document_view.{cpp,h}` — `DocumentView`: the per-pane presentation of one open document (owns the `EditorDocumentView` binding + hosts a `Node3DEditorView` on the document's world). Depends on `node_3d_editor_plugin.h` (gui→scene/3d, confined to this TU) — the permanent form of the ④-spike's temporary include.
- `editor/gui/tabbed_document_host.{cpp,h}` — `TabbedDocumentHost`: a pane's tabbed content (`TabBar` + lazily-created `DocumentView`s; selecting a tab swaps the rendered document). The first non-debug workspace UI.
- `workspace-editor-planning/**` — all planning docs, `ARCHITECTURE.md`, and the `smoke/` regression harness. Non-engine; zero rebase risk.

---

## Maintenance

- When you add or materially change a stock-file divergence, add/adjust its row
  here in the same commit. Keep the in-code tags (`// G1:`, `// G2 Step①:`) —
  the ledger says *which files*, the tags say *which hunks*.
- Re-verify the table against `git diff --name-status master...HEAD` at each
  milestone boundary (i.e. right before each rebase).

## Current G2 step notes

- `editor/gui/editor_workspace.{cpp,h}`: G2 M1.1 adds pane focus promotion on pre-GUI mouse-down / GUI focus-in, a multi-pane focus ring, `focused_pane_changed`, and `last_tabbed_pane` tracking for later `reveal()` target resolution.
- `editor/scene/3d/node_3d_editor_plugin.{cpp,h}`: G2 M1.2 carries the bound `EditorDocument *` into pane `Node3DEditorView`s and promotes that document on 3D mouse-down, mirroring the 2D `_ensure_active` gate.
- `editor/editor_document.h` and `editor/editor_main_screen.{cpp,h}`: G2 M1.3 adds `DocumentViewKind`, `reveal()`, and `focus_editor()`, while keeping `select()` / `select_by_name()` as compatibility shims.
- Script-screen intent callers under `editor/`: G2 M1.4 reroutes the documented `EDITOR_SCRIPT` sites to `focus_editor("Script")`; platform and mono callers remain untouched.
- `editor/editor_document.h`, `editor/gui/document_view.{cpp,h}`, `editor/editor_main_screen.{cpp,h}`: G2 S5.5 adds `TYPE_SCREEN_HOST` + `ScreenHostDocument` (seam #5) and makes the root pane a tab host from startup, so document reveals open as tabs in the current pane (no split-on-first-open). `DocumentView` gained a `NOTIFICATION_PREDELETE` handler — the destructor runs AFTER Node's PREDELETE frees children, so surface-detach work (script-view release, parking `main_screen_vbox`) must happen there; this also fixed S4's latent freed-pointer touch in `~DocumentView`.
- `editor/plugins/editor_plugin.cpp` + the 2D/3D plugin `make_visible` bodies: G2 M2.1 is an **audit + tags** step (no behavior change). Verified that main-screen `make_visible` cannot blank/freeze pane views: 2D show/hide/process reach only the singleton `main_view`, and the `viewport_set_disable_2d`/`environment` calls hit the ACTIVE doc's parked `scene_root` SubViewport (which no view displays — every view renders its World2D through its own `view_viewport`); 3D show/hide reaches only the singleton `main_view`, and `set_process`/`set_physics_process` gate only the singleton's own (near-empty) callbacks while each `Node3DEditorViewport` self-drives from `NOTIFICATION_VISIBILITY_CHANGED`. Contract documented on `EditorPlugin::make_visible` and tagged `// G2 D3` at the two plugin bodies + the `_select_index` driver. Note: the 2D scene_root viewport toggles are now effectively inert (candidate for a later `/simplify` cleanup) — left in place to keep the audit behavior-identical.
