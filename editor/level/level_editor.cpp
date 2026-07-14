/**************************************************************************/
/*  level_editor.cpp                                                      */
/**************************************************************************/
/*  G-Level LE0: SERVICE state for the level-editor workspace seam.       */
/**************************************************************************/

#include "level_editor.h"

#include "core/config/engine.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "editor/editor_document.h"
#include "editor/level/level_editor_view.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/settings/editor_settings.h"
#include "scene/main/scene_tree.h"

#include "modules/level_kernel/level_block.h"
#include "modules/level_kernel/level_mesh_data.h"

LevelEditor *LevelEditor::singleton = nullptr;

real_t LevelEditor::snap_step_or_default() {
	return singleton ? singleton->get_snap_step() : DEFAULT_SNAP_STEP;
}

void LevelEditor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_tool_mode"), &LevelEditor::get_tool_mode);
	ClassDB::bind_method(D_METHOD("set_tool_mode", "mode"), &LevelEditor::set_tool_mode);
	ClassDB::bind_method(D_METHOD("get_snap_step"), &LevelEditor::get_snap_step);
	ClassDB::bind_method(D_METHOD("set_snap_step", "step"), &LevelEditor::set_snap_step);
	ClassDB::bind_method(D_METHOD("is_snap_enabled"), &LevelEditor::is_snap_enabled);
	ClassDB::bind_method(D_METHOD("set_snap_enabled", "enabled"), &LevelEditor::set_snap_enabled);
	ClassDB::bind_method(D_METHOD("get_default_block_height"), &LevelEditor::get_default_block_height);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "tool_mode", PROPERTY_HINT_ENUM, "Select,Block"), "set_tool_mode", "get_tool_mode");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "snap_step", PROPERTY_HINT_RANGE, "0.001,1024,0.001,or_greater,suffix:m"), "set_snap_step", "get_snap_step");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "snap_enabled"), "set_snap_enabled", "is_snap_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "default_block_height", PROPERTY_HINT_NONE, "suffix:m", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "", "get_default_block_height");

	BIND_ENUM_CONSTANT(TOOL_SELECT);
	BIND_ENUM_CONSTANT(TOOL_BLOCK);
}

void LevelEditor::set_tool_mode(ToolMode p_mode) {
	ERR_FAIL_INDEX(int(p_mode), 2);
	if (tool_mode == p_mode) {
		return;
	}
	tool_mode = p_mode;
	for (LevelEditorView *view : views) {
		if (view) {
			view->set_tool_mode(tool_mode);
		}
	}
}

void LevelEditor::set_snap_step(real_t p_step) {
	ERR_FAIL_COND_MSG(!Math::is_finite(p_step) || p_step <= CMP_EPSILON, "Level editor snap step must be finite and greater than zero.");
	snap_step = p_step;
}

void LevelEditor::_register_view(LevelEditorView *p_view) {
	ERR_FAIL_NULL(p_view);
	ERR_FAIL_COND(views.find(p_view) >= 0);
	views.push_back(p_view);
	p_view->set_tool_mode(tool_mode);
}

void LevelEditor::_unregister_view(LevelEditorView *p_view) {
	const int index = views.find(p_view);
	if (index >= 0) {
		views.remove_at(index);
	}
}

