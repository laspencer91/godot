# WP20 — LE3 hotspot fitter + apply commands (Shift+H grouped / Shift+F individual)

<task>
Build the hotspot fitter and its apply commands in this Godot 4.8 fork (repo root = this
workspace). Prereq reading IN ORDER: level-editor-planning/PLAN.md §3,
level-editor-planning/TOOL-FOUNDATIONS.md §3 (determinism, identity discipline) + §4 hotspot
row (scoring / L-shaped runs / re-fit stability — the pre-decided methods), then
level-editor-planning/tools/10-hotspot-system.md §2 (fitter algorithm — BINDING, follow it
subsection by subsection), §3 A/B/C/D-E (chosen solutions), §4 tests 1/2/4/5, §6 (apply
commands). Then the code: WP19's HotspotAtlas/HotspotPatch/binding registry (read the landed
surface), WP16's unwrap/hinge machinery in modules/level_kernel (the hinge-unfold single-step
utility and adjacency walks — the strip unwind REUSES these; do not reimplement),
WP13/15's UV storage (uv_mode PROJECTED/EXPLICIT, reconcile_face_uv, material table),
TexelDensityScanner (per-material density source), editor/level/select_tool.* (selection
access + where WP15 routed its texture ops).

FITTER (kernel, modules/level_kernel — pure, deterministic under seed, no editor deps)
Entry per tools/10 §2: fit(faces, mesh, atlas, mode, seed) -> per-face results
(uv_transform-or-loop-UVs + patch_name). Implement §2.1-§2.6 exactly:
- Island partition: GROUPED = flood-fill merge over shared edges where faces are coplanar
  (dot ≥ 0.9998) OR collinear-continuous developable (fold edge parallel to the run within
  1.15°); INDIVIDUAL = one face per island.
- Island planar unwrap: single-face/coplanar → dominant-axis projection (WP13's convention);
  developable strips → unwind about fold edges via the WP16 hinge-unfold utility THEN project
  (§3-A: fit ONE patch across the ribbon, split U back per face by arc length).
- Edge-aligned world-space OBB with the horizontal-biased reference axis (15° cone,
  deterministic).
- Scoring §2.3: density_err = |log2(patch area_texels) − log2(want area)| as the hard primary
  key with DENSITY_MARGIN = 0.35 octaves; aspect_err secondary with ASPECT_MARGIN = 0.20;
  random tie-break ONLY among finalists, rng seeded by hash(sorted island vertex ids) ^
  atlas_id, DreamUV anti-repeat vs the neighboring island's pick; disallow_random →
  patch_name-sorted finalists[0]. want sizes from THIS face's material texture dims via
  TexelDensityScanner × atlas texel_density_target. Constants as EditorSettings.
- Rotation/mirror §2.4 (orientation_needs_swap; seeded mirror bits; seeded quarter-turns for
  square patches), tiling §2.5 (reps = round(len × density / patch_len_px), repeat not
  stretch, cross-axis 1:1, tiling_policy gate), mip-aware inset §2.6 (INSET_MIP_BLEED = 1.0
  texel/level).
- Write path §3-C: Square/planar-affine results write uv_transform (PROJECTED, texture lock
  free); strips-with-internal-tiling-seams and any non-affine result bake loop UVs
  (EXPLICIT). Reuse the WP13 storage exactly — no new columns.
- Sticky re-fit §3-D/E: results record patch_name (a per-face column or side map on the
  mesh — pick the smallest persistent seam and justify it); on re-fit, if the stored
  patch_name is still a finalist, keep it (no re-roll).
- Fit-debug data (§3-B): the fitter returns per-island diagnostics (decided-by:
  density-unique / aspect / random, want_area, want_aspect, finalist names, chosen) —
  exposed to GDScript; JSON-dumpable by the headless check. The viewport debug OVERLAY is
  the patch-editor WP, not this one.

APPLY COMMANDS (editor, §6)
- Shift+H = grouped fit on the current face selection; Shift+F = individual. Both: resolve
  the atlas via the binding registry from each face's material (faces with no binding are
  skipped with a status message), run the fitter, write results through the existing diff
  path as ONE undo action ("Hotspot Fit"). Selection unchanged. Mode = atlas
  default_mapping_mode; a mapping-mode override dropdown in the Modify Texture panel section
  (Automatic = Square→Conforming above a distortion threshold; Follow Active Quads never
  auto-picked).
