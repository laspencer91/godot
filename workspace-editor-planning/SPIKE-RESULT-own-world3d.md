# Spike result: own-World3D isolation (G1 Step 1) — GREEN

**Date:** 2026-06-30
**Verdict:** GREEN. The make-or-break gate passes. The G1 `DocumentContext` refactor is unblocked.

## What was tested

A throwaway GDScript harness (no engine recompile — run against the existing
`bin/godot.windows.editor.x86_64` build, headless) that builds two
`SubViewportContainer` → `SubViewport` panes, each with `own_world_3d = true`,
each parenting its own camera, light, mesh, and a `StaticBody3D` collider, plus a
"gizmo" mesh instanced straight into the world's scenario via
`RenderingServer.instance_create2`.

Why GDScript and not a C++ plugin: the gate question is purely about the **engine
primitive** (do own-World3D viewports partition rendering + physics?). A script
answers it in seconds with zero compile. The editor-specific risk (Node3DEditor
gizmos hardwired to the root world) is a refactor task *inside* G1, not a
primitive question, so it isn't part of this gate.

## Results — 12/12 checks passed

- worlds differ; **scenarios differ** (`RID` A≠B); **spaces differ**
- `Node3D.get_world_3d()` (→ `Viewport::find_world_3d()`) resolves to the owning
  viewport's own world; an A-side node does **not** resolve to world B
- **picking isolation:** ray in world A hits A's body and **misses** B's body
  (only present in world B's space); symmetric for world B

## Engine facts confirmed (for the G1 implementation)

- `Viewport::get_world_3d()` returns the **explicit** `world_3d` (null for an
  own-world SubViewport). The own world is reached via **`find_world_3d()`**,
  which is what `Node3D::get_world_3d()` uses. DocumentContext must hand the
  active world to Node3DEditor via the **find/own** world, not the explicit one.
- Own world is allocated **lazily** once the SubViewport is processed in-tree;
  read it after a frame, not immediately after setting `own_world_3d`.
- `set_use_own_world_3d(true)` is the toggle (property name: `own_world_3d`).
  `set_world_3d()` can inject a shared world if we ever want N panes on one world.

## Implication for v1 scope

Per-document own World3D is sound. v1 keeps a single live Node3DEditor that
**rebinds its scenario/space to the focused document's world** on activate
(decision 2). True simultaneous per-pane 3D editing (multiple live Node3DEditors)
is deferred to G2 but is **not blocked** by any primitive limitation — the
isolation needed for it is proven here.

## Harness location

Throwaway, not committed to engine source:
`…/scratchpad/world3d_spike/` (project.godot + spike.tscn + spike.gd).
