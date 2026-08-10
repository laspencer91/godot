/**************************************************************************/
/*  editor_derived_data.cpp                                               */
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

#include "editor_derived_data.h"

#include "core/config/project_settings.h"
#include "core/io/config_file.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_uid.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "core/os/time.h"
#include "core/templates/hashfuncs.h"
#include "scene/main/node.h"

static const char *MANIFEST_FILE_NAME = "manifest.cfg";
static const char *MANIFEST_SECTION = "bundle";

EditorDerivedData *EditorDerivedData::singleton = nullptr;

bool EditorDerivedData::_ensure_registry() {
	if (registry_loaded) {
		return true;
	}

	const String registry_path = GLOBAL_GET("editor/derived_data/slot_registry");
	ERR_FAIL_COND_V_MSG(registry_path.is_empty(), false, "EditorDerivedData: the \"editor/derived_data/slot_registry\" project setting is not set; point it at the project's slot registry script.");

	Ref<Script> registry = ResourceLoader::load(registry_path);
	ERR_FAIL_COND_V_MSG(registry.is_null(), false, vformat("EditorDerivedData: cannot load the slot registry script at \"%s\".", registry_path));

	HashMap<StringName, Variant> constants;
	registry->get_constants(&constants);
	ERR_FAIL_COND_V_MSG(!constants.has("SLOTS") || !constants.has("ROOTS"), false, vformat("EditorDerivedData: the slot registry at \"%s\" must expose SLOTS and ROOTS constants.", registry_path));

	registry_slots = constants["SLOTS"];
	registry_roots = constants["ROOTS"];
	registry_loaded = true;
	return true;
}

Error EditorDerivedData::_key_for(Node *p_owner, Dictionary &r_key) const {
	ERR_FAIL_NULL_V(p_owner, ERR_INVALID_PARAMETER);

	const PackedInt32Array id_chain = p_owner->get_unique_scene_id_path();
	ERR_FAIL_COND_V_MSG(id_chain.is_empty(), ERR_UNCONFIGURED, vformat("EditorDerivedData: node \"%s\" has no settled identity — save the scene once so unique scene IDs settle, then bake. (New and duplicated nodes have no ID until their first save.)", p_owner->get_name()));

	Node *root = p_owner;
	while (root->get_owner() != nullptr) {
		root = root->get_owner();
	}
	const String scene_path = root->get_scene_file_path();
	ERR_FAIL_COND_V_MSG(scene_path.is_empty(), ERR_UNCONFIGURED, vformat("EditorDerivedData: the scene owning node \"%s\" has never been saved — save it before baking.", p_owner->get_name()));

	const ResourceUID::ID scene_uid = ResourceLoader::get_resource_uid(scene_path);
	ERR_FAIL_COND_V_MSG(scene_uid == ResourceUID::INVALID_ID, ERR_UNCONFIGURED, vformat("EditorDerivedData: the scene \"%s\" has no resource UID — re-save it so one is assigned.", scene_path));

	r_key["scene_uid"] = ResourceUID::get_singleton()->id_to_text(scene_uid);
	r_key["id_chain"] = id_chain;
	// Labels: decoration for humans reading diffs and manifests, never used for matching.
	r_key["scene_path"] = scene_path;
	r_key["scene_stem"] = scene_path.get_file().get_basename();
	r_key["node_path"] = String(root->get_path_to(p_owner));
	r_key["node_name"] = String(p_owner->get_name());
	return OK;
}

String EditorDerivedData::_chain_hash(const PackedInt32Array &p_chain) {
	const uint32_t hash = hash_murmur3_buffer(p_chain.ptr(), p_chain.size() * sizeof(int32_t));
	return String::num_uint64(hash, 16).lpad(8, "0");
}

String EditorDerivedData::_find_subdir_with_suffix(const String &p_base, const String &p_suffix) {
	Ref<DirAccess> da = DirAccess::open(p_base);
	if (da.is_null()) {
		return String();
	}
	da->list_dir_begin();
	for (String name = da->get_next(); !name.is_empty(); name = da->get_next()) {
		if (da->current_is_dir() && name.ends_with(p_suffix)) {
			da->list_dir_end();
			return p_base.path_join(name);
		}
	}
	da->list_dir_end();
	return String();
}

