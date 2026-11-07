// Coloured projector shadows: a projector's light through translucent casters
// (FSRenderColoredShadows and FSRenderColoredShadowsProjectors)

// Linked into the projector programs next to spotLightF.glsl, only while both settings are on
// (FS_SPOT_COLORED_SHADOW). spotLightF multiplies the projector's light by
// fsSpotColoredTransmittance.
//
// Each shadowed projector has an RGBA8 colour map sharing its shadow depth buffer, drawn during
// its shadow pass the way the sun cascades' maps are (fsColoredShadowCasterF.glsl): the product
// of the transmittance of every alpha-blended caster in a texel in front of the nearest opaque
// caster, white where there is none. Projectors are drawn one light at a time, so the pipeline
// binds that light's map and matrix to one extra texture unit before each draw, white with
// fs_spot_colored_valid = 0 for a light without a map (unshadowed, nothing translucent in its
// frustum, reflection probes).

/*[EXTRA_CODE_HERE]*/

uniform sampler2D fs_spot_colored_map;
uniform mat4 fs_spot_colored_matrix;    // view space to this light's shadow map, [0, 1]
uniform float fs_spot_colored_valid;    // 1 when this light's map was drawn for its current shadow
uniform float spot_shadow_offset;
uniform float spot_shadow_reach;        // metres: projector shadows fade out towards this camera distance
uniform float shadow_fade;              // 1 - the slot's fade in, as the shadow itself is faded

// The projector's transmittance (linear rgb, 1 = unfiltered) at view space position pos, normal
// norm: the same normal offset and camera distance fade as the projector's shadow, and its fade
// in and out as the light enters or leaves a shadow slot.
vec3 fsSpotColoredTransmittance(vec3 pos, vec3 norm)
{
    if (fs_spot_colored_valid < 0.5)
    {
        return vec3(1.0);
    }

    pos += norm * spot_shadow_offset;

    float reach = spot_shadow_reach > 1.0 ? spot_shadow_reach : 128.0;
    float fade = clamp((reach - length(pos)) / (0.2 * reach), 0.0, 1.0);
    fade *= 1.0 - clamp(shadow_fade, 0.0, 1.0);
    if (fade <= 0.0)
    {
        return vec3(1.0);
    }

    vec4 lpos = fs_spot_colored_matrix * vec4(pos, 1.0);
    if (lpos.w <= 0.0)
    {
        return vec3(1.0);
    }
    vec2 uv = lpos.xy / lpos.w;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
    {
        return vec3(1.0);
    }

    vec3 t = clamp(texture(fs_spot_colored_map, uv).rgb, vec3(0.0), vec3(1.0));
    return mix(vec3(1.0), t, fade);
}
