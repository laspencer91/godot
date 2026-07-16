extends SceneTree

const FIXTURE := "res://hotspot_fixture.rect"
const MALFORMED_FIXTURE := "res://hotspot_malformed.rect"
const OUTPUT_DIR := "res://.godot/wp19_hotspot_smoke"
const TEXTURE_PATH := OUTPUT_DIR + "/reference_texture.tres"
const ATLAS_PATH := OUTPUT_DIR + "/atlas_roundtrip.tres"
const EXPORTED_RECT_PATH := OUTPUT_DIR + "/exported.rect"
const BINDING_PATH := OUTPUT_DIR + "/bindings.tres"
const SHARED_TEXTURE_PATH := OUTPUT_DIR + "/brick_albedo.tres"

var failures: Array[String] = []


func _initialize() -> void:
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(OUTPUT_DIR))
	var defaults := HotspotAtlas.new()
	_check(defaults.texel_density_target == 256.0 and defaults.param_names == PackedStringArray([
		"albedo_texture", "BaseColor", "base_color_texture", "texture_albedo",
	]), "atlas_exact_defaults")
	var texture := _make_saved_texture(TEXTURE_PATH, 512, 256)
	if texture == null:
		_finish()
		return
	_check_resource_roundtrip(texture)
	_check_rect_roundtrip(texture)
	_check_binding_roundtrip()
	_check_resize_proofness(texture)
	_finish()


func _make_saved_texture(path: String, width: int, height: int) -> GradientTexture2D:
	var texture := GradientTexture2D.new()
	texture.gradient = Gradient.new()
	texture.width = width
	texture.height = height
	var save_error := ResourceSaver.save(texture, path)
	_check(save_error == OK, "save_texture_%s" % path.get_file())
	if save_error != OK:
		return null
	var loaded := ResourceLoader.load(path, "GradientTexture2D", ResourceLoader.CACHE_MODE_IGNORE) as GradientTexture2D
	_check(loaded != null and loaded.width == width and loaded.height == height,
			"load_texture_dimensions_%s" % path.get_file())
	return loaded


func _make_patch(rect: Rect2, patch_name: StringName, rotation: bool, mirror_x: bool,
		mirror_y: bool, tiling: bool, axis: int, inset: float, extra := {}) -> HotspotPatch:
	var patch := HotspotPatch.new()
	patch.rect_uv = rect
	patch.patch_name = patch_name
	patch.allow_rotation = rotation
	patch.allow_mirror_x = mirror_x
	patch.allow_mirror_y = mirror_y
	patch.allow_tiling = tiling
	patch.tiling_axis = axis
	patch.inset_px = inset
	patch.extra = extra
	return patch


func _make_atlas(texture: Texture2D) -> HotspotAtlas:
	var atlas := HotspotAtlas.new()
	atlas.atlas_id = &"psx_shared_architecture"
	atlas.reference_texture = texture
	atlas.texel_density_target = 384.5
	atlas.default_mapping_mode = HotspotAtlas.MAPPING_FOLLOW_ACTIVE_QUADS
	atlas.disallow_random = true
	atlas.tiling_policy = HotspotAtlas.TILING_ONLY
	atlas.param_names = PackedStringArray([
		"albedo_texture", "BaseColor", "base_color_texture", "texture_albedo", "custom_color_map",
	])
	var targets: Array[StringName] = [
		&"res://materials/M_Brick_A.tres",
		&"res://materials/M_Brick_Wet.tres",
	]
	atlas.target_materials = targets
	var authored_patches: Array[HotspotPatch] = [
		_make_patch(Rect2(0.0, 0.0, 0.25, 0.5), &"plain", false, false, false,
				false, HotspotPatch.TILING_AXIS_U, 0.0, { "source": "manual" }),
		_make_patch(Rect2(0.25, 0.0, 0.5, 0.25), &"rotating_trim", true, true, false,
				true, HotspotPatch.TILING_AXIS_U, 2.5, { "weight": "7" }),
		_make_patch(Rect2(0.75, 0.25, 0.25, 0.75), &"vertical_trim", true, false, true,
				true, HotspotPatch.TILING_AXIS_V, 4.0, { "nested": { "unknown": "kept" } }),
	]
	atlas.patches = authored_patches
	return atlas


