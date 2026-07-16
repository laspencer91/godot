# WP15 — LE2 face texturing ops (Apply/Lift/Wrap/Flow/Align + Modify Texture panel)

<task>
Build the Scythe-grammar face texturing operations for the level editor in this Godot 4.8 fork
(repo root = this workspace). Prereq reading IN ORDER: level-editor-planning/PLAN.md §2 (UX
contract), level-editor-planning/TOOL-FOUNDATIONS.md §2 (UV storage contract) + §4 (face
texturing row), then level-editor-planning/tools/09-face-texturing-ops.md IN FULL (the op
specs — §3 op-by-op, §4.1 wrap solver, §4.2 flow propagation, §4.4 justify/fit semantics, §4.5
PROJECTED/EXPLICIT interop are all binding), then the existing code: modules/level_kernel
(WP13's frame algebra: project_native/get_uv, align_faces_to_grid/align_faces_to_face,
solve_edge_hinge_similarity, reconcile_face_uv, uv_mode/uv columns, LevelMeshDiff),
editor/level/level_editor.{h,cpp} (WP14's active-material state + set_active_material +
active_material_changed), editor/level/material_browser_dock.* (scroll-to affordance),
editor/level/blockout_material_registry.* (the quick-slot → Apply seam WP14 left),
editor/level/select_tool.* (input routing, hover/pick machinery, selection tiers),
modules/level_kernel LevelMeshBaker + LevelBlock (how material_index maps to baked surfaces).

