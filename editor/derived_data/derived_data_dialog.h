/**************************************************************************/
/*  derived_data_dialog.h                                                 */
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

#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "scene/gui/dialogs.h"

class CheckBox;
class EditorFileSystemDirectory;
class HBoxContainer;
class ItemList;
class Label;
class LineEdit;
class MarginContainer;
class Tree;
class TreeItem;

struct TestDerivedDataDialogAccess;

// Size report and cleanup UI for the bundles allocated by EditorDerivedData.
//
// The unit of everything here is the bundle directory, never the individual file: a
// bundle is what the allocator hands out, what the manifest describes, and therefore
// the only thing that can be reasoned about or reclaimed as a whole. Rows are grouped
// scene -> owner node -> bundle so the question the tool actually answers ("what is
// this megabyte doing here, and who asked for it?") is readable off the tree.
class DerivedDataDialog : public ConfirmationDialog {
	GDCLASS(DerivedDataDialog, ConfirmationDialog);

	friend struct TestDerivedDataDialogAccess;

	enum Column {
		COL_BUNDLE,
		COL_SLOT,
		COL_SIZE,
		COL_STATE,
		COL_MAX,
	};

	// Ordered worst-last: parent rows show the highest-numbered state among their children.
	enum BundleState {
		STATE_OK,
		STATE_SHARED,
		STATE_UNREFERENCED,
		STATE_LEGACY,
		STATE_ORPHAN,
		STATE_UNKNOWN,
		STATE_MAX,
	};

	enum RowButton {
		BUTTON_SHOW_DEPENDENTS,
		BUTTON_OPEN_OWNER_SCENE,
		BUTTON_SHOW_IN_FILESYSTEM,
	};

	struct Entry {
		String path; // Bundle directory, or the file itself for unmanaged leftovers.
		bool is_bundle = false;
		String slot;
		String manifest_scene_path; // Decoration recorded at bake time; may be stale.
		String scene_uid;
		PackedInt32Array id_chain;
		String node_path;
		String owner_scene; // scene_uid resolved against the UID cache; empty when it does not resolve.
		String resolved_node_path;
		bool manifest_valid = false;
		uint64_t size = 0;
		BundleState state = STATE_LEGACY;
		Vector<String> referrers;
	};

	struct NodeGroup {
		String display;
		String icon_class;
		bool owner_resolved = false;
		uint64_t size = 0;
		int worst = STATE_OK;
		LocalVector<int> entries;
	};

	struct SceneGroup {
		String display;
		bool unmanaged = false;
		uint64_t size = 0;
		int worst = STATE_OK;
		LocalVector<NodeGroup> nodes;
	};

	// The report is size-ordered, and the direction is runtime state, so the comparators
	// carry it (LocalVector::sort_custom() cannot pass anything to its comparator).
	struct EntrySizeComparator {
		const LocalVector<Entry> *entries = nullptr;
		bool descending = true;
		bool operator()(int p_a, int p_b) const {
			const uint64_t a = (*entries)[p_a].size;
			const uint64_t b = (*entries)[p_b].size;
			return descending ? a > b : a < b;
		}
	};

	struct NodeGroupComparator {
		bool descending = true;
		bool operator()(const NodeGroup &p_a, const NodeGroup &p_b) const {
			return descending ? p_a.size > p_b.size : p_a.size < p_b.size;
		}
	};

	struct SceneGroupComparator {
		bool descending = true;
		bool operator()(const SceneGroup &p_a, const SceneGroup &p_b) const {
			// The synthetic unmanaged group always sinks to the bottom, whatever its size.
			if (p_a.unmanaged != p_b.unmanaged) {
				return p_b.unmanaged;
			}
			return descending ? p_a.size > p_b.size : p_a.size < p_b.size;
		}
	};

	LocalVector<Entry> entries;
	LocalVector<SceneGroup> groups;

	// Derived roots from the registry, cached for the duration of a scan.
	PackedStringArray derived_roots;
	// Resource path -> the files that reference it. Populated from the whole
	// EditorFileSystem tree, normalized through ResourceUID (see _record_dependency).
	HashMap<String, HashSet<String>> refs;
	// Scene path -> stable node-id chains and their current display paths. Cached because
	// the same scene owns many bundles and re-parsing it per bundle would dominate the scan.
	struct SceneNodeIdentity {
		PackedInt32Array id_path;
		String node_path;
	};
	HashMap<String, Vector<SceneNodeIdentity>> scene_nodes;

	bool sort_descending = true;

	Tree *tree = nullptr;
	MarginContainer *tree_mc = nullptr;
	HBoxContainer *unsaved_box = nullptr;
	Label *unsaved_label = nullptr;
	HBoxContainer *filter_box = nullptr;
	LineEdit *filter = nullptr;
	CheckBox *problems_only = nullptr;
	Label *summary = nullptr;
	Label *empty_label = nullptr;

	ConfirmationDialog *delete_confirm = nullptr;
	AcceptDialog *dependents_dialog = nullptr;
	ItemList *dependents_list = nullptr;

	Vector<String> pending_delete;

	void _record_dependency(const String &p_referrer, const String &p_dep);
	void _record_referrer(const String &p_path, const String &p_referrer);
	void _collect_script_literal_references(const String &p_referrer);
	void _collect_references(EditorFileSystemDirectory *p_dir);
	void _scan_root(const String &p_root);
	void _scan_unmanaged(const String &p_dir);
	static void _walk_bundle(const String &p_dir, Vector<String> &r_files, uint64_t &r_size);
	String _resolve_node_by_identity(const String &p_scene_path, const PackedInt32Array &p_id_chain);
	void _classify(Entry &p_entry);
	String _slot_icon_class(const String &p_slot) const;

	void _rescan();
	void _build_groups();
	void _sort_groups();
	void _update_summary();
	void _update_tree();
	void _update_unsaved_banner();
	int _unsaved_scene_count() const;
	bool _has_unsaved_external_data() const;
	bool _has_unsaved_data() const;

	void _filter_changed(const String &p_text);
	void _problems_toggled(bool p_pressed);
	void _column_title_clicked(int p_column, int p_mouse_button);
	void _save_all_pressed();
	void _button_pressed(Object *p_item, int p_column, int p_id, MouseButton p_mouse_button);
	void _collect_checked(TreeItem *p_item);
	void _delete_confirmed();

	static String _state_text(BundleState p_state);
	static StringName _state_icon(BundleState p_state);
	static String _state_tooltip(BundleState p_state);
	static bool _entry_deletable(const Entry &p_entry);
	static String _short_slot(const String &p_slot);

	void ok_pressed() override;

protected:
	void _notification(int p_what);

public:
	void show();
	DerivedDataDialog();
};
