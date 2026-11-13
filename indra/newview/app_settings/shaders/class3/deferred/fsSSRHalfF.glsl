/**
 * @file fsSSRHalfF.glsl
 * @brief Screen-space reflections traced once per 2x2 pixels (FSRenderSSRHalfRes)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
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
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

// The opaque surfaces' screen-space reflection, traced at half resolution before the lighting
// pass. Texel t traces the full-resolution pixel 2t with exactly what softenLightF would hand
// the march there (same glossiness, same direction, the wet shoreline included), and
// softenLightF's upsample (fsSSRHalfUpsample) reads it back weighted by how well that pixel's
// depth and normal match its own. Output: the march's colour and blend weight, 0 where nothing
// was traced (sky, rough surfaces).

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

uniform vec2 fs_ssr_full_res;   // full-resolution size in pixels
uniform sampler2D sceneMap;
uniform vec4 waterPlane;

float getDepth(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);
bool isFarDepth(float d);
GBufferInfo getGBuffer(vec2 screenpos);
float unpackRoughness(vec2 p);
vec3 getSpecularDominantDir(vec3 n, vec3 r, float perceptualRoughness);
float tapScreenSpaceReflection(int totalSamples, vec2 tc, vec3 viewPos, vec3 n, vec3 rayDir, inout vec4 collectedColor, sampler2D source, float glossiness);
float ssrMinGlossiness();
#ifdef FS_WET_SHORE
float fsWetShore(vec3 pos, vec3 norm, vec4 water_plane);
#endif

void main()
{
    ivec2 full = min(ivec2(gl_FragCoord.xy) * 2, ivec2(fs_ssr_full_res) - 1);
    vec2 tc = (vec2(full) + 0.5) / fs_ssr_full_res;

    float depth = getDepth(tc);
    if (isFarDepth(depth))
    {
        frag_color = vec4(0);
        return;
    }

    GBufferInfo gb = getGBuffer(tc);
    bool is_pbr = GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_PBR);
    if (!is_pbr && (GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_HDRI) || GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_SKIP_ATMOS)))
    {
        frag_color = vec4(0); // sky: softenLightF copies a colour there and reflects nothing
        return;
    }

    vec4 pos = getPositionWithDepth(tc, depth);

    float wet = 0.0;
#ifdef FS_WET_SHORE
    wet = fsWetShore(pos.xyz, gb.normal, waterPlane);
#endif

    vec3 refl = normalize(reflect(pos.xyz, gb.normal));
    vec3 dir;
    float gloss;
    if (is_pbr)
    {
        float roughness = unpackRoughness(gb.specular.ga);
        roughness = mix(roughness, min(roughness, 0.15), wet);
        gloss = 1.0 - roughness;
        dir = getSpecularDominantDir(gb.normal, refl, roughness);
    }
    else
    {
        gloss = mix(gb.specular.a, max(gb.specular.a, 0.8), wet);
        dir = refl;
    }

    vec4 ssr = vec4(0);
    if (gloss > ssrMinGlossiness())
    {
        tapScreenSpaceReflection(1, tc, pos.xyz, gb.normal, dir, ssr, sceneMap, gloss);
    }
    frag_color = ssr;
}
