# glTF Material Overrides and Derived Resources

Status: design proposal; implementation not started

Date: 2026-08-10

Scope: advanced 3D scene import settings, initially glTF/GLB materials imported as `BaseMaterial3D`

## Outcome

Make an imported glTF material editable in the Materials tab without requiring the user to extract,
name, locate, and maintain a separate visible material resource.

The right-hand pane should present a normal material Inspector. Each edited field becomes an
explicit, reimport-safe override layered over the material generated from the source file. Fields
the user has not overridden continue to follow changes from Blender or another glTF authoring tool.

The physical storage and regeneration machinery should remain an implementation detail. The user
experience is one material, edited in one place.

## Decisions captured by this design

1. Show a material Inspector, not a list limited to extraction/substitution options.
2. Record explicit edit intent at the field boundary; do not diff whole materials after editing.
3. Store only overridden fields. Do not freeze the complete imported material after the first edit.
4. Give every overridden field an inherited/overridden state and a direct Reset to Source action.
5. Treat texture-slot assignment as a supported direct override, stored by stable resource UID.
6. Do not support editing nested resource internals in the first version.
7. Solve material identity and ambiguous remapping before relying on overrides in production.
8. Keep `Use External` as an advanced, mutually exclusive escape hatch rather than the primary UX.
9. Relate complex override values to Derived Resources, but do not store authoritative user edits in
   a disposable cache.

## Current implementation findings

The advanced scene import dialog currently previews the selected material but edits a
`SceneImportSettingsData` proxy in the right-hand Inspector. For the material category,
`ResourceImporterScene::get_internal_import_options()` defines importer metadata rather than the
complete material property list.

This fork has already added three narrow per-material overrides: texture filter, texture repeat,
and specular (guarded by an enabled flag). They are stored under
`_subresources/materials/<material-id>` and applied during
`ResourceImporterScene::_post_fix_node()`. This proves the persistence and reimport seam, but it is
not yet a general material-authoring experience.

Relevant code seams:

- `editor/import/3d/scene_import_settings.cpp`
  - `_fill_material()` chooses the current material identity.
  - `_select()` binds material import settings to the Inspector.
  - `_re_import()` assembles `_subresources` and starts reimport.
- `editor/import/3d/resource_importer_scene.cpp`
  - `get_internal_import_options()` describes the current per-material settings.
  - `_post_fix_node()` finds generated materials and applies current overrides.
- `editor/inspector/editor_inspector.cpp`
  - `_edit_set()` is the common edit, paste, drag, and UndoRedo path.
- `modules/gltf/gltf_document.cpp`
  - `_parse_images()` resolves external, base64, and buffer-view images.
  - `_parse_image_save_image()` extracts or embeds imported images.

## Target user experience

Selecting a material in the Materials tab should show the ordinary `StandardMaterial3D` sections:
Transparency, Shading, Albedo, Metallic, Roughness, Emission, Normal Map, Sampling, and the other
sections applicable to the selected material.

Each supported property has three meaningful states:

| State | Meaning | UI |
|---|---|---|
| Inherited | The value comes from the latest source import. | Normal row; no override marker. |
| Overridden | The user explicitly owns this field in Godot. | Override marker and Reset to Source action. |
| Orphaned or unsupported | A saved override no longer maps safely to the imported material. | Warning surface with remap, retain, or remove actions. |

Editing an inherited field automatically creates an override. Reset removes the override key and
restores the current source value. A top-level Reset All Overrides action is available, with Undo.

`Use External` moves to an advanced menu. Enabling it disables inline overrides for that material
and clearly states that the external material is authoritative.

Useful secondary UI:

- a source badge such as `glTF material: PaintedMetal`;
- a count of active overrides;
- a Reset All Overrides action;
- an Advanced menu containing Use External and Promote to Project Material;
- an Unmatched Material Overrides section when source identity reconciliation is ambiguous.

## Material composition model

The preview and imported result are composed from a source material plus a sparse override patch:

```text
fresh source material
        +
explicit Godot field overrides
        =
composed imported material
```

Illustrative import data:

