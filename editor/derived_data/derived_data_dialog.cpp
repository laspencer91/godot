/**************************************************************************/
/*  derived_data_dialog.cpp                                               */
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

#include "derived_data_dialog.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_uid.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/templates/sort_array.h"
#include "editor/derived_data/editor_derived_data.h"
#include "editor/docks/filesystem_dock.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/item_list.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/margin_container.h"
#include "scene/gui/tree.h"
#include "scene/resources/packed_scene.h"
#include "scene/scene_string_names.h"

static const char *MANIFEST_FILE_NAME = "manifest.cfg";
static const char *UNMANAGED_GROUP_KEY = "\x01unmanaged";

// Resolves a "uid://..." text to its path, or returns empty if the UID is not in the cache.
// Deliberately not ResourceUID::ensure_path(): that reports an unknown UID with an ERR_PRINT,
// and both callers here ask about UIDs that are *expected* to be dead sometimes (a bundle
// whose owning scene is gone is precisely what an Orphan is).
static String resolve_uid_quietly(const String &p_uid_text) {
	ResourceUID *uid_cache = ResourceUID::get_singleton();
	if (!uid_cache) {
		return String();
	}
	const ResourceUID::ID id = uid_cache->text_to_id(p_uid_text);
	if (id == ResourceUID::INVALID_ID || !uid_cache->has_id(id)) {
		return String();
	}
	return uid_cache->get_id_path(id);
}

// Reference collection /////////////////////////////////////////////////////

void DerivedDataDialog::_record_dependency(const String &p_referrer, const String &p_dep) {
	if (p_dep.is_empty()) {
		return;
	}

	// A dependency string is not a path. Producers write their outputs into scenes as
	// "uid://abc123", and the editor cache also carries the "uid://abc::Type::res://path"
	// and "res://path::Type" shapes. Matching a bundle against the raw string would miss
	// every UID-form reference, classify a live artifact as Unreferenced, and offer it for
	// deletion — the one bug in this dialog that destroys work. So every form a dependency
	// can take is resolved to a path, and a bundle counts as referenced if any of them
	// lands inside it.
	const int head_end = p_dep.find("::");
	const String head = head_end == -1 ? p_dep : p_dep.substr(0, head_end);

	_record_referrer(head, p_referrer);
	if (head_end != -1) {
		// The trailing slice is the path the file was saved with; keep it as a fallback for
		// UIDs that are no longer in the cache.
		const int tail_start = p_dep.find("::", head_end + 2);
		if (tail_start != -1) {
			_record_referrer(p_dep.substr(tail_start + 2), p_referrer);
		}
	}
	if (head.begins_with("uid://")) {
		_record_referrer(resolve_uid_quietly(head), p_referrer);
	}
}

void DerivedDataDialog::_record_referrer(const String &p_path, const String &p_referrer) {
	// Only paths that could name a bundle file are worth indexing: the lookups in
	// _scan_root() are all concrete paths under a derived root, so anything else would
	// grow the map by every dependency in the project and never be read. Dropping a key
	// that can never be queried cannot make a live bundle look unreferenced.
	if (p_path.is_empty()) {
		return;
	}
	for (const String &root : derived_roots) {
		if (p_path.begins_with(root)) {
			refs[p_path].insert(p_referrer);
			return;
		}
	}
}

void DerivedDataDialog::_collect_references(EditorFileSystemDirectory *p_dir) {
	if (!p_dir) {
		return;
	}
	for (int i = 0; i < p_dir->get_subdir_count(); i++) {
		_collect_references(p_dir->get_subdir(i));
	}
	for (int i = 0; i < p_dir->get_file_count(); i++) {
		const String referrer = p_dir->get_file_path(i);
		const Vector<String> deps = p_dir->get_file_deps(i);
		for (const String &dep : deps) {
			_record_dependency(referrer, dep);
		}

		// The cached deps above are not enough on their own. get_file_deps() resolves UID
		// references against the cache and *drops* any it cannot resolve (editor_file_system.cpp,
		// the `continue` in its uid branch). A bundle referenced only by such a UID would arrive
		// here with no referrers at all, be classified Unreferenced, and be offered for deletion.
		// So scenes and resources — the only files that can hold a reference to a bake artifact —
		// are also read for their raw dependency strings, which is where the uid:// and
		// path::Type forms _record_dependency() guards against actually appear. The two sets are
		// unioned rather than swapped: an extra referrer only ever makes the tool more reluctant
		// to delete, which is the safe direction to err.
		const String ext = referrer.get_extension().to_lower();
		if (ext == "tscn" || ext == "scn" || ext == "tres" || ext == "res") {
			List<String> raw_deps;
			ResourceLoader::get_dependencies(referrer, &raw_deps, false);
			for (const String &dep : raw_deps) {
				_record_dependency(referrer, dep);
			}
		}
	}
}

