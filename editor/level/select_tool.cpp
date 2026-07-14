/**************************************************************************/
/*  select_tool.cpp                                                       */
/**************************************************************************/

#include "select_tool.h"

#include "core/object/object.h"
#include "core/templates/hash_set.h"
#include "editor/editor_data.h"
#include "editor/editor_document.h"
#include "editor/level/level_editor_view.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "scene/3d/camera_3d.h"

#include "modules/level_kernel/level_block.h"
#include "modules/level_kernel/level_mesh.h"
#include "modules/level_kernel/level_mesh_adjacency.h"
#include "modules/level_kernel/level_mesh_data.h"

namespace {

template <typename T>
void append_unique(Vector<T> &r_values, const T &p_value) {
	if (!r_values.has(p_value)) {
		r_values.push_back(p_value);
	}
}

} // namespace

bool SelectTool::SurfaceHit::operator<(const SurfaceHit &p_other) const {
	if (!Math::is_equal_approx(t, p_other.t)) {
		return t < p_other.t;
	}
	const uint64_t block_id = block ? (uint64_t)block->get_instance_id() : 0;
	const uint64_t other_block_id = p_other.block ? (uint64_t)p_other.block->get_instance_id() : 0;
	if (block_id != other_block_id) {
		return block_id < other_block_id;
	}
	if (face_id != p_other.face_id) {
		return face_id < p_other.face_id;
	}
	return triangle_id < p_other.triangle_id;
}

bool SelectTool::ScreenCandidate::operator<(const ScreenCandidate &p_other) const {
	if (!Math::is_equal_approx(pixel_distance, p_other.pixel_distance)) {
		return pixel_distance < p_other.pixel_distance;
	}
	if (!Math::is_equal_approx(depth, p_other.depth)) {
		return depth < p_other.depth;
	}
	if (element.block_id != p_other.element.block_id) {
		return (uint64_t)element.block_id < (uint64_t)p_other.element.block_id;
	}
	if (element.handle != p_other.element.handle) {
		return element.handle < p_other.element.handle;
	}
	return element.sub_index < p_other.element.sub_index;
}

SelectionModel *SelectTool::get_selection_model() const {
	LevelEditorView *level_view = get_view();
	LevelDocument *document = level_view ? level_view->get_level_document() : nullptr;
	return document ? document->get_selection_model() : nullptr;
}

SelectionModel::Operation SelectTool::_operation_from_modifiers(const Ref<InputEventWithModifiers> &p_event) {
	if (p_event.is_null()) {
		return SelectionModel::OP_REPLACE;
	}
	if (p_event->is_shift_pressed() && p_event->is_ctrl_pressed()) {
		return SelectionModel::OP_SUBTRACT;
	}
	if (p_event->is_shift_pressed()) {
		return SelectionModel::OP_ADD;
	}
	if (p_event->is_ctrl_pressed()) {
		return SelectionModel::OP_TOGGLE;
	}
	return SelectionModel::OP_REPLACE;
}

SelectionModel::Feature SelectTool::_feature_for_mode(SelectionModel::Mode p_mode) {
	switch (p_mode) {
		case SelectionModel::MODE_VERTEX:
			return SelectionModel::FEATURE_VERTEX;
		case SelectionModel::MODE_EDGE:
			return SelectionModel::FEATURE_EDGE;
		case SelectionModel::MODE_FACE:
		default:
			return SelectionModel::FEATURE_FACE;
	}
}

real_t SelectTool::_point_segment_distance(const Vector2 &p_point, const Vector2 &p_a, const Vector2 &p_b, real_t *r_segment_t) {
	const Vector2 segment = p_b - p_a;
	const real_t length_squared = segment.length_squared();
	real_t segment_t = 0.0;
	if (length_squared > CMP_EPSILON2) {
		segment_t = CLAMP((p_point - p_a).dot(segment) / length_squared, (real_t)0.0, (real_t)1.0);
	}
	if (r_segment_t) {
		*r_segment_t = segment_t;
	}
	return p_point.distance_to(p_a + segment * segment_t);
}

bool SelectTool::_rect_contains_projected(Camera3D *p_camera, const Rect2 &p_rect, const Vector3 &p_world_position) {
	return p_camera && !p_camera->is_position_behind(p_world_position) && p_rect.has_point(p_camera->unproject_position(p_world_position));
}

bool SelectTool::_get_face_loop(const Ref<LevelMeshData> &p_data, int p_face_id, int &r_start, int &r_count) {
	if (p_data.is_null()) {
		return false;
	}
	const PackedByteArray face_alive = p_data->get_face_alive();
	const PackedInt32Array starts = p_data->get_face_loop_starts();
	const PackedInt32Array counts = p_data->get_face_loop_counts();
	const PackedInt32Array loops = p_data->get_loop_vertex_indices();
	if (p_face_id < 0 || p_face_id >= face_alive.size() || face_alive[p_face_id] == 0 ||
			p_face_id >= starts.size() || p_face_id >= counts.size()) {
		return false;
	}
	r_start = starts[p_face_id];
	r_count = counts[p_face_id];
	return r_count >= 3 && r_start >= 0 && r_start + r_count <= loops.size();
}

int SelectTool::_find_face_corner(const Ref<LevelMeshData> &p_data, int p_face_id, int p_vertex_id) {
	int start = 0;
	int count = 0;
	if (!_get_face_loop(p_data, p_face_id, start, count)) {
		return -1;
	}
	const PackedInt32Array loop_vertices = p_data->get_loop_vertex_indices();
	for (int corner = 0; corner < count; corner++) {
		if (loop_vertices[start + corner] == p_vertex_id) {
			return corner;
		}
	}
	return -1;
}

void SelectTool::_set_mode(SelectionModel::Mode p_mode, SelectionModel::Tier p_tier) {
	SelectionModel *selection_model = get_selection_model();
	LevelEditorView *level_view = get_view();
	LevelDocument *document = level_view ? level_view->get_level_document() : nullptr;
	if (!selection_model || !document || (selection_model->get_mode() == p_mode && selection_model->get_tier() == p_tier)) {
		return;
	}
	selection_model->clear();
	if (EditorSelection *node_selection = document->get_selection()) {
		node_selection->clear();
		node_selection->update();
	}
	selection_model->set_mode_and_tier(p_mode, p_tier);
	level_view->set_last_selection_action(SNAME("mode"));
}

bool SelectTool::_is_xray_enabled() const {
	SelectionModel *selection_model = get_selection_model();
	return selection_model && xray_enabled[int(selection_model->get_tier())];
}

