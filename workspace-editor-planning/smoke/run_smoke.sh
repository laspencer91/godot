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

# A 3D-root scene can still contain CanvasItems. Exercise the pane-local selector in both directions,
# assert the matching shared toolbar follows it, and verify each lazily-created surface is retained.
cp "$SMOKE_DIR/scene_view_toggle_project.godot" "$WORK/project.godot"
run_case "scene_view_toggle" -e "res://test_3d.tscn"
if grep -q 'SCENE_VIEW_TOGGLE_OK' "$WORK/scene_view_toggle.log"; then
	echo "  PASS  scene_view_toggle_assertions (2D/3D surfaces + toolbar state verified)"
else
	echo "  FAIL  scene_view_toggle_assertions (toggle sequence did not reach its success marker)"
	fail=1
fi
cp "$SMOKE_DIR/project.godot" "$WORK/project.godot"

# Standardized viewport chrome: register controls at both the view and 3D-subviewport tiers,
# verify deterministic slot ordering and theme variations, then unregister one factory and
# require all of its generated Controls to be freed.
cp "$SMOKE_DIR/viewport_chrome_project.godot" "$WORK/project.godot"
run_case "viewport_chrome" -e "res://test_3d.tscn"
if grep -q 'VIEWPORT_CHROME_OK' "$WORK/viewport_chrome.log"; then
	echo "  PASS  viewport_chrome_assertions (factories + slots + cleanup verified)"
else
	echo "  FAIL  viewport_chrome_assertions (registration test did not reach its success marker)"
	fail=1
fi
cp "$SMOKE_DIR/project.godot" "$WORK/project.godot"

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
# drop target. This case does not depend on the preceding route case's restored split, and native-
# window GUI input is not addressable through an inherited compact layout under the headless driver,
# so isolate it with the editor's clean single-pane default. The marker proves completion.
cp "$SMOKE_DIR/scene_tree_drag_project.godot" "$WORK/project.godot"
rm -f "$WORK/.godot/editor/editor_layout.cfg"
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
