/**************************************************************************/
/*  editor_workspace.h                                                    */
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

#include "core/input/input_event.h"
#include "core/object/object_id.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"
#include "scene/gui/box_container.h"

class EditorDocument;
class EditorWorkspace;
class SplitContainer;
class TabbedDocumentHost;

// Workspace layer for the dividable multi-document editor (G2).
//
// EditorWorkspace replaces the single fixed main-screen area with a tree of
// WorkspacePanes. A WorkspacePane is one region of the workspace and is either:
//   - a LEAF, hosting a single content Control (in v1: the shared main-screen
//     editor stack; later: per-pane tabs of DocumentViews), or
//   - a SPLIT, hosting a SplitContainer that divides the region between two
//     child WorkspacePanes (recursively).
// This increment adds the split mechanic (the tree can now divide) on top of the
// earlier shell; content hosting is still the shared main-screen stack, so real
// per-pane editors arrive later once the 3D/2D editors are instanceable.

class WorkspacePane : public VBoxContainer {
	GDCLASS(WorkspacePane, VBoxContainer);

	// The owning workspace, so a pane can report focus and mint child panes that
	// are also wired back to it. Set by EditorWorkspace when the pane is created.
	EditorWorkspace *workspace = nullptr;

	// G2 M6.1: stable per-pane id, unique within a saved layout, so the session store can map
	// documents/tabs back onto the right leaf across restart. Assigned by EditorWorkspace::make_pane
	// from a monotonic counter; 0 means "not yet assigned" (a bare memnew(WorkspacePane) outside the
	// workspace). Never reused within one layout file.
	uint32_t pane_id = 0;

	// LEAF state: the content this pane displays (fills the pane), reparented in
	// via set_content(). Null while this pane is a SPLIT.
	Control *content = nullptr;

	// SPLIT state: the divider and the two child panes. All null while this pane
	// is a LEAF. `first` is the left/top child, `second` the right/bottom child.
	SplitContainer *split_container = nullptr;
	WorkspacePane *first = nullptr;
	WorkspacePane *second = nullptr;

protected:
	static void _bind_methods() {}
	virtual void input(const Ref<InputEvent> &p_event) override;

public:
	void set_workspace(EditorWorkspace *p_workspace) { workspace = p_workspace; }
	EditorWorkspace *get_workspace() const { return workspace; }

	uint32_t get_pane_id() const { return pane_id; } // G2 M6.1
	void set_pane_id(uint32_t p_id) { pane_id = p_id; } // G2 M6.1

	bool is_leaf() const { return split_container == nullptr; }

	void set_content(Control *p_content);
	Control *get_content() const { return content; }

	// Divide this LEAF pane in two. The current content stays on one side and
	// p_new_content is placed on the other (on the second/bottom-right side when
	// p_new_on_second is true). p_vertical stacks the children top/bottom; false
	// places them side by side. Returns the child pane that holds p_new_content,
	// or nullptr if this pane is not a leaf.
	WorkspacePane *split(bool p_vertical, Control *p_new_content, bool p_new_on_second = true);

	WorkspacePane *get_first() const { return first; }
	WorkspacePane *get_second() const { return second; }
	int get_leaf_count() const;

	// G6: nearest WorkspacePane at or above p_node (null if none). Shared by the workspace and the
	// tabbed hosts so the parent-chain walk lives in one place.
	static WorkspacePane *of(Node *p_node);

	// G2 M6.1: recursive geometry serialization. to_dict() emits this subtree as a schema-v1 node —
	// a leaf { "t":"leaf", "id":int } or a split { "t":"split", "vert":bool, "off":int, "a":<node>,
	// "b":<node> }. from_dict() rebuilds such a subtree (leaves get an empty TabbedDocumentHost;
	// content is assigned later by the session store, M6.3); on any structural anomaly it flips
	// r_ok false and the caller frees the partial tree. Members of WorkspacePane so they reach the
	// private split state directly.
	Dictionary to_dict() const;
	static WorkspacePane *from_dict(const Dictionary &p_dict, EditorWorkspace *p_workspace, bool &r_ok);

	// G2 S8: this SPLIT pane becomes its surviving child. p_removed's whole subtree is
	// freed (DocumentViews run their PREDELETE detach work); the survivor's payload —
	// leaf content or inner split — is re-homed into this pane.
	void collapse_split(WorkspacePane *p_removed);

	WorkspacePane();
};

class EditorWorkspace : public VBoxContainer {
	GDCLASS(EditorWorkspace, VBoxContainer);

	WorkspacePane *root_pane = nullptr;
	WorkspacePane *focused_pane = nullptr;
	WorkspacePane *last_tabbed_pane = nullptr;

