# Live Inspector Redesign Implementation Plan

Status: design ready for implementation planning review  
Prototype: Editor UI Component Gallery (`Inspector Chrome Direction`)  
Prototype commit: `37fbf56360`

## Outcome

Move the approved gallery direction into the live Inspector without replacing Godot's property model, editor plugins, undo/redo, resource workflows, or accessibility behavior.

The live hierarchy should read, from strongest to quietest:

1. Object/class category (`DestructibleWall3D`, `Node3D`, `Node`).
2. Custom exported category/divider (`@export_category`, such as `STRUCTURE`).
3. Property group (`@export_group`, such as `Structural Health`).
4. Property subgroup (`@export_subgroup`, such as `Health` or `Armor`).
5. Property row and editor.

Each deeper semantic level moves right by one indentation token. Full-width header backgrounds remain full width; only their contents and guide rails indent.

## Current code findings

### Hierarchy is already parsed, but its meaning is lost

`EditorInspector::update_tree()` in `editor/inspector/editor_inspector.cpp` already recognizes:

- `PROPERTY_USAGE_CATEGORY`
- `PROPERTY_USAGE_GROUP`
- `PROPERTY_USAGE_SUBGROUP`
- slash-delimited nested property paths

Standard class categories and custom `@export_category` entries both become `EditorInspectorCategory`. Groups, subgroups, and path-generated sections all become `EditorInspectorSection`. Once created, the section only retains a numeric `level` and `indent_depth`; it no longer knows whether it represents an authored group, authored subgroup, or an incidental path section. That is the principal gap preventing reliable styling.

### Existing behavior that must be retained

`EditorInspectorCategory` currently owns class icons, documentation navigation, category copy/paste, favorites, sub-inspector colors, accessibility, and context menus.

`EditorInspectorSection` currently owns collapse persistence, keyboard/accessibility actions, drag-hover expansion, checkable groups, animation key controls, reset/revert state, section copy/paste, nested indentation, and property registration.

The redesign must change their presentation without bypassing those behaviors.

### Property fields are heterogeneous

`EditorProperty` is the common row/layout shell, but its editors are intentionally specialized: scalar `EditorSpinSlider`, text, enums, booleans, colors, paths, node paths, arrays, dictionaries, vectors, transforms, and plugin-provided controls. A single replacement field component would regress specialized behavior. The correct approach is shared theme tokens plus targeted compact modes on the existing controls.

### Resources already use the same Inspector

Top-level resources are inspected by the same `EditorInspector`. An expanded resource-valued property creates another `EditorInspector` as a nested sub-inspector. Therefore category, group, subgroup, property-row, filtering, and field styling will naturally apply to resources if implemented in the shared classes.

`EditorPropertyResource` delegates its row to `EditorResourcePicker` (or its Script, Shader, and AudioStream subclasses). The picker already requests real previews asynchronously and receives both normal and small previews. Today it ignores the small preview and often enlarges the row to the file-dialog thumbnail size. Compact resource rows should reuse the small preview rather than add a parallel thumbnail implementation.

### Node transform ordering

Native property lists are emitted derived-class first. Script exports and the concrete node class therefore appear before the inherited `Node2D` or `Node3D` category. The Transform group is already the first group inside the native Node2D/Node3D category; moving that category to the top makes transforms appear first without duplicating properties.

This should be a stable, Inspector-only presentation reorder. It must not alter ClassDB property order globally.

## Target live appearance

### Dock chrome

- Keep a single Inspector header and its three-dot object menu.
- Keep removed resource/history controls alive for commands, as the current fork already does, but do not restore their toolbar rows.
- Replace the ad hoc filter `LineEdit` with `EditorSearchBar`, full width inside the same outer outline as the property content.
- Feed the search component the number of currently visible matching properties; expose that count from `EditorInspector` rather than making the search component parse properties itself.
- Use one outer Inspector outline. Category and section rows are full bleed within it and do not each draw redundant left/right borders.

### Category and section hierarchy

- Standard class category: compact color-aware vertical gradient, brighter top edge, darker bottom edge, left-aligned class name/icon, no collapse arrow.
- Custom exported category: strong but quieter full-width divider; uppercase is author-controlled, not forcibly transformed.
- Group: subtle solid surface, collapsible, no gradient or bottom separator, small neutral property-count badge.
- Subgroup: quieter surface and font, collapsible, one depth farther right.
- Path-generated nested section: use the subgroup visual language but preserve its actual nesting depth.
- Hover changes only the relevant header surface. Focus must remain keyboard-visible.
- Guide rails and property content use the same depth token as their owning header.

### Properties

- Labels remain handled by `EditorProperty`; editors remain handled by their existing specialized classes.
- Standard width: label left and a bounded editor cluster right.
- Narrow width: use the existing bottom-editor path for complex/vector controls rather than allowing the label to crush numeric values.
- Revert stays immediately before the editor cluster, matching the prototype, while pin/key/delete/check controls preserve their behavior.
- Scalar controls gain a subtle resting border through Inspector-specific theme variations.

### Vectors

