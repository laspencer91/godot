# WP19 — LE3 hotspot atlas foundation (HotspotAtlas resource, .rect I/O, binding registry)

<task>
Build the data half of the LE3 hotspot system in this Godot 4.8 fork (repo root = this
workspace) — the fitter and the patch-editor tab are LATER WPs; nothing here touches face UVs.
Prereq reading IN ORDER: level-editor-planning/PLAN.md §3 (hotspot system),
level-editor-planning/TOOL-FOUNDATIONS.md §3 (identity discipline: RID/path keys never names;
normalized 0..1 patch rects) + §4 hotspot row, then
level-editor-planning/tools/10-hotspot-system.md §1 (data model — BINDING: the field tables,
.rect mapping table, binding-registry design) and §4 test ideas 1/3. Then the existing code:
modules/level_kernel (resource registration patterns — how LevelMeshData/LevelBlock register
and serialize), editor/level/level_editor.{h,cpp} + material_index.* +
texel_density_scanner.* (WP14's param-name scanning and path-keyed indexing — REUSE, do not
duplicate).

IMPLEMENT
1. HotspotAtlas : Resource (modules/level_kernel/hotspot_atlas.{h,cpp}) with tools/10 §1.1's
   exact field set: atlas_id (StringName), reference_texture (Ref<Texture2D>),
   texel_density_target (float, default 256.0), patches, default_mapping_mode (enum
   Automatic/Square/Conforming/FollowActiveQuads), disallow_random (bool), tiling_policy
   (enum NO/ALLOW/ONLY), param_names (PackedStringArray with §1.1's defaults),
   target_materials (Array of paths, informational). Registered, .tres-serializable,
   GDScript-exposed.
2. HotspotPatch per §1.2: rect_uv NORMALIZED 0..1 top-left origin, allow_rotation,
   allow_mirror_x/y, allow_tiling + tiling_axis, inset_px, patch_name (StringName; synthesize
   p{i} when absent), plus a Dictionary `extra` for round-trip preservation of unknown .rect
   keys. Derived aspect/area_texels cached (recomputed on load and on reference_texture
   change, keyed by texture path/RID — never name), never serialized. Storage shape at your
   discretion (struct array serialized via typed arrays/dictionaries) as long as the resource
   round-trips byte-stable and binds cleanly to GDScript.
3. .rect import/export (kernel-side functions on HotspotAtlas or a small
   hotspot_rect_io.{h,cpp}): Source 2 / Mallet keyvalues subrect catalog per §1.3's mapping
   table — px→normalized on import against the file's declared atlas dims, normalized→px on
   export against reference_texture dims; flags map 1:1; unknown keys preserved in `extra`
   and re-emitted on export. Tolerate flexible keyvalues formatting (quoted/unquoted, nesting
   per Source keyvalues conventions); reject malformed files with a typed error, never crash.
4. HotspotBinding registry per §1.4: pattern key → atlas path. Pattern key of a material =
   its param_names-resolved base-color texture's directory + basename stem (reuse
   TexelDensityScanner's texture resolution — extend it minimally if the texture PATH isn't
   currently exposed). Registry lives on LevelEditor, mirrored to a serialized .tres at a
   project-relative path (res://levels/hotspot_bindings.tres by default, creating lazily on
   first save; make the path an EditorSettings entry) so headless checks can read it.
   GDScript-exposed resolve: material path → atlas path or empty.
5. Editor surface THIS WP is minimal: no patch-editor tab, no fitter, no viewport anything.
   Just the resource + I/O + registry, bindable headlessly.

TESTS (modules/level_kernel/tests/smoke_project — new hotspot_atlas_smoke.gd wired like the
other kernel smokes)
  a. Resource round-trip: build an atlas in script (3 patches exercising every flag), save
     .tres, reload → all fields byte-stable; derived aspect/area recomputed not serialized.
  b. .rect round-trip per tools/10 §4.3: author a .rect fixture with rotation+tiling+inset+an
     unknown key; import → assert patch count, normalized rects (0.5px tolerance), flags,
     extra preserved; export → reimport → identical. Include a malformed fixture → typed
     rejection.
  c. Binding registry: two materials sharing one base texture stem resolve to the same
     pattern key → same atlas; a renamed material (same texture) still resolves (path-keyed,
     not name-keyed — the Scythe v0.8 regression test); registry save/load round-trip.
  d. Normalized-rect resize-proofness: change reference_texture dims → rect_uv unchanged,
     derived px values scale.

FILES YOU MAY TOUCH: modules/level_kernel/** (new hotspot files, registration, tests),
editor/level/level_editor.{h,cpp} + texel_density_scanner.* (registry hosting + minimal
texture-path exposure, additive), editor/settings/editor_settings.cpp (bindings path setting,
additive). Append one entry to workspace-editor-planning/DIVERGENCE-LEDGER.md under the
G-Level log (note fitter/patch-editor deferral to the next WPs).
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
unreliable). Retry a few times over ~2 minutes; if still locked, build with extra_suffix=wp19
and verify against bin/godot.windows.editor.dev.x86_64.wp19.console.exe, and say so in the
report. Then run, all green: every kernel smoke in modules/level_kernel/tests/smoke_project
(smoke.gd, transform_smoke.gd, uv_smoke.gd, face_texture_smoke.gd, unwrap_smoke.gd, plus your
hotspot_atlas_smoke.gd) and the editor suite via
`bash workspace-editor-planning/smoke/run_smoke.sh "$(pwd)/bin/<console binary>"` (if Bash
cannot start under your sandbox, run the suite's cases one-for-one from PowerShell as prior
WPs did and say so). Rare pre-existing flakes (rerun once before investigating):
selection_smoke exit-139 at teardown, block_tool extrude-height 2.0-vs-3.0.
</verification_loop>

<compact_output_contract>
Final report: the patch storage/serialization shape chosen; the .rect parser approach and what
formatting variants it tolerates; the pattern-key derivation and where it hooks the scanner;
files touched; build result (standard or suffixed + why); verbatim tails of ALL kernel smokes
and the editor suite; any behavior deltas beyond the new feature (should be none).
</compact_output_contract>
