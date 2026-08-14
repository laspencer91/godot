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
#include "core/templates/hash_set.h"
#include "core/variant/type_info.h"

class Node;

// Editor-owned allocator for machine-generated artifacts (bakes). Producers never
// choose output paths: they ask this singleton for a location keyed by the owning
// node's persistent identity (scene UID + unique-scene-ID chain + slot name), so
// renames and moves re-resolve to the same bundle while true duplication forks.
//
// Each producer registers its own slot. Retained artifacts default to
// res://__derived/ and regenerated artifacts to res://.godot/derived/, so built-in
// bakes work in every project without setup. A project-side registry script remains
// available as an optional compatibility/extension layer.
class EditorDerivedData : public Object {
	GDCLASS(EditorDerivedData, Object);

	static EditorDerivedData *singleton;

	// Producer registrations plus optional project-side extension metadata.
	Dictionary registry_slots; // slot name -> property row.
	Dictionary registry_roots; // storage class int -> "res://..." root.
	HashSet<StringName> producer_registered_slots;
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
	enum StorageClass {
		STORAGE_RETAINED,
		STORAGE_REGENERATED,
	};

	static EditorDerivedData *get_singleton() { return singleton; }
	static void create();
	static void free();

	Error register_slot(const StringName &p_slot, const StringName &p_producer, const PackedStringArray &p_extensions, int p_storage = STORAGE_RETAINED, int p_schema = 1);
	void unregister_slot(const StringName &p_slot);
	bool has_slot(const StringName &p_slot);

	String bundle_for(Node *p_owner, const StringName &p_slot);
	String file_for(Node *p_owner, const StringName &p_slot, const String &p_ext);
	bool owns(Node *p_owner, const StringName &p_slot, const String &p_artifact_path);
	Dictionary describe(const String &p_artifact_path);

	// Read-only views of all producer registrations and optional project extensions,
	// for tools that have to walk the derived roots themselves.
	PackedStringArray get_roots();
	Dictionary get_slots();

	EditorDerivedData();
	~EditorDerivedData() override;
};

VARIANT_ENUM_CAST(EditorDerivedData::StorageClass);
