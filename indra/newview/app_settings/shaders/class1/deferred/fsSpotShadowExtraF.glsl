/**
 * @file fsSpotShadowExtraF.glsl
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

// Projector shadows past the first two (FSRenderSpotShadowCount above 2). The first two are
// sampled once per pixel in the sun light pass and stored in lightMap.b and .a. The others are
// sampled here, by the projector's own draw: the pipeline binds that light's shadow map to
// fs_spot_shadow_map and its matrix to fs_spot_shadow_matrix before each draw, so one texture
// unit serves any number of them. Linked into the projector programs only with the
// FS_SPOT_SHADOW_EXTRA permutation.

/*[EXTRA_CODE_HERE]*/

uniform sampler2DShadow fs_spot_shadow_map;
uniform mat4 fs_spot_shadow_matrix;     // view space to this light's shadow map, [0, 1]
uniform float spot_shadow_offset;
uniform float spot_shadow_reach;        // metres: projector shadows fade out towards this camera distance

float pcfSpotShadow(sampler2DShadow shadowMap, vec4 stc, float bias_scale, vec2 pos_screen);

// PCSS (RenderShadowPCSS): the program also gets PCSS_SHADOW, which links the PCSS build of
// shadowUtil.glsl, and on GL 3.3 FS_SPOT_PCSS_FETCH, which matches that build's
// PCSS_SHADOW_FETCH. The contact hardening filter then needs a plain depth read of the same map:
// fs_spot_shadow_depth, bound to the same texture as fs_spot_shadow_map on a second extra unit
// with the depth fetch sampler object (compare mode off), rebound per light like the map.
#if defined(PCSS_SHADOW)
uniform float spot_shadow_bias;
uniform vec2 proj_shadow_res;
float pcssPoissonPCF(sampler2DShadow shadowMap, mat4 m, vec4 stc, vec3 pos, vec3 norm, float z_bias, vec2 res);
#if defined(FS_SPOT_PCSS_FETCH)
uniform sampler2D fs_spot_shadow_depth;
float pcssSpotContactShadow(sampler2DShadow shadowMap, sampler2D depthMap, mat4 m, vec4 stc, vec3 pos, vec3 norm, float bias_scale);
#endif
#endif

// The same as sampleSpotShadow (shadowUtil.glsl) does for the first two: normal offset, the
// distance fade over the last 20 % of spot_shadow_reach, then the five tap spot PCF, or with
// PCSS the same filter the first two get in the sun light pass.
float fsSampleSpotShadowExtra(vec3 pos, vec3 norm)
{
    pos += norm * spot_shadow_offset;

    float reach = spot_shadow_reach > 1.0 ? spot_shadow_reach : 128.0;
    float fade = clamp((reach - length(pos)) / (0.2 * reach), 0.0, 1.0);
    if (fade <= 0.0)
    {
        return 1.0;
    }

    vec4 lpos = fs_spot_shadow_matrix * vec4(pos, 1.0);
    if (lpos.w <= 0.0)
    {
        return 1.0; // behind the projector: the divide would mirror the map
    }
#if defined(PCSS_SHADOW) && defined(FS_SPOT_PCSS_FETCH)
    float shadow = pcssSpotContactShadow(fs_spot_shadow_map, fs_spot_shadow_depth, fs_spot_shadow_matrix, lpos, pos, norm, 0.8);
#elif defined(PCSS_SHADOW)
    float shadow = pcssPoissonPCF(fs_spot_shadow_map, fs_spot_shadow_matrix, lpos, pos, norm, spot_shadow_bias * 0.8, proj_shadow_res);
#else
    float shadow = pcfSpotShadow(fs_spot_shadow_map, lpos, 0.8, pos.xy);
#endif
    return mix(1.0, shadow, fade);
}
