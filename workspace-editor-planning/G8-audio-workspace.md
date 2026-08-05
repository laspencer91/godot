# G8 — Audio workspace: the AudioEvent authoring surface

**Status: Design (2026-08-05). Nothing implemented.**

An in-engine alternative to FMOD Studio's event editor, built as a first-class *document
surface* on the landed G1–G3 workspace architecture and as the **first real adoption of the
`editor/gui/components/` design system** (commit `590d69f060`, currently gallery-only).

The runtime model this authors against (AudioEvent resource, built-in / event-local /
shared-preset / global parameters, spatial context inferred from emitter+listener, bindings
from parameters to audible properties) is specified in the Scrapline-side conversation doc;
this document is the **editor/UX design**: what the surface looks like, which components build
it, what new components the design system needs, and the interaction grammar that makes
authoring intuitive.

Game-side context: Scrapline already has a proto-event layer (`SfxId` + `AudioCue` +
`AudioManifest` + `AudioDirector.play_at/play_1p`, occlusion in `audio_occlusion.gd`, reverb
in `acoustics_manager.gd`). `docs/audio/AUDIO_BUILD_PLAN.md` Phase 3 reserves "GUI tooling" —
this is that phase, moved into the engine where it belongs.

---

## 1. Placement in the shell

**An AudioEvent opens as a workspace document tab, not a mode and not a global dock.**

- Double-clicking `shoot_assault_rifle.tres` (type `AudioEvent`) in Explore opens an
  `AudioDocument` tab via `EditorMainScreen::reveal()`, exactly like `ShaderDocument`.
  It splits, drags, and persists like every other tab. There is no "Audio main screen".
- The **Deck** (signal-chain editor) is a *document-owned bottom drawer* via
  `DocumentBottomDockHost` — the G7 pattern, Animation being the reference implementation.
  It belongs to the event you're editing; two audio tabs in two panes each carry their own
  Deck state. It is explicitly **not** a project-global bottom panel.
- The stock `EditorAudioBuses` mixer stays where it is (global bottom dock) — buses are
  project-global; events are documents. The Deck's Output module deep-links into it.
- Explore integration: a new `AudioEvent` category/type filter, waveform-thumbnail previews
  for clips (reuse `AudioStreamPreviewGenerator`), and **click-to-audition** in the Details
  side (spacebar plays the selected clip/event with default parameter values).

Plumbing follows the ShaderDocument precedent exactly (append-only `TYPE_AUDIO`, factory +
dedup on `EditorData`, branch in `get_or_create_document_for_path()` **before** the generic
resource fallback, surface provider `SNAME("audio")` registered in
`register_editor_document_surface_providers()`, opened from an `EditorPlugin::edit()`).
See §9 for the full recipe with file/line anchors.

## 2. Anatomy of the surface

```
┌ EditorPaneHeader ────────────────────────────────────────────────────────────┐
│ ♪ Shoot Assault Rifle   ● Unsaved      [▶ Play][■][⟳ Loop] | [meter] | [Save]│
│   res://audio/events/weapons/shoot_assault_rifle.tres                        │
├ HSplit ──────────────┬────────────────────────────────┬──────────────────────┤
│ PARAMETERS (rail)    │ PAGES                          │ INSPECTOR (rail)     │
│ [search bar]         │ ┌ Start │ occlusion │ n_dist │+┐│ EditorForm for the  │
│ ▾ Built-in    (card) │ │                              ││ current selection:  │
│   distance    ═◦═ 7m │ │  Start page: track list      ││ track, instrument,  │
│   occlusion   ═◦═0.2 │ │  Parameter page: curve view  ││ effect, binding, or │
│   direction   (ro)   │ │  Timeline page: on demand    ││ parameter def       │
│ ▾ Event       (card) │ │                              ││                     │
│   perspective [1P|W] │ │                              ││                     │
│   suppressed  [ off] │ │                              ││                     │
│   fire_mode   [AUTO] │ │                              ││                     │
│ ▸ Shared      (card) │ │                              ││                     │
│ ▸ Global      (card) │ └──────────────────────────────┘│                     │
├ Deck (DocumentBottomDockHost drawer) ────────────────────────────────────────┤
│ Scope: [Event Master ▾]   Input → [EQ] → [Compressor] → [Spatializer] → Out  │
└──────────────────────────────────────────────────────────────────────────────┘
```

Concrete component mapping:

