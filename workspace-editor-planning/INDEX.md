# Workspace Editor — Planning Package

Open re-implementation of the closed-source **GDStudio editor** on a custom Godot 4.7 fork (`C:/Users/laspe/dev/work/godot`, branch `feature/workspace-editor`).

**End-state UX:** replace Godot's single-current-scene, single-main-screen (2D | 3D | Script | AssetLib) editor with a **dividable, tabbed, multi-document workspace** where multiple scenes, scripts, and resources are open and editable at once, each pane carrying its own contextual docks.

This package reconciles the per-goal plans into one buildable sequence. Each goal has its own file; read this INDEX first for ordering and cross-cutting decisions.

## The 5 goals

| ID | Goal | Effort | File |
|---|---|---|---|
| **G1** | Multiple scenes open & editable at once (**DocumentContext** + 3D world isolation) | XL | [G1-multiple-scenes.md](./G1-multiple-scenes.md) |
| **G2** | Dividable tabbed workspace replacing the 2D\|3D\|Script main-screen switcher | XL | [G2-dividable-workspace.md](./G2-dividable-workspace.md) |
| **G3** | Per-pane contextual docks (scene-tree + inspector, resource-as-inspector-tab, script methods) | L→XL | [G3-contextual-docks.md](./G3-contextual-docks.md) |
| **G4** | Bottom drawer slideout for file exploration with drag-drop into panes | M | [G4-bottom-drawer.md](./G4-bottom-drawer.md) |
| **G5** | Faster scene/resource open and editor startup (investigation + optimization) | M–L | [G5-faster-load.md](./G5-faster-load.md) |
| **G6** | Drag-a-tab-to-split compass overlay (landed) | M | [G6-drag-to-split.md](./G6-drag-to-split.md) |
| **G7** | Document contextual drawers (Animation + Level Materials migrated; AnimationTree binding hardened) | M | [G7-contextual-bottom-docks.md](./G7-contextual-bottom-docks.md) |

## Cross-cutting references

| Doc | Purpose |
|---|---|
| [ARCHITECTURE.md](./ARCHITECTURE.md) | The service / view-state / document-state taxonomy + seam rules — the tiebreaker for where any editor member belongs. |
| [DIVERGENCE-LEDGER.md](./DIVERGENCE-LEDGER.md) | Per-file record of stock-file edits (ours-by-design vs upstreamable) + the rebase-cadence policy. |
| [STEP5-CANVASVIEW2D-SCOPE.md](./STEP5-CANVASVIEW2D-SCOPE.md) | Scoped design for the per-pane 2D view (CanvasView2D): World2D-share finding, member classification, ⑤a/⑤b/⑤c phasing. The critical path to main-screen replacement. |
| [STEP5b3-SELECTION-SCOPE.md](./STEP5b3-SELECTION-SCOPE.md) | Scoped design for per-pane 2D editing (selection + manipulation, ~2,200 lines) + #6 per-document selection (Model A already works; Model B deferred). Includes the (A) push-through vs (B) bank-the-milestone decision. |
| [smoke/](./smoke/run_smoke.sh) | Headless regression harness (`bash workspace-editor-planning/smoke/run_smoke.sh`): open-3d / open-2d / restore-3-scenes, assert exit 0 + zero error-class lines. |

## Dependency graph

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

**Edge legend:**
- **G1 → G2 (hard):** G2 hosts one `DocumentContext` per tab/pane; the data model must exist first.
- **G1 → G3 (hard):** G3 dereferences `get_active_document()` and per-document scene_root/selection/history on every routed call site.
- **G2 ↔ G3 (interdependent):** G2 supplies the pane leaf + active-tab event that G3's dock host swaps on; G3's per-pane docks + per-document viewports are what make G2's *simultaneous* multi-pane rendering correct (G2 ships single-live-render until then).
- **G2 → G4 (soft):** G2 panes are the eventual per-pane drop targets and the layout tree G4's drawer state persists alongside. G4 works against the stock single main screen without G2.
- **G1/G2 → G5 (soft):** wins A/B/C (async open, lazy restore, lazy plugins) are cleanest expressed on `DocumentContext`; G5 prototypes on flat EditorData behind a migration seam. G5 has **no hard dependency** and its measurement phase can start immediately.

## RECOMMENDED BUILD ORDER

> Rationale up front: **G1's DocumentContext + the own-World3D isolation SPIKE is the foundation everything structural sits on**, and the SPIKE is a genuine make-or-break gate. G2 and G3 cannot deliver their real value (multiple simultaneously-editable scenes, per-pane docks) without it. G4 and G5 are far more independent and can run in parallel to de-risk the schedule and ship visible value early.

