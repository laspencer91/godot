/**************************************************************************/
/*  box3d_physics_server_3d.h — PhysicsServer3D backed by Box3D           */
/*                                                                        */
/*  Milestone 1 (static vertical slice + rigid dynamics): spaces, bodies, */
/*  shapes, state sync. Subclasses PhysicsServer3DDummy so unimplemented  */
/*  surface area no-ops safely; methods migrate to real implementations   */
/*  per the scope order in <box3d repo>/llm/06-project-decisions.md.      */
/**************************************************************************/

#pragma once

#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/rid_owner.h"
#include "servers/physics_3d/physics_server_3d_dummy.h"

#include "box3d_body_3d.h"
#include "box3d_shape_3d.h"
#include "box3d_space_3d.h"

// Placeholder area: stores parameters so Area3D nodes don't break; simulation
// semantics (sensors, gravity overrides) arrive in milestone 4.
class Box3DArea3D {
public:
	RID rid;
	RID space_rid;
	Box3DSpace3D *space = nullptr;
	HashMap<int, Variant> params;
};

class Box3DPhysicsServer3D : public PhysicsServer3DDummy {
	GDCLASS(Box3DPhysicsServer3D, PhysicsServer3DDummy);

	inline static Box3DPhysicsServer3D *singleton = nullptr;

	mutable RID_PtrOwner<Box3DSpace3D> space_owner;
	mutable RID_PtrOwner<Box3DArea3D> area_owner;
	mutable RID_PtrOwner<Box3DBody3D> body_owner;
	mutable RID_PtrOwner<Box3DShape3D> shape_owner;

	HashSet<Box3DSpace3D *> active_spaces;

	bool active = true;
	bool flushing_queries = false;
	bool using_threads = false;
	bool doing_sync = false;

	RID _create_shape(ShapeType p_type);

public:
	explicit Box3DPhysicsServer3D(bool p_using_threads = false);
	~Box3DPhysicsServer3D();

	static Box3DPhysicsServer3D *get_singleton() { return singleton; }
	Box3DSpace3D *get_space(RID p_rid) const { return space_owner.get_or_null(p_rid); }
	bool can_access_space(Box3DSpace3D *p_space) const { return p_space != nullptr && !(using_threads && !doing_sync) && !p_space->is_stepping(); }

	// Shapes.
	virtual RID world_boundary_shape_create() override { return _create_shape(SHAPE_WORLD_BOUNDARY); }
	virtual RID separation_ray_shape_create() override { return _create_shape(SHAPE_SEPARATION_RAY); }
	virtual RID sphere_shape_create() override { return _create_shape(SHAPE_SPHERE); }
	virtual RID box_shape_create() override { return _create_shape(SHAPE_BOX); }
	virtual RID capsule_shape_create() override { return _create_shape(SHAPE_CAPSULE); }
	virtual RID cylinder_shape_create() override { return _create_shape(SHAPE_CYLINDER); }
	virtual RID convex_polygon_shape_create() override { return _create_shape(SHAPE_CONVEX_POLYGON); }
	virtual RID concave_polygon_shape_create() override { return _create_shape(SHAPE_CONCAVE_POLYGON); }
	virtual RID heightmap_shape_create() override { return _create_shape(SHAPE_HEIGHTMAP); }
	virtual RID custom_shape_create() override;
	virtual void shape_set_data(RID p_shape, const Variant &p_data) override;
	virtual Variant shape_get_data(RID p_shape) const override;
	virtual ShapeType shape_get_type(RID p_shape) const override;

	// Spaces.
	virtual RID space_create() override;
	virtual void space_set_active(RID p_space, bool p_active) override;
	virtual bool space_is_active(RID p_space) const override;
	virtual PhysicsDirectSpaceState3D *space_get_direct_state(RID p_space) override;

	// Areas (parameter storage only for now; space RID routes to default area).
	virtual RID area_create() override;
	virtual void area_set_space(RID p_area, RID p_space) override;
	virtual RID area_get_space(RID p_area) const override;
	virtual void area_set_param(RID p_area, AreaParameter p_param, const Variant &p_value) override;
	virtual Variant area_get_param(RID p_area, AreaParameter p_param) const override;