void LevelEditor::_scan_node(Node *p_node) {
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

void LevelEditor::_node_added(Node *p_node) {
	if (LevelBlock *block = Object::cast_to<LevelBlock>(p_node)) {
		_track_block(block);
	}
}

void LevelEditor::_node_removed(Node *p_node) {
	if (LevelBlock *block = Object::cast_to<LevelBlock>(p_node)) {
		_untrack_block(block);
	}
}

void LevelEditor::_track_block(LevelBlock *p_block) {
	ERR_FAIL_NULL(p_block);
	const ObjectID block_id = p_block->get_instance_id();
	const int64_t bound_id = (int64_t)(uint64_t)block_id;
	const Callable entered_callable = callable_mp(this, &LevelEditor::_block_world_entered).bind(bound_id);
	const Callable exiting_callable = callable_mp(this, &LevelEditor::_block_world_exiting).bind(bound_id);
	const Callable baked_callable = callable_mp(this, &LevelEditor::_block_baked).bind(bound_id);
	const Callable transform_callable = callable_mp(this, &LevelEditor::_block_transform_changed).bind(bound_id);
	if (!p_block->is_connected(SNAME("level_world_entered"), entered_callable)) {
		p_block->connect(SNAME("level_world_entered"), entered_callable);
	}
	if (!p_block->is_connected(SNAME("level_world_exiting"), exiting_callable)) {
		p_block->connect(SNAME("level_world_exiting"), exiting_callable);
	}
	if (!p_block->is_connected(SNAME("baked"), baked_callable)) {
		p_block->connect(SNAME("baked"), baked_callable);
	}
	p_block->set_notify_transform(true);
	if (!p_block->is_connected(SNAME("level_transform_changed"), transform_callable)) {
		p_block->connect(SNAME("level_transform_changed"), transform_callable);
	}
	if (p_block->is_inside_world()) {
		_register_block(block_id);
	}
}

void LevelEditor::_untrack_block(LevelBlock *p_block) {
	ERR_FAIL_NULL(p_block);
	const ObjectID block_id = p_block->get_instance_id();
	const int64_t bound_id = (int64_t)(uint64_t)block_id;
	const Callable entered_callable = callable_mp(this, &LevelEditor::_block_world_entered).bind(bound_id);
	const Callable exiting_callable = callable_mp(this, &LevelEditor::_block_world_exiting).bind(bound_id);
	const Callable baked_callable = callable_mp(this, &LevelEditor::_block_baked).bind(bound_id);
	const Callable transform_callable = callable_mp(this, &LevelEditor::_block_transform_changed).bind(bound_id);
	if (p_block->is_connected(SNAME("level_world_entered"), entered_callable)) {
		p_block->disconnect(SNAME("level_world_entered"), entered_callable);
	}
	if (p_block->is_connected(SNAME("level_world_exiting"), exiting_callable)) {
		p_block->disconnect(SNAME("level_world_exiting"), exiting_callable);
	}
	if (p_block->is_connected(SNAME("baked"), baked_callable)) {
		p_block->disconnect(SNAME("baked"), baked_callable);
	}
	if (p_block->is_connected(SNAME("level_transform_changed"), transform_callable)) {
		p_block->disconnect(SNAME("level_transform_changed"), transform_callable);
	}
	_unregister_block(block_id);
}

bool LevelEditor::_get_block_world_aabb(LevelBlock *p_block, AABB &r_aabb) const {
	if (!p_block || !p_block->is_inside_world() || p_block->get_data().is_null()) {
		return false;
	}
	const PackedVector3Array positions = p_block->get_data()->get_vertex_positions();
	const PackedByteArray alive = p_block->get_data()->get_vertex_alive();
	bool initialized = false;
	AABB local_aabb;
	const int vertex_count = MIN(positions.size(), alive.size());
	for (int vertex_id = 0; vertex_id < vertex_count; vertex_id++) {
		if (alive[vertex_id] == 0 || !positions[vertex_id].is_finite()) {
			continue;
		}
		if (!initialized) {
			local_aabb = AABB(positions[vertex_id], Vector3());
			initialized = true;
		} else {
			local_aabb.expand_to(positions[vertex_id]);
		}
	}
	if (!initialized) {
		return false;
	}
	r_aabb = p_block->get_global_transform().xform(local_aabb);
	return r_aabb.position.is_finite() && r_aabb.size.is_finite();
}

void LevelEditor::_register_block(ObjectID p_block_id) {
	Object *object = ObjectDB::get_instance(p_block_id);
	LevelBlock *block = Object::cast_to<LevelBlock>(object);
	Node3DEditor *node_3d_editor = Node3DEditor::get_singleton();
	AABB world_aabb;
	if (!block || !node_3d_editor || !_get_block_world_aabb(block, world_aabb)) {
		_unregister_block(p_block_id);
		return;
	}
	HashMap<ObjectID, DynamicBVH::ID>::Iterator registered = block_bvh_ids.find(p_block_id);
	if (registered) {
		node_3d_editor->update_gizmo_bvh_node(registered->value, world_aabb);
	} else {
		block_bvh_ids[p_block_id] = node_3d_editor->insert_gizmo_bvh_node(block, world_aabb);
	}
}

void LevelEditor::_unregister_block(ObjectID p_block_id) {
	HashMap<ObjectID, DynamicBVH::ID>::Iterator registered = block_bvh_ids.find(p_block_id);
	if (!registered) {
		return;
	}
	if (Node3DEditor *node_3d_editor = Node3DEditor::get_singleton()) {
		node_3d_editor->remove_gizmo_bvh_node(registered->value);
	}
	block_bvh_ids.erase(p_block_id);
}

void LevelEditor::_block_world_entered(int64_t p_block_id) {
	_register_block(ObjectID((uint64_t)p_block_id));
}

void LevelEditor::_block_world_exiting(int64_t p_block_id) {
	_unregister_block(ObjectID((uint64_t)p_block_id));
}

void LevelEditor::_block_baked(int64_t p_block_id) {
	_register_block(ObjectID((uint64_t)p_block_id));
}

void LevelEditor::_block_transform_changed(int64_t p_block_id) {
	_register_block(ObjectID((uint64_t)p_block_id));
}

void LevelEditor::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			SceneTree *scene_tree = get_tree();
			if (scene_tree) {
				scene_tree->connect(SNAME("node_added"), callable_mp(this, &LevelEditor::_node_added));
				scene_tree->connect(SNAME("node_removed"), callable_mp(this, &LevelEditor::_node_removed));
				_scan_node(scene_tree->get_root());
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			SceneTree *scene_tree = get_tree();
			if (scene_tree) {
				const Callable added_callable = callable_mp(this, &LevelEditor::_node_added);
				const Callable removed_callable = callable_mp(this, &LevelEditor::_node_removed);
				if (scene_tree->is_connected(SNAME("node_added"), added_callable)) {
					scene_tree->disconnect(SNAME("node_added"), added_callable);
				}
				if (scene_tree->is_connected(SNAME("node_removed"), removed_callable)) {
					scene_tree->disconnect(SNAME("node_removed"), removed_callable);
				}
			}
			Vector<ObjectID> registered_ids;
			for (const KeyValue<ObjectID, DynamicBVH::ID> &entry : block_bvh_ids) {
				registered_ids.push_back(entry.key);
			}
			for (const ObjectID &block_id : registered_ids) {
				_unregister_block(block_id);
			}
		} break;
	}
}

LevelEditorView *LevelEditor::create_editor_view(LevelDocument *p_document) {
	ERR_FAIL_NULL_V(p_document, nullptr);
	return memnew(LevelEditorView(p_document));
}

LevelEditor::LevelEditor() {
	ERR_FAIL_COND(singleton != nullptr);
	singleton = this;
	set_name("LevelEditor");

	ED_SHORTCUT("level_editor/add_block", TTRC("Add Block"), KeyModifierMask::SHIFT | Key::B);
	Engine::Singleton engine_singleton("LevelEditor", this);
	engine_singleton.editor_only = true;
	Engine::get_singleton()->add_singleton(engine_singleton);
}

LevelEditor::~LevelEditor() {
	if (singleton == this) {
		Engine::get_singleton()->remove_singleton("LevelEditor");
		singleton = nullptr;
	}
}
