/**************************************************************************/
/*  editor_asset_description.h                                            */
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

#include "core/error/error_list.h"
#include "core/string/ustring.h"

class EditorAssetDescription {
public:
	enum AssetKind {
		ASSET_KIND_UNSUPPORTED,
		ASSET_KIND_SCENE_TEXT,
		ASSET_KIND_SCENE_BINARY,
		ASSET_KIND_RESOURCE_TEXT,
		ASSET_KIND_RESOURCE_BINARY,
		ASSET_KIND_IMPORTED,
	};

	enum TextAssetKind {
		TEXT_ASSET_SCENE,
		TEXT_ASSET_RESOURCE,
	};

	static constexpr uint64_t MAX_TEXT_SCAN_BYTES = 1024 * 1024;

	static AssetKind get_asset_kind(const String &p_path);
	static String get_asset_kind_name(AssetKind p_kind);
	static bool is_supported(const String &p_path);

	static Error read_description(const String &p_path, String &r_description, String &r_error);
	static Error write_description(const String &p_path, const String &p_description, String &r_error);
	static bool has_description_bounded(const String &p_path);

	// Editor-state overlay for a currently open text scene, whose unsaved root description can differ
	// from what any file-derived fact (including the harvested Asset Fact Index one) reports. Returns
	// true and sets r_has_description only when p_path is an open .tscn; otherwise returns false and
	// leaves r_has_description untouched, so callers keep their file-derived answer. O(1), no file I/O.
	static bool get_open_scene_description_overlay(const String &p_path, bool &r_has_description);

	// File-only description probe, safe to call from the filesystem scan thread: it touches
	// nothing but FileAccess, so it never consults editor state (open scenes) and never loads
	// a resource. This is the half of has_description_bounded() that the Asset Fact Index
	// harvests into EditorFileSystemDirectory::FileInfo::has_description; the editor-state
	// overlay stays on the main thread. Pass p_is_imported when the caller already knows
	// whether the file has an .import sidecar, to skip the existence check.
	static bool file_has_description(const String &p_path);
	static bool file_has_description(const String &p_path, bool p_is_imported);

	// Pure helpers used by the surgical .tscn/.tres path and its doctests.
	static Error read_text_description(const String &p_text, TextAssetKind p_kind, String &r_description);
	static Error edit_text_description(const String &p_text, TextAssetKind p_kind, const String &p_description, String &r_edited_text);

private:
	static Error _read_utf8_file(const String &p_path, uint64_t p_max_bytes, String &r_text);
	static Error _write_text_asset(const String &p_path, TextAssetKind p_kind, const String &p_description, String &r_error);
};
