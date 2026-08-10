# Asset Fact Index — the shared foundation

**Status (2026-08-10):** Proposed. Nothing implemented. This is the umbrella contract that
[EXPLORE-PERFORMANCE-PLAN.md](./EXPLORE-PERFORMANCE-PLAN.md),
[GLTF-MATERIAL-OVERRIDES-DESIGN.md](./GLTF-MATERIAL-OVERRIDES-DESIGN.md), and the derived data
store proposals (`scrapline/docs/proposals/DERIVED_DATA_STORE-2026-08-09.md`,
`DERIVED_DATA_STORE_IMPLEMENTATION-2026-08-10.md`) each independently specified a piece of.
All engine line references verified against the current `master` worktree.

## 1. The one-sentence design

Per-asset facts are harvested **once**, where file I/O is already happening (the scan thread,
the reimport pass), stored on the scanner's existing per-file record, published as an
immutable generation-stamped snapshot, and **queried everywhere** — no UI surface, worker, or
audit ever opens a file to answer a per-row question.

This is not a new subsystem. `EditorFileSystemDirectory::FileInfo`
(`editor/file_system/editor_file_system.h:56-78`) already *is* an asset-registry record — it
carries type, UID, deps, import validity, and script class metadata, all populated on the
scan thread. Every pathology across the three source docs is a fact that should live there
but instead gets probed per-row at UI-build time (description presence, derived
classification) or looked up quadratically (type, via `_find_file`). The plan finishes the
pattern Godot started rather than building a parallel database.

## 2. Architecture — four layers

### Layer 1 — Harvested facts on the record

New fields on `FileInfo`, each with a defined harvester and invalidation trigger:

| Fact | Type | Harvested where | Invalidated by |
|---|---|---|---|
| `has_description` | bool | Scan cache-miss path (`editor_file_system.cpp:1442-1444` already parses `.import` there); reimport pass (**the value is already extracted and discarded today** — `:3031`, `:3064`); bounded text scan for non-imported assets, on the scan thread | file mtime change, `.import` change, reimport |
| `storage_class` | enum `AUTHORED / RETAINED_DERIVED / REGENERATED` | Computed from path root (`res://__derived/`, `.godot/derived/`); later refined from the store's slot registry / `manifest.cfg` | file move; manifest change |
| `badge_flags` | small bitset | Same `.import` parse (e.g. "has material overrides" from `_subresources/materials`) | `.import` change, reimport |

Deliberately a **fixed field set, not a provider/plugin system**. Each fact has one harvester
and one invalidation story; adding a fact later is one field plus one harvest site. Generic
extensibility is exactly the speculative machinery the store proposal's decision record keeps
striking down.

Facts must also ride the serialized scan cache (`FileCache`, cache-hit copy at
`editor_file_system.cpp:1396-1407`) or they'd be recomputed — with I/O — on every editor
start. That means extending the cache line format and bumping `CACHE_FILE_NAME`
(`"filesystem_cache10"`, `:61`) to a fork-suffixed name so upstream and fork caches never
cross-contaminate.

**Honest carve-out:** the fragment of description probing that consults open-scene editor
state (`_get_open_scene_root`, `editor_asset_description.cpp:427-431`) cannot move to the
scan thread. It remains a main-thread *overlay* applied at row-build time — the record
answers the file-based question; the dock adjusts for the handful of open scenes. That is
O(open scenes), not O(rows), and does no I/O.

### Layer 2 — Immutable snapshot publication

At scan completion — the exact point where the model swap already happens
(`editor_file_system.cpp:1235`, before `filesystem_changed` at `:1245`) — additionally build
and publish an immutable flat snapshot: a vector of records plus path, UID, and
**lowercase-filename** indexes, tagged with a monotonic generation. Main-thread consumers may
keep reading the live tree; **workers read only the snapshot**. This is simultaneously:

- the Explore plan's fix-6 seam (search worker → generation-stamped ResultBatch → stale
  generations discarded), including its "smaller intermediate step" (the lowercase filename
  index) for free;
