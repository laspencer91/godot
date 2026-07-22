# CSG Phase 3B — Edit-Domain Host, Tri-State Input Arbitration, and Center Chrome Slots

**Scope:** Introduce the generic edit-domain layer (`EditorEditDomainRegistry` / `EditorEditDomainProvider` / `EditorEditDomainSession` / `EditorEditDomainHost`), give each 3D pane one host with a per-view domain session, add the single tri-state input seam to `Node3DEditorViewport::_sinput` at the scout's insertion point, add `SLOT_CENTER_LEFT` / `SLOT_CENTER_RIGHT` to `EditorViewportChrome`, and prove the whole contract with a DEV-only dummy domain plus a headless registry/tri-state test. **No CSG tool logic, no scheduler, no face selection.** This lands the seam Phase 4's Surface tool attaches to.

Guiding principle (mirrors the Phase 4 pre-step): **the generic host/registry code is CSG-agnostic and lives in `editor/gui/`; the only edits to viewport core are one arbitration call plus a handful of `&& !suppressed` guards at named sites; every native behavior (navigation, RMB menu, freelook, Space, region-select) is preserved byte-for-byte when no domain is active.** Constraint: touch `editor/**` only (plus `doc_classes` if a new class is exposed — none are for 3B); nothing under `modules/`.

---

## 1. Current-state map (verified 2026-07-22 against the live tree)

All refs `editor/scene/3d/node_3d_editor_viewport.cpp` unless noted. Line numbers re-verified against the scout; the tree has drifted ≤ a few lines and the structure is intact.

**Input pipeline (`_sinput`, entry at `:2624`):**
- Numpad emulation `:2627`; DnD guard `:2634`; vertex-snap fork mode owns its own Escape/LMB/RMB with early `return`s `:2642-2766`.
- Plain-RMB focus grab `:2770-2780` (`is_plain_context_click` computed `:2771-2774`).
- **Global-plugin forwarding block `:2782-2816`** — `after` flag declared `:2782`; `get_editor_plugins_force_input_forwarding()->forward_3d_gui_input` `:2789`; `get_editor_plugins_over()->forward_3d_gui_input` `:2803`; STOP→`return`, CUSTOM→`after = AFTER_GUI_INPUT_CUSTOM`. **Block closes at `:2816`.**
- **Plain-RMB context menu / gizmo cancel `:2818-2831`** (`_popup_context_menu` `:2829`).
- **Navigation `:2833-2838`** (`view_3d_controller->gui_input`; `return` if a gesture just ended).
- Mouse-button switch `:2849`:
  - RIGHT `:2850` (gizmo restore, nav-modifier guards `:2860-2866`, alt-RMB `_list_select` `:2870`, transform cancel, freelook enable `:2886-2888`).
  - LEFT press `:2942`: instant/reposition commit `:2945`; nav-modifier guards `:2949-2955`; ruler `:2957`; list-select `:2963`; `can_select_gizmos` computed `:2973-2980`; **gizmo handle pick `:2983-3011`**; **transform-gizmo select `:3013-3016`**; **subgizmo pick `:3018-3062`**; `clicked = ObjectID()` `:3064`; **object selection gate `if (after != AFTER_GUI_INPUT_CUSTOM)` `:3068`** → `_select_ray` `:3070`, sets `selection_in_progress` `:3074`, region-select init `:3111-3116`, `begin_transform` `:3106`; second `begin_transform` `:3155`; `surface->queue_redraw()` `:3160`.
  - LEFT release `:3161`: gizmo-handle commit `:3174-3186`; **release selection gate `if (after != AFTER_GUI_INPUT_CUSTOM)` `:3188`** → `selection_in_progress = false` `:3189`, `_select_clicked` `:3192`, `_select_region` `:3194`; transform commit `:3202-3225`.
  - Motion `:3234` (guarded `!_edit.instant`): vertex-snap `:3240-3278`; **selected-object gizmo hover `:3280-3312`**; **transform-gizmo hover `:3314-3316`**; handle drag `:3318`; region drag start `:3327-3331` / update `:3333-3337`; transform drag start `:3339-3348`.
  - Keyboard `:3360`: freelook Escape `:3420-3423`; gizmo-handle Escape restore `:3425-3430`; **DEFAULT Escape `_clear_selected()` `:3431-3434`**; transform axis locks `:3438+`.

**Per-view / global split (scout, re-confirmed):**
- Per-viewport state on `Node3DEditorViewport`: `surface` (`node_3d_editor_viewport.h:334`), `_edit` (`:505`), `clicked` (`:423`), `selection_in_progress` (`:428`), `transform_gizmo_visible`, and `editor_view` back-pointer (`:559`).
- Focused-pane enforcement: `_surface_mouse_enter` `:2195-2214` — the `WorkspacePane::of(surface)` / `get_focused_pane() != pane` check `:2204-2209` is the canonical "only the active pane accepts input" gate.
- Selected-object node-attached gizmos (`get_gizmos()` at `:2984`, `:3021`, `:3281`) render in the **shared** `World3D` → visible/pickable in every pane of that world (**KEY GAP**). The transform manipulator is per-viewport (`transform_gizmo_visible`, `update_transform_gizmo_view`).

