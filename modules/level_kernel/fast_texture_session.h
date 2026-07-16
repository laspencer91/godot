/**************************************************************************/
/*  fast_texture_session.h                                                */
/**************************************************************************/
/*  G-Level WP17: headless working-copy model for Fast Texture.           */
/**************************************************************************/

#pragma once

#include "core/math/transform_2d.h"
#include "core/object/ref_counted.h"
#include "core/variant/type_info.h"

class LevelMesh;
class LevelMeshData;
class LevelMeshDiff;

class FastTextureSession : public RefCounted {
	GDCLASS(FastTextureSession, RefCounted);

public:
	enum Mode {
		MODE_USE_EXISTING,
		MODE_CONFORMING,
		MODE_SQUARE,
		MODE_FOLLOW_QUADS,
		MODE_PLANAR,
	};

	enum SpacingMode {
		SPACING_LENGTH,
		SPACING_EVEN,
		SPACING_LENGTH_AVERAGE,
	};

private:
	Ref<LevelMesh> source_mesh;
	Ref<LevelMeshData> original_data;
	Ref<LevelMesh> working_mesh;
	Ref<LevelMeshDiff> last_diff;
	PackedInt32Array face_ids;
	Transform2D nudge;
	Mode mode = MODE_USE_EXISTING;
	SpacingMode spacing = SPACING_LENGTH;
	String last_error;
	bool active = false;

	void _set_error(const String &p_error);
	bool _source_matches_snapshot() const;
	static String _unwrap_error_text(int p_error);

protected:
	static void _bind_methods();

public:
	void set_mesh(const Ref<LevelMesh> &p_mesh);
	Ref<LevelMesh> get_mesh() const;

	bool open(const PackedInt32Array &p_face_ids);
	bool set_mode(int p_mode, int p_spacing = SPACING_LENGTH);
	bool set_nudge(const Transform2D &p_nudge);
	bool can_accept() const;
	bool accept();
	void cancel();

	bool is_active() const { return active; }
	int get_mode() const { return mode; }
	int get_spacing() const { return spacing; }
	Transform2D get_nudge() const { return nudge; }
	PackedInt32Array get_face_ids() const { return face_ids; }
	PackedVector2Array get_mode_loop_uvs() const;
	PackedVector2Array get_working_loop_uvs() const;
	Ref<LevelMeshData> get_working_data() const;
	Ref<LevelMeshData> get_original_data() const;
	Ref<LevelMeshDiff> get_last_diff() const;
	String get_last_error() const { return last_error; }
};

VARIANT_ENUM_CAST(FastTextureSession::Mode);
VARIANT_ENUM_CAST(FastTextureSession::SpacingMode);