Vector<SelectTool::SurfaceHit> SelectTool::_query_surface_hits(Camera3D *p_camera, const Vector2 &p_position) const {
	Vector<SurfaceHit> hits;
	LevelEditorView *level_view = get_view();
	LevelDocument *document = level_view ? level_view->get_level_document() : nullptr;
	Node3DEditor *node_3d_editor = Node3DEditor::get_singleton();
	if (!p_camera || !document || !document->get_root() || !node_3d_editor) {
		return hits;
	}
	const Vector3 ray_origin = p_camera->project_ray_origin(p_position);
	const Vector3 ray_direction = p_camera->project_ray_normal(p_position).normalized();
	const Vector<Node3D *> broad_candidates = node_3d_editor->gizmo_bvh_ray_query(
			ray_origin, ray_origin + ray_direction * p_camera->get_far(), document->get_world_3d());
	HashSet<ObjectID> visited;
	for (Node3D *candidate : broad_candidates) {
		LevelBlock *block = Object::cast_to<LevelBlock>(candidate);
		if (!block || visited.has(block->get_instance_id()) || !document->get_root()->is_ancestor_of(block) || block->get_data().is_null()) {
			continue;
		}
		visited.insert(block->get_instance_id());
		const Transform3D inverse = block->get_global_transform().affine_inverse();
		const Ref<LevelMesh> mesh = block->get_level_mesh();
		const Dictionary result = mesh->ray_closest(inverse.xform(ray_origin), inverse.basis.xform(ray_direction));
		if (!(bool)result.get("hit", false)) {
			continue;
		}
		SurfaceHit hit;
		hit.block = block;
		hit.t = result.get("t", Math::INF);
		hit.face_id = result.get("face_id", -1);
		hit.triangle_id = result.get("tri_id", -1);
		hit.local_triangle = result.get("local_tri", -1);
		if (hit.t >= 0.0 && hit.t <= p_camera->get_far() && hit.face_id >= 0) {
			hits.push_back(hit);
		}
	}
	hits.sort();
	return hits;
}

SelectionModel::Element SelectTool::_make_element(LevelBlock *p_block, SelectionModel::Feature p_feature,
		SelectionModel::HandleKind p_handle_kind, int64_t p_handle, int64_t p_sub_index) const {
	SelectionModel::Element element;
	element.block_id = p_block ? p_block->get_instance_id() : ObjectID();
	element.handle = p_handle;
	element.sub_index = p_sub_index;
	element.feature = p_feature;
	element.tier = get_selection_model() ? get_selection_model()->get_tier() : SelectionModel::TIER_POLYGROUP;
	element.handle_kind = p_handle_kind;
	return element;
}

Vector<SelectionModel::Element> SelectTool::_resolve_face(const SurfaceHit &p_hit, bool p_expand_polygroup) const {
	Vector<SelectionModel::Element> elements;
	SelectionModel *selection_model = get_selection_model();
	if (!selection_model || !p_hit.block || p_hit.block->get_data().is_null()) {
		return elements;
	}
	const Ref<LevelMesh> mesh = p_hit.block->get_level_mesh();
	const int64_t seed_handle = mesh->make_face_handle(p_hit.face_id);
	if (seed_handle == -1) {
		return elements;
	}
	if (selection_model->get_tier() == SelectionModel::TIER_TRIANGLE) {
		if (p_hit.local_triangle >= 0) {
			elements.push_back(_make_element(p_hit.block, SelectionModel::FEATURE_FACE,
					SelectionModel::HANDLE_FACE, seed_handle, p_hit.local_triangle));
		}
		return elements;
	}

	if (!p_expand_polygroup) {
		elements.push_back(_make_element(p_hit.block, SelectionModel::FEATURE_FACE,
				SelectionModel::HANDLE_FACE, seed_handle));
		return elements;
	}
	for (const int face_id : mesh->get_adjacency()->get_polygroup_faces(p_hit.face_id)) {
		if (face_id == p_hit.face_id) {
			continue;
		}
		const int64_t handle = mesh->make_face_handle(face_id);
		if (handle != -1) {
			elements.push_back(_make_element(p_hit.block, SelectionModel::FEATURE_FACE,
					SelectionModel::HANDLE_FACE, handle));
		}
	}
	// The picked face is appended last so it becomes the active member of the expansion.
	elements.push_back(_make_element(p_hit.block, SelectionModel::FEATURE_FACE,
			SelectionModel::HANDLE_FACE, seed_handle));
	return elements;
}

