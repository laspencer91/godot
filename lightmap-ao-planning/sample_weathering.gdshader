// Sample weathering shader for AOBaker3D.
//
// After a bake, AOBaker3D::apply_to_meshes() automatically sets:
//   * ao_atlas    (material uniform)  -- the shared scene-space AO Texture2DArray
//   * ao_uv_rect  (instance uniform)  -- this mesh's rect into the atlas (xy=offset, zw=scale)
//   * ao_slice    (instance uniform)  -- this mesh's atlas layer
// so a single copy of this material can sit on many meshes and each samples its own slot.
//
// Two ways to use it:
//   1. As a mesh's material (or material_override) -- edit the base look below.
//   2. As a next_pass overlay on top of an imported material -- keep the base material, drop this
//      into its "Next Pass" slot, and grime is layered on non-destructively.
//
// AO convention: ao_atlas.r is OPENNESS (1 = open surface, 0 = deep crevice), so grime accumulates
// where openness is low. This is a grime MASK for albedo -- it is not lighting; leave lightmaps to
// the light. Do not add it to EMISSION.

shader_type spatial;

uniform sampler2DArray ao_atlas : filter_linear;
uniform vec3 grime_color : source_color = vec3(0.05, 0.04, 0.03);
uniform float grime_strength : hint_range(0.0, 1.0) = 0.8;
uniform sampler2D base_albedo : source_color, hint_default_white;
uniform vec3 base_color : source_color = vec3(0.8, 0.8, 0.8);

instance uniform vec4 ao_uv_rect = vec4(0.0, 0.0, 1.0, 1.0);
instance uniform float ao_slice = 0.0;

void fragment() {
	vec2 ao_uv = UV2 * ao_uv_rect.zw + ao_uv_rect.xy;
	float openness = texture(ao_atlas, vec3(ao_uv, ao_slice)).r; // 1 = open, 0 = occluded.
	float grime = (1.0 - openness) * grime_strength; // Grime settles where surfaces meet / crevices.

	vec3 albedo = texture(base_albedo, UV).rgb * base_color;
	ALBEDO = mix(albedo, grime_color, grime);
	ROUGHNESS = mix(0.5, 1.0, grime); // Grime is duller than clean surface (optional flavor).
}
