# WP2 — LE0 LevelDocument workspace-tab seam (implementation brief)

<task>
Add the Level Editor as a first-class workspace document type in this Godot 4.8 fork (repo root =
this workspace). READ FIRST, in order: level-editor-planning/PLAN.md (§1 architecture — the
service/view/document sort is BINDING), workspace-editor-planning/ARCHITECTURE.md (three-layer
taxonomy + seam rules), editor/editor_document.h (document model), editor/gui/document_view.cpp
(how surfaces are minted — see the ScriptDocument/ShaderDocument `create_editor_view` cases around
lines 177-186). The shader-editor migration is the template: study commits 618b77e3f1 (GS1
ShaderDocument scaffolding), 35c2a19f1c (GS2 create_editor_view factory), a79c0df2fb (GS3 tabs).
WP1 already landed `modules/level_kernel` (LevelMeshData/LevelMesh/LevelBlock) — built and green.

Deliverables (LE0 scope — a working empty level-editor tab, not tools):
1. `EditorDocument::TYPE_LEVEL` (append-only enum) + `LevelDocument : SceneDocument` in
   editor/editor_document.{h,cpp}. The edited thing IS a scene: LevelDocument shares
   SceneDocument's isolated World3D/World2D, per-document EditorSelection, and undo history id.
   Tab title = scene filename + " [Level]". opens_as_workspace_tab() stays true (inherited).
2. `LevelEditor` services singleton in NEW dir editor/level/ (level_editor.{h,cpp}) — LE0 scope:
   singleton lifecycle registered where other editor services are created,
   `LevelEditorView *create_editor_view(LevelDocument *)` factory, and (stub) tool-mode state.
   NO render state on the service (seam rule 1).
3. `LevelEditorView` in editor/level/level_editor_view.{h,cpp} — the per-pane surface Control
   minted by DocumentView for TYPE_LEVEL: SubViewportContainer + SubViewport bound to the
   document's World3D (set world explicitly; audio/physics untouched), own Camera3D with
   orbit (MMB or Alt+LMB), pan, mouse-wheel dolly, and WASD+RMB freelook (mirror the feel of
   Node3DEditorView's camera; a simplified reimplementation is fine — do NOT reuse
   Node3DEditorView itself), an infinite-style grid (RenderingServer/ImmediateMesh lines at 1m
   with 4m majors, following the create-detached → free-in-dtor decoration lifecycle noted in
   ARCHITECTURE.md), and an empty left toolbar strip (VBox of placeholder tool buttons: Select,
   Block — disabled) plus a top strip mounted into DocumentView's toolbar_host if present for
   scene views (else keep the strip inside the surface).
4. DocumentView routing: mint the surface via LevelEditor::create_editor_view for TYPE_LEVEL
   documents (a new case beside the shader/script ones). Scene-tree/inspector docks: reuse
   whatever SceneDocument views get (acceptable for LE0).
5. Opening path: FileSystem dock context menu on .tscn files — "Open in Level Editor" — creating
   a LevelDocument for that scene and revealing it as a workspace tab. Follow EXACTLY the code
   path scenes take into SceneDocuments (editor_data/editor_node reveal routing; find where
   SceneDocument is constructed and type-classified, and route through the same path with
   TYPE_LEVEL when the flag is set). If a scene is already open as a normal scene tab, opening in
   Level Editor may open an independent LevelDocument for LE0 (note this as a known v1 wart).
6. Headless smoke following the workspace-editor-planning/smoke/ pattern (see
   run_smoke.sh + addons/context_routes_smoke/plugin.gd): new smoke addon + project entry
   `level_tab_smoke` that boots the editor, opens level-editor-planning/testbed/main.tscn as a
   LevelDocument via the same API the context action calls, asserts: document type is TYPE_LEVEL,
   a LevelEditorView surface exists in a pane, its SubViewport's world_3d == the document's
   world_3d, then closes the tab and asserts clean teardown (no leaked instances reported at
   exit). Wire it into run_smoke.sh.
7. Append one line to DIVERGENCE-LEDGER.md describing the shared-file touches (document enum,
   document_view routing, filesystem_dock context action).
</task>

<action_safety>
Allowed files: NEW editor/level/** and smoke files; MINIMAL edits to editor/editor_document.{h,cpp},
editor/gui/document_view.cpp, editor/docks/filesystem_dock.{h,cpp}, and the smallest necessary
touch to editor_node/editor_data reveal routing; DIVERGENCE-LEDGER.md append. Keep shared-file
diffs thin — new logic lives in editor/level/. NEVER touch editor/scene/3d/node_3d_editor_viewport.*
(unrelated uncommitted work). No git commits. Match Godot C++ style and the fork's workspace
naming/comment conventions (G2/GS-style header comments citing the seam).
</action_safety>

<verification_loop>
Build: `scons platform=windows target=editor dev_build=yes` (warm cache). Fix all errors and new
warnings. Run the smoke via the run_smoke.sh mechanism (or invoke the same command it uses for one
smoke project) until green. Also confirm the WP1 checks still pass:
`bin/godot.windows.editor.dev.x86_64.console.exe --headless --path level-editor-planning/testbed --script res://checks/level_kernel_check.gd`.
Done = clean build + level_tab_smoke green + kernel check still green.
</verification_loop>

<compact_output_contract>
Final report: files created/modified with one-line purpose each (flag every shared-file touch);
build result; verbatim smoke output tail; known LE0 warts left deliberately; deviations from the
planning docs with one-line rationale each.
</compact_output_contract>