| Region | Built from |
|---|---|
| Header | `EditorPaneHeader` — title = display name, subtitle = `res://` path, `set_dirty()` wired to document lifecycle. Transport actions (`Play`, `Stop`, checkable `Loop`) as `EditorAction`s in the `primary` group; `Save`/options in `secondary`. A slim output meter as `add_custom_control` (see `EditorMeter`, §6). |
| Parameters rail | `EditorSearchBar` + four `EditorCard`s (`Built-in`, `Event`, `Shared`, `Global`), collapsible, badge = count. Rows are the **live preview scrubbers** (§5). Card header action `+` on Event/Shared adds a definition. Empty Shared/Global cards collapse to a single quiet row, not an `EditorEmptyState` — the rail must stay dense. |
| Page strip | `TabBar` (same `tabbar_background` styling as `TabbedDocumentHost`) + a `+` button. Tabs: `Start`, one per *bound* parameter, `Timeline` when present. |
| Start page | Track list (§3). |
| Parameter page | `EditorCurveView` stack + range strips (§4). |
| Inspector rail | `EditorForm` / `EditorFormSection` / `EditorFormRow` for whatever is selected. Validation via `EditorFormRow::set_status`; event-level problems (missing clip file, unreferenced parameter) via `EditorStatusPanel` at the top of the rail. |
| Deck | Document bottom drawer; horizontal chain of compact module cards (§5). |

The two rails are the panes of an `HSplitContainer` nesting and both are collapsible; on a
narrow pane the Inspector rail collapses first (selection properties are also reachable via
the stock Inspector), then the Parameters rail collapses to icons. `EditorResponsiveRow`
already gives the header/toolbars graceful wrapping.

## 3. Pages: Start, parameter pages, timeline

Sheets are **views over the same event**, not separate runtime subsystems.

- **Start** is implicit and always first. It lists tracks; each track row shows:
  drag-handle · name · instrument cell(s) · condition chips · mute/solo · gain field.
  An instrument cell for a multi-instrument shows "6 clips ▾ shot_01…shot_06" with a
  mini-waveform strip; clicking expands clip rows inline (weights, per-clip trim).
  Conditions render as chips: `suppressed == false`, `perspective == FIRST_PERSON`.
  Clicking a chip focuses the condition in the Inspector; the chip's parameter name is a
  link to that parameter's page.
- **A parameter page is created automatically** the first time something binds to that
  parameter (§5 "Bind to…"), and removed automatically when its last binding dies (with
  undo). Authors never manage pages; pages reflect reality. The tab shows the parameter
  name and a small kind glyph (built-in ◇, event ●, shared ◈, global ⬤).
- **Timeline appears only on demand** ("+ ▸ Timeline"): for start offsets, sequencing,
  sustain/loop points, transitions. Time is rendered by the same `EditorCurveView` with
  x = seconds; it is deliberately *not* the home for value-driven curves. A UI click never
  grows a timeline; the rifle grows one only when the mechanical layer starts 20 ms late.

This keeps the FMOD behaviors (action/parameter/timeline sheets) without cloning the
sheet taxonomy into the data model: internally everything is `event-start condition →
instrument`, `parameter range → enable`, `parameter curve → property`, `timeline position →
instrument/property`.

## 4. The curve experience — `EditorCurveView`

The single most important new piece. Requirement: a **wide-pixel-space** curve editor —
the document surface's center, not an inspector strip.

### Why neither existing editor is reused directly

- `CurveEdit` (`editor/scene/curve_editor_plugin.h:42`) is inspector-scale by design:
  hard-coded 6:13 aspect (`ASPECT_RATIO`, `:107`), no pan/zoom, one curve, no box-select,
  no axis units. Right for a thumbnail, wrong as a primary surface.
- `AnimationBezierTrackEdit` (`editor/animation/animation_bezier_editor.h`) has exactly the
  right interaction model — pan/zoom both axes, multi-curve overlay with focus dimming,
  box-select, tangent modes, snapping — but is hard-coupled to `Animation` tracks, key
  indices, and the track editor's timeline header.

**Decision: new component `editor/gui/components/editor_curve_view.{h,cpp}`**, registered
and themed like its siblings (`EditorComponentTheme::populate`). It *ports* `CurveEdit`'s
point/tangent hit-testing and collision math and *adopts* the bezier editor's navigation
grammar. `CurveEdit` itself stays untouched for inspectors; `CurvePreviewGenerator` renders
the mini-thumbnails used on bound fields and page tabs.

### Data model

`set_curve(Ref<Curve>)` plus a **domain spec** — this is what makes it an audio curve editor
rather than a unit-square widget:

```
x: label ("occlusion"), range (0..1 | 0..max_distance), unit ("", "m", "s"), scale linear
y: label ("Low-pass cutoff"), range (20..20000 Hz | 0..-60 dB | 0..1), unit, scale linear|log
```

