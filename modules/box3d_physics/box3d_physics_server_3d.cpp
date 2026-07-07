/**************************************************************************/
/*  box3d_physics_server_3d.cpp                                           */
/**************************************************************************/

#include "box3d_physics_server_3d.h"

#include "box3d_direct_space_state_3d.h"

Box3DPhysicsServer3D::Box3DPhysicsServer3D(bool p_using_threads) :
		using_threads(p_using_threads) {
	singleton = this;
}

Box3DPhysicsServer3D::~Box3DPhysicsServer3D() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

// --- Shapes ---

RID Box3DPhysicsServer3D::_create_shape(ShapeType p_type) {
	Box3DShape3D *shape = memnew(Box3DShape3D(p_type));
	return shape_owner.make_rid(shape);
}

bool Box3DPhysicsServer3D::_can_mutate_body_shapes(const Box3DBody3D *p_body) const {
	Box3DSpace3D *space = p_body->get_space();
	return space == nullptr || can_access_space(space);
}

bool Box3DPhysicsServer3D::_can_mutate_shape_owners(const Box3DShape3D *p_shape) const {
	for (Box3DCollisionObject3D *object : p_shape->get_owners()) {
		Box3DSpace3D *space = object->get_space();
		if (space != nullptr && !can_access_space(space)) {
			return false;
		}
	}
	return true;
}

RID Box3DPhysicsServer3D::custom_shape_create() {
	ERR_FAIL_V_MSG(RID(), "Box3D: custom shapes are not supported.");
}

void Box3DPhysicsServer3D::shape_set_data(RID p_shape, const Variant &p_data) {
	Box3DShape3D *shape = shape_owner.get_or_null(p_shape);
	ERR_FAIL_NULL(shape);
	shape->set_data(p_data);
}

Variant Box3DPhysicsServer3D::shape_get_data(RID p_shape) const {
	Box3DShape3D *shape = shape_owner.get_or_null(p_shape);
	ERR_FAIL_NULL_V(shape, Variant());
	return shape->get_data();
}

PhysicsServer3D::ShapeType Box3DPhysicsServer3D::shape_get_type(RID p_shape) const {
	Box3DShape3D *shape = shape_owner.get_or_null(p_shape);
	ERR_FAIL_NULL_V(shape, SHAPE_CUSTOM);
	return shape->get_type();
}

void Box3DPhysicsServer3D::shape_set_surface_material(RID p_shape, int p_material_id) {
	Box3DShape3D *shape = shape_owner.get_or_null(p_shape);
	ERR_FAIL_NULL(shape);
	ERR_FAIL_COND_MSG(!_can_mutate_shape_owners(shape), "Box3D: surface material changes are inaccessible right now, wait for iteration or physics process notification.");
	shape->set_surface_material(p_material_id);
}

void Box3DPhysicsServer3D::shape_set_surface_map(RID p_shape, const PackedInt64Array &p_material_ids, const PackedByteArray &p_triangle_indices) {
	Box3DShape3D *shape = shape_owner.get_or_null(p_shape);
	ERR_FAIL_NULL(shape);
	ERR_FAIL_COND_MSG(!_can_mutate_shape_owners(shape), "Box3D: surface material changes are inaccessible right now, wait for iteration or physics process notification.");
	shape->set_surface_map(p_material_ids, p_triangle_indices);
}

int Box3DPhysicsServer3D::shape_get_face_material_id(RID p_shape, int p_face_index) const {
	Box3DShape3D *shape = shape_owner.get_or_null(p_shape);
	ERR_FAIL_NULL_V(shape, 0);
	return shape->get_face_material_id(p_face_index);
}

PackedByteArray Box3DPhysicsServer3D::shape_get_mesh_material_indices(RID p_shape) const {
	Box3DShape3D *shape = shape_owner.get_or_null(p_shape);
	ERR_FAIL_NULL_V(shape, PackedByteArray());
	return shape->get_mesh_material_indices();
}

// --- Spaces ---

RID Box3DPhysicsServer3D::space_create() {
	Box3DSpace3D *space = memnew(Box3DSpace3D);
	space->setup_direct_state(&shape_owner, &body_owner);
	return space_owner.make_rid(space);
}

void Box3DPhysicsServer3D::space_set_active(RID p_space, bool p_active) {
	Box3DSpace3D *space = space_owner.get_or_null(p_space);
	ERR_FAIL_NULL(space);
	if (p_active) {
		active_spaces.insert(space);
	} else {
		active_spaces.erase(space);
	}
}

