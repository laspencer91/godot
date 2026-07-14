/**************************************************************************/
/*  level_snap_service.cpp                                                */
/**************************************************************************/

#include "level_snap_service.h"

Vector3 LevelSnapService::snap_point_to_plane_grid(const Vector3 &p_point, const Transform3D &p_grid_transform, real_t p_step) {
	ERR_FAIL_COND_V(!p_point.is_finite() || !p_grid_transform.is_finite(), p_point);
	ERR_FAIL_COND_V(!p_grid_transform.basis.is_orthogonal(), p_point);
	ERR_FAIL_COND_V(!Math::is_finite(p_step) || p_step <= CMP_EPSILON, p_point);

	Vector3 local = p_grid_transform.basis.transposed().xform(p_point - p_grid_transform.origin);
	local.x = Math::snapped(local.x, p_step);
	local.y = 0.0;
	local.z = Math::snapped(local.z, p_step);
	return p_grid_transform.xform(local);
}

real_t LevelSnapService::snap_delta(real_t p_delta, real_t p_step) {
	ERR_FAIL_COND_V(!Math::is_finite(p_delta), p_delta);
	ERR_FAIL_COND_V(!Math::is_finite(p_step) || p_step <= CMP_EPSILON, p_delta);
	return Math::snapped(p_delta, p_step);
}

Vector3 LevelSnapService::snap_delta(const Vector3 &p_delta, real_t p_step) {
	ERR_FAIL_COND_V(!p_delta.is_finite(), p_delta);
	ERR_FAIL_COND_V(!Math::is_finite(p_step) || p_step <= CMP_EPSILON, p_delta);
	return p_delta.snappedf(p_step);
}

real_t LevelSnapService::snap_angle(real_t p_angle, real_t p_step) {
	return snap_delta(p_angle, p_step);
}

Vector3 LevelSnapService::snap_point_absolute(const Vector3 &p_point, real_t p_step) {
	ERR_FAIL_COND_V(!p_point.is_finite(), p_point);
	ERR_FAIL_COND_V(!Math::is_finite(p_step) || p_step <= CMP_EPSILON, p_point);
	return p_point.snappedf(p_step);
}