// Disk walking /////////////////////////////////////////////////////////////

// Lists the bundle's tree once, collecting both the file paths and the total size. Sidecars
// (.import/.uid/manifest.cfg) are counted deliberately: the reported number is what deleting
// the bundle actually gives back, not the size of the "interesting" files.
void DerivedDataDialog::_walk_bundle(const String &p_dir, Vector<String> &r_files, uint64_t &r_size) {
	Ref<DirAccess> da = DirAccess::open(p_dir);
	if (da.is_null()) {
		return;
	}
	da->list_dir_begin();
	for (String name = da->get_next(); !name.is_empty(); name = da->get_next()) {
		if (name == "." || name == "..") {
			continue;
		}
		const String path = p_dir.path_join(name);
		if (da->current_is_dir()) {
			_walk_bundle(path, r_files, r_size);
		} else {
			r_files.push_back(path);
			const int64_t file_size = FileAccess::get_size(path);
			if (file_size > 0) {
				r_size += (uint64_t)file_size;
			}
		}
	}
	da->list_dir_end();
}

void DerivedDataDialog::_scan_unmanaged(const String &p_dir) {
	// Anything under a derived root that is not inside a bundle predates the allocator
	// (Phase 5 conversions) or was left behind by hand. It has no identity to replay, so it
	// is always deletable and is listed per file rather than pretending to be a bundle.
	for (const String &file : DirAccess::get_files_at(p_dir)) {
		Entry entry;
		entry.path = p_dir.path_join(file);
		const int64_t file_size = FileAccess::get_size(entry.path);
		entry.size = file_size > 0 ? (uint64_t)file_size : 0;
		entries.push_back(entry);
	}
	for (const String &sub : DirAccess::get_directories_at(p_dir)) {
		_scan_root(p_dir.path_join(sub));
	}
}

void DerivedDataDialog::_scan_root(const String &p_dir) {
	if (!DirAccess::dir_exists_absolute(p_dir)) {
		return;
	}

	if (FileAccess::exists(p_dir.path_join(MANIFEST_FILE_NAME))) {
		// This directory is a bundle. A bundle is atomic: never descend into it, never offer
		// its individual files, never measure anything smaller than the whole thing.
		const Dictionary manifest = EditorDerivedData::get_singleton()->describe(p_dir.path_join(MANIFEST_FILE_NAME));

		Entry entry;
		entry.path = p_dir;
		entry.is_bundle = true;
		entry.slot = manifest.get("slot", "");
		entry.manifest_scene_path = manifest.get("scene_path", "");
		entry.scene_uid = manifest.get("scene_uid", "");
		entry.node_path = manifest.get("node_path", "");

		Vector<String> files;
		_walk_bundle(p_dir, files, entry.size);

		const String inside_prefix = p_dir + "/";
		for (const String &file : files) {
			if (file.get_file() == MANIFEST_FILE_NAME) {
				continue;
			}
			const String ext = file.get_extension();
			if (ext == "import" || ext == "uid") {
				continue;
			}
			// A bundle's own files reference each other (a .lmbake points at its .exr atlases);
			// counting those would make every bundle look referenced. Only referrers from
			// outside the bundle prove that something in the project still wants it. The
			// trailing separator matters: without it a sibling bundle sharing a name prefix
			// would be mistaken for an internal referrer and its reference silently dropped.
			const HashSet<String> *referrers = refs.getptr(file);
			if (referrers) {
				for (const String &referrer : *referrers) {
					if (!referrer.begins_with(inside_prefix)) {
						entry.referrers.push_back(referrer);
					}
				}
			}
		}

		_classify(entry);
		entries.push_back(entry);
		return;
	}

	_scan_unmanaged(p_dir);
}

// Classification ///////////////////////////////////////////////////////////

// The two sides of the node-path comparison are produced by different APIs that disagree
// about the leading element. A manifest records Node::get_path_to(), which yields a bare
// "VoxelGI"; SceneState::get_node_path() prepends the "." for the root it is relative to
// and yields "./VoxelGI" for the same node. Only the root itself ("." from both) matched,
// so every non-root bundle read as an Orphan. Normalizing both sides is the fix; comparing
// NodePath objects instead would not be, because NodePath keeps that "." as a real element.
static String normalized_node_path(const String &p_node_path) {
	String path = p_node_path;
	while (path.begins_with("./")) {
		path = path.substr(2);
	}
	return path.is_empty() ? "." : path;
}

