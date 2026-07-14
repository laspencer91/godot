/**************************************************************************/
/*  selection_model.cpp                                                   */
/**************************************************************************/

#include "selection_model.h"

#include "core/object/callable_mp.h"
#include "core/templates/hash_set.h"
#include "editor/editor_document.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"

#include "modules/level_kernel/level_block.h"
#include "modules/level_kernel/level_mesh.h"
#include "modules/level_kernel/level_mesh_data.h"
#include "modules/level_kernel/level_mesh_diff.h"

bool SelectionModel::_contains(const Vector<Element> &p_elements, const Element &p_element, int *r_index) {
	const int index = p_elements.find(p_element);
	if (index >= 0 && r_index) {
		*r_index = index;
	}
	return index >= 0;
}

void SelectionModel::_append_dirty(Vector<ObjectID> &r_dirty, ObjectID p_block_id) {
	if (p_block_id.is_null()) {
		return;
	}
	if (!r_dirty.has(p_block_id)) {
		r_dirty.push_back(p_block_id);
	}
}

void SelectionModel::_emit_changed(const Vector<ObjectID> &p_dirty_blocks) {
	PackedInt64Array dirty_blocks;
	for (const ObjectID &block_id : p_dirty_blocks) {
		dirty_blocks.push_back((int64_t)(uint64_t)block_id);
	}
	emit_signal(SNAME("selection_changed"), dirty_blocks);
}

void SelectionModel::_emit_all_selected_dirty() {
	Vector<ObjectID> dirty;
	for (int feature = 0; feature < FEATURE_MAX; feature++) {
		for (const Element &element : selected[feature]) {
			_append_dirty(dirty, element.block_id);
		}
	}
	_emit_changed(dirty);
}

void SelectionModel::_refresh_active(Feature p_feature) {
	ERR_FAIL_INDEX(int(p_feature), FEATURE_MAX);
	if (has_active[p_feature] && _contains(selected[p_feature], active[p_feature])) {
		return;
	}
	has_active[p_feature] = !selected[p_feature].is_empty();
	if (has_active[p_feature]) {
		active[p_feature] = selected[p_feature][selected[p_feature].size() - 1];
	} else {
		active[p_feature] = Element();
	}
}

bool SelectionModel::_belongs_to_document(LevelBlock *p_block) const {
	if (!document || !p_block) {
		return false;
	}
	Node *root = document->get_root();
	return root && (root == p_block || root->is_ancestor_of(p_block));
}

void SelectionModel::_scan_node(Node *p_node) {
	if (!p_node) {
		return;
	}
	if (LevelBlock *block = Object::cast_to<LevelBlock>(p_node)) {
		_track_block(block);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_scan_node(p_node->get_child(i));
	}
}

void SelectionModel::_node_added(Node *p_node) {
	LevelBlock *block = Object::cast_to<LevelBlock>(p_node);
	if (block && _belongs_to_document(block)) {
		_track_block(block);
	}
}

void SelectionModel::_node_removed(Node *p_node) {
	if (p_node && tracked_block_data.has(p_node->get_instance_id())) {
		_untrack_block(p_node->get_instance_id(), true);
	}
}

void SelectionModel::_track_block(LevelBlock *p_block) {
	if (!_belongs_to_document(p_block)) {
		return;
	}
	const ObjectID block_id = p_block->get_instance_id();
	const int64_t bound_id = (int64_t)(uint64_t)block_id;
	const Callable baked_callable = callable_mp(this, &SelectionModel::_block_baked).bind(bound_id);
	const Callable transform_callable = callable_mp(this, &SelectionModel::_block_transform_changed).bind(bound_id);
	if (!p_block->is_connected(SNAME("baked"), baked_callable)) {
		p_block->connect(SNAME("baked"), baked_callable);
	}
	p_block->set_notify_transform(true);
	if (!p_block->is_connected(SNAME("level_transform_changed"), transform_callable)) {
		p_block->connect(SNAME("level_transform_changed"), transform_callable);
	}

	Ref<LevelMeshData> new_data = p_block->get_data();
	HashMap<ObjectID, Ref<LevelMeshData>>::Iterator tracked = tracked_block_data.find(block_id);
	if (tracked && tracked->value == new_data) {
		return;
	}
	if (tracked && tracked->value.is_valid()) {
		const Callable changed_callable = callable_mp(this, &SelectionModel::_data_changed).bind(bound_id);
		const Callable diff_callable = callable_mp(this, &SelectionModel::_mesh_diff_applied).bind(bound_id);
		tracked->value->disconnect_changed(changed_callable);
		if (tracked->value->is_connected(SNAME("mesh_diff_applied"), diff_callable)) {
			tracked->value->disconnect(SNAME("mesh_diff_applied"), diff_callable);
		}
	}

	tracked_block_data[block_id] = new_data;
	if (new_data.is_valid()) {
		const Callable changed_callable = callable_mp(this, &SelectionModel::_data_changed).bind(bound_id);
		const Callable diff_callable = callable_mp(this, &SelectionModel::_mesh_diff_applied).bind(bound_id);
		new_data->connect_changed(changed_callable);
		new_data->connect(SNAME("mesh_diff_applied"), diff_callable);
	}
}