Vector<SelectionModel::Element> SelectTool::_resolve_edge(Camera3D *p_camera, const Vector2 &p_position, const SurfaceHit &p_hit) const {
	Vector<SelectionModel::Element> result;
	SelectionModel *selection_model = get_selection_model();
	if (!selection_model || !p_camera || !p_hit.block || p_hit.block->get_data().is_null()) {
		return result;
	}
	const Ref<LevelMesh> mesh = p_hit.block->get_level_mesh();
	const Ref<LevelMeshData> data = p_hit.block->get_data();
	const PackedVector3Array positions = data->get_vertex_positions();
	const PackedInt32Array edge_vertices = data->get_edge_vertices();
	Vector<ScreenCandidate> candidates;
	const Vector3 ray_origin = p_camera->project_ray_origin(p_position);
	const Vector3 ray_direction = p_camera->project_ray_normal(p_position).normalized();
	const real_t depth_bias = MAX((real_t)0.0001, (real_t)0.001 * p_hit.t);

	auto append_candidate = [&](const SelectionModel::Element &p_element, int p_vertex_a, int p_vertex_b) {
		if (p_vertex_a < 0 || p_vertex_a >= positions.size() || p_vertex_b < 0 || p_vertex_b >= positions.size()) {
			return;
		}
		const Vector3 world_a = p_hit.block->get_global_transform().xform(positions[p_vertex_a]);
		const Vector3 world_b = p_hit.block->get_global_transform().xform(positions[p_vertex_b]);
		if (p_camera->is_position_behind(world_a) && p_camera->is_position_behind(world_b)) {
			return;
		}
		real_t segment_t = 0.0;
		const real_t pixel_distance = _point_segment_distance(p_position,
				p_camera->unproject_position(world_a), p_camera->unproject_position(world_b), &segment_t);
		if (pixel_distance > EDGE_TOLERANCE_PX) {
			return;
		}
		const Vector3 world_point = world_a.lerp(world_b, segment_t);
		const real_t depth = (world_point - ray_origin).dot(ray_direction);
		if (!_is_xray_enabled() && depth > p_hit.t + depth_bias) {
			return;
		}
		ScreenCandidate candidate;
		candidate.element = p_element;
		candidate.pixel_distance = pixel_distance;
		candidate.depth = depth;
		candidates.push_back(candidate);
	};

	if (selection_model->get_tier() == SelectionModel::TIER_POLYGROUP) {
		PackedInt32Array edge_ids = mesh->get_face_boundary_edge_ids(p_hit.face_id, true);
		// A closed polygroup has no topological outer boundary (box faces are one
		// group each, but future merge/paint ops can still close a group). Keep
		// edge mode usable by falling back to the hit face loop in that case.
		if (edge_ids.is_empty()) {
			edge_ids = mesh->get_face_boundary_edge_ids(p_hit.face_id, false);
		}
		for (const int edge_id : edge_ids) {
			const int offset = edge_id * 2;
			const int64_t handle = mesh->make_edge_handle(edge_id);
			if (handle != -1 && offset >= 0 && offset + 1 < edge_vertices.size()) {
				append_candidate(_make_element(p_hit.block, SelectionModel::FEATURE_EDGE,
										 SelectionModel::HANDLE_EDGE, handle),
						edge_vertices[offset], edge_vertices[offset + 1]);
			}
		}
	} else {
		const PackedInt32Array triangle_vertices = mesh->get_face_triangle_vertex_ids(p_hit.face_id, p_hit.local_triangle);
		if (triangle_vertices.size() == 3) {
			Ref<LevelMeshAdjacency> adjacency = mesh->get_adjacency();
			for (int edge = 0; edge < 3; edge++) {
				const int vertex_a = triangle_vertices[edge];
				const int vertex_b = triangle_vertices[(edge + 1) % 3];
				const int edge_id = adjacency->find_edge(vertex_a, vertex_b);
				if (edge_id >= 0) {
					const int64_t handle = mesh->make_edge_handle(edge_id);
					if (handle != -1) {
						append_candidate(_make_element(p_hit.block, SelectionModel::FEATURE_EDGE,
												 SelectionModel::HANDLE_EDGE, handle),
								vertex_a, vertex_b);
					}
				} else {
					const int corner_a = _find_face_corner(data, p_hit.face_id, vertex_a);
					const int corner_b = _find_face_corner(data, p_hit.face_id, vertex_b);
					const int64_t face_handle = mesh->make_face_handle(p_hit.face_id);
					const int64_t corner_pair = SelectionModel::encode_corner_pair(corner_a, corner_b);
					if (face_handle != -1 && corner_pair >= 0) {
						append_candidate(_make_element(p_hit.block, SelectionModel::FEATURE_EDGE,
												 SelectionModel::HANDLE_FACE, face_handle, corner_pair),
								vertex_a, vertex_b);
					}
				}
			}
		}
	}

	if (!candidates.is_empty()) {
		candidates.sort();
		result.push_back(candidates[0].element);
	}
	return result;
}

Vector<SelectionModel::Element> SelectTool::_resolve_vertex(Camera3D *p_camera, const Vector2 &p_position, const Vector<SurfaceHit> &p_hits) const {
	Vector<SelectionModel::Element> result;
	if (!p_camera || p_hits.is_empty()) {
		return result;
	}
	Vector<ScreenCandidate> candidates;
	HashSet<ObjectID> visited_blocks;
	const Vector3 ray_origin = p_camera->project_ray_origin(p_position);
	const Vector3 ray_direction = p_camera->project_ray_normal(p_position).normalized();
	const real_t surface_depth = p_hits[0].t;
	const real_t depth_bias = MAX((real_t)0.0001, (real_t)0.001 * surface_depth);
	for (const SurfaceHit &hit : p_hits) {
		if (!hit.block || visited_blocks.has(hit.block->get_instance_id()) || hit.block->get_data().is_null()) {
			continue;
		}
		visited_blocks.insert(hit.block->get_instance_id());
		const Ref<LevelMesh> mesh = hit.block->get_level_mesh();
		const PackedVector3Array positions = hit.block->get_data()->get_vertex_positions();
		const PackedByteArray alive = hit.block->get_data()->get_vertex_alive();
		const int vertex_count = MIN(positions.size(), alive.size());
		for (int vertex_id = 0; vertex_id < vertex_count; vertex_id++) {
			if (alive[vertex_id] == 0) {
				continue;
			}
			const Vector3 world_position = hit.block->get_global_transform().xform(positions[vertex_id]);
			if (p_camera->is_position_behind(world_position)) {
				continue;
			}
			const real_t pixel_distance = p_position.distance_to(p_camera->unproject_position(world_position));
			if (pixel_distance > VERTEX_TOLERANCE_PX) {
				continue;
			}
			const real_t depth = (world_position - ray_origin).dot(ray_direction);
			if (!_is_xray_enabled() && depth > surface_depth + depth_bias) {
				continue;
			}
			const int64_t handle = mesh->make_vertex_handle(vertex_id);
			if (handle == -1) {
				continue;
			}
			ScreenCandidate candidate;
			candidate.element = _make_element(hit.block, SelectionModel::FEATURE_VERTEX,
					SelectionModel::HANDLE_VERTEX, handle);
			candidate.pixel_distance = pixel_distance;
			candidate.depth = depth;
			candidates.push_back(candidate);
		}
	}
	if (!candidates.is_empty()) {
		candidates.sort();
		result.push_back(candidates[0].element);
	}
	return result;
}

void SelectTool::_apply_elements(SelectionModel::Feature p_feature, const Vector<SelectionModel::Element> &p_elements,
		SelectionModel::Operation p_operation, const StringName &p_action) {
	SelectionModel *selection_model = get_selection_model();
	if (!selection_model) {
		return;
	}
	SelectionModel::SelectionOp operation;
	operation.feature = p_feature;
	operation.tier = selection_model->get_tier();
	operation.operation = p_operation;
	operation.elements = p_elements;
	selection_model->apply(operation);
	if (LevelEditorView *level_view = get_view()) {
		level_view->set_last_selection_action(p_action);
	}
}

void SelectTool::_apply_object(LevelBlock *p_block, SelectionModel::Operation p_operation) {
	LevelEditorView *level_view = get_view();
	LevelDocument *document = level_view ? level_view->get_level_document() : nullptr;
	EditorSelection *selection = document ? document->get_selection() : nullptr;
	if (!selection) {
		return;
	}
	if (p_operation == SelectionModel::OP_REPLACE) {
		selection->clear();
	}
	if (p_block) {
		switch (p_operation) {
			case SelectionModel::OP_REPLACE:
			case SelectionModel::OP_ADD:
				selection->add_node(p_block);
				break;
			case SelectionModel::OP_TOGGLE:
				if (selection->is_selected(p_block)) {
					selection->remove_node(p_block);
				} else {
					selection->add_node(p_block);
				}
				break;
			case SelectionModel::OP_SUBTRACT:
				if (selection->is_selected(p_block)) {
					selection->remove_node(p_block);
				}
				break;
		}
	}
	selection->update();
	level_view->set_last_selection_action(SNAME("object"));
}

