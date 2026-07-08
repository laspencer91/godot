// Sample weathering shader for AOBaker3D.
//
// AO is delivered by the engine as a first-class per-instance channel (like a lightmap). After a
// bake, AOBaker3D::apply_to_meshes() calls RenderingServer::instance_geometry_set_ao_map() on each
// baked mesh, and the shader reads the result through the built-in float:
//
//     AO_MAP   // openness: 1.0 = fully open surface, 0.0 = deep crevice / contact seam
//
// There is NO texture uniform, no UV2 wiring, no instance params, and no material mutation -- the
// renderer samples the shared atlas per-instance for you. Just drop this material on the baked
// meshes (or into an imported material's Next Pass) and bake.
//
// AO_MAP is a grime MASK for albedo, not lighting -- leave lightmaps to the light. Don't add it to
// EMISSION. On backends other than Forward+ (Mobile / Compatibility) AO_MAP is currently 1.0.

shader_type spatial;

uniform vec3 grime_color : source_color = vec3(0.05, 0.04, 0.03);
uniform float grime_strength : hint_range(0.0, 1.0) = 0.8;
uniform vec3 base_color : source_color = vec3(0.8, 0.8, 0.8);

void fragment() {
	float openness = AO_MAP; // 1.0 = open, 0.0 = occluded.
	float grime = (1.0 - openness) * grime_strength; // Grime settles where surfaces meet / crevices.

	ALBEDO = mix(base_color, grime_color, grime);
	ROUGHNESS = mix(0.5, 1.0, grime); // Grime is duller than clean surface (optional flavor).
}
