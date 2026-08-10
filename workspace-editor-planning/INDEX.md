# Workspace Editor — Planning Package

Open re-implementation of the closed-source **GDStudio editor** on the custom Godot fork at
`C:/Development/Engines/godot`. The work originated on `feature/workspace-editor` and has landed
on `master`; do not use the old feature branch or path as the implementation baseline.

**Product direction / implemented core:** replace Godot's single-current-scene, single-main-screen
(2D | 3D | Script | AssetLib) editor with a **dividable, tabbed, multi-document workspace** where
multiple scenes, scripts, and resources are open at once, each pane carrying document-bound views
and contextual docks; editing commands remain scoped to the focused pane.

**Status (2026-08-05):** the G1/G2 workspace foundation, G4 Explore drawer, G6 drag-to-split,
and the core G3 per-pane document surfaces are implemented. G7 is partially implemented. G5
remains a measure-first investigation, and G9 now specifies investigation-gated native floating
document windows. The canonical smoke suite (`smoke/run_smoke.sh`) is the
automated baseline. A human multi-pane audit, generalized dirty/save behavior, and per-view
restart persistence remain; automated evidence is not being used as a substitute for those
human/lifecycle gaps. NOTE: the G-Level level editor was removed 2026-07-16 (Blender workflow
supersedes it); its final state, briefs, and smoke fixtures live on branch `archive/g-level-editor`.

This package now records both the landed architecture and the remaining work. The detailed goal
documents retain the original design rationale and historical sequence; current source and the
status table below take precedence over future-tense implementation steps in those older plans.

## Goals and status

| ID | Status in current worktree | Goal | File |
|---|---|---|---|
| **G1** | **Implemented; Automated Verified** | Multiple live scene documents, isolated worlds, per-document selection/history | [G1-multiple-scenes.md](./G1-multiple-scenes.md) |
| **G2** | **Implemented core; Automated Verified** | Dividable tabbed workspace, scene/script/help/shader/resource documents, per-pane 2D/3D, layout/tab persistence; per-view restart state is WP32 | [G2-dividable-workspace.md](./G2-dividable-workspace.md) |
| **G3** | **Partially Implemented; Automated Verified core** | Per-pane Scene Tree, Inspector, Signals, Groups; resource-as-Inspector tabs; focused-pane routing; generalized dirty/save and selected ownership migrations remain | [G3-contextual-docks.md](./G3-contextual-docks.md) |
| **G4** | **Implemented; Automated Verified** | Center-column Explore drawer hosting FileSystem, Import, and file-backed resource inspection | [G4-bottom-drawer.md](./G4-bottom-drawer.md) |
| **G5** | **Planned / Measurement Outstanding** | Faster open/startup investigation and measured optimization | [G5-faster-load.md](./G5-faster-load.md) |
| **G6** | **Implemented; Automated Verified** | Drag-a-tab-to-split compass and directional drop zones | [G6-drag-to-split.md](./G6-drag-to-split.md) |
| **G7** | **Partially Implemented; targeted automation complete** | Animation is document-owned; remaining contextual editors are evaluated case by case | [G7-contextual-bottom-docks.md](./G7-contextual-bottom-docks.md) |
| **G8** | **Design (2026-08-05)** | Audio workspace: AudioEvent document surface, parameter/curve authoring, document-owned Deck; first real adoption of the `editor/gui/components/` design system | [G8-audio-workspace.md](./G8-audio-workspace.md) |
| **G9** | **Planned / Investigation Gated** | Native floating document-pane windows with global focused-command routing, redock semantics, and persistence | [G9-floating-document-windows.md](./G9-floating-document-windows.md) |

## Cross-cutting references

| Doc | Purpose |
|---|---|
| [ARCHITECTURE.md](./ARCHITECTURE.md) | The service / view-state / document-state taxonomy + seam rules — the tiebreaker for where any editor member belongs. |
| [DIVERGENCE-LEDGER.md](./DIVERGENCE-LEDGER.md) | Per-file record of stock-file edits (ours-by-design vs upstreamable) + the rebase-cadence policy. |
| [GLTF-MATERIAL-OVERRIDES-DESIGN.md](./GLTF-MATERIAL-OVERRIDES-DESIGN.md) | Inline, per-field glTF material overrides; stable material identity; texture handling; and the authored-versus-derived resource contract. |
| [EXPLORE-PERFORMANCE-PLAN.md](./EXPLORE-PERFORMANCE-PLAN.md) | Code-verified Explore/FileSystem dock lag analysis (O(N²) preview scan, sync description probes, full-rebuild churn) + ordered fix plan; the no-file-I/O-at-row-build rule that derived-data hiding and model-import UI badges must follow. |
| [ASSET-FACT-INDEX.md](./ASSET-FACT-INDEX.md) | The shared foundation the Explore plan, material overrides, and derived data store all wait on: facts harvested once on the scan thread onto `FileInfo`, published as an immutable generation-stamped snapshot, queried everywhere. A→Z implementation steps + full per-concern traceability against all three docs. |
| [STEP5-CANVASVIEW2D-SCOPE.md](./STEP5-CANVASVIEW2D-SCOPE.md) | Historical scope and rationale for the now-landed per-pane `CanvasView2D`; retain for architectural context. |
| [STEP5b3-SELECTION-SCOPE.md](./STEP5b3-SELECTION-SCOPE.md) | Historical per-pane 2D selection/manipulation scope and the rationale for the chosen per-document selection model. |
| [smoke/](./smoke/run_smoke.sh) | Multi-flow regression harness. Future source changes must refresh its evidence rather than relying on fixture presence. |

