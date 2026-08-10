# Explore (FileSystem) Dock Performance Plan

**Status (2026-08-10):** Analysis complete and code-verified; nothing implemented. All line
references are against the current `master` worktree and were confirmed by direct inspection,
not carried over from the original agent report.

## The problem

Opening the Explore drawer on a large directory, searching a large project, or simply scrolling
the file list stutters the whole editor. The filesystem *scanner* is not the culprit — it already
runs on its own low-priority thread. The lag comes from what Explore does **after** the scanner
publishes its model: querying, sorting, description probing, and constructing every GUI row
synchronously on the main thread, including one quadratic lookup path that can re-run every frame
while scrolling.

## Verified cost centers (ranked)

| # | Cost center | Impact |
|---|---|---|
| 1 | Quadratic scene-preview type scan | Highest for large directories; re-runs per frame while scrolling |
| 2 | Full ItemList rebuild + layout on every update | High |
| 3 | Synchronous description probing (file I/O per row on cold cache) | High on cold caches |
| 4 | Global project search + sort + full result build | High for large projects |
| 5 | Unbounded generic preview queueing | Sustained background/UI churn |
| 6 | Duplicate/triplicate rebuilds when clearing category state | Moderate, but free to fix |

### 1. The O(N²) scene-preview scan

`FileSystemDock::_update_visible_scene_previews` (`editor/docks/filesystem_dock.cpp:1208`)
loops over **every** row in the ItemList and calls
`EditorFileSystem::get_file_type(path)`, which resolves the path and then linearly scans the
directory's file array (`editor/file_system/editor_file_system.cpp:2138-2150`). For one flat
directory of N files that is ~N²/2 filename comparisons — about 50 million at 10,000 files.

Two aggravating details found during verification:

- **The visibility check does not shield the expensive part.** In the condition at
  `filesystem_dock.cpp:1231`, the `get_item_rect(i).intersects(visible_rect)` test is the *last*
  clause of the `||` chain, so the linear `get_file_type` lookup runs for every non-folder row
  regardless of visibility. The "visible rows only" intent of the function protects the preview
  *requests*, not the classification pass.
- **It runs far more often than "on show".** It is queued by `NOTIFICATION_VISIBILITY_CHANGED`
  (`filesystem_dock.cpp:793`), by list `resized`, and by every scroll-bar `value_changed`
  (`filesystem_dock.cpp:6152-6154`). The deferred queue coalesces to once per frame — but that
  still means the full quadratic pass can execute **every frame while scrolling** a large
  directory. This matches the perceived scroll stutter better than open-time cost alone.

### 2. Full ItemList rebuild

