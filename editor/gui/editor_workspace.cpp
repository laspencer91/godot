/**************************************************************************/
/*  editor_workspace.cpp                                                  */
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

#include "editor_workspace.h"

#include "core/input/input_event.h"
#include "core/object/callable_mp.h"
#include "editor/editor_document.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/gui/document_view.h" // G2 S6a: current-script-view sync on pane focus.
#include "editor/gui/tabbed_document_host.h"
#include "editor/script/script_editor_plugin.h"
#include "editor/shader/shader_editor_plugin.h" // G-Shader: current-shader-view sync on pane focus.
#include "editor/themes/editor_scale.h"
#include "scene/gui/split_container.h"
#include "scene/main/viewport.h"

void WorkspacePane::set_content(Control *p_content) {
	if (content == p_content) {
		return;
	}
	if (content && content->get_parent() == this) {
		remove_child(content);
	}
	content = p_content;
	if (content) {
		if (content->get_parent()) {
			content->get_parent()->remove_child(content);
		}
		add_child(content);
		content->set_h_size_flags(SIZE_EXPAND_FILL);
		content->set_v_size_flags(SIZE_EXPAND_FILL);
	}
	set_process_input(content != nullptr);
}

WorkspacePane *WorkspacePane::split(bool p_vertical, Control *p_new_content, bool p_new_on_second) {
	if (!is_leaf()) {
		return nullptr;
	}

	// Detach current content; it will move into one of the two new child panes.
	Control *existing = content;
	if (existing && existing->get_parent() == this) {
		remove_child(existing);
	}
	content = nullptr;
	set_process_input(false);

	split_container = memnew(SplitContainer);
	split_container->set_vertical(p_vertical);
	split_container->set_h_size_flags(SIZE_EXPAND_FILL);
	split_container->set_v_size_flags(SIZE_EXPAND_FILL);
	add_child(split_container);

	first = workspace ? workspace->make_pane() : memnew(WorkspacePane);
	second = workspace ? workspace->make_pane() : memnew(WorkspacePane);
	split_container->add_child(first);
	split_container->add_child(second);

	WorkspacePane *existing_pane = p_new_on_second ? first : second;
	WorkspacePane *new_pane = p_new_on_second ? second : first;
	existing_pane->set_content(existing);
	new_pane->set_content(p_new_content);
	return new_pane;
}

void WorkspacePane::input(const Ref<InputEvent> &p_event) {
	if (!workspace || !is_leaf() || !is_visible_in_tree()) {
		return;
	}

	Ref<InputEventMouseButton> mb = p_event;
	if (mb.is_null() || !mb->is_pressed()) {
		return;
	}

	if (get_global_rect().has_point(mb->get_position())) {
		workspace->set_focused_pane(this);
	}
}

void WorkspacePane::collapse_split(WorkspacePane *p_removed) {
	// G2 S8: absorb the surviving child, freeing the removed subtree. Private access
	// across WorkspacePane instances is what lets the survivor's split state move here.
	ERR_FAIL_COND(is_leaf());
	ERR_FAIL_COND(p_removed != first && p_removed != second);
	WorkspacePane *survivor = (p_removed == first) ? second : first;

	SplitContainer *old_split = split_container;
	split_container = nullptr;
	first = nullptr;
	second = nullptr;

	if (survivor->is_leaf()) {
		Control *surviving_content = survivor->get_content();
		const uint32_t surviving_pane_id = survivor->get_pane_id();
		survivor->set_content(nullptr); // Detach before the shell is freed.
		remove_child(old_split);
		memdelete(old_split); // Frees p_removed's subtree and the survivor shell.
		set_pane_id(surviving_pane_id);
		set_content(surviving_content);
	} else {
		// The survivor is itself a split: its divider subtree moves up into this pane.
		SplitContainer *inner = survivor->split_container;
		WorkspacePane *inner_first = survivor->first;
		WorkspacePane *inner_second = survivor->second;
		survivor->split_container = nullptr;
		survivor->first = nullptr;
		survivor->second = nullptr;
		survivor->remove_child(inner);
		remove_child(old_split);
		memdelete(old_split);
		split_container = inner;
		first = inner_first;
		second = inner_second;
		add_child(inner);
	}
}

int WorkspacePane::get_leaf_count() const {
	if (is_leaf()) {
		return 1;
	}

	int count = 0;
	if (first) {
		count += first->get_leaf_count();
	}
	if (second) {
		count += second->get_leaf_count();
	}
	return count;
}

