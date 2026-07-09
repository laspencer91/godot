/**************************************************************************/
/*  box3d_ragdoll.cpp                                                     */
/**************************************************************************/

#include "box3d_ragdoll.h"

#include "box3d_body_3d.h"
#include "box3d_conversions.h"
#include "box3d_physics_server_3d.h"
#include "box3d_space_3d.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/resources/3d/skin.h"
#include "scene/resources/animation.h"
#include "scene/resources/animation_library.h"
#include "scene/resources/mesh.h"
#include "scene/resources/3d/world_3d.h"

int Box3DRagdoll::next_group_index = -1;

static real_t _dict_real(const Dictionary &p_dict, const StringName &p_key, real_t p_default) {
	return p_dict.has(p_key) ? (real_t)p_dict[p_key] : p_default;
}

static int _dict_int(const Dictionary &p_dict, const StringName &p_key, int p_default) {
	return p_dict.has(p_key) ? (int)p_dict[p_key] : p_default;
}

static bool _dict_bool(const Dictionary &p_dict, const StringName &p_key, bool p_default) {
	return p_dict.has(p_key) ? (bool)p_dict[p_key] : p_default;
}

static Transform3D _dict_transform(const Dictionary &p_dict, const StringName &p_key, const Transform3D &p_default) {
	return p_dict.has(p_key) ? (Transform3D)p_dict[p_key] : p_default;
}

static real_t _capsule_mass(real_t p_radius, real_t p_height, real_t p_density_scale) {
	const real_t radius = MAX((real_t)0.01, p_radius);
	const real_t height = MAX((real_t)0.02, p_height);
	const real_t cylinder_height = MAX((real_t)0.0, height - (real_t)2.0 * radius);
	const real_t volume = Math::PI * radius * radius * cylinder_height + ((real_t)4.0 / (real_t)3.0) * Math::PI * radius * radius * radius;
	return MAX((real_t)0.05, volume * (real_t)1000.0 * MAX((real_t)0.0, p_density_scale));
}

static Vector3 _angular_velocity_between(const Basis &p_from, const Basis &p_to, real_t p_delta) {
	if (p_delta <= CMP_EPSILON) {
		return Vector3();
	}
	Quaternion delta = p_to.get_rotation_quaternion() * p_from.get_rotation_quaternion().inverse();
	delta.normalize();
	Vector3 axis;
	real_t angle = 0.0;
	delta.get_axis_angle(axis, angle);
	if (!Math::is_finite((double)axis.x) || !Math::is_finite((double)axis.y) || !Math::is_finite((double)axis.z)) {
		return Vector3();
	}
	if (angle > Math::PI) {
		angle -= Math::TAU;
	}
	return axis * (angle / p_delta);
}

real_t Box3DRagdoll::_bone_mass(const BoneRuntime &p_bone) const {
	return _capsule_mass(p_bone.radius, p_bone.height, p_bone.density_scale);
}

void Box3DRagdollProfile::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_bones", "bones"), &Box3DRagdollProfile::set_bones);
	ClassDB::bind_method(D_METHOD("get_bones"), &Box3DRagdollProfile::get_bones);
	ClassDB::bind_method(D_METHOD("set_filter_pairs", "filter_pairs"), &Box3DRagdollProfile::set_filter_pairs);
	ClassDB::bind_method(D_METHOD("get_filter_pairs"), &Box3DRagdollProfile::get_filter_pairs);
	ClassDB::bind_method(D_METHOD("set_bone_chains", "bone_chains"), &Box3DRagdollProfile::set_bone_chains);
	ClassDB::bind_method(D_METHOD("get_bone_chains"), &Box3DRagdollProfile::get_bone_chains);
	ClassDB::bind_method(D_METHOD("set_collision_layer", "layer"), &Box3DRagdollProfile::set_collision_layer);
	ClassDB::bind_method(D_METHOD("get_collision_layer"), &Box3DRagdollProfile::get_collision_layer);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &Box3DRagdollProfile::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &Box3DRagdollProfile::get_collision_mask);
	ClassDB::bind_method(D_METHOD("set_friction_torque", "torque"), &Box3DRagdollProfile::set_friction_torque);
	ClassDB::bind_method(D_METHOD("get_friction_torque"), &Box3DRagdollProfile::get_friction_torque);
	ClassDB::bind_method(D_METHOD("set_spring_hertz", "hertz"), &Box3DRagdollProfile::set_spring_hertz);
	ClassDB::bind_method(D_METHOD("get_spring_hertz"), &Box3DRagdollProfile::get_spring_hertz);
	ClassDB::bind_method(D_METHOD("set_spring_damping_ratio", "damping_ratio"), &Box3DRagdollProfile::set_spring_damping_ratio);
	ClassDB::bind_method(D_METHOD("get_spring_damping_ratio"), &Box3DRagdollProfile::get_spring_damping_ratio);
	ClassDB::bind_method(D_METHOD("estimate_bone_mass", "bone"), &Box3DRagdollProfile::estimate_bone_mass);
	ClassDB::bind_method(D_METHOD("estimate_total_mass"), &Box3DRagdollProfile::estimate_total_mass);

	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "bones"), "set_bones", "get_bones");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "filter_pairs"), "set_filter_pairs", "get_filter_pairs");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "bone_chains"), "set_bone_chains", "get_bone_chains");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_layer", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_layer", "get_collision_layer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "friction_torque", PROPERTY_HINT_RANGE, "0,100,0.1,or_greater,suffix:N*m"), "set_friction_torque", "get_friction_torque");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spring_hertz", PROPERTY_HINT_RANGE, "0,30,0.1,or_greater,suffix:Hz"), "set_spring_hertz", "get_spring_hertz");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spring_damping_ratio", PROPERTY_HINT_RANGE, "0,10,0.01,or_greater"), "set_spring_damping_ratio", "get_spring_damping_ratio");

	BIND_ENUM_CONSTANT(JOINT_TYPE_NONE);
	BIND_ENUM_CONSTANT(JOINT_TYPE_SPHERICAL);
	BIND_ENUM_CONSTANT(JOINT_TYPE_REVOLUTE);
}

real_t Box3DRagdollProfile::estimate_bone_mass(const Dictionary &p_bone) const {
	return _capsule_mass(_dict_real(p_bone, SNAME("radius"), 0.12), _dict_real(p_bone, SNAME("height"), 0.5), _dict_real(p_bone, SNAME("density_scale"), 1.0));
}

real_t Box3DRagdollProfile::estimate_total_mass() const {
	real_t total = 0.0;
	for (int i = 0; i < bones.size(); i++) {
		Dictionary bone = bones[i];
		if (_dict_bool(bone, SNAME("enabled"), true)) {
			total += estimate_bone_mass(bone);
		}
	}
	return total;
}

