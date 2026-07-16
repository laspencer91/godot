<task>
WP25 — Face eyedropper: copy face texture properties from a picked source face onto the selection.

Repo: C:\Development\Engines\godot (custom Godot fork, master). Files: editor/level/select_tool.*
(+ select_tool_texture.cpp), editor/level/level_editor_view.cpp (the face/material context section
where "Apply Active Material" and the Flip U/V modify buttons live), modules/level_kernel as needed.

Contract:
1. Add an "Eyedropper" toggle button to the face context section in the top strip (next to Apply
   Active Material), with a tooltip like "Pick a face; its material and UV properties are applied
   to the selected faces". Enabled whenever there is a face selection (either tier; object
   selection counts as all faces of the block, same rule Apply uses).
2. Flow: with faces selected, click the eyedropper (it latches, cursor/armed state visible via the
   button's pressed state), then LMB-click a source face in the viewport. Effects:
   a. The source face's material is applied to every selected face (reuse the WP15 per-face
      material-table apply path — same semantics as Lift+Apply combined, one undo action).
   b. The source face's UV properties transfer where they make sense:
      - uv_mode is copied (PROJECTED or EXPLICIT).
      - For PROJECTED sources: copy the packed uv_transform (offset/rotation/scale) into each
        target face's own grid frame — i.e. same texel density, rotation, and offset relative to
        each target's own tangent frame, NOT a raw world-frame copy (raw frames only make sense on
        coplanar faces). Run reconcile_face_uv per the TOOL-FOUNDATIONS §2 contract so uv0 is
        rematerialized.
      - For EXPLICIT sources (hotspot-fit or unwrapped): copy material + realized texel density
        only; do not attempt to clone explicit islands. Report this rule in the status toast.
   c. One undo step named "Eyedrop Face Properties" covering material + UV changes together.
3. After the pick the eyedropper disarms (one-shot). Escape or RMB while armed cancels it. Clicking
   empty space while armed: status toast "no face under cursor", stays armed.
4. Status toast on success via the existing `_show_texture_status` path, e.g.
   "Eyedropped brick_wall_a onto 5 faces".
5. Keyboard affordance optional; if trivial, Alt+E is NOT free — do not bind any key this WP unless
   you verify it is unused; the button alone satisfies the request.
6. Baker: material/uv changes flow through the existing diff -> bake path; no baker changes
   expected. Per-loop uv0 stays the only field the baker reads.

Out of scope: eyedropping onto edges/vertices, cross-document picks, sampling blockout quick-slots.
Do NOT touch editor/scene/3d/node_3d_editor_viewport.* (user's WIP) or editor/gui/document_view.cpp.
</task>

<action_safety>
- NEVER kill, Stop-Process, or taskkill ANY godot process — orphaned editors are for the human. On
  "Access is denied" exe-lock build failures (often masked by a cosmetic methods.py AttributeError),
  fall back to `extra_suffix=wp25`.
- Never run two scons builds concurrently in this tree.
- No git commits.
- Append a WP25 entry to workspace-editor-planning/DIVERGENCE-LEDGER.md.
</action_safety>

<verification_loop>
1. Build: `scons platform=windows target=editor dev_build=yes -j4` (fallback extra_suffix=wp25).
2. Existing smoke assertions (face_texture_smoke, uv_smoke, hotspot smokes, material browser) pass
   UNMODIFIED; if one must change, STOP and flag it in your report instead of editing it.
   NOTE: if WP23 (object drill-down) has landed by the time you run editor GUI smokes, its click
   contract requires an engaging first click on a block before component picks — write any new GUI
   smoke steps accordingly.
3. Extend face_texture_smoke (kernel or editor level, whichever fits the existing structure):
   (a) eyedrop from a rotated/scaled PROJECTED source face onto two selected faces -> targets
   report the same material path, same realized texel density, same rotation component in their
   packed uv_transform; (b) eyedrop from an EXPLICIT source -> targets get material + density, keep
   their uv_mode sane per your implemented rule; (c) single undo restores both material and UV
   state on all targets.
4. Run the touched smoke cases from PowerShell (Git Bash may not start in this sandbox) plus kernel
   smokes: smoke.gd, uv_smoke.gd, face_texture_smoke.gd. Known pre-existing failure unrelated to
   you: scene_tree_drag (user's inspector-lock WIP) — ignore it.
5. Iterate until green.
</verification_loop>

<compact_output_contract>
Report: files touched with one-line summaries; the exact property-transfer rules implemented for
PROJECTED and EXPLICIT sources; undo action structure; new smoke sections; verbatim final PASS/FAIL
lines of every smoke you ran; ledger entry text. No process dumps, no full logs.
</compact_output_contract>
