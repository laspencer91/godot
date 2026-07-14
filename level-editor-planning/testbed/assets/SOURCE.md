# Level Editor Testbed — asset provenance

These are **test copies** taken from One More House's curated PSX modular-architecture kit, for
use as first-production content while developing `modules/level_kernel` (see `PLAN.md` §3, §4
"Testbed project"). They are not authoritative — the game repo's copies under
`one-more-house/assets/environment/modular_architecture/` remain the source of truth.

## Original provenance (copied/summarized from the game repo's `SOURCE.md`)

Creator: Pizza Doggy
Product: PSX Mega Pack II v1.8
Vendor root: `C:\Development\Asset Packs\PSX Mega Pack II v1.8\PSX Mega Pack II`
Selected source branch: `Models\GLB (recommended)\Modular Structures` / sibling `Textures\`

The game repo's `tools/assets/convert_psx_modular_architecture.py` promotes 19 shared, opaque,
texture-only source images out of the vendor pack into
`one-more-house/assets/environment/modular_architecture/textures/` (see that project's
`texture_manifest.json` for hashes/sizes). This testbed copies 5 of those 19 verbatim (same
bytes, no reprocessing) to cover wall/window, floor, and trim variety without pulling in the
full kit:

| File | Role here | Source (vendor pack) |
|---|---|---|
| `T_WindowBlockedDividedGrid.png` | wall / window insert | `window_hr_1.png` |
| `T_PlatformPlanks.png` | floor (wood) | `planks_hr_3.png` |
| `T_PlatformSteel.png` | trim (steel beam) | `steel_beams_hr_1.png` |
| `T_ExposedRebar.png` | trim (damage detail) | `rebar_hr_1.png` |
| `T_GarageDoorLargePlateSteel.png` | wall (steel plate) | `garage_door_hr_4.png` |

`materials/M_*.tres` mirror the game repo's `StandardMaterial3D` convention 1:1 for these five
(same `albedo_texture`, `metallic_specular`, `roughness`, `cull_mode` where present, and
`texture_filter = 5` — `TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC`, the filter every
material in the game repo's modular-architecture and PSX-textures sets uses; it is **not**
nearest/point filtering).

## License

`PIZZA_DOGGY_LICENSE.txt` (retained alongside the game repo's curation, not duplicated here)
records the creator's game-asset license: it permits use and modification in games, including
commercial games, and prohibits redistributing the assets on their own or inside asset
packs/templates. The source pack delivery inspected on this machine did not contain an
additional license/readme file; verify storefront-specific terms before any public source
release. Consult `one-more-house/assets/environment/modular_architecture/SOURCE.md` for the full
account (26 geometries, 19 textures, conversion pipeline) — this file only covers the subset
copied here.