	// Bodies.
	virtual RID body_create() override;
	virtual void body_set_space(RID p_body, RID p_space) override;
	virtual RID body_get_space(RID p_body) const override;
	virtual void body_set_mode(RID p_body, BodyMode p_mode) override;
	virtual BodyMode body_get_mode(RID p_body) const override;
	virtual void body_add_shape(RID p_body, RID p_shape, const Transform3D &p_transform = Transform3D(), bool p_disabled = false) override;
	virtual void body_set_shape(RID p_body, int p_shape_idx, RID p_shape) override;
	virtual void body_set_shape_transform(RID p_body, int p_shape_idx, const Transform3D &p_transform) override;
	virtual void body_set_shape_disabled(RID p_body, int p_shape_idx, bool p_disabled) override;
	virtual int body_get_shape_count(RID p_body) const override;
	virtual RID body_get_shape(RID p_body, int p_shape_idx) const override;
	virtual Transform3D body_get_shape_transform(RID p_body, int p_shape_idx) const override;
	virtual void body_remove_shape(RID p_body, int p_shape_idx) override;
	virtual void body_clear_shapes(RID p_body) override;
	virtual void body_attach_object_instance_id(RID p_body, ObjectID p_id) override;
	virtual ObjectID body_get_object_instance_id(RID p_body) const override;
	virtual void body_set_collision_layer(RID p_body, uint32_t p_layer) override;
	virtual uint32_t body_get_collision_layer(RID p_body) const override;
	virtual void body_set_collision_mask(RID p_body, uint32_t p_mask) override;
	virtual uint32_t body_get_collision_mask(RID p_body) const override;
	virtual void body_set_enable_continuous_collision_detection(RID p_body, bool p_enable) override;
	virtual void body_set_param(RID p_body, BodyParameter p_param, const Variant &p_value) override;
	virtual Variant body_get_param(RID p_body, BodyParameter p_param) const override;
	virtual void body_set_state(RID p_body, BodyState p_state, const Variant &p_variant) override;
	virtual Variant body_get_state(RID p_body, BodyState p_state) const override;
	virtual void body_apply_central_impulse(RID p_body, const Vector3 &p_impulse) override;
	virtual void body_apply_impulse(RID p_body, const Vector3 &p_impulse, const Vector3 &p_position = Vector3()) override;
	virtual void body_apply_torque_impulse(RID p_body, const Vector3 &p_impulse) override;
	virtual void body_apply_central_force(RID p_body, const Vector3 &p_force) override;
	virtual void body_apply_force(RID p_body, const Vector3 &p_force, const Vector3 &p_position = Vector3()) override;
	virtual void body_apply_torque(RID p_body, const Vector3 &p_torque) override;
	virtual void body_add_constant_central_force(RID p_body, const Vector3 &p_force) override;
	virtual void body_add_constant_force(RID p_body, const Vector3 &p_force, const Vector3 &p_position = Vector3()) override;
	virtual void body_add_constant_torque(RID p_body, const Vector3 &p_torque) override;
	virtual void body_set_constant_force(RID p_body, const Vector3 &p_force) override;
	virtual Vector3 body_get_constant_force(RID p_body) const override;
	virtual void body_set_constant_torque(RID p_body, const Vector3 &p_torque) override;
	virtual Vector3 body_get_constant_torque(RID p_body) const override;
	virtual void body_set_state_sync_callback(RID p_body, const Callable &p_callable) override;
	virtual PhysicsDirectBodyState3D *body_get_direct_state(RID p_body) override;

	// Lifecycle.
	virtual void free_rid(RID p_rid) override;
	virtual void set_active(bool p_active) override { active = p_active; }
	virtual void step(real_t p_step) override;
	virtual void sync() override { doing_sync = true; }
	virtual void flush_queries() override;
	virtual void end_sync() override { doing_sync = false; }
	virtual bool is_flushing_queries() const override { return flushing_queries; }
};