func _check_resource_roundtrip(texture: Texture2D) -> void:
	var atlas := _make_atlas(texture)
	var before := _atlas_snapshot(atlas)
	_check(ResourceSaver.save(atlas, ATLAS_PATH) == OK, "atlas_resource_save")
	var serialized_text := FileAccess.get_file_as_string(ATLAS_PATH)
	_check(serialized_text.find("aspect =") == -1 and serialized_text.find("area_texels =") == -1,
			"derived_metrics_not_serialized")
	var loaded := ResourceLoader.load(ATLAS_PATH, "HotspotAtlas", ResourceLoader.CACHE_MODE_IGNORE) as HotspotAtlas
	_check(loaded != null, "atlas_resource_reload")
	if loaded == null:
		return
	_check(_atlas_snapshot(loaded) == before, "atlas_fields_byte_stable")
	_check(loaded.patches.size() == 3 and loaded.patches[1].aspect > 0.0 and
			loaded.patches[1].area_texels > 0.0, "derived_metrics_recomputed_after_load")
	_check(loaded.reference_texture_size == Vector2i(512, 256), "derived_texture_size_recomputed")


func _check_rect_roundtrip(texture: Texture2D) -> void:
	var atlas := HotspotAtlas.new()
	atlas.reference_texture = texture
	var import_error := atlas.import_rect(FIXTURE)
	_check(import_error == OK, "rect_fixture_import_typed_ok")
	_check(atlas.last_rect_error.is_empty(), "rect_fixture_no_error_message")
	_check(atlas.patches.size() == 2, "rect_fixture_patch_count")
	if atlas.patches.size() != 2:
		return
	var first: HotspotPatch = atlas.patches[0]
	var second: HotspotPatch = atlas.patches[1]
	_check(_rect_px_close(atlas.get_patch_rect_px(0), Rect2(16, 8, 128, 64)),
			"rect_fixture_normalized_within_half_pixel")
	_check(first.patch_name == &"hero_trim" and first.allow_rotation and first.allow_mirror_x and
			first.allow_mirror_y and first.allow_tiling and first.tiling_axis == HotspotPatch.TILING_AXIS_U and
			is_equal_approx(first.inset_px, 2.5), "rect_fixture_primary_flags")
	_check(first.extra.get("mallet_custom") == "preserve me" and
			first.extra.get("custom_block", {}).get("mode") == "nested",
			"rect_fixture_unknown_keys_preserved")
	_check(second.patch_name == &"p1" and not second.allow_rotation and second.allow_tiling and
			second.tiling_axis == HotspotPatch.TILING_AXIS_V and is_equal_approx(second.inset_px, 1.0),
			"rect_fixture_aliases_and_synthesized_name")

	var imported_snapshot := _patches_snapshot(atlas)
	_check(atlas.export_rect(EXPORTED_RECT_PATH) == OK, "rect_export")
	var exported_text := FileAccess.get_file_as_string(EXPORTED_RECT_PATH)
	_check(exported_text.find("mallet_custom") >= 0 and exported_text.find("custom_block") >= 0,
			"rect_export_reemits_unknown_keys")
	var reimported := HotspotAtlas.new()
	reimported.reference_texture = texture
	_check(reimported.import_rect_file(EXPORTED_RECT_PATH) == OK, "rect_export_reimport")
	_check(_patches_snapshot(reimported) == imported_snapshot, "rect_export_reimport_identical")

	var before_malformed := _patches_snapshot(atlas)
	var malformed_error := atlas.import_rect(MALFORMED_FIXTURE)
	_check(malformed_error == ERR_PARSE_ERROR and not atlas.last_rect_error.is_empty(),
			"rect_malformed_typed_rejection")
	_check(_patches_snapshot(atlas) == before_malformed, "rect_malformed_does_not_mutate_atlas")