- Improve the existing `EditorPropertyVectorN`; do not create a second vector editor.
- Use the compact `EditorSpinSlider` metrics already proven by the gallery.
- Give X/Y/Z/W compact colored label chips and bordered numeric areas.
- Preserve click-drag numeric adjustment, direct text entry, linked-ratio behavior, degrees/suffix handling, integer editing, deferred drag mode, and keyboard focus.
- Preserve a responsive fallback for Vector4 and very narrow panes.

### Resource-valued fields

- Add a compact display mode to `EditorResourcePicker` and use it from `EditorPropertyResource`.
- Render the real small preview at a fixed row-sized square while retaining the resource name beside it.
- Preserve drag/drop, quick load, edit/inspect, clear, copy/paste, save, make-unique, recursive uniqueness, specialized Script/Shader/Audio pickers, and sub-inspector expansion.
- Derive a short trailing status from the picker's existing ownership/reference calculation. Candidate states are `unique`, `shared ×N`, and `external`; the tooltip remains the authoritative detailed explanation.
- At narrow widths, status text yields before the resource name and menu controls.

## Proposed architecture

### 1. Preserve semantic roles in the live model

Add explicit visual roles instead of inferring appearance from nesting:

- `EditorInspectorCategory::Kind`: `CLASS`, `CUSTOM`, `FAVORITES`.
- `EditorInspectorSection::Kind`: `GROUP`, `SUBGROUP`, `PATH`, `ARRAY`, `FAVORITES`.

Pass the role and semantic depth from `EditorInspector::update_tree()` when each object is created. Continue using the existing section path for collapse persistence and plugin callbacks.

This is the smallest structural change that allows correct styling of `@export_category`, `@export_group`, and `@export_subgroup`.

### 2. Reuse and evolve the standardized header component

`EditorSectionHeader` already provides title, description, badge, collapse state, and actions. Extend it with:

- a visual-role enum;
- hierarchy depth;
- optional leading icon;
- compact density;
- a secondary/status text slot distinct from a badge.

Do not immediately delete the Inspector classes' behavior. Refactor `EditorInspectorCategory` and `EditorInspectorSection` to own/use the standardized header for presentation while retaining copy/paste, docs, revert, keying, checkable-group, drag-unfold, and accessibility orchestration in the Inspector classes.

If action parity makes direct composition too risky in the first patch, land the role/depth theme contract first and move the drawing into `EditorSectionHeader` in the next patch. Do not ship two unrelated sets of visual constants.

### 3. Centralize Inspector density and hierarchy tokens

Add named theme tokens for category/group/subgroup fills, borders, hover, font roles, heights, depth step, guide rail, property inset, field border, compact vector metrics, and resource preview size.

Define them for both modern and classic themes. The gallery should consume the same tokens or component variants after the live implementation; prototype-only copies should then be removed.

### 4. Pin Node2D/Node3D as the first category

Create an Inspector-only stable reorder helper over the copied `List<PropertyInfo>` before visual construction:

1. Detect the standard category whose name is exactly `Node3D` for Node3D-derived objects, otherwise `Node2D` for Node2D-derived objects.
2. Move that category and its property block, ending at the next category marker, ahead of other rendered category blocks.
3. Keep order inside the block unchanged, so Transform remains first and Visibility follows it.
4. Leave Resources and non-transform node families unchanged.
5. Handle `MultiNodeEdit` only when the common edited class exposes one of those standard category blocks.

The helper must preserve custom categories, groups/subgroups, `script`, metadata, plugin property editors, documentation class context, filters, and favorites. If plugin callback-order tests reveal a compatibility issue, perform the same stable move on completed category blocks instead of the property descriptors.

### 5. Apply field changes through existing editors

- Add Inspector-specific field styling through theme variations and `EditorProperty` layout tokens.
- Apply compact vector mode inside `EditorPropertyVectorN` using `EditorSpinSlider`'s existing compact APIs.
- Add compact resource mode to `EditorResourcePicker` using `p_small_preview`.
- Keep specialized property classes and plugin-provided `EditorProperty` implementations intact.

## Generic components worth creating or extending

### Extend: `EditorSectionHeader`

This is the strongest reusable result of the exploration. A role/depth-aware header applies beyond the Inspector to forms, cards, settings sections, and future dock content. It should become the single source for hierarchy header spacing, badge/status placement, hover/focus, and collapse affordances.

### Extend: `EditorResourcePicker`

Add a compact display mode and small-preview sizing API. This remains a resource picker capability, not an Inspector-only wrapper, and benefits any compact property or form surface.

### Reuse: `EditorSpinSlider`

The necessary generic compact metrics already exist from the prototype. The live work should use them rather than add `InspectorCompactSpinSlider`.

### Reuse: `EditorPropertyVectorN`

It is already the generalized Vector2/3/4 implementation. Add a compact visual mode there rather than create axis-field components that duplicate linking, value conversion, and signal behavior.

### Do not create

- No separate `InspectorResourceField`; it would duplicate `EditorResourcePicker`.
- No separate live `InspectorChromeHeader`; evolve `EditorSectionHeader` and the existing Inspector classes.
- No universal replacement input control; theme existing LineEdit, SpinSlider, OptionButton, and specialized editors.
- No generic outer-outline widget; a themed `PanelContainer`/Inspector panel style is sufficient.