WorkspacePane *WorkspacePane::of(Node *p_node) {
	for (Node *n = p_node; n; n = n->get_parent()) {
		if (WorkspacePane *pane = Object::cast_to<WorkspacePane>(n)) {
			return pane;
		}
	}
	return nullptr;
}

Dictionary WorkspacePane::to_dict() const {
	// G2 M6.1: emit this subtree as a schema-v1 node. A member (not a free function) so it can read the
	// private split state; recurses through both children of a split.
	Dictionary d;
	if (is_leaf()) {
		d["t"] = "leaf";
		d["id"] = (int)pane_id;
		return d;
	}
	d["t"] = "split";
	d["vert"] = split_container->is_vertical();
	d["off"] = split_container->get_split_offset();
	d["a"] = first ? first->to_dict() : Dictionary();
	d["b"] = second ? second->to_dict() : Dictionary();
	return d;
}

WorkspacePane *WorkspacePane::from_dict(const Dictionary &p_dict, EditorWorkspace *p_workspace, bool &r_ok) {
	// G2 M6.1: rebuild one subtree. Any anomaly flips r_ok false; the caller frees the returned partial
	// tree (a WorkspacePane frees its whole child chain), so we can return the node built so far.
	if (!r_ok) {
		return nullptr;
	}
	const String t = p_dict.get("t", String());
	WorkspacePane *pane = p_workspace ? p_workspace->make_pane() : memnew(WorkspacePane);

	if (t == "leaf") {
		pane->set_pane_id((uint32_t)(int)p_dict.get("id", 0));
		pane->set_content(memnew(TabbedDocumentHost)); // Empty; the session store adds the tabs (M6.3).
		return pane;
	}
	if (t == "split") {
		WorkspacePane *a = from_dict(p_dict.get("a", Dictionary()), p_workspace, r_ok);
		WorkspacePane *b = from_dict(p_dict.get("b", Dictionary()), p_workspace, r_ok);
		if (!r_ok || !a || !b) {
			r_ok = false;
			if (a) {
				memdelete(a);
			}
			if (b) {
				memdelete(b);
			}
			return pane;
		}
		pane->set_pane_id(0); // Split panes carry no persisted id (to_dict writes ids on leaves only); keep
		// it 0 so a make_pane auto-id can't collide with a leaf's saved id in find_pane_by_id.
		SplitContainer *sc = memnew(SplitContainer);
		sc->set_vertical((bool)p_dict.get("vert", false));
		sc->set_h_size_flags(SIZE_EXPAND_FILL);
		sc->set_v_size_flags(SIZE_EXPAND_FILL);
		pane->split_container = sc;
		pane->first = a;
		pane->second = b;
		sc->add_child(a);
		sc->add_child(b);
		pane->add_child(sc);
		sc->set_split_offset((int)p_dict.get("off", 0)); // Applied (and clamped to size) on the next sort.
		return pane;
	}

	r_ok = false; // Unknown node kind.
	return pane;
}

WorkspacePane::WorkspacePane() {
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_v_size_flags(SIZE_EXPAND_FILL);
	add_theme_constant_override("separation", 0);
}

void EditorWorkspace::_bind_methods() {
	ADD_SIGNAL(MethodInfo("focused_pane_changed", PropertyInfo(Variant::OBJECT, "pane", PROPERTY_HINT_NODE_TYPE, "WorkspacePane")));
}

WorkspacePane *EditorWorkspace::make_pane() {
	WorkspacePane *pane = memnew(WorkspacePane);
	pane->set_workspace(this);
	pane->set_pane_id(next_pane_id++); // G2 M6.1: stable id for the session store (overwritten on restore).
	return pane;
}

void EditorWorkspace::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			Viewport *viewport = get_viewport();
			if (viewport) {
				viewport->connect("gui_focus_changed", callable_mp(this, &EditorWorkspace::_on_gui_focus_changed));
			}
		} break;
	}
}

void EditorWorkspace::_on_gui_focus_changed(Control *p_control) {
	for (Control *control = p_control; control; control = Object::cast_to<Control>(control->get_parent())) {
		WorkspacePane *pane = Object::cast_to<WorkspacePane>(control);
		if (pane && pane->get_workspace() == this && pane->is_leaf()) {
			set_focused_pane(pane);
			return;
		}
	}
}

