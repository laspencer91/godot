/**************************************************************************/
/*  level_snap_service.h                                                  */
/**************************************************************************/
/*  G-Level S7: shared world/grid delta quantization for modal tools.     */
/**************************************************************************/

#pragma once

#include "core/math/transform_3d.h"

class LevelSnapService {
public:
	static constexpr real_t DEFAULT_ANGLE_STEP = Math::PI / 12.0;

	// Creation grids use local X/Z as tangents and local Y as the plane normal.
	// The caller freezes this transform at gesture start.
	static Vector3 snap_point_to_plane_grid(const Vector3 &p_point, const Transform3D &p_grid_transform, real_t p_step);
	static Vector3 snap_point_to_plane_grid(const Vector3 &p_point, const Transform3D &p_grid_transform, real_t p_step, bool p_enabled);
	static Vector3 snap_delta(const Vector3 &p_delta, real_t p_step);
	static Vector3 snap_delta(const Vector3 &p_delta, real_t p_step, bool p_enabled);
	static real_t snap_delta(real_t p_delta, real_t p_step);
	static real_t snap_delta(real_t p_delta, real_t p_step, bool p_enabled);
	static real_t snap_angle(real_t p_angle, real_t p_step = DEFAULT_ANGLE_STEP);
	static Vector3 snap_point_absolute(const Vector3 &p_point, real_t p_step);
};