bool DerivedDataDialog::_scene_declares_node(const String &p_scene_path, const String &p_node_path) {
	HashMap<String, HashSet<String>>::Iterator cached = scene_nodes.find(p_scene_path);
	if (!cached) {
		cached = scene_nodes.insert(p_scene_path, HashSet<String>());
		Ref<PackedScene> scene = ResourceLoader::load(p_scene_path, "PackedScene");
		const Ref<SceneState> state = scene.is_valid() ? scene->get_state() : Ref<SceneState>();
		if (state.is_valid()) {
			for (int i = 0; i < state->get_node_count(); i++) {
				cached->value.insert(normalized_node_path(String(state->get_node_path(i))));
			}
		}
	}
	return cached->value.has(normalized_node_path(p_node_path));
}

void DerivedDataDialog::_classify(Entry &p_entry) {
	if (!p_entry.is_bundle) {
		p_entry.state = STATE_LEGACY;
		return;
	}

	// Identity first: a bundle whose owner cannot be found is orphaned no matter who still
	// links to it, because nothing can ever rebake into it again.
	if (!p_entry.scene_uid.is_empty()) {
		// Same contract as ResourceUID::ensure_path(), minus the ERR_PRINT it emits for a
		// UID it cannot resolve — which is the normal, expected case for an orphan.
		const String resolved = p_entry.scene_uid.begins_with("uid://") ? resolve_uid_quietly(p_entry.scene_uid) : p_entry.scene_uid;
		if (resolved.begins_with("res://") && FileAccess::exists(resolved)) {
			p_entry.owner_scene = resolved;
		}
	}
	if (p_entry.owner_scene.is_empty() || !_scene_declares_node(p_entry.owner_scene, p_entry.node_path)) {
		p_entry.state = STATE_ORPHAN;
		return;
	}

	if (p_entry.referrers.is_empty()) {
		p_entry.state = STATE_UNREFERENCED;
		return;
	}
	for (const String &referrer : p_entry.referrers) {
		if (referrer == p_entry.owner_scene) {
			p_entry.state = STATE_OK;
			return;
		}
	}
	// Referenced, but not by the scene the manifest says owns it — deleting it would break
	// somebody else's link, so it is never offered.
	p_entry.state = STATE_SHARED;
}

String DerivedDataDialog::_slot_icon_class(const String &p_slot) const {
	if (p_slot.is_empty() || !EditorDerivedData::get_singleton()) {
		return String();
	}
	const Dictionary slots = EditorDerivedData::get_singleton()->get_slots();
	if (!slots.has(p_slot)) {
		return String();
	}
	const Dictionary row = slots[p_slot];
	String type = row.get("type", "");
	if (type.is_empty()) {
		type = String(row.get("script_class", ""));
	}
	return type;
}

// Scan orchestration ///////////////////////////////////////////////////////

void DerivedDataDialog::_rescan() {
	entries.clear();
	groups.clear();

	derived_roots = EditorDerivedData::get_singleton() ? EditorDerivedData::get_singleton()->get_roots() : PackedStringArray();

	EditorProgress progress("derived_data_scan", TTR("Scanning Derived Data"), 2);

	progress.step(TTR("Collecting references..."), 0);
	if (EditorFileSystem::get_singleton()) {
		_collect_references(EditorFileSystem::get_singleton()->get_filesystem());
	}

	progress.step(TTR("Measuring bundles..."), 1);
	for (const String &root : derived_roots) {
		_scan_root(root);
	}

	_build_groups();

	// Scan-only scratch. The dialog lives for the whole editor session, so holding a
	// project-wide reference index and every owner scene's node table between openings
	// would be pure retention: everything the tree needs is in entries/groups by now.
	refs.clear();
	scene_nodes.clear();

	_update_unsaved_banner();
	_update_tree();
}