```text
_subresources = {
    "materials": {
        "<material-identity>": {
            "override_schema": 1,
            "source_locator": {
                "name": "PaintedMetal",
                "index": 3,
                "signature": "<source-signature>"
            },
            "overrides": {
                "albedo_color": Color(0.8, 0.1, 0.08, 1),
                "roughness": 0.35,
                "texture_filter": 2,
                "albedo_texture": "uid://..."
            }
        }
    }
}
```

The serialized schema must be versioned and must distinguish override membership from the value.
An explicit override equal to today's source value is still different from inheritance because it
remains fixed after a future source change.

## Detecting and recording field changes

Do not compare the working material to the source material at save time. Material setters clamp
values, update related flags, rebuild property lists, and may derive secondary state. A whole-object
diff would confuse those implementation effects with user intent.

Introduce an `ImportedMaterialOverrideProxy` (working name) with these responsibilities:

- retain the freshly imported source material;
- retain a sparse override map and override-membership set;
- own or reference a composed preview material;
- expose the supported material property list to `EditorInspector`;
- return the override value from `_get()` when present, otherwise the source value;
- receive explicit Inspector edits in `_set()`;
- apply the normalized value to the preview immediately;
- update override data in memory and mark the dialog dirty;
- expose override-aware reset/revert behavior.

Programmatic source refresh must update the source and composed materials without passing through
the user-edit recording path. This prevents reimport, preview initialization, and property-list
normalization from creating false overrides.

### UndoRedo contract

Every property transaction must record both value and membership:

```text
before: { overridden: false, value: source_value }
after:  { overridden: true,  value: edited_value }
```

Undo restores both members of that tuple. Redo restores both as well. Drag editing should continue
to use `MERGE_ENDS`, updating the in-memory patch during the drag but writing import metadata only
when Reimport is accepted.

Reset to Source is also an UndoRedo action: it removes membership, restores the current source
value in the preview, and can be undone back to the prior override.

## Supported property boundary

### First version

- `BaseMaterial3D` scalar, boolean, enum, color, and vector properties exposed for editing;
- texture-slot assignments such as `albedo_texture`, `normal_texture`, and `orm_texture`;
- resource references stored as UIDs rather than object pointers or fragile paths;
- source values inherited when no override exists;
- per-field and all-field reset;
- existing texture filter, repeat, and specular settings migrated into the general patch schema.

The implementation should use an allowlist derived from editor/storage property usage plus explicit
exclusions. It must not expose `resource_path`, `resource_local_to_scene`, internal rendering state,
or other resource-management properties as import overrides.

### Deferred

- editing the internals of a texture or another expanded subresource;
- `next_pass` material graphs;
- inline creation of procedural texture dependency trees;
- `ShaderMaterial` shader replacement and dynamic shader-parameter schemas;
- arbitrary custom material classes produced by importer extensions.

Unsupported saved keys remain visible as orphaned metadata until migrated or explicitly removed.
They must not be silently discarded.

## Texture storage and override behavior

glTF images may be external image files, base64 data embedded in `.gltf` JSON, or image bytes in a
glTF buffer view (commonly inside a `.glb` binary chunk).

The current default is `Extract Textures`. Godot writes embedded images into the extraction path,
imports those image sources normally, and stores their generated runtime artifacts under
`.godot/imported`. The alternative Basis Universal and uncompressed modes keep a texture as a
subresource of the imported scene.

Material overrides should not duplicate an inherited glTF texture:

```text
no albedo_texture override -> use the texture produced by the latest glTF import
albedo_texture override    -> use the explicitly selected project/managed texture UID
```

Changing an image's own import settings remains the texture import workflow, not a nested material
override. Assigning a different texture to a material slot is a direct material-field override and
is in scope.

## Material identity

Current scene-import material settings primarily fall back to material name, with generated names
for unnamed materials. Names can change in Blender, duplicate names can occur, and index-based
fallbacks can shift when materials are reordered. A silent mismatch would apply authored values to
the wrong material, which is worse than dropping them.

Identity resolution should use this preference order:

1. exporter-provided stable UUID or persistent ID in glTF extras, when available;
2. an importer-defined stable source locator retained in import metadata;
3. unique material name plus source index and a structural signature;
4. explicit user remapping when automated matching is ambiguous.