## Implementation phases

### Phase 1 — Semantic hierarchy and standardized header foundation

- Add category/section role enums and explicit depth.
- Extend `EditorSectionHeader` role/depth/status/icon support with component tests.
- Wire class category, custom category, group, subgroup, and path roles.
- Preserve collapse state, context menus, copy/paste, checkable groups, keying, reverts, drag-hover unfold, RTL, and accessibility.
- Apply full-width hierarchy styling and guide rails in modern and classic themes.

Gate: a scripted object containing category, group, subgroup, path nesting, checkable groups, and modified values behaves identically before/after except for presentation.

### Phase 2 — Inspector shell and property-row geometry

- Replace the dock filter with `EditorSearchBar`.
- Publish the filtered visible-property count from `EditorInspector` to the search bar.
- Add the single outer outline and remove redundant child side borders.
- Add semantic property insets and standard/narrow responsive geometry to `EditorProperty`.
- Restyle scalar/text/enum fields through Inspector theme variations.

Gate: 260, 300, and 400 px panes remain usable with no clipped values or inaccessible action icons.

### Phase 3 — Compact vectors

- Move gallery compact metrics into `EditorPropertyVectorN`.
- Preserve all vector functionality and add narrow-width fallback.
- Cover Vector2/2i/3/3i/4/4i, linked scale, degree conversion, suffixes, and large/negative values.

Gate: mouse dragging, text entry, undo/redo, keying, reset, link ratios, and keyboard traversal all pass.

### Phase 4 — Compact resources and nested resources

- Add `EditorResourcePicker` compact mode using the real small preview.
- Add status text from existing resource-count/internal/external state.
- Validate Script, Shader, AudioStream, Material, Texture, external, built-in, shared, unique, read-only, and null resources.
- Retheme nested sub-inspectors to maintain hierarchy without double outlines.

Gate: every existing picker command and nested Inspector action remains available.

### Phase 5 — Transform category pinning

- Add the stable Node2D/Node3D category reorder.
- Test scripted derived nodes, native derived nodes, MultiNodeEdit, filters, favorites, and plugins.
- Keep Node3D/Node2D collapse state and property paths unchanged.

Gate: Transform is first for Node2D/Node3D-derived objects, while resources and other objects retain their order.

### Phase 6 — Prototype convergence and cleanup

- Make the gallery use production components and tokens.
- Remove prototype-only header/resource/vector drawing that has a live equivalent.
- Add screenshots/manual QA matrix and optimized-build verification.

## Known gaps and risks

- Group/subgroup semantics currently disappear at section creation; role propagation is mandatory.
- `section_depth` from property hints and slash-path nesting must compose with semantic group depth rather than overwrite it.
- Filtered results and favorites rebuild sections independently; their counts, roles, and indentation need explicit coverage.
- Property-count badges need a defined rule. Recommended: visible descendant property count; modified count is separate status text and takes precedence when space is constrained.
- Resource `unique/shared` language must match actual internal/external/reference semantics; do not infer it only from a resource name or path.
- Async previews must reject stale callbacks and must not increase compact row height.
- Nested sub-inspectors currently add their own colored borders/backgrounds; the outer-outline design must avoid doubled frames.
- Arrays, dictionaries, custom plugin controls, sectioned inspectors, Project Settings, remote/debugger inspectors, and skeleton/bone inspectors reuse parts of this code and require regression coverage.
- Refactoring headers must retain accessibility roles/actions and keyboard focus, not merely mouse behavior.
- Both modern and classic themes must receive valid tokens; hard-coded dark-theme colors are not acceptable.
- High DPI, RTL, localization, long property names, and editor scale can invalidate pixel assumptions from the gallery.

## Verification matrix

- Object types: scripted Node3D, scripted Node2D, Control, plain Node, Resource, nested Resource, MultiNodeEdit.
- Hierarchy: class category, custom category, group, subgroup, slash path, array, dictionary, favorites.
- Property behavior: modified/reset, pin, key, checkable, read-only, warning, delete, copy/paste, documentation.
- Fields: integer, float, text, enum, boolean, Vector2/3/4 and integer variants, color, path, node path, resource.
- Resources: Material and Texture preview, Script, Shader, AudioStream, null, external, built-in, shared, unique, nested.
- Layout: 260 px, 300 px, 400 px, wide dock; 100%, 125%, 150%, 200% scale; long localized labels; RTL.
- Themes: modern dark/light and classic dark/light.
- Automation: component tests, new Inspector hierarchy/order tests, `git diff --check`, test-enabled editor build, optimized `focus_save` build, and manual gallery/live comparison.

## Recommended first implementation slice

Land Phase 1 plus the Node2D/Node3D reorder helper behind tests, then inspect it in the live Inspector before changing fields. This gives the largest structural gain—correct categories, groups, subgroups, and transform placement—while keeping field and resource behavior unchanged. Once the hierarchy is accepted, apply property density, vectors, and resources incrementally.
