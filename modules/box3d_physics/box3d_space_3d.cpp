/**************************************************************************/
/*  box3d_space_3d.cpp                                                    */
/**************************************************************************/

#include "box3d_space_3d.h"

#include "box3d_area_3d.h"
#include "box3d_body_3d.h"
#include "box3d_collision_object_3d.h"
#include "box3d_conversions.h"
#include "box3d_direct_space_state_3d.h"

#include <string.h>

// TODO(box3d): substep count + worker count become physics/box3d/* project settings.
static const int BOX3D_SUBSTEPS = 4;

static Box3DCollisionObject3D *_box3d_shape_object(b3ShapeId p_shape_id) {
	if (!b3Shape_IsValid(p_shape_id)) {
		return nullptr;
	}
	const b3BodyId body_id = b3Shape_GetBody(p_shape_id);
	if (!b3Body_IsValid(body_id)) {
		return nullptr;
	}
	return static_cast<Box3DCollisionObject3D *>(b3Body_GetUserData(body_id));
}

static bool _box3d_custom_filter_callback(b3ShapeId p_shape_a, b3ShapeId p_shape_b, void *p_context) {
	Box3DCollisionObject3D *object_a = _box3d_shape_object(p_shape_a);
	Box3DCollisionObject3D *object_b = _box3d_shape_object(p_shape_b);
	if (object_a == nullptr || object_b == nullptr || object_a->get_type() != Box3DCollisionObject3D::TYPE_BODY || object_b->get_type() != Box3DCollisionObject3D::TYPE_BODY) {
		return true;
	}

	const Box3DBody3D *body_a = static_cast<Box3DBody3D *>(object_a);
	const Box3DBody3D *body_b = static_cast<Box3DBody3D *>(object_b);
	return !body_a->has_collision_exception(body_b->get_rid()) && !body_b->has_collision_exception(body_a->get_rid());
}

Box3DSpace3D::Box3DSpace3D() {
	b3WorldDef def = b3DefaultWorldDef();
	def.workerCount = 1; // Single-threaded until task-system integration lands.
	world = b3CreateWorld(&def);
	b3World_SetCustomFilterCallback(world, _box3d_custom_filter_callback, this);
	_update_world_gravity();
}

Box3DSpace3D::~Box3DSpace3D() {
	// Bodies must already be detached (Godot frees bodies before their space).
	if (direct_state) {
		memdelete(direct_state);
	}
	if (recording_active) {
		b3World_StopRecording(world);
	}
	if (recording != nullptr) {
		b3DestroyRecording(recording);
	}
	b3DestroyWorld(world);
}

void Box3DSpace3D::_update_world_gravity() {
	b3World_SetGravity(world, to_box3d(default_gravity_vector * default_gravity));
}

void Box3DSpace3D::body_removed(Box3DBody3D *p_body) {
	bodies.erase(p_body);
	pending_kinematic.erase(p_body);
	dirty_bodies.erase(p_body);
}

void Box3DSpace3D::area_removed(Box3DArea3D *p_area) {
	areas.erase(p_area);
	dirty_areas.erase(p_area);
	for (Box3DBody3D *body : bodies) {
		body->remove_area(p_area);
	}
}

void Box3DSpace3D::setup_direct_state(RID_PtrOwner<Box3DShape3D> *p_shape_owner, RID_PtrOwner<Box3DBody3D> *p_body_owner) {
	if (!direct_state) {
		direct_state = memnew(Box3DDirectSpaceState3D);
	}
	direct_state->setup(this, p_shape_owner, p_body_owner);
}

void Box3DSpace3D::step(real_t p_step) {
	for (Box3DBody3D *body : pending_kinematic) {
		body->apply_kinematic_target((float)p_step);
	}
	pending_kinematic.clear();

	for (Box3DBody3D *body : bodies) {
		body->clear_reported_contacts();
		body->apply_environment_forces((float)p_step);
	}

	stepping = true;
	b3World_Step(world, (float)p_step, BOX3D_SUBSTEPS);
	stepping = false;
	last_step = p_step;

	// Pull move events; buffer state-sync work for flush_queries().
	b3BodyEvents events = b3World_GetBodyEvents(world);
	for (int i = 0; i < events.moveCount; i++) {
		const b3BodyMoveEvent &e = events.moveEvents[i];
		Box3DBody3D *body = static_cast<Box3DBody3D *>(e.userData);
		if (body == nullptr) {
			continue;
		}
		body->sync_from_move_event(e.transform, e.fellAsleep);
		if (!body->in_dirty_list) {
			body->in_dirty_list = true;
			dirty_bodies.push_back(body);
		}
	}

	b3SensorEvents sensor_events = b3World_GetSensorEvents(world);
	for (int i = 0; i < sensor_events.beginCount; i++) {
		_process_sensor_event(true, sensor_events.beginEvents[i].sensorShapeId, sensor_events.beginEvents[i].visitorShapeId);
	}
	for (int i = 0; i < sensor_events.endCount; i++) {
		_process_sensor_event(false, sensor_events.endEvents[i].sensorShapeId, sensor_events.endEvents[i].visitorShapeId);
	}

	_harvest_body_contacts();
}

