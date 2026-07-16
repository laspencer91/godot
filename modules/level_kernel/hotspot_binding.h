/**************************************************************************/
/*  hotspot_binding.h                                                     */
/**************************************************************************/
/*  G-Level LE3: headless-readable pattern-key to atlas-path registry.    */
/**************************************************************************/

#pragma once

#include "core/io/resource.h"

class HotspotBinding : public Resource {
	GDCLASS(HotspotBinding, Resource);

	Dictionary bindings;

protected:
	static void _bind_methods();

public:
	void set_bindings(const Dictionary &p_bindings);
	Dictionary get_bindings() const { return bindings; }

	void set_binding(const String &p_pattern_key, const String &p_atlas_path);
	bool erase_binding(const String &p_pattern_key);
	void clear_bindings();
	String resolve_pattern(const String &p_pattern_key) const;
	String resolve_texture_path(const String &p_texture_path) const;
	PackedStringArray get_pattern_keys() const;

	static String pattern_key_from_texture_path(const String &p_texture_path);
};
