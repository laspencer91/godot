/**************************************************************************/
/*  editor_asset_description.cpp                                          */
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

#include "editor_asset_description.h"

#include "core/io/config_file.h"
#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/variant/variant_parser.h"
#include "editor/editor_data.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "scene/main/node.h"
#include "scene/resources/packed_scene.h"

namespace {

struct DescriptionSection {
	int body_start = -1;
	int section_end = -1;
	int property_start = -1;
	int property_content_end = -1;
	int property_end = -1;
	String property_value;
};

static bool _is_target_section(const String &p_line, EditorAssetDescription::TextAssetKind p_kind) {
	if (p_kind == EditorAssetDescription::TEXT_ASSET_RESOURCE) {
		return p_line == "[resource]";
	}
	return p_line == "[node]" || p_line.begins_with("[node ");
}

static bool _find_description_section(const String &p_text, EditorAssetDescription::TextAssetKind p_kind, DescriptionSection &r_section) {
	bool in_target_section = false;
	int line_start = 0;
	while (line_start <= p_text.length()) {
		const int newline_position = p_text.find_char('\n', line_start);
		const int next_line_start = newline_position == -1 ? p_text.length() : newline_position + 1;
		int content_end = newline_position == -1 ? p_text.length() : newline_position;
		if (content_end > line_start && p_text[content_end - 1] == '\r') {
			content_end--;
		}
		const String line = p_text.substr(line_start, content_end - line_start);

		if (!in_target_section && _is_target_section(line, p_kind)) {
			in_target_section = true;
			r_section.body_start = next_line_start;
			r_section.section_end = p_text.length();
		} else if (in_target_section && line.begins_with("[")) {
			r_section.section_end = line_start;
			break;
		} else if (in_target_section) {
			const int equals_position = line.find_char('=');
			if (equals_position >= 0 && line.left(equals_position).strip_edges() == "editor_description") {
				if (r_section.property_start >= 0) {
					return false;
				}
				r_section.property_start = line_start;
				r_section.property_content_end = content_end;
				r_section.property_end = next_line_start;
				r_section.property_value = line.substr(equals_position + 1).strip_edges();
			}
		}

		if (newline_position == -1) {
			break;
		}
		line_start = next_line_start;
	}

	return in_target_section;
}

// The pure-extension half of EditorAssetDescription::get_asset_kind(): the single table mapping a
// file extension to an asset kind, with no filesystem access at all. Callers that must stay
// stat-free (the scan-thread file probe) use this directly; get_asset_kind() adds the .import
// sidecar fallback on top.
static EditorAssetDescription::AssetKind _kind_from_extension(const String &p_extension) {
	if (p_extension == "tscn") {
		return EditorAssetDescription::ASSET_KIND_SCENE_TEXT;
	}
	if (p_extension == "scn") {
		return EditorAssetDescription::ASSET_KIND_SCENE_BINARY;
	}
	if (p_extension == "tres") {
		return EditorAssetDescription::ASSET_KIND_RESOURCE_TEXT;
	}
	if (p_extension == "res") {
		return EditorAssetDescription::ASSET_KIND_RESOURCE_BINARY;
	}
	return EditorAssetDescription::ASSET_KIND_UNSUPPORTED;
}

static Node *_get_open_scene_root(const String &p_path) {
	if (!EditorNode::get_singleton()) {
		return nullptr;
	}
	EditorData &editor_data = EditorNode::get_editor_data();
	const int scene_index = editor_data.get_edited_scene_from_path(p_path);
	return scene_index >= 0 ? editor_data.get_edited_scene_root(scene_index) : nullptr;
}

static Error _read_scene_state_description(const String &p_path, String &r_description) {
	if (Node *root = _get_open_scene_root(p_path)) {
		r_description = root->get_editor_description();
		return OK;
	}

	Error load_error = OK;
	Ref<PackedScene> scene = ResourceLoader::load(p_path, "PackedScene", ResourceLoader::CACHE_MODE_IGNORE, &load_error);
	if (load_error != OK || scene.is_null()) {
		return load_error != OK ? load_error : ERR_CANT_OPEN;
	}

	const Ref<SceneState> state = scene->get_state();
	if (state.is_null() || state->get_node_count() == 0) {
		return ERR_FILE_CORRUPT;
	}
	for (int property_index = 0; property_index < state->get_node_property_count(0); property_index++) {
		if (state->get_node_property_name(0, property_index) == SNAME("editor_description")) {
			const Variant value = state->get_node_property_value(0, property_index);
			if (value.get_type() != Variant::STRING) {
				return ERR_FILE_CORRUPT;
			}
			r_description = value;
			return OK;
		}
	}

	r_description.clear();
	return OK;
}

} // namespace