	// G2 M6.1: monotonic source of stable pane ids (see WorkspacePane::pane_id). Never reset within a
	// session; load_geometry advances it past every restored id so a post-restore split can't collide.
	uint32_t next_pane_id = 1;

protected:
	static void _bind_methods();
	void _notification(int p_what);
	void _on_gui_focus_changed(Control *p_control);

public:
	WorkspacePane *make_pane();

	WorkspacePane *get_root_pane() const { return root_pane; }
	WorkspacePane *get_focused_pane() const { return focused_pane ? focused_pane : root_pane; }
	WorkspacePane *get_last_tabbed_pane() const { return last_tabbed_pane; }
	void set_focused_pane(WorkspacePane *p_pane);

	// G2 S5: locate the leaf pane whose TabbedDocumentHost already shows p_document (no-duplicate-tab
	// rule), or null if none. Walks the whole pane tree.
	WorkspacePane *find_pane_showing(EditorDocument *p_document) const;

	// G2 M6.2: locate the pane with the given stable id (see WorkspacePane::pane_id), or null. Used by
	// session restore to re-home a document into the exact leaf it was saved in.
	WorkspacePane *find_pane_by_id(uint32_t p_id) const;

	// G2 M6.2: every leaf pane whose content is a TabbedDocumentHost, in tree order. Lets the session
	// store iterate panes without re-implementing the split/leaf recursion outside the workspace.
	Vector<WorkspacePane *> get_tabbed_leaves() const;

	// G2 S5: resolve where a revealed document should open (seam #3): (a) the focused pane if it hosts
	// tabs; else (b) the most-recently-focused tabbed leaf; else (c) split the focused leaf and mint a
	// fresh TabbedDocumentHost on the new side. The returned pane's content is a TabbedDocumentHost
	// (or null if it could not be resolved). This is what makes "open a script from a script" land as
	// a new tab in the SAME pane.
	WorkspacePane *resolve_target_pane_for_documents();

	// G2 S8: deliberate pane close (tab-bar context menu / last tab closed). Drains the
	// pane's remaining tabs through the host close pipeline, then collapses the parent
	// split onto the sibling. The root pane never closes.
	void close_pane(WorkspacePane *p_pane);

	// G2 S8: close_pane on the next idle frame, ObjectID-guarded — safe to call from
	// inside the pane's own signal emissions (tab close, context menu).
	void queue_close_pane(WorkspacePane *p_pane);

	// G2 S8: split p_pane and MOVE its p_tab into a fresh TabbedDocumentHost on the new side. The
	// tab's DocumentView is re-homed, not closed and reopened, so no close side effects fire. The new
	// pane lands on the second (right/bottom) side when p_new_on_second, else first (left/top) -- G6's
	// drag-to-split compass uses this to reach all four directions. Returns the new pane (null if refused).
	WorkspacePane *split_pane_with_tab(WorkspacePane *p_pane, int p_tab, bool p_vertical, bool p_new_on_second = true);

	// G6: the drag-to-split primitive. Move p_source's tab p_tab onto p_target: p_center adopts it as a
	// tab of p_target's own host; otherwise p_target is split (p_vertical / p_new_on_second choose the
	// side) and the tab lands in a fresh host there. A cross-pane move that empties the source pane
	// closes it. Returns the pane the tab ended up in (null if refused). split_pane_with_tab delegates here.
	WorkspacePane *move_tab_into_pane(TabbedDocumentHost *p_source, int p_tab, WorkspacePane *p_target, bool p_center, bool p_vertical, bool p_new_on_second);

	// G2 M6.1: round-trip the split-tree geometry. save_geometry() returns a schema-v1 dictionary
	// { "v":1, "next":int, "root":<node> } (see WorkspacePane::to_dict). load_geometry() rebuilds the
	// whole pane tree from such a dictionary — leaves are empty tab hosts (the session store fills them,
	// M6.3) — and returns false, leaving the current tree untouched, on any version/structural anomaly.
	Dictionary save_geometry() const;
	bool load_geometry(const Dictionary &p_geometry);

	EditorWorkspace();

private:
	WorkspacePane *_find_pane_showing(WorkspacePane *p_pane, EditorDocument *p_document) const;
	WorkspacePane *_find_pane_by_id(WorkspacePane *p_pane, uint32_t p_id) const; // G2 M6.2
	void _gather_tabbed_leaves(WorkspacePane *p_pane, Vector<WorkspacePane *> &r_leaves) const; // G2 M6.2
	WorkspacePane *_find_tabbed_leaf(WorkspacePane *p_pane) const;
	void _close_pane_by_id(ObjectID p_pane_id);
};