Storage stays normalized in `Curve` (0..1); the binding record owns the mapping. The view
renders **real units on both axes**: a cutoff curve shows `20 kHz → 700 Hz` on a log-scaled
y-axis, a gain curve shows dB linearly (already perceptual). Grid lines land on meaningful
values (octaves for Hz, 6 dB steps for gain, meters for distance) — this is the difference
between "a curve" and "a decision an audio designer can read".

### Interaction grammar

| Action | Gesture |
|---|---|
| Add point | double-click on/near the curve |
| Move point / handle | drag; `Shift` axis-locks; arrow keys nudge selection |
| Tangents | drag handles; per-point mode free / linear / balanced / mirrored (context menu) |
| Select many | box-drag on empty space; `Ctrl`-click toggles |
| Pan / zoom | middle-drag pan; `Ctrl`+wheel zoom-x, `Shift`+wheel zoom-y, wheel both; `F` frames curve; `Shift+F` frames selection |
| Snap | toggle + step config in the view's corner toolbar (reuse `CurveEditor`'s snap/preset toolbar pattern); snapping is in *display units* (0.5 m, 1 dB, 1 semitone) |
| Precise entry | selecting a point surfaces x/y `EditorSpinSlider`s in the corner toolbar, in display units |
| Presets | constant / linear / ease-in / ease-out / smoothstep, plus audio presets: inverse-square rolloff, log taper |

### Overlay + live cursor

A parameter page stacks every binding on that parameter in **one** view: the focused
binding at full opacity with handles, the rest ghosted (~25% alpha, no handles); each has a
lane label at its right edge ("Low-pass cutoff", "Event gain", "Reverb send"). Clicking a
ghost focuses it. Y-axes differ per binding, so the axis gutter always shows the *focused*
binding's units — the ghosts communicate shape correlation, not absolute values.

A vertical **live cursor** marks the parameter's current preview value (from the rail
scrubber, §5, or from the running transport), with a badge showing the focused binding's
mapped output ("−9.2 dB"). This closes the loop: drag the occlusion scrubber, hear the
muffle, watch the cursor ride the curve.

Below the curve view, discrete behavior on the same axis renders as **range strips** — one
slim row per conditional track (`Blast: [0 … 0.6]`), draggable ends, same x-transform as
the curves. Continuous and discrete responses to a parameter live on one page, one axis.

For enum/bool parameters the page has no curve; it is columns per value (FIRST_PERSON /
WORLD) with per-track enable toggles — a matrix, not a graph.

## 5. The Deck and the bindable-field pattern

### Deck

Scope selector at left (`Event Master ▾` / any track — selecting a track on the Start page
also retargets the Deck, with a lock toggle to pin scope). The chain is a horizontal row of
**compact module cards** (`EditorCard` with a new dense variation, §6): title, bypass
toggle, 3–6 key fields, overflow into the Inspector rail on selection. `+` inserts an
effect; drag reorders; `Output` module shows the routing target (bus) and a meter, and
links to the global bus mixer.

The drawer is zero-min-size (per `DocumentBottomDockHost` design) so opening the Deck never
resizes the neighboring rails.

### Fields, not knobs

Skeuomorphic knobs read as "audio software" but drag poorly with a mouse and waste the
design system. **Every numeric on a module is an `EditorSpinSlider`-based field**: drag to
scrub, click to type an exact value, `Ctrl+click` (or context ▸ Reset) restores default.
This keeps the entire Deck keyboard-accessible and visually native.

The audio-specific addition is **bindability**, as a wrapper component (`EditorBindableField`,
§6). States:

- **Unbound**: plain field + a quiet ◇ affordance on hover.
- **Bound**: the field's value area is replaced by a curve micro-thumbnail
  (`CurvePreviewGenerator`) + the live computed value; accent-tinted ◆. The field is no
  longer directly editable — clicking it **jumps to the parameter page with that binding
  focused** (editing the curve *is* editing the value).
- Context menu: `Bind to ▸` (submenu: built-ins, event params, shared, globals, `New event
  parameter…`), `Unbind (bake current value)`, `Go to curve`, `Reset`.

Choosing `Bind to ▸ occlusion` on the low-pass cutoff: creates the binding with a sensible
default curve (property-aware — cutoff defaults 20 kHz→700 Hz descending, gain 0→−14 dB),
auto-creates the `occlusion` page if absent, switches the center view to it, focuses the
new curve. One gesture from "knob" to "authored behavior" — this is the signature flow of
the whole tool and the one to get *feeling* right first.