bool Box3DPhysicsServer3D::space_is_active(RID p_space) const {
	Box3DSpace3D *space = space_owner.get_or_null(p_space);
	ERR_FAIL_NULL_V(space, false);
	return active_spaces.has(space);
}

PhysicsDirectSpaceState3D *Box3DPhysicsServer3D::space_get_direct_state(RID p_space) {
	Box3DSpace3D *space = space_owner.get_or_null(p_space);
	ERR_FAIL_NULL_V(space, nullptr);
	ERR_FAIL_COND_V_MSG((using_threads && !doing_sync) || space->is_stepping(), nullptr, "Space state is inaccessible right now, wait for iteration or physics process notification.");
	return space->get_direct_state();
}

// --- Areas ---

RID Box3DPhysicsServer3D::area_create() {
	Box3DArea3D *area = memnew(Box3DArea3D);
	RID rid = area_owner.make_rid(area);
	area->set_rid(rid);
	return rid;
}

void Box3DPhysicsServer3D::area_set_space(RID p_area, RID p_space) {
	Box3DArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);
	Box3DSpace3D *space = nullptr;
	if (p_space.is_valid()) {
		space = space_owner.get_or_null(p_space);
		ERR_FAIL_NULL(space);
	}
	area->set_space_rid(p_space);
	area->set_space(space);
}

RID Box3DPhysicsServer3D::area_get_space(RID p_area) const {
	Box3DArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL_V(area, RID());
	return area->get_space_rid();
}

void Box3DPhysicsServer3D::area_add_shape(RID p_area, RID p_shape, const Transform3D &p_transform, bool p_disabled) {
	Box3DArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);
	Box3DShape3D *shape = shape_owner.get_or_null(p_shape);
	ERR_FAIL_NULL(shape);
	area->add_shape(p_shape, shape, p_transform, p_disabled);
}

void Box3DPhysicsServer3D::area_set_shape(RID p_area, int p_shape_idx, RID p_shape) {
	Box3DArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);
	Box3DShape3D *shape = shape_owner.get_or_null(p_shape);
	ERR_FAIL_NULL(shape);
	area->set_shape(p_shape_idx, p_shape, shape);
}

void Box3DPhysicsServer3D::area_set_shape_transform(RID p_area, int p_shape_idx, const Transform3D &p_transform) {
	Box3DArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);
	area->set_shape_transform(p_shape_idx, p_transform);
}

int Box3DPhysicsServer3D::area_get_shape_count(RID p_area) const {
	Box3DArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL_V(area, 0);
	return area->get_shape_count();
}

RID Box3DPhysicsServer3D::area_get_shape(RID p_area, int p_shape_idx) const {
	Box3DArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL_V(area, RID());
	const Box3DArea3D::ShapeSlot *slot = area->get_shape_slot(p_shape_idx);
	return slot ? slot->rid : RID();
}

Transform3D Box3DPhysicsServer3D::area_get_shape_transform(RID p_area, int p_shape_idx) const {
	Box3DArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL_V(area, Transform3D());
	const Box3DArea3D::ShapeSlot *slot = area->get_shape_slot(p_shape_idx);
	return slot ? slot->xform : Transform3D();
}

void Box3DPhysicsServer3D::area_remove_shape(RID p_area, int p_shape_idx) {
	Box3DArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);
	area->remove_shape_at(p_shape_idx);
}

void Box3DPhysicsServer3D::area_clear_shapes(RID p_area) {
	Box3DArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);
	area->clear_shapes();
}

void Box3DPhysicsServer3D::area_set_shape_disabled(RID p_area, int p_shape_idx, bool p_disabled) {
	Box3DArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);
	area->set_shape_disabled(p_shape_idx, p_disabled);
}

void Box3DPhysicsServer3D::area_attach_object_instance_id(RID p_area, ObjectID p_id) {
	Box3DArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);
	area->set_instance_id(p_id);
}

ObjectID Box3DPhysicsServer3D::area_get_object_instance_id(RID p_area) const {
	Box3DArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL_V(area, ObjectID());
	return area->get_instance_id();
}

void Box3DPhysicsServer3D::area_set_param(RID p_area, AreaParameter p_param, const Variant &p_value) {
	// World3D configures the default area through the *space* RID.
	Box3DSpace3D *space = space_owner.get_or_null(p_area);
	if (space) {
		space->set_default_area_param(p_param, p_value);
		return;
	}
	Box3DArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);
	area->set_param(p_param, p_value);
}

