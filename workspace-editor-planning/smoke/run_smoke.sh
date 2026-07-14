#!/usr/bin/env bash
#
# Workspace-editor smoke test — the regression net for the multi-document /
# dividable-workspace work. Codifies the per-commit "open scenes, quit exit 0,
# no error/leak spam" check that was previously run by hand.
#
# It runs the editor headlessly against a self-contained test project (this dir)
# in three configurations and, for each, asserts BOTH:
#   (1) the process exits 0, and
#   (2) zero lines match the error/warning/leak/order-bug class.
#
# Cases:
#   - open test_3d.tscn        (3D editor path: grid/origin decoration, worlds)
#   - open test_2d.tscn        (2D editor path)
#   - restore 3 open scenes    (the multi-scene layout-restore path that has
#                               historically exposed ordering bugs)
#
# Usage:
#   run_smoke.sh [path-to-godot-editor-binary]
#   SMOKE_HEADLESS=1 run_smoke.sh [path-to-godot-editor-binary]
# Binary resolution order: $1  ->  $GODOT_BIN  ->  repo bin/godot.*console.exe
#
# Exit status: 0 = all cases pass; non-zero = at least one case failed.
#
# NOTE: this is a LOG-level net (exit code + error-class grep), which is API-
# stable across the in-flight document refactor. The planned upgrade, once the
# EditorDocument/SceneDocument split lands, is to also assert each live
# document's World3D RID is distinct (proving isolation programmatically). Add
# that as an EditorScript run alongside these cases when the document API settles.

set -u

SMOKE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SMOKE_DIR/../.." && pwd)"
LEVEL_TESTBED="$REPO_ROOT/level-editor-planning/testbed"

# --- resolve the editor binary -------------------------------------------------
BIN="${1:-${GODOT_BIN:-}}"
if [[ -z "$BIN" ]]; then
	# Prefer the console build (writes to stdout/stderr on Windows).
	for cand in \
		"$REPO_ROOT/bin/godot.windows.editor.x86_64.console.exe" \
		"$REPO_ROOT/bin/godot.windows.editor.x86_64.exe" \
		"$REPO_ROOT"/bin/godot.*.editor.* ; do
		if [[ -x "$cand" ]]; then BIN="$cand"; break; fi
	done
fi
if [[ -z "$BIN" || ! -x "$BIN" ]]; then
	echo "FATAL: could not find a Godot editor binary (pass one as arg 1 or set GODOT_BIN)." >&2
	exit 2
fi

host_path() {
	local path="$1"
	if [[ "$BIN" == *.exe && -n "${WSL_DISTRO_NAME:-}" ]] && command -v wslpath >/dev/null 2>&1; then
		wslpath -w "$path"
	elif [[ "$BIN" == *.exe ]] && command -v cygpath >/dev/null 2>&1; then
		cygpath -w "$path"
	else
		printf '%s\n' "$path"
	fi
}

# --- error-class matcher -------------------------------------------------------
# Any of these lines in a run means the case failed. Kept in one place so the net
# tightens uniformly.
ERR_RE='ERROR|WARNING|material.*null|leaked|Camera is not|Condition.*is true|Parameter .* is null'

# Some Windows CI images emit a Vulkan loader diagnostic before Godot selects
# headless rendering. It is environmental and carries no engine error state.
error_lines() {
	grep -nE "$ERR_RE" "$1" | grep -v 'Loader Message'
}

error_count() {
	error_lines "$1" | wc -l
}

RUN_ARGS=()
if [[ "${SMOKE_HEADLESS:-0}" == "1" ]]; then
	RUN_ARGS+=(--headless)
fi

QUIT_AFTER="${QUIT_AFTER:-200}"
if [[ "$BIN" == *.exe && -n "${WSL_DISTRO_NAME:-}" ]]; then
	# Windows editor binaries cannot open WSL-private /tmp paths. Keep the throwaway
	# project beside the repo so wslpath can hand the editor a normal Windows path.
	WORK_PARENT="$REPO_ROOT/.godot-smoke-tmp"
	mkdir -p "$WORK_PARENT"
	WORK="$(mktemp -d "$WORK_PARENT/tmp.XXXXXX")"
