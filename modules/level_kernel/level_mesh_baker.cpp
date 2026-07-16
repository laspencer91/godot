/**************************************************************************/
/*  level_mesh_baker.cpp                                                  */
/**************************************************************************/

#include "level_mesh_baker.h"

#include "level_mesh_data.h"

#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "core/templates/hash_map.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"

namespace {

struct SurfaceBucket {
	PackedVector3Array vertices;
	PackedVector3Array normals;
	PackedVector2Array uv0;
	PackedColorArray colors;
	PackedInt32Array indices;
};

} // namespace

String LevelMeshBaker::get_builtin_blockout_material_path(int p_slot) {
	if (p_slot < 0 || p_slot >= 10) {
		return String();
	}
	return "builtin://level_editor/blockout/" + itos(p_slot);
}

Ref<Material> LevelMeshBaker::resolve_material_path(const String &p_path) {
	static const String prefix = "builtin://level_editor/blockout/";
	if (!p_path.begins_with(prefix)) {
		if (p_path.is_empty()) {
			return Ref<Material>();
		}
		Ref<Material> material = ResourceLoader::load(p_path);
		return material;
	}

	const String slot_string = p_path.trim_prefix(prefix);
	if (!slot_string.is_valid_int()) {
		return Ref<Material>();
	}
	const int slot = slot_string.to_int();
	if (slot < 0 || slot >= 10 || get_builtin_blockout_material_path(slot) != p_path) {
		return Ref<Material>();
	}

	static const Color colors[10] = {
		Color("4a4d52"), Color("696d73"), Color("92969c"), Color("c2c5c9"),
		Color("d46a2d"), Color("e89b3f"), Color("3f72bb"), Color("53a4cf"),
		Color("4f9a62"), Color("78b94d")
	};
	static const char *names[10] = {
		"Blockout 1 - Charcoal", "Blockout 2 - Dark Gray", "Blockout 3 - Mid Gray", "Blockout 4 - Light Gray",
		"Blockout 5 - Orange", "Blockout 6 - Amber", "Blockout 7 - Blue", "Blockout 8 - Cyan",
		"Blockout 9 - Green", "Blockout 0 - Lime"
	};

	const Color base = colors[slot];
	const Color alternate = base.lerp(Color(1, 1, 1), 0.16f);
	const Color grid = base.lerp(Color(0, 0, 0), 0.58f);
	const Color axis = slot % 2 == 0 ? Color(1.0, 0.82, 0.22) : Color(0.95, 0.95, 0.95);
	Ref<Image> image = Image::create_empty(64, 64, false, Image::FORMAT_RGBA8);
	for (int y = 0; y < 64; y++) {
		for (int x = 0; x < 64; x++) {
			Color pixel = (((x / 8) + (y / 8)) & 1) == 0 ? base : alternate;
			if (x % 16 == 0 || y % 16 == 0) {
				pixel = grid;
			}
			if (x == 32 || y == 32) {
				pixel = axis;
			}
			image->set_pixel(x, y, pixel);
		}
	}

	Ref<ImageTexture> texture = ImageTexture::create_from_image(image);
	texture->set_name(names[slot]);
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_name(names[slot]);
	material->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, texture);
	material->set_texture_filter(BaseMaterial3D::TEXTURE_FILTER_NEAREST_WITH_MIPMAPS);
	material->set_flag(BaseMaterial3D::FLAG_USE_TEXTURE_REPEAT, true);
	material->set_roughness(0.9f);
	material->set_metallic(0.0f);
	return material;
}

Vector3 LevelMeshBaker::_face_normal(const LevelMeshData &p_data, int p_face_id) {
	const int loop_start = p_data.face_loop_starts[p_face_id];
	const Vector3 &p0 = p_data.vertex_positions[p_data.loop_vertex_indices[loop_start]];
	const Vector3 &p1 = p_data.vertex_positions[p_data.loop_vertex_indices[loop_start + 1]];
	const Vector3 &p2 = p_data.vertex_positions[p_data.loop_vertex_indices[loop_start + 2]];
	return (p1 - p0).cross(p2 - p0).normalized();
}