void Box3DSpace3D::call_queries() {
	for (Box3DBody3D *body : dirty_bodies) {
		body->in_dirty_list = false;
		body->call_state_sync();
	}
	dirty_bodies.clear();

	for (Box3DArea3D *area : dirty_areas) {
		area->call_queries();
	}
	dirty_areas.clear();
}

bool Box3DSpace3D::start_recording(int p_byte_capacity) {
	ERR_FAIL_COND_V_MSG(recording_active, false, "Box3D: this space is already recording.");
	if (recording != nullptr) {
		b3DestroyRecording(recording);
		recording = nullptr;
	}
	recording = b3CreateRecording(MAX(0, p_byte_capacity));
	ERR_FAIL_NULL_V_MSG(recording, false, "Box3D: failed to create recording buffer.");
	b3World_StartRecording(world, recording);
	recording_active = true;
	return true;
}

PackedByteArray Box3DSpace3D::stop_recording() {
	ERR_FAIL_COND_V_MSG(!recording_active, PackedByteArray(), "Box3D: this space is not recording.");
	b3World_StopRecording(world);
	recording_active = false;
	return get_recording_data();
}

PackedByteArray Box3DSpace3D::get_recording_data() const {
	PackedByteArray bytes;
	ERR_FAIL_NULL_V(recording, bytes);
	const int size = b3Recording_GetSize(recording);
	if (size <= 0) {
		return bytes;
	}
	const uint8_t *data = b3Recording_GetData(recording);
	ERR_FAIL_NULL_V(data, bytes);
	bytes.resize(size);
	memcpy(bytes.ptrw(), data, size);
	return bytes;
}

int Box3DSpace3D::get_recording_size() const {
	return recording != nullptr ? b3Recording_GetSize(recording) : 0;
}

bool Box3DSpace3D::save_recording(const String &p_path) const {
	ERR_FAIL_NULL_V_MSG(recording, false, "Box3D: no recording is available to save.");
	ERR_FAIL_COND_V_MSG(recording_active, false, "Box3D: stop recording before saving.");
	const CharString path = p_path.utf8();
	return b3SaveRecordingToFile(recording, path.get_data());
}

static int _box3d_shape_index(b3ShapeId p_shape_id) {
	return (int)(uintptr_t)b3Shape_GetUserData(p_shape_id);
}

void Box3DSpace3D::_process_sensor_event(bool p_added, b3ShapeId p_sensor_shape, b3ShapeId p_visitor_shape) {
	Box3DCollisionObject3D *sensor_object = _box3d_shape_object(p_sensor_shape);
	Box3DCollisionObject3D *visitor_object = _box3d_shape_object(p_visitor_shape);
	if (sensor_object == nullptr || visitor_object == nullptr || sensor_object->get_type() != Box3DCollisionObject3D::TYPE_AREA) {
		return;
	}

	Box3DArea3D *sensor_area = static_cast<Box3DArea3D *>(sensor_object);
	const int sensor_shape = _box3d_shape_index(p_sensor_shape);
	const int visitor_shape = _box3d_shape_index(p_visitor_shape);

	if (visitor_object->get_type() == Box3DCollisionObject3D::TYPE_BODY) {
		Box3DBody3D *body = static_cast<Box3DBody3D *>(visitor_object);
		if (p_added) {
			body->add_area(sensor_area);
		} else {
			body->remove_area(sensor_area);
		}
		sensor_area->queue_body_event(p_added, body->get_rid(), body->get_instance_id(), visitor_shape, sensor_shape);
	} else if (visitor_object->get_type() == Box3DCollisionObject3D::TYPE_AREA) {
		Box3DArea3D *area = static_cast<Box3DArea3D *>(visitor_object);
		// Godot contract: an area is reported to other areas only when it is monitorable, and
		// area-area detection is one-way (sensor mask vs visitor layer). Box3D-level filtering
		// always passes for area pairs (both carry the query bit in category and mask), so the
		// layer test lives here.
		if (!area->is_monitorable() || (sensor_area->get_collision_mask() & area->get_collision_layer()) == 0) {
			return;
		}
		sensor_area->queue_area_event(p_added, area->get_rid(), area->get_instance_id(), visitor_shape, sensor_shape);
	}

	if (!dirty_areas.has(sensor_area)) {
		dirty_areas.push_back(sensor_area);
	}
}