void EditorWorkspace::set_focused_pane(WorkspacePane *p_pane) {
	if (!p_pane) {
		return;
	}

	WorkspacePane *old_focused = focused_pane;
	if (old_focused == p_pane) {
		return;
	}
	if (old_focused) {
		if (TabbedDocumentHost *old_host = Object::cast_to<TabbedDocumentHost>(old_focused->get_content())) {
			old_host->set_context_active(false);
		}
	}

	focused_pane = p_pane;
	if (TabbedDocumentHost *host = Object::cast_to<TabbedDocumentHost>(focused_pane->get_content())) {
		last_tabbed_pane = focused_pane;

		// G4: focusing a scene pane makes its scene the active edited scene, so the global bottom docks
		// (Animation, Terrain) follow whichever pane you click into — not only when you click its tab or
		// viewport. No-op for a non-scene current tab, and no-op when the scene is already active, so
		// re-clicking the same pane costs nothing.
		host->activate_current_document();

		// G2 S6a: the "current script" the ScriptEditor services act on follows pane focus — the
		// newly focused pane's active tab decides it (a script view, or null for any other kind).
		if (ScriptEditor *se = ScriptEditor::get_singleton()) {
			se->set_current_surface(host->get_current_view());
		}
		// G-Shader: the shader File menu likewise follows pane focus into the focused shader tab.
		if (ShaderEditorPlugin *sep = ShaderEditorPlugin::get_singleton()) {
			sep->set_current_surface(host->get_current_view());
		}
		// G2 M7.2a: the shared 2D/3D toolbar follows pane focus into the focused scene pane's header.
		if (EditorNode *en = EditorNode::get_singleton()) {
			en->update_scene_pane_toolbar(host->get_current_view());
		}
		host->set_context_active(true);
	}

	if (old_focused) {
		old_focused->queue_redraw();
	}
	focused_pane->queue_redraw();
	emit_signal(SNAME("focused_pane_changed"), focused_pane);
}

WorkspacePane *EditorWorkspace::_find_pane_showing(WorkspacePane *p_pane, EditorDocument *p_document) const {
	if (!p_pane) {
		return nullptr;
	}
	if (p_pane->is_leaf()) {
		if (TabbedDocumentHost *host = Object::cast_to<TabbedDocumentHost>(p_pane->get_content())) {
			if (host->has_document(p_document)) {
				return p_pane;
			}
		}
		return nullptr;
	}
	if (WorkspacePane *found = _find_pane_showing(p_pane->get_first(), p_document)) {
		return found;
	}
	return _find_pane_showing(p_pane->get_second(), p_document);
}

WorkspacePane *EditorWorkspace::find_pane_showing(EditorDocument *p_document) const {
	if (!p_document) {
		return nullptr;
	}
	return _find_pane_showing(root_pane, p_document);
}

WorkspacePane *EditorWorkspace::find_pane_by_id(uint32_t p_id) const {
	return p_id == 0 ? nullptr : _find_pane_by_id(root_pane, p_id); // G2 M6.2 (0 == unassigned; e.g. split panes).
}

WorkspacePane *EditorWorkspace::_find_pane_by_id(WorkspacePane *p_pane, uint32_t p_id) const {
	if (!p_pane) {
		return nullptr;
	}
	if (p_pane->get_pane_id() == p_id) {
		return p_pane;
	}
	if (WorkspacePane *found = _find_pane_by_id(p_pane->get_first(), p_id)) {
		return found;
	}
	return _find_pane_by_id(p_pane->get_second(), p_id);
}

Vector<WorkspacePane *> EditorWorkspace::get_tabbed_leaves() const {
	Vector<WorkspacePane *> leaves;
	_gather_tabbed_leaves(root_pane, leaves);
	return leaves;
}

void EditorWorkspace::_gather_tabbed_leaves(WorkspacePane *p_pane, Vector<WorkspacePane *> &r_leaves) const {
	if (!p_pane) {
		return;
	}
	if (p_pane->is_leaf()) {
		if (Object::cast_to<TabbedDocumentHost>(p_pane->get_content())) {
			r_leaves.push_back(p_pane);
		}
		return;
	}
	_gather_tabbed_leaves(p_pane->get_first(), r_leaves);
	_gather_tabbed_leaves(p_pane->get_second(), r_leaves);
}

