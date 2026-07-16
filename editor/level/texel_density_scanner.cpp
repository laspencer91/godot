/**************************************************************************/
/*  texel_density_scanner.cpp                                             */
/**************************************************************************/
/*  G-Level LE2: imported-texture dimension resolver for materials.       */
/**************************************************************************/

#include "texel_density_scanner.h"

#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "editor/settings/editor_settings.h"
#include "scene/resources/shader.h"
#include "scene/resources/texture.h"

PackedStringArray TexelDensityScanner::_get_parameter_names() {
	PackedStringArray defaults;
	defaults.push_back("albedo");
	defaults.push_back("albedo_texture");
	defaults.push_back("basecolor");
	defaults.push_back("base_color");
	defaults.push_back("base_color_texture");
	defaults.push_back("texture_albedo");
	defaults.push_back("diffuse");
	defaults.push_back("diffuse_texture");
	defaults.push_back("color_texture");

	EditorSettings *settings = EditorSettings::get_singleton();
	if (!settings || !settings->has_setting("level_editor/material_browser/texel_density_param_names")) {
		return defaults;
	}
	const Variant configured = settings->get("level_editor/material_browser/texel_density_param_names");
	if (configured.get_type() != Variant::PACKED_STRING_ARRAY) {
		return defaults;
	}
	const PackedStringArray names = configured;
	return names.is_empty() ? defaults : names;
}

String TexelDensityScanner::_make_parameter_names_fingerprint(const PackedStringArray &p_names) {
	String fingerprint;
	for (const String &name : p_names) {
		fingerprint += name.to_lower() + "\n";
	}
	return fingerprint;
}

std::optional<TexelDensityResult> TexelDensityScanner::_scan_uncached(const Ref<Material> &p_material, const PackedStringArray &p_names) {
	if (p_material.is_null()) {
		return std::nullopt;
	}

	Ref<Texture2D> texture;
	StringName matched_uniform;
	Ref<BaseMaterial3D> base_material = p_material;
	if (base_material.is_valid()) {
		texture = base_material->get_texture(BaseMaterial3D::TEXTURE_ALBEDO);
	} else {
		Ref<ShaderMaterial> shader_material = p_material;
		if (shader_material.is_valid() && shader_material->get_shader().is_valid()) {
			List<PropertyInfo> uniforms;
			shader_material->get_shader()->get_shader_uniform_list(&uniforms);
			for (const String &preferred_name : p_names) {
				const String preferred_lower = preferred_name.to_lower();
				for (const PropertyInfo &uniform : uniforms) {
					const bool texture_2d_sampler = uniform.type == Variant::OBJECT &&
							uniform.hint == PROPERTY_HINT_RESOURCE_TYPE && uniform.hint_string == Texture2D::get_class_static();
					if (!texture_2d_sampler || String(uniform.name).to_lower() != preferred_lower) {
						continue;
					}
					const Variant value = shader_material->get_shader_parameter(uniform.name);
					if (value.get_type() == Variant::OBJECT) {
						texture = value;
					}
					matched_uniform = uniform.name;
					break;
				}
				if (!matched_uniform.is_empty()) {
					break;
				}
			}
		}
	}

	if (texture.is_null()) {
		return std::nullopt;
	}
	const Size2i dimensions(texture->get_width(), texture->get_height());
	if (dimensions.x <= 0 || dimensions.y <= 0) {
		return std::nullopt;
	}

	TexelDensityResult result;
	result.texture = texture;
	result.texture_path = texture->get_path();
	result.dimensions = dimensions;
	result.shader_uniform = matched_uniform;
	return result;
}

Dictionary TexelDensityScanner::_result_to_dictionary(const std::optional<TexelDensityResult> &p_result) {
	Dictionary result;
	result["found"] = p_result.has_value();
	if (!p_result.has_value()) {
		result["width"] = 0;
		result["height"] = 0;
		result["dimensions"] = Size2i();
		result["uniform_name"] = StringName();
		result["texture_path"] = String();
		return result;
	}
	result["width"] = p_result->dimensions.x;
	result["height"] = p_result->dimensions.y;
	result["dimensions"] = p_result->dimensions;
	result["uniform_name"] = p_result->shader_uniform;
	result["texture"] = p_result->texture;
	result["texture_path"] = p_result->texture_path;
	return result;
}

void TexelDensityScanner::_bind_methods() {
	ClassDB::bind_method(D_METHOD("scan_material", "material", "source_path"), &TexelDensityScanner::scan_material, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("scan_path", "path"), &TexelDensityScanner::scan_path);
	ClassDB::bind_method(D_METHOD("invalidate", "path"), &TexelDensityScanner::invalidate);
	ClassDB::bind_method(D_METHOD("clear"), &TexelDensityScanner::clear);
}

std::optional<TexelDensityResult> TexelDensityScanner::scan(const Ref<Material> &p_material, const String &p_source_path) {
	const PackedStringArray names = _get_parameter_names();
	const String fingerprint = _make_parameter_names_fingerprint(names);
	MutexLock lock(cache_mutex);
	if (fingerprint != parameter_names_fingerprint) {
		cache.clear();
		parameter_names_fingerprint = fingerprint;
	}

	if (!p_source_path.is_empty()) {
		const CacheEntry *cached = cache.getptr(p_source_path);
		if (cached) {
			return cached->has_result ? std::optional<TexelDensityResult>(cached->result) : std::nullopt;
		}
	}

	const std::optional<TexelDensityResult> scanned = _scan_uncached(p_material, names);
	if (!p_source_path.is_empty()) {
		CacheEntry entry;
		entry.has_result = scanned.has_value();
		if (scanned.has_value()) {
			entry.result = *scanned;
		}
		cache.insert(p_source_path, entry);
	}
	return scanned;
}

Dictionary TexelDensityScanner::scan_material(const Ref<Material> &p_material, const String &p_source_path) {
	return _result_to_dictionary(scan(p_material, p_source_path));
}

Dictionary TexelDensityScanner::scan_path(const String &p_path) {
	Ref<Material> material = ResourceLoader::load(p_path);
	return _result_to_dictionary(scan(material, p_path));
}

void TexelDensityScanner::invalidate(const String &p_path) {
	MutexLock lock(cache_mutex);
	cache.erase(p_path);
}

void TexelDensityScanner::clear() {
	MutexLock lock(cache_mutex);
	cache.clear();
}