**Pane / host ownership:**
- `Node3DEditorView` (`node_3d_editor_plugin.h:606`) is the pane-level surface: owns `viewport_chrome` (`:612`), the viewport quad `viewports[]` (`:613`), `document` (`:616`); ctor creates the chrome with `Dictionary{ "view": this }` (`node_3d_editor_plugin.cpp:2797-2800`); `activate_viewport_chrome()` (`:2814`) is called after viewports build (`:2831`); dtor at `:2803`. Pane-hosted scenes force single-viewport (`create_view_bound_to` `:2845`).
- Each `Node3DEditorViewport` reaches its pane via `editor_view` (`:559`). **The host belongs on `Node3DEditorView`** (one per pane, same scope as the chrome); each viewport routes its events to `editor_view`'s host and passes itself as the originating viewport.

**Chrome (`editor/gui/editor_viewport_chrome.{h,cpp}`):**
- `enum Slot` has 6 corner slots + `SLOT_MAX` (`.h:95-103`). `slots[SLOT_MAX]`, `control_orders[SLOT_MAX]` (`.h:109-110`).
- `_layout_slots()` positions each slot by a `switch (i)` with 6 cases + a no-op `default` (`.cpp:186-207`); unhandled slots would pile at `(0,0)`.
- `add_control` / `remove_control` are public (`.h:125-126`); `register_control_factory` validates `p_slot` via `ERR_FAIL_INDEX(..., SLOT_MAX)` (`.cpp:78`) → new slots are auto-covered.

**Registry lifecycle (`editor/register_editor_types.cpp`):** create order `EditorDocumentSurfaceRegistry::create()` `:159` → `register_editor_document_surface_providers()` `:160` → `EditorViewportChromeRegistry::create()` `:161`; symmetric teardown `:361-363`. This is where the domain registry create/free + provider registration hooks in.

