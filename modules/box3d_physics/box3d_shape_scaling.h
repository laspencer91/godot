/**************************************************************************/
/*  box3d_shape_scaling.h                                                 */
/**************************************************************************/

#pragma once

#include "box3d_conversions.h"

#include "box3d/box3d.h"
#include "box3d/collision.h"

_FORCE_INLINE_ Transform3D box3d_get_scaled_shape_transform(const Transform3D &p_shape_transform, const Vector3 &p_body_scale) {
	return Transform3D().scaled_local(p_body_scale) * p_shape_transform;
}

_FORCE_INLINE_ real_t box3d_get_uniform_primitive_scale(const Transform3D &p_shape_transform) {
	const Vector3 axis_scale = p_shape_transform.basis.get_scale_abs();
	return (axis_scale.x + axis_scale.y + axis_scale.z) / 3.0;
}

// Object scale is applied after a shape's local transform in Godot. Box3D's
// transformed-hull entry point applies scale before its transform, so two
// stages are required to preserve that ordering for non-uniform or mirrored
// object scale.
_FORCE_INLINE_ b3ShapeId box3d_create_scaled_hull_shape(b3BodyId p_body_id, const b3ShapeDef *p_def, const b3HullData *p_hull, const Transform3D &p_shape_transform, const Vector3 &p_body_scale) {
	Vector3 shape_scale;
	const Transform3D shape_transform = box3d_decompose_transform(p_shape_transform, shape_scale);
	const Vector3 unit_scale(1.0, 1.0, 1.0);

	if (p_body_scale.is_equal_approx(unit_scale)) {
		return b3CreateTransformedHullShape(p_body_id, p_def, p_hull, to_box3d(shape_transform), to_box3d(shape_scale));
	}
	if (p_shape_transform.is_equal_approx(Transform3D())) {
		return b3CreateTransformedHullShape(p_body_id, p_def, p_hull, b3Transform_identity, to_box3d(p_body_scale));
	}

	b3HullData *shape_hull = b3CloneAndTransformHull(p_hull, to_box3d(shape_transform), to_box3d(shape_scale));
	if (shape_hull == nullptr) {
		return b3_nullShapeId;
	}

	const b3ShapeId shape_id = b3CreateTransformedHullShape(p_body_id, p_def, shape_hull, b3Transform_identity, to_box3d(p_body_scale));
	b3DestroyHull(shape_hull);
	return shape_id;
}
