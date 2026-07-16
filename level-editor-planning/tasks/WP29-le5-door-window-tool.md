<task>
WP29 — Door/Window tool: face-anchored grid-snapped rectangle -> through-cut, with a preset
palette. DEPENDS ON WP28's `subtract_box` kernel op — do not start unless it exists and
cutout_smoke passes.

Repo: C:\Development\Engines\godot (custom Godot fork, master). Files: editor/level/ (new tool +
level_editor_view context dock), reuse WP28 kernel op; spec context: tools/07-booleans.md B6.

User-specified UX (implement as written):
1. A Door tool (new LevelEditorTool, toolbar button; works for windows too — call it "Cutout" in
   code, label the button "Door/Window"). Arming it turns the tool context section of the top
   strip into the tool's settings panel:
   - PALETTE at top: a list of preset buttons. Clicking a preset fills the settings below.
     Right-click on a preset -> confirmation-free "Delete Preset" context menu item.
   - SETTINGS below: Width and Height spin boxes (meters, grid-snapped steps).
   - BOTTOM: a "Save Preset" button — saves the CURRENT settings under a name the user must type
     (inline LineEdit or small dialog; empty name = disabled save; duplicate name = overwrite).
   - Ship sensible defaults on first run: e.g. "Door 1x2", "Door 1.5x2.5", "Window 1x1",
     "Window 2x1". Persist presets across sessions in the editor-settings layer the level editor
     already uses for persistent state (match how snap step / mode state is persisted; if nothing
     is persisted yet, use EditorSettings project metadata) — document where.
2. Gesture: with the tool armed, hovering a block face shows a Width x Height rectangle preview
   lying IN that face's plane, centered on the cursor, snapped to the face's grid (same tangent
   frame convention as face texturing/grid alignment; snap uses the current snap step and follows
   the mouse along the surface). The rectangle clamps sensibly at face edges (do not require it to
   fit — overhang is legal; the boolean handles it).
3. LMB confirms: build the cut box from the rectangle — box depth runs from just above the face
   (+epsilon along the normal) THROUGH the entire block and out the other side (target block's
   local AABB extent along -normal, plus epsilon both ends) — and call WP28 `subtract_box` with
   the active material as p_cut_material, as ONE undo action ("Cut Opening"). On structured
   failure from the kernel: status toast with the failure message, geometry untouched.
4. After a successful cut the tool stays armed (repeat openings); Esc/RMB exits to Select. If the
   kernel rejects repeatedly the tool must never wedge — every failure path returns to hovering.
5. Preview rendering through the existing ToolOverlay; footprint + a subtle depth hint (e.g. the
   projected exit rectangle) if cheap — the flat rectangle alone is acceptable v1.

Out of scope: arched/shaped openings, frame/trim spawning, multi-face spans, add/union ops.
Do NOT touch editor/scene/3d/node_3d_editor_viewport.* (user's WIP) or editor/gui/document_view.cpp.
</task>

<action_safety>
- NEVER kill, Stop-Process, or taskkill ANY godot process — orphaned editors are for the human. On
  "Access is denied" exe-lock build failures (often masked by a cosmetic methods.py AttributeError),
  fall back to `extra_suffix=wp29`.
- Never run two scons builds concurrently in this tree.
- No git commits.
- Append a WP29 entry to workspace-editor-planning/DIVERGENCE-LEDGER.md.
</action_safety>

<verification_loop>
1. Build: `scons platform=windows target=editor dev_build=yes -j4` (fallback extra_suffix=wp29).
2. Existing smoke assertions pass UNMODIFIED; if one must change, STOP and flag it in your report
   instead of editing it. NOTE: WP23's drill-down click contract applies to GUI smokes (engaging
   click before component interaction).
3. New editor GUI smoke: arm the tool -> context section shows palette/settings; select the
   "Door 1x2" preset -> spin boxes read 1 and 2; hover a wall face -> overlay reports preview
   geometry; click -> block face/edge counts change consistently with a through-cut and the
   opening's cavity faces carry the active material; undo -> original counts restored; save a
   preset named "Test 2x2" -> it appears in the palette and survives an editor-settings
   round-trip; right-click delete removes it.
4. Run the touched editor cases + cutout_smoke + kernel smoke sweep from PowerShell (Git Bash may
   not start in this sandbox). Known pre-existing failure unrelated to you: scene_tree_drag
   (user's inspector-lock WIP) — ignore it.
5. Iterate until green.
</verification_loop>

<compact_output_contract>
Report: files touched with one-line summaries; where presets persist and the storage shape; the
rectangle->cut-box construction (frame, depth, epsilons); new smoke sections; verbatim final
PASS/FAIL lines of every smoke you ran; ledger entry text. No process dumps, no full logs.
</compact_output_contract>
