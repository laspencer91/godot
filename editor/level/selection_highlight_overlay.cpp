/**************************************************************************/
/*  selection_highlight_overlay.cpp                                       */
/**************************************************************************/

#include "selection_highlight_overlay.h"

#include "core/object/callable_mp.h"
#include "editor/level/selection_model.h"
#include "servers/rendering/rendering_server.h"

#include "modules/level_kernel/level_block.h"
#include "modules/level_kernel/level_mesh.h"
#include "modules/level_kernel/level_mesh_data.h"

void SelectionHighlightOverlay::_selection_changed(const PackedInt64Array &p_dirty_blocks) {
	for (const int64_t block_id : p_dirty_blocks) {
		_rebuild_block(ObjectID((uint64_t)block_id));
	}
}

void SelectionHighlightOverlay::_create_block_render(ObjectID p_block_id) {
	if (block_renders.has(p_block_id)) {
		return;
	}
	BlockRender render;
	render.mesh.instantiate();
	RenderingServer *rs = RenderingServer::get_singleton();
	ERR_FAIL_NULL(rs);
	render.instance = rs->instance_create();
	rs->instance_set_base(render.instance, render.mesh->get_rid());
	rs->instance_set_layer_mask(render.instance, 1u << render_layer);
	rs->instance_geometry_set_cast_shadows_setting(render.instance, RSE::SHADOW_CASTING_SETTING_OFF);
	rs->instance_geometry_set_flag(render.instance, RSE::INSTANCE_FLAG_IGNORE_OCCLUSION_CULLING, true);
	rs->instance_geometry_set_flag(render.instance, RSE::INSTANCE_FLAG_USE_BAKED_LIGHT, false);
	rs->instance_set_visible(render.instance, false);
	block_renders[p_block_id] = render;
	callable_mp(this, &SelectionHighlightOverlay::_reconcile_block).bind((int64_t)(uint64_t)p_block_id).call_deferred();
}

void SelectionHighlightOverlay::_reconcile_block(int64_t p_block_id) {
	const ObjectID block_id = ObjectID((uint64_t)p_block_id);
	HashMap<ObjectID, BlockRender>::Iterator render = block_renders.find(block_id);
	if (!render || !render->value.instance.is_valid()) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!rs || !scenario.is_valid()) {
		return;
	}
	rs->instance_set_scenario(render->value.instance, scenario);
	render->value.attached = true;
	_sync_visibility(block_id);
}

void SelectionHighlightOverlay::_free_block_render(ObjectID p_block_id) {
	HashMap<ObjectID, BlockRender>::Iterator render = block_renders.find(p_block_id);
	if (!render) {
		return;
	}
	if (render->value.instance.is_valid()) {
		if (RenderingServer *rs = RenderingServer::get_singleton()) {
			rs->instance_set_scenario(render->value.instance, RID());
			rs->free_rid(render->value.instance);
		}
	}
	block_renders.erase(p_block_id);
}

void SelectionHighlightOverlay::_sync_visibility(ObjectID p_block_id) {
	HashMap<ObjectID, BlockRender>::Iterator render = block_renders.find(p_block_id);
	if (render && render->value.instance.is_valid() && RenderingServer::get_singleton()) {
		RenderingServer::get_singleton()->instance_set_visible(render->value.instance,
				render->value.attached && render->value.has_geometry && view_visible);
	}
}

bool SelectionHighlightOverlay::_append_triangle(const Ref<LevelMesh> &p_mesh, int p_face_id, int p_local_triangle, Vector<Vector3> &r_vertices) {
	if (p_mesh.is_null()) {
		return false;
	}
	const PackedInt32Array vertex_ids = p_mesh->get_face_triangle_vertex_ids(p_face_id, p_local_triangle);
	if (vertex_ids.size() != 3) {
		return false;
	}
	const PackedVector3Array positions = p_mesh->get_data()->get_vertex_positions();
	for (int corner = 0; corner < 3; corner++) {
		if (vertex_ids[corner] < 0 || vertex_ids[corner] >= positions.size()) {
			return false;
		}
	}
	r_vertices.push_back(positions[vertex_ids[0]]);
	r_vertices.push_back(positions[vertex_ids[1]]);
	r_vertices.push_back(positions[vertex_ids[2]]);
	return true;
}