Variant Box3DPhysicsServer3D::area_get_param(RID p_area, AreaParameter p_param) const {
	Box3DSpace3D *space = space_owner.get_or_null(p_area);
	if (space) {
		return space->get_default_area_param(p_param);
	}
	Box3DArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL_V(area, Variant());
	return area->get_param(p_param);
}

void Box3DPhysicsServer3D::area_set_transform(RID p_area, const Transform3D &p_transform) {
	Box3DArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);
	area->set_transform(p_transform);
}

Transform3D Box3DPhysicsServer3D::area_get_transform(RID p_area) const {
	Box3DArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL_V(area, Transform3D());
	return area->get_transform();
}

void Box3DPhysicsServer3D::area_set_collision_layer(RID p_area, uint32_t p_layer) {
	Box3DArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);
	area->set_collision_layer(p_layer);
}

uint32_t Box3DPhysicsServer3D::area_get_collision_layer(RID p_area) const {
	Box3DArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL_V(area, 0);
	return area->get_collision_layer();
}

void Box3DPhysicsServer3D::area_set_collision_mask(RID p_area, uint32_t p_mask) {
	Box3DArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);
	area->set_collision_mask(p_mask);
}

uint32_t Box3DPhysicsServer3D::area_get_collision_mask(RID p_area) const {
	Box3DArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL_V(area, 0);
	return area->get_collision_mask();
}

void Box3DPhysicsServer3D::area_set_monitorable(RID p_area, bool p_monitorable) {
	Box3DArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);
	area->set_monitorable(p_monitorable);
}

void Box3DPhysicsServer3D::area_set_monitor_callback(RID p_area, const Callable &p_callback) {
	Box3DArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);
	area->set_monitor_callback(p_callback);
}

void Box3DPhysicsServer3D::area_set_area_monitor_callback(RID p_area, const Callable &p_callback) {
	Box3DArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);
	area->set_area_monitor_callback(p_callback);
}

void Box3DPhysicsServer3D::area_set_ray_pickable(RID p_area, bool p_enable) {
	Box3DArea3D *area = area_owner.get_or_null(p_area);
	ERR_FAIL_NULL(area);
	area->set_ray_pickable(p_enable);
}

// --- Bodies ---

RID Box3DPhysicsServer3D::body_create() {
	Box3DBody3D *body = memnew(Box3DBody3D);
	RID rid = body_owner.make_rid(body);
	body->set_rid(rid);
	return rid;
}

void Box3DPhysicsServer3D::body_set_space(RID p_body, RID p_space) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	Box3DSpace3D *space = nullptr;
	if (p_space.is_valid()) {
		space = space_owner.get_or_null(p_space);
		ERR_FAIL_NULL(space);
	}
	body->set_space_rid(p_space);
	body->set_space(space);
}

RID Box3DPhysicsServer3D::body_get_space(RID p_body) const {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, RID());
	return body->get_space_rid();
}

void Box3DPhysicsServer3D::body_set_mode(RID p_body, BodyMode p_mode) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->set_mode(p_mode);
}

PhysicsServer3D::BodyMode Box3DPhysicsServer3D::body_get_mode(RID p_body) const {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, BODY_MODE_STATIC);
	return body->get_mode();
}

void Box3DPhysicsServer3D::body_add_shape(RID p_body, RID p_shape, const Transform3D &p_transform, bool p_disabled) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	Box3DShape3D *shape = shape_owner.get_or_null(p_shape);
	ERR_FAIL_NULL(shape);
	body->add_shape(p_shape, shape, p_transform, p_disabled);
}

void Box3DPhysicsServer3D::body_set_shape(RID p_body, int p_shape_idx, RID p_shape) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	Box3DShape3D *shape = shape_owner.get_or_null(p_shape);
	ERR_FAIL_NULL(shape);
	body->set_shape(p_shape_idx, p_shape, shape);
}

void Box3DPhysicsServer3D::body_set_shape_transform(RID p_body, int p_shape_idx, const Transform3D &p_transform) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->set_shape_transform(p_shape_idx, p_transform);
}

void Box3DPhysicsServer3D::body_set_shape_disabled(RID p_body, int p_shape_idx, bool p_disabled) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->set_shape_disabled(p_shape_idx, p_disabled);
}

void Box3DPhysicsServer3D::body_set_surface_material(RID p_body, int p_shape_idx, int p_material_id) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	ERR_FAIL_COND_MSG(!_can_mutate_body_shapes(body), "Box3D: surface material changes are inaccessible right now, wait for iteration or physics process notification.");
	body->set_surface_material(p_shape_idx, p_material_id);
}

int Box3DPhysicsServer3D::body_get_shape_count(RID p_body) const {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, 0);
	return body->get_shape_count();
}