- the query surface for the derived-data audit, size report, and any future off-thread
  consumer;
- the resolution of the hard constraint that the live `EditorFileSystemDirectory` tree is
  swapped during scan publication and cannot be traversed off-thread.

### Layer 3 — Per-surface classification policy

A thin, stateless, I/O-free policy module: pure predicates over layer-1 facts answering
"does this surface show this file." Implements the store decision's §11.6 split exactly:

- **Browse surfaces** (dock tree, directory picker, unfiltered quick-open) hide
  `RETAINED_DERIVED`.
- **Type-filtered pick surfaces** (resource picker Load / Quick Load with a type filter)
  show everything.
- **Export, dependency inspection, and management UI** never consult the policy at all —
  they walk the model directly, so shipping and dependency tracking are structurally
  unaffected (the store doc's §10.7 requirement, honored by construction rather than by
  discipline).

This is presentation policy, not filesystem semantics — no `hidden` bit on
`EditorFileSystemDirectory` that a future consumer could accidentally honor.

### Layer 4 — Writers register facts, never predicates

The derived store's `manifest.cfg` and slot registry, and the material-override import
metadata, are fact *sources* the harvester reads. No feature gets its own probe, its own
cache, or its own dock patch. One channel in (harvest at scan/import/bake time), one channel
out (record/snapshot queries).

**What the index deliberately does not own:** authored state. Override maps stay in
`.import`; bake linkage stays in typed properties; the index holds only derived, regenerable
facts — safe to throw away and rebuild from a rescan, which is precisely why it can be
published immutably and queried off-thread.

## 3. Implementation plan, A→Z

Each step is independently landable, verified before the next, and logged in
[DIVERGENCE-LEDGER.md](./DIVERGENCE-LEDGER.md). Engine changes land on **both trees**
(godot-box3d master and this fork) per the standing two-tree rule. Steps A–D are the
foundation; E–G are the measured architectural tier; H–J land with their consuming features.

- **A. Land the independent Explore quick fixes** (perf plan fixes 1, 2, 4: the `||`
  reorder + recorded scene-row types at `filesystem_dock.cpp:1231`/`1549`, category-rebuild
  dedup, generic-preview bounding). No dependency on the index; recovers most perceived lag;
  establishes the baseline for the step-F measurement.
- **B. Extend the record and cache.** Add the three fact fields to `FileInfo` and
  `FileCache`; extend the cache serialization (`_save_filesystem_cache` at
  `editor_file_system.cpp:2025`, load at `:494`); bump the cache filename. Fields default to
  "unknown/false" — behavior identical until harvesters land. Verify: clean scan and warm
  restart produce identical trees; upstream diff is purely additive.
- **C. Harvest `has_description`.** Three sites: (1) scan cache-miss for imported files —
  the `.import` parse at `:1442-1444`; (2) the reimport pass — the value is already read at
  `:3064`, store it instead of dropping it; (3) non-imported text assets — port
  `EditorAssetDescription::has_description_bounded`'s file-reading half to the scan thread,
  bounded by `MAX_TEXT_SCAN_BYTES`, cache-miss only. Verify: cold scan of the full project;
  fact matches the old probe's answer on a sampled set.
- **D. Dock consumes the record.** `_update_file_list` and the tree builder read
  `fi->has_description` plus the open-scene overlay; delete `description_cache` and the
  per-row `_asset_has_description` I/O path (perf plan fix 3 done). Verify: cold-cache
  navigation into a 10k-file directory does zero file opens from the dock (procmon or
  instrumented count).
- **E. Snapshot publication.** Immutable `AssetSnapshot` built at `:1235`; generation
  counter; path/UID/lowercase-name indexes. No consumer yet — verify construction cost is
  negligible relative to the scan it rides on, and that incremental rescans republish
  correctly.
