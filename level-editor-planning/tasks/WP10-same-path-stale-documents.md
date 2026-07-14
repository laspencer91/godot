# WP10 — same-path document staleness: silent reload + conflict dialog (implementation brief)

<task>
This Godot 4.8 fork (repo root = this workspace) has a multi-document workspace editor where the
SAME scene file can be open twice as two different document types: a plain scene document and a
"level document" (TYPE_LEVEL, the G-Level level editor — see editor/editor_document.h and
EditorNode::open_scene_in_level_editor, editor/editor_node.cpp:5303). Saving from one leaves the
sibling displaying stale content, because the existing staleness machinery
(EditorNode::_scan_external_changes, editor_node.cpp:1624 — per-document stored mtime vs disk
mtime, "Files have been modified outside Godot" ConfirmationDialog, _reload_modified_scenes)
only runs on window FOCUS-IN and thus never catches in-process saves.

Implement, in editor/editor_node.{h,cpp} (plus editor/editor_data.{h,cpp} if a small query
helper is cleaner there):

1. SAVE HOOK. In the err == OK success tail of EditorNode::_save_scene (editor_node.cpp:2619-2622,
   right where set_scene_as_saved / set_scene_modified_time already run), if any OTHER edited
   document shares the saved path (compare localized paths, exclude the saved idx), queue that
   path and schedule ONE deferred processing call (coalesce: multiple saves in one frame — e.g.
   Save All — must result in a single deferred run over the accumulated queue). Do NOT act
   synchronously inside _save_scene — reloading removes/reopens documents and must not run
   inside the save call stack.

2. DEFERRED PROCESSOR. A new method (e.g. _process_same_path_stale_scenes) that, at fire time,
   RE-VALIDATES from scratch — for each edited document whose path is in the queued set:
   - skip if disk mtime <= its stored scene_modified_time (not actually stale anymore; this is
     what makes Save-All-with-two-dirty-siblings resolve correctly with no spurious prompt);
   - if the document has NO unsaved changes (use the same signal the scene tabs' unsaved
     asterisk uses — look at _update_unsaved_cache / EditorUndoRedoManager unsaved state — NOT
     just a guess): reload it in place SILENTLY, preserving its position and document type (see
     item 3), and without stealing focus from the currently edited document;
   - if the document HAS unsaved changes: this is a genuine conflict — reuse the existing
     disk_changed ConfirmationDialog flow (populate disk_changed_list / disk_changed_scenes with
     just the conflicted scenes and pop it, exactly like _scan_external_changes does). Its
     existing buttons already carry the right semantics (reload-from-disk vs resave-mine).
   Clear the queue after processing.

3. TYPE-PRESERVING RELOAD (fixes a latent PRE-EXISTING bug too). _reload_modified_scenes
   (editor_node.cpp:1679) reopens every stale document via open_scene(), which converts a stale
   LEVEL document back into a plain scene document. Factor the remove+reopen+move-to-index steps
   into a helper that captures the document's type BEFORE _remove_edited_scene and reopens
   TYPE_LEVEL documents via open_scene_in_level_editor(path) instead of open_scene(path)
   (EditorDocument is obtained via editor_data.get_document(i); check get_type() ==
   EditorDocument::TYPE_LEVEL). Use the helper from BOTH _reload_modified_scenes and the new
   silent-reload path, so the focus-in dialog flow is fixed as well. Preserve the existing
   behavior of restoring the current scene index afterwards.

Notes/context:
- _open_scene_internal's already-open early-return matches on (path, document type) pairs, so
  reopening one sibling never falsely matches the other-type sibling. No change needed there.
- Known accepted trade-offs (do NOT try to solve): reloaded documents lose undo history (same
  as the existing reload dialog); mtime granularity is 1 s; staleness through instanced
  sub-scenes is out of scope.
- Keep the seam thin: this is a shared-file touch (editor_node), so smallest-possible diff,
  no drive-by refactors.

4. LEDGER. Append one entry to workspace-editor-planning/DIVERGENCE-LEDGER.md under the G-Level
   log describing the editor_node touch (same-path stale-document handling + type-preserving
   reload), following the ledger's existing entry format.

SMOKE (new, additive). Add a headless editor smoke to the existing suite pattern (look at how
workspace-editor-planning/smoke/run_smoke.sh stages cases and how
level-editor-planning/testbed/addons/*_smoke plugins are written; add e.g. addons/stale_reload_smoke
to the testbed project and wire it into run_smoke.sh the same way existing level cases are):
   a. open main.tscn as a plain scene document, then open the SAME path in the level editor via
      FileSystemDock.open_scene_in_level_editor (both open simultaneously);
   b. mutate the level document (add a LevelBlock like transform_smoke's _make_block does, or any
      minimal scene change through its document context) and save it via the editor save path
      (EditorInterface.save_scene or the equivalent that routes through _save_scene for the
      CURRENT document — make sure the level document is current);
   c. wait some frames; assert the plain scene document was silently reloaded (its edited scene
      root now contains the new node) and that it is STILL a plain scene document, and that the
      level document is untouched and still current;
   d. assert no disk_changed dialog is visible (silent path);
   e. (if achievable headless without flakiness) dirty the plain document first, save from the
      level document again, and assert the disk_changed dialog DID appear; otherwise skip (d)'s
      counterpart and note it.
Existing smoke assertions must pass UNMODIFIED.
</task>

<action_safety>
Allowed files: editor/editor_node.{h,cpp}, editor/editor_data.{h,cpp} (optional small helper),
workspace-editor-planning/DIVERGENCE-LEDGER.md, the new smoke addon + its run_smoke.sh wiring.
NEVER touch editor/scene/3d/node_3d_editor_viewport.* (user's uncommitted WIP). No git commits.
No existing bound signatures change. No behavior changes outside the described feature (the
type-preserving reload IS a deliberate fix to the existing dialog flow).
</action_safety>

<verification_loop>
Build from repo root: `scons platform=windows target=editor dev_build=yes -j4`; fix all errors
and new warnings. If the FINAL LINK fails with "Access is denied" on
bin/godot.windows.editor.dev.x86_64.exe, the USER'S LIVE EDITOR holds the lock — NEVER kill or
Stop-Process ANY godot process for ANY reason (sandbox-visible process metadata is unreliable).
Retry a few times over ~2 minutes; if still locked, build with extra_suffix=wp10 and verify
against bin/godot.windows.editor.dev.x86_64.wp10.console.exe, and say so in the report.
Then run, all green: the new stale_reload smoke plus the FULL existing suite via
`bash workspace-editor-planning/smoke/run_smoke.sh "$(pwd)/bin/<console binary>"` (known
pre-existing failure that is NOT yours: floating_camera_preview, sub_viewport null) and the
kernel module smokes (--headless --path modules/level_kernel/tests/smoke_project --script
smoke.gd and --script transform_smoke.gd).
</verification_loop>

<compact_output_contract>
Final report: where the hook landed and how coalescing works; the unsaved-changes signal you
used and why it is the same one the tab asterisk uses; the type-preserving reload helper shape;
files touched; build result (standard or suffixed + why); verbatim tail of the new smoke and
the suite summary; any behavior deltas beyond the described feature (should be none).
</compact_output_contract>
