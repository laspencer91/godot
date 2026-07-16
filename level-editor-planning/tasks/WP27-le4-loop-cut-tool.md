<task>
WP27 — Interactive centered Loop Cut: kernel edge-loop insert + hover-preview tool.

Repo: C:\Development\Engines\godot (custom Godot fork, master). Spec: level-editor-planning/tools/
05-edge-ops.md §2.1 (Connect/face_split), §2.2 (edge-loop insert), §3.2 (non-quad fallback), §4
(validation invariants). This is an ordered pull-forward of the LE4 loop-cut plan; implement to
that spec — deltas below are the user's UX decisions layered on top.

Stage 1 — kernel (modules/level_kernel, headless-testable first):
1. Atomic ops per tools/05: `edge_split(edge_id, t)` (SFME vertex insert with loop-attribute
   interpolation at t) and `face_split(face_id, vert_a, vert_b)` (chord insert; both children
   inherit material_index/uv_transform/polygroup/smooth verbatim; run reconcile_face_uv per the
   TOOL-FOUNDATIONS §2 UV contract so uv0 stays materialized). If equivalents already exist from
   the extrude family, extend rather than duplicate.
2. Ring walk per §2.2 (`radial -> loop.next.next -> radial` across quads), stopping at non-quads
   / boundaries / poles per §3.2. A composite `insert_edge_loop(seed_edge_id, t)` kernel op:
   edge_split every ring edge at t, face_split every crossed quad, one diff.
3. Kernel smoke (new script in modules/level_kernel/tests/smoke_project, e.g. loop_cut_smoke.gd):
   Euler characteristic delta matches §2.2 (+k verts, +2k edges, +k faces for ring length k) on a
   box (closed ring) and on a quad strip (open ring); UV interpolation at t verified on a textured
   face (texture-lock invariant: the parent faces' uv mapping is unchanged, the new loop lands at
   the correct UV parameter); attribute inheritance verbatim on both children; degenerate rejects
   (t=0/1 clamp or reject per your reading of §2.2 — document the choice).

Stage 2 — interactive tool (editor/level/):
4. A Loop Cut tool (new LevelEditorTool, toolbar button beside Select/Block; hotkey only if a free
   one is verified — the button suffices). Armed behavior, per the user:
   - Hovering geometry previews the CENTERED loop (t = 0.5 always in v1): the ring is seeded from
     the face edge nearest the cursor on the hit face (same face-local rule as edge picking), and
     the preview polyline follows the mouse across surfaces live, drawn through the existing
     ToolOverlay.
   - LMB applies the cut at center through the kernel op as one undo action ("Insert Edge Loop");
     the tool STAYS armed for repeated cuts. Esc or RMB exits back to Select.
   - Where no valid ring exists under the cursor (n-gon/pole per §3.2), show no preview
     (optionally a status hint); a click there is a no-op. Reject-don't-corrupt.
5. No slide phase in v1 (user asked for centered); leave a clean seam for a future t-slide.

Out of scope: Bevel, Bridge, Connect-as-user-facing-tool (kernel face_split is internal here),
multi-loop, slide UI.
Do NOT touch editor/scene/3d/node_3d_editor_viewport.* (user's WIP) or editor/gui/document_view.cpp.
</task>

<action_safety>
- NEVER kill, Stop-Process, or taskkill ANY godot process — orphaned editors are for the human. On
  "Access is denied" exe-lock build failures (often masked by a cosmetic methods.py AttributeError),
  fall back to `extra_suffix=wp27`.
- Never run two scons builds concurrently in this tree.
- No git commits.
- Append a WP27 entry to workspace-editor-planning/DIVERGENCE-LEDGER.md (note the LE4 pull-forward
  and the centered-only v1 scope decision).
</action_safety>

<verification_loop>
1. Build: `scons platform=windows target=editor dev_build=yes -j4` (fallback extra_suffix=wp27).
2. Existing smoke assertions pass UNMODIFIED; if one must change, STOP and flag it in your report
   instead of editing it. NOTE: if WP23 (object drill-down) has landed, GUI selection smokes need
   an engaging first click before component picks land.
3. New kernel smoke (Stage 1.3) green headless:
   `bin/<console exe> --headless --path modules/level_kernel/tests/smoke_project --script loop_cut_smoke.gd`
4. New editor GUI smoke section: arm Loop Cut over a box face -> overlay reports preview geometry;
   click -> face/edge/vert counts reflect one inserted loop; second click on the new strip -> a
   second loop; Esc -> tool exits, overlay cleared; undo twice -> original counts restored.
5. Run the full kernel smoke set (smoke, transform_smoke, uv_smoke, face_texture_smoke) + touched
   editor cases from PowerShell (Git Bash may not start in this sandbox). Known pre-existing
   failure unrelated to you: scene_tree_drag (user's inspector-lock WIP) — ignore it.
6. Iterate until green.
</verification_loop>

<compact_output_contract>
Report: files touched with one-line summaries; kernel op signatures + the §3.2 termination rules as
implemented; the ring-seed rule; Euler deltas asserted; new smoke files/sections; verbatim final
PASS/FAIL lines of every smoke you ran; ledger entry text. No process dumps, no full logs.
</compact_output_contract>
