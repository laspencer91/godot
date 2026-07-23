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
	real_t push_strength = 1.0;
	real_t step_height = 0.0;
	HashSet<RID> exclusions;

	static void _bind_methods();

	Box3DSpace3D *_get_space() const;
	b3QueryFilter _make_filter() const;
	bool _can_query() const;
	Array _collide_internal(const Vector3 &p_position) const;
	bool _probe_walkable(const Vector3 &p_from, real_t p_length, Vector3 &r_normal, real_t &r_hit_y, int &r_material_id) const;
	bool _ground_probe(const Vector3 &p_feet, const Array &p_planes, Vector3 &r_normal, int &r_material_id) const;
	bool _has_step_obstruction(const Vector3 &p_start, const Vector3 &p_horizontal) const;
	bool _try_step_up(const Vector3 &p_start, const Vector3 &p_target, Vector3 &r_position, Array &r_planes, Vector3 &r_floor_normal, int &r_material_id) const;

public:
	Box3DCharacterMover();

	void setup(RID p_space);
	void set_capsule(float p_height, float p_radius);
	void set_collision_mask(uint32_t p_mask);
	uint32_t get_collision_mask() const { return collision_mask; }
	void set_floor_max_angle(real_t p_angle);
	real_t get_floor_max_angle() const { return floor_max_angle; }
	void set_push_strength(real_t p_strength);
	real_t get_push_strength() const { return push_strength; }
	void set_step_height(real_t p_height);
	real_t get_step_height() const { return step_height; }
	void set_exclusions(const TypedArray<RID> &p_bodies);

	float cast_motion(const Vector3 &p_position, const Vector3 &p_translation) const;
	Array collide(const Vector3 &p_position) const;
	Dictionary solve_planes(const Vector3 &p_target_delta, const Array &p_planes) const;
	Vector3 clip_velocity(const Vector3 &p_velocity, const Array &p_planes) const;
	Dictionary move(const Vector3 &p_position, const Vector3 &p_velocity, float p_delta) const;
};