void SelectTool::_pick_click(Camera3D *p_camera, const Vector2 &p_position, SelectionModel::Operation p_operation, bool p_double_click) {
	SelectionModel *selection_model = get_selection_model();
	if (!selection_model) {
		return;
	}
	const Vector<SurfaceHit> hits = _query_surface_hits(p_camera, p_position);
	if (selection_model->get_mode() == SelectionModel::MODE_OBJECT) {
		_apply_object(hits.is_empty() ? nullptr : hits[0].block, p_operation);
		return;
	}
	const SelectionModel::Feature feature = _feature_for_mode(selection_model->get_mode());
	if (hits.is_empty()) {
		_apply_elements(feature, Vector<SelectionModel::Element>(), p_operation, SNAME("miss"));
		return;
	}

	if (p_double_click && selection_model->get_mode() == SelectionModel::MODE_FACE) {
		Vector<SelectionModel::Element> elements;
		const Ref<LevelMesh> mesh = hits[0].block->get_level_mesh();
		const PackedInt32Array flood_faces = mesh->get_adjacency()->coplanar_flood_fill(hits[0].face_id);
		for (const int face_id : flood_faces) {
			const int64_t face_handle = mesh->make_face_handle(face_id);
			if (face_handle == -1) {
				continue;
			}
			if (selection_model->get_tier() == SelectionModel::TIER_TRIANGLE) {
				const int triangle_count = mesh->get_face_triangle_count(face_id);
				for (int triangle = 0; triangle < triangle_count; triangle++) {
					append_unique(elements, _make_element(hits[0].block, SelectionModel::FEATURE_FACE, SelectionModel::HANDLE_FACE, face_handle, triangle));
				}
			} else {
				SurfaceHit flood_hit = hits[0];
				flood_hit.face_id = face_id;
				for (const SelectionModel::Element &element : _resolve_face(flood_hit, true)) {
					append_unique(elements, element);
				}
			}
		}
		_apply_elements(SelectionModel::FEATURE_FACE, elements, p_operation, SNAME("flood"));
		return;
	}

	Vector<SelectionModel::Element> elements;
	switch (selection_model->get_mode()) {
		case SelectionModel::MODE_VERTEX:
			elements = _resolve_vertex(p_camera, p_position, hits);
			break;
		case SelectionModel::MODE_EDGE:
			elements = _resolve_edge(p_camera, p_position, hits[0]);
			break;
		case SelectionModel::MODE_FACE:
			elements = _resolve_face(hits[0], true);
			break;
		default:
			break;
	}
	_apply_elements(feature, elements, p_operation, SNAME("click"));
	if (p_double_click && selection_model->get_mode() == SelectionModel::MODE_EDGE && !elements.is_empty()) {
		_select_edge_walk(false);
	}
}

void SelectTool::_select_edge_walk(bool p_ring) {
	SelectionModel *selection_model = get_selection_model();
	if (!selection_model || selection_model->get_mode() != SelectionModel::MODE_EDGE) {
		return;
	}
	SelectionModel::Element active_element;
	if (!selection_model->get_active(SelectionModel::FEATURE_EDGE, active_element) || active_element.handle_kind != SelectionModel::HANDLE_EDGE) {
		return;
	}
	LevelBlock *block = nullptr;
	Ref<LevelMesh> mesh;
	int seed_edge = -1;
	if (!selection_model->resolve(active_element, block, mesh, seed_edge)) {
		return;
	}
	const PackedInt32Array walked = p_ring ? mesh->get_adjacency()->walk_edge_ring(seed_edge) : mesh->get_adjacency()->walk_edge_loop(seed_edge);
	// Both tiers use the raw quad-gated kernel walk. Lifting the walk to
	// polygroup boundaries only becomes meaningful once multi-face polygroups
	// exist (LE2 merge/paint); with one group per face it degenerates to
	// selecting whole face loops instead of an edge loop.
	Vector<SelectionModel::Element> elements;
	for (const int edge_id : walked) {
		if (edge_id == seed_edge) {
			continue;
		}
		const int64_t handle = mesh->make_edge_handle(edge_id);
		if (handle != -1) {
			elements.push_back(_make_element(block, SelectionModel::FEATURE_EDGE, SelectionModel::HANDLE_EDGE, handle));
		}
	}
	// Preserve the seed as the active edge for repeated loop/ring commands.
	elements.push_back(active_element);
	_apply_elements(SelectionModel::FEATURE_EDGE, elements, SelectionModel::OP_REPLACE,
			p_ring ? SNAME("ring") : SNAME("loop"));
}