void DerivedDataDialog::_build_groups() {
	groups.clear();

	HashMap<String, int> group_lookup;
	// Build-time scratch only: the indices go stale the moment the groups are sorted.
	HashMap<String, int> node_lookup;

	for (uint32_t i = 0; i < entries.size(); i++) {
		const Entry &entry = entries[i];

		String group_key;
		String group_display;
		String node_key;
		String node_display;
		String node_icon;
		bool owner_resolved = true;

		if (!entry.is_bundle) {
			group_key = UNMANAGED_GROUP_KEY;
			group_display = TTR("Unmanaged (no manifest)");
			node_key = entry.path.get_base_dir();
			node_display = node_key;
			owner_resolved = false;
		} else {
			const String scene = entry.owner_scene.is_empty() ? entry.manifest_scene_path : entry.owner_scene;
			group_key = scene.is_empty() ? String("?") : scene;
			group_display = scene.is_empty() ? TTR("(unknown scene)") : scene.get_file().get_basename();
			node_key = entry.node_path;
			// _classify() already asked exactly this question: a bundle is an Orphan iff its
			// owner scene is missing or no longer declares the node.
			owner_resolved = entry.state != STATE_ORPHAN;
			node_display = owner_resolved ? entry.node_path : TTR("(owner not found)");
			node_icon = _slot_icon_class(entry.slot);
		}

		int group_index;
		if (const int *found = group_lookup.getptr(group_key)) {
			group_index = *found;
		} else {
			SceneGroup group;
			group.display = group_display;
			group.unmanaged = !entry.is_bundle;
			group_index = (int)groups.size();
			groups.push_back(group);
			group_lookup.insert(group_key, group_index);
		}

		SceneGroup &group = groups[group_index];
		const String qualified_node_key = group_key + "\x01" + node_key;
		int node_index;
		if (const int *found = node_lookup.getptr(qualified_node_key)) {
			node_index = *found;
		} else {
			NodeGroup node_group;
			node_group.display = node_display;
			node_group.icon_class = node_icon;
			node_group.owner_resolved = owner_resolved;
			node_index = (int)group.nodes.size();
			group.nodes.push_back(node_group);
			node_lookup.insert(qualified_node_key, node_index);
		}

		NodeGroup &node_group = group.nodes[node_index];
		node_group.entries.push_back((int)i);
		node_group.size += entry.size;
		node_group.worst = MAX(node_group.worst, (int)entry.state);
		group.size += entry.size;
		group.worst = MAX(group.worst, (int)entry.state);
	}

	_sort_groups();
	_update_summary();
}

// Size-ordered by default: the whole point of the report is "what is big".
void DerivedDataDialog::_sort_groups() {
	SortArray<SceneGroup, SceneGroupComparator> group_sorter;
	group_sorter.compare.descending = sort_descending;
	group_sorter.sort(groups.ptr(), groups.size());

	SortArray<NodeGroup, NodeGroupComparator> node_sorter;
	node_sorter.compare.descending = sort_descending;

	SortArray<int, EntrySizeComparator> entry_sorter;
	entry_sorter.compare.entries = &entries;
	entry_sorter.compare.descending = sort_descending;

	for (SceneGroup &group : groups) {
		node_sorter.sort(group.nodes.ptr(), group.nodes.size());
		for (NodeGroup &node_group : group.nodes) {
			entry_sorter.sort(node_group.entries.ptr(), node_group.entries.size());
		}
	}
}

// Totals over every entry, filter-independent — so they are computed with the groups
// rather than rebuilt on each keystroke in _update_tree().
void DerivedDataDialog::_update_summary() {
	uint64_t total_size = 0;
	uint64_t deletable_size = 0;
	int bundle_count = 0;
	int problem_count = 0;

	for (const Entry &entry : entries) {
		total_size += entry.size;
		if (entry.is_bundle) {
			bundle_count++;
		}
		if (entry.state != STATE_OK) {
			problem_count++;
		}
		if (_state_deletable(entry.state)) {
			deletable_size += entry.size;
		}
	}

	summary->set_text(vformat(TTR("%s in %d bundles · %s deletable · %d problems"),
			String::humanize_size(total_size), bundle_count, String::humanize_size(deletable_size), problem_count));
}

// Presentation /////////////////////////////////////////////////////////////

String DerivedDataDialog::_state_text(BundleState p_state) {
	switch (p_state) {
		case STATE_OK:
			return TTR("OK");
		case STATE_SHARED:
			return TTR("Shared");
		case STATE_UNREFERENCED:
			return TTR("Unreferenced");
		case STATE_LEGACY:
			return TTR("Legacy");
		case STATE_ORPHAN:
			return TTR("Orphan");
		default:
			return String();
	}
}

StringName DerivedDataDialog::_state_icon(BundleState p_state) {
	switch (p_state) {
		case STATE_OK:
			return SNAME("StatusSuccess");
		case STATE_SHARED:
		case STATE_UNREFERENCED:
			return SNAME("StatusWarning");
		case STATE_LEGACY:
			return SNAME("NodeWarning");
		case STATE_ORPHAN:
			return SNAME("StatusError");
		default:
			return SNAME("StatusWarning");
	}
}

