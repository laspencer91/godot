/**************************************************************************/
/*  box3d_ragdoll.h                                                       */
/**************************************************************************/

#pragma once

#include <thirdparty/box3d/include/box3d/box3d.h>

#include "core/io/resource.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/3d/skeleton_modifier_3d.h"

class Box3DBody3D;
class Box3DPhysicsServer3D;
class PhysicsDirectBodyState3D;
class AnimationLibrary;
class MeshInstance3D;
class Skeleton3D;

class Box3DRagdollProfile : public Resource {
	GDCLASS(Box3DRagdollProfile, Resource);

	TypedArray<Dictionary> bones;
	Array filter_pairs;
	Dictionary bone_chains;
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

	// Canonical bone-entry schema: every consumer of a profile bone Dictionary
	// (runtime build, gizmo drawing, inspector, mass estimation) must parse it
	// through parse_bone_entry() so keys and defaults stay in one place.
	struct BoneParams {
		Transform3D offset;
		Transform3D joint_frame;
		// Rotation from the rest pose to the joint's neutral pose, expressed in
		// the joint frame. Generated profiles center this on the mean animation
		// pose so limits are symmetric ranges around it.
		Quaternion rest_delta;
		bool has_joint_frame = false;
		bool enabled = true;
		JointType joint_type = JOINT_TYPE_SPHERICAL;
		real_t radius = 0.12;
		real_t height = 0.5;
		real_t density_scale = 1.0;
		real_t swing_limit = Math::deg_to_rad((real_t)25.0);
		real_t twist_lower = Math::deg_to_rad((real_t)-15.0);
		real_t twist_upper = Math::deg_to_rad((real_t)15.0);
		real_t joint_friction_scale = 1.0;
		real_t blend = 1.0;
	};
	static BoneParams parse_bone_entry(const Dictionary &p_entry);

	HashMap<StringName, StringName> build_bone_to_chain_map() const;
	static StringName ungrouped_chain_name();

	void set_bones(const TypedArray<Dictionary> &p_bones) {
		bones = p_bones;
		emit_changed();
	}
	TypedArray<Dictionary> get_bones() const { return bones; }
	void set_filter_pairs(const Array &p_pairs) {
		filter_pairs = p_pairs;
		emit_changed();
	}
	Array get_filter_pairs() const { return filter_pairs; }
	void set_bone_chains(const Dictionary &p_chains) {
		bone_chains = p_chains;
		emit_changed();
	}
	Dictionary get_bone_chains() const { return bone_chains; }
	void set_collision_layer(uint32_t p_layer) {
		collision_layer = p_layer;
		emit_changed();
	}
	uint32_t get_collision_layer() const { return collision_layer; }
	void set_collision_mask(uint32_t p_mask) {
		collision_mask = p_mask;
		emit_changed();
	}
	uint32_t get_collision_mask() const { return collision_mask; }
	void set_friction_torque(real_t p_torque) {
		friction_torque = MAX((real_t)0.0, p_torque);
		emit_changed();
	}
	real_t get_friction_torque() const { return friction_torque; }
	void set_spring_hertz(real_t p_hertz) {
		spring_hertz = MAX((real_t)0.0, p_hertz);
		emit_changed();
	}
	real_t get_spring_hertz() const { return spring_hertz; }
	void set_spring_damping_ratio(real_t p_ratio) {
		spring_damping_ratio = MAX((real_t)0.0, p_ratio);
		emit_changed();
	}
	real_t get_spring_damping_ratio() const { return spring_damping_ratio; }

	real_t estimate_bone_mass(const Dictionary &p_bone) const;
	real_t estimate_total_mass() const;
};

VARIANT_ENUM_CAST(Box3DRagdollProfile::JointType);

class Box3DRagdoll : public SkeletonModifier3D {
	GDCLASS(Box3DRagdoll, SkeletonModifier3D);

	Ref<Box3DRagdollProfile> profile;
	// Uniform size multiplier for the built ragdoll: capsule dimensions, mass, body positions, and joint
	// anchors are all scaled by this at build(). 1.0 reproduces the profile exactly; a per-instance value
	// (e.g. a deterministic per-zombie variation) makes bigger/smaller corpses from ONE profile. The
	// skeleton->mesh sync propagates the scaled bone poses to the skin, so this also drives visible size.
	real_t build_scale = 1.0;

