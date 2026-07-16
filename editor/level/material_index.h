/**************************************************************************/
/*  material_index.h                                                      */
/**************************************************************************/
/*  G-Level LE2: path-keyed, load-free project material index.            */
/**************************************************************************/

#pragma once

#include "core/object/ref_counted.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"

class EditorFileSystemDirectory;

struct MaterialIndexEntry {
	String path;
	String display_name;
	String folder;
	StringName class_name;
	bool is_convention_named = false;

	Dictionary to_dictionary() const;
};

class MaterialIndex : public RefCounted {
	GDCLASS(MaterialIndex, RefCounted);

	HashMap<String, MaterialIndexEntry> entries;
	HashSet<String> hidden_paths;
	bool initialized = false;

	static String _get_name_prefix();
	static bool _is_material_type(const StringName &p_type);
	static MaterialIndexEntry _make_entry(const String &p_path, const StringName &p_type);
	static bool _entry_equal(const MaterialIndexEntry &p_a, const MaterialIndexEntry &p_b);
	void _walk(EditorFileSystemDirectory *p_directory, HashMap<String, MaterialIndexEntry> &r_entries) const;
	void _filesystem_changed();
	void _sources_changed(bool p_exist);
	void _resources_changed(const PackedStringArray &p_paths);
	void _save_hidden_paths() const;

protected:
	static void _bind_methods();

public:
	void initialize();
	void rebuild();

	int get_count() const { return entries.size(); }
	const HashMap<String, MaterialIndexEntry> &get_entries() const { return entries; }
	const MaterialIndexEntry *get_entry(const String &p_path) const { return entries.getptr(p_path); }
	PackedStringArray get_paths() const;
	Array get_entries_array() const;
	Dictionary get_entry_dictionary(const String &p_path) const;

	bool is_hidden(const String &p_path) const { return hidden_paths.has(p_path); }
	void set_hidden(const String &p_path, bool p_hidden);
	void reload_hidden_paths();

	MaterialIndex() = default;
};