void SelectionHighlightOverlay::_append_face_triangles(const Ref<LevelMesh> &p_mesh, int p_face_id, Vector<Vector3> &r_vertices) {
	if (p_mesh.is_null()) {
		return;
	}
	const PackedVector3Array corners = p_mesh->get_face_corner_positions(p_face_id);
	for (int triangle = 0; triangle < corners.size() - 2; triangle++) {
		r_vertices.push_back(corners[0]);
		r_vertices.push_back(corners[triangle + 1]);
		r_vertices.push_back(corners[triangle + 2]);
	}
}

void SelectionHighlightOverlay::_rebuild_block(ObjectID p_block_id) {
	if (!selection_model) {
		return;
	}
	if (!selection_model->is_block_tracked(p_block_id)) {
		_free_block_render(p_block_id);
		return;
	}
	Object *object = ObjectDB::get_instance(p_block_id);
	LevelBlock *block = Object::cast_to<LevelBlock>(object);
	if (!block || block->get_data().is_null()) {
		_free_block_render(p_block_id);
		return;
	}

	Vector<Vector3> passive_vertices;
	Vector<Vector3> vertices;
	Vector<Vector3> active_vertices;
	Vector<Vector3> passive_edges;
	Vector<Vector3> edges;
	Vector<Vector3> active_edges;
	Vector<Vector3> faces;
	Vector<Vector3> active_faces;
	const Ref<LevelMeshData> data = block->get_data();
	const PackedVector3Array positions = data->get_vertex_positions();
	const PackedByteArray vertex_alive = data->get_vertex_alive();
	const PackedInt32Array edge_vertices = data->get_edge_vertices();
	const PackedByteArray edge_alive = data->get_edge_alive();
	const PackedInt32Array face_starts = data->get_face_loop_starts();
	const PackedInt32Array face_counts = data->get_face_loop_counts();
	const PackedInt32Array loop_vertices = data->get_loop_vertex_indices();

	if (selection_model->get_mode() == SelectionModel::MODE_VERTEX) {
		for (int vertex_id = 0; vertex_id < positions.size() && vertex_id < vertex_alive.size(); vertex_id++) {
			if (vertex_alive[vertex_id] != 0) {
				passive_vertices.push_back(positions[vertex_id]);
			}
		}
	} else if (selection_model->get_mode() == SelectionModel::MODE_EDGE) {
		for (int edge_id = 0; edge_id < edge_alive.size(); edge_id++) {
			if (edge_alive[edge_id] == 0) {
				continue;
			}
			const int offset = edge_id * 2;
			if (offset < 0 || offset + 1 >= edge_vertices.size()) {
				continue;
			}
			const int vertex_a = edge_vertices[offset];
			const int vertex_b = edge_vertices[offset + 1];
			if (vertex_a >= 0 && vertex_a < positions.size() && vertex_b >= 0 && vertex_b < positions.size()) {
				passive_edges.push_back(positions[vertex_a]);
				passive_edges.push_back(positions[vertex_b]);
			}
		}
	}

	for (int feature = 0; feature < SelectionModel::FEATURE_MAX; feature++) {
		for (const SelectionModel::Element &element : selection_model->get_selected(SelectionModel::Feature(feature))) {
			if (element.block_id != p_block_id) {
				continue;
			}
			LevelBlock *resolved_block = nullptr;
			Ref<LevelMesh> mesh;
			int slot = -1;
			if (!selection_model->resolve(element, resolved_block, mesh, slot) || resolved_block != block) {
				continue;
			}
			const bool is_active = selection_model->is_active(element);
			switch (element.feature) {
				case SelectionModel::FEATURE_VERTEX: {
					if (slot >= 0 && slot < positions.size()) {
						(is_active ? active_vertices : vertices).push_back(positions[slot]);
					}
				} break;
				case SelectionModel::FEATURE_EDGE: {
					Vector<Vector3> &target = is_active ? active_edges : edges;
					if (element.handle_kind == SelectionModel::HANDLE_EDGE) {
						const int offset = slot * 2;
						if (offset >= 0 && offset + 1 < edge_vertices.size()) {
							const int vertex_a = edge_vertices[offset];
							const int vertex_b = edge_vertices[offset + 1];
							if (vertex_a >= 0 && vertex_a < positions.size() && vertex_b >= 0 && vertex_b < positions.size()) {
								target.push_back(positions[vertex_a]);
								target.push_back(positions[vertex_b]);
							}
						}
					} else if (element.handle_kind == SelectionModel::HANDLE_FACE && slot < face_starts.size() && slot < face_counts.size()) {
						int corner_a = -1;
						int corner_b = -1;
						const int start = face_starts[slot];
						const int count = face_counts[slot];
						if (SelectionModel::decode_corner_pair(element.sub_index, corner_a, corner_b) &&
								corner_a < count && corner_b < count && start >= 0 && start + count <= loop_vertices.size()) {
							const int vertex_a = loop_vertices[start + corner_a];
							const int vertex_b = loop_vertices[start + corner_b];
							if (vertex_a >= 0 && vertex_a < positions.size() && vertex_b >= 0 && vertex_b < positions.size()) {
								target.push_back(positions[vertex_a]);
								target.push_back(positions[vertex_b]);
							}
						}
					}
				} break;
				case SelectionModel::FEATURE_FACE: {
					Vector<Vector3> &target = is_active ? active_faces : faces;
					if (element.tier == SelectionModel::TIER_TRIANGLE && element.sub_index >= 0) {
						_append_triangle(mesh, slot, (int)element.sub_index, target);
					} else {
						_append_face_triangles(mesh, slot, target);
					}
				} break;
				default:
					break;
			}
		}
	}

	_create_block_render(p_block_id);
	HashMap<ObjectID, BlockRender>::Iterator render = block_renders.find(p_block_id);
	if (!render) {
		return;
	}
	render->value.mesh->clear_surfaces();
	auto append_surface = [&](Mesh::PrimitiveType p_primitive, const Ref<StandardMaterial3D> &p_material, const Vector<Vector3> &p_vertices) {
		if (p_vertices.is_empty()) {
			return;
		}
		render->value.mesh->surface_begin(p_primitive, p_material);
		for (const Vector3 &vertex : p_vertices) {
			render->value.mesh->surface_add_vertex(vertex);
		}
		render->value.mesh->surface_end();
	};
	append_surface(Mesh::PRIMITIVE_LINES, passive_edge_material, passive_edges);
	append_surface(Mesh::PRIMITIVE_POINTS, passive_vertex_material, passive_vertices);
	append_surface(Mesh::PRIMITIVE_TRIANGLES, face_material, faces);
	append_surface(Mesh::PRIMITIVE_TRIANGLES, active_face_material, active_faces);
	append_surface(Mesh::PRIMITIVE_LINES, edge_material, edges);
	append_surface(Mesh::PRIMITIVE_LINES, active_edge_material, active_edges);
	append_surface(Mesh::PRIMITIVE_POINTS, vertex_material, vertices);
	append_surface(Mesh::PRIMITIVE_POINTS, active_vertex_material, active_vertices);
	render->value.has_geometry = !passive_vertices.is_empty() || !passive_edges.is_empty() ||
			!vertices.is_empty() || !active_vertices.is_empty() || !edges.is_empty() ||
			!active_edges.is_empty() || !faces.is_empty() || !active_faces.is_empty();
	if (render->value.instance.is_valid() && RenderingServer::get_singleton()) {
		RenderingServer::get_singleton()->instance_set_transform(render->value.instance, block->get_global_transform());
	}
	_sync_visibility(p_block_id);
}