String DerivedDataDialog::_state_tooltip(BundleState p_state) {
	switch (p_state) {
		case STATE_OK:
			return TTR("In use: the owning scene references this bundle and the manifest identity still replays against it. Cannot be deleted from here.");
		case STATE_SHARED:
			return TTR("Something references this bundle, but not the node the manifest says owns it. Deleting it would break that reference, so it cannot be deleted from here.");
		case STATE_UNREFERENCED:
			return TTR("The owning node still exists but nothing loads this bundle. Safe to reclaim; rebake to recreate it.");
		case STATE_LEGACY:
			return TTR("No bundle manifest — a pre-allocator leftover. Safe to reclaim.");
		case STATE_ORPHAN:
			return TTR("The owning scene or node recorded in the manifest no longer exists. Nothing can ever rebake into this bundle.");
		default:
			return String();
	}
}

bool DerivedDataDialog::_state_deletable(BundleState p_state) {
	return p_state != STATE_OK && p_state != STATE_SHARED;
}

String DerivedDataDialog::_short_slot(const String &p_slot) {
	const int dot = p_slot.find_char('.');
	return dot == -1 ? p_slot : p_slot.substr(dot + 1);
}

int DerivedDataDialog::_unsaved_scene_count() const {
	int count = 0;
	EditorData &editor_data = EditorNode::get_editor_data();
	for (int i = 0; i < editor_data.get_edited_scene_count(); i++) {
		if (editor_data.is_scene_changed(i)) {
			count++;
		}
	}
	return count;
}

void DerivedDataDialog::_update_unsaved_banner() {
	const int unsaved = _unsaved_scene_count();
	// The scan reads the project off disk, so a reference that only exists in an unsaved
	// buffer is invisible to it and the bundle it points at would look Unreferenced.
	// Reading the size report is still harmless, so the report stays live and only the
	// destructive half is withheld.
	unsaved_box->set_visible(unsaved > 0);
	if (unsaved > 0) {
		unsaved_label->set_text(vformat(TTR("%d scene(s) have unsaved changes — deletion disabled."), unsaved));
	}
	get_ok_button()->set_disabled(unsaved > 0);
}