WorkspacePane *EditorWorkspace::resolve_target_pane_for_documents() {
	// (a) The focused pane, if it already hosts tabs — this is what keeps "open a script from a
	// script" in the SAME pane (the focused pane IS a TabbedDocumentHost while you edit a script).
	WorkspacePane *focused = get_focused_pane();
	if (focused && Object::cast_to<TabbedDocumentHost>(focused->get_content())) {
		return focused;
	}
	// (b) The most-recently-focused tabbed leaf.
	if (last_tabbed_pane && last_tabbed_pane->is_leaf() && Object::cast_to<TabbedDocumentHost>(last_tabbed_pane->get_content())) {
		return last_tabbed_pane;
	}
	// (c) Split the focused leaf, minting a fresh tabbed host on the new (second/right) side.
	if (!focused || !focused->is_leaf()) {
		focused = root_pane;
	}
	TabbedDocumentHost *host = memnew(TabbedDocumentHost);
	WorkspacePane *new_pane = focused->split(false, host, true);
	if (new_pane) {
		last_tabbed_pane = new_pane;
		return new_pane;
	}
	memdelete(host); // split failed (target not a leaf); nothing hosts the orphan.
	return nullptr;
}

WorkspacePane *EditorWorkspace::_find_tabbed_leaf(WorkspacePane *p_pane) const {
	if (!p_pane) {
		return nullptr;
	}
	if (p_pane->is_leaf()) {
		return Object::cast_to<TabbedDocumentHost>(p_pane->get_content()) ? p_pane : nullptr;
	}
	if (WorkspacePane *found = _find_tabbed_leaf(p_pane->get_first())) {
		return found;
	}
	return _find_tabbed_leaf(p_pane->get_second());
}

void EditorWorkspace::close_pane(WorkspacePane *p_pane) {
	// G2 S8: deliberate pane close. Remaining tabs drain through the host close pipeline
	// first (state cache, script-close notifications), then the parent split collapses
	// onto the sibling. The root pane never closes.
	if (!p_pane || p_pane == root_pane || !p_pane->is_inside_tree()) {
		return;
	}
	if (TabbedDocumentHost *host = Object::cast_to<TabbedDocumentHost>(p_pane->get_content())) {
		const ObjectID pane_id = p_pane->get_instance_id();
		while (host->get_document_count() > 0) {
			// drop_tab, not close_tab: a scene tab's close_tab routes through the async
			// unsaved-changes prompt and leaves the tab in place — the drain would spin.
			// Pane close keeps its "views go away, scenes stay open" semantics.
			host->drop_tab(0);
			if (!ObjectDB::get_instance(pane_id)) {
				return; // Something already tore the pane down mid-drain.
			}
		}
	}

	SplitContainer *sc = Object::cast_to<SplitContainer>(p_pane->get_parent());
	ERR_FAIL_NULL(sc);
	WorkspacePane *parent = Object::cast_to<WorkspacePane>(sc->get_parent());
	ERR_FAIL_NULL(parent);

	// Track by ObjectID across the collapse: the closed subtree and (when the survivor
	// is a split) the survivor's own shell are freed by it.
	const ObjectID focused_id = focused_pane ? focused_pane->get_instance_id() : ObjectID();
	const ObjectID last_tabbed_id = last_tabbed_pane ? last_tabbed_pane->get_instance_id() : ObjectID();

	parent->collapse_split(p_pane);

	focused_pane = Object::cast_to<WorkspacePane>(ObjectDB::get_instance(focused_id));
	last_tabbed_pane = Object::cast_to<WorkspacePane>(ObjectDB::get_instance(last_tabbed_id));
	if (!last_tabbed_pane) {
		last_tabbed_pane = _find_tabbed_leaf(root_pane);
	}
	if (!focused_pane) {
		set_focused_pane(last_tabbed_pane ? last_tabbed_pane : root_pane);
	} else {
		focused_pane->queue_redraw(); // Focus border hides when one leaf remains.
	}
}

void EditorWorkspace::_close_pane_by_id(ObjectID p_pane_id) {
	close_pane(Object::cast_to<WorkspacePane>(ObjectDB::get_instance(p_pane_id)));
}

void EditorWorkspace::queue_close_pane(WorkspacePane *p_pane) {
	if (!p_pane) {
		return;
	}
	callable_mp(this, &EditorWorkspace::_close_pane_by_id).call_deferred(p_pane->get_instance_id());
}

WorkspacePane *EditorWorkspace::split_pane_with_tab(WorkspacePane *p_pane, int p_tab, bool p_vertical, bool p_new_on_second) {
	// G2 S8 / G6: the tab-bar context-menu split is just a same-pane edge move; delegate to the general
	// drag-to-split primitive so both paths share the detach/split/adopt logic.
	ERR_FAIL_NULL_V(p_pane, nullptr);
	TabbedDocumentHost *host = Object::cast_to<TabbedDocumentHost>(p_pane->get_content());
	ERR_FAIL_NULL_V(host, nullptr);
	return move_tab_into_pane(host, p_tab, p_pane, false, p_vertical, p_new_on_second);
}