**Phase 0 — Gate (do before committing to the architecture):**
1. **G1 Step 1: own-World3D isolation SPIKE (throwaway).** Two own-World3D SubViewports rendering two scenes side-by-side, manually-routed gizmos, per-world picking, zero bleed. If this fails, the whole multi-live-3D vision changes — fall back to single-live-3D-pane and re-scope **before** any refactor. *This is the single highest-leverage thing to do first.*
2. **Resolve the editor/-stock constraint renegotiation** (see Open Decisions #1) — G1 cannot proceed cleanly until the user confirms editor/ may be modified.

**Phase 1 — Foundation:**
3. **G1 (full):** DocumentContext, EditorData → N live documents, EditorNode delegating accessors, single bound-document scenario/world helper, dock-rebind seam. Ship singleton-rebind v1 (one live 3D pane). This is the largest single piece of the project.

**Phase 2 — Build the workspace on the foundation (G2 + G3 are tightly coupled; build the tree first, then the docks, then converge on true multi-pane):**
4. **G2 slices 1–2:** WorkspaceManager + pane/split tree + 5-zone drop overlay hosting the existing singleton plugins via reparent-on-activate; scene 2D/3D toggle; layout persistence. (Scripts staged last.)
5. **G3 Phase 1:** per-document dock instances + accessor-repoint + ScriptMethodsDock + per-pane WorkspaceDockHost. This replaces G2's rebound-singleton shortcut.
6. **G2/G3 convergence:** per-pane Node3DEditor/CanvasItemEditor instancing + per-document World3D rendering → true simultaneous side-by-side (retires the G1 single-live-pane tech debt). **G2 slice 3:** scripts as first-class tabs (deepest sub-refactor, last).

**Parallel track (any time; minimal coupling) — start these early to ship value and de-risk:**
- **G4 (M):** bottom drawer. Ships against the stock single main screen immediately; per-pane drop *resolution* lands later once DocumentContext exists. Lowest-risk goal — drag/drop already works end to end.
- **G5 Phase 1 (S–M):** the mandatory measure-first gate (optimized build, real workload, benchmark instrumentation, baseline JSON). Can begin on day one — it is investigation, not refactor. Self-contained wins (icon cache D, thumbnail throttle E, import audit F) can land independently of the architecture. Sequence the structural wins (A async open, B lazy restore, C lazy plugins) alongside or just after G1's DocumentContext to avoid double-work.

**Why this order:** G1 is on the critical path and gates G2/G3; doing the SPIKE and the constraint decision before the refactor prevents a wasted XL effort. G4 and G5-Phase-1 carry no hard dependency, so they run in parallel to keep momentum and surface early wins while the foundation is built. The deepest, riskiest sub-refactors (scripts-as-tabs, per-pane editor de-singletonization) are deliberately sequenced last, after the workspace is proven for scenes/resources.

## CROSS-CUTTING ISSUES

1. **The "editor/ stays stock, custom code in modules/" constraint cannot survive this project.** DocumentContext owns editor-only types (SubViewport, EditorSelection) and EditorData/EditorNode/Node3DEditor must be edited directly; G2 replaces EditorMainScreen; G3 repoints stock dock accessors; G4 edits the EditorNode ctor. **Verified fact:** the fork *already* carries custom code under `editor/docks/`, so the posture does not hold today. **Mitigation pattern (all goals):** isolate new code in self-contained subtrees (`editor/editor_document_context.*`, `editor/workspace/`, `editor/gui/workspace_file_drawer.*`, `modules/<workspace-module>/`), keep stock-file edits surgical, well-commented, and concentrated behind single helpers, and track every editor/ change as a documented fork patch to minimize rebase cost.
2. **One DocumentContext abstraction, defined once.** G1, G2, and G3 all describe a Document/DocumentContext type. There must be exactly ONE base class (G1 owns it). G2 and G3 must consume G1's interface, not define parallel abstractions. The `Type` enum (SCENE_2D/3D/MIXED/SCRIPT/RESOURCE) must be defined by G1 even though G1 only implements scene documents, so script/resource tabs extend it.
3. **The single scenario/world/selection resolution helper is a shared seam.** G1 funnels all 3D scenario/world/space-state and dock/selection resolution through one bound-document helper + a per-viewport world member. G2's per-pane instancing, G3's per-pane docks, and G4's per-pane drop resolution all plug into that same seam. Keeping it as ONE method makes the v1→per-pane transition a mechanical substitution rather than a second rewrite — **do not scatter this logic.**
4. **Singleton-rebind v1 → per-pane instancing end-state is a recurring, deliberate tech-debt pattern.** G1 (docks/3D), G2 (docks), and G3 (docks) all ship a "rebind the singleton on activation" v1 and defer "one instance per pane" to a later step. This is intentional and consistent; the rebind logic lives in one method per subsystem so it can be swapped without touching call sites. The cost: only one fully-live pane at a time until convergence.
5. **Background-document liveness is unresolved and affects multiple goals.** Whether inactive documents keep running scripts/physics/live-edit (vs suspended/lazily realized) affects G1 (memory/CPU ceiling of N own-World3D SubViewports), G5 (WIN B lazy restore), and correctness of any tool depending on live state. Needs one consistent answer.
6. **Layout/session persistence spans goals.** G1 (`DocumentContext::serialize/deserialize`), G2 (versioned `[Workspace]` split-tree section), G3 (per-pane dock layout via EditorDock virtuals), and G4 (drawer open/height) must all serialize into a single coherent, versioned editor-layout format with graceful fallback to a default single-pane layout. Coordinate the schema once.
7. **Per-document drop routing is blocked on DocumentContext.** G4's per-pane DropTarget contract, G2's pane drop targets, and G1's per-document scene_root all converge: until DocumentContext lands, drops resolve to the global viewport. Design the contract now, gate implementation on G1.

## DECISIONS

**Resolved 2026-06-30 (logan):**

1. **[G1] Modify `editor/` — RESOLVED: refactor freely.** It's the user's own fork; prioritize the cleanest end-state architecture over upstream-rebase cost. Larger divergence from upstream Godot is accepted. New code still lives in self-contained subtrees where natural, but stock-file edits are not artificially constrained. DocumentContext lives under `editor/` (not `modules/`).
2. **[G1/G2] v1 side-by-side fidelity — RESOLVED: single live 3D pane first.** v1 binds one `Node3DEditor`/`CanvasItemEditor` to the focused pane (rebind-on-activate); true per-pane 3D-editor instancing is deferred to G2 convergence. This concerns active *editing/gizmos*, not rendering — see decision 3.
3. **[G1/G5] Background-document liveness — RESOLVED: keep all documents live.** Every open document keeps its own World3D SubViewport ticking scripts/physics/live-edit simultaneously; **no suspension in v1**. This *overrides* G1 step 8's suspension consideration and makes G5 WIN B (lazy restore) optional rather than required. Memory/CPU scales with open-tab count — accepted; revisit only if it becomes a problem. Net effect with decision 2: all panes *render* their own world live, but only the focused pane gets active editor gizmos/manipulation in v1.

**Resolved 2026-07-01 (logan) — product direction:**

6. **[G2] In-tab doc reordering is NOT a required feature.** Organization happens via panes + splits, not by dragging tabs within a pane. Keep tab reorder only if the `TabBar` gives it for free; don't build it.
7. **[G2] Splits must be resizable** (already true — `WorkspacePane` uses a draggable `SplitContainer`; keep it that way).
8. **[G3] Scene-tree + inspector are PER-PANE / in-the-tab, not global docks.** Each scene `DocumentView` grows to host its editing surface + its scene-tree + its inspector. This reinforces G3's per-pane-dock direction and is the intended end-state (not a rebound global dock).
9. **[G4] File system = a small bottom button that opens a slide-out drawer dock** (not per-pane).
10. **[G3] The stock dock drag-rearrange may be unnecessary entirely** once docks live in panes — revisit after G3; do not invest in preserving it prematurely.

**Still open (not blocking; decide when the goal starts):**
4. **[G5 — gating] Representative target project + cold-vs-warm:** what real project (size, asset count, scene complexity) reproduces the perceived 7s open, how many scenes are typically kept open on restore, and is 7s→3s measured cold or warm? **No optimization can begin without this** — the GDStudio 7s→3s figure is unverified hearsay (a goal, not a spec).
5. **[G4 — scope] Bottom drawer relationship to the existing bottom panel:** does the file drawer coexist with the current `bottom_panel` (Log/Audio), or eventually replace it? And does it span the center column only or the whole client area? (Recommendation: center column, FileSystem-only v1, coexist for now.)

### Secondary decisions (recommendations noted; lower urgency)

- **[G2/G3]** Per-pane dock containers in the first workspace milestone, or rebound-singleton acceptable? *(Rec: rebound-singleton for the first slice, per-pane in G3.)*
- **[G2]** How do AssetLib and the Game main-screen plugin map to the document model — singleton utility tabs or excluded? Are floating panes (WindowWrapper) in scope? *(Rec: defer floating panes.)*
- **[G3]** Do SignalsDock/GroupsDock/ImportDock become per-document or stay global-rebound for v1? *(Rec: global for v1.)*
- **[G5]** Should benchmark marks ship permanently (TOOLS_ENABLED-gated) or live on a local profiling branch? Which rendering backend (RD vs GLES3) for the shader-cache audit? Is the DocumentContext refactor greenlit before G5's A/B/C wins (decides M vs XL + accepted rework)?
