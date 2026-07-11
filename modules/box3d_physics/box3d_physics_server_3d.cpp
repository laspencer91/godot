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

RID Box3DPhysicsServer3D::_create_shape(PS3DE::ShapeType p_type) {
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

bool Box3DPhysicsServer3D::_can_mutate_joint(const Box3DJoint3D *p_joint) const {
	return _can_mutate_joint_bodies(p_joint->get_body_a(), p_joint->get_body_b());
}

bool Box3DPhysicsServer3D::_can_mutate_joint_bodies(const Box3DBody3D *p_body_a, const Box3DBody3D *p_body_b) const {
	if (p_body_a != nullptr) {
		Box3DSpace3D *space = p_body_a->get_space();
		if (space != nullptr && !can_access_space(space)) {
			return false;
		}
	}
	if (p_body_b != nullptr) {
		Box3DSpace3D *space = p_body_b->get_space();
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

PS3DE::ShapeType Box3DPhysicsServer3D::shape_get_type(RID p_shape) const {
	Box3DShape3D *shape = shape_owner.get_or_null(p_shape);
	ERR_FAIL_NULL_V(shape, PS3DE::SHAPE_CUSTOM);
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

bool Box3DPhysicsServer3D::space_start_recording(RID p_space, int p_byte_capacity) {
	Box3DSpace3D *space = space_owner.get_or_null(p_space);
	ERR_FAIL_NULL_V(space, false);
	ERR_FAIL_COND_V_MSG(!can_access_space(space), false, "Box3D: recording changes are inaccessible right now, wait for iteration or physics process notification.");
	return space->start_recording(p_byte_capacity);
}

PackedByteArray Box3DPhysicsServer3D::space_stop_recording(RID p_space) {
	Box3DSpace3D *space = space_owner.get_or_null(p_space);
	ERR_FAIL_NULL_V(space, PackedByteArray());
	ERR_FAIL_COND_V_MSG(!can_access_space(space), PackedByteArray(), "Box3D: recording changes are inaccessible right now, wait for iteration or physics process notification.");
	return space->stop_recording();
}

bool Box3DPhysicsServer3D::space_is_recording(RID p_space) const {
	Box3DSpace3D *space = space_owner.get_or_null(p_space);
	ERR_FAIL_NULL_V(space, false);
	return space->is_recording();
}

int Box3DPhysicsServer3D::space_get_recording_size(RID p_space) const {
	Box3DSpace3D *space = space_owner.get_or_null(p_space);
	ERR_FAIL_NULL_V(space, 0);
	ERR_FAIL_COND_V_MSG(!can_access_space(space), 0, "Box3D: recording data is inaccessible right now, wait for iteration or physics process notification.");
	return space->get_recording_size();
}

bool Box3DPhysicsServer3D::space_save_recording(RID p_space, const String &p_path) const {
	Box3DSpace3D *space = space_owner.get_or_null(p_space);
	ERR_FAIL_NULL_V(space, false);
	ERR_FAIL_COND_V_MSG(!can_access_space(space), false, "Box3D: recording changes are inaccessible right now, wait for iteration or physics process notification.");
	return space->save_recording(p_path);
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

void Box3DPhysicsServer3D::area_set_param(RID p_area, PS3DE::AreaParameter p_param, const Variant &p_value) {
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

Variant Box3DPhysicsServer3D::area_get_param(RID p_area, PS3DE::AreaParameter p_param) const {
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

void Box3DPhysicsServer3D::body_set_mode(RID p_body, PS3DE::BodyMode p_mode) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->set_mode(p_mode);
}

PS3DE::BodyMode Box3DPhysicsServer3D::body_get_mode(RID p_body) const {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, PS3DE::BODY_MODE_STATIC);
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
	if (Box3DBody3D *body = body_owner.get_or_null(p_body)) {
		body->set_instance_id(p_id);
		return;
	}

	if (Box3DSoftBodyPlaceholder *soft_body = soft_body_owner.get_or_null(p_body)) {
		soft_body->set_instance_id(p_id);
		return;
	}

	ERR_FAIL_MSG("Box3D: attempted to attach an instance id to an invalid RID.");
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

void Box3DPhysicsServer3D::body_add_collision_exception(RID p_body, RID p_body_b) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	ERR_FAIL_COND_MSG(!_can_mutate_body_shapes(body), "Box3D: collision exception changes are inaccessible right now, wait for iteration or physics process notification.");
	body->add_collision_exception(p_body_b);
}

void Box3DPhysicsServer3D::body_remove_collision_exception(RID p_body, RID p_body_b) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	ERR_FAIL_COND_MSG(!_can_mutate_body_shapes(body), "Box3D: collision exception changes are inaccessible right now, wait for iteration or physics process notification.");
	body->remove_collision_exception(p_body_b);
}

void Box3DPhysicsServer3D::body_get_collision_exceptions(RID p_body, List<RID> *p_exceptions) {
	const Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	for (const RID &exception : body->get_collision_exceptions()) {
		p_exceptions->push_back(exception);
	}
}

void Box3DPhysicsServer3D::body_set_enable_continuous_collision_detection(RID p_body, bool p_enable) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->set_enable_continuous_collision_detection(p_enable);
}

void Box3DPhysicsServer3D::body_set_param(RID p_body, PS3DE::BodyParameter p_param, const Variant &p_value) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	body->set_param(p_param, p_value);
}

Variant Box3DPhysicsServer3D::body_get_param(RID p_body, PS3DE::BodyParameter p_param) const {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, Variant());
	return body->get_param(p_param);
}

void Box3DPhysicsServer3D::body_set_state(RID p_body, PS3DE::BodyState p_state, const Variant &p_variant) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(body);
	switch (p_state) {
		case PS3DE::BODY_STATE_TRANSFORM:
			body->set_transform(p_variant);
			break;
		case PS3DE::BODY_STATE_LINEAR_VELOCITY:
			body->set_linear_velocity(p_variant);
			break;
		case PS3DE::BODY_STATE_ANGULAR_VELOCITY:
			body->set_angular_velocity(p_variant);
			break;
		case PS3DE::BODY_STATE_SLEEPING:
			body->set_sleep_state(p_variant);
			break;
		case PS3DE::BODY_STATE_CAN_SLEEP:
			body->set_can_sleep(p_variant);
			break;
	}
}