void SelectionHighlightOverlay::initialize(SelectionModel *p_selection_model, const RID &p_scenario, int p_render_layer) {
	ERR_FAIL_COND_MSG(selection_model != nullptr, "SelectionHighlightOverlay is already initialized.");
	ERR_FAIL_NULL(p_selection_model);
	ERR_FAIL_INDEX(p_render_layer, 32);
	selection_model = p_selection_model;
	scenario = p_scenario;
	render_layer = p_render_layer;
	selection_model->connect(SNAME("selection_changed"), callable_mp(this, &SelectionHighlightOverlay::_selection_changed));
	mark_all_dirty();
}

void SelectionHighlightOverlay::set_view_visible(bool p_visible) {
	view_visible = p_visible;
	for (const KeyValue<ObjectID, BlockRender> &entry : block_renders) {
		_sync_visibility(entry.key);
	}
}

void SelectionHighlightOverlay::set_face_highlight_dimmed(bool p_dimmed) {
	if (face_highlight_dimmed == p_dimmed) {
		return;
	}
	face_highlight_dimmed = p_dimmed;
	face_material->set_albedo(Color(0.18, 0.58, 1.0, p_dimmed ? 0.06 : 0.22));
	active_face_material->set_albedo(Color(1.0, 0.56, 0.10, p_dimmed ? 0.09 : 0.34));
}