EditorAssetDescription::AssetKind EditorAssetDescription::get_asset_kind(const String &p_path) {
	const AssetKind kind = _kind_from_extension(p_path.get_extension().to_lower());
	if (kind != ASSET_KIND_UNSUPPORTED) {
		return kind;
	}
	if (FileAccess::exists(p_path + ".import")) {
		return ASSET_KIND_IMPORTED;
	}
	return ASSET_KIND_UNSUPPORTED;
}

String EditorAssetDescription::get_asset_kind_name(AssetKind p_kind) {
	switch (p_kind) {
		case ASSET_KIND_SCENE_TEXT:
			return "Text scene (.tscn)";
		case ASSET_KIND_SCENE_BINARY:
			return "Binary scene (.scn)";
		case ASSET_KIND_RESOURCE_TEXT:
			return "Text resource (.tres)";
		case ASSET_KIND_RESOURCE_BINARY:
			return "Binary resource (.res)";
		case ASSET_KIND_IMPORTED:
			return "Imported source (.import)";
		case ASSET_KIND_UNSUPPORTED:
			return "Unsupported asset";
	}
	return "Unsupported asset";
}

bool EditorAssetDescription::is_supported(const String &p_path) {
	const AssetKind kind = get_asset_kind(p_path);
	return kind != ASSET_KIND_UNSUPPORTED && kind != ASSET_KIND_RESOURCE_BINARY;
}

Error EditorAssetDescription::_read_utf8_file(const String &p_path, uint64_t p_max_bytes, String &r_text) {
	Error open_error = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ, &open_error);
	if (file.is_null()) {
		return open_error;
	}

	const uint64_t byte_count = MIN(file->get_length(), p_max_bytes);
	const PackedByteArray bytes = file->get_buffer(byte_count);
	r_text.clear();
	if (bytes.is_empty()) {
		return OK;
	}
	return r_text.append_utf8((const char *)bytes.ptr(), bytes.size());
}

Error EditorAssetDescription::read_text_description(const String &p_text, TextAssetKind p_kind, String &r_description) {
	r_description.clear();
	DescriptionSection section;
	if (!_find_description_section(p_text, p_kind, section)) {
		return ERR_FILE_CORRUPT;
	}
	if (section.property_start < 0) {
		return OK;
	}

	VariantParser::StreamString stream;
	stream.s = section.property_value;
	Variant parsed_value;
	String parse_error;
	int error_line = 0;
	const Error error = VariantParser::parse(&stream, parsed_value, parse_error, error_line);
	if (error != OK || parsed_value.get_type() != Variant::STRING) {
		return ERR_PARSE_ERROR;
	}
	r_description = parsed_value;
	return OK;
}

Error EditorAssetDescription::edit_text_description(const String &p_text, TextAssetKind p_kind, const String &p_description, String &r_edited_text) {
	r_edited_text = p_text;
	DescriptionSection section;
	if (!_find_description_section(p_text, p_kind, section)) {
		return ERR_FILE_CORRUPT;
	}

	if (section.property_start >= 0 && p_description.is_empty()) {
		r_edited_text = p_text.erase(section.property_start, section.property_end - section.property_start);
		return OK;
	}
	if (section.property_start < 0 && p_description.is_empty()) {
		return OK;
	}

	const String encoded_description = "\"" + p_description.c_escape() + "\"";
	const String property_line = "editor_description = " + encoded_description;
	if (section.property_start >= 0) {
		r_edited_text = p_text.left(section.property_start) + property_line + p_text.substr(section.property_content_end);
		return OK;
	}

	const String newline = p_text.contains("\r\n") ? "\r\n" : "\n";
	if (section.body_start < p_text.length()) {
		r_edited_text = p_text.insert(section.body_start, property_line + newline);
	} else if (p_text.ends_with("\n")) {
		r_edited_text = p_text + property_line + newline;
	} else {
		r_edited_text = p_text + newline + property_line + newline;
	}
	return OK;
}