A structural signature can include material type, texture/image source locators, and selected source
properties. It is evidence for reconciliation, not unquestioned identity. Multiple equally good
matches must produce an unmatched warning rather than a guess.

Required identity behaviors:

- material rename: reconcile when unambiguous or request remapping;
- material reorder: retain overrides without depending only on array index;
- duplicate names: assign distinct identities;
- material deletion: retain an orphan record until explicit cleanup;
- copied source asset: follow the copied `.import` sidecar but receive a new asset UID;
- source type change: apply compatible keys and retain incompatible keys as unsupported.

No algorithm can perfectly infer identity after arbitrary rename, reorder, and material-content
replacement when the source provides no persistent ID. The UI must represent this uncertainty.

## Import pipeline precedence

The intended ordering is:

```text
glTF source material
  -> glTF decoding and extension mapping
  -> internal importer/plugin processing
  -> inline material override patch
  -> optional extraction or external substitution
  -> user post-import script
  -> importer post-process plugins
  -> scene-instance material overrides
```

Consequences:

- inline overrides normally survive source reimport;
- Use External is mutually exclusive and external substitution wins at that stage;
- an advanced post-import script can intentionally replace or change the material afterward;
- a material override authored on a scene instance remains downstream and higher priority than an
  import-level material override.

If product direction instead requires inline overrides to beat post-import scripts, they must be
reapplied in a final scene traversal. That would change current post-import semantics and is not the
recommended default.

## Relationship to Derived Resources

This feature exposes an important distinction that the Derived Resources design should preserve.

### Authored state

Authored state expresses user intent and cannot be deleted safely:

- the per-field override map and override membership;
- material identity/remapping decisions;
- user-created procedural textures or other inline resources;
- references to project resources chosen by the user.

Authored state must be durable, source-control-compatible, migratable, and recoverable. The simple
override map naturally belongs in the source asset's `.import` parameters. If the managed-resource
store owns complex authored resources, that store is not a disposable cache even if it is hidden
from the Explore/FileSystem UI.

### Derived state

Derived state can be regenerated from source plus authored state:

- the composed imported material;
- imported scene `.scn` output;
- compressed texture artifacts under `.godot/imported`;
- extracted copies of images whose authoritative bytes remain in the GLB;
- preview materials and thumbnails.

Derived state may live in a cache and may be deleted when the importer can deterministically rebuild
it.

### Required Derived Resources contract

Before material overrides store complex resources there, the Derived Resources work must define:

- whether the store is authoritative, derived, or supports both with an explicit distinction;
- stable UID allocation and lookup;
- an owner key combining source asset UID, subresource kind, and material identity;
- atomic create/update and crash-safe persistence;
- move/copy/rename behavior for the owning source asset;
- orphan retention, remapping, garbage collection, and recovery;
- source-control and export policy;
- behavior when `.godot` or another cache directory is cleared;
- APIs for Promote to Project Resource and optional Reveal/diagnostics without exposing routine
  storage management in the normal UX.

Recommended rule: the override map is authoritative; the composed material is derived. A complex
resource created by the user from inside a field is authoritative managed content until promoted or
removed, even if it is stored by a service that also manages regenerable outputs.

No Derived Resources design or implementation is present under that name in the current checkout.
This document therefore defines an integration contract, not a dependency on an existing API.

## Workflows that can disconnect or supersede overrides

| Workflow | Required response |
|---|---|
| Source material renamed or reordered | Reconcile identity or show an unmatched record; never guess silently. |
| Source material deleted | Retain orphaned overrides until explicit cleanup. |
| Material class/schema changes | Apply compatible keys and retain unsupported keys with warnings. |
| Texture moved | Resolve by UID. |
| Texture deleted | Keep the override but show a missing-resource state. |
| Source reimports while dialog is open | Rebuild the source, reapply the in-memory patch, and rebind the proxy. |
| User closes with pending edits | Confirm discard or run Reimport; do not silently lose edits. |
| Use External enabled | Disable inline fields or require leaving external mode first. |
| Post-import script replaces material | Script wins under the recommended pipeline order; surface this precedence. |
| Scene instance overrides material | Instance-level value wins without changing import metadata. |
| `.godot` cache cleared | Rebuild derived outputs from source, `.import`, and durable managed content. |
| Engine property renamed | Migrate by override schema version or retain as unsupported. |

