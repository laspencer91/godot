# modules/scrapcore

The ScrapCore movement motor as an engine module — Phase 2 of the port plan
(`docs/SCRAPCORE_ENGINE_MODULE_PLAN-2026-07-28.md` in the game repo).

## D1: the canonical source lives in the GAME repo

This module carries **no copy** of the ScrapCore core. The motor, state,
params, codec and math are compiled from the game repo's `native/ScrapCore/`
via the `scrapcore_path` scons option:

```
python -m SCons ... scrapcore_path=C:/Development/games/scrapline/native/ScrapCore
```

Default: `../../games/scrapline/native/ScrapCore`, resolved against the engine
root (the standard solo-dev layout). If the path is wrong the build FAILS with
an explanatory error — there is no silent fallback, because a vendored or stale
core would drift from the parity harness and golden traces that certify it
(13 scenarios, float-precision, game repo `benches/scrapcore/`).

This couples engine builds to the game checkout on this machine. Accepted:
solo-dev, and the alternative (two copies) is how contract drift happens.

## Status

- **Gate A (this skeleton):** `ScrapCoreMotor` (RefCounted) with `pack_version()`
  and `motor_smoke()` — proves the external-path compile links real core code.
- **Gate B:** the `Box3DPawnBody` adapter (IPawnBody over Box3DCharacterMover /
  PhysicsServer3D) + the D2 packed-command tick surface + shadow parity harness.
- **Gate C/D:** flag-gated game integration, measurement, soak.

Movement events cross back to the game through the bridge generated from
`schemas/events/movement_events.schema`: `drain_events()` returns a `PackedByteArray`, never
dictionaries, and the generated GDScript decoder constructs the production typed variants. The
module also exposes the compiled contract SHA-256 and bridge-format version so a stale editor build
is refused before native simulation starts.