String EditorDerivedData::_find_bundle(const String &p_root, const String &p_uid_body, const String &p_chain_hash, const String &p_slot) const {
	// Names are decoration, the "-<identity>" suffix is what is matched (A3): a renamed
	// scene or node keeps resolving to the directory allocated under its old decoration.
	const String scene_dir = _find_subdir_with_suffix(p_root, "-" + p_uid_body);
	if (scene_dir.is_empty()) {
		return String();
	}
	const String node_dir = _find_subdir_with_suffix(scene_dir, "-" + p_chain_hash);
	if (node_dir.is_empty()) {
		return String();
	}
	const String bundle_dir = node_dir.path_join(p_slot);
	return DirAccess::dir_exists_absolute(bundle_dir) ? bundle_dir : String();
}

Error EditorDerivedData::_write_manifest(const String &p_bundle_dir, const StringName &p_slot, const Dictionary &p_slot_info, const Dictionary &p_key, bool p_fresh) const {
	const String manifest_path = p_bundle_dir.path_join(MANIFEST_FILE_NAME);
	const String now = Time::get_singleton()->get_datetime_string_from_system(true);

	Ref<ConfigFile> manifest;
	manifest.instantiate();
	if (!p_fresh) {
		manifest->load(manifest_path); // Best-effort: an unreadable manifest is rewritten whole.
	}
	manifest->set_value(MANIFEST_SECTION, "slot", String(p_slot));
	manifest->set_value(MANIFEST_SECTION, "schema", p_slot_info.get("schema", 0));
	manifest->set_value(MANIFEST_SECTION, "producer", p_slot_info.get("producer", ""));
	manifest->set_value(MANIFEST_SECTION, "scene_uid", p_key["scene_uid"]);
	manifest->set_value(MANIFEST_SECTION, "id_chain", p_key["id_chain"]);
	manifest->set_value(MANIFEST_SECTION, "scene_path", p_key["scene_path"]);
	manifest->set_value(MANIFEST_SECTION, "node_path", p_key["node_path"]);
	if (p_fresh || !manifest->has_section_key(MANIFEST_SECTION, "created")) {
		manifest->set_value(MANIFEST_SECTION, "created", now);
	}
	manifest->set_value(MANIFEST_SECTION, "updated", now);
	return manifest->save(manifest_path);
}

String EditorDerivedData::bundle_for(Node *p_owner, const StringName &p_slot) {
	if (!_ensure_registry()) {
		return String();
	}
	ERR_FAIL_COND_V_MSG(!registry_slots.has(String(p_slot)), String(), vformat("EditorDerivedData: unknown slot \"%s\" — register it in the slot registry before allocating.", p_slot));
	const Dictionary slot_info = registry_slots[String(p_slot)];

	Dictionary key;
	if (_key_for(p_owner, key) != OK) {
		return String();
	}

	const int storage = slot_info.get("storage", 0);
	ERR_FAIL_COND_V_MSG(!registry_roots.has(storage), String(), vformat("EditorDerivedData: slot \"%s\" names storage class %d, which has no root in the registry.", p_slot, storage));
	const String root = registry_roots[storage];

	const String uid_body = String(key["scene_uid"]).trim_prefix("uid://");
	const String chain_hash = _chain_hash(key["id_chain"]);

	String bundle_dir = _find_bundle(root, uid_body, chain_hash, String(p_slot));
	bool fresh = bundle_dir.is_empty();
	if (fresh) {
		const String scene_dir_name = String(key["scene_stem"]).validate_filename() + "-" + uid_body;
		const String node_dir_name = String(key["node_name"]).validate_filename() + "-" + chain_hash;
		bundle_dir = root.path_join(scene_dir_name).path_join(node_dir_name).path_join(String(p_slot));
		const Error err = DirAccess::make_dir_recursive_absolute(bundle_dir);
		ERR_FAIL_COND_V_MSG(err != OK, String(), vformat("EditorDerivedData: cannot create bundle directory \"%s\" (error %d).", bundle_dir, err));
	} else {
		// A suffix match with a foreign manifest means either manifest corruption or an
		// identity-hash collision; silently sharing the bundle is the one unacceptable
		// outcome, so verify before handing the path out.
		const Dictionary manifest = describe(bundle_dir.path_join(MANIFEST_FILE_NAME));
		if (!manifest.is_empty()) {
			const bool matches = String(manifest.get("slot", "")) == String(p_slot) &&
					String(manifest.get("scene_uid", "")) == String(key["scene_uid"]) &&
					PackedInt32Array(manifest.get("id_chain", PackedInt32Array())) == PackedInt32Array(key["id_chain"]);
			ERR_FAIL_COND_V_MSG(!matches, String(), vformat("EditorDerivedData: bundle \"%s\" matches node \"%s\" by directory name but its manifest declares a different identity — refusing to share it. Inspect the manifest.", bundle_dir, key["node_path"]));
		}
	}

	const Error err = _write_manifest(bundle_dir, p_slot, slot_info, key, fresh);
	ERR_FAIL_COND_V_MSG(err != OK, String(), vformat("EditorDerivedData: cannot write the manifest in \"%s\" (error %d).", bundle_dir, err));
	return bundle_dir;
}