Vector<SelectionModel::Element> SelectTool::_collect_all(SelectionModel::Feature p_feature) const {
	Vector<SelectionModel::Element> elements;
	SelectionModel *selection_model = get_selection_model();
	LevelEditorView *level_view = get_view();
	LevelDocument *document = level_view ? level_view->get_level_document() : nullptr;
	if (!selection_model || !document || !document->get_root()) {
		return elements;
	}
	Vector<Node *> stack;
	stack.push_back(document->get_root());
	while (!stack.is_empty()) {
		Node *node = stack[stack.size() - 1];
		stack.resize(stack.size() - 1);
		for (int child = 0; child < node->get_child_count(); child++) {
			stack.push_back(node->get_child(child));
		}
		LevelBlock *block = Object::cast_to<LevelBlock>(node);
		if (!block || block->get_data().is_null()) {
			continue;
		}
		const Ref<LevelMesh> mesh = block->get_level_mesh();
		const Ref<LevelMeshData> data = block->get_data();
		switch (p_feature) {
			case SelectionModel::FEATURE_VERTEX: {
				const PackedByteArray alive = data->get_vertex_alive();
				for (int vertex_id = 0; vertex_id < alive.size(); vertex_id++) {
					if (alive[vertex_id] != 0) {
						const int64_t handle = mesh->make_vertex_handle(vertex_id);
						if (handle != -1) {
							append_unique(elements, _make_element(block, p_feature, SelectionModel::HANDLE_VERTEX, handle));
						}
					}
				}
			} break;
			case SelectionModel::FEATURE_EDGE: {
				const PackedByteArray alive = data->get_edge_alive();
				Vector<int> edge_ids;
				if (selection_model->get_tier() == SelectionModel::TIER_POLYGROUP) {
					const PackedByteArray face_alive = data->get_face_alive();
					for (int face_id = 0; face_id < face_alive.size(); face_id++) {
						if (face_alive[face_id] == 0) {
							continue;
						}
						for (const int edge_id : mesh->get_adjacency()->get_polygroup_boundary_edges(face_id)) {
							append_unique(edge_ids, edge_id);
						}
					}
				}
				// Triangle tier skips the polygroup branch, so raw edges are its
				// primary set. An empty polygroup rim is only a defensive fallback.
				if (edge_ids.is_empty()) {
					for (int edge_id = 0; edge_id < alive.size(); edge_id++) {
						if (alive[edge_id] != 0) {
							append_unique(edge_ids, edge_id);
						}
					}
				}
				for (const int edge_id : edge_ids) {
					const int64_t handle = mesh->make_edge_handle(edge_id);
					if (handle != -1) {
						append_unique(elements, _make_element(block, p_feature, SelectionModel::HANDLE_EDGE, handle));
					}
				}
				if (selection_model->get_tier() == SelectionModel::TIER_TRIANGLE) {
					const PackedByteArray face_alive = data->get_face_alive();
					Ref<LevelMeshAdjacency> adjacency = mesh->get_adjacency();
					for (int face_id = 0; face_id < face_alive.size(); face_id++) {
						if (face_alive[face_id] == 0) {
							continue;
						}
						const int64_t face_handle = mesh->make_face_handle(face_id);
						const int triangle_count = mesh->get_face_triangle_count(face_id);
						for (int triangle = 0; triangle < triangle_count; triangle++) {
							const PackedInt32Array triangle_vertices = mesh->get_face_triangle_vertex_ids(face_id, triangle);
							if (triangle_vertices.size() != 3) {
								continue;
							}
							for (int edge = 0; edge < 3; edge++) {
								const int vertex_a = triangle_vertices[edge];
								const int vertex_b = triangle_vertices[(edge + 1) % 3];
								if (adjacency->find_edge(vertex_a, vertex_b) >= 0) {
									continue;
								}
								const int64_t corners = SelectionModel::encode_corner_pair(
										_find_face_corner(data, face_id, vertex_a), _find_face_corner(data, face_id, vertex_b));
								if (face_handle != -1 && corners >= 0) {
									append_unique(elements, _make_element(block, p_feature, SelectionModel::HANDLE_FACE, face_handle, corners));
								}
							}
						}
					}
				}
			} break;
			case SelectionModel::FEATURE_FACE: {
				const PackedByteArray alive = data->get_face_alive();
				for (int face_id = 0; face_id < alive.size(); face_id++) {
					if (alive[face_id] == 0) {
						continue;
					}
					const int64_t handle = mesh->make_face_handle(face_id);
					if (handle == -1) {
						continue;
					}
					if (selection_model->get_tier() == SelectionModel::TIER_TRIANGLE) {
						const int triangle_count = mesh->get_face_triangle_count(face_id);
						for (int triangle = 0; triangle < triangle_count; triangle++) {
							append_unique(elements, _make_element(block, p_feature, SelectionModel::HANDLE_FACE, handle, triangle));
						}
					} else {
						append_unique(elements, _make_element(block, p_feature, SelectionModel::HANDLE_FACE, handle));
					}
				}
			} break;
			default:
				break;
		}
	}
	return elements;
}

void SelectTool::_select_all(bool p_invert) {
	SelectionModel *selection_model = get_selection_model();
	if (!selection_model) {
		return;
	}
	if (selection_model->get_mode() == SelectionModel::MODE_OBJECT) {
		LevelEditorView *level_view = get_view();
		LevelDocument *document = level_view ? level_view->get_level_document() : nullptr;
		EditorSelection *node_selection = document ? document->get_selection() : nullptr;
		if (!document || !document->get_root() || !node_selection) {
			return;
		}
		Vector<LevelBlock *> target_blocks;
		Vector<Node *> stack;
		stack.push_back(document->get_root());
		while (!stack.is_empty()) {
			Node *node = stack[stack.size() - 1];
			stack.resize(stack.size() - 1);
			for (int child = 0; child < node->get_child_count(); child++) {
				stack.push_back(node->get_child(child));
			}
			if (LevelBlock *block = Object::cast_to<LevelBlock>(node)) {
				if (!p_invert || !node_selection->is_selected(block)) {
					target_blocks.push_back(block);
				}
			}
		}
		node_selection->clear();
		for (LevelBlock *block : target_blocks) {
			node_selection->add_node(block);
		}
		node_selection->update();
		level_view->set_last_selection_action(p_invert ? SNAME("invert") : SNAME("all"));
		return;
	}
	const SelectionModel::Feature feature = _feature_for_mode(selection_model->get_mode());
	Vector<SelectionModel::Element> elements = _collect_all(feature);
	if (p_invert) {
		Vector<SelectionModel::Element> inverted;
		const Vector<SelectionModel::Element> &current = selection_model->get_selected(feature);
		for (const SelectionModel::Element &candidate : elements) {
			bool selected = false;
			for (const SelectionModel::Element &existing : current) {
				if (candidate == existing) {
					selected = true;
					break;
				}
			}
			if (!selected) {
				inverted.push_back(candidate);
			}
		}
		elements = inverted;
	}
	_apply_elements(feature, elements, SelectionModel::OP_REPLACE, p_invert ? SNAME("invert") : SNAME("all"));
}

Vector<Plane> SelectTool::_build_screen_frustum(Camera3D *p_camera, const Rect2 &p_rect) const {
	Vector<Plane> frustum;
	if (!p_camera || !p_rect.has_area()) {
		return frustum;
	}
	const Vector2 min = p_rect.position;
	const Vector2 max = p_rect.get_end();
	const Vector2 screen_corners[4] = {
		min,
		Vector2(max.x, min.y),
		max,
		Vector2(min.x, max.y),
	};
	Vector3 points[4];
	const real_t z_offset = MAX((real_t)5.0, p_camera->get_near());
	for (int corner = 0; corner < 4; corner++) {
		points[corner] = p_camera->project_ray_origin(screen_corners[corner]) +
				p_camera->project_ray_normal(screen_corners[corner]).normalized() * z_offset;
	}
	const Vector3 camera_position = p_camera->get_global_position();
	for (int side = 0; side < 4; side++) {
		if (p_camera->get_projection() == Camera3D::PROJECTION_ORTHOGONAL) {
			frustum.push_back(Plane((points[side] - points[(side + 1) % 4]).normalized(), points[side]));
		} else {
			frustum.push_back(Plane(points[side], points[(side + 1) % 4], camera_position));
		}
	}
	const Vector3 camera_forward = -p_camera->get_global_transform().basis.get_column(2).normalized();
	Plane near_plane(-camera_forward, camera_position);
	near_plane.d -= p_camera->get_near();
	frustum.push_back(near_plane);
	Plane far_plane = -near_plane;
	far_plane.d += p_camera->get_far();
	frustum.push_back(far_plane);
	return frustum;
}

