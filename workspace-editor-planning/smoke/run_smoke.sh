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

fail=0

run_case() {
	local name="$1"; shift
	local log="$WORK/$name.log"
	"$BIN" --path "$HOST_WORK" "$@" --quit-after "$QUIT_AFTER" >"$log" 2>&1
	local code=$?
	local errs
	errs=$(grep -cE "$ERR_RE" "$log")
	if [[ $code -eq 0 && $errs -eq 0 ]]; then
		echo "  PASS  $name (exit 0, 0 errors)"
	else
		echo "  FAIL  $name (exit $code, $errs error-class lines)"
		grep -nE "$ERR_RE" "$log" | head -8 | sed 's/^/        /'
		fail=1
	fi
}

echo "Workspace-editor smoke test"
echo "  binary : $BIN"
echo "  project: $WORK (copy of $SMOKE_DIR)"
echo "  frames : $QUIT_AFTER per case"
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
"$BIN" --path "$HOST_RT" -e --quit-after "$QUIT_AFTER" >"$RT/save.log" 2>&1
# The scene must appear in a pane's "docs" list (not just open_scenes) after the save.
if grep -q '"docs".*res://test_3d.tscn' "$RT/.godot/editor/editor_layout.cfg"; then
	"$BIN" --path "$HOST_RT" -e --quit-after "$QUIT_AFTER" >"$RT/restore.log" 2>&1
	rt_code=$?
	rt_errs=$(grep -cE "$ERR_RE" "$RT/restore.log")
	if [[ $rt_code -eq 0 && $rt_errs -eq 0 ]]; then
		echo "  PASS  save_restore_roundtrip (scene persisted to tabs + restored clean)"
	else
		echo "  FAIL  save_restore_roundtrip (restore exit $rt_code, $rt_errs error-class lines)"
		grep -nE "$ERR_RE" "$RT/restore.log" | head -8 | sed 's/^/        /'
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