RID Box3DPhysicsServer3D::body_get_shape(RID p_body, int p_shape_idx) const {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, RID());
	const Box3DBody3D::ShapeSlot *slot = body->get_shape_slot(p_shape_idx);
	return slot ? slot->rid : RID();
}

Transform3D Box3DPhysicsServer3D::body_get_shape_transform(RID p_body, int p_shape_idx) const {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, Transform3D());
	const Box3DBody3D::ShapeSlot *slot = body->get_shape_slot(p_shape_idx);
	return slot ? slot->xform : Transform3D();
}

void Box3DPhysicsServer3D::body_remove_shape(RID p_body, int p_shape_idx) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->remove_shape_at(p_shape_idx);
}

void Box3DPhysicsServer3D::body_clear_shapes(RID p_body) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->clear_shapes();
}

void Box3DPhysicsServer3D::body_attach_object_instance_id(RID p_body, ObjectID p_id) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->set_instance_id(p_id);
}

ObjectID Box3DPhysicsServer3D::body_get_object_instance_id(RID p_body) const {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, ObjectID());
	return body->get_instance_id();
}

void Box3DPhysicsServer3D::body_set_collision_layer(RID p_body, uint32_t p_layer) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->set_collision_layer(p_layer);
}

uint32_t Box3DPhysicsServer3D::body_get_collision_layer(RID p_body) const {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, 0);
	return body->get_collision_layer();
}

void Box3DPhysicsServer3D::body_set_collision_mask(RID p_body, uint32_t p_mask) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->set_collision_mask(p_mask);
}

uint32_t Box3DPhysicsServer3D::body_get_collision_mask(RID p_body) const {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, 0);
	return body->get_collision_mask();
}

void Box3DPhysicsServer3D::body_set_enable_continuous_collision_detection(RID p_body, bool p_enable) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->set_enable_continuous_collision_detection(p_enable);
}

void Box3DPhysicsServer3D::body_set_param(RID p_body, BodyParameter p_param, const Variant &p_value) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->set_param(p_param, p_value);
}

Variant Box3DPhysicsServer3D::body_get_param(RID p_body, BodyParameter p_param) const {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, Variant());
	return body->get_param(p_param);
}

void Box3DPhysicsServer3D::body_set_state(RID p_body, BodyState p_state, const Variant &p_variant) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	switch (p_state) {
		case BODY_STATE_TRANSFORM:
			body->set_transform(p_variant);
			break;
		case BODY_STATE_LINEAR_VELOCITY:
			body->set_linear_velocity(p_variant);
			break;
		case BODY_STATE_ANGULAR_VELOCITY:
			body->set_angular_velocity(p_variant);
			break;
		case BODY_STATE_SLEEPING:
			body->set_sleep_state(p_variant);
			break;
		case BODY_STATE_CAN_SLEEP:
			body->set_can_sleep(p_variant);
			break;
	}
}

Variant Box3DPhysicsServer3D::body_get_state(RID p_body, BodyState p_state) const {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, Variant());
	switch (p_state) {
		case BODY_STATE_TRANSFORM:
			return body->get_transform();
		case BODY_STATE_LINEAR_VELOCITY:
			return body->get_linear_velocity();
		case BODY_STATE_ANGULAR_VELOCITY:
			return body->get_angular_velocity();
		case BODY_STATE_SLEEPING:
			return body->is_sleeping();
		case BODY_STATE_CAN_SLEEP:
			return body->get_can_sleep();
	}
	return Variant();
}

void Box3DPhysicsServer3D::body_apply_central_impulse(RID p_body, const Vector3 &p_impulse) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->apply_central_impulse(p_impulse);
}

void Box3DPhysicsServer3D::body_apply_impulse(RID p_body, const Vector3 &p_impulse, const Vector3 &p_position) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->apply_impulse(p_impulse, p_position);
}

void Box3DPhysicsServer3D::body_apply_torque_impulse(RID p_body, const Vector3 &p_impulse) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->apply_torque_impulse(p_impulse);
}

void Box3DPhysicsServer3D::body_apply_central_force(RID p_body, const Vector3 &p_force) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->apply_central_force(p_force);
}

void Box3DPhysicsServer3D::body_apply_force(RID p_body, const Vector3 &p_force, const Vector3 &p_position) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->apply_force(p_force, p_position);
}

void Box3DPhysicsServer3D::body_apply_torque(RID p_body, const Vector3 &p_torque) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->apply_torque(p_torque);
}

