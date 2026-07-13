/**************************************************************************/
/*  workspace_file_drawer.h                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "scene/gui/panel_container.h"

class Button;
class ConfigFile;
class HSplitContainer;
class TabContainer;
class Tween;

// WorkspaceFileDrawer is the slide-up file-exploration overlay (G4). It is an
// Unreal-Content-Drawer-style panel that floats OVER the lower part of the
// central editor column rather than reflowing it: the pane layout underneath
// keeps its size while the drawer animates up from the bottom edge. In v1 it
// hosts the project FileSystemDock; the tab host makes adding Import/etc. later
// additive.
//
// Positioning: the drawer is a top-level child of the central column
// (center_vb), so its owning Container never lays it out; instead it tracks the
// host's global rect and paints itself as a centered, max-width band anchored to
// the host's bottom edge. A single 0..1 "shown" amount, animated by a Tween,
// lerps it between fully-below-the-host (closed) and its open position.
class WorkspaceFileDrawer : public PanelContainer {
	GDCLASS(WorkspaceFileDrawer, PanelContainer);

	// The central column the drawer overlays (center_vb). Not owned; we track its
	// global rect to size/position ourselves over it.
	Control *host = nullptr;

	Button *close_button = nullptr;
	HSplitContainer *split = nullptr; // Left: FileSystem tab host. Right: collapsible file details.
	TabContainer *tab_host = nullptr; // Hosts the drawer's panels (FileSystem in v1).
	TabContainer *detail_tabs = nullptr; // Import settings and file-backed Resource inspector.

	// G4: file details live in a collapsible right side of the drawer. These panels are project-file
	// context, not scene documents, and are therefore intentionally outside EditorDockManager.
	Control *import_panel = nullptr;
	Control *resource_inspector_panel = nullptr;
	Button *details_toggle = nullptr;
	bool details_open = false;
	bool import_enabled = true; // Feature-profile gate for the Import panel specifically.

	void _update_details_visibility();
	void _on_details_toggled(bool p_pressed);
	void _show_detail_panel(Control *p_panel);

	bool drawer_open = false;
	bool enabled = true; // Feature-profile gate; a disabled drawer force-closes and refuses to open.
	// Re-entrancy guard: add_theme_style_override() re-fires NOTIFICATION_THEME_CHANGED synchronously,
	// which would recurse back into _update_theme() (stack overflow). Set while applying overrides.
	bool applying_theme = false;
	float shown_amount = 0.0f; // 0 = fully closed (off-screen below host), 1 = fully open.
	float height_fraction = 0.4f; // Fraction of host height the open drawer occupies.
	Ref<Tween> tween;

	void _relayout(); // Recompute our global rect from host rect + shown_amount.
	void _set_shown_amount(float p_amount); // Tween target setter -> store + relayout.
	void _on_close_pressed();
	void _update_theme();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// The overlay region (the central editor column). Connects rect tracking so
	// the drawer follows the column as side docks / the window resize.
	void set_host(Control *p_host);

	// Add p_panel as a titled tab in the drawer. Reparents p_panel in.
	void add_panel(Control *p_panel, const String &p_title);

	// G4: install the Import panel into the drawer's collapsible right side (reparents it in).
	void set_import_panel(Control *p_panel);
	// Install the file-backed .tres/.res inspector alongside Import.
	void set_resource_inspector_panel(Control *p_panel);
	// Open/close the shared detail side. Also driven by the selected file type.
	void set_details_open(bool p_open);
	// Feature-profile gate for Import specifically; Inspector remains available when Import is hidden.
	void set_import_enabled(bool p_enabled);
	// Slot for ImportDock::edit_target_changed -- auto-reveals the panel when a reimportable file is picked.
	void on_import_target_changed(bool p_has_content);
	// Slot for ResourceInspectorDock::edit_target_changed -- selects Inspector for a .tres/.res file.
	void on_resource_inspector_target_changed(bool p_has_content);

	void set_open(bool p_open, bool p_animate = true);
	bool is_open() const { return drawer_open; }

	// Feature-profile gate. A disabled drawer force-closes and refuses to reopen, so no shortcut or
	// button path can surface a profile-hidden FileSystem dock. Owned here as the single source of truth.
	void set_enabled(bool p_enabled);

	void save_state_to_config(Ref<ConfigFile> p_config, const String &p_section) const;
	void load_state_from_config(const Ref<ConfigFile> &p_config, const String &p_section);

	WorkspaceFileDrawer();
};