void SelectionModel::_untrack_block(ObjectID p_block_id, bool p_drop_selection) {
	HashMap<ObjectID, Ref<LevelMeshData>>::Iterator tracked = tracked_block_data.find(p_block_id);
	if (tracked) {
		const int64_t bound_id = (int64_t)(uint64_t)p_block_id;
		if (tracked->value.is_valid()) {
			const Callable changed_callable = callable_mp(this, &SelectionModel::_data_changed).bind(bound_id);
			const Callable diff_callable = callable_mp(this, &SelectionModel::_mesh_diff_applied).bind(bound_id);
			tracked->value->disconnect_changed(changed_callable);
			if (tracked->value->is_connected(SNAME("mesh_diff_applied"), diff_callable)) {
				tracked->value->disconnect(SNAME("mesh_diff_applied"), diff_callable);
			}
		}
		tracked_block_data.erase(p_block_id);
	}

	if (Object *object = ObjectDB::get_instance(p_block_id)) {
		if (LevelBlock *block = Object::cast_to<LevelBlock>(object)) {
			const int64_t bound_id = (int64_t)(uint64_t)p_block_id;
			const Callable baked_callable = callable_mp(this, &SelectionModel::_block_baked).bind(bound_id);
			const Callable transform_callable = callable_mp(this, &SelectionModel::_block_transform_changed).bind(bound_id);
			if (block->is_connected(SNAME("baked"), baked_callable)) {
				block->disconnect(SNAME("baked"), baked_callable);
			}
			if (block->is_connected(SNAME("level_transform_changed"), transform_callable)) {
				block->disconnect(SNAME("level_transform_changed"), transform_callable);
			}
		}
	}

	bool changed = false;
	if (p_drop_selection) {
		for (int feature = 0; feature < FEATURE_MAX; feature++) {
			for (int i = selected[feature].size() - 1; i >= 0; i--) {
				if (selected[feature][i].block_id == p_block_id) {
					selected[feature].remove_at(i);
					changed = true;
				}
			}
			_refresh_active(Feature(feature));
		}
	}
	if (changed) {
		revision++;
		Vector<ObjectID> dirty;
		dirty.push_back(p_block_id);
		_emit_changed(dirty);
	}
}

void SelectionModel::_block_baked(int64_t p_block_id) {
	const ObjectID block_id = ObjectID((uint64_t)p_block_id);
	if (Object *object = ObjectDB::get_instance(block_id)) {
		if (LevelBlock *block = Object::cast_to<LevelBlock>(object)) {
			_track_block(block);
			mark_block_dirty(block);
		}
	}
}

void SelectionModel::_block_transform_changed(int64_t p_block_id) {
	const ObjectID block_id = ObjectID((uint64_t)p_block_id);
	Vector<ObjectID> dirty;
	dirty.push_back(block_id);
	_emit_changed(dirty);
}

void SelectionModel::_data_changed(int64_t p_block_id) {
	Vector<ObjectID> dirty;
	dirty.push_back(ObjectID((uint64_t)p_block_id));
	_emit_changed(dirty);
}

