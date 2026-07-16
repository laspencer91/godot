/**************************************************************************/
/*  material_index.cpp                                                    */
/**************************************************************************/
/*  G-Level LE2: path-keyed, load-free project material index.            */
/**************************************************************************/

#include "material_index.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/settings/editor_settings.h"
#include "scene/resources/material.h"

static constexpr const char *MATERIAL_BROWSER_METADATA_SECTION = "level_editor/material_browser";
static constexpr const char *MATERIAL_BROWSER_HIDDEN_KEY = "hidden_materials";

Dictionary MaterialIndexEntry::to_dictionary() const {
	Dictionary result;
	result["path"] = path;
	result["display_name"] = display_name;
	result["folder"] = folder;
	result["class_name"] = class_name;
	result["is_convention_named"] = is_convention_named;
	return result;
}

String MaterialIndex::_get_name_prefix() {
	EditorSettings *settings = EditorSettings::get_singleton();
	if (!settings || !settings->has_setting("level_editor/material_browser/name_filter_prefix")) {
		return "M_";
	}
	return settings->get("level_editor/material_browser/name_filter_prefix");
}

bool MaterialIndex::_is_material_type(const StringName &p_type) {
	return !p_type.is_empty() && ClassDB::is_parent_class(p_type, Material::get_class_static());
}

MaterialIndexEntry MaterialIndex::_make_entry(const String &p_path, const StringName &p_type) {
	MaterialIndexEntry entry;
	entry.path = p_path;
	entry.folder = p_path.get_base_dir();
	entry.class_name = p_type;
	const String prefix = _get_name_prefix();
	const String base_name = p_path.get_file().get_basename();
	entry.is_convention_named = !prefix.is_empty() && base_name.begins_with(prefix);
	entry.display_name = entry.is_convention_named ? base_name.trim_prefix(prefix) : base_name;
	return entry;
}

bool MaterialIndex::_entry_equal(const MaterialIndexEntry &p_a, const MaterialIndexEntry &p_b) {
	return p_a.path == p_b.path && p_a.display_name == p_b.display_name && p_a.folder == p_b.folder &&
			p_a.class_name == p_b.class_name && p_a.is_convention_named == p_b.is_convention_named;
}

void MaterialIndex::_walk(EditorFileSystemDirectory *p_directory, HashMap<String, MaterialIndexEntry> &r_entries) const {
	if (!p_directory) {
		return;
	}
	for (int file_index = 0; file_index < p_directory->get_file_count(); file_index++) {
		const StringName type = p_directory->get_file_type(file_index);
		if (!_is_material_type(type)) {
			continue;
		}
		const String path = p_directory->get_file_path(file_index);
		r_entries.insert(path, _make_entry(path, type));
	}
	for (int directory_index = 0; directory_index < p_directory->get_subdir_count(); directory_index++) {
		_walk(p_directory->get_subdir(directory_index), r_entries);
	}
}

void MaterialIndex::_filesystem_changed() {
	rebuild();
}

void MaterialIndex::_sources_changed(bool p_exist) {
	(void)p_exist;
	rebuild();
}

void MaterialIndex::_resources_changed(const PackedStringArray &p_paths) {
	EditorFileSystem *filesystem = EditorFileSystem::get_singleton();
	if (!filesystem) {
		return;
	}
	for (const String &path : p_paths) {
		MaterialIndexEntry *entry = entries.getptr(path);
		if (!entry) {
			continue;
		}
		const StringName type = filesystem->get_file_type(path);
		if (!_is_material_type(type)) {
			entries.erase(path);
			emit_signal(SNAME("material_removed"), path);
			continue;
		}
		*entry = _make_entry(path, type);
		emit_signal(SNAME("material_changed"), path);
	}
}

void MaterialIndex::_save_hidden_paths() const {
	EditorSettings *settings = EditorSettings::get_singleton();
	if (!settings) {
		return;
	}
	PackedStringArray paths;
	for (const String &path : hidden_paths) {
		paths.push_back(path);
	}
	paths.sort();
	settings->set_project_metadata(MATERIAL_BROWSER_METADATA_SECTION, MATERIAL_BROWSER_HIDDEN_KEY, paths);
}