static Vector3 _box3d_contact_point(const b3BodyId &p_body, const b3ManifoldPoint &p_point, bool p_shape_a) {
	const b3Vec3 anchor = p_shape_a ? p_point.anchorA : p_point.anchorB;
	return to_godot(b3OffsetPos(b3Body_GetWorldPoint(p_body, b3Body_GetLocalCenter(p_body)), anchor));
}

void Box3DSpace3D::_harvest_body_contacts() {
	LocalVector<b3ContactData> contact_data;
	for (Box3DBody3D *body : bodies) {
		if (!body->reports_contacts() || !body->in_space()) {
			continue;
		}
		contact_data.resize(body->get_max_contacts_reported());
		const int data_count = b3Body_GetContactData(body->get_body_id(), contact_data.ptr(), contact_data.size());
		for (int i = 0; i < data_count; i++) {
			const b3ContactData &data = contact_data[i];
			const bool body_is_a = B3_ID_EQUALS(b3Shape_GetBody(data.shapeIdA), body->get_body_id());
			const b3ShapeId self_shape = body_is_a ? data.shapeIdA : data.shapeIdB;
			const b3ShapeId other_shape = body_is_a ? data.shapeIdB : data.shapeIdA;
			Box3DCollisionObject3D *other_object = _box3d_shape_object(other_shape);
			if (other_object == nullptr || other_object->get_type() != Box3DCollisionObject3D::TYPE_BODY) {
				continue;
			}
			Box3DBody3D *other_body = static_cast<Box3DBody3D *>(other_object);
			for (int j = 0; j < data.manifoldCount; j++) {
				const b3Manifold &manifold = data.manifolds[j];
				for (int k = 0; k < manifold.pointCount; k++) {
					const b3ManifoldPoint &point = manifold.points[k];
					if (point.totalNormalImpulse <= 0.0f) {
						continue;
					}
					const Vector3 self_point = _box3d_contact_point(body->get_body_id(), point, body_is_a);
					const Vector3 other_point = _box3d_contact_point(other_body->get_body_id(), point, !body_is_a);
					const Vector3 normal = to_godot(manifold.normal) * (body_is_a ? -1.0 : 1.0);
					Box3DBody3D::Contact contact;
					// Godot's get_contact_local_* getters return GLOBAL coordinates despite the
					// names (see PhysicsDirectBodyState3D docs and the Jolt module) — store world space.
					contact.local_pos = self_point;
					contact.local_normal = normal;
					contact.impulse = normal * point.totalNormalImpulse;
					contact.local_shape = _box3d_shape_index(self_shape);
					contact.local_velocity_at_pos = to_godot(b3Body_GetWorldPointVelocity(body->get_body_id(), to_box3d(self_point)));
					contact.collider = other_body->get_rid();
					contact.collider_pos = other_point;
					contact.collider_instance_id = other_body->get_instance_id();
					contact.collider_shape = _box3d_shape_index(other_shape);
					contact.collider_velocity_at_pos = to_godot(b3Body_GetWorldPointVelocity(other_body->get_body_id(), to_box3d(other_point)));
					contact.depth = MAX((real_t)0.0, (real_t)-point.separation);
					body->add_contact(contact);
				}
			}
		}
		if (body->contact_count > 0 && !body->in_dirty_list) {
			body->in_dirty_list = true;
			dirty_bodies.push_back(body);
		}
	}
}

void Box3DSpace3D::set_default_area_param(PhysicsServer3D::AreaParameter p_param, const Variant &p_value) {
	switch (p_param) {
		case PhysicsServer3D::AREA_PARAM_GRAVITY: {
			default_gravity = p_value;
			_update_world_gravity();
		} break;
		case PhysicsServer3D::AREA_PARAM_GRAVITY_VECTOR: {
			default_gravity_vector = p_value;
			_update_world_gravity();
		} break;
		case PhysicsServer3D::AREA_PARAM_LINEAR_DAMP: {
			default_linear_damp = p_value; // Applied per body once damping lands (milestone 2).
		} break;
		case PhysicsServer3D::AREA_PARAM_ANGULAR_DAMP: {
			default_angular_damp = p_value;
		} break;
		default: {
			// Override modes / priority / wind are meaningless on the default area for now.
		} break;
	}
}

Variant Box3DSpace3D::get_default_area_param(PhysicsServer3D::AreaParameter p_param) const {
	switch (p_param) {
		case PhysicsServer3D::AREA_PARAM_GRAVITY:
			return default_gravity;
		case PhysicsServer3D::AREA_PARAM_GRAVITY_VECTOR:
			return default_gravity_vector;
		case PhysicsServer3D::AREA_PARAM_LINEAR_DAMP:
			return default_linear_damp;
		case PhysicsServer3D::AREA_PARAM_ANGULAR_DAMP:
			return default_angular_damp;
		default:
			return Variant();
	}
}
