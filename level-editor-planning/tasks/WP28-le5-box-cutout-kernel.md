<task>
WP28 — Box cutout kernel op: Subtract-only pull-forward of the booleans plan.

Repo: C:\Development\Engines\godot (custom Godot fork, master). Spec: level-editor-planning/tools/
07-booleans.md — read it END TO END first; it is a complete defensive design and you are
implementing its pipeline RESTRICTED to: op = Subtract, exactly one target block, exactly one tool
operand which is a kernel-generated axis-frame box (from create_box, never user geometry). No
union, no intersect, no multi-tool batches, no Split-to-Blocks. The restriction changes scope, not
architecture — build the foundation services so the full tool can grow later.

Key references named by the doc: modules/csg/csg_shape.cpp `_pack_manifold`/`_unpack_manifold`;
thirdparty/manifold/include/manifold/manifold.h (MeshGL64, BatchBoolean, ReserveIDs, Status);
thirdparty/manifold/src/shared.h (TriRef faceID/originalID passthrough).

Deliverables (kernel, modules/level_kernel, headless-testable, no editor deps):
1. The §2 foundation services, scoped to what Subtract-box needs but named/shaped per the doc so
   they extend cleanly: FaceTriangulator (ear-clip planar n-gons, per-face faceID tagging, convex
   fan fast-path), ManifoldPacker/Unpacker (numProp=3 position-only, runOriginalID per BLOCK via
   ReserveIDs, faceID per source face, merge maps from the indexed mesh's own shared verts),
   AttributeRoundtripTable ((runOriginalID, faceID) -> material/uv_transform/polygroup tuple),
   CoplanarFaceMerger (edge-use-multiset boundary trace, CCW outer/CW holes, collinear/T-junction
   vertex removal with the bbox-relative tolerance policy from §2.4), PlanarUVProjector
   (world-axis tangent frame per B6/§2.5 for fresh cut faces).
2. Pipeline entry point on LevelMesh, e.g.
   `subtract_box(const Transform3D &p_box_frame, const Vector3 &p_box_size, int p_cut_material)`
   -> runs stages [1]..[12] of §1 inside the existing begin_transaction/commit diff seam. Decision
   points bind exactly as the doc's D-table: B1 one-shot never live; B2 face-key attributes; B3
   survivors copy uv_transform VERBATIM (they stay in-plane — no UV recovery math); B4
   detect-guard-reject open input, never auto-close; B5 result stays one block, multi-shell legal;
   B6 cavity-wall faces (tool-originated) get p_cut_material + projected world-planar UVs.
3. Failure contract per D4 reject-illegal-input-never-corrupt: non-manifold input, Manifold error
   Status, or budget overrun (NumTri guard) -> op returns a structured failure (code + message),
   transaction rolls back, mesh untouched. NO partial writes ever.
4. Hole faces: after a through-cut a wall face becomes an n-gon with a hole loop. If LevelMeshData
   cannot yet represent hole loops, the CoplanarFaceMerger must decompose such regions into
   simple polygons (bridge/keyhole split) — document which path you took; the baker and all
   existing smokes must keep working with the result either way.

New kernel smoke (modules/level_kernel/tests/smoke_project/cutout_smoke.gd) — minimum checks:
- Through-cut: 2x2x0.2 wall, box punched clean through -> result watertight (every edge radial==2),
  V-E+F Euler check consistent with declared genus change, front/back faces show the opening,
  cavity walls carry p_cut_material with projected UVs, ALL surviving faces' uv_transform
  byte-identical to their sources (B3).
- Partial cut (box does not exit the far side) -> pocket geometry, still watertight.
- Edge-graze/coplanar-face case (box face flush with wall face) -> either clean result or clean
  structured rejection, never a corrupt mesh.
- Reject path: deliberately opened input mesh -> structured failure, mesh byte-identical after.
- Undo: apply diff, revert diff -> original data restored, baker output identical.

RESEARCH LATITUDE: where the doc leaves a choice open (ear-clip robustness, tolerance constants,
keyhole-split details), research Manifold's own docs/samples in thirdparty/manifold and Godot's CSG
module first, decide, and record each decision in your report + the ledger entry. Do not expand
scope to other ops.

Out of scope: any editor UI (WP29 builds the door tool on this), union/intersect, Cap Boundaries.
Do NOT touch editor/scene/3d/node_3d_editor_viewport.* (user's WIP) or editor/gui/document_view.cpp.
</task>

<action_safety>
- NEVER kill, Stop-Process, or taskkill ANY godot process — orphaned editors are for the human. On
  "Access is denied" exe-lock build failures (often masked by a cosmetic methods.py AttributeError),
  fall back to `extra_suffix=wp28`.
- Never run two scons builds concurrently in this tree.
- No git commits.
- Append a WP28 entry to workspace-editor-planning/DIVERGENCE-LEDGER.md (note the LE5 subtract-only
  pull-forward + every open-choice decision you made).
</action_safety>

<verification_loop>
1. Build: `scons platform=windows target=editor dev_build=yes -j4` (fallback extra_suffix=wp28).
2. Existing smoke assertions pass UNMODIFIED (all seven kernel smokes + editor suite untouched by
   a kernel-only WP); if one must change, STOP and flag it in your report instead of editing it.
3. cutout_smoke.gd green headless:
   `bin/<console exe> --headless --path modules/level_kernel/tests/smoke_project --script cutout_smoke.gd`
4. Full kernel smoke sweep (smoke, transform_smoke, uv_smoke, face_texture_smoke, unwrap_smoke,
   hotspot_atlas_smoke, hotspot_fitter_smoke) from PowerShell.
5. Iterate until green.
</verification_loop>

<compact_output_contract>
Report: files touched with one-line summaries; the subtract_box signature + failure codes; the
hole-face representation decision; every open-choice decision made under research latitude; new
smoke checks; verbatim final PASS/FAIL lines of every smoke you ran; ledger entry text. No process
dumps, no full logs.
</compact_output_contract>