void Box3DRagdoll::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_profile", "profile"), &Box3DRagdoll::set_profile);
	ClassDB::bind_method(D_METHOD("get_profile"), &Box3DRagdoll::get_profile);
	ClassDB::bind_method(D_METHOD("build"), &Box3DRagdoll::build);
	ClassDB::bind_method(D_METHOD("teardown"), &Box3DRagdoll::teardown);
	ClassDB::bind_method(D_METHOD("die", "impulse", "hit_bone", "ramp_time"), &Box3DRagdoll::die, DEFVAL(Vector3()), DEFVAL(StringName()), DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("revive"), &Box3DRagdoll::revive);
	ClassDB::bind_method(D_METHOD("is_built"), &Box3DRagdoll::is_built);
	ClassDB::bind_method(D_METHOD("is_ragdoll_active"), &Box3DRagdoll::is_ragdoll_active);
	ClassDB::bind_method(D_METHOD("get_bone_body", "bone"), &Box3DRagdoll::get_bone_body);
	ClassDB::bind_method(D_METHOD("get_bone_global_transform", "bone"), &Box3DRagdoll::get_bone_global_transform);
	ClassDB::bind_method(D_METHOD("get_bone_linear_velocity", "bone"), &Box3DRagdoll::get_bone_linear_velocity);
	ClassDB::bind_method(D_METHOD("get_center_of_mass_velocity"), &Box3DRagdoll::get_center_of_mass_velocity);
	ClassDB::bind_method(D_METHOD("are_bodies_sleeping"), &Box3DRagdoll::are_bodies_sleeping);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "profile", PROPERTY_HINT_RESOURCE_TYPE, "Box3DRagdollProfile"), "set_profile", "get_profile");
	ADD_SIGNAL(MethodInfo("ragdoll_asleep"));
}

void Box3DRagdoll::set_profile(const Ref<Box3DRagdollProfile> &p_profile) {
	if (profile == p_profile) {
		return;
	}
	teardown();
	profile = p_profile;
}

Dictionary Box3DRagdoll::_profile_entry_for_bone(const StringName &p_name) const {
	if (profile.is_null()) {
		return Dictionary();
	}
	TypedArray<Dictionary> entries = profile->get_bones();
	for (int i = 0; i < entries.size(); i++) {
		Dictionary entry = entries[i];
		if ((StringName)entry.get(SNAME("bone"), StringName()) == p_name) {
			return entry;
		}
	}
	return Dictionary();
}

void Box3DRagdoll::_load_runtime_from_profile(BoneRuntime &r_bone, const Dictionary &p_entry) const {
	r_bone.offset = _dict_transform(p_entry, SNAME("offset"), Transform3D());
	r_bone.radius = _dict_real(p_entry, SNAME("radius"), r_bone.radius);
	r_bone.height = _dict_real(p_entry, SNAME("height"), r_bone.height);
	r_bone.density_scale = _dict_real(p_entry, SNAME("density_scale"), r_bone.density_scale);
	r_bone.joint_type = (Box3DRagdollProfile::JointType)_dict_int(p_entry, SNAME("joint_type"), r_bone.joint_type);
	r_bone.swing_limit = _dict_real(p_entry, SNAME("swing_limit"), r_bone.swing_limit);
	r_bone.twist_lower = _dict_real(p_entry, SNAME("twist_lower"), r_bone.twist_lower);
	r_bone.twist_upper = _dict_real(p_entry, SNAME("twist_upper"), r_bone.twist_upper);
	r_bone.joint_friction_scale = _dict_real(p_entry, SNAME("joint_friction_scale"), r_bone.joint_friction_scale);
	r_bone.blend = CLAMP(_dict_real(p_entry, SNAME("blend"), r_bone.blend), (real_t)0.0, (real_t)1.0);
}

Transform3D Box3DRagdoll::_bone_world_pose(Skeleton3D *p_skeleton, const BoneRuntime &p_bone) const {
	return p_skeleton->get_global_transform() * p_skeleton->get_bone_global_pose(p_bone.bone) * p_bone.offset;
}

void Box3DRagdoll::_set_body_transform(Box3DPhysicsServer3D *p_server, BoneRuntime &r_bone, const Transform3D &p_transform) {
	p_server->body_set_state(r_bone.body, PhysicsServer3D::BODY_STATE_TRANSFORM, p_transform);
}

bool Box3DRagdoll::_create_body_for_bone(Box3DPhysicsServer3D *p_server, Skeleton3D *p_skeleton, BoneRuntime &r_bone) {
	Dictionary capsule;
	capsule[SNAME("radius")] = MAX((real_t)0.01, r_bone.radius);
	capsule[SNAME("height")] = MAX((real_t)0.02, r_bone.height);

	r_bone.shape = p_server->capsule_shape_create();
	p_server->shape_set_data(r_bone.shape, capsule);

	r_bone.body = p_server->body_create();
	p_server->body_set_mode(r_bone.body, PhysicsServer3D::BODY_MODE_KINEMATIC);
	p_server->body_set_collision_layer(r_bone.body, profile->get_collision_layer());
	p_server->body_set_collision_mask(r_bone.body, profile->get_collision_mask());
	p_server->body_set_param(r_bone.body, PhysicsServer3D::BODY_PARAM_MASS, _bone_mass(r_bone));
	p_server->body_add_shape(r_bone.body, r_bone.shape, Transform3D(), false);
	p_server->body_set_state_sync_callback(r_bone.body, callable_mp(this, &Box3DRagdoll::_body_state_changed));
	Box3DBody3D *body = p_server->get_body(r_bone.body);
	ERR_FAIL_NULL_V(body, false);
	body->set_collision_group_index(group_index);
	_set_body_transform(p_server, r_bone, _bone_world_pose(p_skeleton, r_bone));
	p_server->body_set_space(r_bone.body, space_rid);
	return true;
}

Transform3D Box3DRagdoll::_remap_twist_x_to_z(const Transform3D &p_frame) const {
	Basis x_to_z;
	x_to_z.set_column(0, Vector3(0, 1, 0));
	x_to_z.set_column(1, Vector3(0, 0, 1));
	x_to_z.set_column(2, Vector3(1, 0, 0));
	return Transform3D(p_frame.basis * x_to_z, p_frame.origin);
}

