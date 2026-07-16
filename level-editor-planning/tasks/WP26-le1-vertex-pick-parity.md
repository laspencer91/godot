<task>
WP26 — Vertex picking parity with the edge-pick fix: face-local nearest vertex.

Repo: C:\Development\Engines\godot (custom Godot fork, master). File: editor/level/select_tool.cpp
(`_resolve_vertex`).

Background: edge picking was previously fixed to be face-local — "the ray chooses the visible face,
then the nearest real edge on that exact face wins" (see the comment in `_resolve_edge`,
polygroup branch). Vertex picking still uses the old global scan: every alive vertex of every hit
block within a hard VERTEX_TOLERANCE_PX (10 px) screen radius. In practice you must click within
10 px of a vertex or NOTHING selects, and on dense/overlapping geometry the wrong block's vertex
can win. The user wants the same fix as edges:

Contract:
1. When the pick ray hits a surface, candidates come ONLY from the hit face of the front hit
   (p_hits[0]): polygroup tier = the face's boundary-loop vertices
   (via the same face-loop access `_resolve_edge` uses — `get_face_boundary_edge_ids` endpoints or
   the face corner loop); triangle tier = the hit triangle's 3 vertices
   (`get_face_triangle_vertex_ids`). Nearest in screen space wins, NO hard pixel tolerance — a
   click on a face in vertex mode always selects that face's nearest corner, exactly like edge
   mode always selects the face's nearest edge.
2. Behind-camera guards and handle resolution as today. Deterministic tie-break: smaller pixel
   distance, then smaller depth, then lower handle (extend ScreenCandidate ordering only if
   needed).
3. X-ray mode (`_is_xray_enabled()`): keep the existing global tolerance-based scan for x-ray —
   that mode exists precisely to grab occluded vertices, face-locality would break it. Only the
   normal (non-xray) path changes.
4. No changes to marquee, edge, face, or object picking.

Out of scope: anything beyond `_resolve_vertex` and minimal helpers.
Do NOT touch editor/scene/3d/node_3d_editor_viewport.* (user's WIP) or editor/gui/document_view.cpp.
</task>

<action_safety>
- NEVER kill, Stop-Process, or taskkill ANY godot process — orphaned editors are for the human. On
  "Access is denied" exe-lock build failures (often masked by a cosmetic methods.py AttributeError),
  fall back to `extra_suffix=wp26`.
- Never run two scons builds concurrently in this tree.
- No git commits.
- Append a WP26 entry to workspace-editor-planning/DIVERGENCE-LEDGER.md.
</action_safety>

<verification_loop>
1. Build: `scons platform=windows target=editor dev_build=yes -j4` (fallback extra_suffix=wp26).
2. Existing smoke assertions pass UNMODIFIED — EXCEPT any that encode the old 10 px-tolerance miss
   behavior ("click far from a vertex selects nothing while over a face"); those may be amended to
   the new contract, listed in your report with justification. NOTE: if WP23 (object drill-down)
   has landed, GUI selection smokes need an engaging first click before component picks land.
3. Extend the selection smoke: in vertex mode, a click in the MIDDLE of a face (far from any
   corner) selects that face's nearest corner vertex; a click near a different corner selects that
   corner; with two overlapping blocks, the front face's vertex wins.
4. Run the selection smoke case(s) from PowerShell (Git Bash may not start in this sandbox) plus
   the kernel smoke. Known pre-existing failure unrelated to you: scene_tree_drag (user's
   inspector-lock WIP) — ignore it.
5. Iterate until green.
</verification_loop>

<compact_output_contract>
Report: files touched with one-line summaries; the exact candidate rule per tier and the xray
carve-out; any amended assertions + justification; new smoke sections; verbatim final PASS/FAIL
lines of every smoke you ran; ledger entry text. No process dumps, no full logs.
</compact_output_contract>
