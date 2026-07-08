/**************************************************************************/
/*  box3d_ragdoll.h                                                       */
/**************************************************************************/

#pragma once

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/io/resource.h"
#include "scene/3d/skeleton_modifier_3d.h"

#include "box3d/box3d.h"

class Box3DBody3D;
class Box3DPhysicsServer3D;
class PhysicsDirectBodyState3D;

class Box3DRagdollProfile : public Resource {
	GDCLASS(Box3DRagdollProfile, Resource);

	TypedArray<Dictionary> bones;
	Array filter_pairs;
	uint32_t collision_layer = 1;
	uint32_t collision_mask = 1;
	real_t friction_torque = 8.0;
	real_t spring_hertz = 1.0;
	real_t spring_damping_ratio = 0.7;

protected:
	static void _bind_methods();

public:
	enum JointType {
		JOINT_TYPE_NONE,
		JOINT_TYPE_SPHERICAL,
		JOINT_TYPE_REVOLUTE,
	};

	void set_bones(const TypedArray<Dictionary> &p_bones) { bones = p_bones; }
	TypedArray<Dictionary> get_bones() const { return bones; }
	void set_filter_pairs(const Array &p_pairs) { filter_pairs = p_pairs; }
	Array get_filter_pairs() const { return filter_pairs; }
	void set_collision_layer(uint32_t p_layer) { collision_layer = p_layer; }
	uint32_t get_collision_layer() const { return collision_layer; }
	void set_collision_mask(uint32_t p_mask) { collision_mask = p_mask; }
	uint32_t get_collision_mask() const { return collision_mask; }
	void set_friction_torque(real_t p_torque) { friction_torque = MAX((real_t)0.0, p_torque); }
	real_t get_friction_torque() const { return friction_torque; }
	void set_spring_hertz(real_t p_hertz) { spring_hertz = MAX((real_t)0.0, p_hertz); }
	real_t get_spring_hertz() const { return spring_hertz; }
	void set_spring_damping_ratio(real_t p_ratio) { spring_damping_ratio = MAX((real_t)0.0, p_ratio); }
	real_t get_spring_damping_ratio() const { return spring_damping_ratio; }
};

VARIANT_ENUM_CAST(Box3DRagdollProfile::JointType);

class Box3DRagdoll : public SkeletonModifier3D {
	GDCLASS(Box3DRagdoll, SkeletonModifier3D);

	Ref<Box3DRagdollProfile> profile;

	struct BoneRuntime {
		StringName name;
		int bone = -1;
		int parent = -1;
		int parent_runtime = -1;
		Box3DRagdollProfile::JointType joint_type = Box3DRagdollProfile::JOINT_TYPE_SPHERICAL;
		Transform3D offset;
		real_t radius = 0.12;
		real_t height = 0.5;
		real_t density_scale = 1.0;
		real_t swing_limit = Math::deg_to_rad((real_t)25.0);
		real_t twist_lower = Math::deg_to_rad((real_t)-15.0);
		real_t twist_upper = Math::deg_to_rad((real_t)15.0);
		real_t joint_friction_scale = 1.0;
		real_t blend = 1.0;
		RID body;
		RID shape;
		b3JointId joint = {};
		Transform3D previous_pose;
		Transform3D current_pose;
		bool has_pose_history = false;
		uint32_t pose_capture_count = 0;
	};

	LocalVector<BoneRuntime> bones;
	LocalVector<b3JointId> filter_joints;
	HashMap<StringName, int> bone_lookup;
	RID space_rid;
	int group_index = 0;
	bool built = false;
	bool ragdoll_active = false;
	bool asleep_emitted = false;
	real_t last_capture_delta = 1.0 / 60.0;

	static int next_group_index;

	void _clear_native_joints();
	void _clear_bodies();
	void _capture_animation_pose(real_t p_delta);
	Dictionary _profile_entry_for_bone(const StringName &p_name) const;
	void _load_runtime_from_profile(BoneRuntime &r_bone, const Dictionary &p_entry) const;
	Transform3D _bone_world_pose(Skeleton3D *p_skeleton, const BoneRuntime &p_bone) const;
	void _set_body_transform(Box3DPhysicsServer3D *p_server, BoneRuntime &r_bone, const Transform3D &p_transform);
	bool _create_body_for_bone(Box3DPhysicsServer3D *p_server, Skeleton3D *p_skeleton, BoneRuntime &r_bone);
	void _create_joint_for_bone(Box3DPhysicsServer3D *p_server, BoneRuntime &r_bone);
	void _create_filter_joints(Box3DPhysicsServer3D *p_server);
	void _seed_body_velocity(Box3DPhysicsServer3D *p_server, BoneRuntime &r_bone, real_t p_delta);
	real_t _bone_mass(const BoneRuntime &p_bone) const;
	bool _has_velocity_history() const;
	void _sync_skeleton_from_bodies(Box3DPhysicsServer3D *p_server, Skeleton3D *p_skeleton);
	bool _all_bodies_sleeping(Box3DPhysicsServer3D *p_server) const;
	Transform3D _remap_twist_x_to_z(const Transform3D &p_frame) const;
	void _body_state_changed(PhysicsDirectBodyState3D *p_state);

protected:
	static void _bind_methods();
	void _notification(int p_what);
	virtual void _process_modification(double p_delta) override;

public:
	void set_profile(const Ref<Box3DRagdollProfile> &p_profile);
	Ref<Box3DRagdollProfile> get_profile() const { return profile; }

	bool build();
	void teardown();
	void die(const Vector3 &p_impulse = Vector3(), const StringName &p_hit_bone = StringName(), real_t p_ramp_time = 0.0);
	void revive();
	bool is_built() const { return built; }
	bool is_ragdoll_active() const { return ragdoll_active; }
	RID get_bone_body(const StringName &p_bone) const;
	Transform3D get_bone_global_transform(const StringName &p_bone) const;
	Vector3 get_bone_linear_velocity(const StringName &p_bone) const;
	Vector3 get_center_of_mass_velocity() const;
	bool are_bodies_sleeping() const;

	virtual bool has_process() const override { return true; }
	~Box3DRagdoll();
};
