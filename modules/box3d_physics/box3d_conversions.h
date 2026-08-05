/**************************************************************************/
/*  box3d_conversions.h — Godot <-> Box3D type converters                 */
/*  Part of the box3d_physics module (feature/box3d-physics branch).      */
/**************************************************************************/

#pragma once

#include "core/math/transform_3d.h"

#include "box3d/math_functions.h"

// Float-precision build of Box3D assumed (b3Pos == b3Vec3, b3WorldTransform == b3Transform).
// Revisit converters if BOX3D_DOUBLE_PRECISION is ever enabled alongside precision=double.

_FORCE_INLINE_ b3Vec3 to_box3d(const Vector3 &p_v) {
	return b3Vec3{ (float)p_v.x, (float)p_v.y, (float)p_v.z };
}

_FORCE_INLINE_ Vector3 to_godot(const b3Vec3 &p_v) {
	return Vector3(p_v.x, p_v.y, p_v.z);
}

_FORCE_INLINE_ b3Quat to_box3d(const Quaternion &p_q) {
	b3Quat q;
	q.v = b3Vec3{ (float)p_q.x, (float)p_q.y, (float)p_q.z };
	q.s = (float)p_q.w;
	return q;
}

_FORCE_INLINE_ Quaternion to_godot(const b3Quat &p_q) {
	return Quaternion(p_q.v.x, p_q.v.y, p_q.v.z, p_q.s);
}

_FORCE_INLINE_ b3Transform to_box3d(const Transform3D &p_t) {
	b3Transform t;
	t.p = to_box3d(p_t.origin);
	t.q = to_box3d(p_t.basis.get_rotation_quaternion());
	return t;
}

// Box3D body transforms are rigid, while Godot's Transform3D also carries scale.
// Basis::get_scale() and get_rotation_quaternion() are a paired R * S
// decomposition, including mirrored transforms.
_FORCE_INLINE_ Transform3D box3d_decompose_transform(const Transform3D &p_transform, Vector3 &r_scale) {
	r_scale = p_transform.basis.get_scale();
	return Transform3D(Basis(p_transform.basis.get_rotation_quaternion()), p_transform.origin);
}

_FORCE_INLINE_ Transform3D to_godot(const b3Transform &p_t) {
	return Transform3D(Basis(to_godot(p_t.q)), to_godot(p_t.p));
}