void Box3DRagdoll::_create_joint_for_bone(Box3DPhysicsServer3D *p_server, BoneRuntime &r_bone) {
	if (r_bone.parent_runtime < 0 || r_bone.joint_type == Box3DRagdollProfile::JOINT_TYPE_NONE) {
		return;
	}

	Box3DBody3D *body = p_server->get_body(r_bone.body);
	Box3DBody3D *parent_body = p_server->get_body(bones[r_bone.parent_runtime].body);
	ERR_FAIL_NULL(body);
	ERR_FAIL_NULL(parent_body);
	Box3DSpace3D *space = body->get_space();
	ERR_FAIL_NULL(space);

	const Transform3D parent_world = parent_body->get_transform();
	const Transform3D child_world = body->get_transform();
	Transform3D parent_frame = parent_world.affine_inverse() * child_world;
	Transform3D child_frame;

	if (r_bone.joint_type == Box3DRagdollProfile::JOINT_TYPE_REVOLUTE) {
		b3RevoluteJointDef def = b3DefaultRevoluteJointDef();
		def.base.bodyIdA = parent_body->get_body_id();
		def.base.bodyIdB = body->get_body_id();
		def.base.localFrameA = to_box3d(parent_frame);
		def.base.localFrameB = to_box3d(child_frame);
		def.base.collideConnected = false;
		def.base.constraintHertz = (float)GLOBAL_GET("physics/box3d/joints/constraint_hertz");
		def.base.constraintDampingRatio = (float)GLOBAL_GET("physics/box3d/joints/constraint_damping_ratio");
		def.enableLimit = true;
		def.lowerAngle = (float)r_bone.twist_lower;
		def.upperAngle = (float)r_bone.twist_upper;
		def.enableMotor = true;
		def.motorSpeed = 0.0f;
		def.maxMotorTorque = (float)(profile->get_friction_torque() * r_bone.joint_friction_scale);
		r_bone.joint = b3CreateRevoluteJoint(space->get_world(), &def);
		return;
	}

	b3SphericalJointDef def = b3DefaultSphericalJointDef();
	def.base.bodyIdA = parent_body->get_body_id();
	def.base.bodyIdB = body->get_body_id();
	def.base.localFrameA = to_box3d(_remap_twist_x_to_z(parent_frame));
	def.base.localFrameB = to_box3d(_remap_twist_x_to_z(child_frame));
	def.base.collideConnected = false;
	def.base.constraintHertz = (float)GLOBAL_GET("physics/box3d/joints/constraint_hertz");
	def.base.constraintDampingRatio = (float)GLOBAL_GET("physics/box3d/joints/constraint_damping_ratio");
	def.enableConeLimit = true;
	def.coneAngle = (float)CLAMP(r_bone.swing_limit, (real_t)0.0, (real_t)Math::PI);
	def.enableTwistLimit = true;
	def.lowerTwistAngle = (float)r_bone.twist_lower;
	def.upperTwistAngle = (float)r_bone.twist_upper;
	def.enableMotor = true;
	def.motorVelocity = b3Vec3_zero;
	def.maxMotorTorque = (float)(profile->get_friction_torque() * r_bone.joint_friction_scale);
	def.enableSpring = profile->get_spring_hertz() > 0.0;
	def.hertz = (float)profile->get_spring_hertz();
	def.dampingRatio = (float)profile->get_spring_damping_ratio();
	def.targetRotation = b3Quat_identity;
	r_bone.joint = b3CreateSphericalJoint(space->get_world(), &def);
}

void Box3DRagdoll::_create_filter_joints(Box3DPhysicsServer3D *p_server) {
	if (profile.is_null() || bones.is_empty()) {
		return;
	}
	Box3DBody3D *first_body = p_server->get_body(bones[0].body);
	ERR_FAIL_NULL(first_body);
	Box3DSpace3D *space = first_body->get_space();
	ERR_FAIL_NULL(space);

	Array pairs = profile->get_filter_pairs();
	for (int i = 0; i < pairs.size(); i++) {
		PackedStringArray pair = pairs[i];
		if (pair.size() < 2) {
			continue;
		}
		const int *a_idx = bone_lookup.getptr(StringName(pair[0]));
		const int *b_idx = bone_lookup.getptr(StringName(pair[1]));
		if (a_idx == nullptr || b_idx == nullptr) {
			continue;
		}
		Box3DBody3D *a = p_server->get_body(bones[*a_idx].body);
		Box3DBody3D *b = p_server->get_body(bones[*b_idx].body);
		if (a == nullptr || b == nullptr) {
			continue;
		}
		b3FilterJointDef def = b3DefaultFilterJointDef();
		def.base.bodyIdA = a->get_body_id();
		def.base.bodyIdB = b->get_body_id();
		filter_joints.push_back(b3CreateFilterJoint(space->get_world(), &def));
	}
}

bool Box3DRagdoll::build() {
	if (built) {
		return true;
	}
	ERR_FAIL_COND_V_MSG(profile.is_null(), false, "Box3D: Box3DRagdoll needs a profile.");
	Skeleton3D *skeleton = get_skeleton();
	ERR_FAIL_NULL_V(skeleton, false);
	Ref<World3D> world = get_world_3d();
	ERR_FAIL_COND_V(world.is_null(), false);
	space_rid = world->get_space();
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	ERR_FAIL_NULL_V(server, false);
	ERR_FAIL_NULL_V(server->get_space(space_rid), false);

	group_index = next_group_index--;
	if (next_group_index >= 0) {
		next_group_index = -1;
	}

	bones.clear();
	bone_lookup.clear();
	TypedArray<Dictionary> entries = profile->get_bones();
	for (int i = 0; i < entries.size(); i++) {
		Dictionary entry = entries[i];
		if (!_dict_bool(entry, SNAME("enabled"), true)) {
			continue;
		}
		const StringName bone_name = entry.get(SNAME("bone"), StringName());
		const int bone_idx = skeleton->find_bone(String(bone_name));
		if (bone_name == StringName() || bone_idx < 0) {
			WARN_PRINT(vformat("Box3D: ragdoll profile entry references missing bone '%s'.", String(bone_name)));
			continue;
		}
		BoneRuntime runtime;
		runtime.name = bone_name;
		runtime.bone = bone_idx;
		runtime.parent = skeleton->get_bone_parent(bone_idx);
		_load_runtime_from_profile(runtime, entry);
		const int runtime_index = bones.size();
		bones.push_back(runtime);
		bone_lookup[bone_name] = runtime_index;
	}

	ERR_FAIL_COND_V_MSG(bones.is_empty(), false, "Box3D: ragdoll profile has no valid enabled bones.");

	for (uint32_t i = 0; i < bones.size(); i++) {
		for (uint32_t j = 0; j < bones.size(); j++) {
			if (bones[j].bone == bones[i].parent) {
				bones[i].parent_runtime = j;
				break;
			}
		}
		ERR_FAIL_COND_V(!_create_body_for_bone(server, skeleton, bones[i]), false);
	}
	for (uint32_t i = 0; i < bones.size(); i++) {
		if (bones[i].parent_runtime < 0) {
			continue;
		}
		const real_t child_mass = _bone_mass(bones[i]);
		const real_t parent_mass = _bone_mass(bones[bones[i].parent_runtime]);
		const real_t min_mass = MIN(child_mass, parent_mass);
		const real_t max_mass = MAX(child_mass, parent_mass);
		if (min_mass > 0.0 && max_mass / min_mass > 10.0) {
			WARN_PRINT(vformat("Box3D: ragdoll adjacent bone mass ratio exceeds 10:1 between '%s' and '%s' (%.2f kg vs %.2f kg).", String(bones[bones[i].parent_runtime].name), String(bones[i].name), parent_mass, child_mass));
		}
	}
	for (uint32_t i = 0; i < bones.size(); i++) {
		_create_joint_for_bone(server, bones[i]);
	}
	_create_filter_joints(server);

	skeleton->set_modifier_callback_mode_process(Skeleton3D::MODIFIER_CALLBACK_MODE_PROCESS_PHYSICS);
	_capture_animation_pose(last_capture_delta);
	built = true;
	asleep_emitted = false;
	return true;
}