	struct BoneRuntime {
		StringName name;
		int bone = -1;
		int parent = -1;
		int parent_runtime = -1;
		Box3DRagdollProfile::JointType joint_type = Box3DRagdollProfile::JOINT_TYPE_SPHERICAL;
		Transform3D offset;
		Transform3D joint_frame;
		Quaternion rest_delta;
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
		bool has_joint_frame = false;
		uint32_t pose_capture_count = 0;
	};

	LocalVector<BoneRuntime> bones;
	LocalVector<b3JointId> filter_joints;
	HashMap<StringName, int> bone_lookup;
	RID space_rid;
	bool built = false;
	bool ragdoll_active = false;
	bool asleep_emitted = false;
	real_t last_capture_delta = 1.0 / 60.0;

#ifdef TOOLS_ENABLED
	RID editor_space_rid;
	RID editor_ground_body;
	RID editor_ground_shape;
	LocalVector<Transform3D> editor_pose_snapshot;
	bool simulate_in_editor = false;
	bool editor_rebuild_queued = false;
	real_t editor_step_accumulator = 0.0;
#endif

	void _clear_native_joints();
	void _clear_bodies();
	void _capture_animation_pose(real_t p_delta);
	Dictionary _profile_entry_for_bone(const StringName &p_name) const;
	void _load_runtime_from_profile(BoneRuntime &r_bone, const Dictionary &p_entry) const;
	Transform3D _bone_world_pose(Skeleton3D *p_skeleton, const BoneRuntime &p_bone) const;
	void _set_body_transform(Box3DPhysicsServer3D *p_server, BoneRuntime &r_bone, const Transform3D &p_transform);
	bool _create_body_for_bone(Box3DPhysicsServer3D *p_server, Skeleton3D *p_skeleton, BoneRuntime &r_bone);
	void _create_joint_for_bone(Box3DPhysicsServer3D *p_server, Skeleton3D *p_skeleton, BoneRuntime &r_bone);
	void _create_filter_joints(Box3DPhysicsServer3D *p_server);
	void _seed_body_velocity(Box3DPhysicsServer3D *p_server, BoneRuntime &r_bone, real_t p_delta);
	real_t _bone_mass(const BoneRuntime &p_bone) const;
	bool _has_velocity_history() const;
	void _sync_skeleton_from_bodies(Box3DPhysicsServer3D *p_server, Skeleton3D *p_skeleton);
	bool _all_bodies_sleeping(Box3DPhysicsServer3D *p_server) const;
	Transform3D _remap_twist_x_to_z(const Transform3D &p_frame) const;
	void _body_state_changed(PhysicsDirectBodyState3D *p_state);
	bool _build_in_space(RID p_space);
	void _attach_disconnected_components(Skeleton3D *p_skeleton);
	void _teardown_impl();

#ifdef TOOLS_ENABLED
	void _profile_changed();
	void _rebuild_editor_simulation();
	bool _start_editor_simulation();
	void _stop_editor_simulation(bool p_restore_pose);
	bool _create_editor_ground(Box3DPhysicsServer3D *p_server, Skeleton3D *p_skeleton);
	void _step_editor_simulation(real_t p_delta);
#endif

protected:
	static void _bind_methods();
	void _notification(int p_what);
	virtual void _process_modification(double p_delta) override;

public:
	void set_profile(const Ref<Box3DRagdollProfile> &p_profile);
	Ref<Box3DRagdollProfile> get_profile() const { return profile; }
	void set_build_scale(real_t p_scale);
	real_t get_build_scale() const { return build_scale; }

	bool build();
	void teardown();
	void die(const Vector3 &p_impulse = Vector3(), const StringName &p_hit_bone = StringName(), real_t p_ramp_time = 0.0);
	void revive();
	bool is_built() const { return built; }
	bool is_ragdoll_active() const { return ragdoll_active; }
	RID get_bone_body(const StringName &p_bone) const;
	StringName get_bone_joint_parent(const StringName &p_bone) const;
	Transform3D get_bone_global_transform(const StringName &p_bone) const;
	Vector3 get_bone_linear_velocity(const StringName &p_bone) const;
	Vector3 get_center_of_mass_velocity() const;
	bool are_bodies_sleeping() const;

#ifdef TOOLS_ENABLED
	void set_simulate_in_editor(bool p_enabled);
	bool is_simulating_in_editor() const { return simulate_in_editor; }
	void reset_simulation();
#endif

	virtual bool has_process() const override { return true; }
	~Box3DRagdoll();
};