Vector<LevelBlock *> SelectTool::_query_marquee_blocks(Camera3D *p_camera, const Rect2 &p_rect) const {
	Vector<LevelBlock *> blocks;
	LevelEditorView *level_view = get_view();
	LevelDocument *document = level_view ? level_view->get_level_document() : nullptr;
	Node3DEditor *node_3d_editor = Node3DEditor::get_singleton();
	if (!document || !document->get_root() || !node_3d_editor) {
		return blocks;
	}
	const Vector<Plane> frustum = _build_screen_frustum(p_camera, p_rect);
	if (frustum.is_empty()) {
		return blocks;
	}
	const Vector<Node3D *> candidates = node_3d_editor->gizmo_bvh_frustum_query(frustum, document->get_world_3d());
	HashSet<ObjectID> visited;
	for (Node3D *candidate : candidates) {
		LevelBlock *block = Object::cast_to<LevelBlock>(candidate);
		if (block && !visited.has(block->get_instance_id()) && document->get_root()->is_ancestor_of(block)) {
			visited.insert(block->get_instance_id());
			blocks.push_back(block);
		}
	}
	return blocks;
}

void SelectTool::_apply_object_marquee(Camera3D *p_camera, const Rect2 &p_rect, const Vector<LevelBlock *> &p_blocks) {
	LevelEditorView *level_view = get_view();
	LevelDocument *document = level_view ? level_view->get_level_document() : nullptr;
	EditorSelection *selection = document ? document->get_selection() : nullptr;
	if (!selection) {
		return;
	}
	Vector<LevelBlock *> qualifying;
	for (LevelBlock *block : p_blocks) {
		if (!block || block->get_data().is_null()) {
			continue;
		}
		const PackedVector3Array positions = block->get_data()->get_vertex_positions();
		const PackedByteArray alive = block->get_data()->get_vertex_alive();
		bool has_vertex = false;
		bool enclosed = true;
		for (int vertex_id = 0; vertex_id < MIN(positions.size(), alive.size()); vertex_id++) {
			if (alive[vertex_id] == 0) {
				continue;
			}
			has_vertex = true;
			enclosed = enclosed && _rect_contains_projected(p_camera, p_rect, block->get_global_transform().xform(positions[vertex_id]));
		}
		if (has_vertex && enclosed) {
			qualifying.push_back(block);
		}
	}
	if (press_operation == SelectionModel::OP_REPLACE) {
		selection->clear();
	}
	for (LevelBlock *block : qualifying) {
		if (press_operation == SelectionModel::OP_SUBTRACT ||
				(press_operation == SelectionModel::OP_TOGGLE && selection->is_selected(block))) {
			if (selection->is_selected(block)) {
				selection->remove_node(block);
			}
		} else {
			selection->add_node(block);
		}
	}
	selection->update();
	level_view->set_last_selection_action(SNAME("marquee"));
}

void SelectTool::_apply_marquee(Camera3D *p_camera, const Rect2 &p_rect) {
	SelectionModel *selection_model = get_selection_model();
	if (!selection_model || !p_camera || !p_rect.has_area()) {
		return;
	}
	const Vector<LevelBlock *> blocks = _query_marquee_blocks(p_camera, p_rect);
	if (selection_model->get_mode() == SelectionModel::MODE_OBJECT) {
		_apply_object_marquee(p_camera, p_rect, blocks);
		return;
	}
	const SelectionModel::Feature feature = _feature_for_mode(selection_model->get_mode());
	Vector<SelectionModel::Element> elements;

	for (LevelBlock *block : blocks) {
		if (!block || block->get_data().is_null()) {
			continue;
		}
		const Ref<LevelMesh> mesh = block->get_level_mesh();
		const Ref<LevelMeshData> data = block->get_data();
		const PackedVector3Array positions = data->get_vertex_positions();
		const PackedByteArray vertex_alive = data->get_vertex_alive();
		const PackedInt32Array edge_vertices = data->get_edge_vertices();
		const PackedByteArray edge_alive = data->get_edge_alive();

		if (feature == SelectionModel::FEATURE_VERTEX) {
			for (int vertex_id = 0; vertex_id < MIN(positions.size(), vertex_alive.size()); vertex_id++) {
				if (vertex_alive[vertex_id] != 0 && _rect_contains_projected(p_camera, p_rect, block->get_global_transform().xform(positions[vertex_id]))) {
					const int64_t handle = mesh->make_vertex_handle(vertex_id);
					if (handle != -1) {
						append_unique(elements, _make_element(block, feature, SelectionModel::HANDLE_VERTEX, handle));
					}
				}
			}
			continue;
		}

		if (feature == SelectionModel::FEATURE_EDGE) {
			Vector<int> selectable_edge_ids;
			if (selection_model->get_tier() == SelectionModel::TIER_POLYGROUP) {
				const PackedByteArray face_alive = data->get_face_alive();
				for (int face_id = 0; face_id < face_alive.size(); face_id++) {
					if (face_alive[face_id] == 0) {
						continue;
					}
					for (const int edge_id : mesh->get_adjacency()->get_polygroup_boundary_edges(face_id)) {
						append_unique(selectable_edge_ids, edge_id);
					}
				}
			}
			// Triangle tier skips the polygroup branch, so raw edges are its
			// primary set. An empty polygroup rim is only a defensive fallback.
			if (selectable_edge_ids.is_empty()) {
				for (int edge_id = 0; edge_id < edge_alive.size(); edge_id++) {
					if (edge_alive[edge_id] != 0) {
						append_unique(selectable_edge_ids, edge_id);
					}
				}
			}
			for (const int edge_id : selectable_edge_ids) {
				const int offset = edge_id * 2;
				if (edge_id < 0 || edge_id >= edge_alive.size() || edge_alive[edge_id] == 0 || offset + 1 >= edge_vertices.size()) {
					continue;
				}
				const int vertex_a = edge_vertices[offset];
				const int vertex_b = edge_vertices[offset + 1];
				if (vertex_a < 0 || vertex_a >= positions.size() || vertex_b < 0 || vertex_b >= positions.size()) {
					continue;
				}
				if (_rect_contains_projected(p_camera, p_rect, block->get_global_transform().xform(positions[vertex_a])) &&
						_rect_contains_projected(p_camera, p_rect, block->get_global_transform().xform(positions[vertex_b]))) {
					const int64_t handle = mesh->make_edge_handle(edge_id);
					if (handle != -1) {
						append_unique(elements, _make_element(block, feature, SelectionModel::HANDLE_EDGE, handle));
					}
				}
			}
			if (selection_model->get_tier() == SelectionModel::TIER_TRIANGLE) {
				const PackedByteArray face_alive = data->get_face_alive();
				Ref<LevelMeshAdjacency> adjacency = mesh->get_adjacency();
				for (int face_id = 0; face_id < face_alive.size(); face_id++) {
					if (face_alive[face_id] == 0) {
						continue;
					}
					const int64_t face_handle = mesh->make_face_handle(face_id);
					const int triangle_count = mesh->get_face_triangle_count(face_id);
					for (int triangle = 0; triangle < triangle_count; triangle++) {
						const PackedInt32Array triangle_vertices = mesh->get_face_triangle_vertex_ids(face_id, triangle);
						if (triangle_vertices.size() != 3) {
							continue;
						}
						for (int edge = 0; edge < 3; edge++) {
							const int vertex_a = triangle_vertices[edge];
							const int vertex_b = triangle_vertices[(edge + 1) % 3];
							if (adjacency->find_edge(vertex_a, vertex_b) >= 0 ||
									!_rect_contains_projected(p_camera, p_rect, block->get_global_transform().xform(positions[vertex_a])) ||
									!_rect_contains_projected(p_camera, p_rect, block->get_global_transform().xform(positions[vertex_b]))) {
								continue;
							}
							const int64_t corners = SelectionModel::encode_corner_pair(
									_find_face_corner(data, face_id, vertex_a), _find_face_corner(data, face_id, vertex_b));
							if (face_handle != -1 && corners >= 0) {
								append_unique(elements, _make_element(block, feature, SelectionModel::HANDLE_FACE, face_handle, corners));
							}
						}
					}
				}
			}
			continue;
		}

		const PackedByteArray face_alive = data->get_face_alive();
		if (selection_model->get_tier() == SelectionModel::TIER_TRIANGLE) {
			for (int face_id = 0; face_id < face_alive.size(); face_id++) {
				if (face_alive[face_id] == 0) {
					continue;
				}
				const int64_t face_handle = mesh->make_face_handle(face_id);
				const int triangle_count = mesh->get_face_triangle_count(face_id);
				for (int triangle = 0; triangle < triangle_count; triangle++) {
					const PackedInt32Array triangle_vertices = mesh->get_face_triangle_vertex_ids(face_id, triangle);
					if (triangle_vertices.size() != 3) {
						continue;
					}
					bool enclosed = true;
					for (int corner = 0; corner < 3; corner++) {
						enclosed = enclosed && _rect_contains_projected(p_camera, p_rect, block->get_global_transform().xform(positions[triangle_vertices[corner]]));
					}
					if (enclosed && face_handle != -1) {
						append_unique(elements, _make_element(block, feature, SelectionModel::HANDLE_FACE, face_handle, triangle));
					}
				}
			}
		} else {
			Vector<int> qualifying_faces;
			for (int face_id = 0; face_id < face_alive.size(); face_id++) {
				int start = 0;
				int count = 0;
				if (face_alive[face_id] == 0 || !_get_face_loop(data, face_id, start, count)) {
					continue;
				}
				const PackedInt32Array loop_vertices = data->get_loop_vertex_indices();
				bool enclosed = true;
				for (int corner = 0; corner < count; corner++) {
					const int vertex_id = loop_vertices[start + corner];
					enclosed = enclosed && vertex_id >= 0 && vertex_id < positions.size() &&
							_rect_contains_projected(p_camera, p_rect, block->get_global_transform().xform(positions[vertex_id]));
				}
				if (enclosed) {
					for (const int polygroup_face : mesh->get_adjacency()->get_polygroup_faces(face_id)) {
						append_unique(qualifying_faces, polygroup_face);
					}
				}
			}
			qualifying_faces.sort();
			for (const int face_id : qualifying_faces) {
				const int64_t handle = mesh->make_face_handle(face_id);
				if (handle != -1) {
					append_unique(elements, _make_element(block, feature, SelectionModel::HANDLE_FACE, handle));
				}
			}
		}
	}
	_apply_elements(feature, elements, press_operation, SNAME("marquee"));
}