void Box3DRagdoll::_clear_native_joints() {
	for (uint32_t i = 0; i < bones.size(); i++) {
		if (b3Joint_IsValid(bones[i].joint)) {
			b3DestroyJoint(bones[i].joint, true);
		}
		bones[i].joint = b3_nullJointId;
	}
	for (b3JointId joint : filter_joints) {
		if (b3Joint_IsValid(joint)) {
			b3DestroyJoint(joint, true);
		}
	}
	filter_joints.clear();
}

void Box3DRagdoll::_clear_bodies() {
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	if (server == nullptr) {
		return;
	}
	for (uint32_t i = 0; i < bones.size(); i++) {
		if (bones[i].body.is_valid()) {
			server->free_rid(bones[i].body);
		}
		if (bones[i].shape.is_valid()) {
			server->free_rid(bones[i].shape);
		}
		bones[i].body = RID();
		bones[i].shape = RID();
	}
}

void Box3DRagdoll::teardown() {
	_clear_native_joints();
	_clear_bodies();
	bones.clear();
	filter_joints.clear();
	bone_lookup.clear();
	built = false;
	ragdoll_active = false;
	asleep_emitted = false;
	space_rid = RID();
}

void Box3DRagdoll::_capture_animation_pose(real_t p_delta) {
	Skeleton3D *skeleton = get_skeleton();
	if (skeleton == nullptr) {
		return;
	}
	last_capture_delta = MAX(p_delta, (real_t)CMP_EPSILON);
	for (uint32_t i = 0; i < bones.size(); i++) {
		BoneRuntime &bone = bones[i];
		const Transform3D pose = _bone_world_pose(skeleton, bone);
		if (!bone.has_pose_history) {
			bone.previous_pose = pose;
			bone.current_pose = pose;
			bone.has_pose_history = true;
		} else {
			bone.previous_pose = bone.current_pose;
			bone.current_pose = pose;
		}
		bone.pose_capture_count++;
		if (built && !ragdoll_active) {
			Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
			if (server != nullptr) {
				_set_body_transform(server, bone, pose);
				server->body_set_state(bone.body, PhysicsServer3D::BODY_STATE_LINEAR_VELOCITY, Vector3());
				server->body_set_state(bone.body, PhysicsServer3D::BODY_STATE_ANGULAR_VELOCITY, Vector3());
			}
		}
	}
}

void Box3DRagdoll::_seed_body_velocity(Box3DPhysicsServer3D *p_server, BoneRuntime &r_bone, real_t p_delta) {
	const real_t delta = MAX(p_delta, (real_t)CMP_EPSILON);
	const Vector3 linear_velocity = (r_bone.current_pose.origin - r_bone.previous_pose.origin) / delta;
	const Vector3 angular_velocity = _angular_velocity_between(r_bone.previous_pose.basis, r_bone.current_pose.basis, delta);
	p_server->body_set_state(r_bone.body, PhysicsServer3D::BODY_STATE_LINEAR_VELOCITY, linear_velocity);
	p_server->body_set_state(r_bone.body, PhysicsServer3D::BODY_STATE_ANGULAR_VELOCITY, angular_velocity);
	p_server->body_set_state(r_bone.body, PhysicsServer3D::BODY_STATE_SLEEPING, false);
}

bool Box3DRagdoll::_has_velocity_history() const {
	if (bones.is_empty()) {
		return false;
	}
	for (uint32_t i = 0; i < bones.size(); i++) {
		if (bones[i].pose_capture_count < 2) {
			return false;
		}
	}
	return true;
}

void Box3DRagdoll::die(const Vector3 &p_impulse, const StringName &p_hit_bone, real_t p_ramp_time) {
	if (!built && !build()) {
		return;
	}
	if (p_ramp_time > 0.0) {
		WARN_PRINT_ONCE("Box3D: Box3DRagdoll die() ramp_time is reserved for the driven-ragdoll milestone and is ignored in R1.");
	}
	if (!_has_velocity_history()) {
		WARN_PRINT("Box3D: Box3DRagdoll die() has fewer than two cached physics poses; build() should be called at spawn so death momentum can be inherited.");
	}
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	ERR_FAIL_NULL(server);
	for (uint32_t i = 0; i < bones.size(); i++) {
		server->body_set_mode(bones[i].body, PhysicsServer3D::BODY_MODE_RIGID);
		_set_body_transform(server, bones[i], bones[i].current_pose);
		_seed_body_velocity(server, bones[i], last_capture_delta);
	}
	if (!p_impulse.is_zero_approx()) {
		int hit_idx = 0;
		if (p_hit_bone != StringName()) {
			const int *found = bone_lookup.getptr(p_hit_bone);
			if (found != nullptr) {
				hit_idx = *found;
			} else {
				WARN_PRINT(vformat("Box3D: Box3DRagdoll die() hit_bone '%s' was not found; applying impulse to the root body.", String(p_hit_bone)));
			}
		}
		server->body_apply_impulse(bones[hit_idx].body, p_impulse, Vector3());
	}
	ragdoll_active = true;
	asleep_emitted = false;
}

void Box3DRagdoll::revive() {
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	if (server != nullptr) {
		for (uint32_t i = 0; i < bones.size(); i++) {
			server->body_set_mode(bones[i].body, PhysicsServer3D::BODY_MODE_KINEMATIC);
			server->body_set_state(bones[i].body, PhysicsServer3D::BODY_STATE_LINEAR_VELOCITY, Vector3());
			server->body_set_state(bones[i].body, PhysicsServer3D::BODY_STATE_ANGULAR_VELOCITY, Vector3());
		}
	}
	ragdoll_active = false;
	asleep_emitted = false;
	_capture_animation_pose(last_capture_delta);
}

