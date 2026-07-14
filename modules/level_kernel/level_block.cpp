/**************************************************************************/
/*  level_block.cpp                                                       */
/**************************************************************************/

#include "level_block.h"

#include "level_mesh.h"
#include "level_mesh_baker.h"
#include "level_mesh_data.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/3d/physics/static_body_3d.h"
#include "scene/resources/3d/concave_polygon_shape_3d.h"

void LevelBlock::_data_changed() {
	if (mesh_instance != nullptr) {
		rebuild();
	}
}

void LevelBlock::_preview_changed() {
	_ensure_internal_nodes();
	Ref<LevelMeshBaker> baker;
	baker.instantiate();
	mesh_instance->set_mesh(baker->bake(data));
	// Preview deliberately leaves collision stale until the committed changed
	// notification, but render bounds and selection highlights must follow it.
	emit_signal(SNAME("baked"));
}

void LevelBlock::_ensure_internal_nodes() {
	if (mesh_instance != nullptr) {
		return;
	}

	mesh_instance = memnew(MeshInstance3D);
	mesh_instance->set_name("_LevelMesh");
	add_child(mesh_instance, false, INTERNAL_MODE_BACK);

	static_body = memnew(StaticBody3D);
	static_body->set_name("_LevelCollision");
	add_child(static_body, false, INTERNAL_MODE_BACK);

	collision_shape = memnew(CollisionShape3D);
	collision_shape->set_name("_LevelCollisionShape");
	static_body->add_child(collision_shape, false, INTERNAL_MODE_BACK);
}

void LevelBlock::set_data(const Ref<LevelMeshData> &p_data) {
	if (data == p_data) {
		return;
	}
	if (data.is_valid()) {
		data->disconnect_changed(callable_mp(this, &LevelBlock::_data_changed));
		const Callable preview_callable = callable_mp(this, &LevelBlock::_preview_changed);
		if (data->is_connected(SNAME("mesh_preview_changed"), preview_callable)) {
			data->disconnect(SNAME("mesh_preview_changed"), preview_callable);
		}
	}

	if (p_data.is_valid()) {
		data = p_data;
	} else {
		data.instantiate();
	}
	data->connect_changed(callable_mp(this, &LevelBlock::_data_changed));
	data->connect(SNAME("mesh_preview_changed"), callable_mp(this, &LevelBlock::_preview_changed));
	if (level_mesh.is_valid()) {
		level_mesh->set_data(data);
	}

	if (mesh_instance != nullptr) {
		rebuild();
	}
}

Ref<LevelMeshData> LevelBlock::get_data() const {
	return data;
}

Ref<LevelMesh> LevelBlock::get_level_mesh() {
	if (level_mesh.is_null()) {
		level_mesh.instantiate();
	}
	if (level_mesh->get_data() != data) {
		level_mesh->set_data(data);
	}
	return level_mesh;
}

void LevelBlock::rebuild() {
	_ensure_internal_nodes();

	Ref<LevelMeshBaker> baker;
	baker.instantiate();
	mesh_instance->set_mesh(baker->bake(data));

	const PackedVector3Array collision_faces = baker->bake_collision_faces(data);
	if (collision_faces.is_empty()) {
		collision_shape->set_shape(Ref<Shape3D>());
		emit_signal(SNAME("baked"));
		return;
	}

	Ref<ConcavePolygonShape3D> shape;
	shape.instantiate();
	shape->set_faces(collision_faces);
	collision_shape->set_shape(shape);
	emit_signal(SNAME("baked"));
}

void LevelBlock::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_WORLD: {
			emit_signal(SNAME("level_world_entered"));
		} break;
		case NOTIFICATION_READY: {
			rebuild();
		} break;
		case NOTIFICATION_TRANSFORM_CHANGED: {
			emit_signal(SNAME("level_transform_changed"));
		} break;
		case NOTIFICATION_EXIT_WORLD: {
			emit_signal(SNAME("level_world_exiting"));
		} break;
	}
}

LevelBlock::LevelBlock() {
	data.instantiate();
	data->connect_changed(callable_mp(this, &LevelBlock::_data_changed));
	data->connect(SNAME("mesh_preview_changed"), callable_mp(this, &LevelBlock::_preview_changed));
}

LevelBlock::~LevelBlock() {
	if (data.is_valid()) {
		data->disconnect_changed(callable_mp(this, &LevelBlock::_data_changed));
		const Callable preview_callable = callable_mp(this, &LevelBlock::_preview_changed);
		if (data->is_connected(SNAME("mesh_preview_changed"), preview_callable)) {
			data->disconnect(SNAME("mesh_preview_changed"), preview_callable);
		}
	}
}

void LevelBlock::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_data", "data"), &LevelBlock::set_data);
	ClassDB::bind_method(D_METHOD("get_data"), &LevelBlock::get_data);
	ClassDB::bind_method(D_METHOD("get_level_mesh"), &LevelBlock::get_level_mesh);
	ClassDB::bind_method(D_METHOD("rebuild"), &LevelBlock::rebuild);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "data", PROPERTY_HINT_RESOURCE_TYPE, "LevelMeshData"), "set_data", "get_data");
	ADD_SIGNAL(MethodInfo("baked"));
	ADD_SIGNAL(MethodInfo("level_world_entered"));
	ADD_SIGNAL(MethodInfo("level_world_exiting"));
	ADD_SIGNAL(MethodInfo("level_transform_changed"));
	// The constructor intentionally owns an empty data resource. Declare the
	// serialized default explicitly so editor doctool inspection does not treat
	// that per-instance resource as a shared Object default.
	ADD_PROPERTY_DEFAULT("data", Ref<LevelMeshData>());
}
