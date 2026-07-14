# Level Editor Testbed

Standalone dev/test project for the G-Level level-editor effort (see `../PLAN.md`, "Testbed
project" §4) — kept separate from One More House so the game repo stays clean until phases land.
Open with the fork editor: `bin/godot.windows.editor.dev.x86_64.console.exe --path level-editor-planning/testbed`.
Run the kernel check headless:
`bin/godot.windows.editor.dev.x86_64.console.exe --headless --path level-editor-planning/testbed --script res://checks/level_kernel_check.gd`
