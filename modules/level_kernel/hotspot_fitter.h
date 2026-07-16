/**************************************************************************/
/*  hotspot_fitter.h                                                      */
/**************************************************************************/
/*  G-Level LE3: deterministic, editor-independent hotspot fitting.       */
/**************************************************************************/

#pragma once

#include "core/object/ref_counted.h"
#include "core/variant/dictionary.h"
#include "core/variant/type_info.h"

class HotspotAtlas;
class LevelMesh;

class HotspotFitter : public RefCounted {
	GDCLASS(HotspotFitter, RefCounted);

protected:
	static void _bind_methods();

public:
	enum IslandMode {
		ISLAND_GROUPED = 0,
		ISLAND_INDIVIDUAL = 1,
	};

	static constexpr real_t DEFAULT_DENSITY_MARGIN = (real_t)0.35;
	static constexpr real_t DEFAULT_ASPECT_MARGIN = (real_t)0.20;
	static constexpr real_t DEFAULT_COS_COPLANAR = (real_t)0.9998;
	static constexpr real_t DEFAULT_COS_COLLINEAR = (real_t)0.9998;
	static constexpr real_t DEFAULT_HORIZONTAL_BIAS_DEGREES = (real_t)15.0;
	static constexpr real_t DEFAULT_INSET_MIP_BLEED = (real_t)1.0;

	// The five-argument form is the stable fitter entry. Options are additive:
	// editor settings, material texture dimensions, mapping override, and the
	// local-to-world transform can be supplied without coupling the kernel to
	// editor-only services.
	Dictionary fit(const PackedInt32Array &p_face_ids, const Ref<LevelMesh> &p_mesh,
			const Ref<HotspotAtlas> &p_atlas, int p_island_mode, int64_t p_seed,
			const Dictionary &p_options = Dictionary()) const;
};

VARIANT_ENUM_CAST(HotspotFitter::IslandMode);
