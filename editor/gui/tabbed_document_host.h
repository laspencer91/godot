/**************************************************************************/
/*  tabbed_document_host.h                                                */
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

#include "core/templates/vector.h"
#include "scene/gui/box_container.h"

class Control;
class DocumentView;
class EditorDocument;
class PaneDropOverlay;
class PanelContainer;
class PopupMenu;
class TabBar;
class WorkspacePane;

// TabbedDocumentHost is the tabbed content of one workspace pane (G2): a TabBar
// of open documents over a content area that shows the active document's
// DocumentView. Selecting a tab swaps which document the pane renders; this is
// the first non-debug piece of the multi-document workspace UX.
//
// Ownership/lifecycle: every DocumentView is a child of the content host (so the
// scene tree frees them all), created lazily on first selection. Only the active
// tab's view is visible; hidden views' SubViewports stop rendering on their own
// (SubViewport UPDATE_WHEN_VISIBLE), which is the cheap half of the inactive-doc
// cost policy (ARCHITECTURE.md open decision #1 covers pausing physics too).
class TabbedDocumentHost : public VBoxContainer {
	GDCLASS(TabbedDocumentHost, VBoxContainer);

	// G2 styling: the tab bar sits in a panel styled with the editor's "tabbar_background" stylebox
	// (TabContainer theme), matching the native scene-tab strip instead of a bare TabBar.
	PanelContainer *tabbar_panel = nullptr;
	TabBar *tab_bar = nullptr;
	Control *content_host = nullptr; // Fills the pane; parent of every DocumentView.
	PaneDropOverlay *drop_overlay = nullptr; // G6: drag-to-split compass over the content area (kept topmost).

	Vector<EditorDocument *> documents; // One per tab, index-aligned with the TabBar. Not owned.
	Vector<DocumentView *> views; // Parallel to documents; lazily created, owned via content_host.
	int current = -1;

	// G2 S8: tab-bar context menu (Split Right/Down, Close Tab, Close Pane).
	enum {
		MENU_SPLIT_RIGHT,
		MENU_SPLIT_DOWN,
		MENU_CLOSE_TAB,
		MENU_CLOSE_PANE,
	};
	PopupMenu *tab_menu = nullptr;
	int menu_tab = -1; // The right-clicked tab the open menu acts on.

	DocumentView *_ensure_view(int p_idx); // Create (hidden) if absent; returns the view.
	void _show(int p_idx); // Reveal tab p_idx's view, hide the rest.
	void _activate_document(int p_idx); // Make tab p_idx's document the editor's active edited scene.
	void _sync_current_script_view(int p_idx); // G2 S6a: tell ScriptEditor which script view (if any) is current.
	void _remove_tab_entry(int p_idx); // Drop the tab row, keeping the current selection when it survives.
	void _drop_tab_at(int p_idx); // Mechanical tab removal + close side effects (no scene-close routing).
	bool suppress_activation = false; // Guards the reselect inside _drop_tab_at from re-entering the global scene switch.
	void _on_tab_selected(int p_idx);
	void _on_tab_close(int p_idx);
	void _on_tab_rmb(int p_idx); // G2 S8
	void _on_menu_pressed(int p_id); // G2 S8
	WorkspacePane *_owning_pane() const; // G2 S8: nearest WorkspacePane ancestor (null outside a workspace).
	void _raise_overlay(); // G6: keep drop_overlay the topmost child of content_host (drawn over views).
	static TabbedDocumentHost *_host_from_drag_data(const Variant &p_data, int &r_tab, const Node *p_ref); // G6

protected:
	static void _bind_methods() {}
	void _notification(int p_what);

public:
	// Append a tab for p_document titled p_title; returns its index. The first
	// document added becomes the active tab.
	int add_document(EditorDocument *p_document, const String &p_title);

	void set_current(int p_idx);
	int get_current() const { return current; }
	int get_document_count() const { return documents.size(); }

	// G2 S5: reveal a document in this pane — focus its existing tab, or append one titled from the
	// document (path filename, else a generic fallback) and select it. has_document backs the
	// no-duplicate-tab rule (a document lives in at most one pane in v1).
	bool has_document(EditorDocument *p_document) const;
	void focus_document(EditorDocument *p_document);

	// G2 S6a: make sure p_document has a tab (title derived like focus_document) WITHOUT changing
	// the selection — the background-open path (open dominant script while p_grab_focus is false).
	// The view is minted eagerly (hidden) so the document's editor surface exists either way.
	// Returns the tab index.
	int ensure_document(EditorDocument *p_document);

	// G2 S6a: the current tab's DocumentView (null when no tabs / not yet minted).
	DocumentView *get_current_view() const;
	DocumentView *get_document_view(EditorDocument *p_document) const;
	void set_context_active(bool p_active);

	// G4: make this host's current tab the editor's active edited scene (no-op for a non-scene current
	// tab, since only scene documents have an edited-scene index). Used by pane focus so the global
	// bottom docks (Animation, Terrain) track whichever scene pane you click into, not just the pane
	// whose tab/viewport you last clicked.
	void activate_current_document() { _activate_document(current); }

	// G2 S7: close p_document's tab (same pipeline as the tab X, side effects included).
	// False if the document has no tab here.
	bool close_document(EditorDocument *p_document);

	// G2 S8: close tab p_idx (same pipeline as the tab X — a scene tab routes through
	// EditorNode's close-scene flow, prompt included).
	void close_tab(int p_idx) { _on_tab_close(p_idx); }

	// Mechanical removal, NO scene-close routing: the tab/view go away, side effects
	// (script state cache, chrome parking) still run. Used by the pane-close drain (a
	// scene tab must not pop an async prompt mid-drain; its scene stays open) and by
	// drop_document_tab when a scene document is being destroyed.
	void drop_tab(int p_idx) { _drop_tab_at(p_idx); }
	bool drop_document_tab(EditorDocument *p_document);

	EditorDocument *get_document(int p_idx) const; // G2 S8 (null on bad index).

	// G2 S8: tab move between hosts. detach_tab removes the tab and hands back its live
	// DocumentView WITHOUT close side effects; adopt_tab re-homes such a view here and
	// selects it. Used by EditorWorkspace::split_pane_with_tab.
	DocumentView *detach_tab(int p_idx);
	int adopt_tab(EditorDocument *p_document, DocumentView *p_view);

	// G6: drag-to-split drop handling, driven by this pane's PaneDropOverlay. can_accept_tab_drop
	// validates that a native TabBar drag payload resolves to a live source host + in-range tab;
	// accept_tab_drop is a pure pass-through of the overlay's resolved split intent to
	// EditorWorkspace::move_tab_into_pane (whose signature this triple matches).
	bool can_accept_tab_drop(const Variant &p_data) const;
	void accept_tab_drop(const Variant &p_data, bool p_center, bool p_vertical, bool p_new_on_second);

	TabbedDocumentHost();
};