func _check_binding_roundtrip() -> void:
	var texture := _make_saved_texture(SHARED_TEXTURE_PATH, 128, 128)
	if texture == null:
		return
	var material_a := StandardMaterial3D.new()
	material_a.resource_name = "M_Brick_A"
	material_a.albedo_texture = texture
	var material_b := StandardMaterial3D.new()
	material_b.resource_name = "M_Brick_Wet"
	material_b.albedo_texture = texture
	var key_a := HotspotBinding.pattern_key_from_texture_path(material_a.albedo_texture.resource_path)
	var key_b := HotspotBinding.pattern_key_from_texture_path(material_b.albedo_texture.resource_path)
	_check(key_a == key_b and key_a.ends_with("/brick"), "shared_texture_stem_same_pattern_key")
	material_b.resource_name = "Completely_Renamed_Material"
	var renamed_key := HotspotBinding.pattern_key_from_texture_path(material_b.albedo_texture.resource_path)
	_check(renamed_key == key_a, "material_rename_does_not_change_pattern_key")

	var registry := HotspotBinding.new()
	registry.set_binding(key_a, "res://levels/brick_hotspot.tres")
	_check(registry.resolve_texture_path(material_a.albedo_texture.resource_path) ==
			registry.resolve_texture_path(material_b.albedo_texture.resource_path),
			"shared_material_textures_resolve_same_atlas")
	_check(ResourceSaver.save(registry, BINDING_PATH) == OK, "binding_registry_save")
	var loaded := ResourceLoader.load(BINDING_PATH, "HotspotBinding", ResourceLoader.CACHE_MODE_IGNORE) as HotspotBinding
	_check(loaded != null and loaded.bindings == registry.bindings and
			loaded.resolve_pattern(key_a) == "res://levels/brick_hotspot.tres",
			"binding_registry_save_load_roundtrip")


func _check_resize_proofness(texture: GradientTexture2D) -> void:
	var atlas := HotspotAtlas.new()
	atlas.reference_texture = texture
	var patch := _make_patch(Rect2(0.125, 0.25, 0.5, 0.25), &"resize", false, false,
			false, false, HotspotPatch.TILING_AXIS_U, 0.0)
	var patches: Array[HotspotPatch] = [patch]
	atlas.patches = patches
	var rect_before: Rect2 = patch.rect_uv
	var pixels_before: Rect2 = atlas.get_patch_rect_px(0)
	var area_before: float = patch.area_texels
	texture.width = 1024
	texture.height = 512
	atlas.refresh_derived_metrics()
	var pixels_after: Rect2 = atlas.get_patch_rect_px(0)
	_check(patch.rect_uv == rect_before, "normalized_rect_unchanged_after_texture_resize")
	_check(pixels_after.position == pixels_before.position * 2.0 and
			pixels_after.size == pixels_before.size * 2.0 and
			is_equal_approx(patch.area_texels, area_before * 4.0),
			"derived_pixels_and_area_scale_after_texture_resize")


func _atlas_snapshot(atlas: HotspotAtlas) -> Dictionary:
	return {
		"atlas_id": atlas.atlas_id,
		"reference_texture_path": atlas.reference_texture.resource_path if atlas.reference_texture else "",
		"texel_density_target": atlas.texel_density_target,
		"patches": _patches_snapshot(atlas),
		"default_mapping_mode": atlas.default_mapping_mode,
		"disallow_random": atlas.disallow_random,
		"tiling_policy": atlas.tiling_policy,
		"param_names": atlas.param_names,
		"target_materials": atlas.target_materials,
	}


func _patches_snapshot(atlas: HotspotAtlas) -> Array:
	var result: Array = []
	for patch: HotspotPatch in atlas.patches:
		result.append({
			"rect_uv": patch.rect_uv,
			"allow_rotation": patch.allow_rotation,
			"allow_mirror_x": patch.allow_mirror_x,
			"allow_mirror_y": patch.allow_mirror_y,
			"allow_tiling": patch.allow_tiling,
			"tiling_axis": patch.tiling_axis,
			"inset_px": patch.inset_px,
			"patch_name": patch.patch_name,
			"extra": patch.extra.duplicate(true),
		})
	return result


func _rect_px_close(actual: Rect2, expected: Rect2) -> bool:
	return abs(actual.position.x - expected.position.x) <= 0.5 and \
			abs(actual.position.y - expected.position.y) <= 0.5 and \
			abs(actual.size.x - expected.size.x) <= 0.5 and \
			abs(actual.size.y - expected.size.y) <= 0.5


func _check(condition: bool, label: String) -> void:
	if not condition:
		failures.append(label)


func _finish() -> void:
	if failures.is_empty():
		print("HOTSPOT_ATLAS_SMOKE_OK")
		quit(0)
	else:
		printerr("HOTSPOT_ATLAS_SMOKE_FAIL: %s" % ", ".join(failures))
		quit(1)