## Implementation phases

### Phase 0 - contract and identity spike

- Define and test the override schema and versioning.
- Inventory `BaseMaterial3D` properties and produce the initial allowlist/exclusions.
- Test ConfigFile/Variant serialization for every supported value type.
- Test UID serialization and missing-resource behavior.
- Add stable glTF material source locators and duplicate-name coverage.
- Prototype rename/reorder reconciliation and the unmatched state.
- Resolve the Derived Resources authority/durability contract before storing complex authored data.

Gate: the same patch maps to the same material after ordinary reimport, reorder, project restart,
and cache deletion; ambiguous identity is reported rather than misapplied.

### Phase 1 - direct material proxy

- Add `ImportedMaterialOverrideProxy` and bind it for selected `BaseMaterial3D` materials.
- Compose a live preview from source plus overrides.
- Capture scalar, enum, boolean, color, and vector edits.
- Add per-field override indication, Reset to Source, and Reset All Overrides.
- Preserve filtering, folding, tooltips, accessibility, and the wider right-hand pane.

Gate: edit, drag, paste, reset, Undo, Redo, switch material, close/reopen dialog, and reimport all
preserve correct membership and values.

### Phase 2 - texture references

- Support texture-slot assignment as a direct override.
- Serialize external/project references by UID.
- Keep source glTF textures inherited without duplicating them.
- Show missing references without deleting the override.
- Disable unsupported nested dependency editing, or route creation through a durable Derived
  Resources API once available.

Gate: source texture changes flow through when inherited; selected override textures survive move,
restart, reimport, and cache deletion.

### Phase 3 - compatibility and lifecycle

- Migrate current texture-filter, texture-repeat, and specular override data.
- Add orphan/unmatched/unsupported UI.
- Handle external-mode transitions explicitly.
- Handle source reimport while the dialog is open.
- Add schema migration and diagnostics.

Gate: old metadata continues to import identically, and no destructive cleanup is automatic.

### Deferred expansion

- managed inline procedural textures;
- `next_pass`;
- `ShaderMaterial` and shader parameter identity;
- custom importer material proxies;
- batch editing across multiple imported materials.

## Verification matrix

Automated coverage should include:

- scalar/color/enum/vector override application;
- explicit override equal to source value;
- per-field reset and Reset All with UndoRedo;
- drag edits merged into one UndoRedo action;
- texture UID persistence and missing textures;
- source value update with inherited and overridden fields;
- rename, reorder, duplicate-name, delete, and re-add material cases;
- source material type/property-list changes;
- migration from the three current narrow overrides;
- external material and post-import script precedence;
- project restart and `.godot` cache deletion;
- copied and moved source assets;
- deterministic headless/CI import without opening the dialog.

Manual coverage should verify:

- the pane reads as a normal material Inspector rather than an importer form;
- override markers and reset actions remain understandable at narrow widths;
- preview updates are immediate and do not flicker during numeric drags;
- switching between materials never leaks edits to another identity;
- unmatched overrides are visible and recoverable;
- users never maintain a hidden implementation resource for the normal workflow.

## Recommended first implementation slice

Do Phase 0 and a narrow Phase 1 together:

1. establish stable material identity behavior and override schema;
2. expose only `albedo_color`, `metallic`, `roughness`, `emission`, `emission_energy_multiplier`,
   `normal_scale`, `texture_filter`, and `texture_repeat` through the proxy;
3. implement membership-aware UndoRedo and Reset to Source;
4. persist and reapply through the existing `_subresources/materials` seam;
5. verify reimport, restart, source changes, rename/reorder ambiguity, and cache deletion;
6. expand to remaining direct `BaseMaterial3D` properties only after those lifecycle gates pass.

This slice proves the hard architecture without committing prematurely to nested-resource storage or
the unfinished Derived Resources API.