bool SelectTool::_handle_input(Camera3D *p_camera, const Ref<InputEvent> &p_event) {
	SelectionModel *selection_model = get_selection_model();
	if (!selection_model) {
		return false;
	}
	Ref<InputEventMouse> mouse_event = p_event;
	if (mouse_event.is_valid()) {
		pointer_position = mouse_event->get_position();
		pointer_position_valid = true;
	}
	Ref<InputEventKey> key = p_event;
	if (key.is_valid() && key->is_pressed() && !key->is_echo()) {
		const Key code = key->get_keycode() != Key::NONE ? key->get_keycode() : key->get_physical_keycode();
		const bool axis_key = code == Key::X || code == Key::Y || code == Key::Z;
		if (transform_active && axis_key) {
			if (key->get_modifiers_mask().is_empty()) {
				const int axis = code == Key::X ? Vector3::AXIS_X : (code == Key::Y ? Vector3::AXIS_Y : Vector3::AXIS_Z);
				return _cycle_transform_constraint(p_camera, axis);
			}
			return false;
		}
		if (code == Key::R && key->get_modifiers_mask().is_empty() && !pointer_down && !marquee_active &&
				!transform_candidate && !transform_active) {
			if (!pointer_position_valid && p_camera && p_camera->get_viewport()) {
				pointer_position = p_camera->get_viewport()->get_mouse_position();
				pointer_position_valid = true;
			}
			press_position = pointer_position;
			current_position = pointer_position;
			gesture_shift = false;
			gesture_ctrl = false;
			transform_committed = false;
			if (_begin_transform_drag(p_camera, TRANSFORM_DRAG_ROTATE)) {
				return true;
			}
			_reset_gesture();
			return false;
		}
		if (key->is_ctrl_pressed() && !key->is_alt_pressed() && code == Key::D) {
			return _duplicate_objects();
		}
		if (key->is_ctrl_pressed() && key->is_shift_pressed() && !key->is_alt_pressed() && code == Key::G) {
			return _vertices_to_grid();
		}
		if (!key->is_ctrl_pressed() && !key->is_alt_pressed() &&
				(code == Key::LEFT || code == Key::RIGHT || code == Key::UP || code == Key::DOWN ||
						code == Key::PAGEUP || code == Key::PAGEDOWN)) {
			return _nudge(p_camera, code);
		}
		if (code == Key::KEY_1 || code == Key::KEY_2 || code == Key::KEY_3) {
			const SelectionModel::Mode target_mode = code == Key::KEY_1 ? SelectionModel::MODE_VERTEX : (code == Key::KEY_2 ? SelectionModel::MODE_EDGE : SelectionModel::MODE_FACE);
			const SelectionModel::Tier target_tier = key->is_shift_pressed() && key->is_ctrl_pressed() ? SelectionModel::TIER_TRIANGLE : SelectionModel::TIER_POLYGROUP;
			_set_mode(target_mode, target_tier);
			return true;
		}
		if (code == Key::KEY_4 && !key->is_ctrl_pressed() && !key->is_alt_pressed()) {
			_set_mode(SelectionModel::MODE_OBJECT, selection_model->get_tier());
			return true;
		}
		if (code == Key::KEY_6 && !key->is_ctrl_pressed() && !key->is_alt_pressed()) {
			const SelectionModel::Tier target_tier = selection_model->get_tier() == SelectionModel::TIER_POLYGROUP ? SelectionModel::TIER_TRIANGLE : SelectionModel::TIER_POLYGROUP;
			_set_mode(selection_model->get_mode(), target_tier);
			return true;
		}
		if (key->is_ctrl_pressed() && !key->is_alt_pressed() && code == Key::A) {
			_select_all(false);
			return true;
		}
		if (key->is_ctrl_pressed() && !key->is_alt_pressed() && code == Key::I) {
			_select_all(true);
			return true;
		}
		if (!key->is_shift_pressed() && !key->is_ctrl_pressed() && !key->is_alt_pressed() && !key->is_meta_pressed() && code == Key::Z) {
			const int tier_index = int(selection_model->get_tier());
			xray_enabled[tier_index] = !xray_enabled[tier_index];
			if (LevelEditorView *level_view = get_view()) {
				level_view->set_meta(StringName("_level_selection_xray"), xray_enabled[tier_index]);
				level_view->set_last_selection_action(SNAME("xray"));
			}
			return true;
		}
		if (!key->is_ctrl_pressed() && !key->is_alt_pressed() && code == Key::L) {
			_select_edge_walk(false);
			return true;
		}
		if (!key->is_shift_pressed() && !key->is_ctrl_pressed() && !key->is_alt_pressed() && !key->is_meta_pressed() && code == Key::X) {
			_select_edge_walk(true);
			return true;
		}
	}

	Ref<InputEventMouseButton> button = p_event;
	if (button.is_valid() && button->get_button_index() == MouseButton::RIGHT && button->is_pressed() &&
			transform_active && _is_rotation_drag()) {
		_cancel_transform_drag();
		exit_gesture();
		if (LevelEditorView *level_view = get_view()) {
			level_view->set_last_selection_action(SNAME("transform_cancel"));
		}
		return true;
	}
	if (button.is_valid() && button->get_button_index() == MouseButton::LEFT && button->is_pressed() &&
			transform_active && _is_rotation_drag()) {
		current_position = button->get_position();
		_update_transform_drag(p_camera, current_position, button->is_ctrl_pressed());
		_commit_transform_drag();
		exit_gesture();
		return true;
	}
	if (button.is_valid() && button->get_button_index() == MouseButton::LEFT && !button->is_alt_pressed()) {
		if (button->is_pressed()) {
			pointer_down = true;
			marquee_active = false;
			transform_candidate = _press_hits_current_selection(p_camera, button->get_position());
			transform_active = false;
			transform_committed = false;
			gesture_shift = button->is_shift_pressed();
			gesture_ctrl = button->is_ctrl_pressed();
			press_was_double_click = button->is_double_click();
			press_position = button->get_position();
			current_position = press_position;
			press_operation = _operation_from_modifiers(button);
			return true;
		}
		if (!pointer_down) {
			return false;
		}
		current_position = button->get_position();
		if (transform_active) {
			_update_transform_drag(p_camera, current_position, button->is_ctrl_pressed());
			_commit_transform_drag();
		} else if (marquee_active) {
			_apply_marquee(p_camera, Rect2(press_position, current_position - press_position).abs());
		} else {
			_pick_click(p_camera, current_position, press_operation, press_was_double_click);
		}
		exit_gesture();
		return true;
	}

	Ref<InputEventMouseMotion> motion = p_event;
	if (motion.is_valid() && transform_active && _is_rotation_drag()) {
		current_position = motion->get_position();
		_update_transform_drag(p_camera, current_position, motion->is_ctrl_pressed());
		return true;
	}
	if (motion.is_valid() && pointer_down && motion->get_button_mask().has_flag(MouseButtonMask::LEFT)) {
		current_position = motion->get_position();
		if (transform_active) {
			_update_transform_drag(p_camera, current_position, motion->is_ctrl_pressed());
			return true;
		}
		if (!marquee_active && drag_started(press_position, current_position)) {
			if (transform_candidate && _begin_transform_drag(p_camera)) {
				_update_transform_drag(p_camera, current_position, motion->is_ctrl_pressed());
				return true;
			}
			transform_candidate = false;
			marquee_active = true;
		}
		if (marquee_active) {
			get_view()->set_marquee_rect(Rect2(press_position, current_position - press_position).abs(), true);
		}
		return true;
	}
	return false;
}

