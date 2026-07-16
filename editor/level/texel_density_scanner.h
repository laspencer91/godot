/**************************************************************************/
/*  texel_density_scanner.h                                               */
/**************************************************************************/
/*  G-Level LE2: imported-texture dimension resolver for materials.       */
/**************************************************************************/

#pragma once

#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "scene/resources/material.h"

#include <optional>

struct TexelDensityResult {
	Ref<Texture2D> texture;
	String texture_path;
	Size2i dimensions;
	StringName shader_uniform;
};

class TexelDensityScanner : public RefCounted {
	GDCLASS(TexelDensityScanner, RefCounted);

	struct CacheEntry {
		bool has_result = false;
		TexelDensityResult result;
	};

	mutable Mutex cache_mutex;
	HashMap<String, CacheEntry> cache;
	String parameter_names_fingerprint;

	static PackedStringArray _get_parameter_names();
	static String _make_parameter_names_fingerprint(const PackedStringArray &p_names);
	static std::optional<TexelDensityResult> _scan_uncached(const Ref<Material> &p_material, const PackedStringArray &p_names);
	static Dictionary _result_to_dictionary(const std::optional<TexelDensityResult> &p_result);

protected:
	static void _bind_methods();

public:
	std::optional<TexelDensityResult> scan(const Ref<Material> &p_material, const String &p_source_path = String());
	Dictionary scan_material(const Ref<Material> &p_material, const String &p_source_path = String());
	Dictionary scan_path(const String &p_path);
	void invalidate(const String &p_path);
	void clear();
};
