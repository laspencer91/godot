/**************************************************************************/
/*  ao_baker_3d.cpp                                                       */
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

#include "ao_baker_3d.h"

#include "core/io/config_file.h"
#include "core/io/dir_access.h"
#include "core/io/image.h"
#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/3d/lightmapper.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/texture.h"
#include "servers/rendering/rendering_server.h"

// Whether every triangle surface of p_mesh carries both UV2 and normals (and there is at least one
// triangle surface). This is exactly the condition a mesh must meet to be baked. A mesh that has
// triangle surfaces but fails here is a candidate for auto-unwrap; one with no triangle surfaces is
// not bakeable at all.
static bool _mesh_ready_for_bake(const Ref<Mesh> &p_mesh, bool *r_has_triangle_surface = nullptr) {
	bool surfaces_found = false;
	bool all_have_uv2_and_normal = true;
	for (int i = 0; i < p_mesh->get_surface_count(); i++) {
		if (p_mesh->surface_get_primitive_type(i) != Mesh::PRIMITIVE_TRIANGLES) {
			continue;
		}
		surfaces_found = true;
		const uint64_t format = p_mesh->surface_get_format(i);
		if (!(format & Mesh::ARRAY_FORMAT_TEX_UV2) || !(format & Mesh::ARRAY_FORMAT_NORMAL)) {
			all_have_uv2_and_normal = false;
			break;
		}
	}
	if (r_has_triangle_surface) {
		*r_has_triangle_surface = surfaces_found;
	}
	return surfaces_found && all_have_uv2_and_normal;
}

void AOBaker3D::_find_meshes(Node *p_at_node, Vector<MeshFound> &r_meshes) {
	MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(p_at_node);
	if (mi && mi->get_gi_mode() == GeometryInstance3D::GI_MODE_STATIC && mi->is_visible_in_tree()) {
		Ref<Mesh> mesh = mi->get_mesh();
		if (mesh.is_valid() && _mesh_ready_for_bake(mesh)) {
			MeshFound mf;
			mf.xform = get_global_transform().affine_inverse() * mi->get_global_transform();
			mf.node_path = get_path_to(mi);
			mf.mesh = mesh;
			mf.lightmap_scale = mi->get_lightmap_texel_scale();
			r_meshes.push_back(mf);
		}
	}

	for (int i = 0; i < p_at_node->get_child_count(); i++) {
		Node *child = p_at_node->get_child(i);
		if (!child->get_owner()) {
			continue; // Helper node not part of the saved scene.
		}
		_find_meshes(child, r_meshes);
	}
}

void AOBaker3D::_collect_candidates(Node *p_at_node, Vector<MeshInstance3D *> &r_ready, Vector<MeshInstance3D *> &r_missing_uv2) const {
	MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(p_at_node);
	if (mi && mi->get_gi_mode() == GeometryInstance3D::GI_MODE_STATIC && mi->is_visible_in_tree()) {
		Ref<Mesh> mesh = mi->get_mesh();
		if (mesh.is_valid()) {
			bool has_triangle_surface = false;
			if (_mesh_ready_for_bake(mesh, &has_triangle_surface)) {
				r_ready.push_back(mi);
			} else if (has_triangle_surface) {
				// Has bakeable geometry but is missing UV2 (or normals) -- offer to unwrap it.
				r_missing_uv2.push_back(mi);
			}
		}
	}

	for (int i = 0; i < p_at_node->get_child_count(); i++) {
		Node *child = p_at_node->get_child(i);
		if (!child->get_owner()) {
			continue; // Helper node not part of the saved scene.
		}
		_collect_candidates(child, r_ready, r_missing_uv2);
	}
}

void AOBaker3D::get_bake_candidates(Vector<MeshInstance3D *> &r_ready, Vector<MeshInstance3D *> &r_missing_uv2) const {
	_collect_candidates(get_parent() ? get_parent() : (Node *)this, r_ready, r_missing_uv2);
}

