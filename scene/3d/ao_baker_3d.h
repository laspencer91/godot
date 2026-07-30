/**************************************************************************/
/*  ao_baker_3d.h                                                         */
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

#include "scene/3d/node_3d.h"

class Mesh;
class Material;
class MeshInstance3D;
class TextureLayered;

// Tools node that bakes a contextual ambient-occlusion mask (sampled on UV2) for weathering shaders.
// It drives the shared GPU lightmapper (LightmapperRD::bake_ao) -- a second, lightweight front-end to
// the same baker LightmapGI uses -- and produces per-mesh AO textures, NOT lighting. See
// lightmap-ao-planning/LIGHTMAP-AO-PLAN.md.
class AOBaker3D : public Node3D {
	GDCLASS(AOBaker3D, Node3D);

public:
	enum BakeError {
		BAKE_ERROR_OK,
		BAKE_ERROR_NO_MESHES,
		BAKE_ERROR_NO_LIGHTMAPPER,
		BAKE_ERROR_BAKE_FAILED,
		BAKE_ERROR_CANT_CREATE_DATA,
	};

private:
	int ao_ray_count = 128;
	float ao_max_distance = 0.75f;
	float ao_texel_scale = 0.5f;
	bool use_denoiser = true;
	float denoiser_strength = 0.1f;
	int denoiser_range = 10;
	float bias = 0.0005f;
	int max_texture_size = 4096;

	// Debug: if set, each per-mesh mask is also written as a PNG here on bake (so the AO can be eyeballed
	// in the FileSystem dock before any weathering shader exists). Not the real save path -- that's
	// Phase 2. Empty by default.
	String debug_output_directory;

	// Baked output, lightmap-style: ONE scene-space AO atlas shared by every baked mesh, plus a
	// per-mesh transform into it. Persisted with the scene. A weathering material samples the shared
	// atlas -- meshes do not each carry their own texture.
	Ref<TextureLayered> ao_atlas;
	// NodePath (relative to this node) -> Array{ Vector4 uv_rect (xy=offset, zw=scale), int slice }.
	// Pushed to each MeshInstance3D as instance shader parameters by apply_to_meshes().
	Dictionary ao_transforms;
	String last_bake_error_message;

	BakeError _bake_error(BakeError p_error, const String &p_message);

	struct MeshFound {
		Ref<Mesh> mesh;
		Transform3D xform;
		NodePath node_path;
		float lightmap_scale = 1.0f;
	};
	// Minimal mesh gather (the mesh half of LightmapGI::_find_meshes_and_lights): visible GI-static
	// MeshInstance3Ds whose every triangle surface has UV2 + normals. Deliberately duplicated rather
	// than shared, to avoid a merge-conflict surface in upstream lightmap_gi.cpp.
	void _find_meshes(Node *p_at_node, Vector<MeshFound> &r_meshes);

	// Editor helper walk (see get_bake_candidates).
	void _collect_candidates(Node *p_at_node, Vector<MeshInstance3D *> &r_ready, Vector<MeshInstance3D *> &r_missing_uv2) const;

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void set_ao_ray_count(int p_ao_ray_count);
	int get_ao_ray_count() const;
	void set_ao_max_distance(float p_distance);
	float get_ao_max_distance() const;
	void set_ao_texel_scale(float p_scale);
	float get_ao_texel_scale() const;
	void set_use_denoiser(bool p_enable);
	bool is_using_denoiser() const;
	void set_denoiser_strength(float p_strength);
	float get_denoiser_strength() const;
	void set_denoiser_range(int p_range);
	int get_denoiser_range() const;
	void set_bias(float p_bias);
	float get_bias() const;
	void set_max_texture_size(int p_size);
	int get_max_texture_size() const;
	void set_debug_output_directory(const String &p_dir);
	String get_debug_output_directory() const;

	void set_ao_atlas(const Ref<TextureLayered> &p_atlas);
	Ref<TextureLayered> get_ao_atlas() const;
	void set_ao_transforms(const Dictionary &p_transforms);
	Dictionary get_ao_transforms() const;
	String get_last_bake_error_message() const;

	// Deliver the bake to every baked mesh through the engine's per-instance AO-map channel
	// (RenderingServer::instance_geometry_set_ao_map): shared atlas + per-mesh UV rect + slice. A
	// shader reads the value via the AO_MAP built-in. Returns the number of meshes wired. Called at
	// the end of bake() and re-applied on scene load (the RS binding is not serialized).
	int apply_to_meshes();

	// Whole-level AO bake (synchronous). Gathers static meshes under this node's parent and bakes the
	// AO atlas via LightmapperRD::bake_ao. When p_atlas_path is non-empty, a tiled PNG is
	// imported there as a CompressedTexture2DArray before it is installed on this node. An empty path
	// preserves the programmatic/in-memory path used by tests and tools outside the editor plugin.
	BakeError bake(const String &p_atlas_path = String());

	// Editor-only helper for the pre-bake dialog: walks the same scope bake() uses and splits the
	// visible GI-static MeshInstance3Ds into those already carrying UV2 (r_ready, baked as-is) and
	// those missing it (r_missing_uv2, candidates for auto-unwrap). Unbound; the plugin classifies
	// the missing ones as fixable vs blocked.
	void get_bake_candidates(Vector<MeshInstance3D *> &r_ready, Vector<MeshInstance3D *> &r_missing_uv2) const;

	AOBaker3D();
};

VARIANT_ENUM_CAST(AOBaker3D::BakeError);