`_update_file_list` (`filesystem_dock.cpp:1358`) clears and reconstructs the entire ItemList on
every update — navigation, search keystroke (debounced), filesystem change, display-mode change.
Every row pays icon selection, `add_item`, metadata set, description probe (#3), and preview
queue (#5).

### 3. Synchronous description probing

`_asset_has_description` (`filesystem_dock.cpp:1805`) is called per row during list build
(`:1610`) and per tree item. Results are cached in `description_cache`, but a **cold cache does
real file I/O per row**: `EditorAssetDescription::has_description_bounded`
(`editor/file_system/editor_asset_description.cpp:410-440`) loads the `.import` ConfigFile for
imported assets, or reads up to `MAX_TEXT_SCAN_BYTES` of scene/resource text. A 10k-row search
result on a cold cache is ~10k file opens on the main thread.

### 4. Global search

`_search` (`filesystem_dock.cpp:1320`) recursively walks the whole project model per (debounced)
keystroke, token-matching every filename. It caps at 10,000 **results**, not traversal work —
and the real cost is downstream: up to 10k rows each paying #2/#3/#5, plus
`sort_file_info_list` over the full match list.

### 5. Unbounded generic preview queueing

The *scene*-preview path is already well-behaved: visible-only, deduplicated, with stale-request
cancellation (`_update_visible_scene_previews` / `visible_scene_preview_requests`). The problem
is the **generic** path: `_update_file_list` fires `queue_resource_preview` for every non-broken
row at `filesystem_dock.cpp:1620`, visible or not, with no dedup against a rebuild that just
re-queued the same paths.

### 6. Duplicate category rebuilds

`navigate_to_path` with a color-category filter active (`filesystem_dock.cpp:1142-1153`) runs
`_end_category_filter` → `_rebuild_category_rail` → `_update_color_filter_view`. Because
`_end_category_filter` sets `category_restore_pending`, `_update_color_filter_view`
(`:4347-4359`) calls `_update_display_mode(true)` **and then** rebuilds the tree and file list
again for the restore — after which `_navigate_to_path` rebuilds the file list a third time.

## Why this work is on the main thread (and what actually has to stay there)

Current flow:

```
scan thread                     main thread
───────────                     ───────────
builds new filesystem model  →  swaps model (editor_file_system.cpp:1235)
                                emits filesystem_changed (:1245)
                                FileSystemDock::_fs_changed (filesystem_dock.cpp:1998)
                                  → update_all()
                                    → search / sort / classify
                                    → description probes (file I/O)
                                    → construct every row
                                    → layout + visible-preview detection
```

The scanner threads only discovery/import. Explore's derived view — filtering, sorting,
classification, presentation — is synchronous because the dock is an older immediate-mode
implementation that traverses the editor model and builds controls in one call.

Hard main-thread constraints (cannot move):

- `Control` / `TreeItem` / `ItemList` mutation, text shaping, selection, and layout are not
  thread-safe.
- The live `EditorFileSystemDirectory` tree is swapped and mutated during scan publication; a
  worker cannot traverse it without a snapshot or immutable index.
- Part of the description probe consults open-scene editor state
  (`_get_open_scene_root`, `editor_asset_description.cpp:427-431`).

Everything else — token matching, type filtering, category membership, sorting, description
sidecar reads, preview eligibility — is pure computation over data and *could* move to a worker,
given a snapshot. Note that `filesystem_changed` is the wrong trigger for search: typing a query
does not change the filesystem. A query needs its own publication path.

Target architecture (the eventual fix #5 shape):

```
filesystem scanner → publish immutable FileRecord snapshot/index
search worker      → query snapshot → sort/classify → publish generation-stamped ResultBatch
main thread        → discard stale generations → apply ~100-250 rows per frame
```

Generation stamping also fixes the type-`a`-then-`asset` race: stale query results are simply
discarded. Moving `_update_file_list` wholesale to a worker is **not** viable — it mixes pure
computation, filesystem I/O, editor state, and GUI mutation in one function and would race or
crash.

## Fix plan (ordered)

1. **Kill the O(N²) scan — two parts, both cheap.**
   a. Reorder the `||` chain at `filesystem_dock.cpp:1231` so the `get_item_rect` visibility test
      runs before `get_file_type`. Near-free; immediately fixes the per-scroll-frame worst case.
   b. Eliminate the lookup entirely: `_update_file_list` already holds `file_info.type` when it
      builds each row (`:1549`) — record scene-row indices (or the type) during construction and
      have `_update_visible_scene_previews` consult that instead of `get_file_type`.
2. **Deduplicate category rebuilds** (#6). Small, localized; suppress the redundant
   `_update_display_mode`/restore/navigate rebuild chain so one navigation = one rebuild.
3. **Make description indicators lazy / visible-row-only**, or harvest description presence
   during the filesystem scan (the scanner already parses `.import` files, so the imported-asset
   case rides along nearly free) so the dock never opens files while building UI.
4. **Bound the generic preview path**: queue `queue_resource_preview` only for visible rows and
   dedup/cancel stale requests, mirroring the existing scene-preview mechanism.
5. **Cap or incrementalize search-result UI population** (~500-1,000 rows initially, apply the
   rest in per-frame batches). The 10,000-match cap alone does not help; row construction is the
   cost.
6. **Off-thread search** with the snapshot + generation-stamped ResultBatch architecture above
   (or, as a smaller intermediate step, a lowercase filename index rebuilt on
   `filesystem_changed`).

Fixes 1, 2, and 4 are small localized changes and should recover most of the perceived lag.
Fix 3 is moderate. Fixes 5-6 are the architectural investment and should be measured against
the post-1-4 baseline before being scheduled (same measure-first discipline as G5).

## Relationship to other docs

- **Asset Fact Index** ([ASSET-FACT-INDEX.md](./ASSET-FACT-INDEX.md)) — the umbrella plan.
  Fixes 3, 5, and 6 of this doc are its steps C/D, G, and E; fixes 1, 2, and 4 land
  unchanged as its step A. Implementation should follow that doc's sequencing.
- **Derived data store** (`scrapline/docs/proposals/DERIVED_DATA_STORE-2026-08-09.md` and
  `DERIVED_DATA_STORE_IMPLEMENTATION-2026-08-10.md`) — directly coupled, in both directions.
  The store's core promise is that machine-generated artifacts are *hidden from Explore*, which
  means the dock grows a filtering/classification responsibility per row; that filter must be
  answered from data already resident in the scanner's model (or the fix-6 snapshot/index),
  **never** by a per-row path lookup or sidecar read — precisely the pattern this plan removes
  (#1, #3). Conversely, this plan's fix 3 (harvest per-file metadata during the scan instead of
  probing files at UI-build time) is the same architectural move the store needs: the scanner's
  published model becomes the single source of derived per-file facts. If the store lands first,
  description presence and storage-class/hidden flags should ride the same harvested-metadata
  channel rather than each adding its own probe.
- **Model import UI / glTF material overrides**
  ([GLTF-MATERIAL-OVERRIDES-DESIGN.md](./GLTF-MATERIAL-OVERRIDES-DESIGN.md)) — the import UI
  surfaces per-file state in Explore and the inspector. Any badges/indicators it adds to file
  rows must follow the same rule: read from the scanned model or snapshot, no file I/O during
  list construction. The fix-6 generation-stamped snapshot is the seam that UI should query.
- **G4** ([G4-bottom-drawer.md](./G4-bottom-drawer.md)) — Explore drawer hosting this dock; this
  plan optimizes its FileSystem content, not the drawer chrome.
- **G5** ([G5-faster-load.md](./G5-faster-load.md)) — shares the measure-first gate for the
  architectural tier; this plan's fixes 1-4 are exempt because the pathology is verified in code
  and the changes are trivially bounded.
- **DIVERGENCE-LEDGER.md** — `filesystem_dock.cpp` and `editor_file_system.cpp` are stock files
  with fork edits; log the changes there when implemented.
