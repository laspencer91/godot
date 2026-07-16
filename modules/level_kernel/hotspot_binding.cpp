/**************************************************************************/
/*  hotspot_binding.cpp                                                   */
/**************************************************************************/
/*  G-Level LE3: headless-readable pattern-key to atlas-path registry.    */
/**************************************************************************/

#include "hotspot_binding.h"

#include "core/object/class_db.h"

namespace {

String _normalize_pattern_key(const String &p_pattern_key) {
	String key = p_pattern_key.strip_edges().replace("\\", "/").simplify_path();
	while (key.begins_with("/")) {
		key = key.trim_prefix("/");
	}
	return key;
}

} // namespace

void HotspotBinding::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_bindings", "bindings"), &HotspotBinding::set_bindings);
	ClassDB::bind_method(D_METHOD("get_bindings"), &HotspotBinding::get_bindings);
	ClassDB::bind_method(D_METHOD("set_binding", "pattern_key", "atlas_path"), &HotspotBinding::set_binding);
	ClassDB::bind_method(D_METHOD("erase_binding", "pattern_key"), &HotspotBinding::erase_binding);
	ClassDB::bind_method(D_METHOD("clear_bindings"), &HotspotBinding::clear_bindings);
	ClassDB::bind_method(D_METHOD("resolve_pattern", "pattern_key"), &HotspotBinding::resolve_pattern);
	ClassDB::bind_method(D_METHOD("resolve_texture_path", "texture_path"), &HotspotBinding::resolve_texture_path);
	ClassDB::bind_method(D_METHOD("get_pattern_keys"), &HotspotBinding::get_pattern_keys);
	ClassDB::bind_static_method("HotspotBinding", D_METHOD("pattern_key_from_texture_path", "texture_path"), &HotspotBinding::pattern_key_from_texture_path);

	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "bindings"), "set_bindings", "get_bindings");
}

void HotspotBinding::set_bindings(const Dictionary &p_bindings) {
	Dictionary canonical;
	const Array keys = p_bindings.keys();
	for (int i = 0; i < keys.size(); i++) {
		const Variant key_variant = keys[i];
		const Variant value_variant = p_bindings[key_variant];
		if ((key_variant.get_type() != Variant::STRING && key_variant.get_type() != Variant::STRING_NAME) ||
				(value_variant.get_type() != Variant::STRING && value_variant.get_type() != Variant::STRING_NAME)) {
			continue;
		}
		const String key = _normalize_pattern_key(String(key_variant));
		const String atlas_path = String(value_variant).strip_edges();
		if (!key.is_empty() && !atlas_path.is_empty()) {
			canonical[key] = atlas_path;
		}
	}
	if (bindings == canonical) {
		return;
	}
	bindings = canonical;
	emit_changed();
}

void HotspotBinding::set_binding(const String &p_pattern_key, const String &p_atlas_path) {
	const String key = _normalize_pattern_key(p_pattern_key);
	const String atlas_path = p_atlas_path.strip_edges();
	ERR_FAIL_COND_MSG(key.is_empty(), "A hotspot pattern key cannot be empty.");
	ERR_FAIL_COND_MSG(atlas_path.is_empty(), "A hotspot atlas path cannot be empty.");
	if (bindings.get(key, String()) == atlas_path) {
		return;
	}
	bindings[key] = atlas_path;
	emit_changed();
}

bool HotspotBinding::erase_binding(const String &p_pattern_key) {
	const String key = _normalize_pattern_key(p_pattern_key);
	if (!bindings.erase(key)) {
		return false;
	}
	emit_changed();
	return true;
}

void HotspotBinding::clear_bindings() {
	if (bindings.is_empty()) {
		return;
	}
	bindings.clear();
	emit_changed();
}

String HotspotBinding::resolve_pattern(const String &p_pattern_key) const {
	return bindings.get(_normalize_pattern_key(p_pattern_key), String());
}

String HotspotBinding::resolve_texture_path(const String &p_texture_path) const {
	return resolve_pattern(pattern_key_from_texture_path(p_texture_path));
}

PackedStringArray HotspotBinding::get_pattern_keys() const {
	PackedStringArray result;
	const Array keys = bindings.keys();
	result.resize(keys.size());
	for (int i = 0; i < keys.size(); i++) {
		result.set(i, String(keys[i]));
	}
	result.sort();
	return result;
}

String HotspotBinding::pattern_key_from_texture_path(const String &p_texture_path) {
	String texture_path = p_texture_path.strip_edges().replace("\\", "/");
	if (texture_path.is_empty()) {
		return String();
	}
	const int subresource_separator = texture_path.find("::");
	if (subresource_separator >= 0) {
		texture_path = texture_path.left(subresource_separator);
	}
	if (texture_path.begins_with("res://")) {
		texture_path = texture_path.trim_prefix("res://");
	}
	texture_path = texture_path.simplify_path();
	const String directory = texture_path.get_base_dir();
	String stem = texture_path.get_file().get_basename();
	if (stem.is_empty()) {
		return String();
	}

	// Base-color channel suffixes are not part of the pattern identity. This
	// turns e.g. psx/brick_albedo.png into the planned psx/brick key while
	// retaining multi-word pattern stems such as brick_wall.
	static const char *channel_suffixes[] = {
		"_base_color_texture",
		"_basecolor_texture",
		"_albedo_texture",
		"_texture_albedo",
		"_diffuse_texture",
		"_color_texture",
		"_base_color",
		"_basecolor",
		"_albedo",
		"_diffuse",
		"_color",
	};
	const String lower_stem = stem.to_lower();
	for (const char *suffix : channel_suffixes) {
		const String suffix_string = suffix;
		if (lower_stem.ends_with(suffix_string) && stem.length() > suffix_string.length()) {
			stem = stem.left(stem.length() - suffix_string.length());
			break;
		}
	}
	return _normalize_pattern_key(directory.is_empty() ? stem : directory.path_join(stem));
}