void SelectionHighlightOverlay::mark_all_dirty() {
	if (!selection_model) {
		return;
	}
	for (const ObjectID &block_id : selection_model->get_tracked_block_ids()) {
		_rebuild_block(block_id);
	}
}

SelectionHighlightOverlay::SelectionHighlightOverlay() {
	auto configure_material = [](const Ref<StandardMaterial3D> &p_material, const Color &p_color, bool p_disable_depth_test = true) {
		p_material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
		p_material->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
		p_material->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
		p_material->set_flag(BaseMaterial3D::FLAG_DISABLE_DEPTH_TEST, p_disable_depth_test);
		p_material->set_albedo(p_color);
	};
	vertex_material.instantiate();
	active_vertex_material.instantiate();
	passive_vertex_material.instantiate();
	edge_material.instantiate();
	active_edge_material.instantiate();
	passive_edge_material.instantiate();
	face_material.instantiate();
	active_face_material.instantiate();
	configure_material(vertex_material, Color(0.30, 0.76, 1.0, 0.96));
	configure_material(active_vertex_material, Color(1.0, 0.72, 0.18, 1.0));
	configure_material(passive_vertex_material, Color(0.42, 0.78, 1.0, 0.52), false);
	configure_material(edge_material, Color(0.24, 0.70, 1.0, 0.92));
	configure_material(active_edge_material, Color(1.0, 0.64, 0.12, 1.0));
	configure_material(passive_edge_material, Color(0.38, 0.72, 1.0, 0.22), false);
	configure_material(face_material, Color(0.18, 0.58, 1.0, 0.22));
	configure_material(active_face_material, Color(1.0, 0.56, 0.10, 0.34));
	vertex_material->set_flag(BaseMaterial3D::FLAG_USE_POINT_SIZE, true);
	active_vertex_material->set_flag(BaseMaterial3D::FLAG_USE_POINT_SIZE, true);
	passive_vertex_material->set_flag(BaseMaterial3D::FLAG_USE_POINT_SIZE, true);
	vertex_material->set_point_size(9.0f);
	active_vertex_material->set_point_size(12.0f);
	passive_vertex_material->set_point_size(5.0f);
}

SelectionHighlightOverlay::~SelectionHighlightOverlay() {
	// Object teardown automatically removes inbound signal connections. The
	// document model may already be in PREDELETE during editor shutdown.
	Vector<ObjectID> block_ids;
	for (const KeyValue<ObjectID, BlockRender> &entry : block_renders) {
		block_ids.push_back(entry.key);
	}
	for (const ObjectID &block_id : block_ids) {
		_free_block_render(block_id);
	}
	selection_model = nullptr;
	scenario = RID();
}