**Test discovery:** `tests/SCsub:11,22` globs `*/**/*.cpp` recursively and force-links; a new `tests/editor/test_editor_edit_domain.cpp` using `TEST_FORCE_LINK(test_editor_edit_domain)` is picked up with **no SCsub edit** (same as 3A's `test_editor_document_surface.cpp`).

---

## 2. New files + edited files (type/method inventory)

### New: `editor/gui/editor_edit_domain.h`

The generic edit-domain contract. Mirrors `editor_document_surface.h`'s Context/Provider/Instance/Registry idiom exactly (typed context struct, borrowed stateless providers, `Object`-derived per-pane owner, manual-singleton registry with `create()/free()`).

```cpp
#pragma once
#include "core/object/object.h"
#include "core/templates/hash_map.h"

class Control;
class Camera3D;
class EditorDocument;
class InputEvent;
class Node3DEditorView;        // pane-level owner (fwd only; no plugin include in this header)
class Node3DEditorViewport;    // originating viewport (fwd only)
template <typename T> class Ref;

// CSG-3B: tri-state arbitration result (plan §5). Aggregation precedence CONSUMED > BLOCK > PASS.
enum class EditorEditDomainInput {
    PASS_TO_VIEWPORT,   // native viewport handling continues unchanged
    BLOCK_NATIVE_EDIT,  // native gizmos/selection must not react; navigation still native
    CONSUMED,           // the domain handled the event (accept_event + return)
};

// CSG-3B: the ONE context carrier. Same discipline as EditorDocumentSurfaceContext — a typed,
// borrowed struct; providers/sessions never own or retain these pointers. No parallel Dictionary
// shape: the chrome's existing Dictionary{"view"} reaches the host via Node3DEditorView, and this
// struct is what the host hands to providers and sessions. active_viewport is set per routed event.
struct EditorEditDomainContext {
    Node3DEditorView *view = nullptr;          // pane-level surface that owns the host
    EditorDocument *document = nullptr;        // bound document (may be null)
    Node3DEditorViewport *active_viewport = nullptr; // viewport that delivered the current event
};

// CSG-3B: one session per view. Owns domain-local state (tool, gesture, overlays, panels).
class EditorEditDomainSession : public Object {
    GDCLASS(EditorEditDomainSession, Object);
protected:
    static void _bind_methods() {}
public:
    virtual void enter(const EditorEditDomainContext &p_context) {}
    virtual void exit() {}
    virtual void retarget(const EditorEditDomainContext &p_context) {}   // stub in 3B

    // Core arbitration. Must return PASS for MMB/wheel/nav-modified/plain-RMB (keeps nav native).
    virtual EditorEditDomainInput handle_input(const EditorEditDomainContext &p_context,
            Camera3D *p_camera, const Ref<InputEvent> &p_event) { return EditorEditDomainInput::PASS_TO_VIEWPORT; }

    // Escape / Tab explicit entry points (host calls these; see §3 for why they are pre-consumed).
    virtual bool handle_escape() { return false; }   // true = consumed (cancel gesture, else exit)
    virtual bool handle_tool_toggle() { return false; } // Tab: toggle Surface/Operand; true=consumed

    // View-local overlay draw (host forwards from the viewport's _draw). No-op in 3B dummy beyond a marker.
    virtual void draw_overlay(Node3DEditorViewport *p_viewport) {}

    // Optional contextual controls the host mounts into chrome center slots (plan §6).
    virtual Control *build_tool_rail() { return nullptr; }        // SLOT_CENTER_LEFT
    virtual Control *build_contextual_panel() { return nullptr; } // SLOT_CENTER_RIGHT

    virtual ~EditorEditDomainSession() {}
};

// CSG-3B: stateless factory; never retained by the sessions it mints (3A discipline).
class EditorEditDomainProvider {
public:
    virtual StringName get_domain_id() const = 0;
    virtual bool is_available(const EditorEditDomainContext &p_context) const = 0;
    // hit is a lightweight ObjectID for 3B (real CSGSurfaceHit resolution is Phase 4).
    virtual bool can_activate_from_double_click(const EditorEditDomainContext &p_context, ObjectID p_hit) const { return false; }
    virtual EditorEditDomainSession *create_session(const EditorEditDomainContext &p_context) const = 0;
    virtual ~EditorEditDomainProvider() {}
};

// CSG-3B: manual-singleton registry, identical lifecycle idiom to EditorDocumentSurfaceRegistry
// and EditorViewportChromeRegistry (create/free in register_editor_types.cpp; symmetric shutdown).
class EditorEditDomainRegistry {
    static EditorEditDomainRegistry *singleton;
    HashMap<StringName, EditorEditDomainProvider *> providers; // borrowed
public:
    static void create();
    static void free();
    static EditorEditDomainRegistry *get_singleton() { return singleton; }

    bool register_provider(EditorEditDomainProvider *p_provider);
    bool unregister_provider(const StringName &p_domain_id, EditorEditDomainProvider *p_provider = nullptr);
    EditorEditDomainProvider *get_provider(const StringName &p_domain_id) const;
    // Returns the first available provider willing to activate from a double-click (double-click entry).
    EditorEditDomainProvider *find_double_click_provider(const EditorEditDomainContext &p_context, ObjectID p_hit) const;
    void get_available_providers(const EditorEditDomainContext &p_context, LocalVector<EditorEditDomainProvider *> &r_out) const;
};

// CSG-3B: one host per pane-level surface. Owns the active session; performs arbitration, chrome
// mounting, and provider-disappearance safety. Generic — knows Node3DEditorView/Viewport only by
// fwd decl and the typed context. Not an Object (plain heap owner, freed by Node3DEditorView dtor).
class EditorEditDomainHost {
    EditorEditDomainContext context;
    Control *chrome_host = nullptr;                 // EditorViewportChrome (as Control) for center slots
    StringName active_domain_id;
    EditorEditDomainProvider *active_provider = nullptr; // borrowed; validated before use
    EditorEditDomainSession *active_session = nullptr;   // owned
    Control *mounted_rail = nullptr;                // owned-by-chrome once mounted
    Control *mounted_panel = nullptr;

    void _mount_chrome_controls();
    void _unmount_chrome_controls();

public:
    void set_context(const EditorEditDomainContext &p_context) { context = p_context; }
    void set_chrome_host(Control *p_chrome) { chrome_host = p_chrome; }

    bool is_active() const { return active_session != nullptr; }
    StringName get_active_domain_id() const { return active_domain_id; }
    EditorEditDomainSession *get_active_session() const { return active_session; }

    bool enter_domain(const StringName &p_domain_id, Node3DEditorViewport *p_viewport);
    void exit_domain();
    // 3B entry: query providers for a double-click on p_hit; enter the first willing one.
    bool try_activate_from_double_click(Node3DEditorViewport *p_viewport, ObjectID p_hit);

    // Per-event arbitration; sets active_viewport, routes to the session, maps Escape/Tab.
    EditorEditDomainInput route_input(Node3DEditorViewport *p_viewport, Camera3D *p_camera, const Ref<InputEvent> &p_event);
    void route_draw(Node3DEditorViewport *p_viewport);

    // Provider/scene disappearance safety (plan §5 "exit safely if provider/view/scene disappears").
    void notify_provider_unregistered(EditorEditDomainProvider *p_provider);

    ~EditorEditDomainHost(); // exit_domain() + unmount, so no stale chrome/session pointers survive
};

#ifdef DEV_ENABLED
// DEV-only dummy domain proving the seam in the live editor (see §5). Registered/removed alongside
// the registry under DEV_ENABLED so shipped editors carry no dummy.
void register_editor_edit_domain_dev_providers();
void unregister_editor_edit_domain_dev_providers();
#endif
```

### New: `editor/gui/editor_edit_domain.cpp`

- `EditorEditDomainRegistry::singleton` definition; `create()/free()` (free unregisters remaining providers defensively, mirrors chrome/surface registries).
- `register_provider` / `unregister_provider` / `get_provider` / `find_double_click_provider` / `get_available_providers`.
- `EditorEditDomainHost::enter_domain` — resolve provider from registry; `is_available` check; `create_session`; `session->enter(context)`; `_mount_chrome_controls()`. `exit_domain` — `_unmount_chrome_controls()`; `session->exit()`; `memdelete(session)`; null the pointers. `try_activate_from_double_click` — `find_double_click_provider` then `enter_domain`.
- `route_input` — sets `context.active_viewport = p_viewport`, then:
  - `InputEventKey` ESCAPE press (non-echo): `session->handle_escape()` → if the session reports it consumed a gesture-cancel it returns CONSUMED; if it reports "no gesture" the host calls `exit_domain()` and returns CONSUMED (Escape always consumes while active — plan §8, scout "Escape multiply-owned").
  - `InputEventKey` TAB press: `session->handle_tool_toggle()` → CONSUMED if handled (host guarantees the caller `accept_event()`s; see §3).
  - Otherwise `return session->handle_input(context, p_camera, p_event)`.
- `route_draw` — `session->draw_overlay(p_viewport)`.
- `_mount_chrome_controls` — if `chrome_host` is an `EditorViewportChrome`, `build_tool_rail()`→`add_control(SLOT_CENTER_LEFT, ...)`, `build_contextual_panel()`→`add_control(SLOT_CENTER_RIGHT, ...)`; store pointers. `_unmount_chrome_controls` — `remove_control` + `queue_free` each, null them. (Guarded for null chrome so the host is headless-constructible.)
- `notify_provider_unregistered` — if `p_provider == active_provider`, `exit_domain()`.
- DEV dummy domain (see §5).

### Edited: `editor/gui/editor_viewport_chrome.h`
- `enum Slot`: insert `SLOT_CENTER_LEFT`, `SLOT_CENTER_RIGHT` **after `SLOT_BOTTOM_RIGHT`, before `SLOT_MAX`** (numeric values become 6, 7; corner slots 0–5 unchanged).

### Edited: `editor/gui/editor_viewport_chrome.cpp`
- `_layout_slots()` switch: add two cases —
  - `SLOT_CENTER_LEFT`: `position = Vector2(safe_area[SIDE_LEFT], (available_size.y - minimum_size.y) * 0.5)`
  - `SLOT_CENTER_RIGHT`: `position = Vector2(available_size.x - safe_area[SIDE_RIGHT] - minimum_size.x, (available_size.y - minimum_size.y) * 0.5)`
  - (`fit_child_in_rect` already clamps with `.max(Vector2())`, so safe-area handling matches the corner slots.)

### Edited: `editor/scene/3d/node_3d_editor_plugin.h`
- `Node3DEditorView`: add `EditorEditDomainHost *edit_domain_host = nullptr;` member; forward-declare `class EditorEditDomainHost;`; add `EditorEditDomainHost *get_edit_domain_host() const { return edit_domain_host; }`.

### Edited: `editor/scene/3d/node_3d_editor_plugin.cpp`
- `Node3DEditorView` ctor (after chrome creation `:2800`): `edit_domain_host = memnew(EditorEditDomainHost);` — build `EditorEditDomainContext{ this, document, nullptr }`; `set_context(...)`; `set_chrome_host(viewport_chrome)`. (Document may be bound later via `bind_document`; refresh the context's `document` there.)
- `Node3DEditorView` dtor (`:2803`): `if (edit_domain_host) { memdelete(edit_domain_host); edit_domain_host = nullptr; }` — the host dtor exits any active domain and unmounts chrome.
- `bind_document`: after setting `document`, update `edit_domain_host`'s context document.
- Add `Node3DEditor::is_edit_domain_active_anywhere()` (declared in the plugin header): iterate `editor_views`, return true if any `view->get_edit_domain_host()->is_active()`. This is the **global gizmo-suppression** aggregate (KEY GAP mitigation, decision-baked).
- `#include "editor/gui/editor_edit_domain.h"`.

### Edited: `editor/scene/3d/node_3d_editor_viewport.h`
- Add `bool domain_blocks_native = false;` (per-viewport, per-event suppression flag).
- Declare `bool _domain_pane_accepts_input() const;`, `void _neutralize_click_state();`.

### Edited: `editor/scene/3d/node_3d_editor_viewport.cpp`
- The arbitration hook (§3), the five suppression guards (§3), the `_draw` overlay forward, and the two helpers.

### Edited: `editor/register_editor_types.cpp`
- After `EditorViewportChromeRegistry::create()` (`:161`): `EditorEditDomainRegistry::create();` and, under `#ifdef DEV_ENABLED`, `register_editor_edit_domain_dev_providers();`.
- Symmetric teardown before `EditorViewportChromeRegistry::free()` (`:363`): `#ifdef DEV_ENABLED unregister_editor_edit_domain_dev_providers(); #endif` then `EditorEditDomainRegistry::free();`.

### New: `tests/editor/test_editor_edit_domain.cpp` (see §7).

---

## 3. The input-hook insertion and suppression wiring

### 3.1 Arbitration call — immediately after `:2816`, before `:2818`

Insert a single block between the close of the global-plugin forwarding block (`:2816`) and the `if (is_plain_context_click)` RMB block (`:2818`). This is the scout's recommended seam: after DnD/vertex-snap returns (hazard 6), after global plugins, **before** RMB menu and navigation (so PASS events reach the native context menu and orbit/pan/zoom unchanged).

```cpp
    // CSG-3B: edit-domain arbitration. Runs before native gizmo/selection handling, after global
    // plugin forwarding. PASS leaves every native path (nav, RMB menu, freelook, Space) untouched.
    domain_blocks_native = false;
    EditorEditDomainHost *domain_host = editor_view ? editor_view->get_edit_domain_host() : nullptr;
    if (domain_host && _domain_pane_accepts_input()) {
        if (domain_host->is_active()) {
            switch (domain_host->route_input(this, camera, p_event)) {
                case EditorEditDomainInput::CONSUMED: {
                    _neutralize_click_state();   // hazard 8
                    accept_event();
                    return;
                } break;
                case EditorEditDomainInput::BLOCK_NATIVE_EDIT: {
                    domain_blocks_native = true;                 // gates gizmo/transform/begin_transform below
                    after = EditorPlugin::AFTER_GUI_INPUT_CUSTOM; // reuses the existing selection gate (:3068/:3188)
                } break;
                case EditorEditDomainInput::PASS_TO_VIEWPORT:
                    break;
            }
        } else {
            // Double-click activation entry (added here per decision; no native _select_ray yet).
            Ref<InputEventMouseButton> db = p_event;
            if (db.is_valid() && db->is_pressed() && db->get_button_index() == MouseButton::LEFT && db->is_double_click()) {
                ObjectID hit = _select_ray(db->get_position()); // cheap pick for provider gating
                if (domain_host->try_activate_from_double_click(this, hit)) {
                    _neutralize_click_state();
                    accept_event();
                    return;
                }
            }
        }
    }
```

Notes:
- `_domain_pane_accepts_input()` replicates the `_surface_mouse_enter` focused-pane gate (`:2204-2209`) **and** ignores freelook-redirected events: returns `false` when a `WorkspacePane` exists and `workspace->get_focused_pane() != pane`, or when `view_3d_controller->is_freelook_enabled()` on this viewport, or when the view's `freelook_viewport` is another viewport (redirected event). This satisfies "input on non-focused panes suspended; ignore freelook-redirected events."
- `after` is the existing local `EditorPlugin::AfterGUIInput` (`:2782`). Setting it to `CUSTOM` on BLOCK reuses the native selection gates at `:3068` and `:3188` for free — no new selection branch.
- `domain_blocks_native` is recomputed every event (reset at the top of the hook). A domain that wants edits suppressed across a press→release must return `BLOCK_NATIVE_EDIT` (or `CONSUMED`) on **every** edit-class event while active; the dummy domain and the contract require this.

### 3.2 Suppression guards (exact sites)

Two suppression scopes, per the decision:

**A. Per-viewport, this event (`domain_blocks_native`)** — gates the *transform manipulator* and native selection/transform on the active editing viewport:
- Transform-gizmo select `:3013-3016` → `if (transform_gizmo_visible && !domain_blocks_native && _transform_gizmo_select(_edit.mouse_pos)) {`
- `begin_transform` at `:3106` and `:3155` → wrap each `if (mode != TRANSFORM_NONE && !domain_blocks_native)`.
- Transform-gizmo hover `:3314` → add `&& !domain_blocks_native` to the condition guarding `_transform_gizmo_select(_edit.mouse_pos, true)`.
- Object selection: already gated by `after == CUSTOM` (set in the hook) at `:3068` / `:3188` — no code change needed there.

**B. Global while any domain active (`spatial_editor->is_edit_domain_active_anywhere()`)** — gates *selected-object node-attached gizmos*, which render in the shared `World3D` and are pickable from every pane (KEY GAP). MVP = suppress globally:
- Gizmo handle pick `:2983` → `if (can_select_gizmos && !spatial_editor->is_edit_domain_active_anywhere()) {`
- Subgizmo pick `:3018/3019` → same guard on the `if (can_select_gizmos ...)`.
- Selected-object gizmo hover `:3280` → add `&& !spatial_editor->is_edit_domain_active_anywhere()` to the `if (!freelook && get_single_selected_node())` condition.

Rationale for the split: the transform manipulator is per-viewport RIDs, so per-viewport `domain_blocks_native` is exact for it; the node-attached gizmos are global, so a global aggregate is required to stop a *second* pane from grabbing a handle while a domain is active elsewhere. **Future per-view path** (not built): hide/suppress node-attached gizmo *rendering* per-view via per-gizmo visibility in the shared scenario — noted only.

**Never suppressed** (verify untouched): navigation `:2833-2838`, plain-RMB context menu `:2818-2831`, freelook enable `:2886`, Space maximize (`:3632` region, keyboard), region-select drag `:3327-3337`, wheel. The hook returns PASS for these because the session returns `PASS_TO_VIEWPORT` for MMB/wheel/nav-modified/plain-RMB (contract; enforced by the dummy domain and asserted in tests).

### 3.3 Hazard-8 neutralization

`_neutralize_click_state()` (new helper) clears the cross-event press/motion/release state so a later native release (if it ever passes) cannot act on a press the domain consumed:
```cpp
void Node3DEditorViewport::_neutralize_click_state() {
    clicked = ObjectID();
    selection_in_progress = false;
    view_3d_controller->cursor.region_select = false;
    movement_threshold_passed = false;
}
```
Called whenever the hook returns after `CONSUMED` or a successful double-click activation.

### 3.4 Overlay forward

In `_draw()` (`:4654`), after the existing force-draw/`forward_3d_draw_over_viewport` forwarding, add:
```cpp
    if (editor_view && editor_view->get_edit_domain_host() && editor_view->get_edit_domain_host()->is_active()) {
        editor_view->get_edit_domain_host()->route_draw(this);
    }
```
This is naturally view-local (per-viewport `surface` draw signal) — no material mutation, satisfying plan §9. The dummy domain draws a marker rect to prove it.

### 3.5 Escape / Tab ordering

The hook at `:2817` runs before the keyboard section (`:3360`) and before the DEFAULT Escape `_clear_selected()` (`:3431`). So a routed Escape is consumed by the domain *before* every native Escape owner in the button/keyboard switch. Vertex-snap (`:2658/:2683`) still precedes the hook — acceptable, the two fork modes are mutually exclusive; documented. Tab has **no** existing viewport binding; the host consuming Tab + the hook's `accept_event()` blocks Control focus traversal (hazard 5).

---

## 4. Chrome slot additions — existing 6-slot users unaffected

- The two new enum values are appended before `SLOT_MAX`, so `SLOT_TOP_LEFT..SLOT_BOTTOM_RIGHT` keep values 0–5. `slots[SLOT_MAX]` / `control_orders[SLOT_MAX]` grow from 6 to 8 automatically; the ctor loop `for (i < SLOT_MAX)` builds the two extra `VBoxContainer`s with the same `MOUSE_FILTER_IGNORE` + `ViewportChromeSlot` variation.
- `_layout_slots()` iterates `i < SLOT_MAX` and now has cases for the two center slots; the existing six cases are byte-identical. No existing registration passes `SLOT_CENTER_*`, so no existing control changes position.
- `register_control_factory`'s `ERR_FAIL_INDEX(p_slot, SLOT_MAX)` (`:78`) and `add_control`'s `ERR_FAIL_INDEX(p_slot, SLOT_MAX)` (`:225`) already accept the new indices.
- The current single registered chrome factory (editor_id `"3d"`) and any other corner-slot users are untouched — they reference slots by name, never by numeric value, and never enumerate `SLOT_CENTER_*`.
- 3B mounts center-slot controls via the **host-direct** `add_control`/`remove_control` path (dynamic per-session), not via registered factories. The registered-factory reflection pattern (factory receives the host, renders the active session's rail/panel) is the Phase 4+ production path — noted, not built.

---

## 5. The dummy domain (DEV_ENABLED only)

Purpose: prove each contract point in the live editor without CSG logic. Compiled only under `DEV_ENABLED`; registered by `register_editor_edit_domain_dev_providers()` and removed by its unregister twin, so shipped editors carry nothing.

`DummyEditDomainProvider` (`domain_id = "dev_dummy"`):
- `is_available` → true when the context has a view (always, in DEV) — lets the toggle/double-click enter it.
- `can_activate_from_double_click` → returns true (proves double-click entry through the hook).
- `create_session` → `memnew(DummyEditDomainSession)`.

`DummyEditDomainSession` proves:
- **Enter/exit** — `enter` sets a flag + prints a DEV marker; `exit` clears it; host mounts/unmounts the two chrome controls. Verifiable: entering shows a center-left rail label and center-right panel; exiting removes both (proves `SLOT_CENTER_LEFT`/`SLOT_CENTER_RIGHT` + host mount/unmount + no stale pointers).
- **Input routing** — `handle_input`: returns `CONSUMED` for LMB press/release (claims the click, proving selection/gizmo suppression + `_neutralize_click_state`); returns `BLOCK_NATIVE_EDIT` while LMB is held with movement (proves the gizmo/transform guards without the domain performing an action); returns `PASS_TO_VIEWPORT` for MMB, wheel, nav-modified LMB, and plain-RMB (proves navigation + context menu stay native).
- **Escape** — `handle_escape` returns true (cancel) once if a fake "gesture" flag is set, else false → host exits (proves Escape consumes before `_clear_selected`, cancel-then-exit ordering).
- **Tab** — `handle_tool_toggle` flips a dummy "tool" and returns true (proves Tab consumed + focus traversal blocked).
- **Overlay** — `draw_overlay` draws a labeled rect via the viewport surface `draw_*` (proves view-local overlay; only in the pane that entered).
- **Chrome** — `build_tool_rail()` returns a `VBoxContainer` with two `Button`s (fake Surface/Operand); `build_contextual_panel()` returns a panel Control (proves session-supplied contextual Control mounting).

Entry for manual testing: a DEV-only viewport context-menu entry or a temporary toggle that calls `editor_view->get_edit_domain_host()->enter_domain("dev_dummy", this)`; plus the double-click path already wired in the hook. (Keep the entry DEV-gated; it is scaffolding, not a shipped feature.)

Acceptance (plan §28 3B) proven by the dummy: claims LMB, draws an overlay, supplies tools, mounts a panel — all without viewport-core changes beyond the single seam; navigation remains native; two views hold independent sessions (each `Node3DEditorView` has its own host); provider removal cannot leave stale pointers (`notify_provider_unregistered` → `exit_domain`).

---

## 6. Migration / implementation order (each step compiles)

Serialize dev builds — before every build run `Get-Process python` and confirm no SCons is active (shared tree). Build command: `scons platform=windows target=editor dev_build=yes tests=yes d3d12=yes winrt=no -j24`. Test after the relevant steps: `bin/godot.windows.editor.dev.x86_64.exe --test --test-case="*EditorEditDomain*"` plus regression `*EditorDocumentSurface*` and `*ResponsiveLayout*`.

**Step 0 — Baseline.** Build dev+tests; confirm `*EditorDocumentSurface*` (2/2), `*ResponsiveLayout*` (2/2) green. Record.

**Step 1 — Chrome slots (isolated).** Add `SLOT_CENTER_LEFT/RIGHT` to the enum + two `_layout_slots()` cases. Pure additive; existing chrome untouched. Build. (No test change; existing chrome still lays out identically.)

**Step 2 — `editor_edit_domain.{h,cpp}` (generic layer, unwired).** Registry + provider + session + host + `EditorEditDomainInput`. Wire `EditorEditDomainRegistry::create()/free()` into `register_editor_types.cpp` after the chrome registry. Add `tests/editor/test_editor_edit_domain.cpp` (registry + tri-state + lifecycle). Build + run `*EditorEditDomain*` — this is the headless-verifiable milestone.

**Step 3 — Host ownership on `Node3DEditorView`.** Member + getter + ctor create / dtor free + context build + `bind_document` refresh + `Node3DEditor::is_edit_domain_active_anywhere()`. No viewport input wiring yet. Build (editor links; hosts exist but are never entered).

**Step 4 — Viewport arbitration + suppression.** Add `domain_blocks_native`, `_domain_pane_accepts_input()`, `_neutralize_click_state()`; insert the hook after `:2816`; add the five suppression guards (§3.2) and the `_draw` forward. Build. With no provider entered, every guard's extra condition is `false`/inactive → native behavior byte-identical (regression-verify manually: selection, gizmos, nav, RMB menu, Escape).

**Step 5 — DEV dummy domain.** Add the DEV-gated provider/session + `register/unregister_editor_edit_domain_dev_providers()` under `DEV_ENABLED` in `register_editor_types.cpp`. Manual editor-runtime verification of all §5 points in a live pane; verify a second pane holds an independent session and native gizmos are suppressed in both while one is active. Build.

**Step 6 — `/simplify` pass + commit.** Re-verify all three filters; one commit (Co-Authored-By trailer per progress-log convention). Stage files explicitly — never `git add -A`. `workspace-editor-planning/` is read-only.

Each step is independently revertable; Steps 1–3 are additive/unwired (lowest risk), Step 4 is the only behavior-sensitive edit to viewport core, Step 5 is DEV-only scaffolding.

---

## 7. Test plan

**Headless-testable (new `tests/editor/test_editor_edit_domain.cpp`, `TEST_FORCE_LINK(test_editor_edit_domain)`, `#ifdef TOOLS_ENABLED`):** follows the 3A test exactly — constructs a **local** `EditorEditDomainRegistry` (not the singleton) so no `EditorNode` is needed.

- **Registry** — register a `TestDomainProvider`; `get_provider` resolves it; `is_available` honored; `register_provider` rejects duplicate id; `unregister_provider` succeeds and `get_provider` returns null after. Register two providers, `get_available_providers` returns both; `find_double_click_provider` returns the willing one.
- **Independent sessions per view** — a host built with view-A context and a host with view-B context each `enter_domain` → distinct `EditorEditDomainSession*`; entering/exiting one does not touch the other (proves per-view session ownership).
- **Provider-removal safety** — host enters a domain; `notify_provider_unregistered(provider)` → host exits, `is_active()` false, session freed, no dangling pointer (mirrors 3A's "unregister before destroy" assertion).
- **Tri-state mapping** — a `TestSession` programmed to return each `EditorEditDomainInput`; `host.route_input(nullptr, nullptr, event)` returns the programmed value for a generic event (host/session must not deref the null viewport/camera on this path). Verify MMB/wheel/plain-RMB events map to `PASS_TO_VIEWPORT` through a session that implements the contract.
- **Escape/Tab lifecycle** — Escape press with a "gesture active" session flag → `route_input` returns `CONSUMED` and session stays active (cancel); Escape press with no gesture → `CONSUMED` and host exits (`is_active()` false). Tab press → `CONSUMED` and `handle_tool_toggle` observed.
- **Contextual-control lifecycle** — a `TestSession` returning non-null `build_tool_rail`/`build_contextual_panel`; construct the host with a real `EditorViewportChrome`; enter → both controls parented into the center slots; exit → both removed and freed; no leak. (If chrome instantiation proves theme-dependent in the harness, fall back to asserting the host calls the builders and stores/clears the pointers with a null chrome — the mount call is guarded.)

**Editor-runtime only (not headless; manual via the DEV dummy in Step 5):** the five suppression guards, `after==CUSTOM` selection gate, `_neutralize_click_state` across a real press/release, double-click entry through `_sinput`, overlay draw, focused-pane suspension, freelook-redirect ignore, and global gizmo suppression across two panes.

Filters to run: `*EditorEditDomain*` (new), `*EditorDocumentSurface*`, `*ResponsiveLayout*` (regression).

---

## 8. Risks and mitigations

| Risk | Mitigation |
|------|-----------|
| **Stale press/selection state** (`clicked`, `selection_in_progress`, `region_select`) after a domain consumes a press → native release handler (`:3188-3196`) acts on it (hazard 8) | `_neutralize_click_state()` on every `CONSUMED`/double-click return, clearing all four flags. Domain must return `CONSUMED`/`BLOCK` on the matching release too (contract + dummy). |
| **Gizmo suppression scope** — node-attached gizmos are global (KEY GAP); per-viewport bool alone lets a second pane grab a handle | Global aggregate `Node3DEditor::is_edit_domain_active_anywhere()` gates the three node-attached gizmo sites (`:2983/:3018/:3280`); per-viewport `domain_blocks_native` gates only the per-view transform manipulator. Future per-view render-hide noted, not built. |
| **`domain_blocks_native` leaks** and suppresses native editing when no domain is active | Reset to `false` at the top of the hook every event; only set `true` on a `BLOCK` from an active, focused-pane domain. Step 4 regression pass with no provider entered confirms identical native behavior. |
| **Navigation / RMB menu / freelook / Space accidentally suppressed** | Hook placed after global plugins, before RMB/nav; session returns `PASS` for MMB/wheel/nav-modified/plain-RMB; BLOCK sets only edit-site flags, never gates `:2833`/`:2818`/`:2886`/region-drag. Asserted by the PASS-mapping test + manual Step 5. |
| **Escape double-owned** — `_clear_selected` (`:3431`) or gizmo-restore fires instead of the domain | Hook precedes the keyboard section; routed Escape `accept_event()+return` before all native owners. Vertex-snap precedence documented (mutually exclusive modes). |
| **Tab steals Control focus** | Host consumes Tab; hook `accept_event()`s → no focus traversal (hazard 5). |
| **Input on non-focused pane / freelook-redirected events** processed | `_domain_pane_accepts_input()` reuses the `_surface_mouse_enter` focused-pane check and rejects freelook/redirected events before routing. |
| **Provider destroyed while a session is live** → dangling `active_provider`/session/chrome | `notify_provider_unregistered` → `exit_domain`; host dtor exits+unmounts; registry `free()` unregisters remaining providers defensively; provider is only ever borrowed (3A discipline). Covered by the removal-safety test. |
| **Chrome enum insertion breaks existing corner users** | New values appended before `SLOT_MAX`; corner slots keep 0–5; existing users reference by name; `_layout_slots` default no longer reached for center slots. |
| **Double-click entry couples to picking internals prematurely** | 3B passes a cheap `_select_ray` `ObjectID` (or `ObjectID()`); provider gating is a bool; real `CSGSurfaceHit` resolution deferred to Phase 4. |
| **New generic API grows speculatively** | Provider/session interface carries only proven 3B needs (plan §5 "add capabilities only when two domains need them"); `retarget` is a stub; no capability dictionary. |
| **Two panes contaminate each other** | Host is per-`Node3DEditorView`; session state is pane-local; overlays draw via each viewport's own `surface`. Independent-session test + manual two-pane check. |

---

## 9. Explicitly out of scope (Phase 4+)

- **No CSG tool logic:** no Surface/Draw/Paint/Operand tools, no push/pull, no extrusion, no face selection, no `CSGSurfaceHit`/`CSGSurfaceKey` wiring into the session.
- **No evaluation scheduler**, no interactive/final split, no worker threads, no ghosts (Phase 4 + the pre-step already in flight in `modules/csg`).
- **No real CSG domain provider** — only the DEV dummy. The shipped CSG `EditorEditDomainProvider`, its tool rail, and its registered center-slot chrome factories are Phase 4+.
- **No registered-factory chrome reflection** for center slots in 3B (host-direct mount only); the factory-receives-host production pattern is noted for Phase 4+.
- **No per-view node-attached gizmo render suppression** — MVP suppresses globally while any domain is active; per-view render-hide is future.
- **No `modules/**` edits**, no `SCsub` edits (globbing covers the new files), no `doc_classes` (no newly exposed script classes; the session is `Object`-derived but unbound for 3B).
- **No context-menu CSG entries, no toolbar toggle, no scene-tree lifecycle rules** (double-click entry is stubbed via the dummy; the full activation matrix of plan §7 is Phase 4+).

---

### Critical files
- `editor/gui/editor_edit_domain.h` / `editor_edit_domain.cpp` (new) — registry/provider/session/host + DEV dummy.
- `editor/scene/3d/node_3d_editor_viewport.{h,cpp}` — arbitration hook (after `:2816`), five suppression guards, `_draw` forward, two helpers.
- `editor/scene/3d/node_3d_editor_plugin.{h,cpp}` — `Node3DEditorView` host ownership + `Node3DEditor::is_edit_domain_active_anywhere()`.
- `editor/gui/editor_viewport_chrome.{h,cpp}` — `SLOT_CENTER_LEFT/RIGHT` enum + layout.
- `editor/register_editor_types.cpp` — registry create/free + DEV provider registration.

New test: `tests/editor/test_editor_edit_domain.cpp` (auto-discovered; filter `*EditorEditDomain*`).