### Preview model (the Parameters rail is live)

The rail rows double as the preview harness, which is what lets the runtime own spatial
facts without making them un-auditionable:

- **Built-ins** are read-only to gameplay but **scrubbable in preview**: distance gets a
  slider over 0..event-max (in meters), occlusion 0..1, relative speed a small bipolar
  slider. A `⟲` per row returns it to "simulated" (default) state.
- **Event/shared enums** render as segmented buttons (`FIRST_PERSON | WORLD`), bools as
  toggles, continuous as fields.
- Transport `Play` starts the *actual runtime* event instance with the rail's values
  injected; scrubbing while playing updates the instance live (this is also the API's
  live-parameter test). Meter and curve cursors follow.
- Later (not v1): "Audition at camera" — drive built-ins from the editor 3D camera against
  a picked emitter node instead of sliders.

## 6. Design-system deltas

This surface is the library's first real consumer and its density stress test. New
components (all in `editor/gui/components/`, themed in `editor_component_theme.cpp`,
documented in `doc/classes/`, covered in `tests/editor/test_editor_components.cpp`, added
to the gallery):

| Component | Role | Notes |
|---|---|---|
| `EditorCurveView` | wide-space curve editing | §4; the flagship. Generic (x/y domain spec), so shaders/gameplay tools inherit it. |
| `EditorBindableField` | spin-slider + bind states | §5. Generic "this property can be driven" pattern — future use: shader params, VFX. |
| `EditorChip` | condition/tag chips | Generalize the badge already inside `EditorSectionHeader`/`EditorPaneHeader` dirty-chip into a standalone, optionally clickable/deletable chip. |
| `EditorSegmented` | small enum switch | v1 fallback: `OptionButton` with `EditorFieldOptionButton` variation; promote to a real segmented control when the rail proves it. |
| `EditorMeter` | level meter | Wrap `EditorAudioMeterNotches` + bus VU logic from `editor_audio_buses.cpp` into a reusable horizontal/vertical meter. |

Tuning the existing system (things adoption will force; do them as they bite, log in the
divergence ledger):

1. **Spacing scale is ad-hoc** — the component table uses 0/3/4/5/6/8/9/10/12/14. Collapse
   toward a 4/8/12(/16) ladder as surfaces adopt; this document's density problems are the
   test cases. Keep the rule: *no `add_theme_constant_override` in components — variations
   + the table only.*
2. **`EditorCard` needs a dense variation** (`EditorCardCompact`): reduced header padding
   (9→6 v), no description row, tighter body margins (12→8). Needed by Deck modules and the
   Parameters rail; forms keep the current comfortable metrics.
3. **`EditorFormRow` label column** (`label_minimum_width` 140) is too wide for a 260 px
   rail — the rail uses its own row composition (label-above on narrow), which is fine, but
   consider a `label_minimum_width` per-variation override so rails can use forms too.
4. **Checkable `EditorAction`** already exists (`checkable`/`checked`) — Loop and the Deck
   bypass toggles are its first real exercise; verify the visual checked state of
   `EditorActionButtonFlat` reads clearly at toolbar size.

## 7. Signature flows (acceptance script)

1. **Clips → event**: multi-select `shot_01..06.ogg` in Explore ▸ right-click ▸ *Create
   Audio Event* → new `.tres` beside them, opens as a tab, Start page has one track with a
   6-clip multi-instrument. Press Play: it round-robins. Zero dialogs.
2. **Suppressed variant**: duplicate the Blast track, swap clips, add condition chip
   `suppressed == true` (creating the bool event parameter inline from the chip editor),
   original gets `== false`. Toggle `suppressed` in the rail while looping — audible swap.
3. **Occlusion muffle**: Deck ▸ EQ module ▸ low-pass cutoff ▸ `Bind to ▸ occlusion` →
   lands on the occlusion page, default 20 kHz→700 Hz curve focused; drag the rail's
   occlusion scrubber while looping; the cursor rides the curve and the muffle tracks.
   Add a second binding (event gain 0→−14 dB) and see it ghosted on the same page.
4. **Distance layers**: near/far tail tracks with range strips on `normalized_distance`,
   crossfade curves overlaid; scrub distance in meters and watch strips light up as their
   range contains the cursor.
5. **Splits behave**: drag the event tab into a right split next to `weapon_config` tabs;
   Deck follows its document; a second event in the left pane keeps its own Deck scope,
   page, and rail state (`capture_view_state`/`apply_view_state`).

If each of these runs without touching a modal dialog or the stock Inspector, the design
has met its bar.