void Box3DPhysicsServer3D::body_add_constant_central_force(RID p_body, const Vector3 &p_force) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->add_constant_central_force(p_force);
}

void Box3DPhysicsServer3D::body_add_constant_force(RID p_body, const Vector3 &p_force, const Vector3 &p_position) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->add_constant_force(p_force, p_position);
}

void Box3DPhysicsServer3D::body_add_constant_torque(RID p_body, const Vector3 &p_torque) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->add_constant_torque(p_torque);
}

void Box3DPhysicsServer3D::body_set_constant_force(RID p_body, const Vector3 &p_force) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->set_constant_force(p_force);
}

Vector3 Box3DPhysicsServer3D::body_get_constant_force(RID p_body) const {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, Vector3());
	return body->get_constant_force();
}

void Box3DPhysicsServer3D::body_set_constant_torque(RID p_body, const Vector3 &p_torque) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->set_constant_torque(p_torque);
}

Vector3 Box3DPhysicsServer3D::body_get_constant_torque(RID p_body) const {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, Vector3());
	return body->get_constant_torque();
}

void Box3DPhysicsServer3D::body_set_max_contacts_reported(RID p_body, int p_contacts) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->set_max_contacts_reported(p_contacts);
}

int Box3DPhysicsServer3D::body_get_max_contacts_reported(RID p_body) const {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, 0);
	return body->get_max_contacts_reported();
}

void Box3DPhysicsServer3D::body_set_contacts_reported_depth_threshold(RID p_body, real_t p_threshold) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->set_contacts_reported_depth_threshold(p_threshold);
}

real_t Box3DPhysicsServer3D::body_get_contacts_reported_depth_threshold(RID p_body) const {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, 0.0);
	return body->get_contacts_reported_depth_threshold();
}

void Box3DPhysicsServer3D::body_set_omit_force_integration(RID p_body, bool p_omit) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->set_omit_force_integration(p_omit);
}

bool Box3DPhysicsServer3D::body_is_omitting_force_integration(RID p_body) const {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, false);
	return body->is_omitting_force_integration();
}

void Box3DPhysicsServer3D::body_set_state_sync_callback(RID p_body, const Callable &p_callable) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->set_state_sync_callback(p_callable);
}

void Box3DPhysicsServer3D::body_set_force_integration_callback(RID p_body, const Callable &p_callable, const Variant &p_udata) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->set_force_integration_callback(p_callable, p_udata);
}

PhysicsDirectBodyState3D *Box3DPhysicsServer3D::body_get_direct_state(RID p_body) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, nullptr);
	ERR_FAIL_COND_V_MSG((using_threads && !doing_sync) || (body->get_space() && body->get_space()->is_stepping()), nullptr, "Body state is inaccessible right now, wait for iteration or physics process notification.");
	return body->get_direct_state();
}

// --- Lifecycle ---

void Box3DPhysicsServer3D::free_rid(RID p_rid) {
	if (Box3DShape3D *shape = shape_owner.get_or_null(p_rid)) {
		// Detach from every object still using it.
		while (shape->get_owners().size() > 0) {
			Box3DCollisionObject3D *object = *shape->get_owners().begin();
			if (object->get_type() == Box3DCollisionObject3D::TYPE_BODY) {
				static_cast<Box3DBody3D *>(object)->remove_shape(shape);
			} else if (object->get_type() == Box3DCollisionObject3D::TYPE_AREA) {
				static_cast<Box3DArea3D *>(object)->remove_shape(shape);
			}
		}
		shape_owner.free(p_rid);
		memdelete(shape);
	} else if (Box3DBody3D *body = body_owner.get_or_null(p_rid)) {
		body->clear_shapes();
		body->set_space(nullptr);
		body_owner.free(p_rid);
		memdelete(body);
	} else if (Box3DArea3D *area = area_owner.get_or_null(p_rid)) {
		area_owner.free(p_rid);
		memdelete(area);
	} else if (Box3DSpace3D *space = space_owner.get_or_null(p_rid)) {
		active_spaces.erase(space);
		space_owner.free(p_rid);
		memdelete(space);
	} else {
		ERR_FAIL_MSG("Box3D: attempted to free an invalid RID.");
	}
}

void Box3DPhysicsServer3D::step(real_t p_step) {
	if (!active) {
		return;
	}
	for (Box3DSpace3D *space : active_spaces) {
		space->step(p_step);
	}
}

void Box3DPhysicsServer3D::flush_queries() {
	if (!active) {
		return;
	}
	flushing_queries = true;
	for (Box3DSpace3D *space : active_spaces) {
		space->call_queries();
	}
	flushing_queries = false;
}
