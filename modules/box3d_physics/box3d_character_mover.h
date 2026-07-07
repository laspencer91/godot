/**************************************************************************/
/*  box3d_character_mover.h                                               */
/**************************************************************************/

#pragma once

#include "core/object/ref_counted.h"
#include "core/templates/hash_set.h"
#include "core/variant/typed_array.h"

#include "box3d/box3d.h"

class Box3DSpace3D;

class Box3DCharacterMover : public RefCounted {
	GDCLASS(Box3DCharacterMover, RefCounted);

	RID space_rid;
	b3Capsule capsule = {};
	uint32_t collision_mask = 1;
	real_t floor_max_angle = 0.7853981633974483;
	HashSet<RID> exclusions;

	static void _bind_methods();

	Box3DSpace3D *_get_space() const;
	b3QueryFilter _make_filter() const;
	bool _can_query() const;
	Array _collide_internal(const Vector3 &p_position) const;

public:
	Box3DCharacterMover();

	void setup(RID p_space);
	void set_capsule(float p_height, float p_radius);
	void set_collision_mask(uint32_t p_mask);
	uint32_t get_collision_mask() const { return collision_mask; }
	void set_floor_max_angle(real_t p_angle);
	real_t get_floor_max_angle() const { return floor_max_angle; }
	void set_exclusions(const TypedArray<RID> &p_bodies);

	float cast_motion(const Vector3 &p_position, const Vector3 &p_translation) const;
	Array collide(const Vector3 &p_position) const;
	Dictionary solve_planes(const Vector3 &p_target_delta, const Array &p_planes) const;
	Vector3 clip_velocity(const Vector3 &p_velocity, const Array &p_planes) const;
	Dictionary move(const Vector3 &p_position, const Vector3 &p_velocity, float p_delta) const;
};