void Box3DRagdoll::_sync_skeleton_from_bodies(Box3DPhysicsServer3D *p_server, Skeleton3D *p_skeleton) {
	const Transform3D skeleton_inv = p_skeleton->get_global_transform().affine_inverse();
	for (uint32_t i = 0; i < bones.size(); i++) {
		BoneRuntime &bone = bones[i];
		Box3DBody3D *body = p_server->get_body(bone.body);
		if (body == nullptr) {
			continue;
		}
		const Transform3D bone_world = body->get_transform() * bone.offset.affine_inverse();
		const Transform3D target_global = skeleton_inv * bone_world;
		const Transform3D current_global = p_skeleton->get_bone_global_pose(bone.bone);
		p_skeleton->set_bone_global_pose(bone.bone, current_global.interpolate_with(target_global, bone.blend));
	}
}

bool Box3DRagdoll::_all_bodies_sleeping(Box3DPhysicsServer3D *p_server) const {
	if (bones.is_empty()) {
		return false;
	}
	for (uint32_t i = 0; i < bones.size(); i++) {
		Box3DBody3D *body = p_server->get_body(bones[i].body);
		if (body == nullptr || !body->in_space() || b3Body_IsAwake(body->get_body_id())) {
			return false;
		}
	}
	return true;
}

void Box3DRagdoll::_process_modification(double p_delta) {
	if (!built) {
		_capture_animation_pose(p_delta);
		return;
	}
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	Skeleton3D *skeleton = get_skeleton();
	if (server == nullptr || skeleton == nullptr) {
		return;
	}
	if (!ragdoll_active) {
		_capture_animation_pose(p_delta);
		return;
	}
	_sync_skeleton_from_bodies(server, skeleton);
	if (!asleep_emitted && _all_bodies_sleeping(server)) {
		asleep_emitted = true;
		emit_signal(SNAME("ragdoll_asleep"));
	}
}

void Box3DRagdoll::_body_state_changed(PhysicsDirectBodyState3D *p_state) {
	if (!ragdoll_active || asleep_emitted) {
		return;
	}
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	if (server != nullptr && _all_bodies_sleeping(server)) {
		asleep_emitted = true;
		emit_signal(SNAME("ragdoll_asleep"));
	}
}

void Box3DRagdoll::_notification(int p_what) {
	if (p_what == NOTIFICATION_EXIT_TREE) {
		teardown();
	}
}

RID Box3DRagdoll::get_bone_body(const StringName &p_bone) const {
	const int *idx = bone_lookup.getptr(p_bone);
	return idx != nullptr ? bones[*idx].body : RID();
}

Transform3D Box3DRagdoll::get_bone_global_transform(const StringName &p_bone) const {
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	ERR_FAIL_NULL_V(server, Transform3D());
	const int *idx = bone_lookup.getptr(p_bone);
	ERR_FAIL_NULL_V(idx, Transform3D());
	Box3DBody3D *body = server->get_body(bones[*idx].body);
	return body != nullptr ? body->get_transform() : Transform3D();
}

Vector3 Box3DRagdoll::get_bone_linear_velocity(const StringName &p_bone) const {
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	ERR_FAIL_NULL_V(server, Vector3());
	const int *idx = bone_lookup.getptr(p_bone);
	ERR_FAIL_NULL_V(idx, Vector3());
	Box3DBody3D *body = server->get_body(bones[*idx].body);
	return body != nullptr ? body->get_linear_velocity() : Vector3();
}

Vector3 Box3DRagdoll::get_center_of_mass_velocity() const {
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	ERR_FAIL_NULL_V(server, Vector3());
	Vector3 weighted_velocity;
	real_t total_mass = 0.0;
	for (uint32_t i = 0; i < bones.size(); i++) {
		Box3DBody3D *body = server->get_body(bones[i].body);
		if (body == nullptr || !body->in_space()) {
			continue;
		}
		const real_t mass = b3Body_GetMass(body->get_body_id());
		weighted_velocity += body->get_linear_velocity() * mass;
		total_mass += mass;
	}
	return total_mass > 0.0 ? weighted_velocity / total_mass : Vector3();
}

bool Box3DRagdoll::are_bodies_sleeping() const {
	Box3DPhysicsServer3D *server = Box3DPhysicsServer3D::get_singleton();
	ERR_FAIL_NULL_V(server, false);
	return _all_bodies_sleeping(server);
}

static Basis _basis_y_to_axis(const Vector3 &p_axis) {
	const Vector3 axis = p_axis.normalized();
	if (!axis.is_normalized()) {
		return Basis();
	}
	return Basis(Quaternion(Vector3(0, 1, 0), axis));
}

static real_t _signed_twist_angle_x(const Quaternion &p_rotation) {
	Quaternion q = p_rotation;
	q.normalize();
	Quaternion twist(q.x, 0.0, 0.0, q.w);
	if (twist.length_squared() <= CMP_EPSILON) {
		return 0.0;
	}
	twist.normalize();
	Vector3 axis;
	real_t angle = 0.0;
	twist.get_axis_angle(axis, angle);
	if (angle > Math::PI) {
		angle -= Math::TAU;
	}
	return axis.x < 0.0 ? -angle : angle;
}

static real_t _swing_angle_without_x_twist(const Quaternion &p_rotation) {
	Quaternion q = p_rotation;
	q.normalize();
	Quaternion twist(q.x, 0.0, 0.0, q.w);
	if (twist.length_squared() <= CMP_EPSILON) {
		return 0.0;
	}
	twist.normalize();
	Quaternion swing = q * twist.inverse();
	swing.normalize();
	real_t angle = (real_t)2.0 * Math::acos(CLAMP(swing.w, (real_t)-1.0, (real_t)1.0));
	if (angle > Math::PI) {
		angle = Math::TAU - angle;
	}
	return Math::abs(angle);
}