## Historical dependency graph (landed sequencing)

```
                 ┌─────────────────────────────┐
                 │  G1  DocumentContext         │  ← FOUNDATION
                 │  (+ own-World3D SPIKE gate)  │
                 └──────┬───────────────┬───────┘
              hard      │               │   hard
            ┌───────────┘               └───────────┐
            ▼                                        ▼
   ┌─────────────────────┐                ┌─────────────────────┐
   │ G2 Dividable        │ ── hard ─────▶ │ G3 Per-pane docks   │
   │ workspace           │ ◀── soft ───── │ (true multi-pane    │
   │ (panes/tabs/split)  │   (panes feed  │  rendering needs G2)│
   └──────────┬──────────┘    dock host)  └─────────────────────┘
              │ soft (drop targets,
              │       layout persistence)
              ▼
   ┌─────────────────────┐        ┌─────────────────────────────┐
   │ G4 Bottom drawer    │        │ G5 Faster load              │
   │ (degrades to single │        │ (soft: A/B/C cleanest on    │
   │  main screen w/o G2)│        │  DocumentContext; measure-  │
   └─────────────────────┘        │  first, no hard dep)        │
                                  └─────────────────────────────┘
```

**Edge legend and outcome:** G1's document/world model was the hard foundation for G2 and G3;
G2 and G3 then converged on live pane-owned views and docks. G4 was intentionally independent and
landed as the global Explore drawer. G5 still has no hard dependency, but any future optimization
must measure the implemented document/workspace architecture rather than prototype against the old
flat editor model.

## Current remaining-work order

The original G1 → G2/G3 sequence succeeded and should not be restarted. Current work should use
the landed `EditorDocument` / `EditorDocumentView` / `EditorWorkspace` seams and proceed in this
order:

1. **Finish restoration state and human acceptance.** Implement per-view camera/canvas restart
   persistence for ordinary 3D and 2D views (the WP32 brief, archived on `archive/g-level-editor`,
   documents the intended scope), then complete the human multi-pane, duplicate-view, restart,
   focus, and mouse-capture audit. The temporary split/debug shortcuts are removed in the current
   worktree; that is no longer an open queue item.
2. **Finish generalized document lifecycle and selected G3/G7 ownership migrations.** Wire dirty,
   save/save-all, close prompts, and tab indication, then move only the contextual editors that
   materially need document ownership into `DocumentBottomDockHost`. Animation is the reference
   implementation; project-global tools remain focus-bound.
3. **Extend the document model, do not rebuild it.** New resource/tool surfaces should add an
   `EditorDocument` kind or a view factory where specialization is justified; generic resources
   already have Inspector-backed tabs.
4. **Start G5 only with its measurement gate.** Define a representative workload, capture repeatable
   cold/warm baselines on an optimized editor, then rank or discard the proposed optimizations.
5. **Start G9 only with its native-window gates.** Prove live surface reparenting, global focus and
   command routing, dialog ownership, and cross-window drag feasibility before writing a persistence
   schema. Ship explicit undock/redock before optional drag or nested floating splits.

Historical rationale: G1 was correctly treated as the hard world/document foundation; G2 and G3
were correctly coupled because panes need document-bound views and docks; G4 was correctly kept
independent; scripts and per-pane view instancing were correctly staged after the scene foundation.

## CROSS-CUTTING ISSUES

1. **`editor/` ownership is resolved.** Workspace state owns editor-only types, so the landed design
   correctly lives under `editor/` with surgical stock-file seams. Continue logging fork-owned
   upstream-file edits in the divergence ledger; do not move editor state into a runtime module to
   satisfy the obsolete stock-editor constraint.
2. **There is one implemented document hierarchy.** `EditorDocument` and its scene, script, help,
   shader, and generic-resource variants are the shared abstraction. Extend that append-only
   type/factory model rather than introducing a parallel `DocumentContext` tree.
3. **Focused-pane resolution remains the shared seam.** World, selection, toolbar, animation, and
   contextual commands must resolve through the focused pane/document. Background views may remain
   live, but editing commands are not broadcast.