void MaterialIndex::_bind_methods() {
	ClassDB::bind_method(D_METHOD("rebuild"), &MaterialIndex::rebuild);
	ClassDB::bind_method(D_METHOD("get_count"), &MaterialIndex::get_count);
	ClassDB::bind_method(D_METHOD("get_paths"), &MaterialIndex::get_paths);
	ClassDB::bind_method(D_METHOD("get_entries"), &MaterialIndex::get_entries_array);
	ClassDB::bind_method(D_METHOD("get_entry", "path"), &MaterialIndex::get_entry_dictionary);
	ClassDB::bind_method(D_METHOD("is_hidden", "path"), &MaterialIndex::is_hidden);
	ClassDB::bind_method(D_METHOD("set_hidden", "path", "hidden"), &MaterialIndex::set_hidden);
	ClassDB::bind_method(D_METHOD("reload_hidden_paths"), &MaterialIndex::reload_hidden_paths);

	ADD_SIGNAL(MethodInfo("material_added", PropertyInfo(Variant::STRING, "path")));
	ADD_SIGNAL(MethodInfo("material_removed", PropertyInfo(Variant::STRING, "path")));
	ADD_SIGNAL(MethodInfo("material_changed", PropertyInfo(Variant::STRING, "path")));
}

void MaterialIndex::initialize() {
	if (initialized) {
		return;
	}
	initialized = true;
	reload_hidden_paths();
	EditorFileSystem *filesystem = EditorFileSystem::get_singleton();
	if (!filesystem) {
		return;
	}
	filesystem->connect(SNAME("filesystem_changed"), callable_mp(this, &MaterialIndex::_filesystem_changed));
	filesystem->connect(SNAME("sources_changed"), callable_mp(this, &MaterialIndex::_sources_changed));
	filesystem->connect(SNAME("resources_reimported"), callable_mp(this, &MaterialIndex::_resources_changed));
	filesystem->connect(SNAME("resources_reload"), callable_mp(this, &MaterialIndex::_resources_changed));
	if (filesystem->get_filesystem()) {
		rebuild();
	}
}

void MaterialIndex::rebuild() {
	EditorFileSystem *filesystem = EditorFileSystem::get_singleton();
	if (!filesystem || !filesystem->get_filesystem()) {
		return;
	}

	HashMap<String, MaterialIndexEntry> snapshot;
	_walk(filesystem->get_filesystem(), snapshot);

	PackedStringArray removed;
	PackedStringArray added;
	PackedStringArray changed;
	for (const KeyValue<String, MaterialIndexEntry> &current : entries) {
		if (!snapshot.has(current.key)) {
			removed.push_back(current.key);
		}
	}
	for (const KeyValue<String, MaterialIndexEntry> &candidate : snapshot) {
		const MaterialIndexEntry *current = entries.getptr(candidate.key);
		if (!current) {
			added.push_back(candidate.key);
		} else if (!_entry_equal(*current, candidate.value)) {
			changed.push_back(candidate.key);
		}
	}
	removed.sort();
	added.sort();
	changed.sort();

	for (const String &path : removed) {
		entries.erase(path);
		emit_signal(SNAME("material_removed"), path);
	}
	for (const String &path : added) {
		entries.insert(path, snapshot[path]);
		emit_signal(SNAME("material_added"), path);
	}
	for (const String &path : changed) {
		entries[path] = snapshot[path];
		emit_signal(SNAME("material_changed"), path);
	}
}

PackedStringArray MaterialIndex::get_paths() const {
	PackedStringArray paths;
	for (const KeyValue<String, MaterialIndexEntry> &entry : entries) {
		paths.push_back(entry.key);
	}
	paths.sort();
	return paths;
}

Array MaterialIndex::get_entries_array() const {
	Array result;
	const PackedStringArray paths = get_paths();
	for (const String &path : paths) {
		result.push_back(entries[path].to_dictionary());
	}
	return result;
}

Dictionary MaterialIndex::get_entry_dictionary(const String &p_path) const {
	const MaterialIndexEntry *entry = entries.getptr(p_path);
	return entry ? entry->to_dictionary() : Dictionary();
}

void MaterialIndex::set_hidden(const String &p_path, bool p_hidden) {
	if (p_hidden == hidden_paths.has(p_path)) {
		return;
	}
	if (p_hidden) {
		hidden_paths.insert(p_path);
	} else {
		hidden_paths.erase(p_path);
	}
	_save_hidden_paths();
	if (entries.has(p_path)) {
		emit_signal(SNAME("material_changed"), p_path);
	}
}

void MaterialIndex::reload_hidden_paths() {
	hidden_paths.clear();
	EditorSettings *settings = EditorSettings::get_singleton();
	if (!settings) {
		return;
	}
	const Variant stored = settings->get_project_metadata(MATERIAL_BROWSER_METADATA_SECTION, MATERIAL_BROWSER_HIDDEN_KEY, PackedStringArray());
	if (stored.get_type() != Variant::PACKED_STRING_ARRAY) {
		return;
	}
	const PackedStringArray paths = stored;
	for (const String &path : paths) {
		hidden_paths.insert(path);
	}
}