String EditorDerivedData::file_for(Node *p_owner, const StringName &p_slot, const String &p_ext) {
	ERR_FAIL_COND_V_MSG(p_ext.is_empty() || p_ext.contains("."), String(), vformat("EditorDerivedData: \"%s\" is not a bare file extension.", p_ext));
	const String bundle_dir = bundle_for(p_owner, p_slot);
	if (bundle_dir.is_empty()) {
		return String();
	}
	return bundle_dir.path_join("data." + p_ext);
}

bool EditorDerivedData::owns(Node *p_owner, const StringName &p_slot, const String &p_artifact_path) {
	const Dictionary manifest = describe(p_artifact_path);
	if (manifest.is_empty()) {
		return false;
	}
	Dictionary key;
	if (_key_for(p_owner, key) != OK) {
		return false;
	}
	return String(manifest.get("slot", "")) == String(p_slot) &&
			String(manifest.get("scene_uid", "")) == String(key["scene_uid"]) &&
			PackedInt32Array(manifest.get("id_chain", PackedInt32Array())) == PackedInt32Array(key["id_chain"]);
}

Dictionary EditorDerivedData::describe(const String &p_artifact_path) {
	Dictionary result;
	if (!_ensure_registry()) {
		return result;
	}

	// Only paths under a registered derived root can belong to a bundle.
	String root;
	for (const KeyValue<Variant, Variant> &kv : registry_roots) {
		const String candidate = kv.value;
		if (p_artifact_path.begins_with(candidate)) {
			root = candidate;
			break;
		}
	}
	if (root.is_empty()) {
		return result;
	}

	// Walk up from the artifact to the bundle directory (the one holding the manifest).
	String dir = p_artifact_path.get_file().is_empty() ? p_artifact_path : p_artifact_path.get_base_dir();
	while (dir.length() > root.length() && dir.begins_with(root)) {
		const String manifest_path = dir.path_join(MANIFEST_FILE_NAME);
		if (FileAccess::exists(manifest_path)) {
			Ref<ConfigFile> manifest;
			manifest.instantiate();
			if (manifest->load(manifest_path) != OK) {
				return result;
			}
			for (const String &manifest_key : manifest->get_section_keys(MANIFEST_SECTION)) {
				result[manifest_key] = manifest->get_value(MANIFEST_SECTION, manifest_key);
			}
			result["bundle_dir"] = dir;
			return result;
		}
		dir = dir.get_base_dir();
	}
	return result;
}

void EditorDerivedData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("file_for", "owner", "slot", "ext"), &EditorDerivedData::file_for);
	ClassDB::bind_method(D_METHOD("bundle_for", "owner", "slot"), &EditorDerivedData::bundle_for);
	ClassDB::bind_method(D_METHOD("owns", "owner", "slot", "artifact_path"), &EditorDerivedData::owns);
	ClassDB::bind_method(D_METHOD("describe", "artifact_path"), &EditorDerivedData::describe);
}

void EditorDerivedData::create() {
	memnew(EditorDerivedData);
}

void EditorDerivedData::free() {
	ERR_FAIL_NULL(singleton);
	memdelete(singleton);
}

EditorDerivedData::EditorDerivedData() {
	ERR_FAIL_COND(singleton != nullptr);
	singleton = this;
}

EditorDerivedData::~EditorDerivedData() {
	singleton = nullptr;
}
