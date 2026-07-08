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

#include "core/io/dir_access.h"
#include "core/io/image.h"
#include "core/object/class_db.h"
#include "scene/3d/lightmapper.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"

void AOBaker3D::_find_meshes(Node *p_at_node, Vector<MeshFound> &r_meshes) {
	MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(p_at_node);
	if (mi && mi->get_gi_mode() == GeometryInstance3D::GI_MODE_STATIC && mi->is_visible_in_tree()) {
		Ref<Mesh> mesh = mi->get_mesh();
		if (mesh.is_valid()) {
			bool all_have_uv2_and_normal = true;
			bool surfaces_found = false;
			for (int i = 0; i < mesh->get_surface_count(); i++) {
				if (mesh->surface_get_primitive_type(i) != Mesh::PRIMITIVE_TRIANGLES) {
					continue;
				}
				if (!(mesh->surface_get_format(i) & Mesh::ARRAY_FORMAT_TEX_UV2)) {
					all_have_uv2_and_normal = false;
					break;
				}
				if (!(mesh->surface_get_format(i) & Mesh::ARRAY_FORMAT_NORMAL)) {
					all_have_uv2_and_normal = false;
					break;
				}
				surfaces_found = true;
			}

			if (surfaces_found && all_have_uv2_and_normal) {
				MeshFound mf;
				mf.xform = get_global_transform().affine_inverse() * mi->get_global_transform();
				mf.node_path = get_path_to(mi);
				mf.mesh = mesh;
				mf.lightmap_scale = mi->get_lightmap_texel_scale();
				r_meshes.push_back(mf);
			}
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

AOBaker3D::BakeError AOBaker3D::bake() {
	Vector<MeshFound> meshes;
	_find_meshes(get_parent() ? get_parent() : (Node *)this, meshes);
	if (meshes.is_empty()) {
		return BAKE_ERROR_NO_MESHES;
	}

	Ref<Lightmapper> lightmapper = Lightmapper::create();
	if (lightmapper.is_null()) {
		return BAKE_ERROR_NO_LIGHTMAPPER;
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
		return BAKE_ERROR_BAKE_FAILED;
	}

	// Crop each mesh's rect out of its atlas slice into a standalone R8 mask, keyed by the mesh's
	// NodePath (from the userdata dictionary). The mesh's raw UV2 samples this directly.
	Dictionary new_masks;
	const int mesh_count = lightmapper->get_bake_mesh_count();
	const int ao_slices = lightmapper->get_bake_ao_texture_count();
	for (int i = 0; i < mesh_count; i++) {
		const int slice = lightmapper->get_bake_mesh_texture_slice(i);
		if (slice < 0 || slice >= ao_slices) {
			continue;
		}
		Ref<Image> atlas = lightmapper->get_bake_ao_texture(slice);
		if (atlas.is_null()) {
			continue;
		}
		const int aw = atlas->get_width();
		const int ah = atlas->get_height();
		const Rect2 uv = lightmapper->get_bake_mesh_uv_scale(i);
		const Point2i ofs = Point2i(Math::round(uv.position.x * aw), Math::round(uv.position.y * ah));
		const Size2i sz = Size2i(Math::round(uv.size.x * aw), Math::round(uv.size.y * ah));
		if (sz.x <= 0 || sz.y <= 0) {
			continue;
		}

		Ref<Image> cropped;
		cropped.instantiate();
		cropped->initialize_data(sz.x, sz.y, false, Image::FORMAT_R8);
		cropped->blit_rect(atlas, Rect2i(ofs, sz), Point2i(0, 0));

		Dictionary ud = lightmapper->get_bake_mesh_userdata(i);
		NodePath np = ud.get("path", NodePath());
		new_masks[np] = ImageTexture::create_from_image(cropped);
	}

	ao_masks = new_masks;

	// Debug: dump each mask to a PNG so the AO can be inspected before a weathering shader exists.
	if (!debug_output_directory.is_empty()) {
		Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_RESOURCES);
		if (da.is_valid()) {
			da->make_dir_recursive(debug_output_directory);
		}
		Array keys = new_masks.keys();
		for (int i = 0; i < keys.size(); i++) {
			NodePath np = keys[i];
			Ref<Texture2D> tex = new_masks[keys[i]];
			if (tex.is_null()) {
				continue;
			}
			Ref<Image> img = tex->get_image();
			if (img.is_null()) {
				continue;
			}
			int n = np.get_name_count();
			String base = n > 0 ? String(np.get_name(n - 1)) : ("mesh_" + itos(i));
			img->save_png(debug_output_directory.path_join(vformat("ao_%s_%d.png", base, i)));
		}
	}

	return BAKE_ERROR_OK;
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

void AOBaker3D::set_ao_masks(const Dictionary &p_masks) { ao_masks = p_masks; }
Dictionary AOBaker3D::get_ao_masks() const { return ao_masks; }

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
	ClassDB::bind_method(D_METHOD("set_ao_masks", "masks"), &AOBaker3D::set_ao_masks);
	ClassDB::bind_method(D_METHOD("get_ao_masks"), &AOBaker3D::get_ao_masks);
	ClassDB::bind_method(D_METHOD("bake"), &AOBaker3D::bake);

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
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "ao_masks", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), "set_ao_masks", "get_ao_masks");

	BIND_ENUM_CONSTANT(BAKE_ERROR_OK);
	BIND_ENUM_CONSTANT(BAKE_ERROR_NO_MESHES);
	BIND_ENUM_CONSTANT(BAKE_ERROR_NO_LIGHTMAPPER);
	BIND_ENUM_CONSTANT(BAKE_ERROR_BAKE_FAILED);
}

AOBaker3D::AOBaker3D() {
}