- **F. Measure (the G5 gate).** Against the post-A/D baseline: search latency on the full
  project, worst directory open, scroll cost. **If search is now acceptable, stop here** —
  steps G's worker is the expensive tier and exists only if the numbers demand it.
- **G. Search worker.** Query thread reads the snapshot, token-matches, sorts, classifies;
  emits generation-stamped ResultBatch; main thread discards stale generations and applies
  ~100–250 rows/frame (perf plan fixes 5–6). Typing a query no longer touches
  `filesystem_changed` at all. Verify: type `a` then rapidly `asset` — no stale rows, no
  frame > budget.
- **H. `storage_class` fact.** Path-root computation first (needs only the two root
  constants); manifest/slot-registry refinement when the store's Phase 4 allocator exists.
  Sequenced with the store's own build order — see the traceability note on its §11.8
  step 7.
- **I. Policy layer + surface wiring.** The layer-3 predicates, consumed by
  `filesystem_dock.cpp` (`_create_tree`, `_update_file_list`, `_search`),
  `editor_quick_open_dialog.cpp`, `editor_dir_dialog.cpp`. Lands **with** the store's hiding
  phase, which its build order deliberately places *after* the size report exists (hiding
  large binaries without a size signal makes accumulation worse). Verify: the §11.6 matrix —
  Quick Load with type filter finds a derived resource; dock browse does not show it; export
  includes it.
- **J. `badge_flags`.** When material-overrides Phase 1 needs an Explore/inspector badge,
  harvest it from the same `.import` parse. Not before.

**Costs accepted:** one additive divergence in `editor_file_system.{h,cpp}` (the cheapest
shape for the two-tree cherry-pick, and one coherent ledger entry instead of three features
each patching the scanner); a cache-format fork; scan-thread I/O for non-imported
description scans on cache-miss only (bounded, off-main-thread, amortized by the cache).

## 4. Traceability — every concern in the three docs, and what this plan does about it

Dispositions: **SOLVED** (this plan directly fixes it) · **ENABLED** (this plan provides the
seam; the fix itself is separate, named work) · **UNAFFECTED** (orthogonal; intentionally not
touched) · **OUT OF SCOPE** (belongs to another design; this plan deliberately refuses it).

### 4.1 EXPLORE-PERFORMANCE-PLAN.md

| Concern | Disposition | How |
|---|---|---|
| #1 O(N²) scene-preview type scan (incl. per-scroll-frame re-run) | **SOLVED** — step A | Fix 1a/1b land unchanged as step A; independent of the index. The snapshot additionally makes any future type lookup O(1), removing the temptation to call `get_file_type` per row ever again. |
| #2 Full ItemList rebuild on every update | **ENABLED** — steps E/G | The rebuild itself is dock-side work (perf plan fixes 2/5). The snapshot + batched ResultBatch application (step G) is what makes *incremental* population possible; without it, incrementalism has no stable data source. Rebuild-on-navigation churn is step A (fix 2). |
| #3 Synchronous description probing (cold-cache file I/O per row) | **SOLVED** — steps C/D | The fact is harvested on the scan thread (the reimport path already reads it and throws it away today, `editor_file_system.cpp:3064`); the dock reads a bool. `description_cache` and its invalidation logic are deleted, not relocated. |
| #4 Global search: full traversal per keystroke + sort + full result build | **SOLVED (gated)** — steps E/F/G | Lowercase-name index (E) makes matching cheap; the worker (G) moves traversal+sort off-thread; batched application caps row-build cost. Explicitly gated on the step-F measurement per the plan's own G5 discipline. |
| #5 Unbounded generic preview queueing | **UNAFFECTED** — step A | Purely dock-side (fix 4, visible-only + dedup). The index has nothing to add and does not pretend to. |
| #6 Duplicate/triplicate category rebuilds | **UNAFFECTED** — step A | Dock-side control flow (fix 2). Included in step A only for sequencing. |
| Hard constraint: Control/TreeItem/ItemList mutation is main-thread-only | **SOLVED (respected)** | The architecture never proposes moving GUI work; workers produce data batches, the main thread applies bounded row counts per frame. |
| Hard constraint: live `EditorFileSystemDirectory` tree is swapped during scan publication | **SOLVED** — step E | The immutable snapshot is precisely the answer: workers never see the live tree. |
| Hard constraint: description probe consults open-scene state | **SOLVED (carve-out)** — step D | Split: file-based answer harvested; open-scene fragment stays as an O(open scenes) main-thread overlay. |
| `filesystem_changed` is the wrong trigger for search | **SOLVED** — step G | Queries get their own publication path (generation-stamped ResultBatch); the signal remains the trigger only for model changes. |
| Type-`a`-then-`asset` stale-result race | **SOLVED** — step G | Generation stamping; stale generations discarded on arrival. |
| Moving `_update_file_list` wholesale to a worker would race/crash | **SOLVED (respected)** | The plan never moves it; it shrinks what it does (reads facts, applies batches). |