else
	WORK="$(mktemp -d)"
fi
trap 'rm -rf "$WORK"; if [[ -n "${WORK_PARENT:-}" ]]; then rmdir "$WORK_PARENT" 2>/dev/null || true; fi' EXIT
HOST_WORK="$(host_path "$WORK")"

# Work on a throwaway copy so the committed project stays clean (no .godot/).
cp "$SMOKE_DIR"/*.tscn "$SMOKE_DIR/project.godot" "$WORK"/
# G-Shader: the shader fixture (+ its .uid) for the shader-tab restore case below.
cp "$SMOKE_DIR"/*.gdshader "$SMOKE_DIR"/*.gdshader.uid "$WORK"/ 2>/dev/null || true
# Generic resource-tab fixtures, including the editor plugin that invokes the public API.
cp "$SMOKE_DIR"/*.tres "$WORK"/ 2>/dev/null || true
cp -R "$SMOKE_DIR/addons" "$WORK"/ 2>/dev/null || true

fail=0

run_case() {
	local name="$1"; shift
	local log="$WORK/$name.log"
	"$BIN" "${RUN_ARGS[@]}" --path "$HOST_WORK" "$@" --quit-after "$QUIT_AFTER" >"$log" 2>&1
	local code=$?
	local errs
	errs=$(error_count "$log")
	if [[ $code -eq 0 && $errs -eq 0 ]]; then
		echo "  PASS  $name (exit 0, 0 errors)"
	else
		echo "  FAIL  $name (exit $code, $errs error-class lines)"
		error_lines "$log" | head -8 | sed 's/^/        /'
		fail=1
	fi
}

echo "Workspace-editor smoke test"
echo "  binary : $BIN"
echo "  project: $WORK (copy of $SMOKE_DIR)"
echo "  frames : $QUIT_AFTER per case"
echo "  display: $([[ ${#RUN_ARGS[@]} -gt 0 ]] && echo headless || echo default)"
echo

run_case "open_3d" -e "res://test_3d.tscn"
run_case "open_2d" -e "res://test_2d.tscn"

# Restore case: seed the editor layout with 3 open scenes, then launch with no
# explicit scene so the editor restores them.
mkdir -p "$WORK/.godot/editor"
cp "$SMOKE_DIR/restore_layout.cfg" "$WORK/.godot/editor/editor_layout.cfg"
run_case "restore_3_scenes" -e

# Workspace-session restore (M6.2): seed a 2-pane split layout — test_2d in one pane, test_3d +
# the screen-host in the other — and launch with no explicit scene. A clean run proves the pane
# tree rebuilds, documents re-home into the right panes, and per-pane scene views mint without
# the double-render/leak spam the restore path historically tripped on.
cp "$SMOKE_DIR/restore_workspace.cfg" "$WORK/.godot/editor/editor_layout.cfg"
run_case "restore_workspace" -e

# G-Shader: shader-tab restore. Seed a session whose pane holds the screen-host + a text-shader tab,
# then launch with no scene. A clean run proves a saved shader path resolves to a ShaderDocument and
# its DocumentView mints the editor widget (create_editor_view) + mounts the File menu without spam —
# the flip's new code, which the scene-only cases above never touch.
cp "$SMOKE_DIR/restore_shader.cfg" "$WORK/.godot/editor/editor_layout.cfg"
run_case "restore_shader" -e

# Generic embedded resource tab: the owning scene loads first, then its cached BoxMesh subresource
# resolves to a ResourceDocument whose full-pane Inspector retains the scene as its undo context.
cp "$SMOKE_DIR/restore_resource.cfg" "$WORK/.godot/editor/editor_layout.cfg"
run_case "restore_resource" -e

# Exact public route used by Terrain3D's pencil button. The temporary project enables a tiny editor
# plugin which calls EditorInterface.edit_resource(test_resource.tres) after startup. Besides a clean
# run, require the saved workspace to name that resource as the current tab; this proves the call did
# not fall back to the retired global Inspector or merely load the Resource without revealing it.
cp "$SMOKE_DIR/api_project.godot" "$WORK/project.godot"
run_case "edit_resource_api" -e
if grep -q '"cur".*res://test_resource.tres' "$WORK/.godot/editor/editor_layout.cfg"; then
	echo "  PASS  edit_resource_api_layout (resource persisted as current workspace tab)"
else
	echo "  FAIL  edit_resource_api_layout (resource was not the saved current workspace tab)"
	fail=1
fi

# Cross-document + drawer routing: retain a gizmo for a background document while another scene owns
# global focus, then exercise the Inspector's Show-in-FileSystem route and the Alt+F keyboard toggle.
# The plugin asserts the target selection and search-focus distinction; the shared error matcher catches
# the old is_editable_instance/focus_dock failures directly.
cp "$SMOKE_DIR/context_routes_project.godot" "$WORK/project.godot"
cp "$SMOKE_DIR/restore_workspace.cfg" "$WORK/.godot/editor/editor_layout.cfg"
run_case "context_routes" -e

# Pane SceneTree selection timing: a click must commit its node to the paired Inspector on release,
# while the same press becoming a drag must preserve the previously inspected object as the property
# drop target. The marker makes sure the synthetic GUI sequence ran to completion.
cp "$SMOKE_DIR/scene_tree_drag_project.godot" "$WORK/project.godot"
run_case "scene_tree_drag" -e "res://test_3d.tscn"
if grep -q 'SCENE_TREE_DRAG_SELECTION_OK' "$WORK/scene_tree_drag.log"; then
	echo "  PASS  scene_tree_drag_assertions (press/release/drag states verified)"
else
	echo "  FAIL  scene_tree_drag_assertions (GUI sequence did not reach its success marker)"
	fail=1
fi

# Floating camera preview: constructing the overlay applies a local panel style, which synchronously
# emits a theme-change notification. Exercise open/close/reopen so a recursive theme update fails as
# a crash (and require the marker so an editor that merely survives without routing the menu also fails).
cp "$SMOKE_DIR/floating_camera_preview_project.godot" "$WORK/project.godot"
run_case "floating_camera_preview" -e "res://test_3d.tscn"
if grep -q 'FLOATING_CAMERA_PREVIEW_TOGGLE_OK' "$WORK/floating_camera_preview.log"; then
	echo "  PASS  floating_camera_preview_assertions (open/close/reopen verified)"
else
	echo "  FAIL  floating_camera_preview_assertions (toggle sequence did not reach its success marker)"
	fail=1
fi

# G-Level LE0: use a throwaway copy of the dedicated level-editor testbed. The plugin calls the same
# public FileSystemDock route as "Open in Level Editor", checks TYPE_LEVEL + the explicit world bind,
# closes the workspace tab, and requires that its LevelEditorView has actually been torn down.
LEVEL_WORK="$WORK/level_tab_smoke"
mkdir -p "$LEVEL_WORK"
cp "$LEVEL_TESTBED/main.tscn" "$LEVEL_WORK/"
cp "$REPO_ROOT/thirdparty/certs/ca-bundle.crt" "$LEVEL_WORK/"
cp -R "$LEVEL_TESTBED/assets" "$LEVEL_TESTBED/addons" "$LEVEL_WORK/"
cp "$LEVEL_TESTBED/level_tab_smoke_project.godot" "$LEVEL_WORK/project.godot"
HOST_LEVEL_WORK="$(host_path "$LEVEL_WORK")"
LEVEL_LOG="$WORK/level_tab_smoke.log"
"$BIN" "${RUN_ARGS[@]}" --path "$HOST_LEVEL_WORK" -e --quit-after "$QUIT_AFTER" >"$LEVEL_LOG" 2>&1
level_code=$?
level_errs=$(error_count "$LEVEL_LOG")
if [[ $level_code -eq 0 && $level_errs -eq 0 ]]; then
	echo "  PASS  level_tab_smoke (exit 0, 0 errors)"
else
	echo "  FAIL  level_tab_smoke (exit $level_code, $level_errs error-class lines)"
	error_lines "$LEVEL_LOG" | head -8 | sed 's/^/        /'
	fail=1
fi
if grep -q 'LEVEL_TAB_SMOKE_OK' "$LEVEL_LOG"; then
	echo "  PASS  level_tab_smoke_assertions (document/view/world/teardown verified)"
else
	echo "  FAIL  level_tab_smoke_assertions (test did not reach its success marker)"
	fail=1
fi

# G-Level WP10 same-path staleness: open main.tscn as both a plain scene and a level document,
# save a level-only mutation, and require the clean sibling to reload silently without changing
# type/current focus. The dirty-dialog counterpart is skipped because embedded ConfirmationDialog
# visibility does not transition reliably under the headless display driver.
STALE_WORK="$WORK/stale_reload_smoke"
mkdir -p "$STALE_WORK"
cp "$LEVEL_TESTBED/main.tscn" "$STALE_WORK/"
cp "$REPO_ROOT/thirdparty/certs/ca-bundle.crt" "$STALE_WORK/"
cp -R "$LEVEL_TESTBED/assets" "$LEVEL_TESTBED/addons" "$STALE_WORK/"
cp "$LEVEL_TESTBED/stale_reload_smoke_project.godot" "$STALE_WORK/project.godot"
HOST_STALE_WORK="$(host_path "$STALE_WORK")"
STALE_LOG="$WORK/stale_reload_smoke.log"
STALE_QUIT_AFTER="${STALE_QUIT_AFTER:-400}"
"$BIN" "${RUN_ARGS[@]}" --path "$HOST_STALE_WORK" -e --quit-after "$STALE_QUIT_AFTER" >"$STALE_LOG" 2>&1
stale_code=$?
stale_errs=$(error_count "$STALE_LOG")
if [[ $stale_code -eq 0 && $stale_errs -eq 0 ]]; then
	echo "  PASS  stale_reload_smoke (exit 0, 0 errors)"
else
	echo "  FAIL  stale_reload_smoke (exit $stale_code, $stale_errs error-class lines)"
	error_lines "$STALE_LOG" | head -8 | sed 's/^/        /'
	fail=1
fi
if grep -q 'STALE_RELOAD_SMOKE_OK' "$STALE_LOG"; then
	echo "  PASS  stale_reload_smoke_assertions (silent/type/current verified; dirty-dialog headless-skipped)"
else
	echo "  FAIL  stale_reload_smoke_assertions (test did not reach its success marker)"
	fail=1
fi

# G-Level LE0 BlockTool: a separate single-plugin project drives the real level viewport input
# route, then verifies topology/AABB, document-local undo/redo, and scene save/reload rebuilding.
BLOCK_WORK="$WORK/block_tool_smoke"
mkdir -p "$BLOCK_WORK"
cp "$LEVEL_TESTBED/main.tscn" "$BLOCK_WORK/"
cp "$REPO_ROOT/thirdparty/certs/ca-bundle.crt" "$BLOCK_WORK/"
cp -R "$LEVEL_TESTBED/assets" "$LEVEL_TESTBED/addons" "$BLOCK_WORK/"
cp "$LEVEL_TESTBED/block_tool_smoke_project.godot" "$BLOCK_WORK/project.godot"
HOST_BLOCK_WORK="$(host_path "$BLOCK_WORK")"
BLOCK_LOG="$WORK/block_tool_smoke.log"
"$BIN" "${RUN_ARGS[@]}" --path "$HOST_BLOCK_WORK" -e --quit-after "$QUIT_AFTER" >"$BLOCK_LOG" 2>&1
block_code=$?
block_errs=$(error_count "$BLOCK_LOG")
if [[ $block_code -eq 0 && $block_errs -eq 0 ]]; then
	echo "  PASS  block_tool_smoke (exit 0, 0 errors)"
else
	echo "  FAIL  block_tool_smoke (exit $block_code, $block_errs error-class lines)"
	error_lines "$BLOCK_LOG" | head -8 | sed 's/^/        /'
	fail=1
fi
if grep -q 'BLOCK_TOOL_SMOKE_OK' "$BLOCK_LOG"; then
	echo "  PASS  block_tool_smoke_assertions (input/mesh/undo/save-reload verified)"
else
	echo "  FAIL  block_tool_smoke_assertions (test did not reach its success marker)"
	fail=1
fi

# G-Level LE1 SelectTool: fixed-camera synthetic input covers the world-scoped gizmo BVH ->
# element-BVH pipeline, mode/tier resolution, modifier grammar, marquee, adjacency walks,
# EditorSelection interop, and stale-handle removal when an authored block is undone.
SELECTION_WORK="$WORK/selection_smoke"
mkdir -p "$SELECTION_WORK"
cp "$LEVEL_TESTBED/main.tscn" "$SELECTION_WORK/"
cp "$REPO_ROOT/thirdparty/certs/ca-bundle.crt" "$SELECTION_WORK/"
cp -R "$LEVEL_TESTBED/assets" "$LEVEL_TESTBED/addons" "$SELECTION_WORK/"
cp "$LEVEL_TESTBED/selection_smoke_project.godot" "$SELECTION_WORK/project.godot"
HOST_SELECTION_WORK="$(host_path "$SELECTION_WORK")"
SELECTION_LOG="$WORK/selection_smoke.log"
SELECTION_APPDATA="$SELECTION_WORK/appdata"
SELECTION_LOCALAPPDATA="$SELECTION_WORK/localappdata"
mkdir -p "$SELECTION_APPDATA/Godot" "$SELECTION_LOCALAPPDATA"
cp "$LEVEL_TESTBED/selection_editor_settings.tres" "$SELECTION_APPDATA/Godot/editor_settings-4.8.tres"
HOST_SELECTION_APPDATA="$(host_path "$SELECTION_APPDATA")"
HOST_SELECTION_LOCALAPPDATA="$(host_path "$SELECTION_LOCALAPPDATA")"
APPDATA="$HOST_SELECTION_APPDATA" LOCALAPPDATA="$HOST_SELECTION_LOCALAPPDATA" \
	"$BIN" "${RUN_ARGS[@]}" --path "$HOST_SELECTION_WORK" -e --quit-after "$QUIT_AFTER" >"$SELECTION_LOG" 2>&1
selection_code=$?
selection_errs=$(error_count "$SELECTION_LOG")
if [[ $selection_code -eq 0 && $selection_errs -eq 0 ]]; then
	echo "  PASS  selection_smoke (exit 0, 0 errors)"
else
	echo "  FAIL  selection_smoke (exit $selection_code, $selection_errs error-class lines)"
	error_lines "$SELECTION_LOG" | head -8 | sed 's/^/        /'
	fail=1
fi
if grep -q 'SELECTION_SMOKE_OK' "$SELECTION_LOG"; then
	echo "  PASS  selection_smoke_assertions (picking/marquee/walks/object/undo verified)"
else
	echo "  FAIL  selection_smoke_assertions (test did not reach its success marker)"
	fail=1
fi

# G-Level LE1 transform/extrude: selection-owned synthetic drag input verifies relative
# world-delta snapping, Escape rollback with no history entry, face extrude composition,
# one-step undo, and stable SelectionModel handles after topology restoration.
TRANSFORM_WORK="$WORK/transform_smoke"
mkdir -p "$TRANSFORM_WORK"
cp "$LEVEL_TESTBED/main.tscn" "$TRANSFORM_WORK/"
cp "$REPO_ROOT/thirdparty/certs/ca-bundle.crt" "$TRANSFORM_WORK/"
cp -R "$LEVEL_TESTBED/assets" "$LEVEL_TESTBED/addons" "$TRANSFORM_WORK/"
cp "$LEVEL_TESTBED/transform_smoke_project.godot" "$TRANSFORM_WORK/project.godot"
HOST_TRANSFORM_WORK="$(host_path "$TRANSFORM_WORK")"
TRANSFORM_LOG="$WORK/transform_smoke.log"
TRANSFORM_APPDATA="$TRANSFORM_WORK/appdata"
TRANSFORM_LOCALAPPDATA="$TRANSFORM_WORK/localappdata"
mkdir -p "$TRANSFORM_APPDATA/Godot" "$TRANSFORM_LOCALAPPDATA"
cp "$LEVEL_TESTBED/selection_editor_settings.tres" "$TRANSFORM_APPDATA/Godot/editor_settings-4.8.tres"
HOST_TRANSFORM_APPDATA="$(host_path "$TRANSFORM_APPDATA")"
HOST_TRANSFORM_LOCALAPPDATA="$(host_path "$TRANSFORM_LOCALAPPDATA")"
APPDATA="$HOST_TRANSFORM_APPDATA" LOCALAPPDATA="$HOST_TRANSFORM_LOCALAPPDATA" \
	"$BIN" "${RUN_ARGS[@]}" --path "$HOST_TRANSFORM_WORK" -e --quit-after "$QUIT_AFTER" >"$TRANSFORM_LOG" 2>&1
transform_code=$?
transform_errs=$(error_count "$TRANSFORM_LOG")
if [[ $transform_code -eq 0 && $transform_errs -eq 0 ]]; then
	echo "  PASS  transform_smoke (exit 0, 0 errors)"
else
	echo "  FAIL  transform_smoke (exit $transform_code, $transform_errs error-class lines)"
	error_lines "$TRANSFORM_LOG" | head -8 | sed 's/^/        /'
	fail=1
fi
if grep -q 'TRANSFORM_EDITOR_SMOKE_OK' "$TRANSFORM_LOG"; then
	echo "  PASS  transform_smoke_assertions (snap/cancel/extrude/undo/selection verified)"
else
	echo "  FAIL  transform_smoke_assertions (test did not reach its success marker)"
	fail=1
fi

# Save-capture round-trip (M6.2): the restore_workspace fixture hand-authors the tab paths, so it
# never exercises the SAVE side. This case does: restore open scenes (the active one is revealed into
# a pane by M7.1), quit (which must WRITE that scene into the workspace tabs), then restore again. It
# guards the regression where SceneDocument::get_path() returned "" and every scene was silently
# dropped from the saved layout. A SINGLE open scene also guards the empty-slot-reuse reveal fix (a
# lone scene must still land in a pane, not be left on the legacy screen-host tab).
RT="$WORK/roundtrip"
mkdir -p "$RT/.godot/editor"
cp "$SMOKE_DIR"/*.tscn "$SMOKE_DIR/project.godot" "$RT"/
printf '[EditorNode]\n\nopen_scenes=PackedStringArray("res://test_3d.tscn")\ncurrent_scene="res://test_3d.tscn"\n' > "$RT/.godot/editor/editor_layout.cfg"
HOST_RT="$(host_path "$RT")"
"$BIN" "${RUN_ARGS[@]}" --path "$HOST_RT" -e --quit-after "$QUIT_AFTER" >"$RT/save.log" 2>&1
# The scene must appear in a pane's "docs" list (not just open_scenes) after the save.
if grep -q '"docs".*res://test_3d.tscn' "$RT/.godot/editor/editor_layout.cfg"; then
	"$BIN" "${RUN_ARGS[@]}" --path "$HOST_RT" -e --quit-after "$QUIT_AFTER" >"$RT/restore.log" 2>&1
	rt_code=$?
	rt_errs=$(error_count "$RT/restore.log")
	if [[ $rt_code -eq 0 && $rt_errs -eq 0 ]]; then
		echo "  PASS  save_restore_roundtrip (scene persisted to tabs + restored clean)"
	else
		echo "  FAIL  save_restore_roundtrip (restore exit $rt_code, $rt_errs error-class lines)"
		error_lines "$RT/restore.log" | head -8 | sed 's/^/        /'
		fail=1
	fi
else
	echo "  FAIL  save_restore_roundtrip (scene was NOT written into the saved workspace tabs)"
	fail=1
fi

echo
if [[ $fail -eq 0 ]]; then
	echo "SMOKE: PASS"
else
	echo "SMOKE: FAIL"
fi
exit $fail
