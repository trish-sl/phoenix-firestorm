/**
 * @file blurLightF.glsl
 *
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

// <FS:Trish> FSRenderShadowScreenFilter: the second pass also writes the filtered sun shadow and
// its depth, as next frame's history (fsShadowScreenFilterF.glsl)
//out vec4 frag_color;
#if defined(FS_SHADOW_SCREEN_FILTER)
out vec4 frag_data[2];
#define frag_color frag_data[0]
float fsScreenFilterShadow(float blurred, vec2 tc, vec3 pos, vec3 norm, out vec2 range);
float fsScreenFilterTemporal(float shadow, vec2 range, vec3 pos);
#else
out vec4 frag_color;
#endif
// </FS:Trish>

uniform sampler2D lightMap;

uniform float dist_factor;
uniform float blur_size;
uniform vec2 delta;
uniform vec2 screen_res;
uniform vec3 kern[4];
uniform float kern_scale;
uniform mat4 inv_proj;

in vec2 vary_fragcoord;

vec4 getPosition(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);
float getDepth(vec2 pos_screen);
bool isFarDepth(float d);
float ndcZFromScreenDepth(float d);
vec4 getNorm(vec2 pos_screen);
vec3 getGeoNorm(vec2 pos_screen); // <FS:Trish/> deferredUtil.glsl
vec3 fsPositionWithDepthPersp(vec2 pos_screen, float depth); // <FS:Trish/> deferredUtil.glsl: the camera's perspective inverse, seven terms

// <FS:Trish> The plane of the surface as the depth buffer holds it, from the nearer neighbour on
// each axis (so a silhouette picks the pixel's own surface); fallback where there is none. The
// taps below are tested by their depth, so this is the plane that test needs: a legacy surface
// has no geometric normal in the G-buffer, and its normal-mapped one tilted the plane with
// every bump of a stone or brick wall.
bool fsBlurNeighbour(vec2 tc, out vec3 p)
{
    float d = getDepth(tc);
    if (isFarDepth(d))
    {
        p = vec3(0.0);
        return false;
    }
    p = fsPositionWithDepthPersp(tc, d);
    return true;
}

vec3 fsBlurDepthNormal(vec2 tc, vec3 pos, vec3 fallback)
{
    vec2 px = 1.0 / screen_res;
    vec3 pr, pl, pu, pd;
    bool r = fsBlurNeighbour(tc + vec2(px.x, 0.0), pr);
    bool l = fsBlurNeighbour(tc - vec2(px.x, 0.0), pl);
    bool u = fsBlurNeighbour(tc + vec2(0.0, px.y), pu);
    bool d = fsBlurNeighbour(tc - vec2(0.0, px.y), pd);
    if (!(r || l) || !(u || d))
    {
        return fallback;
    }
    vec3 dr = pr - pos;
    vec3 dl = pos - pl;
    vec3 du = pu - pos;
    vec3 dd = pos - pd;
    vec3 dx = (r && (!l || abs(dr.z) <= abs(dl.z))) ? dr : dl;
    vec3 dy = (u && (!d || abs(du.z) <= abs(dd.z))) ? du : dd;
    vec3 n = cross(dx, dy);
    float len = length(n);
    if (len < 1e-10)
    {
        return fallback;
    }
    return n / len; // only the plane matters here, not which way it faces
}
// </FS:Trish>

void main()
{
    vec2 tc = vary_fragcoord.xy;
    vec4 ccol = texture(lightMap, tc).rgba;

    // Sky: the lighting pass never reads the light map there, and a nearby surface rejects a sky
    // sample by plane distance, so the twelve neighbour taps would be spent on nothing.
    float depth = getDepth(tc);
    if (isFarDepth(depth))
    {
        frag_color = max(ccol, vec4(0));
#if defined(FS_SHADOW_SCREEN_FILTER)
        frag_data[1] = vec4(1.0, 0.0, 0.0, 1.0); // <FS:Trish/> no depth: never taken as history
#endif
        return;
    }

    vec4 norm = getNorm(tc);
    vec3 pos = getPositionWithDepth(tc, depth).xyz;

    vec2 dlt = kern_scale * delta / (1.0+norm.xy*norm.xy);
    dlt /= max(-pos.z*dist_factor, 1.0);

    vec2 defined_weight = kern[0].xy; // special case the first (centre) sample's weight in the blur; we have to sample it anyway so we get it for 'free'
    vec4 col = defined_weight.xyxx * ccol;

    // relax tolerance according to distance to avoid speckling artifacts, as angles and distances are a lot more abrupt within a small screen area at larger distances
    float pointplanedist_tolerance_pow2 = pos.z*pos.z*0.00005;

    // <FS:Trish> The plane the taps are tested against is the surface's geometric one (getGeoNorm,
    // which legacy surfaces answer with their shading normal as before). With the normal-mapped
    // shading normal the plane tilted with every bump, so on a bumpy wall or floor most taps a few
    // centimetres away fell outside the tolerance and were dropped: the AO and shadow noise the
    // blur exists to remove (GTAO's step pattern, the cascade dither) stayed on those surfaces.
    // The plane is now the depth buffer's own (fsBlurDepthNormal): legacy surfaces answer
    // getGeoNorm with their normal-mapped normal, which left the same noise on them.
    vec3 plane_n = fsBlurDepthNormal(tc, pos, getGeoNorm(tc));
    // </FS:Trish>

    // Each tap only needs its distance from this fragment's plane, dot(n, P) - dot(n, pos), where
    // P = inv_proj * ndc / w. Fold n into the matrix once here and that becomes two dot products
    // per tap instead of a full unprojection.
    // <FS:Trish> the geometric plane, see plane_n
    vec4 nRow = vec4(dot(plane_n, inv_proj[0].xyz), dot(plane_n, inv_proj[1].xyz),
                     dot(plane_n, inv_proj[2].xyz), dot(plane_n, inv_proj[3].xyz));
    // </FS:Trish>
    vec4 wRow = vec4(inv_proj[0].w, inv_proj[1].w, inv_proj[2].w, inv_proj[3].w);
    // <FS:Trish> the geometric plane, see plane_n
    float nDotPos = dot(plane_n, pos.xyz);
    // </FS:Trish>

    // perturb sampling origin slightly in screen-space to hide edge-ghosting artifacts where smoothing radius is quite large
    tc *= screen_res;
    float tc_mod = 0.5*(tc.x + tc.y);
    tc_mod -= floor(tc_mod);
    tc_mod *= 2.0;
    tc += ( (tc_mod - 0.5) * kern[1].z * dlt * 0.5 );

    // TODO: move this to kern instead of building kernel per pixel
    vec3 k[7];
    k[0] = kern[0];
    k[2] = kern[1];
    k[4] = kern[2];
    k[6] = kern[3];

    k[1] = (k[0]+k[2])*0.5f;
    k[3] = (k[2]+k[4])*0.5f;
    k[5] = (k[4]+k[6])*0.5f;

    for (int i = 1; i < 7; i++)
    {
        vec2 samptc = tc + k[i].z*dlt*2.0;
        samptc /= screen_res;
        vec4 sampndc = vec4(samptc * 2.0 - 1.0, ndcZFromScreenDepth(getDepth(samptc)), 1.0);

        float d = dot(nRow, sampndc) / dot(wRow, sampndc) - nDotPos; // dist from plane

        if (d*d <= pointplanedist_tolerance_pow2)
        {
            col += texture(lightMap, samptc)*k[i].xyxx;
            defined_weight += k[i].xy;
        }
    }

    for (int i = 1; i < 7; i++)
    {
        vec2 samptc = tc - k[i].z*dlt*2.0;
        samptc /= screen_res;
        vec4 sampndc = vec4(samptc * 2.0 - 1.0, ndcZFromScreenDepth(getDepth(samptc)), 1.0);

        float d = dot(nRow, sampndc) / dot(wRow, sampndc) - nDotPos; // dist from plane

        if (d*d <= pointplanedist_tolerance_pow2)
        {
            col += texture(lightMap, samptc)*k[i].xyxx;
            defined_weight += k[i].xy;
        }
    }

    col /= defined_weight.xyxx;
    //col.y *= col.y;
    // <FS:Trish> FSRenderShadowScreenFilter: the sun shadow over its penumbra, then the history
#if defined(FS_SHADOW_SCREEN_FILTER)
    vec2 fs_range;
    col.r = fsScreenFilterShadow(col.r, vary_fragcoord.xy, pos, norm.xyz, fs_range);
    col.r = fsScreenFilterTemporal(col.r, fs_range, pos);
    frag_data[1] = vec4(clamp(col.r, 0.0, 1.0), -pos.z, 0.0, 1.0);
#endif
    // </FS:Trish>

    frag_color = max(col, vec4(0));

#ifdef IS_AMD_CARD
    // If it's AMD make sure the GLSL compiler sees the arrays referenced once by static index. Otherwise it seems to optimise the storage awawy which leads to unfun crashes and artifacts.
    vec3 dummy1 = kern[0];
    vec3 dummy2 = kern[3];
#endif
}

