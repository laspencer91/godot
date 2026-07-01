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

# --- error-class matcher -------------------------------------------------------
# Any of these lines in a run means the case failed. Kept in one place so the net
# tightens uniformly.
ERR_RE='ERROR|WARNING|material.*null|leaked|Camera is not|Condition.*is true|Parameter .* is null'

QUIT_AFTER="${QUIT_AFTER:-200}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# Work on a throwaway copy so the committed project stays clean (no .godot/).
cp "$SMOKE_DIR"/*.tscn "$SMOKE_DIR/project.godot" "$WORK"/

fail=0

run_case() {
	local name="$1"; shift
	local log="$WORK/$name.log"
	"$BIN" --path "$WORK" "$@" --quit-after "$QUIT_AFTER" >"$log" 2>&1
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

echo
if [[ $fail -eq 0 ]]; then
	echo "SMOKE: PASS"
else
	echo "SMOKE: FAIL"
fi
exit $fail