Error EditorAssetDescription::_write_text_asset(const String &p_path, TextAssetKind p_kind, const String &p_description, String &r_error) {
	String source_text;
	Error error = _read_utf8_file(p_path, UINT64_MAX, source_text);
	if (error != OK) {
		r_error = "The asset could not be read as valid UTF-8 text.";
		return error;
	}

	String edited_text;
	error = edit_text_description(source_text, p_kind, p_description, edited_text);
	if (error != OK) {
		r_error = p_kind == TEXT_ASSET_SCENE ? "The scene root node block could not be found or parsed." : "The resource block could not be found or parsed.";
		return error;
	}
	if (edited_text == source_text) {
		return OK;
	}

	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE, &error);
	if (file.is_null()) {
		r_error = "The asset could not be opened for writing.";
		return error;
	}
	if (!file->store_string(edited_text)) {
		r_error = "The asset description could not be written.";
		return ERR_FILE_CANT_WRITE;
	}
	return OK;
}

Error EditorAssetDescription::read_description(const String &p_path, String &r_description, String &r_error) {
	r_description.clear();
	r_error.clear();
	const AssetKind kind = get_asset_kind(p_path);
	switch (kind) {
		case ASSET_KIND_SCENE_TEXT:
		case ASSET_KIND_SCENE_BINARY: {
			const Error error = _read_scene_state_description(p_path, r_description);
			if (error != OK) {
				r_error = "The scene description could not be read from its packed scene state.";
			}
			return error;
		}
		case ASSET_KIND_RESOURCE_TEXT: {
			String source_text;
			Error error = _read_utf8_file(p_path, MAX_TEXT_SCAN_BYTES, source_text);
			if (error == OK) {
				error = read_text_description(source_text, TEXT_ASSET_RESOURCE, r_description);
			}
			if (error != OK) {
				r_error = "The resource description could not be read from its [resource] block.";
			}
			return error;
		}
		case ASSET_KIND_RESOURCE_BINARY: {
			r_error = "Binary .res resource descriptions are not supported in Explore V1.";
			return ERR_UNAVAILABLE;
		}
		case ASSET_KIND_IMPORTED: {
			Ref<ConfigFile> config;
			config.instantiate();
			const Error error = config->load(p_path + ".import");
			if (error != OK) {
				r_error = "The import sidecar could not be read.";
				return error;
			}
			if (config->has_section_key("remap", "description")) {
				r_description = config->get_value("remap", "description");
			}
			return OK;
		}
		case ASSET_KIND_UNSUPPORTED:
			r_error = "This asset type does not support descriptions in Explore V1.";
			return ERR_UNAVAILABLE;
	}
	return ERR_UNAVAILABLE;
}

Error EditorAssetDescription::write_description(const String &p_path, const String &p_description, String &r_error) {
	r_error.clear();
	const AssetKind kind = get_asset_kind(p_path);
	switch (kind) {
		case ASSET_KIND_SCENE_TEXT:
		case ASSET_KIND_SCENE_BINARY: {
			if (Node *root = _get_open_scene_root(p_path)) {
				EditorUndoRedoManager *undo_redo = EditorUndoRedoManager::get_singleton();
				undo_redo->create_action("Edit Asset Description");
				undo_redo->add_do_property(root, "editor_description", p_description);
				undo_redo->add_undo_property(root, "editor_description", root->get_editor_description());
				undo_redo->commit_action();
				return OK;
			}
			if (kind == ASSET_KIND_SCENE_BINARY) {
				r_error = "Descriptions for unopened binary .scn scenes cannot be written in Explore V1. Open the scene first, then edit its root description.";
				return ERR_UNAVAILABLE;
			}
			return _write_text_asset(p_path, TEXT_ASSET_SCENE, p_description, r_error);
		}
		case ASSET_KIND_RESOURCE_TEXT: {
			if (ResourceCache::has(p_path)) {
				Ref<Resource> resource = ResourceCache::get_ref(p_path);
				if (resource.is_valid() && resource->get_path() == p_path) {
					const String previous_description = resource->get_editor_description();
					resource->set_editor_description(p_description);
					const Error error = ResourceSaver::save(resource, p_path);
					if (error != OK) {
						resource->set_editor_description(previous_description);
						r_error = "The loaded resource could not be saved.";
					}
					return error;
				}
			}
			return _write_text_asset(p_path, TEXT_ASSET_RESOURCE, p_description, r_error);
		}
		case ASSET_KIND_RESOURCE_BINARY:
			r_error = "Binary .res resource descriptions cannot be written in Explore V1.";
			return ERR_UNAVAILABLE;
		case ASSET_KIND_IMPORTED: {
			Ref<ConfigFile> config;
			config.instantiate();
			Error error = config->load(p_path + ".import");
			if (error != OK) {
				r_error = "The import sidecar could not be read.";
				return error;
			}
			if (p_description.is_empty()) {
				config->erase_section_key("remap", "description");
			} else {
				config->set_value("remap", "description", p_description);
			}
			error = config->save(p_path + ".import");
			if (error != OK) {
				r_error = "The import sidecar description could not be saved.";
			}
			return error;
		}
		case ASSET_KIND_UNSUPPORTED:
			r_error = "This asset type does not support descriptions in Explore V1.";
			return ERR_UNAVAILABLE;
	}
	return ERR_UNAVAILABLE;
}