void SelectionModel::_mesh_diff_applied(const Ref<LevelMeshDiff> &p_diff, bool p_reverted, int64_t p_block_id) {
	const ObjectID block_id = ObjectID((uint64_t)p_block_id);
	Object *object = ObjectDB::get_instance(block_id);
	LevelBlock *block = Object::cast_to<LevelBlock>(object);
	if (block) {
		revalidate(block, p_diff, p_reverted);
	}
}

bool SelectionModel::_drop_removed_handles(ObjectID p_block_id, const PackedInt64Array &p_vertex_handles,
		const PackedInt64Array &p_edge_handles, const PackedInt64Array &p_face_handles) {
	HashSet<int64_t> removed_vertices;
	HashSet<int64_t> removed_edges;
	HashSet<int64_t> removed_faces;
	for (const int64_t handle : p_vertex_handles) {
		removed_vertices.insert(handle);
	}
	for (const int64_t handle : p_edge_handles) {
		removed_edges.insert(handle);
	}
	for (const int64_t handle : p_face_handles) {
		removed_faces.insert(handle);
	}

	bool changed = false;
	for (int feature = 0; feature < FEATURE_MAX; feature++) {
		for (int i = selected[feature].size() - 1; i >= 0; i--) {
			const Element &element = selected[feature][i];
			if (element.block_id != p_block_id) {
				continue;
			}
			const bool removed = (element.handle_kind == HANDLE_VERTEX && removed_vertices.has(element.handle)) ||
					(element.handle_kind == HANDLE_EDGE && removed_edges.has(element.handle)) ||
					(element.handle_kind == HANDLE_FACE && removed_faces.has(element.handle));
			if (removed) {
				selected[feature].remove_at(i);
				changed = true;
			}
		}
		_refresh_active(Feature(feature));
	}
	return changed;
}

void SelectionModel::bind_document(LevelDocument *p_document) {
	ERR_FAIL_NULL(p_document);
	if (document == p_document && scene_tree) {
		return;
	}
	ERR_FAIL_COND_MSG(document && document != p_document, "SelectionModel cannot be rebound to another document.");
	document = p_document;
	Node *root = document->get_root();
	if (!root || !root->get_tree()) {
		return;
	}
	scene_tree = root->get_tree();
	const Callable added_callable = callable_mp(this, &SelectionModel::_node_added);
	const Callable removed_callable = callable_mp(this, &SelectionModel::_node_removed);
	if (!scene_tree->is_connected(SNAME("node_added"), added_callable)) {
		scene_tree->connect(SNAME("node_added"), added_callable);
	}
	if (!scene_tree->is_connected(SNAME("node_removed"), removed_callable)) {
		scene_tree->connect(SNAME("node_removed"), removed_callable);
	}
	_scan_node(root);
}

void SelectionModel::set_mode_and_tier(Mode p_mode, Tier p_tier) {
	ERR_FAIL_INDEX(int(p_mode), 4);
	ERR_FAIL_INDEX(int(p_tier), 2);
	if (mode == p_mode && tier == p_tier) {
		return;
	}
	mode = p_mode;
	tier = p_tier;
	_emit_all_selected_dirty();
}

void SelectionModel::apply(const SelectionOp &p_op) {
	ERR_FAIL_INDEX(int(p_op.feature), FEATURE_MAX);
	ERR_FAIL_INDEX(int(p_op.tier), 2);
	ERR_FAIL_INDEX(int(p_op.operation), 4);
	Vector<Element> incoming;
	for (const Element &element : p_op.elements) {
		if (element.feature == p_op.feature && !_contains(incoming, element)) {
			incoming.push_back(element);
		}
	}

	Vector<Element> &target = selected[p_op.feature];
	const bool old_has_active = has_active[p_op.feature];
	const Element old_active = active[p_op.feature];
	Vector<ObjectID> dirty;
	for (const Element &element : target) {
		_append_dirty(dirty, element.block_id);
	}
	for (const Element &element : incoming) {
		_append_dirty(dirty, element.block_id);
	}

	bool changed = false;
	switch (p_op.operation) {
		case OP_REPLACE: {
			if (target.size() != incoming.size()) {
				changed = true;
			} else {
				for (int i = 0; i < target.size(); i++) {
					if (!(target[i] == incoming[i])) {
						changed = true;
						break;
					}
				}
			}
			target = incoming;
			has_active[p_op.feature] = !target.is_empty();
			if (has_active[p_op.feature]) {
				active[p_op.feature] = target[target.size() - 1];
			}
		} break;
		case OP_ADD: {
			for (const Element &element : incoming) {
				if (!_contains(target, element)) {
					target.push_back(element);
					changed = true;
				}
				active[p_op.feature] = element;
				has_active[p_op.feature] = true;
			}
		} break;
		case OP_TOGGLE: {
			for (const Element &element : incoming) {
				int index = -1;
				if (_contains(target, element, &index)) {
					target.remove_at(index);
				} else {
					target.push_back(element);
					active[p_op.feature] = element;
					has_active[p_op.feature] = true;
				}
				changed = true;
			}
			_refresh_active(p_op.feature);
		} break;
		case OP_SUBTRACT: {
			for (const Element &element : incoming) {
				int index = -1;
				if (_contains(target, element, &index)) {
					target.remove_at(index);
					changed = true;
				}
			}
			_refresh_active(p_op.feature);
		} break;
	}

	const bool active_changed = old_has_active != has_active[p_op.feature] ||
			(old_has_active && has_active[p_op.feature] && !(old_active == active[p_op.feature]));
	if (changed || active_changed) {
		revision++;
		_emit_changed(dirty);
	}
}