void Box3DRagdollProfileGenerator::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_vertex_weight_threshold", "threshold"), &Box3DRagdollProfileGenerator::set_vertex_weight_threshold);
	ClassDB::bind_method(D_METHOD("get_vertex_weight_threshold"), &Box3DRagdollProfileGenerator::get_vertex_weight_threshold);
	ClassDB::bind_method(D_METHOD("set_animation_padding", "radians"), &Box3DRagdollProfileGenerator::set_animation_padding);
	ClassDB::bind_method(D_METHOD("get_animation_padding"), &Box3DRagdollProfileGenerator::get_animation_padding);
	ClassDB::bind_method(D_METHOD("set_minimum_bone_length", "length"), &Box3DRagdollProfileGenerator::set_minimum_bone_length);
	ClassDB::bind_method(D_METHOD("get_minimum_bone_length"), &Box3DRagdollProfileGenerator::get_minimum_bone_length);
	ClassDB::bind_method(D_METHOD("set_minimum_radius", "radius"), &Box3DRagdollProfileGenerator::set_minimum_radius);
	ClassDB::bind_method(D_METHOD("get_minimum_radius"), &Box3DRagdollProfileGenerator::get_minimum_radius);
	ClassDB::bind_method(D_METHOD("set_fallback_radius_ratio", "ratio"), &Box3DRagdollProfileGenerator::set_fallback_radius_ratio);
	ClassDB::bind_method(D_METHOD("get_fallback_radius_ratio"), &Box3DRagdollProfileGenerator::get_fallback_radius_ratio);
	ClassDB::bind_method(D_METHOD("set_maximum_adjacent_mass_ratio", "ratio"), &Box3DRagdollProfileGenerator::set_maximum_adjacent_mass_ratio);
	ClassDB::bind_method(D_METHOD("get_maximum_adjacent_mass_ratio"), &Box3DRagdollProfileGenerator::get_maximum_adjacent_mass_ratio);
	ClassDB::bind_method(D_METHOD("get_warnings"), &Box3DRagdollProfileGenerator::get_warnings);
	ClassDB::bind_method(D_METHOD("clear_warnings"), &Box3DRagdollProfileGenerator::clear_warnings);
	ClassDB::bind_method(D_METHOD("generate_profile", "skeleton", "mesh_instance", "animation_library"), &Box3DRagdollProfileGenerator::generate_profile, DEFVAL(Variant()), DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("analyze_profile", "profile"), &Box3DRagdollProfileGenerator::analyze_profile);
	ClassDB::bind_method(D_METHOD("get_gizmo_lines", "profile", "skeleton"), &Box3DRagdollProfileGenerator::get_gizmo_lines);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "vertex_weight_threshold", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_vertex_weight_threshold", "get_vertex_weight_threshold");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "animation_padding", PROPERTY_HINT_RANGE, "0,3.14159,0.001,radians"), "set_animation_padding", "get_animation_padding");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "minimum_bone_length", PROPERTY_HINT_RANGE, "0.001,10,0.001,or_greater,suffix:m"), "set_minimum_bone_length", "get_minimum_bone_length");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "minimum_radius", PROPERTY_HINT_RANGE, "0.001,2,0.001,or_greater,suffix:m"), "set_minimum_radius", "get_minimum_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fallback_radius_ratio", PROPERTY_HINT_RANGE, "0.01,1,0.01"), "set_fallback_radius_ratio", "get_fallback_radius_ratio");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "maximum_adjacent_mass_ratio", PROPERTY_HINT_RANGE, "1,100,0.1"), "set_maximum_adjacent_mass_ratio", "get_maximum_adjacent_mass_ratio");
}

void Box3DRagdollProfileGenerator::_warn(const String &p_warning) {
	warnings.append(p_warning);
	WARN_PRINT(p_warning);
}

StringName Box3DRagdollProfileGenerator::_chain_for_bone(const String &p_bone) const {
	const String lower = p_bone.to_lower();
	if (lower.contains("arm") || lower.contains("hand") || lower.contains("elbow") || lower.contains("forearm")) {
		return lower.contains("_r") || lower.ends_with("r") || lower.contains("right") ? SNAME("arm_r") : SNAME("arm_l");
	}
	if (lower.contains("leg") || lower.contains("foot") || lower.contains("knee") || lower.contains("thigh") || lower.contains("shin")) {
		return lower.contains("_r") || lower.ends_with("r") || lower.contains("right") ? SNAME("leg_r") : SNAME("leg_l");
	}
	if (lower.contains("head") || lower.contains("neck")) {
		return SNAME("head");
	}
	return SNAME("spine");
}

int Box3DRagdollProfileGenerator::_track_bone_index(Skeleton3D *p_skeleton, const NodePath &p_path) const {
	for (int i = p_path.get_subname_count() - 1; i >= 0; i--) {
		const int bone = p_skeleton->find_bone(String(p_path.get_subname(i)));
		if (bone >= 0) {
			return bone;
		}
	}
	for (int i = p_path.get_name_count() - 1; i >= 0; i--) {
		const int bone = p_skeleton->find_bone(String(p_path.get_name(i)));
		if (bone >= 0) {
			return bone;
		}
	}
	return -1;
}

void Box3DRagdollProfileGenerator::_collect_weighted_vertices(Skeleton3D *p_skeleton, MeshInstance3D *p_mesh_instance, HashMap<int, LocalVector<Vector3>> &r_vertices) {
	if (p_mesh_instance == nullptr || p_mesh_instance->get_mesh().is_null()) {
		return;
	}

	Ref<Mesh> mesh = p_mesh_instance->get_mesh();
	Ref<Skin> skin = p_mesh_instance->get_skin();
	Vector<int> bind_to_bone;
	if (skin.is_valid()) {
		bind_to_bone.resize(skin->get_bind_count());
		for (int i = 0; i < skin->get_bind_count(); i++) {
			int bone = skin->get_bind_bone(i);
			if (bone < 0 && skin->get_bind_name(i) != StringName()) {
				bone = p_skeleton->find_bone(String(skin->get_bind_name(i)));
			}
			bind_to_bone.write[i] = bone;
		}
	}

	const Transform3D mesh_to_skeleton = p_skeleton->get_global_transform().affine_inverse() * p_mesh_instance->get_global_transform();
	for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
		const uint32_t format = mesh->surface_get_format(surface);
		if ((format & Mesh::ARRAY_FORMAT_VERTEX) == 0 || (format & Mesh::ARRAY_FORMAT_BONES) == 0 || (format & Mesh::ARRAY_FORMAT_WEIGHTS) == 0) {
			continue;
		}
		const int weights_per_vertex = (format & Mesh::ARRAY_FLAG_USE_8_BONE_WEIGHTS) ? 8 : 4;
		const Array arrays = mesh->surface_get_arrays(surface);
		const Vector<Vector3> vertices = arrays[Mesh::ARRAY_VERTEX];
		const Vector<int> bones = arrays[Mesh::ARRAY_BONES];
		const Vector<float> weights = arrays[Mesh::ARRAY_WEIGHTS];
		if (bones.size() != vertices.size() * weights_per_vertex || weights.size() != vertices.size() * weights_per_vertex) {
			_warn(vformat("Box3D: ragdoll generator skipped mesh surface %d because skin arrays do not match vertex count.", surface));
			continue;
		}
		for (int vertex = 0; vertex < vertices.size(); vertex++) {
			for (int influence = 0; influence < weights_per_vertex; influence++) {
				const int influence_idx = vertex * weights_per_vertex + influence;
				if (weights[influence_idx] < vertex_weight_threshold) {
					continue;
				}
				int bone = bones[influence_idx];
				if (bind_to_bone.size() > 0) {
					if (bone < 0 || bone >= bind_to_bone.size()) {
						continue;
					}
					bone = bind_to_bone[bone];
				}
				if (bone < 0 || bone >= p_skeleton->get_bone_count()) {
					continue;
				}
				if (!r_vertices.has(bone)) {
					r_vertices.insert(bone, LocalVector<Vector3>());
				}
				LocalVector<Vector3> *bucket = r_vertices.getptr(bone);
				bucket->push_back(mesh_to_skeleton.xform(vertices[vertex]));
			}
		}
	}
}