void DerivedDataDialog::_update_tree() {
	tree->clear();

	if (entries.is_empty()) {
		tree_mc->hide();
		filter_box->hide();
		summary->hide();
		empty_label->show();
		return;
	}
	empty_label->hide();
	tree_mc->show();
	filter_box->show();
	summary->show();

	const String filter_text = filter->get_text().strip_edges();
	const bool hide_ok = problems_only->is_pressed();

	const Ref<Texture2D> scene_icon = EditorNode::get_singleton()->get_class_icon("PackedScene");
	const Ref<Texture2D> folder_icon = tree->get_theme_icon(SNAME("folder"), SNAME("FileDialog"));
	const Ref<Texture2D> warning_icon = tree->get_editor_theme_icon(SNAME("NodeWarning"));
	const Ref<Texture2D> dependents_icon = tree->get_editor_theme_icon(SNAME("GuiVisibilityVisible"));
	const Ref<Texture2D> open_scene_icon = tree->get_editor_theme_icon(SNAME("PackedScene"));
	const Ref<Texture2D> filesystem_icon = tree->get_editor_theme_icon(SNAME("Filesystem"));
	const Ref<Texture2D> node_fallback_icon = EditorNode::get_singleton()->get_class_icon("Node");

	// Every row asks for its state's icon, label and tooltip, so resolve the five states
	// once instead of once per row (the tooltips in particular are full sentences).
	Ref<Texture2D> state_icons[STATE_MAX];
	String state_texts[STATE_MAX];
	String state_tooltips[STATE_MAX];
	for (int i = 0; i < STATE_MAX; i++) {
		state_icons[i] = tree->get_editor_theme_icon(_state_icon((BundleState)i));
		state_texts[i] = _state_text((BundleState)i);
		state_tooltips[i] = _state_tooltip((BundleState)i);
	}
	const String dependents_tooltip = TTR("Show Dependents");
	const String open_scene_tooltip = TTR("Open Owner Scene");
	const String filesystem_tooltip = TTR("Show in FileSystem");

	TreeItem *root = tree->create_item();

	for (const SceneGroup &group : groups) {
		TreeItem *group_item = nullptr;

		for (const NodeGroup &node_group : group.nodes) {
			TreeItem *node_item = nullptr;

			for (const int entry_index : node_group.entries) {
				const Entry &entry = entries[entry_index];

				if (hide_ok && entry.state == STATE_OK) {
					continue;
				}
				if (!filter_text.is_empty()) {
					const bool hit = entry.path.containsn(filter_text) ||
							entry.slot.containsn(filter_text) ||
							group.display.containsn(filter_text) ||
							node_group.display.containsn(filter_text);
					if (!hit) {
						continue;
					}
				}

				if (!group_item) {
					group_item = tree->create_item(root);
					group_item->set_text(COL_BUNDLE, group.display);
					group_item->set_icon(COL_BUNDLE, group.unmanaged ? warning_icon : scene_icon);
					group_item->set_text(COL_SIZE, String::humanize_size(group.size));
					group_item->set_text_alignment(COL_SIZE, HORIZONTAL_ALIGNMENT_RIGHT);
					group_item->set_text(COL_STATE, state_texts[group.worst]);
					group_item->set_icon(COL_STATE, state_icons[group.worst]);
					// A clean scene stays folded; anything with a problem opens itself so the
					// report reads top-down without hunting.
					group_item->set_collapsed(group.worst == STATE_OK);
				}
				if (!node_item) {
					node_item = tree->create_item(group_item);
					node_item->set_text(COL_BUNDLE, node_group.display);
					Ref<Texture2D> node_icon = node_fallback_icon;
					if (!node_group.owner_resolved) {
						node_icon = warning_icon;
					} else if (!node_group.icon_class.is_empty()) {
						node_icon = EditorNode::get_singleton()->get_class_icon(node_group.icon_class, "Node");
					}
					node_item->set_icon(COL_BUNDLE, node_icon);
					node_item->set_text(COL_SIZE, String::humanize_size(node_group.size));
					node_item->set_text_alignment(COL_SIZE, HORIZONTAL_ALIGNMENT_RIGHT);
					node_item->set_text(COL_STATE, state_texts[node_group.worst]);
					node_item->set_icon(COL_STATE, state_icons[node_group.worst]);
				}

				TreeItem *item = tree->create_item(node_item);
				item->set_cell_mode(COL_BUNDLE, TreeItem::CELL_MODE_CHECK);
				item->set_text(COL_BUNDLE, entry.path.get_file());
				item->set_tooltip_text(COL_BUNDLE, entry.path);
				Ref<Texture2D> row_icon = folder_icon;
				if (!entry.is_bundle && EditorFileSystem::get_singleton()) {
					row_icon = EditorNode::get_singleton()->get_class_icon(EditorFileSystem::get_singleton()->get_file_type(entry.path), "Object");
				}
				item->set_icon(COL_BUNDLE, row_icon);
				item->set_metadata(COL_BUNDLE, entry_index);

				// Non-deletable rows keep their checkbox and render it greyed out: the control
				// is the explanation, and silently omitting it would read as a missing feature.
				const bool deletable = _state_deletable(entry.state);
				item->set_editable(COL_BUNDLE, deletable);
				if (!deletable) {
					item->set_tooltip_text(COL_BUNDLE, entry.path + "\n\n" + state_tooltips[entry.state]);
				}

				item->set_text(COL_SLOT, _short_slot(entry.slot));
				item->set_tooltip_text(COL_SLOT, entry.slot);
				item->set_text(COL_SIZE, String::humanize_size(entry.size));
				item->set_text_alignment(COL_SIZE, HORIZONTAL_ALIGNMENT_RIGHT);
				item->set_text(COL_STATE, state_texts[entry.state]);
				item->set_icon(COL_STATE, state_icons[entry.state]);
				item->set_tooltip_text(COL_STATE, state_tooltips[entry.state]);

				item->add_button(COL_STATE, dependents_icon, BUTTON_SHOW_DEPENDENTS, false, dependents_tooltip);
				if (!entry.owner_scene.is_empty()) {
					item->add_button(COL_STATE, open_scene_icon, BUTTON_OPEN_OWNER_SCENE, false, open_scene_tooltip);
				}
				item->add_button(COL_STATE, filesystem_icon, BUTTON_SHOW_IN_FILESYSTEM, false, filesystem_tooltip);
			}
		}
	}
}

// Interaction //////////////////////////////////////////////////////////////

void DerivedDataDialog::_filter_changed(const String &p_text) {
	_update_tree();
}

void DerivedDataDialog::_problems_toggled(bool p_pressed) {
	_update_tree();
}

void DerivedDataDialog::_column_title_clicked(int p_column, int p_mouse_button) {
	if (p_column != COL_SIZE || p_mouse_button != (int)MouseButton::LEFT) {
		return;
	}
	sort_descending = !sort_descending;
	_sort_groups();
	_update_tree();
}

void DerivedDataDialog::_save_all_pressed() {
	EditorNode::get_singleton()->save_all_scenes();
	_rescan();
}

