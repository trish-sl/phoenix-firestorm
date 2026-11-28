/**
 * @file class2/deferred/sunLightSSAOF.glsl
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2007, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

/*[EXTRA_CODE_HERE]*/

// <FS:Trish> FSRenderShadowScreenFilter: a second target takes the sun shadow's penumbra radius
// (screen pixels) for the screen-space filter that follows (fsShadowScreenFilterF.glsl)
//out vec4 frag_color;
#if defined(FS_SHADOW_SCREEN_FILTER)
out vec4 frag_data[2];
#define frag_color frag_data[0]
float fsSunShadowPenumbra(); // shadowUtil.glsl, PCSS build
#else
out vec4 frag_color;
#endif
// </FS:Trish>

//class 2 -- shadows and SSAO

// Inputs
in vec2 vary_fragcoord;

// See sunLightF.glsl: non-zero for a projector slot no shadow-casting spot light occupies.
uniform ivec2 spot_shadow_inactive;

vec4 getPositionWithDepth(vec2 pos_screen, float depth);
float getDepth(vec2 pos_screen);
bool isFarDepth(float d);
vec4 getNorm(vec2 pos_screen);

float sampleDirectionalShadow(vec3 shadow_pos, vec3 norm, vec2 pos_screen);
// <FS:Trish> FSRenderShadowSlopeBias: the surface's normal from the depth buffer (fsShadowSlopeF.glsl)
#if defined(FS_SHADOW_DEPTH_SLOPE)
vec3 fsDepthSlopeNormal(vec2 tc, vec3 pos, vec3 fallback);
#endif
// </FS:Trish>
float sampleSpotShadow(vec3 shadow_pos, vec3 norm, int index, vec2 pos_screen);
#if defined(FS_CLOUD_SHADOWS)
float fsSunShadowExtras(float shadow, vec3 pos, vec3 norm); // fsShadowExtrasF.glsl
#endif
float calcAmbientOcclusion(vec4 pos, vec3 norm, vec2 pos_screen);
#ifdef FS_GTAO
float fsCalcGTAO(vec3 pos, vec3 norm, vec2 pos_screen);
#endif
// <FS:Trish> FSRenderCreaseAO: short range occlusion of corners and creases (fsCreaseAOF.glsl)
#ifdef FS_CREASE_AO
float fsCreaseAO(vec3 pos, vec3 norm, vec2 tc);
#endif
// </FS:Trish>
vec3 getGeoNorm(vec2 pos_screen);
vec4 getNormAndGeoNorm(vec2 screenpos, out vec3 geo); // <FS:Trish/> deferredUtil.glsl: both from one fetch

void main()
{
    vec2 pos_screen = vary_fragcoord.xy;
    float depth = getDepth(pos_screen);

    // Sky: the shadow terms are already fully lit beyond the last split, and the lighting pass
    // never reads the occlusion of a sky fragment -- so the eight SSAO taps are spent on nothing.
    // The blur rejects sky samples by plane distance, so neighbours do not see the difference.
    if (isFarDepth(depth))
    {
        frag_color = vec4(1.0);
#if defined(FS_SHADOW_SCREEN_FILTER)
        frag_data[1] = vec4(0.0); // <FS:Trish/>
#endif
        return;
    }

    vec4 pos  = getPositionWithDepth(pos_screen, depth);
    // <FS:Trish> The shading and the geometric normal from one fetch of the normal attachment, not
    // one for getNorm and one per getGeoNorm below
    //vec4 norm = getNorm(pos_screen);
    vec3 fs_geo_norm;
    vec4 norm = getNormAndGeoNorm(pos_screen, fs_geo_norm);
    // </FS:Trish>

    vec4 col;
    // <FS:Trish> FSRenderShadowSlopeBias: the sun shadow's bias and offset follow the surface as
    // drawn, not its normal map
    //col.r = sampleDirectionalShadow(pos.xyz, norm.xyz, pos_screen);
#if defined(FS_SHADOW_DEPTH_SLOPE)
    col.r = sampleDirectionalShadow(pos.xyz, fsDepthSlopeNormal(pos_screen, pos.xyz, norm.xyz), pos_screen);
#else
    col.r = sampleDirectionalShadow(pos.xyz, norm.xyz, pos_screen);
#endif
    // </FS:Trish>
#if defined(FS_CLOUD_SHADOWS)
    col.r = fsSunShadowExtras(col.r, pos.xyz, norm.xyz);
#endif
    // Geometric normal, not the shading one. The occlusion test compares neighbouring depths
    // against this plane, and depth knows nothing about normal-mapped detail -- testing against a
    // perturbed normal makes a flat wall occlude itself wherever its map has texture.
#ifdef FS_GTAO
    //col.g = fsCalcGTAO(pos.xyz, getGeoNorm(pos_screen), pos_screen);
    col.g = fsCalcGTAO(pos.xyz, fs_geo_norm, pos_screen); // <FS:Trish/>
#else
    //col.g = calcAmbientOcclusion(pos, getGeoNorm(pos_screen), pos_screen);
    col.g = calcAmbientOcclusion(pos, fs_geo_norm, pos_screen); // <FS:Trish/>
#endif
    // <FS:Trish> FSRenderCreaseAO: the corners and creases the wider search above barely sees, on top
#ifdef FS_CREASE_AO
    //col.g *= fsCreaseAO(pos.xyz, getGeoNorm(pos_screen), pos_screen);
    col.g *= fsCreaseAO(pos.xyz, fs_geo_norm, pos_screen);
#endif
    // </FS:Trish>
    col.b = spot_shadow_inactive.x != 0 ? 1.0 : sampleSpotShadow(pos.xyz, norm.xyz, 0, pos_screen);
    col.a = spot_shadow_inactive.y != 0 ? 1.0 : sampleSpotShadow(pos.xyz, norm.xyz, 1, pos_screen);

    frag_color = clamp(col, vec4(0), vec4(1));
#if defined(FS_SHADOW_SCREEN_FILTER)
    frag_data[1] = vec4(fsSunShadowPenumbra(), 0.0, 0.0, 1.0); // <FS:Trish/>
#endif
}