void Box3DRagdollProfileGenerator::_apply_animation_limits(Skeleton3D *p_skeleton, const Ref<AnimationLibrary> &p_animation_library, HashMap<StringName, Dictionary> &r_entries) {
	if (p_animation_library.is_null()) {
		return;
	}
	LocalVector<StringName> animation_names;
	p_animation_library->get_animation_list(&animation_names);
	bool found_rotation_track = false;
	for (uint32_t animation_idx = 0; animation_idx < animation_names.size(); animation_idx++) {
		Ref<Animation> animation = p_animation_library->get_animation(animation_names[animation_idx]);
		if (animation.is_null()) {
			continue;
		}
		const double length = MAX(animation->get_length(), (double)ANIM_MIN_LENGTH);
		const int sample_count = MAX(2, (int)Math::ceil(length * 30.0) + 1);
		for (int track = 0; track < animation->get_track_count(); track++) {
			if (animation->track_get_type(track) != Animation::TYPE_ROTATION_3D) {
				continue;
			}
			const int bone_idx = _track_bone_index(p_skeleton, animation->track_get_path(track));
			if (bone_idx < 0) {
				continue;
			}
			const StringName bone_name = StringName(p_skeleton->get_bone_name(bone_idx));
			Dictionary *entry = r_entries.getptr(bone_name);
			if (entry == nullptr) {
				continue;
			}
			found_rotation_track = true;
			real_t swing = 0.0;
			real_t twist_lower = 0.0;
			real_t twist_upper = 0.0;
			bool has_sample = false;
			for (int sample = 0; sample < sample_count; sample++) {
				const double time = length * (double)sample / (double)(sample_count - 1);
				Quaternion q;
				if (animation->try_rotation_track_interpolate(track, time, &q) != OK) {
					continue;
				}
				const real_t sample_twist = _signed_twist_angle_x(q);
				const real_t sample_swing = _swing_angle_without_x_twist(q);
				if (!has_sample) {
					twist_lower = sample_twist;
					twist_upper = sample_twist;
					has_sample = true;
				} else {
					twist_lower = MIN(twist_lower, sample_twist);
					twist_upper = MAX(twist_upper, sample_twist);
				}
				swing = MAX(swing, sample_swing);
			}
			if (has_sample) {
				(*entry)[SNAME("swing_limit")] = CLAMP(swing + animation_padding, (real_t)0.0, (real_t)Math::PI);
				(*entry)[SNAME("twist_lower")] = CLAMP(twist_lower - animation_padding, (real_t)-Math::PI, (real_t)Math::PI);
				(*entry)[SNAME("twist_upper")] = CLAMP(twist_upper + animation_padding, (real_t)-Math::PI, (real_t)Math::PI);
			}
		}
	}
	if (!found_rotation_track) {
		_warn("Box3D: ragdoll generator received an AnimationLibrary but found no rotation tracks matching skeleton bones.");
	}
}

void Box3DRagdollProfileGenerator::_clamp_adjacent_mass_ratios(Skeleton3D *p_skeleton, HashMap<StringName, Dictionary> &r_entries) {
	for (int bone = 0; bone < p_skeleton->get_bone_count(); bone++) {
		const int parent = p_skeleton->get_bone_parent(bone);
		if (parent < 0) {
			continue;
		}
		Dictionary *child_entry = r_entries.getptr(StringName(p_skeleton->get_bone_name(bone)));
		Dictionary *parent_entry = r_entries.getptr(StringName(p_skeleton->get_bone_name(parent)));
		if (child_entry == nullptr || parent_entry == nullptr) {
			continue;
		}
		const real_t child_mass = _capsule_mass(_dict_real(*child_entry, SNAME("radius"), 0.12), _dict_real(*child_entry, SNAME("height"), 0.5), _dict_real(*child_entry, SNAME("density_scale"), 1.0));
		const real_t parent_mass = _capsule_mass(_dict_real(*parent_entry, SNAME("radius"), 0.12), _dict_real(*parent_entry, SNAME("height"), 0.5), _dict_real(*parent_entry, SNAME("density_scale"), 1.0));
		const real_t min_mass = MIN(child_mass, parent_mass);
		const real_t max_mass = MAX(child_mass, parent_mass);
		if (min_mass <= 0.0 || max_mass / min_mass <= maximum_adjacent_mass_ratio) {
			continue;
		}
		Dictionary *lighter = child_mass < parent_mass ? child_entry : parent_entry;
		const real_t density_scale = _dict_real(*lighter, SNAME("density_scale"), 1.0);
		(*lighter)[SNAME("density_scale")] = density_scale * (max_mass / min_mass) / maximum_adjacent_mass_ratio;
		_warn(vformat("Box3D: ragdoll generator raised density_scale on '%s' to keep adjacent mass ratio within %.1f:1.", child_mass < parent_mass ? p_skeleton->get_bone_name(bone) : p_skeleton->get_bone_name(parent), maximum_adjacent_mass_ratio));
	}
}