class Box3DRagdollProfileGenerator : public RefCounted {
	GDCLASS(Box3DRagdollProfileGenerator, RefCounted);

	PackedStringArray warnings;
	real_t vertex_weight_threshold = 0.5;
	real_t animation_padding = Math::deg_to_rad((real_t)10.0);
	real_t minimum_bone_length = 0.08;
	real_t prune_bone_length = 0.06;
	real_t target_total_mass = 0.0;
	real_t minimum_radius = 0.04;
	real_t fallback_radius_ratio = 0.2;
	real_t maximum_adjacent_mass_ratio = 10.0;
	real_t maximum_swing_limit = Math::deg_to_rad((real_t)80.0);

	void _warn(const String &p_warning);
	StringName _chain_for_bone(const String &p_bone) const;
	int _track_bone_index(Skeleton3D *p_skeleton, const NodePath &p_path) const;
	Vector3 _bone_axis(Skeleton3D *p_skeleton, int p_bone) const;
	void _fit_bone_capsule(Skeleton3D *p_skeleton, int p_bone, const LocalVector<Vector3> *p_vertices, Vector3 &r_axis, real_t &r_length, real_t &r_radius) const;
	void _collect_weighted_vertices(Skeleton3D *p_skeleton, MeshInstance3D *p_mesh_instance, HashMap<int, LocalVector<Vector3>> &r_vertices);
	void _apply_animation_limits(Skeleton3D *p_skeleton, const Ref<AnimationLibrary> &p_animation_library, HashMap<StringName, Dictionary> &r_entries);
	void _clamp_adjacent_mass_ratios(Skeleton3D *p_skeleton, HashMap<StringName, Dictionary> &r_entries);

protected:
	static void _bind_methods();

public:
	void set_vertex_weight_threshold(real_t p_threshold) { vertex_weight_threshold = CLAMP(p_threshold, (real_t)0.0, (real_t)1.0); }
	real_t get_vertex_weight_threshold() const { return vertex_weight_threshold; }
	void set_animation_padding(real_t p_padding) { animation_padding = MAX((real_t)0.0, p_padding); }
	real_t get_animation_padding() const { return animation_padding; }
	void set_minimum_bone_length(real_t p_length) { minimum_bone_length = MAX((real_t)0.001, p_length); }
	real_t get_minimum_bone_length() const { return minimum_bone_length; }
	void set_prune_bone_length(real_t p_length) { prune_bone_length = MAX((real_t)0.0, p_length); }
	real_t get_prune_bone_length() const { return prune_bone_length; }
	void set_target_total_mass(real_t p_mass) { target_total_mass = MAX((real_t)0.0, p_mass); }
	real_t get_target_total_mass() const { return target_total_mass; }
	void set_minimum_radius(real_t p_radius) { minimum_radius = MAX((real_t)0.001, p_radius); }
	real_t get_minimum_radius() const { return minimum_radius; }
	void set_fallback_radius_ratio(real_t p_ratio) { fallback_radius_ratio = MAX((real_t)0.01, p_ratio); }
	real_t get_fallback_radius_ratio() const { return fallback_radius_ratio; }
	void set_maximum_adjacent_mass_ratio(real_t p_ratio) { maximum_adjacent_mass_ratio = MAX((real_t)1.0, p_ratio); }
	real_t get_maximum_adjacent_mass_ratio() const { return maximum_adjacent_mass_ratio; }
	void set_maximum_swing_limit(real_t p_limit) { maximum_swing_limit = CLAMP(p_limit, (real_t)0.0, (real_t)Math::PI); }
	real_t get_maximum_swing_limit() const { return maximum_swing_limit; }

	PackedStringArray get_warnings() const { return warnings; }
	void clear_warnings() { warnings.clear(); }
	Ref<Box3DRagdollProfile> generate_profile(Skeleton3D *p_skeleton, MeshInstance3D *p_mesh_instance = nullptr, const Ref<AnimationLibrary> &p_animation_library = Ref<AnimationLibrary>());
	Dictionary analyze_profile(const Ref<Box3DRagdollProfile> &p_profile) const;
	Dictionary get_gizmo_line_groups(const Ref<Box3DRagdollProfile> &p_profile, Skeleton3D *p_skeleton) const;
	PackedVector3Array get_gizmo_lines(const Ref<Box3DRagdollProfile> &p_profile, Skeleton3D *p_skeleton) const;
};