AOBaker3D::BakeError AOBaker3D::_bake_error(BakeError p_error, const String &p_message) {
	last_bake_error_message = p_message;
	ERR_PRINT(vformat("AOBaker3D: %s", p_message));
	return p_error;
}

AOBaker3D::BakeError AOBaker3D::bake(const String &p_atlas_path) {
	last_bake_error_message.clear();

	Vector<MeshFound> meshes;
	_find_meshes(get_parent() ? get_parent() : (Node *)this, meshes);
	if (meshes.is_empty()) {
		return _bake_error(BAKE_ERROR_NO_MESHES, "No visible GI-static meshes with normals and UV2 were found in the bake scope.");
	}

	Ref<Lightmapper> lightmapper = Lightmapper::create();
	if (lightmapper.is_null()) {
		return _bake_error(BAKE_ERROR_NO_LIGHTMAPPER, "No lightmapper backend is available.");
	}

	for (int m = 0; m < meshes.size(); m++) {
		const MeshFound &mf = meshes[m];

		Size2i hint = mf.mesh->get_lightmap_size_hint();
		if (hint == Size2i(0, 0)) {
			hint = Size2i(64, 64);
		}
		// Independent (lower) texel density: grime masks don't need lighting-grade density.
		Size2i tex_size = Size2i(Size2(hint) * mf.lightmap_scale * ao_texel_scale);
		tex_size.x = MAX(tex_size.x, 1);
		tex_size.y = MAX(tex_size.y, 1);

		// AO is a secondary mask, so an unusually large UV2 chart should gracefully lose density
		// instead of failing the entire level bake. Leave room for the atlas packer's denoiser padding
		// and preserve the chart's aspect ratio while fitting it to the configured texture limit.
		const int chart_limit = MAX(max_texture_size - MAX(2, denoiser_range), 1);
		if (tex_size.x > chart_limit || tex_size.y > chart_limit) {
			const Size2i requested_size = tex_size;
			const float fit_scale = MIN((float)chart_limit / tex_size.x, (float)chart_limit / tex_size.y);
			tex_size.x = MAX((int)Math::floor(tex_size.x * fit_scale), 1);
			tex_size.y = MAX((int)Math::floor(tex_size.y * fit_scale), 1);
			print_line(vformat("AOBaker3D: fitted oversized AO chart %s from %dx%d to %dx%d (Max Texture Size: %d).", mf.node_path, requested_size.x, requested_size.y, tex_size.x, tex_size.y, max_texture_size));
		}

		Lightmapper::MeshData md;
		{
			Dictionary d;
			d["path"] = mf.node_path;
			md.userdata = d;
		}

		// Dummy solid images: add_mesh requires non-empty albedo/emission and the atlas packer derives
		// each mesh's rect from the albedo dimensions. AO rays never sample them (no material capture).
		md.albedo_on_uv2.instantiate();
		md.albedo_on_uv2->initialize_data(tex_size.x, tex_size.y, false, Image::FORMAT_RGBA8);
		md.albedo_on_uv2->fill(Color(1, 1, 1, 1));
		md.emission_on_uv2.instantiate();
		md.emission_on_uv2->initialize_data(tex_size.x, tex_size.y, false, Image::FORMAT_RGBAH);
		md.emission_on_uv2->fill(Color(0, 0, 0, 1));

		const Basis normal_xform = mf.xform.basis.inverse().transposed();
		for (int i = 0; i < mf.mesh->get_surface_count(); i++) {
			if (mf.mesh->surface_get_primitive_type(i) != Mesh::PRIMITIVE_TRIANGLES) {
				continue;
			}
			Array a = mf.mesh->surface_get_arrays(i);
			Ref<Material> mat = mf.mesh->surface_get_material(i);
			RID mat_rid = mat.is_valid() ? mat->get_rid() : RID();

			Vector<Vector3> vertices = a[Mesh::ARRAY_VERTEX];
			Vector<Vector2> uv = a[Mesh::ARRAY_TEX_UV2];
			Vector<Vector3> normals = a[Mesh::ARRAY_NORMAL];
			Vector<int> index = a[Mesh::ARRAY_INDEX];
			if (uv.is_empty() || normals.is_empty()) {
				continue;
			}
			const Vector3 *vr = vertices.ptr();
			const Vector2 *uvr = uv.ptr();
			const Vector3 *nr = normals.ptr();

			int facecount;
			const int *ir = nullptr;
			if (index.size()) {
				facecount = index.size() / 3;
				ir = index.ptr();
			} else {
				facecount = vertices.size() / 3;
			}

			for (int j = 0; j < facecount; j++) {
				uint32_t vidx[3];
				for (int k = 0; k < 3; k++) {
					vidx[k] = ir ? (uint32_t)ir[j * 3 + k] : (uint32_t)(j * 3 + k);
				}
				for (int k = 0; k < 3; k++) {
					md.points.push_back(mf.xform.xform(vr[vidx[k]]));
					md.uv2.push_back(uvr[vidx[k]]);
					md.normal.push_back(normal_xform.xform(nr[vidx[k]]).normalized());
					md.material.push_back(mat_rid);
				}
			}
		}

		if (md.points.is_empty()) {
			continue;
		}
		lightmapper->add_mesh(md);
	}

	Lightmapper::BakeError err = lightmapper->bake_ao(ao_ray_count, ao_max_distance, bias, max_texture_size, use_denoiser, denoiser_strength, denoiser_range, 1.0f);
	if (err != Lightmapper::BAKE_OK) {
		String reason;
		switch (err) {
			case Lightmapper::BAKE_ERROR_TEXTURE_EXCEEDS_MAX_SIZE:
				reason = vformat("A mesh's AO chart exceeds Max Texture Size (%d). Lower AO Texel Scale or raise Max Texture Size.", max_texture_size);
				break;
			case Lightmapper::BAKE_ERROR_LIGHTMAP_CANT_PRE_BAKE_MESHES:
				reason = "The GPU lightmapper could not prepare the AO bake. Check RenderingDevice errors and ensure the editor is using Forward+ or Mobile rendering.";
				break;
			case Lightmapper::BAKE_ERROR_ATLAS_TOO_SMALL:
				reason = vformat("The AO charts could not fit in Max Texture Size (%d). Lower AO Texel Scale or raise Max Texture Size.", max_texture_size);
				break;
			case Lightmapper::BAKE_ERROR_USER_ABORTED:
				reason = "The AO bake was cancelled.";
				break;
			default:
				reason = vformat("The lightmapper returned an unknown bake error (%d).", err);
				break;
		}
		return _bake_error(BAKE_ERROR_BAKE_FAILED, reason);
	}

	// Keep the whole scene-space atlas (all slices) as ONE Texture2DArray, and record each mesh's
	// rect + slice into it. This mirrors how the lightmapper stores its result: a shared texture the
	// material samples, plus a per-instance transform -- NOT a texture per mesh.
	const int ao_slices = lightmapper->get_bake_ao_texture_count();
	Vector<Ref<Image>> slice_images;
	for (int s = 0; s < ao_slices; s++) {
		Ref<Image> img = lightmapper->get_bake_ao_texture(s);
		if (img.is_valid()) {
			slice_images.push_back(img);
		}
	}
	if (slice_images.is_empty()) {
		return _bake_error(BAKE_ERROR_BAKE_FAILED, "The lightmapper completed without producing an AO atlas image.");
	}

	Ref<Texture2DArray> atlas;
	atlas.instantiate();
	if (atlas->create_from_images(slice_images) != OK) {
		return _bake_error(BAKE_ERROR_BAKE_FAILED, "The baked AO atlas layers have incompatible dimensions or formats.");
	}
	Dictionary new_transforms;
	const int mesh_count = lightmapper->get_bake_mesh_count();
	for (int i = 0; i < mesh_count; i++) {
		const int slice = lightmapper->get_bake_mesh_texture_slice(i);
		if (slice < 0 || slice >= slice_images.size()) {
			continue;
		}
		const Rect2 uv = lightmapper->get_bake_mesh_uv_scale(i);
		const NodePath np = ((Dictionary)lightmapper->get_bake_mesh_userdata(i)).get("path", NodePath());

		Array entry;
		entry.push_back(Vector4(uv.position.x, uv.position.y, uv.size.x, uv.size.y));
		entry.push_back(slice);
		new_transforms[np] = entry;
	}

	Ref<TextureLayered> stored_atlas = atlas;

	// The editor supplies an external PNG path so the atlas payload does not become Base64 inside a
	// text scene. The source image is imported as a Texture2DArray, matching the lightmap pipeline and
	// keeping the scene reference small. Keep the empty-path behavior for programmatic in-memory bakes.
	if (!p_atlas_path.is_empty()) {
		ERR_FAIL_COND_V_MSG(!p_atlas_path.begins_with("res://") || p_atlas_path.get_extension().to_lower() != "png", BAKE_ERROR_CANT_CREATE_DATA, "AO bake data path must be a res:// PNG path.");
		ERR_FAIL_NULL_V_MSG(ResourceLoader::import, BAKE_ERROR_CANT_CREATE_DATA, "AO bake data can only be imported while running in the editor.");
		Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_RESOURCES);
		ERR_FAIL_COND_V_MSG(da.is_null(), BAKE_ERROR_CANT_CREATE_DATA, "Could not access the project resource directory for AO bake data.");
		const String relative_dir = p_atlas_path.get_base_dir().trim_prefix("res://");
		ERR_FAIL_COND_V_MSG(!relative_dir.is_empty() && da->make_dir_recursive(relative_dir) != OK, BAKE_ERROR_CANT_CREATE_DATA, "Could not create the AO bake data directory.");

		const int slice_width = slice_images[0]->get_width();
		const int slice_height = slice_images[0]->get_height();
		const int max_columns = Image::MAX_WIDTH / slice_width;
		const int max_rows = Image::MAX_HEIGHT / slice_height;
		ERR_FAIL_COND_V_MSG(max_columns < 1 || max_rows < 1 || slice_images.size() > (int64_t)max_columns * max_rows, BAKE_ERROR_CANT_CREATE_DATA, "AO bake atlas is too large to store in one imported Texture2DArray.");

		int columns = MIN((int)Math::ceil(Math::sqrt((double)slice_images.size())), max_columns);
		int rows = (slice_images.size() + columns - 1) / columns;
		if (rows > max_rows) {
			columns = (slice_images.size() + max_rows - 1) / max_rows;
			rows = (slice_images.size() + columns - 1) / columns;
		}

		Ref<Image> source_image = Image::create_empty(slice_width * columns, slice_height * rows, false, Image::FORMAT_L8);
		for (int i = 0; i < slice_images.size(); i++) {
			// AO readback is R8. Image::convert(R8 -> L8) applies RGB luminance weights and would
			// incorrectly cap fully open texels at the red coefficient (~0.2126). Reinterpret the
			// single-channel bytes as L8 instead so the PNG preserves openness exactly.
			Ref<Image> red_slice = slice_images[i]->duplicate();
			red_slice->convert(Image::FORMAT_R8);
			Ref<Image> slice = Image::create_from_data(slice_width, slice_height, false, Image::FORMAT_L8, red_slice->get_data());
			const Point2i destination((i % columns) * slice_width, (i / columns) * slice_height);
			source_image->blit_rect(slice, Rect2i(0, 0, slice_width, slice_height), destination);
		}

		Ref<ConfigFile> import_config;
		import_config.instantiate();
		const String import_path = p_atlas_path + ".import";
		if (FileAccess::exists(import_path)) {
			import_config->load(import_path);
		}
		import_config->set_value("remap", "importer", "2d_array_texture");
		import_config->set_value("remap", "type", "CompressedTexture2DArray");
		import_config->set_value("params", "compress/mode", 0);
		import_config->set_value("params", "compress/channel_pack", 1);
		import_config->set_value("params", "mipmaps/generate", false);
		import_config->set_value("params", "slices/horizontal", columns);
		import_config->set_value("params", "slices/vertical", rows);
		ERR_FAIL_COND_V_MSG(import_config->save(import_path) != OK, BAKE_ERROR_CANT_CREATE_DATA, "Could not save the AO Texture2DArray import configuration.");
		ERR_FAIL_COND_V_MSG(source_image->save_png(p_atlas_path) != OK, BAKE_ERROR_CANT_CREATE_DATA, "Could not save the AO atlas source PNG.");
		ERR_FAIL_COND_V_MSG(ResourceLoader::import(p_atlas_path) != OK, BAKE_ERROR_CANT_CREATE_DATA, "Could not import the AO atlas source as a Texture2DArray.");

		stored_atlas = ResourceLoader::load(p_atlas_path, "TextureLayered", ResourceLoader::CACHE_MODE_REPLACE);
		ERR_FAIL_COND_V_MSG(stored_atlas.is_null() || stored_atlas->get_layered_type() != TextureLayered::LAYERED_TYPE_2D_ARRAY, BAKE_ERROR_CANT_CREATE_DATA, "Imported AO bake data is not a Texture2DArray.");
	}

	ao_atlas = stored_atlas;
	ao_transforms = new_transforms;

	// Debug: dump each mesh's rect (sliced out of the atlas) to a PNG so the AO can be eyeballed in
	// the FileSystem dock before a weathering shader exists. Not the storage format -- just a preview.
	if (!debug_output_directory.is_empty()) {
		Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_RESOURCES);
		if (da.is_valid()) {
			da->make_dir_recursive(debug_output_directory);
		}
		Array keys = new_transforms.keys();
		for (int i = 0; i < keys.size(); i++) {
			const NodePath np = keys[i];
			const Array entry = new_transforms[keys[i]];
			const Vector4 rect = entry[0];
			const int slice = entry[1];
			Ref<Image> src = slice_images[slice];
			const int aw = src->get_width();
			const int ah = src->get_height();
			const Point2i ofs = Point2i(Math::round(rect.x * aw), Math::round(rect.y * ah));
			const Size2i sz = Size2i(Math::round(rect.z * aw), Math::round(rect.w * ah));
			if (sz.x <= 0 || sz.y <= 0) {
				continue;
			}
			Ref<Image> crop;
			crop.instantiate();
			crop->initialize_data(sz.x, sz.y, false, src->get_format());
			crop->blit_rect(src, Rect2i(ofs, sz), Point2i(0, 0));
			const int n = np.get_name_count();
			const String base = n > 0 ? String(np.get_name(n - 1)) : ("mesh_" + itos(i));
			crop->save_png(debug_output_directory.path_join(vformat("ao_%s_%d.png", base, i)));
		}
	}

	apply_to_meshes();
	return BAKE_ERROR_OK;
}