WorkspacePane *EditorWorkspace::move_tab_into_pane(TabbedDocumentHost *p_source, int p_tab, WorkspacePane *p_target, bool p_center, bool p_vertical, bool p_new_on_second) {
	// G6: shared by the tab-bar context menu (same source & target) and the drag-to-split compass
	// (source may be another pane). The dragged tab's DocumentView is re-homed live — no close/reopen.
	ERR_FAIL_NULL_V(p_source, nullptr);
	ERR_FAIL_NULL_V(p_target, nullptr);
	TabbedDocumentHost *target_host = Object::cast_to<TabbedDocumentHost>(p_target->get_content());
	ERR_FAIL_NULL_V(target_host, nullptr);
	ERR_FAIL_INDEX_V(p_tab, p_source->get_document_count(), nullptr);

	const bool same_host = (p_source == target_host);
	if (p_center && same_host) {
		return p_target; // Dropping a tab back onto its own pane's center: nothing to move.
	}
	if (!p_center && same_host && p_source->get_document_count() < 2) {
		return nullptr; // Splitting out the only tab would just bounce back into an empty pane.
	}

	EditorDocument *doc = p_source->get_document(p_tab);
	DocumentView *view = p_source->detach_tab(p_tab);
	ERR_FAIL_NULL_V(view, nullptr);

	WorkspacePane *result = nullptr;
	if (p_center) {
		target_host->adopt_tab(doc, view);
		result = p_target;
	} else {
		TabbedDocumentHost *new_host = memnew(TabbedDocumentHost);
		WorkspacePane *new_pane = p_target->split(p_vertical, new_host, p_new_on_second);
		if (!new_pane) {
			memdelete(new_host);
			p_source->adopt_tab(doc, view); // Split refused; put the tab back.
			return nullptr;
		}
		new_host->adopt_tab(doc, view);
		result = new_pane;
	}

	// A cross-pane move can empty the source pane; drain it through the close pipeline (root never closes).
	if (!same_host && p_source->get_document_count() == 0) {
		if (WorkspacePane *source_pane = WorkspacePane::of(p_source)) {
			queue_close_pane(source_pane);
		}
	}

	set_focused_pane(result);
	return result;
}

Dictionary EditorWorkspace::save_geometry() const {
	// G2 M6.1: schema-v1 wrapper around the recursive root node. "next" persists the id counter so
	// restored ids never collide with post-restore splits.
	Dictionary d;
	d["v"] = 1;
	d["next"] = (int)next_pane_id;
	d["root"] = root_pane ? root_pane->to_dict() : Dictionary();
	return d;
}

bool EditorWorkspace::load_geometry(const Dictionary &p_geometry) {
	// G2 M6.1: rebuild the whole tree, or refuse (leaving the live tree intact) on any anomaly.
	if ((int)p_geometry.get("v", 0) != 1) {
		return false;
	}
	bool ok = true;
	WorkspacePane *new_root = WorkspacePane::from_dict(p_geometry.get("root", Dictionary()), this, ok);
	if (!ok || !new_root) {
		if (new_root) {
			memdelete(new_root); // Frees the partial subtree; the current tree is untouched.
		}
		return false;
	}

	// Swap the rebuilt tree in for the live one.
	if (root_pane) {
		remove_child(root_pane);
		memdelete(root_pane);
	}
	root_pane = new_root;
	add_child(root_pane);

	// Restore the id counter to the saved value (from_dict overwrote every auto-assigned id with the
	// stored one, so the counter's transient bumps during rebuild don't matter). The saved "next" is
	// always past every stored id, so a later split can't collide — and save->load->save round-trips.
	next_pane_id = MAX((uint32_t)(int)p_geometry.get("next", 1), 1u);

	// Focus is left UNSET here on purpose: during a restore the leaves are still empty, and
	// set_focused_pane's side effects (script surface, scene-pane toolbar) reach into content that isn't
	// placed yet. get_focused_pane falls back to the root while null, and the restore driver calls
	// set_focused_pane once — after it fills the panes — so those side effects bind correctly.
	last_tabbed_pane = _find_tabbed_leaf(root_pane);
	focused_pane = nullptr;
	return true;
}

EditorWorkspace::EditorWorkspace() {
	set_h_size_flags(SIZE_EXPAND_FILL);
	set_v_size_flags(SIZE_EXPAND_FILL);
	add_theme_constant_override("separation", 0);

	root_pane = make_pane();
	add_child(root_pane);
	focused_pane = root_pane;
}
