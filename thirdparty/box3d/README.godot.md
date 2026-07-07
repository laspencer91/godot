# Box3D (vendored)

- Upstream: https://github.com/erincatto/box3d
- Version: v0.1.0+7, commit `52f1a25` (2026-07-06)
- License: MIT (see LICENSE)
- Contents: `include/` and `src/` only (samples/tests/benchmarks/docs stripped). `src/CMakeLists.txt` removed; built via `modules/box3d_physics/SCsub`.
- Local patches: none.

On version bump: re-vendor include+src, update this pin, and follow the bump protocol in the box3d repo's `llm/06-project-decisions.md`.