4. **Per-pane views and core docks are landed.** The old singleton-rebind stage is historical for
   2D/3D views and Scene Tree/Inspector/Signals/Groups. Remaining global contextual editors are a G7
   migration decision, not evidence that the workspace still has only one live pane.
5. **Background-document liveness is resolved.** Open scene documents remain live. Revisit this only
   if measured memory/CPU behavior justifies a new lifecycle policy.
6. **Layout/session persistence is implemented but extensible.** Split geometry, pane ids, document
   assignments/tabs, drawer state, and focused-document restoration use the existing versioned
   workspace schema. Per-view ordinary 3D and 2D camera/canvas state across process restart
   remains a bounded extension (archived WP32 brief), with graceful fallback.
7. **Per-document routing is no longer blocked on a missing model.** New drop targets and tools must
   still prove that they target the focused pane's document. The current automated routes are green;
   focus, mouse ownership, and visual behavior remain part of the human audit and future feature
   fixtures.
8. **Generic resource tabs exist; generalized dirty/save semantics do not.** `ResourceDocument`
   tabs are implemented, but `EditorDocument::dirty` is unwired and uniform tab indicators,
   save/save-all, close prompts, and persistence for all applicable document kinds remain open
   lifecycle work.

## DECISIONS

**Resolved 2026-06-30 (logan):**

1. **[G1] Modify `editor/` — RESOLVED: refactor freely.** It's the user's own fork; prioritize the cleanest end-state architecture over upstream-rebase cost. Larger divergence from upstream Godot is accepted. New code still lives in self-contained subtrees where natural, but stock-file edits are not artificially constrained. DocumentContext lives under `editor/` (not `modules/`).
2. **[G1/G2] v1 side-by-side fidelity — RESOLVED, historical staging decision.** The temporary
   single-editor rebind stage allowed the foundation to land; G2 convergence subsequently delivered
   pane-owned `Node3DEditorView` and `CanvasView2D` surfaces. Editing/shortcut ownership remains
   focused-pane scoped.
3. **[G1/G5] Background-document liveness — RESOLVED: keep all documents live.** Every open
   scene document keeps its own world/view live; no suspension policy was introduced. This
   overrides G1 step 8's suspension consideration and makes G5 WIN B (lazy restore) optional rather
   than required. Memory/CPU scales with open-tab count — accepted; revisit only if measurement
   justifies it. All visible panes render their document, while editor gizmos/manipulation and
   shortcuts follow the focused pane.

**Resolved 2026-07-01 (logan) — product direction:**

6. **[G2] In-tab doc reordering is NOT a required feature.** Organization happens via panes + splits, not by dragging tabs within a pane. Keep tab reorder only if the `TabBar` gives it for free; don't build it.
7. **[G2] Splits must be resizable** (already true — `WorkspacePane` uses a draggable `SplitContainer`; keep it that way).
8. **[G3] Scene-tree + inspector are PER-PANE / in-the-tab, not global docks.** Each scene `DocumentView` grows to host its editing surface + its scene-tree + its inspector. This reinforces G3's per-pane-dock direction and is the intended end-state (not a rebound global dock).
9. **[G4] File system = a small bottom button that opens a slide-out drawer dock** (not per-pane).
10. **[G3] The stock dock drag-rearrange may be unnecessary entirely** once docks live in panes — revisit after G3; do not invest in preserving it prematurely.

**Resolved by the landed implementation (2026-07-16):**

11. **[G4] Drawer relationship — RESOLVED.** The center-column Explore drawer coexists with the
    global bottom panel and hosts FileSystem plus Import/file-backed resource details.
12. **[G2/G3] Pane ownership — RESOLVED.** Scene views own per-pane Scene Tree, Inspector,
    Signals, and Groups surfaces; script/help/shader/resource documents are workspace tabs.
13. **[G2] Legacy main screens — RESOLVED.** Singleton utility/plugin screens remain available
    through `ScreenHostDocument`; floating panes remain deferred.

**Still open:**
4. **[G5 — gating] Representative target project + cold-vs-warm:** what real project (size, asset count, scene complexity) reproduces the perceived 7s open, how many scenes are typically kept open on restore, and is 7s→3s measured cold or warm? **No optimization can begin without this** — the GDStudio 7s→3s figure is unverified hearsay (a goal, not a spec).

### Secondary decisions (recommendations noted; lower urgency)

- **[G7]** Which remaining contextual editors materially benefit from document ownership, and which
  should remain project-global but focus-bound? Migrate from observed multi-pane correctness needs,
  not wholesale symmetry.
- **[G5]** Should new benchmark marks ship permanently (`TOOLS_ENABLED`-gated) or live on a local
  profiling branch? Which rendering backend represents the target workload for shader-cache audits?
