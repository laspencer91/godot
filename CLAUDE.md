# Godot fork — working agreements

This working directory is shared by MULTIPLE agent sessions at once. The rules
below exist because concurrent or flag-divergent builds corrupt the shared
object tree.

## Building the editor

- **Always build via `.\build_editor.ps1`. Never run raw `scons` for the
  production editor.** The script serializes builds (lock file + detection of
  scons runs started outside it), pins the canonical flags, verifies the link
  actually replaced `bin\godot.windows.editor.x86_64.exe`, and restores the
  `godot.exe` / `godot-editor.exe` hardlinks that every relink severs.
- Do not vary production build flags between sessions. Flag flips (e.g.
  `winrt=no` vs the coroutine-silencing define, `production=yes` vs
  `dev_build=no`) force large rebuilds of `bin\obj` and, when combined with a
  concurrent build, yield libs that no longer match their referencing objects
  (phantom `LNK1120` unresolved externals).
- Dev/test builds (`scons platform=windows target=editor dev_build=yes
  tests=yes winrt=no -jN`) write to a separate `.dev.` object namespace and may
  run alongside nothing else — check for running scons first
  (`Get-CimInstance Win32_Process -Filter "Name='python.exe'"` and look for
  SCons in the command line), and never run two builds of the same namespace
  concurrently.
- A running editor holds the exe lock; the final link then fails even though
  the console output can look successful. The wrapper script detects this via
  the binary timestamp. If you must free the path manually, RENAME the running
  exe aside instead of killing the editor.
- Unit tests: build with `tests=yes` (dev), then
  `bin\godot.windows.editor.dev.x86_64.exe --test --test-case="*Box3D*"` (etc.).

## Committing

- The working tree usually carries other sessions' uncommitted work. Stage
  files explicitly (never `git add -A` / `git commit -a`), and only files your
  task touched.