int AOBaker3D::apply_to_meshes() {
	// Deliver the bake through the engine's per-instance AO-map channel (mirrors the lightmap): the
	// shared atlas + this mesh's UV rect + slice are set on the RenderingServer instance, so any
	// shader that reads the AO_MAP built-in gets it -- no material mutation, no global, no uniforms.
	const RID atlas_rid = ao_atlas.is_valid() ? ao_atlas->get_rid() : RID();
	int wired = 0;
	Array keys = ao_transforms.keys();
	for (int i = 0; i < keys.size(); i++) {
		const NodePath np = keys[i];
		MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(get_node_or_null(np));
		if (!mi) {
			continue;
		}
		const Array entry = ao_transforms[keys[i]];
		if (entry.size() < 2) {
			continue;
		}
		const Vector4 rect = entry[0];
		const int slice = entry[1];
		RenderingServer::get_singleton()->instance_geometry_set_ao_map(mi->get_instance(), atlas_rid, Rect2(rect.x, rect.y, rect.z, rect.w), slice);
		wired++;
	}
	return wired;
}

void AOBaker3D::set_ao_ray_count(int p_ao_ray_count) { ao_ray_count = CLAMP(p_ao_ray_count, 16, 8192); }
int AOBaker3D::get_ao_ray_count() const { return ao_ray_count; }
void AOBaker3D::set_ao_max_distance(float p_distance) { ao_max_distance = MAX(p_distance, 0.0f); }
float AOBaker3D::get_ao_max_distance() const { return ao_max_distance; }
void AOBaker3D::set_ao_texel_scale(float p_scale) { ao_texel_scale = MAX(p_scale, 0.01f); }
float AOBaker3D::get_ao_texel_scale() const { return ao_texel_scale; }
void AOBaker3D::set_use_denoiser(bool p_enable) { use_denoiser = p_enable; }
bool AOBaker3D::is_using_denoiser() const { return use_denoiser; }
void AOBaker3D::set_denoiser_strength(float p_strength) { denoiser_strength = MAX(p_strength, 0.0f); }
float AOBaker3D::get_denoiser_strength() const { return denoiser_strength; }
void AOBaker3D::set_denoiser_range(int p_range) { denoiser_range = MAX(p_range, 1); }
int AOBaker3D::get_denoiser_range() const { return denoiser_range; }
void AOBaker3D::set_bias(float p_bias) { bias = MAX(p_bias, 0.0f); }
float AOBaker3D::get_bias() const { return bias; }
void AOBaker3D::set_max_texture_size(int p_size) { max_texture_size = MAX(p_size, 256); }
int AOBaker3D::get_max_texture_size() const { return max_texture_size; }
void AOBaker3D::set_debug_output_directory(const String &p_dir) { debug_output_directory = p_dir; }
String AOBaker3D::get_debug_output_directory() const { return debug_output_directory; }