void DerivedDataDialog::_button_pressed(Object *p_item, int p_column, int p_id, MouseButton p_mouse_button) {
	if (p_mouse_button != MouseButton::LEFT) {
		return;
	}
	TreeItem *item = Object::cast_to<TreeItem>(p_item);
	ERR_FAIL_NULL(item);
	const Variant meta = item->get_metadata(COL_BUNDLE);
	if (meta.get_type() != Variant::INT) {
		return;
	}
	const int index = meta;
	ERR_FAIL_INDEX(index, (int)entries.size());
	const Entry &entry = entries[index];

	switch (p_id) {
		case BUTTON_SHOW_DEPENDENTS: {
			// Deliberately not DependencyEditorOwners: that dialog matches raw dependency
			// strings and so cannot see UID-form references, which is exactly how producers
			// write these artifacts. The scan already built the correct answer.
			dependents_list->clear();
			HashSet<String> unique;
			for (const String &referrer : entry.referrers) {
				unique.insert(referrer);
			}
			for (const String &referrer : unique) {
				const String type = EditorFileSystem::get_singleton() ? EditorFileSystem::get_singleton()->get_file_type(referrer) : String();
				dependents_list->add_item(referrer, EditorNode::get_singleton()->get_class_icon(type, "Object"));
			}
			if (unique.is_empty()) {
				dependents_list->add_item(TTR("Nothing in the project references this bundle."));
				dependents_list->set_item_selectable(0, false);
			}
			dependents_dialog->set_title(vformat(TTR("Dependents of %s"), entry.path));
			dependents_dialog->popup_centered_clamped(Size2(600, 400) * EDSCALE, 0.6);
		} break;
		case BUTTON_OPEN_OWNER_SCENE: {
			if (entry.owner_scene.is_empty()) {
				return;
			}
			hide();
			EditorNode::get_singleton()->load_scene(entry.owner_scene);
		} break;
		case BUTTON_SHOW_IN_FILESYSTEM: {
			if (FileSystemDock::get_singleton()) {
				FileSystemDock::get_singleton()->navigate_to_path(entry.is_bundle ? entry.path + "/" : entry.path);
			}
		} break;
	}
}

void DerivedDataDialog::_collect_checked(TreeItem *p_item) {
	while (p_item) {
		if (p_item->get_cell_mode(COL_BUNDLE) == TreeItem::CELL_MODE_CHECK && p_item->is_checked(COL_BUNDLE) && p_item->is_editable(COL_BUNDLE)) {
			const Variant meta = p_item->get_metadata(COL_BUNDLE);
			if (meta.get_type() == Variant::INT) {
				const int index = meta;
				// Belt and braces: a disabled row can never reach here, but the delete path
				// re-checks the classification rather than trusting the widget state.
				if (index >= 0 && index < (int)entries.size() && _state_deletable(entries[index].state)) {
					pending_delete.push_back(index);
				}
			}
		}
		if (p_item->get_first_child()) {
			_collect_checked(p_item->get_first_child());
		}
		p_item = p_item->get_next();
	}
}

void DerivedDataDialog::ok_pressed() {
	pending_delete.clear();
	if (_unsaved_scene_count() > 0) {
		return;
	}
	_collect_checked(tree->get_root());
	if (pending_delete.is_empty()) {
		return;
	}

	uint64_t reclaimed = 0;
	String list;
	for (uint32_t i = 0; i < pending_delete.size(); i++) {
		const Entry &entry = entries[pending_delete[i]];
		reclaimed += entry.size;
		if (i < 20) {
			list += "\n" + entry.path;
		} else if (i == 20) {
			list += "\n" + vformat(TTR("...and %d more."), (int)pending_delete.size() - 20);
		}
	}

	delete_confirm->set_text(vformat(TTR("Permanently delete %d bundle(s), reclaiming %s? (No undo!)"),
										 (int)pending_delete.size(), String::humanize_size(reclaimed)) +
			"\n" + list);
	delete_confirm->popup_centered();
}

void DerivedDataDialog::_delete_confirmed() {
	for (const int index : pending_delete) {
		if (index < 0 || index >= (int)entries.size()) {
			continue;
		}
		const Entry &entry = entries[index];
		const String absolute = ProjectSettings::get_singleton()->globalize_path(entry.path);
		print_verbose("Moving to trash: " + absolute);
		if (OS::get_singleton()->move_to_trash(absolute) != OK) {
			EditorNode::get_singleton()->add_io_error(TTR("Cannot remove:") + "\n" + entry.path + "\n");
		}
	}
	pending_delete.clear();

	if (EditorFileSystem::get_singleton()) {
		EditorFileSystem::get_singleton()->scan_changes();
	}
	_rescan();
}

void DerivedDataDialog::show() {
	_rescan();
	popup_centered_clamped(Size2(900, 620) * EDSCALE, 0.8);
}