bool EditorAssetDescription::file_has_description(const String &p_path) {
	return file_has_description(p_path, get_asset_kind(p_path) == ASSET_KIND_IMPORTED);
}

bool EditorAssetDescription::file_has_description(const String &p_path, bool p_is_imported) {
	if (p_is_imported) {
		Ref<ConfigFile> config;
		config.instantiate();
		return config->load(p_path + ".import") == OK && config->has_section_key("remap", "description") && !String(config->get_value("remap", "description")).is_empty();
	}

	// Same gating as get_asset_kind(), minus the .import lookup the caller already answered — hence
	// the extension-only table rather than get_asset_kind(), which would stat the sidecar.
	// Binary .scn/.res and every unsupported type have no file-only answer here: .res is
	// unsupported entirely, and .scn descriptions live in a packed scene state that only the
	// main-thread probe may load.
	TextAssetKind text_kind;
	switch (_kind_from_extension(p_path.get_extension().to_lower())) {
		case ASSET_KIND_SCENE_TEXT:
			text_kind = TEXT_ASSET_SCENE;
			break;
		case ASSET_KIND_RESOURCE_TEXT:
			text_kind = TEXT_ASSET_RESOURCE;
			break;
		default:
			return false;
	}

	String source_text;
	if (_read_utf8_file(p_path, MAX_TEXT_SCAN_BYTES, source_text) != OK) {
		return false;
	}
	String description;
	return read_text_description(source_text, text_kind, description) == OK && !description.is_empty();
}

bool EditorAssetDescription::get_open_scene_description_overlay(const String &p_path, bool &r_has_description) {
	// The overlay only ever applies to a currently open text scene: an unsaved editor_description on
	// the open scene's root can differ from what's on disk, and thus from any file-derived or
	// harvested fact. O(1), no file I/O. A binary .scn that's open still gets a correct answer, but
	// through has_description_bounded()'s full read (_read_scene_state_description() consults the
	// same open root), not through this overlay.
	if (_kind_from_extension(p_path.get_extension().to_lower()) != ASSET_KIND_SCENE_TEXT) {
		return false;
	}
	const Node *root = _get_open_scene_root(p_path);
	if (!root) {
		return false;
	}
	r_has_description = !root->get_editor_description().is_empty();
	return true;
}

bool EditorAssetDescription::has_description_bounded(const String &p_path) {
	const AssetKind kind = get_asset_kind(p_path);
	if (!is_supported(p_path)) {
		return false;
	}
	if (kind == ASSET_KIND_SCENE_BINARY) {
		String description;
		String error;
		return read_description(p_path, description, error) == OK && !description.is_empty();
	}
	if (kind == ASSET_KIND_IMPORTED) {
		return file_has_description(p_path, true);
	}

	// ASSET_KIND_SCENE_BINARY already returned above; only an open text scene is handled without a file scan.
	bool open_scene_has_description = false;
	if (get_open_scene_description_overlay(p_path, open_scene_has_description)) {
		return open_scene_has_description;
	}

	return file_has_description(p_path, false);
}