void AOBaker3D::set_ao_atlas(const Ref<TextureLayered> &p_atlas) { ao_atlas = p_atlas; }
Ref<TextureLayered> AOBaker3D::get_ao_atlas() const { return ao_atlas; }
void AOBaker3D::set_ao_transforms(const Dictionary &p_transforms) { ao_transforms = p_transforms; }
Dictionary AOBaker3D::get_ao_transforms() const { return ao_transforms; }
String AOBaker3D::get_last_bake_error_message() const { return last_bake_error_message; }

void AOBaker3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_ao_ray_count", "ao_ray_count"), &AOBaker3D::set_ao_ray_count);
	ClassDB::bind_method(D_METHOD("get_ao_ray_count"), &AOBaker3D::get_ao_ray_count);
	ClassDB::bind_method(D_METHOD("set_ao_max_distance", "distance"), &AOBaker3D::set_ao_max_distance);
	ClassDB::bind_method(D_METHOD("get_ao_max_distance"), &AOBaker3D::get_ao_max_distance);
	ClassDB::bind_method(D_METHOD("set_ao_texel_scale", "scale"), &AOBaker3D::set_ao_texel_scale);
	ClassDB::bind_method(D_METHOD("get_ao_texel_scale"), &AOBaker3D::get_ao_texel_scale);
	ClassDB::bind_method(D_METHOD("set_use_denoiser", "enable"), &AOBaker3D::set_use_denoiser);
	ClassDB::bind_method(D_METHOD("is_using_denoiser"), &AOBaker3D::is_using_denoiser);
	ClassDB::bind_method(D_METHOD("set_denoiser_strength", "strength"), &AOBaker3D::set_denoiser_strength);
	ClassDB::bind_method(D_METHOD("get_denoiser_strength"), &AOBaker3D::get_denoiser_strength);
	ClassDB::bind_method(D_METHOD("set_denoiser_range", "range"), &AOBaker3D::set_denoiser_range);
	ClassDB::bind_method(D_METHOD("get_denoiser_range"), &AOBaker3D::get_denoiser_range);
	ClassDB::bind_method(D_METHOD("set_bias", "bias"), &AOBaker3D::set_bias);
	ClassDB::bind_method(D_METHOD("get_bias"), &AOBaker3D::get_bias);
	ClassDB::bind_method(D_METHOD("set_max_texture_size", "size"), &AOBaker3D::set_max_texture_size);
	ClassDB::bind_method(D_METHOD("get_max_texture_size"), &AOBaker3D::get_max_texture_size);
	ClassDB::bind_method(D_METHOD("set_debug_output_directory", "dir"), &AOBaker3D::set_debug_output_directory);
	ClassDB::bind_method(D_METHOD("get_debug_output_directory"), &AOBaker3D::get_debug_output_directory);
	ClassDB::bind_method(D_METHOD("set_ao_atlas", "atlas"), &AOBaker3D::set_ao_atlas);
	ClassDB::bind_method(D_METHOD("get_ao_atlas"), &AOBaker3D::get_ao_atlas);
	ClassDB::bind_method(D_METHOD("set_ao_transforms", "transforms"), &AOBaker3D::set_ao_transforms);
	ClassDB::bind_method(D_METHOD("get_ao_transforms"), &AOBaker3D::get_ao_transforms);
	ClassDB::bind_method(D_METHOD("get_last_bake_error_message"), &AOBaker3D::get_last_bake_error_message);
	ClassDB::bind_method(D_METHOD("apply_to_meshes"), &AOBaker3D::apply_to_meshes);
	ClassDB::bind_method(D_METHOD("bake", "atlas_path"), &AOBaker3D::bake, DEFVAL(String()));

	ADD_PROPERTY(PropertyInfo(Variant::INT, "ao_ray_count", PROPERTY_HINT_RANGE, "16,8192,1"), "set_ao_ray_count", "get_ao_ray_count");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ao_max_distance", PROPERTY_HINT_RANGE, "0.0,10.0,0.01,or_greater,suffix:m"), "set_ao_max_distance", "get_ao_max_distance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ao_texel_scale", PROPERTY_HINT_RANGE, "0.05,2.0,0.05"), "set_ao_texel_scale", "get_ao_texel_scale");
	ADD_GROUP("Denoiser", "denoiser_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "denoiser_use"), "set_use_denoiser", "is_using_denoiser");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "denoiser_strength", PROPERTY_HINT_RANGE, "0.0,1.0,0.01"), "set_denoiser_strength", "get_denoiser_strength");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "denoiser_range", PROPERTY_HINT_RANGE, "1,50,1"), "set_denoiser_range", "get_denoiser_range");
	ADD_GROUP("Advanced", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bias", PROPERTY_HINT_RANGE, "0.0,0.01,0.00001"), "set_bias", "get_bias");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_texture_size", PROPERTY_HINT_RANGE, "256,16384,1"), "set_max_texture_size", "get_max_texture_size");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "debug_output_directory", PROPERTY_HINT_DIR), "set_debug_output_directory", "get_debug_output_directory");
	// Baked output: stored/loaded with the scene, hidden from the inspector, no undo churn.
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "ao_atlas", PROPERTY_HINT_RESOURCE_TYPE, "TextureLayered", PROPERTY_USAGE_NO_EDITOR), "set_ao_atlas", "get_ao_atlas");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "ao_transforms", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), "set_ao_transforms", "get_ao_transforms");

	BIND_ENUM_CONSTANT(BAKE_ERROR_OK);
	BIND_ENUM_CONSTANT(BAKE_ERROR_NO_MESHES);
	BIND_ENUM_CONSTANT(BAKE_ERROR_NO_LIGHTMAPPER);
	BIND_ENUM_CONSTANT(BAKE_ERROR_BAKE_FAILED);
	BIND_ENUM_CONSTANT(BAKE_ERROR_CANT_CREATE_DATA);
}

void AOBaker3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			// The RenderingServer per-instance AO-map binding is runtime state (not serialized), so
			// re-push it whenever this scene enters the tree -- in the editor and at game runtime.
			// Deferred so sibling MeshInstance3Ds have created their RS instances first.
			if (ao_atlas.is_valid()) {
				callable_mp(this, &AOBaker3D::apply_to_meshes).call_deferred();
			}
		} break;
	}
}

AOBaker3D::AOBaker3D() {
}