void DerivedDataDialog::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			filter->set_right_icon(get_editor_theme_icon(SNAME("Search")));
			unsaved_label->add_theme_color_override(SceneStringName(font_color), get_theme_color(SNAME("warning_color"), EditorStringName(Editor)));
		} break;
	}
}

DerivedDataDialog::DerivedDataDialog() {
	set_title(TTR("Derived Data"));
	set_ok_button_text(TTR("Delete Selected"));
	set_hide_on_ok(false);

	delete_confirm = memnew(ConfirmationDialog);
	delete_confirm->set_ok_button_text(TTR("Delete"));
	add_child(delete_confirm);
	delete_confirm->connect(SceneStringName(confirmed), callable_mp(this, &DerivedDataDialog::_delete_confirmed));

	dependents_dialog = memnew(AcceptDialog);
	add_child(dependents_dialog);
	dependents_list = memnew(ItemList);
	dependents_list->set_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
	dependents_dialog->add_child(dependents_list);

	VBoxContainer *vbc = memnew(VBoxContainer);
	add_child(vbc);

	unsaved_box = memnew(HBoxContainer);
	vbc->add_child(unsaved_box);
	unsaved_label = memnew(Label);
	unsaved_label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	unsaved_box->add_child(unsaved_label);
	Button *save_all_button = memnew(Button);
	save_all_button->set_text(TTR("Save All"));
	save_all_button->connect(SceneStringName(pressed), callable_mp(this, &DerivedDataDialog::_save_all_pressed));
	unsaved_box->add_child(save_all_button);
	unsaved_box->hide();

	filter_box = memnew(HBoxContainer);
	vbc->add_child(filter_box);
	filter = memnew(LineEdit);
	filter->set_placeholder(TTR("Filter"));
	filter->set_clear_button_enabled(true);
	filter->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	filter->connect(SceneStringName(text_changed), callable_mp(this, &DerivedDataDialog::_filter_changed));
	filter_box->add_child(filter);
	problems_only = memnew(CheckBox);
	problems_only->set_text(TTR("Problems Only"));
	problems_only->connect(SceneStringName(toggled), callable_mp(this, &DerivedDataDialog::_problems_toggled));
	filter_box->add_child(problems_only);

	tree = memnew(Tree);
	tree->set_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
	tree->set_theme_type_variation("TreeTable");
	tree->set_columns(COL_MAX);
	tree->set_column_titles_visible(true);
	tree->set_column_title(COL_BUNDLE, TTR("Bundle"));
	tree->set_column_title(COL_SLOT, TTR("Slot"));
	tree->set_column_title(COL_SIZE, TTR("Size"));
	tree->set_column_title(COL_STATE, TTR("State"));
	tree->set_column_expand(COL_BUNDLE, true);
	tree->set_column_clip_content(COL_BUNDLE, true);
	tree->set_column_expand(COL_SLOT, false);
	tree->set_column_clip_content(COL_SLOT, true);
	tree->set_column_custom_minimum_width(COL_SLOT, 130 * EDSCALE);
	tree->set_column_expand(COL_SIZE, false);
	tree->set_column_clip_content(COL_SIZE, true);
	tree->set_column_custom_minimum_width(COL_SIZE, 90 * EDSCALE);
	tree->set_column_title_alignment(COL_SIZE, HORIZONTAL_ALIGNMENT_RIGHT);
	tree->set_column_expand(COL_STATE, false);
	tree->set_column_clip_content(COL_STATE, true);
	tree->set_column_custom_minimum_width(COL_STATE, 220 * EDSCALE);
	tree->set_hide_root(true);
	tree->set_scroll_hint_mode(Tree::SCROLL_HINT_MODE_BOTTOM);
	tree->connect("button_clicked", callable_mp(this, &DerivedDataDialog::_button_pressed));
	tree->connect("column_title_clicked", callable_mp(this, &DerivedDataDialog::_column_title_clicked));

	tree_mc = vbc->add_margin_child(TTR("Generated Bundles:"), tree, true);
	tree_mc->set_theme_type_variation("NoBorderHorizontalWindow");
	tree_mc->set_v_size_flags(Control::SIZE_EXPAND_FILL);

	empty_label = memnew(Label);
	empty_label->set_text(TTR("No derived data. Bakes will appear here once produced."));
	empty_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	empty_label->set_vertical_alignment(VERTICAL_ALIGNMENT_CENTER);
	empty_label->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	empty_label->hide();
	vbc->add_child(empty_label);

	summary = memnew(Label);
	summary->set_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
	vbc->add_child(summary);
}