- Route the hotkeys exactly like WP15's texture ops (pane-focused, no collisions — grep
  current Shift+H/Shift+F usage first; report any conflict and your resolution).

TESTS
Kernel smoke (modules/level_kernel/tests/smoke_project — new hotspot_fitter_smoke.gd):
  a. tools/10 §4.1 determinism: same mesh+atlas+seed twice → byte-identical results;
     disallow_random → finalists[0] stable across runs.
  b. §4.2 density/aspect bounds on a generated wall grid: realized texels/m within
     2^DENSITY_MARGIN of target; no face stretched beyond 2^ASPECT_MARGIN (tiling repeats
     instead).
  c. §4.5 partition unit: 5 coplanar quads → 1 island; developable L-run → 1 island with
     contiguous non-overlapping arc-split U ranges; wall+floor → 2 islands.
  d. Sticky re-fit: fit, nudge one vertex slightly, re-fit → ≥95% of faces keep their
     patch_name; undo byte-exact both times.
  e. Tiling: a wall longer than the trim patch → reps > 1, seams on rep boundaries,
     cross-axis unstretched.
Game-side headless check: one-more-house/tools/checks/hotspot_fitter_check.gd driving the
same assertions through the game project (the tools/checks pattern; keep it thin — reuse the
smoke's fixture-building script code). NOTE: write the game-side file but do NOT run game
project imports beyond what the check needs; if the game project's .godot cache blocks class
resolution, note it in the report (the check runner does --import once after merges).
Editor smoke: extend the face_texture (or a new) suite case minimally: select faces with a
bound atlas fixture, Shift+H commits one undo step and changes UVs; undo restores.

FILES YOU MAY TOUCH: modules/level_kernel/** (fitter, diagnostics, bindings, smoke),
editor/level/** (hotkey routing, panel dropdown, status messages),
editor/settings/editor_settings.cpp (constants, additive),
C:\Development\Games\one-more-house\tools\checks\hotspot_fitter_check.gd (new game-side
check ONLY — nothing else in the game repo), the smoke harness (additive; per-case frame
budget bump if needed). Append one entry to workspace-editor-planning/DIVERGENCE-LEDGER.md
(note the debug-overlay deferral to the patch-editor WP) and note in the report that a
game-side DECISIONS.md entry should be logged (do NOT edit DECISIONS.md yourself — the game
repo has concurrent-writer hazards).
</task>

<action_safety>
NEVER touch editor/scene/3d/node_3d_editor_viewport.* (user's uncommitted WIP). No git
commits in EITHER repo. In the game repo touch ONLY the single new check file named above.
No changes outside the files listed. Existing bound signatures must not change (additive
only). Existing smoke assertions — kernel AND editor suite — must pass UNMODIFIED; if one
fails, your change altered behavior; fix the change, not the smoke.
</action_safety>

<verification_loop>
Build from repo root: `scons platform=windows target=editor dev_build=yes -j4`; fix all errors
and new warnings. If the FINAL LINK fails with "Access is denied" on
bin/godot.windows.editor.dev.x86_64.exe, the USER'S LIVE EDITOR may hold the lock — NEVER kill
or Stop-Process ANY godot process for ANY reason (sandbox-visible process metadata is
unreliable). Retry a few times over ~2 minutes; if still locked, build with extra_suffix=wp20
and verify against bin/godot.windows.editor.dev.x86_64.wp20.console.exe, and say so in the
report. Then run, all green: every kernel smoke in modules/level_kernel/tests/smoke_project
(all existing + hotspot_atlas_smoke.gd + your hotspot_fitter_smoke.gd) and the editor suite
via `bash workspace-editor-planning/smoke/run_smoke.sh "$(pwd)/bin/<console binary>"` (if
Bash cannot start under your sandbox, run the suite's cases one-for-one from PowerShell as
prior WPs did and say so). Rare pre-existing flakes (rerun once before investigating):
selection_smoke exit-139 at teardown, block_tool extrude-height 2.0-vs-3.0.
</verification_loop>

<compact_output_contract>
Final report: the island-partition and strip-unwind implementation (what WP16 machinery was
reused); the scoring implementation and constants wiring; the sticky patch_name persistence
seam chosen; where the apply commands routed and any hotkey conflicts found; files touched
(both repos); build result; verbatim tails of ALL kernel smokes and the touched editor case;
any behavior deltas beyond the new feature (should be none).
</compact_output_contract>