## 8. Phasing

- **P1 — skeleton**: `AudioDocument`/provider/plumbing; pane header + transport playing a
  flat multi-instrument; Start page track list; Inspector rail via forms; save/dirty.
- **P2 — parameters + curves**: parameter rail (event-local + built-ins with scrub
  preview), `EditorCurveView`, `EditorBindableField` on a minimal Deck (gain + one EQ),
  bind flow, auto pages.
- **P3 — deck + routing**: full module chain, per-track scope, spatializer module (unit
  size / max distance / attenuation from `AudioStreamPlayer3D` semantics), bus routing +
  `EditorMeter`, condition matrix pages for enums.
- **P4 — timeline + library**: on-demand timeline page, shared parameter presets library,
  global parameters in project settings, Explore audition polish.

Engine-tree note: implement on `dev/tools/godot` (fork-master); anything the game's gates
must see goes to `godot-box3d` per the two-tree working agreement.

## 9. Implementation anchors (from the 2026-08-05 exploration)

- Document enum + subclasses: `editor/editor_document.h:55-65` (append `TYPE_AUDIO`),
  `ShaderDocument` at `:242` is the template; `opens_as_workspace_tab() → true`.
- Factory/dedup: pattern at `editor/editor_data.cpp:979-998`
  (`get_or_create_shader_document`); session-restore branch in
  `get_or_create_document_for_path()` at `editor_data.cpp:1065-1101` **before** the generic
  resource fallback.
- Surface: derive `EditorBuiltinDocumentSurfaceInstance`
  (`editor/gui/editor_document_surface.cpp:64`); provider registration at `:1073-1099`;
  default-surface mapping at `:1043-1065`.
- Open path: `ShaderEditorPlugin::edit()` at `editor/shader/shader_editor_plugin.cpp:181-194`;
  reveal logic `editor/editor_main_screen.cpp:472-527`.
- Deck host: `DocumentBottomDockHost` (`editor/gui/document_bottom_dock.h:44`), toggle
  buttons mount via `DocumentView::get_toolbar_host()`.
- Component library + theme table: `editor/gui/components/`,
  `editor/themes/editor_component_theme.cpp:27-213`; registration
  `editor/register_editor_types.cpp:192-204`; gallery
  `editor/plugins/editor_component_gallery_plugin.cpp:50-176`.
- Curve math to port: `editor/scene/curve_editor_plugin.h:42-146`
  (`CurveEdit` hit-testing/tangents); navigation grammar:
  `editor/animation/animation_bezier_editor.h`.
- Waveforms: `editor/audio/audio_stream_preview.h:38-56`
  (`AudioStreamPreview(Generator)`); meters: `EditorAudioMeterNotches` in
  `editor/audio/editor_audio_buses.h`.
- Runtime precedent for stream parameters: `scene/audio/audio_stream_player_internal.cpp:243`
  (parameter forwarding, used by `AudioStreamInteractive::switch_to_clip`).
- Scrapline integration seams: `src/audio/audio_director.gd` (spatial facts / occlusion
  feed), `src/audio/audio_cue.gd` + `audio_manifest.gd` (the proto-events that become
  `AudioEvent` resources), known 2D/3D mismatch on the remote-fire path
  (`client_combat_fx.gd:62-66` plays `AUTO_1P_2D` cues via `play_at`) — the `perspective`
  parameter replaces that ambiguity.

## 10. Decisions

**Resolved 2026-08-05 (logan):**

1. **`AudioEvent` runtime lives in the engine** (module, e.g. `scene/audio/` or
   `modules/audio_events/`). Scrapline's `AudioDirector` becomes the spatial/acoustics
   provider feeding built-ins (occlusion, distance); editor and runtime share one schema.
2. **Per-track spatial mode ships in v1.** Each track carries listener-relative vs
   positional on its spatializer slot — the rifle's FIRST_PERSON hybrid is a launch
   requirement, and this retires the `AUTO_1P_2D`-cue-played-3D ambiguity.
3. **`EditorCurveView` is a new design-system component** — port `CurveEdit`'s
   point/tangent math, adopt `AnimationBezierTrackEdit`'s navigation grammar; neither
   existing class is reused directly.
4. **Timeline scope is offsets + markers only** (start delays, sustain/loop points,
   transition markers). No clip arrangement / DAW-lite; time remains a special parameter.

**Still open (low urgency):**

- **`EditorSegmented`** — build the real control in P2, or ship v1 on `OptionButton`
  (`EditorFieldOptionButton` variation) and decide after using the rail for a week.
