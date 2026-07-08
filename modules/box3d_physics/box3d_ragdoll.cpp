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
	const real_t radius = MAX((real_t)0.01, p_bone.radius);
	const real_t height = MAX((real_t)0.02, p_bone.height);
	const real_t cylinder_height = MAX((real_t)0.0, height - (real_t)2.0 * radius);
	const real_t volume = Math::PI * radius * radius * cylinder_height + ((real_t)4.0 / (real_t)3.0) * Math::PI * radius * radius * radius;
	return MAX((real_t)0.05, volume * (real_t)1000.0 * MAX((real_t)0.0, p_bone.density_scale));
}

void Box3DRagdollProfile::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_bones", "bones"), &Box3DRagdollProfile::set_bones);
	ClassDB::bind_method(D_METHOD("get_bones"), &Box3DRagdollProfile::get_bones);
	ClassDB::bind_method(D_METHOD("set_filter_pairs", "filter_pairs"), &Box3DRagdollProfile::set_filter_pairs);
	ClassDB::bind_method(D_METHOD("get_filter_pairs"), &Box3DRagdollProfile::get_filter_pairs);
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

	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "bones"), "set_bones", "get_bones");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "filter_pairs"), "set_filter_pairs", "get_filter_pairs");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_layer", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_layer", "get_collision_layer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "friction_torque", PROPERTY_HINT_RANGE, "0,100,0.1,or_greater,suffix:N*m"), "set_friction_torque", "get_friction_torque");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spring_hertz", PROPERTY_HINT_RANGE, "0,30,0.1,or_greater,suffix:Hz"), "set_spring_hertz", "get_spring_hertz");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spring_damping_ratio", PROPERTY_HINT_RANGE, "0,10,0.01,or_greater"), "set_spring_damping_ratio", "get_spring_damping_ratio");

	BIND_ENUM_CONSTANT(JOINT_TYPE_NONE);
	BIND_ENUM_CONSTANT(JOINT_TYPE_SPHERICAL);
	BIND_ENUM_CONSTANT(JOINT_TYPE_REVOLUTE);
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

Box3DRagdoll::~Box3DRagdoll() {
	teardown();
}