Variant Box3DPhysicsServer3D::body_get_state(RID p_body, PS3DE::BodyState p_state) const {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, Variant());
	switch (p_state) {
		case PS3DE::BODY_STATE_TRANSFORM:
			return body->get_transform();
		case PS3DE::BODY_STATE_LINEAR_VELOCITY:
			return body->get_linear_velocity();
		case PS3DE::BODY_STATE_ANGULAR_VELOCITY:
			return body->get_angular_velocity();
		case PS3DE::BODY_STATE_SLEEPING:
			return body->is_sleeping();
		case PS3DE::BODY_STATE_CAN_SLEEP:
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

bool Box3DPhysicsServer3D::body_test_motion(RID p_body, const PS3DT::MotionParameters &p_parameters, PS3DT::MotionResult *r_result) {
	Box3DBody3D *body = body_owner.get_or_null(p_body);
	ERR_FAIL_NULL_V(body, false);
	Box3DSpace3D *space = body->get_space();
	ERR_FAIL_NULL_V(space, false);
	ERR_FAIL_COND_V_MSG((using_threads && !doing_sync) || space->is_stepping(), false, "body_test_motion is inaccessible right now, wait for iteration or physics process notification.");
	return space->get_direct_state()->body_test_motion(*body, p_parameters, r_result);
}

// --- Joints ---

RID Box3DPhysicsServer3D::joint_create() {
	Box3DJoint3D *joint = memnew(Box3DJoint3D);
	RID rid = joint_owner.make_rid(joint);
	joint->set_rid(rid);
	return rid;
}

void Box3DPhysicsServer3D::joint_clear(RID p_joint) {
	Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	ERR_FAIL_COND_MSG(!_can_mutate_joint(joint), "Box3D: joint changes are inaccessible right now, wait for iteration or physics process notification.");
	joint->clear();
}

PS3DE::JointType Box3DPhysicsServer3D::joint_get_type(RID p_joint) const {
	const Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL_V(joint, PS3DE::JOINT_TYPE_PIN);
	return joint->get_type();
}

void Box3DPhysicsServer3D::joint_set_solver_priority(RID p_joint, int p_priority) {
	Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	joint->set_solver_priority(p_priority);
}

int Box3DPhysicsServer3D::joint_get_solver_priority(RID p_joint) const {
	const Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL_V(joint, 0);
	return joint->get_solver_priority();
}

void Box3DPhysicsServer3D::joint_disable_collisions_between_bodies(RID p_joint, bool p_disable) {
	Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	ERR_FAIL_COND_MSG(!_can_mutate_joint(joint), "Box3D: joint changes are inaccessible right now, wait for iteration or physics process notification.");
	joint->set_collision_disabled(p_disable);
}

bool Box3DPhysicsServer3D::joint_is_disabled_collisions_between_bodies(RID p_joint) const {
	const Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL_V(joint, false);
	return joint->is_collision_disabled();
}

void Box3DPhysicsServer3D::joint_make_pin(RID p_joint, RID p_body_A, const Vector3 &p_local_A, RID p_body_B, const Vector3 &p_local_B) {
	Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	Box3DBody3D *body_a = body_owner.get_or_null(p_body_A);
	Box3DBody3D *body_b = body_owner.get_or_null(p_body_B);
	ERR_FAIL_NULL(body_a);
	ERR_FAIL_NULL_MSG(body_b, "Box3D: world-body joints are not supported yet.");
	ERR_FAIL_COND(body_a == body_b);
	ERR_FAIL_COND_MSG(!_can_mutate_joint(joint), "Box3D: joint changes are inaccessible right now, wait for iteration or physics process notification.");
	ERR_FAIL_COND_MSG(!_can_mutate_joint_bodies(body_a, body_b), "Box3D: joint changes are inaccessible right now, wait for iteration or physics process notification.");
	joint->make_pin(body_a, body_b, p_local_A, p_local_B);
}

void Box3DPhysicsServer3D::pin_joint_set_param(RID p_joint, PS3DE::PinJointParam p_param, real_t p_value) {
	Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	ERR_FAIL_COND(joint->get_type() != PS3DE::JOINT_TYPE_PIN);
	joint->pin_set_param(p_param, p_value);
}

real_t Box3DPhysicsServer3D::pin_joint_get_param(RID p_joint, PS3DE::PinJointParam p_param) const {
	const Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL_V(joint, 0.0);
	ERR_FAIL_COND_V(joint->get_type() != PS3DE::JOINT_TYPE_PIN, 0.0);
	return joint->pin_get_param(p_param);
}

void Box3DPhysicsServer3D::pin_joint_set_local_a(RID p_joint, const Vector3 &p_A) {
	Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	ERR_FAIL_COND(joint->get_type() != PS3DE::JOINT_TYPE_PIN);
	ERR_FAIL_COND_MSG(!_can_mutate_joint(joint), "Box3D: joint changes are inaccessible right now, wait for iteration or physics process notification.");
	joint->pin_set_local_a(p_A);
}

Vector3 Box3DPhysicsServer3D::pin_joint_get_local_a(RID p_joint) const {
	const Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL_V(joint, Vector3());
	ERR_FAIL_COND_V(joint->get_type() != PS3DE::JOINT_TYPE_PIN, Vector3());
	return joint->pin_get_local_a();
}

void Box3DPhysicsServer3D::pin_joint_set_local_b(RID p_joint, const Vector3 &p_B) {
	Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	ERR_FAIL_COND(joint->get_type() != PS3DE::JOINT_TYPE_PIN);
	ERR_FAIL_COND_MSG(!_can_mutate_joint(joint), "Box3D: joint changes are inaccessible right now, wait for iteration or physics process notification.");
	joint->pin_set_local_b(p_B);
}

Vector3 Box3DPhysicsServer3D::pin_joint_get_local_b(RID p_joint) const {
	const Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL_V(joint, Vector3());
	ERR_FAIL_COND_V(joint->get_type() != PS3DE::JOINT_TYPE_PIN, Vector3());
	return joint->pin_get_local_b();
}

void Box3DPhysicsServer3D::joint_make_hinge(RID p_joint, RID p_body_A, const Transform3D &p_hinge_A, RID p_body_B, const Transform3D &p_hinge_B) {
	Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	Box3DBody3D *body_a = body_owner.get_or_null(p_body_A);
	Box3DBody3D *body_b = body_owner.get_or_null(p_body_B);
	ERR_FAIL_NULL(body_a);
	ERR_FAIL_NULL_MSG(body_b, "Box3D: world-body joints are not supported yet.");
	ERR_FAIL_COND(body_a == body_b);
	ERR_FAIL_COND_MSG(!_can_mutate_joint(joint), "Box3D: joint changes are inaccessible right now, wait for iteration or physics process notification.");
	ERR_FAIL_COND_MSG(!_can_mutate_joint_bodies(body_a, body_b), "Box3D: joint changes are inaccessible right now, wait for iteration or physics process notification.");
	joint->make_hinge(body_a, body_b, p_hinge_A, p_hinge_B);
}

void Box3DPhysicsServer3D::joint_make_hinge_simple(RID p_joint, RID p_body_A, const Vector3 &p_pivot_A, const Vector3 &p_axis_A, RID p_body_B, const Vector3 &p_pivot_B, const Vector3 &p_axis_B) {
	ERR_FAIL_MSG("Box3D: simple hinge joints are not supported; use joint_make_hinge with explicit local frames.");
}

void Box3DPhysicsServer3D::hinge_joint_set_param(RID p_joint, PS3DE::HingeJointParam p_param, real_t p_value) {
	Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	ERR_FAIL_COND(joint->get_type() != PS3DE::JOINT_TYPE_HINGE);
	ERR_FAIL_COND_MSG(!_can_mutate_joint(joint), "Box3D: joint changes are inaccessible right now, wait for iteration or physics process notification.");
	joint->hinge_set_param(p_param, p_value);
}

real_t Box3DPhysicsServer3D::hinge_joint_get_param(RID p_joint, PS3DE::HingeJointParam p_param) const {
	const Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL_V(joint, 0.0);
	ERR_FAIL_COND_V(joint->get_type() != PS3DE::JOINT_TYPE_HINGE, 0.0);
	return joint->hinge_get_param(p_param);
}

void Box3DPhysicsServer3D::hinge_joint_set_flag(RID p_joint, PS3DE::HingeJointFlag p_flag, bool p_enabled) {
	Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	ERR_FAIL_COND(joint->get_type() != PS3DE::JOINT_TYPE_HINGE);
	ERR_FAIL_COND_MSG(!_can_mutate_joint(joint), "Box3D: joint changes are inaccessible right now, wait for iteration or physics process notification.");
	joint->hinge_set_flag(p_flag, p_enabled);
}

bool Box3DPhysicsServer3D::hinge_joint_get_flag(RID p_joint, PS3DE::HingeJointFlag p_flag) const {
	const Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL_V(joint, false);
	ERR_FAIL_COND_V(joint->get_type() != PS3DE::JOINT_TYPE_HINGE, false);
	return joint->hinge_get_flag(p_flag);
}

void Box3DPhysicsServer3D::joint_make_slider(RID p_joint, RID p_body_A, const Transform3D &p_local_frame_A, RID p_body_B, const Transform3D &p_local_frame_B) {
	Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	Box3DBody3D *body_a = body_owner.get_or_null(p_body_A);
	Box3DBody3D *body_b = body_owner.get_or_null(p_body_B);
	ERR_FAIL_NULL(body_a);
	ERR_FAIL_NULL_MSG(body_b, "Box3D: world-body joints are not supported yet.");
	ERR_FAIL_COND(body_a == body_b);
	ERR_FAIL_COND_MSG(!_can_mutate_joint(joint), "Box3D: joint changes are inaccessible right now, wait for iteration or physics process notification.");
	ERR_FAIL_COND_MSG(!_can_mutate_joint_bodies(body_a, body_b), "Box3D: joint changes are inaccessible right now, wait for iteration or physics process notification.");
	joint->make_slider(body_a, body_b, p_local_frame_A, p_local_frame_B);
}

void Box3DPhysicsServer3D::slider_joint_set_param(RID p_joint, PS3DE::SliderJointParam p_param, real_t p_value) {
	Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	ERR_FAIL_COND(joint->get_type() != PS3DE::JOINT_TYPE_SLIDER);
	ERR_FAIL_COND_MSG(!_can_mutate_joint(joint), "Box3D: joint changes are inaccessible right now, wait for iteration or physics process notification.");
	joint->slider_set_param(p_param, p_value);
}

real_t Box3DPhysicsServer3D::slider_joint_get_param(RID p_joint, PS3DE::SliderJointParam p_param) const {
	const Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL_V(joint, 0.0);
	ERR_FAIL_COND_V(joint->get_type() != PS3DE::JOINT_TYPE_SLIDER, 0.0);
	return joint->slider_get_param(p_param);
}

void Box3DPhysicsServer3D::joint_make_cone_twist(RID p_joint, RID p_body_A, const Transform3D &p_local_frame_A, RID p_body_B, const Transform3D &p_local_frame_B) {
	Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	Box3DBody3D *body_a = body_owner.get_or_null(p_body_A);
	Box3DBody3D *body_b = body_owner.get_or_null(p_body_B);
	ERR_FAIL_NULL(body_a);
	ERR_FAIL_NULL_MSG(body_b, "Box3D: world-body joints are not supported yet.");
	ERR_FAIL_COND(body_a == body_b);
	ERR_FAIL_COND_MSG(!_can_mutate_joint(joint), "Box3D: joint changes are inaccessible right now, wait for iteration or physics process notification.");
	ERR_FAIL_COND_MSG(!_can_mutate_joint_bodies(body_a, body_b), "Box3D: joint changes are inaccessible right now, wait for iteration or physics process notification.");
	joint->make_cone_twist(body_a, body_b, p_local_frame_A, p_local_frame_B);
}

void Box3DPhysicsServer3D::cone_twist_joint_set_param(RID p_joint, PS3DE::ConeTwistJointParam p_param, real_t p_value) {
	Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	ERR_FAIL_COND(joint->get_type() != PS3DE::JOINT_TYPE_CONE_TWIST);
	ERR_FAIL_COND_MSG(!_can_mutate_joint(joint), "Box3D: joint changes are inaccessible right now, wait for iteration or physics process notification.");
	joint->cone_twist_set_param(p_param, p_value);
}

real_t Box3DPhysicsServer3D::cone_twist_joint_get_param(RID p_joint, PS3DE::ConeTwistJointParam p_param) const {
	const Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL_V(joint, 0.0);
	ERR_FAIL_COND_V(joint->get_type() != PS3DE::JOINT_TYPE_CONE_TWIST, 0.0);
	return joint->cone_twist_get_param(p_param);
}

void Box3DPhysicsServer3D::joint_make_generic_6dof(RID p_joint, RID p_body_A, const Transform3D &p_local_frame_A, RID p_body_B, const Transform3D &p_local_frame_B) {
	Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	Box3DBody3D *body_a = body_owner.get_or_null(p_body_A);
	Box3DBody3D *body_b = body_owner.get_or_null(p_body_B);
	ERR_FAIL_NULL(body_a);
	ERR_FAIL_NULL_MSG(body_b, "Box3D: world-body joints are not supported yet.");
	ERR_FAIL_COND(body_a == body_b);
	ERR_FAIL_COND_MSG(!_can_mutate_joint(joint), "Box3D: joint changes are inaccessible right now, wait for iteration or physics process notification.");
	ERR_FAIL_COND_MSG(!_can_mutate_joint_bodies(body_a, body_b), "Box3D: joint changes are inaccessible right now, wait for iteration or physics process notification.");
	joint->make_generic_6dof(body_a, body_b, p_local_frame_A, p_local_frame_B);
}

void Box3DPhysicsServer3D::generic_6dof_joint_set_param(RID p_joint, Vector3::Axis p_axis, PS3DE::G6DOFJointAxisParam p_param, real_t p_value) {
	Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	ERR_FAIL_COND(joint->get_type() != PS3DE::JOINT_TYPE_6DOF);
	ERR_FAIL_COND_MSG(!_can_mutate_joint(joint), "Box3D: joint changes are inaccessible right now, wait for iteration or physics process notification.");
	joint->generic_6dof_set_param(p_axis, p_param, p_value);
}

real_t Box3DPhysicsServer3D::generic_6dof_joint_get_param(RID p_joint, Vector3::Axis p_axis, PS3DE::G6DOFJointAxisParam p_param) const {
	const Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL_V(joint, 0.0);
	ERR_FAIL_COND_V(joint->get_type() != PS3DE::JOINT_TYPE_6DOF, 0.0);
	return joint->generic_6dof_get_param(p_axis, p_param);
}

void Box3DPhysicsServer3D::generic_6dof_joint_set_flag(RID p_joint, Vector3::Axis p_axis, PS3DE::G6DOFJointAxisFlag p_flag, bool p_enable) {
	Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	ERR_FAIL_COND(joint->get_type() != PS3DE::JOINT_TYPE_6DOF);
	ERR_FAIL_COND_MSG(!_can_mutate_joint(joint), "Box3D: joint changes are inaccessible right now, wait for iteration or physics process notification.");
	joint->generic_6dof_set_flag(p_axis, p_flag, p_enable);
}

bool Box3DPhysicsServer3D::generic_6dof_joint_get_flag(RID p_joint, Vector3::Axis p_axis, PS3DE::G6DOFJointAxisFlag p_flag) const {
	const Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL_V(joint, false);
	ERR_FAIL_COND_V(joint->get_type() != PS3DE::JOINT_TYPE_6DOF, false);
	return joint->generic_6dof_get_flag(p_axis, p_flag);
}

void Box3DPhysicsServer3D::joint_set_box3d_param(RID p_joint, Box3DJointParam p_param, real_t p_value) {
	Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	ERR_FAIL_COND_MSG(!_can_mutate_joint(joint), "Box3D: joint changes are inaccessible right now, wait for iteration or physics process notification.");
	joint->set_box3d_param(p_param, p_value);
}

real_t Box3DPhysicsServer3D::joint_get_box3d_param(RID p_joint, Box3DJointParam p_param) const {
	const Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL_V(joint, 0.0);
	return joint->get_box3d_param(p_param);
}

void Box3DPhysicsServer3D::joint_set_box3d_target_rotation(RID p_joint, const Quaternion &p_target_rotation) {
	Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	ERR_FAIL_COND_MSG(!_can_mutate_joint(joint), "Box3D: joint changes are inaccessible right now, wait for iteration or physics process notification.");
	joint->set_box3d_target_rotation(p_target_rotation);
}

Quaternion Box3DPhysicsServer3D::joint_get_box3d_target_rotation(RID p_joint) const {
	const Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL_V(joint, Quaternion());
	return joint->get_box3d_target_rotation();
}

void Box3DPhysicsServer3D::joint_set_box3d_motor_velocity(RID p_joint, const Vector3 &p_velocity) {
	Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL(joint);
	ERR_FAIL_COND_MSG(!_can_mutate_joint(joint), "Box3D: joint changes are inaccessible right now, wait for iteration or physics process notification.");
	joint->set_box3d_motor_velocity(p_velocity);
}

Vector3 Box3DPhysicsServer3D::joint_get_box3d_motor_velocity(RID p_joint) const {
	const Box3DJoint3D *joint = joint_owner.get_or_null(p_joint);
	ERR_FAIL_NULL_V(joint, Vector3());
	return joint->get_box3d_motor_velocity();
}

// --- Soft bodies ---

RID Box3DPhysicsServer3D::soft_body_create() {
	Box3DSoftBodyPlaceholder *soft_body = memnew(Box3DSoftBodyPlaceholder);
	RID rid = soft_body_owner.make_rid(soft_body);
	soft_body->set_rid(rid);
	return rid;
}

void Box3DPhysicsServer3D::soft_body_set_space(RID p_body, RID p_space) {
	Box3DSoftBodyPlaceholder *soft_body = soft_body_owner.get_or_null(p_body);
	ERR_FAIL_NULL(soft_body);

	if (p_space.is_valid()) {
		WARN_PRINT_ONCE("Box3D: SoftBody3D simulation is not implemented; soft bodies will not move or deform.");
	}
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
		while (!body->get_joints().is_empty()) {
			Box3DJoint3D *joint = *body->get_joints().begin();
			joint->clear();
		}
		body->clear_shapes();
		body->set_space(nullptr);
		body_owner.free(p_rid);
		memdelete(body);
	} else if (Box3DSoftBodyPlaceholder *soft_body = soft_body_owner.get_or_null(p_rid)) {
		soft_body_owner.free(p_rid);
		memdelete(soft_body);
	} else if (Box3DJoint3D *joint = joint_owner.get_or_null(p_rid)) {
		joint_owner.free(p_rid);
		memdelete(joint);
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