### 4.2 Derived data store (DERIVED_DATA_STORE-2026-08-09.md §10–§11, IMPLEMENTATION-2026-08-10.md)

| Concern | Disposition | How |
|---|---|---|
| Hiding derived artifacts requires per-row classification with no per-row I/O | **SOLVED** — steps H/I | `storage_class` is a harvested fact; the hiding predicate is a pure function over it. The exact coupling both docs flagged. |
| §10.7: hiding is presentation, not a semantic `hidden` flag on the directory tree | **SOLVED** — layer 3 | Policy predicates live outside the model; no consumer can "accidentally honor" a flag that doesn't exist on the tree. (The older §3.2 `hidden`-flag sketch was already superseded by §10.7; this plan sides with §10.7.) |
| §11.6: per-surface split — browse hides, type-filtered pick shows, export/deps see all | **SOLVED** — step I | The policy layer is keyed by surface intent; the verification for step I is literally the §11.6 matrix. |
| Export and dependency tracking must be structurally unaffected | **SOLVED** — layer 3 | Export/dependency code never calls the policy; nothing to get wrong. |
| Option-5 warning: hiding large binaries without a size signal worsens accumulation | **SOLVED (sequencing preserved)** — step I | Hiding lands with the store's hiding phase, which its own build order places after the size report. This plan does not let the index become a way to hide things earlier. |
| Audit (impl. Phase 1) must run before fork work, as project-side scene-text parsing (A5) | **UNAFFECTED / later ENABLED** | The audit's sequencing stands — it needs no index. Once snapshots exist (step E), the audit's plumbing *may* migrate from text-parsing to snapshot queries (deps are on the record), but that is an offered improvement, not a dependency in either direction. |
| Size report / *Clean Unreferenced Artifacts* | **ENABLED** — steps E/H | `storage_class` + snapshot enumeration give both operations a cheap, complete input. The operations themselves are store work. |
| Identity: scene UID + owner ID chain + producer slot | **OUT OF SCOPE** | Owned by the store (§11.4). The index never participates in identity; it only reads manifests the allocator writes. |
| §11.5 rebake rule (in-place/UID-preserving; fork + repoint on mismatch) | **OUT OF SCOPE** | Allocator behavior. The index's only contact: a rebake that moves a bundle invalidates that path's facts via the normal file-change path. |
| A8 copy/paste contract, `owns()`/`describe()`, GC reference-driven | **OUT OF SCOPE** | Store singleton's API. Note the boundary A8 already draws — "nothing outside the singleton parses `manifest.cfg`" — the index's manifest-informed `storage_class` refinement (step H) must therefore go through `describe()`, not parse manifests itself. |
| Slot registry as single source of truth (A1) | **SOLVED (respected)** — step H | The index reads classification from the registry/manifest channel; it never grows a second opinion about a slot's class. |
| §11.7 "snappy": editor stalls from `reimport_files()` blocking behind a modal | **OUT OF SCOPE (explicitly)** | Same position as the store doc: separate work. The index improves *query-time* stalls (dock, search), not import-time stalls. Do not expect step G to fix bake/import blocking. |
| Naming drift / orphaned bakes (the two live defects) | **OUT OF SCOPE** | Cured by the allocator and audit, not by facts. The index only makes the resulting artifacts classifiable and hideable. |

