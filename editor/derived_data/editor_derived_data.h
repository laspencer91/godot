/**************************************************************************/
/*  editor_derived_data.h                                                 */
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

#include "core/object/object.h"

class Node;

// Editor-owned allocator for machine-generated artifacts (bakes). Producers never
// choose output paths: they ask this singleton for a location keyed by the owning
// node's persistent identity (scene UID + unique-scene-ID chain + slot name), so
// renames and moves re-resolve to the same bundle while true duplication forks.
//
// The slot table itself lives project-side (the script named by the
// "editor/derived_data/slot_registry" project setting, expected to expose SLOTS
// and ROOTS constants) — the engine holds the mechanism, the project holds the
// policy, and there is exactly one source of truth for both.
class EditorDerivedData : public Object {
	GDCLASS(EditorDerivedData, Object);

	static EditorDerivedData *singleton;

	// Cached from the project-side registry script for the editor session.
	Dictionary registry_slots; // slot name -> property row.
	Dictionary registry_roots; // storage class int -> "res://..." root.
	bool registry_loaded = false;

	bool _ensure_registry();
	Error _key_for(Node *p_owner, Dictionary &r_key) const;
	String _find_bundle(const String &p_root, const String &p_uid_body, const String &p_chain_hash, const String &p_slot) const;
	static String _find_subdir_with_suffix(const String &p_base, const String &p_suffix);
	static String _chain_hash(const PackedInt32Array &p_chain);
	Error _write_manifest(const String &p_bundle_dir, const StringName &p_slot, const Dictionary &p_slot_info, const Dictionary &p_key, bool p_fresh) const;

protected:
	static void _bind_methods();

public:
	static EditorDerivedData *get_singleton() { return singleton; }
	static void create();
	static void free();

	String bundle_for(Node *p_owner, const StringName &p_slot);
	String file_for(Node *p_owner, const StringName &p_slot, const String &p_ext);
	bool owns(Node *p_owner, const StringName &p_slot, const String &p_artifact_path);
	Dictionary describe(const String &p_artifact_path);

	// Read-only views of the project-side registry, for tools that have to walk the
	// derived roots themselves (the Derived Data dialog). Handing the cached tables out
	// keeps the registry-loading policy in one place instead of re-implemented per tool.
	PackedStringArray get_roots();
	Dictionary get_slots();

	EditorDerivedData();
	~EditorDerivedData() override;
};