Ref<ArrayMesh> LevelMeshBaker::bake(const Ref<LevelMeshData> &p_data) const {
	ERR_FAIL_COND_V(p_data.is_null(), Ref<ArrayMesh>());

	Ref<ArrayMesh> mesh;
	mesh.instantiate();

	HashMap<int, int> bucket_by_material;
	Vector<int> material_indices;
	Vector<SurfaceBucket> buckets;
	for (int face_id = 0; face_id < p_data->face_alive.size(); face_id++) {
		if (!p_data->face_is_bakeable(face_id)) {
			continue;
		}
		const int material_index = p_data->face_material_indices[face_id];
		const int *existing_bucket = bucket_by_material.getptr(material_index);
		int bucket_index = -1;
		if (existing_bucket) {
			bucket_index = *existing_bucket;
		} else {
			bucket_index = buckets.size();
			bucket_by_material.insert(material_index, bucket_index);
			material_indices.push_back(material_index);
			buckets.push_back(SurfaceBucket());
		}

		SurfaceBucket &bucket = buckets.write[bucket_index];
		const int loop_start = p_data->face_loop_starts[face_id];
		const int loop_count = p_data->face_loop_counts[face_id];
		const int surface_vertex_start = bucket.vertices.size();
		const Vector3 face_normal = _face_normal(**p_data, face_id);
		for (int corner = 0; corner < loop_count; corner++) {
			const int loop_id = loop_start + corner;
			bucket.vertices.push_back(p_data->vertex_positions[p_data->loop_vertex_indices[loop_id]]);
			const Vector3 loop_normal = p_data->loop_normals[loop_id];
			bucket.normals.push_back(loop_normal.length_squared() > CMP_EPSILON2 ? loop_normal.normalized() : face_normal);
			bucket.uv0.push_back(p_data->loop_uv0[loop_id]);
			bucket.colors.push_back(p_data->loop_colors[loop_id]);
		}

		// Kernel loops are CCW-outward (Manifold convention); Godot front faces are
		// clockwise, so the fan is emitted reversed at the bake seam.
		for (int corner = 1; corner < loop_count - 1; corner++) {
			bucket.indices.push_back(surface_vertex_start);
			bucket.indices.push_back(surface_vertex_start + corner + 1);
			bucket.indices.push_back(surface_vertex_start + corner);
		}
	}

	// Preserve the existing ascending-material surface order while faces are
	// validated and bucketed only once.
	material_indices.sort();
	for (const int material_index : material_indices) {
		const int *bucket_index = bucket_by_material.getptr(material_index);
		if (!bucket_index || *bucket_index < 0 || *bucket_index >= buckets.size()) {
			continue;
		}
		const SurfaceBucket &bucket = buckets[*bucket_index];

		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = bucket.vertices;
		arrays[Mesh::ARRAY_NORMAL] = bucket.normals;
		arrays[Mesh::ARRAY_TEX_UV] = bucket.uv0;
		arrays[Mesh::ARRAY_COLOR] = bucket.colors;
		arrays[Mesh::ARRAY_INDEX] = bucket.indices;
		mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
		const int surface_index = mesh->get_surface_count() - 1;
		mesh->surface_set_name(surface_index, "Material " + itos(material_index));
		const Ref<Material> material = resolve_material_path(p_data->get_material_path(material_index));
		if (material.is_valid()) {
			mesh->surface_set_material(surface_index, material);
		}
	}

	return mesh;
}

PackedVector3Array LevelMeshBaker::bake_collision_faces(const Ref<LevelMeshData> &p_data) const {
	PackedVector3Array collision_faces;
	ERR_FAIL_COND_V(p_data.is_null(), collision_faces);

	for (int face_id = 0; face_id < p_data->face_alive.size(); face_id++) {
		if (!p_data->face_is_bakeable(face_id)) {
			continue;
		}
		const int loop_start = p_data->face_loop_starts[face_id];
		const int loop_count = p_data->face_loop_counts[face_id];
		const Vector3 &p0 = p_data->vertex_positions[p_data->loop_vertex_indices[loop_start]];
		// Same CCW-to-clockwise flip as the render bake so physics sees matching face normals.
		for (int corner = 1; corner < loop_count - 1; corner++) {
			collision_faces.push_back(p0);
			collision_faces.push_back(p_data->vertex_positions[p_data->loop_vertex_indices[loop_start + corner + 1]]);
			collision_faces.push_back(p_data->vertex_positions[p_data->loop_vertex_indices[loop_start + corner]]);
		}
	}

	return collision_faces;
}

void LevelMeshBaker::_bind_methods() {
	ClassDB::bind_method(D_METHOD("bake", "data"), &LevelMeshBaker::bake);
	ClassDB::bind_method(D_METHOD("bake_collision_faces", "data"), &LevelMeshBaker::bake_collision_faces);
}