### 4.3 GLTF-MATERIAL-OVERRIDES-DESIGN.md

| Concern | Disposition | How |
|---|---|---|
| Import-UI badges/indicators on Explore rows must not do file I/O at list-build time | **SOLVED** — step J | `badge_flags` harvested from the same `.import` parse; the dock reads a bitset. This is the doc's only direct demand on Explore, and the index is its designated seam. |
| Material identity (locators, signatures, rename/reorder reconciliation, unmatched state) | **OUT OF SCOPE** | Authored-state design, owned by the overrides doc (Phase 0). The index shares only the *discipline*: durable IDs are identity, names are decoration. |
| Override schema, membership vs value, UndoRedo tuple contract, proxy `_get`/`_set` | **OUT OF SCOPE** | Editor-dialog machinery; no contact with the index. |
| Authored vs derived distinction — "do not store authoritative user edits in a disposable cache" | **SOLVED (respected)** — layer 4 | The index holds *only* derived, rescan-regenerable facts. Override maps stay in `.import`; nothing authored ever migrates into the index, so the doc's core fear (authored intent in a throwaway store) cannot occur here. |
| Required Derived Resources contract (authority, UID allocation, owner keys, atomicity, GC, export policy, cache-clear behavior) | **OUT OF SCOPE** | The store answers this (its §11 largely does). The index is a *consumer* of the resulting classification, not a party to the contract. |
| Texture references by UID; missing-resource states | **OUT OF SCOPE** | Overrides/store concern. The snapshot's UID index (step E) incidentally makes UID→path resolution cheap for any future diagnostics UI. |
| Reimport-safe persistence via `_subresources` seam | **UNAFFECTED** | The index reads `.import` after any reimport; badge facts refresh through the normal invalidation path. No new persistence introduced. |
| `.godot` cache cleared → rebuild derived outputs | **SOLVED (respected)** | The index's own data is exactly as disposable: wiping the filesystem cache costs one cold rescan, nothing else. Consistent with the doc's derived-state definition. |

### 4.4 What remains unsolved by anything in this plan (stated plainly)

1. **Import/bake-time main-thread blocking** (`reimport_files()` modal) — the other half of
   "snappy," tracked separately per the store doc's §11.7.
2. **ItemList rebuild churn on navigation** beyond the dedup fix — a true incremental
   diff-apply for the dock is possible atop the snapshot but is not scheduled anywhere; fix 2
   + step G batching are expected to make it unnecessary.
3. **Everything authored:** material identity, override lifecycles, bake ownership, rebake
   semantics. Three designs own those; this plan's contribution is refusing to duplicate any
   of it.

## 5. Relationship to other docs

- [EXPLORE-PERFORMANCE-PLAN.md](./EXPLORE-PERFORMANCE-PLAN.md) — fixes 3, 5, 6 are steps
  C/D, G, E of this plan; fixes 1, 2, 4 are step A, unchanged.
- Derived data store proposals — steps H/I implement the hiding/classification surface its
  §11.6 specifies, on its schedule (after size report). The index reads the store's slot
  registry and `describe()`; it writes nothing the store owns.
- [GLTF-MATERIAL-OVERRIDES-DESIGN.md](./GLTF-MATERIAL-OVERRIDES-DESIGN.md) — step J is its
  badge seam; the authored/derived boundary in §2 layer 4 is its "not a disposable cache"
  requirement made structural.
- [G5-faster-load.md](./G5-faster-load.md) — step F is the same measure-first gate.
- [DIVERGENCE-LEDGER.md](./DIVERGENCE-LEDGER.md) — one coherent entry for the
  `editor_file_system.{h,cpp}` extension when steps B+ land.
