/**************************************************************************/
/*  selection_highlight_overlay.h                                         */
/**************************************************************************/
/*  G-Level S5: dirty-block stable-selection highlight renderer.          */
/**************************************************************************/

#pragma once

#include "core/object/object.h"
#include "core/templates/hash_map.h"
#include "core/templates/rid.h"
#include "scene/resources/immediate_mesh.h"
#include "scene/resources/material.h"

class LevelBlock;
class LevelMesh;
class SelectionModel;

class SelectionHighlightOverlay : public Object {
	struct BlockRender {
		Ref<ImmediateMesh> mesh;
		RID instance;
		bool attached = false;
		bool has_geometry = false;
	};

	SelectionModel *selection_model = nullptr; // Owned by LevelDocument.
	RID scenario;
	int render_layer = 20;
	bool view_visible = false;
	HashMap<ObjectID, BlockRender> block_renders;

	Ref<StandardMaterial3D> vertex_material;
	Ref<StandardMaterial3D> active_vertex_material;
	Ref<StandardMaterial3D> passive_vertex_material;
	Ref<StandardMaterial3D> edge_material;
	Ref<StandardMaterial3D> active_edge_material;
	Ref<StandardMaterial3D> passive_edge_material;
	Ref<StandardMaterial3D> face_material;
	Ref<StandardMaterial3D> active_face_material;
	bool face_highlight_dimmed = false;

	void _selection_changed(const PackedInt64Array &p_dirty_blocks);
	void _create_block_render(ObjectID p_block_id);
	void _reconcile_block(int64_t p_block_id);
	void _free_block_render(ObjectID p_block_id);
	void _rebuild_block(ObjectID p_block_id);
	void _sync_visibility(ObjectID p_block_id);
	static void _append_face_triangles(const Ref<LevelMesh> &p_mesh, int p_face_id, Vector<Vector3> &r_vertices);
	static bool _append_triangle(const Ref<LevelMesh> &p_mesh, int p_face_id, int p_local_triangle, Vector<Vector3> &r_vertices);

public:
	void initialize(SelectionModel *p_selection_model, const RID &p_scenario, int p_render_layer);
	void set_view_visible(bool p_visible);
	void set_face_highlight_dimmed(bool p_dimmed);
	void mark_all_dirty();

	SelectionHighlightOverlay();
	~SelectionHighlightOverlay();
};