Ref<Box3DRagdollProfile> Box3DRagdollProfileGenerator::generate_profile(Skeleton3D *p_skeleton, MeshInstance3D *p_mesh_instance, const Ref<AnimationLibrary> &p_animation_library) {
	clear_warnings();
	ERR_FAIL_NULL_V(p_skeleton, Ref<Box3DRagdollProfile>());
	Ref<Box3DRagdollProfile> profile;
	profile.instantiate();
	profile->set_friction_torque(8.0);
	profile->set_spring_hertz(1.0);
	profile->set_spring_damping_ratio(0.7);

	HashMap<int, LocalVector<Vector3>> weighted_vertices;
	_collect_weighted_vertices(p_skeleton, p_mesh_instance, weighted_vertices);

	HashMap<StringName, Dictionary> entries;
	Dictionary chains;
	for (int bone = 0; bone < p_skeleton->get_bone_count(); bone++) {
		const StringName bone_name = StringName(p_skeleton->get_bone_name(bone));
		const Transform3D bone_rest = p_skeleton->get_bone_global_rest(bone);
		Vector3 axis;
		Vector<int> children = p_skeleton->get_bone_children(bone);
		if (!children.is_empty()) {
			real_t best_score = -Math::INF;
			for (int child : children) {
				const Vector3 candidate = p_skeleton->get_bone_global_rest(child).origin - bone_rest.origin;
				const real_t candidate_length = candidate.length();
				const real_t score = (candidate_length > CMP_EPSILON ? candidate.y / candidate_length : (real_t)0.0) * (real_t)10.0 + candidate_length;
				if (score > best_score) {
					best_score = score;
					axis = candidate;
				}
			}
		} else {
			const int parent = p_skeleton->get_bone_parent(bone);
			if (parent >= 0) {
				axis = bone_rest.origin - p_skeleton->get_bone_global_rest(parent).origin;
			}
		}
		if (axis.length() < minimum_bone_length) {
			axis = Vector3(0, minimum_bone_length, 0);
		}

		const real_t length = MAX(axis.length(), minimum_bone_length);
		const Vector3 axis_dir = axis.normalized();
		real_t radius = MAX(minimum_radius, length * fallback_radius_ratio);
		const LocalVector<Vector3> *vertices = weighted_vertices.getptr(bone);
		if (vertices != nullptr && vertices->size() > 0) {
			Vector<real_t> distances;
			for (uint32_t i = 0; i < vertices->size(); i++) {
				const Vector3 rel = (*vertices)[i] - bone_rest.origin;
				const real_t projection = CLAMP(rel.dot(axis_dir), (real_t)0.0, length);
				const Vector3 closest = bone_rest.origin + axis_dir * projection;
				distances.append(((*vertices)[i] - closest).length());
			}
			distances.sort();
			radius = CLAMP(distances[distances.size() / 2], minimum_radius, length * (real_t)0.45);
		}
		const real_t height = MAX(length + radius * (real_t)2.0, radius * (real_t)2.1);
		const Vector3 center = bone_rest.origin + axis_dir * (length * (real_t)0.5);
		const Transform3D capsule_world(_basis_y_to_axis(axis), center);
		const Transform3D offset = bone_rest.affine_inverse() * capsule_world;

		const String lower = String(bone_name).to_lower();
		Box3DRagdollProfile::JointType joint_type = Box3DRagdollProfile::JOINT_TYPE_SPHERICAL;
		if (p_skeleton->get_bone_parent(bone) < 0) {
			joint_type = Box3DRagdollProfile::JOINT_TYPE_NONE;
		} else if (lower.contains("leg") || lower.contains("knee") || lower.contains("shin") || lower.contains("forearm") || lower.contains("elbow")) {
			joint_type = Box3DRagdollProfile::JOINT_TYPE_REVOLUTE;
		}

		Dictionary entry;
		entry[SNAME("enabled")] = true;
		entry[SNAME("bone")] = bone_name;
		entry[SNAME("joint_type")] = joint_type;
		entry[SNAME("radius")] = radius;
		entry[SNAME("height")] = height;
		entry[SNAME("offset")] = offset;
		entry[SNAME("density_scale")] = 1.0;
		entry[SNAME("swing_limit")] = Math::deg_to_rad((real_t)(joint_type == Box3DRagdollProfile::JOINT_TYPE_REVOLUTE ? 0.0 : 35.0));
		entry[SNAME("twist_lower")] = Math::deg_to_rad((real_t)(joint_type == Box3DRagdollProfile::JOINT_TYPE_REVOLUTE ? -5.0 : -15.0));
		entry[SNAME("twist_upper")] = Math::deg_to_rad((real_t)(joint_type == Box3DRagdollProfile::JOINT_TYPE_REVOLUTE ? 45.0 : 15.0));
		entry[SNAME("joint_friction_scale")] = lower.contains("head") ? 0.4 : (lower.contains("neck") ? 0.8 : 1.0);
		entries.insert(bone_name, entry);

		const StringName chain = _chain_for_bone(String(bone_name));
		PackedStringArray chain_bones = chains.has(chain) ? (PackedStringArray)chains[chain] : PackedStringArray();
		chain_bones.append(String(bone_name));
		chains[chain] = chain_bones;
	}

	_apply_animation_limits(p_skeleton, p_animation_library, entries);
	_clamp_adjacent_mass_ratios(p_skeleton, entries);

	TypedArray<Dictionary> bones_array;
	Array filter_pairs;
	for (int bone = 0; bone < p_skeleton->get_bone_count(); bone++) {
		const StringName bone_name = StringName(p_skeleton->get_bone_name(bone));
		Dictionary *entry = entries.getptr(bone_name);
		if (entry == nullptr) {
			continue;
		}
		bones_array.push_back(*entry);
		const int parent = p_skeleton->get_bone_parent(bone);
		if (parent >= 0 && entries.has(StringName(p_skeleton->get_bone_name(parent)))) {
			filter_pairs.append(PackedStringArray({ p_skeleton->get_bone_name(parent), p_skeleton->get_bone_name(bone) }));
		}
	}
	profile->set_bones(bones_array);
	profile->set_filter_pairs(filter_pairs);
	profile->set_bone_chains(chains);
	return profile;
}

Dictionary Box3DRagdollProfileGenerator::analyze_profile(const Ref<Box3DRagdollProfile> &p_profile) const {
	Dictionary result;
	ERR_FAIL_COND_V(p_profile.is_null(), result);
	const TypedArray<Dictionary> bones = p_profile->get_bones();
	real_t min_mass = 0.0;
	real_t max_mass = 0.0;
	for (int i = 0; i < bones.size(); i++) {
		Dictionary bone = bones[i];
		const real_t mass = p_profile->estimate_bone_mass(bone);
		if (i == 0 || mass < min_mass) {
			min_mass = mass;
		}
		if (i == 0 || mass > max_mass) {
			max_mass = mass;
		}
	}
	result[SNAME("bone_count")] = bones.size();
	result[SNAME("total_mass")] = p_profile->estimate_total_mass();
	result[SNAME("minimum_bone_mass")] = min_mass;
	result[SNAME("maximum_bone_mass")] = max_mass;
	result[SNAME("maximum_mass_ratio")] = min_mass > 0.0 ? max_mass / min_mass : 0.0;
	result[SNAME("warnings")] = warnings;
	return result;
}

PackedVector3Array Box3DRagdollProfileGenerator::get_gizmo_lines(const Ref<Box3DRagdollProfile> &p_profile, Skeleton3D *p_skeleton) const {
	PackedVector3Array lines;
	ERR_FAIL_COND_V(p_profile.is_null(), lines);
	ERR_FAIL_NULL_V(p_skeleton, lines);
	const Transform3D skeleton_global = p_skeleton->get_global_transform();
	const TypedArray<Dictionary> bones = p_profile->get_bones();
	for (int i = 0; i < bones.size(); i++) {
		Dictionary entry = bones[i];
		const int bone_idx = p_skeleton->find_bone(String((StringName)entry.get(SNAME("bone"), StringName())));
		if (bone_idx < 0) {
			continue;
		}
		const real_t height = _dict_real(entry, SNAME("height"), 0.5);
		const Transform3D offset = _dict_transform(entry, SNAME("offset"), Transform3D());
		const Transform3D pose = skeleton_global * p_skeleton->get_bone_global_rest(bone_idx) * offset;
		lines.append(pose.xform(Vector3(0, -height * 0.5, 0)));
		lines.append(pose.xform(Vector3(0, height * 0.5, 0)));
	}
	return lines;
}

Box3DRagdoll::~Box3DRagdoll() {
	teardown();
}