SCOPE 0 — MATERIAL BINDING (investigate first, implement what's missing)
Faces carry material_index, but confirm how an index resolves to an actual Material resource on
the baked surfaces. If there is no per-block material table yet: add one on
LevelMeshData/LevelBlock (an ordered array of material resource PATHS — path-keyed per
TOOL-FOUNDATIONS §3.3, loaded lazily for bake), with: an "intern this material → index" helper
(dedup by path), full LevelMeshDiff/undo/serialization/duplicate coverage for the new column,
and LevelMeshBaker assigning the resolved Material per surface group. Keep the existing default
appearance when a face has no material (index -1 or empty path → current behavior). This is the
plumbing everything below stands on — do it first and verify with the existing smokes before
building ops.

OPS (each = one kernel diff per gesture, undoable, headless-drivable via GDScript bindings)
1. Apply (Shift+T): every selected face (either tier; object selection = all faces of the
   block) gets material_index = interned active material. Faces with no valid frame (zero
   uv_tangent) get Align-to-Grid lazily first (09 §3.1) — inside the same diff. No-op cleanly
   when no active material or no selection.
2. Lift (Shift+RMB): the face under the cursor (no selection required) becomes the source:
   set_active_material(its material) AND capture its full UV state (uv_mode, origin, tangent,
   uv_transform) into a "captured mapping" slot on LevelEditor beside the active material. A
   later Apply also stamps the captured mapping onto faces WHEN the capture exists and came
   from the same material (09 §3.1 — lift ≠ eyedropper; this is what makes Apply reproduce the
   look). EXPLICIT-mode source: capture material only (identity mapping), per 09 §4.5's
   graceful degradation — note it in the report. The WP14 browser scroll-to fires via the
   existing active_material_changed signal for free — verify, don't rewire.
3. Wrap (Alt+RMB): clicked face = destination, the adjacent face across the last-shared edge
   with the cursor-picked edge... follow 09 §3.2 exactly: destination(s) solved FROM an
   already-textured source across the shared edge via solve_edge_hinge_similarity, writing
   uv_transform and forcing PROJECTED. Kernel op wrap_faces(source_face_id, dest_face_ids or
   propagation set) returning one diff. Editor gesture: Alt+RMB on a face wraps it from its
   selected/last-active textured neighbor (pick the deterministic rule 09 §4.1 specifies for
   multi-neighbor ambiguity: lowest-index already-PROJECTED neighbor; conflicting neighbors →
   reject with a status message, never guess).
4. Wrap to selection (Shift+Alt+RMB): BFS from the clicked source face across the current
   selection's internal shared edges, each face solved against its already-solved BFS parent
   (propagation, NOT all-against-source — 09 §4.2). One diff.
5. Flow (Ctrl+Alt+RMB drag): same pairwise propagation, but the face set is the ordered chain
   of faces the cursor crosses during the drag (hover-pick per motion event, dedup consecutive,
   only accept faces sharing an edge with the previous chain face). Commit one diff on release;
   Escape cancels. If the chain rule needs simplification, propagating across the hovered
   sequence's shared edges is the required minimum — say what you shipped.
6. Align to Grid (Shift+Ctrl+G) / Align to Face (Shift+Ctrl+F): hotkeys on the current face
   selection calling the existing WP13 kernel ops. Grid resets uv_transform to identity, Face
   preserves it (09 §3.3).
7. Modify Texture panel: a persistent panel (follow however WP14's dock registered; a section
   inside the material browser dock or its own dock — pick what fits the fork's dock patterns
   and say which) operating on selected faces' uv_transform ONLY (never O/T — 09 §3.4), all in
   CURRENT UV space (09 §4.4: read get_uv bbox, adjust translation/scale about the bbox
   center): 8-direction shift (Numpad grammar per Scythe: 4/6/8/2 + diagonals 7/9/1/3), Scale
   X/Y up/down (Ctrl+Alt+Numpad), Rotate CW/CCW (Alt+Numpad9/7), Fit (Ctrl+Alt+Numpad5),
   Justify L/R/T/B/Center (Alt+Numpad4/6/8/2/5), face-UV flips (Alt+T vertical / Alt+R
   horizontal — negate one column re-centered on the bbox). Buttons in the panel + the numpad
   hotkeys routed only when a level pane has focus and faces are selected. Each press = one
   diff. Step sizes: sensible defaults (shift step = 1/8 of the material texture footprint or a
   fixed 0.125 UV; rotate 15°; scale ×2^(±1/4)) exposed as EditorSettings — document choices.
8. Quick-slot Apply chord: complete WP14's seam — Shift+Alt+N with a non-empty face selection
   now sets the active material AND runs Apply on it in one action (09 §3.5).
DEFERRED (note in ledger): texture-level flips Alt+E/Alt+D (a material-flag concept, needs
material asset semantics), EXPLICIT-source similarity-fit Apply templates (09 §4.5's advanced
half), Flow along edge-loop paths beyond the hover chain.

TESTS
Kernel-side additions to modules/level_kernel/tests/smoke_project (additive — extend
uv_smoke.gd or add face_texture_smoke.gd wired the same way):
  a. Wrap continuity: two quads at a dihedral angle, arbitrary uv_transform on A, wrap B →
     get_uv equal along the shared edge at t = 0/0.25/0.5/0.75/1.0 from both faces' formulas
     (09 §5.1 — endpoints-only is not enough).
  b. Flow chain: 6-face strip, flow from face 0 → every internal edge continuous (test a's
     method), face 5's transform scale within tolerance of face 0's (09 §5.2), single diff,
     byte-exact undo.
  c. Apply + material table: intern two materials by path, apply to disjoint selections →
     correct indices, dedup on re-intern, diff/undo restores the table byte-identical, baker
     produces per-material surface groups.
  d. Lift/Apply round-trip: lift face A (PROJECTED), apply to face B → B's material AND UV
     state class match A's capture; lift from an EXPLICIT face → material-only capture.
  e. Justify rotation-invariance: face with a 37° rotation baked into uv_transform, Justify
     Left → resulting UV bbox u_min == 0 regardless (09 §5.6); Fit covers the material
     footprint per axis.
  f. Panel ops: each numpad op on a fixture face = exactly one undo step; shift direction is
     UV-space (independent of frame orientation).
Editor suite: extend the transform smoke (or a new case, following the WP12/WP14 pattern —
mind the suite's per-case frame budget; give a new case its own QUIT_AFTER like
TRANSFORM_QUIT_AFTER if it needs it) covering: Shift+T applies the active material to a
selected face; Shift+RMB lifts (active material changes); one Modify Texture numpad op fires
through pane-focused routing and undoes in one step.

FILES YOU MAY TOUCH: modules/level_kernel/** (material table, wrap/flow/apply kernel ops,
bindings, tests), editor/level/** (op routing in select_tool.*, LevelEditor captured-mapping
state, Modify Texture panel UI, quick-slot completion, level_editor_view wiring),
editor/settings/editor_settings.cpp (step-size settings, additive), the smoke harness +
testbed (additive). Append one entry to workspace-editor-planning/DIVERGENCE-LEDGER.md under
the G-Level log (note the deferrals and the material-table design).
</task>

<action_safety>
NEVER touch editor/scene/3d/node_3d_editor_viewport.* (user's uncommitted WIP). No git
commits. No changes outside the files listed above. Existing bound signatures must not change
(additive only). Existing smoke assertions — kernel AND editor suite — must pass UNMODIFIED;
if one fails, your change altered behavior; fix the change, not the smoke.
</action_safety>

<verification_loop>
Build from repo root: `scons platform=windows target=editor dev_build=yes -j4`; fix all errors
and new warnings. If the FINAL LINK fails with "Access is denied" on
bin/godot.windows.editor.dev.x86_64.exe, the USER'S LIVE EDITOR may hold the lock — NEVER kill
or Stop-Process ANY godot process for ANY reason (sandbox-visible process metadata is
unreliable). Retry a few times over ~2 minutes; if still locked, build with extra_suffix=wp15
and verify against bin/godot.windows.editor.dev.x86_64.wp15.console.exe, and say so in the
report. Then run, all green: every kernel smoke in modules/level_kernel/tests/smoke_project
(smoke.gd, transform_smoke.gd, uv_smoke.gd, plus yours) and the editor suite via
`bash workspace-editor-planning/smoke/run_smoke.sh "$(pwd)/bin/<console binary>"` (if Bash
cannot start under your sandbox, run the suite's cases one-for-one from PowerShell as prior
WPs did and say so). Rare pre-existing flakes (rerun once before investigating):
selection_smoke exit-139 at teardown, block_tool extrude-height 2.0-vs-3.0.
</verification_loop>

<compact_output_contract>
Final report: the material-table design (or confirmation one existed); how each RMB op routes
through select_tool input handling without breaking existing Alt/Shift gestures; the
captured-mapping semantics shipped for Lift/Apply; where the Modify Texture panel lives and
its step defaults; files touched; build result (standard or suffixed + why); verbatim tails of
all kernel smokes and the new editor case; any behavior deltas beyond the new features (should
be none).
</compact_output_contract>