void SelectionModel::clear() {
	Vector<ObjectID> dirty;
	bool changed = false;
	for (int feature = 0; feature < FEATURE_MAX; feature++) {
		for (const Element &element : selected[feature]) {
			_append_dirty(dirty, element.block_id);
		}
		changed = changed || !selected[feature].is_empty();
		selected[feature].clear();
		has_active[feature] = false;
		active[feature] = Element();
	}
	if (changed) {
		revision++;
		_emit_changed(dirty);
	}
}

void SelectionModel::revalidate(LevelBlock *p_block, const Ref<LevelMeshDiff> &p_diff, bool p_reverted) {
	ERR_FAIL_NULL(p_block);
	ERR_FAIL_COND(p_diff.is_null());
	const PackedInt64Array vertices = p_reverted ? p_diff->get_revert_removed_vertex_handles() : p_diff->get_removed_vertex_handles();
	const PackedInt64Array edges = p_reverted ? p_diff->get_revert_removed_edge_handles() : p_diff->get_removed_edge_handles();
	const PackedInt64Array faces = p_reverted ? p_diff->get_revert_removed_face_handles() : p_diff->get_removed_face_handles();
	if (_drop_removed_handles(p_block->get_instance_id(), vertices, edges, faces)) {
		revision++;
	}
	Vector<ObjectID> dirty;
	dirty.push_back(p_block->get_instance_id());
	_emit_changed(dirty);
}

void SelectionModel::mark_block_dirty(LevelBlock *p_block) {
	ERR_FAIL_NULL(p_block);
	Vector<ObjectID> dirty;
	dirty.push_back(p_block->get_instance_id());
	_emit_changed(dirty);
}

const Vector<SelectionModel::Element> &SelectionModel::get_selected(Feature p_feature) const {
	ERR_FAIL_INDEX_V(int(p_feature), FEATURE_MAX, selected[0]);
	return selected[p_feature];
}

int SelectionModel::get_count(Feature p_feature) const {
	ERR_FAIL_INDEX_V(int(p_feature), FEATURE_MAX, 0);
	return selected[p_feature].size();
}

bool SelectionModel::get_active(Feature p_feature, Element &r_element) const {
	ERR_FAIL_INDEX_V(int(p_feature), FEATURE_MAX, false);
	if (!has_active[p_feature]) {
		return false;
	}
	r_element = active[p_feature];
	return true;
}

bool SelectionModel::is_active(const Element &p_element) const {
	return p_element.feature >= FEATURE_VERTEX && p_element.feature < FEATURE_MAX &&
			has_active[p_element.feature] && active[p_element.feature] == p_element;
}

