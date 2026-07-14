# Tool foundations: synthesis of the per-tool plans

**Status:** finalized 2026-07-13, synthesized from the eleven plans in `tools/` (one per tool,
each with ≥3 difficulties, one chosen method each, and open-source/article references).
**Read order for implementers:** `PLAN.md` → this file → the relevant `tools/NN-*.md`.
This file is the tiebreaker where tool plans disagree; conflicts resolved here are marked ★.

---

## 1. The shape: eight shared services, tools as thin state machines

Every tool decomposed into the same substrate. Build these once, in this dependency order;
no tool ships code that duplicates a service.

| # | Service | Contents (from the plans) | Consumed by |
|---|---------|---------------------------|-------------|
| S1 | **Modal tool framework + ToolOverlay** | Tool state machines; ghost previews live ONLY in a RenderingServer overlay and are architecturally unable to touch the kernel pre-commit; one `exit_gesture()` path shared by commit/Escape/deactivate; drag plane + basis frozen once at gesture start, never re-derived per event; Enter/Escape universal. | all viewport tools (01) |
| S2 | **Kernel transaction API** | RAII `TransactionScope`: begin → atomic ops → commit(diff) / rollback; previews that need real kernel state use transaction+rollback; failed preconditions roll back automatically = reject-don't-corrupt is free. Diffs are the only mutation record (undo). | every operator (05, 06, 03) |
| S3 | **Topology/adjacency service** | Cached, lazily-rebuilt adjacency (edge→faces, vert→edges/faces, face→edges in loop order); quad-gated loop/ring walks (n-gons + non-4-valence verts are hard terminators — Blender's rule); region-rim rule = per-edge selected-adjacent-face count (uniform for closed/boundary/donut regions); link-condition validator gating collapses/welds; coplanar flood fill. | selection, extrude, edge ops, hotspot islands (02, 04, 05) |
| S4 | **Picking service** | Broad phase = the fork's existing world-scoped gizmo BVH (`630785ab`); narrow phase = per-block *element* BVH inside `LevelMesh` (kernel, not physics, not GPU back-buffer — geometric picking keeps checks headless); screen-space vert/edge tolerance + depth reject; **generation-stamped stable selection handles** revalidated O(changed) from diffs (kills the Cyclops stale-handle crash class). | selection, drag-align raycasts (02, 03) |
| S5 | **Overlay/highlight rendering** | Per-world gizmo cull layer (existing per-scenario mask allocator); dirty-only highlight rebuild on selection/topology change. | selection, all tools (02) |
| S6 | **UV frame algebra** | Per-face projection frame (see §2 storage contract); hinge-unfold operator (rotate B coplanar about the shared edge; COPY shared-edge UVs, don't re-project — bit-exact seams); closed-form 2-point complex-similarity solver for wrap/flow; `reconcile_face_uv` — the ONE kernel post-step every geometry op calls; texture lock lives here and nowhere else. | unwraps, wrap/flow/lift, hotspot, transform lock (08, 09, 10, 03) |
| S7 | **Snap service** | Always snap the **delta** in world space (never the absolute point, never coupled to Local/World gizmo basis — the UE5 bug); dual rule: distance-snapped delta + grid-plane vertex snap (TrenchBroom); grid-tied epsilon that S8 reuses; axis-keyed tangent table for surface-snapped creation grids (stable, no flicker). | all tools (03, 01, 06) |
| S8 | **Robust geometry** | `GeomPredicates`: thick-plane snap-to-plane classification with grid-tied epsilon, exact orient3d only as sign tie-breaker; `PolyTriangulator` (ear-clip + hole bridging); `FaceTriangulator` (Manifold packing, all tris of a face share one faceID); `CoplanarFaceMerger` (boolean n-gon reconstruction); spatial hash (weld). | clip/mirror, booleans, bake, weld (06, 07, 05) |

## 2. ★ The UV storage contract (resolves 08 vs 09 vs 10)

Plans 08, 09, 10 each proposed a storage variant. Reconciled contract — this supersedes the
sketches in all three and amends PLAN.md's data model:

**Per-face columns:** `uv_mode : {PROJECTED, EXPLICIT}` · `uv_origin : Vector3` ·
`uv_tangent : Vector3` (bitangent derived live from current normal — only the discontinuous
part of the frame is frozen, per 09) · `uv_transform : Transform2D` (valid when PROJECTED).
**Per-loop column:** `uv0` — **always materialized, always what the baker reads** (08's D7
rule: output never depends on editor-side recipes).

- PROJECTED = the affine recipe (frame + transform) is authoritative; loop UVs are
  re-materialized from it by `reconcile_face_uv` after any geometry change. This IS texture
  lock: rigid face motion → exact frame transform; asymmetric vertex drags → least-squares
  affine refit with a condition-number guard that freezes rather than blows up (09/03).
- EXPLICIT = per-loop UVs are authoritative (conforming / follow-active-quads unwraps, and
  hotspot's non-affine modes). No recipe stored.
- Face-texturing ops (wrap/flow/justify/fit/align) REQUIRE PROJECTED; applying one to an
  EXPLICIT face first normalizes it to PROJECTED (09's asymmetric read/write rule). Reads
  from EXPLICIT neighbors work unmodified.
- "Align to Grid" and "Align to Face" are just the two population methods for the same frame
  fields — world-aligned vs face-local is a false dichotomy (09).
- 10's `uv_mode_flag` = this `uv_mode`; its "dual storage" folds into this contract.

## 3. Cross-tool patterns (pre-decided, binding)

1. **Reject-don't-corrupt is a mechanism, not a discipline** — precondition validators
   (manifold/link-condition/degeneracy) + S2 rollback. Fuzz checks assert rejection, not
   absence of attempts.
2. **Determinism everywhere**: geometric picking (headless-capable), seeded tie-breaks,
   lowest-face-id BFS seeds, sticky-by-patch-name hotspot re-fits with per-island
   vertex-hash seeds — editor and headless runs must be bit-identical.
3. **Identity discipline**: RID/path keys never names (Scythe's collision bug); generation-
   stamped element handles; normalized 0..1 patch rects (texture-resize-proof).
4. **Attribute inheritance standard**: new wall/side faces inherit material+smoothing from
   the single rim-owning face and get FRESH polygroup ids; `uv_transform` is re-projected,
   never copied, EXCEPT exact-subregion cases (boolean survivors, face splits) where
   verbatim copy is provably correct (04, 05, 07).
5. **Preview/commit split**: ghosts in overlays (S1) or rolled-back transactions (S2); undo
   stack only ever sees committed diffs; selection changes are never undo steps.
6. **Deliberate v1 scope cuts that make hard ops tractable**: bevel = single-segment,
   grid-quantized, edges-only (corner topology becomes enumerable — 05); clip/mirror scoped
   to faces/polygroups/objects (Scythe's crash surface, made an explicit rule — 06);
   booleans keep one multi-shell block by default, `Decompose()` split is explicit (07).

## 4. Pre-decision table (the headstart)

| Tool (plan) | Hard piece | Decided method |
|---|---|---|
| Block (01) | drag plane | freeze plane+basis at gesture start |
| | surface snap | hit normal → nearest world axis; axis-keyed tangent table; OBB fallback for slopes |
| | degenerate drags | `has_volume()` guards + `default_block_height` fallback, re-arm on collapse |
| Selection (02) | vert/edge picking | geometric element-BVH + depth reject (NOT GPU back-buffer) |
| | loops/rings on n-gons | quad-gated walk, n-gon/pole = terminator |
| | stale handles | generation stamps + O(changed) diff revalidation |
| Transform (03) | pivot for sub-objects | Blender pivot taxonomy + MMB temp pivot auto-reset on selection change |
| | snapping | snap the world-space delta; dual rule; never basis-coupled |
| | drag-align | mid-drag BVH raycast with dragged-face-id exclusion |
| | texture lock | face-tangent-basis solve (NOT dominant-axis approx); exact for rigid, LSQ + condition guard otherwise |
| Extrude (04) | region rim | per-edge selected-face count; ambiguous non-manifold edges = reject |
| | winding | locally from each rim edge's owning face; never averaged normals |
| | inset corners | angle-bisector miter + even-offset term + miter-limit clamp |
| Edge ops (05) | bevel | v1 restricted: single-segment, grid-quantized width, edges only; enumerable corner patches |
| | loop insert | quad walk, pole termination, partial-loop commit |
| | bridge | arc-length correspondence + best-rotation twist search |
| | weld | spatial-hash clusters gated by link condition |
| Clip/Mirror (06) | plane split | thick-plane classification, grid-tied epsilon, orient3d tie-break |
| | capping | 2D loop extraction + ear-clip with hole bridging; n-gon fast path |
| | preview | plane quad + intersection polyline live; real split only at commit |
| Booleans (07) | attribute round-trip | key on Manifold `(runOriginalID, faceID)`; survivors copy `uv_transform` verbatim |
| | watertight guard | detect + abort + highlight open edges; never auto-close |
| | n-gon rebuild | group-by-key + boundary trace + collinear collapse (key IS the merge partition) |
| | ⚠ spikes before LE5 | (a) `faceID` survives `BatchBoolean`? (b) which operand owns new cut faces? |
| Unwrap (08) | conforming | BFS hinge-unfold, distortion-threshold splits, cycle-closure seam cuts, lowest-id seed |
| | follow quads | quad-restricted grid walk, clean termination at non-quads; Length/Even/Avg spacing |
| | overlay editor | tool-owned Control at view-state layer (not a second pane) |
| Face texturing (09) | wrap | closed-form 2-point complex-similarity solve across the hinge |
| | flow | pairwise propagation of the same solver (twist-free by construction) |
| | justify/fit | pure UV-space coordinates; never world/screen axes |
| Hotspot (10) | scoring | log2 texel-density error as hard primary key + octave margin; aspect secondary; seeded random tertiary; fit-debug overlay |
| | L-shaped runs | developable test → unwind-to-strip + arc-length per-face split |
| | re-fit stability | sticky-by-patch-name; per-island vertex-hash seed |
| Material browser (11) | trim thumbnails | private preview queue + own disk-cache namespace (a 2nd registered generator would hijack ALL editor material previews — first-match wins) |
| | scale | virtualized grid + EditorFileSystem signal-driven incremental index |
| | blockout materials | editor-bundled PCK under reserved builtin path, project-overridable |

## 5. Schema/plan deltas folded back into PLAN.md

- Kernel per-face columns gain `uv_mode`, `uv_origin`, `uv_tangent` (§2 above).
- `reconcile_face_uv` added to the kernel op contract (every topology/position op ends with it).
- Two Manifold spikes are LE5 entry criteria (07).
- Blockout quick slots are `Shift+Alt+1..0` (PLAN.md §3 supersedes the older 1..6 note).
- Hotkey note: our `L`=loop `X`=ring vs Alyx's `L`/`G` — keep ours, revisit in keymap pass (02).