void SelectTool::_reset_gesture() {
	if (transform_active && !transform_committed) {
		_cancel_transform_drag();
	}
	pointer_down = false;
	marquee_active = false;
	transform_candidate = false;
	transform_active = false;
	transform_committed = false;
	gesture_shift = false;
	gesture_ctrl = false;
	transform_drag_mode = TRANSFORM_DRAG_NONE;
	_reset_transform_constraint();
	mesh_drag_states.clear();
	object_drag_states.clear();
	transform_drag_plane = Plane();
	transform_pivot = Vector3();
	transform_axis = Vector3();
	transform_press_point = Vector3();
	transform_press_axis_parameter = 0.0;
	transform_axis_screen_fallback = false;
	transform_view_axis = Vector3();
	transform_rotation_pivot_screen = Vector2();
	transform_rotation_press_vector = Vector2();
	transform_rotation_angle = 0.0;
	transform_rotation_reference_valid = false;
	transform_snap_step = LevelEditor::DEFAULT_SNAP_STEP;
	press_was_double_click = false;
	press_position = Vector2();
	current_position = Vector2();
	press_operation = SelectionModel::OP_REPLACE;
	if (LevelEditorView *level_view = get_view()) {
		level_view->set_marquee_rect(Rect2(), false);
	}
}

void SelectTool::_escape_pressed() {
	if (pointer_down || transform_active) {
		_cancel_transform_drag();
		exit_gesture();
		if (LevelEditorView *level_view = get_view()) {
			level_view->set_last_selection_action(SNAME("transform_cancel"));
		}
		return;
	}
	exit_gesture();
	if (SelectionModel *selection_model = get_selection_model()) {
		selection_model->clear();
	}
	LevelEditorView *level_view = get_view();
	LevelDocument *document = level_view ? level_view->get_level_document() : nullptr;
	if (document && document->get_selection()) {
		document->get_selection()->clear();
		document->get_selection()->update();
	}
	if (level_view) {
		level_view->set_last_selection_action(SNAME("escape"));
	}
}