bool SelectionModel::resolve(const Element &p_element, LevelBlock *&r_block, Ref<LevelMesh> &r_mesh, int &r_slot) const {
	r_block = nullptr;
	r_mesh.unref();
	r_slot = -1;
	Object *object = ObjectDB::get_instance(p_element.block_id);
	LevelBlock *block = Object::cast_to<LevelBlock>(object);
	if (!block || !_belongs_to_document(block) || block->get_data().is_null()) {
		return false;
	}
	const Ref<LevelMesh> mesh = block->get_level_mesh();
	switch (p_element.handle_kind) {
		case HANDLE_VERTEX:
			r_slot = mesh->resolve_vertex(p_element.handle);
			break;
		case HANDLE_EDGE:
			r_slot = mesh->resolve_edge(p_element.handle);
			break;
		case HANDLE_FACE:
			r_slot = mesh->resolve_face(p_element.handle);
			break;
	}
	if (r_slot < 0) {
		return false;
	}
	r_block = block;
	r_mesh = mesh;
	return true;
}

Array SelectionModel::get_debug_entries() const {
	Array entries;
	for (int feature = 0; feature < FEATURE_MAX; feature++) {
		for (const Element &element : selected[feature]) {
			Dictionary entry;
			entry["block_id"] = (int64_t)(uint64_t)element.block_id;
			entry["handle"] = element.handle;
			entry["sub_index"] = element.sub_index;
			entry["feature"] = int(element.feature);
			entry["tier"] = int(element.tier);
			entry["handle_kind"] = int(element.handle_kind);
			entry["active"] = is_active(element);
			entries.push_back(entry);
		}
	}
	return entries;
}

Dictionary SelectionModel::get_debug_active() const {
	Dictionary result;
	if (mode == MODE_OBJECT) {
		return result;
	}
	const Feature feature = Feature(mode);
	if (!has_active[feature]) {
		return result;
	}
	const Element &element = active[feature];
	result["block_id"] = (int64_t)(uint64_t)element.block_id;
	result["handle"] = element.handle;
	result["sub_index"] = element.sub_index;
	result["feature"] = int(element.feature);
	result["tier"] = int(element.tier);
	result["handle_kind"] = int(element.handle_kind);
	return result;
}

int64_t SelectionModel::encode_corner_pair(int p_corner_a, int p_corner_b) {
	if (p_corner_a < 0 || p_corner_b < 0 || p_corner_a == p_corner_b) {
		return -1;
	}
	const uint32_t corner_a = (uint32_t)MIN(p_corner_a, p_corner_b);
	const uint32_t corner_b = (uint32_t)MAX(p_corner_a, p_corner_b);
	return (int64_t)(((uint64_t)corner_a << 32) | corner_b);
}

bool SelectionModel::decode_corner_pair(int64_t p_value, int &r_corner_a, int &r_corner_b) {
	if (p_value < 0) {
		return false;
	}
	const uint64_t bits = (uint64_t)p_value;
	const uint32_t corner_a = (uint32_t)(bits >> 32);
	const uint32_t corner_b = (uint32_t)(bits & UINT32_MAX);
	if (corner_a > INT32_MAX || corner_b > INT32_MAX || corner_a == corner_b) {
		return false;
	}
	r_corner_a = (int)corner_a;
	r_corner_b = (int)corner_b;
	return true;
}

SelectionModel::SelectionModel() {
	add_user_signal(MethodInfo("selection_changed", PropertyInfo(Variant::PACKED_INT64_ARRAY, "dirty_blocks")));
}

SelectionModel::~SelectionModel() {
	if (scene_tree) {
		const Callable added_callable = callable_mp(this, &SelectionModel::_node_added);
		const Callable removed_callable = callable_mp(this, &SelectionModel::_node_removed);
		if (scene_tree->is_connected(SNAME("node_added"), added_callable)) {
			scene_tree->disconnect(SNAME("node_added"), added_callable);
		}
		if (scene_tree->is_connected(SNAME("node_removed"), removed_callable)) {
			scene_tree->disconnect(SNAME("node_removed"), removed_callable);
		}
	}
	Vector<ObjectID> tracked_ids;
	for (const KeyValue<ObjectID, Ref<LevelMeshData>> &entry : tracked_block_data) {
		tracked_ids.push_back(entry.key);
	}
	for (const ObjectID &block_id : tracked_ids) {
		_untrack_block(block_id, false);
	}
}

LevelDocument::LevelDocument() {
	type = TYPE_LEVEL;
	selection_model = memnew(SelectionModel);
}

LevelDocument::~LevelDocument() {
	if (selection_model) {
		memdelete(selection_model);
		selection_model = nullptr;
	}
}
