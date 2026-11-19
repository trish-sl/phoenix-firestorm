/**
 * @file screenSpaceReflResolveF.glsl
 * @brief Temporal resolve of the screen-space reflection trace
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
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

// Temporal accumulation of the screen-space reflection trace (screenSpaceReflTraceF.glsl), in the
// trace's encoded space (blend weight premultiplied, luminance compressed). From the graphics
// care package's resolve (Geenz, SLV project/graphics-care-package), with this viewer's G-buffer
// decoding and the trace's texel to pixel mapping; no SMAA T2x jitter here.

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

uniform sampler2D fs_ssr_trace;     // this frame's trace (SSR resolution)
uniform sampler2D fs_ssr_history;   // last frame's resolve (SSR resolution)

uniform vec2 fs_ssr_full_res;       // full-resolution size in pixels
uniform vec2 fs_ssr_res;            // SSR buffer size in pixels
uniform mat4 projection_matrix;
uniform mat4 inv_modelview_delta;   // current camera space -> previous camera space
uniform float ssrHistoryBlend;      // RenderScreenSpaceReflectionTemporalBlend, 0 without history

float getDepth(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);
bool isFarDepth(float d);
GBufferInfo getGBuffer(vec2 screenpos);
float unpackRoughness(vec2 p);

// <FS:Trish> A value that isn't a finite number would stay in the history for good (mix(x, NaN, 0)
// is NaN) and spread through the 3x3 moments. Such values are left out instead.
bool fsResolveFinite(vec4 v)
{
    return !(any(isnan(v)) || any(isinf(v)));
}
// </FS:Trish>

// single-scalar clip; per-channel clamps break premultiplied rgb/a ratios
vec4 clipToAABB(vec4 v, vec4 mn, vec4 mx)
{
    vec4 center = 0.5 * (mx + mn);
    vec4 extent = 0.5 * (mx - mn) + 1e-4;
    vec4 delta  = v - center;
    vec4 ts     = abs(delta) / extent;
    float t     = max(max(ts.x, ts.y), max(ts.z, ts.w));
    return (t > 1.0) ? (center + delta / t) : v;
}

// 5-tap Catmull-Rom; bilinear refetch compounds into blur over frames
vec4 sampleHistoryCatmullRom(vec2 uv, vec2 texSize)
{
    vec2 samplePos = uv * texSize;
    vec2 texPos1 = floor(samplePos - 0.5) + 0.5;
    vec2 f = samplePos - texPos1;

    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);

    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / w12;

    vec2 texPos0  = (texPos1 - 1.0) / texSize;
    vec2 texPos3  = (texPos1 + 2.0) / texSize;
    vec2 texPos12 = (texPos1 + offset12) / texSize;

    vec4 result = vec4(0.0);
    float wSum = 0.0;

    result += textureLod(fs_ssr_history, vec2(texPos12.x, texPos0.y), 0.0) * (w12.x * w0.y);
    wSum   += w12.x * w0.y;
    result += textureLod(fs_ssr_history, vec2(texPos0.x, texPos12.y), 0.0) * (w0.x * w12.y);
    wSum   += w0.x * w12.y;
    result += textureLod(fs_ssr_history, texPos12, 0.0) * (w12.x * w12.y);
    wSum   += w12.x * w12.y;
    result += textureLod(fs_ssr_history, vec2(texPos3.x, texPos12.y), 0.0) * (w3.x * w12.y);
    wSum   += w3.x * w12.y;
    result += textureLod(fs_ssr_history, vec2(texPos12.x, texPos3.y), 0.0) * (w12.x * w3.y);
    wSum   += w12.x * w3.y;

    return max(result / wSum, vec4(0.0));
}

void main()
{
    ivec2 px = ivec2(gl_FragCoord.xy);
    vec4 cur = texelFetch(fs_ssr_trace, px, 0);
    cur = fsResolveFinite(cur) ? cur : vec4(0.0); // <FS:Trish/> a bad trace texel is a miss

    // the full-resolution pixel the trace took this texel from
    ivec2 full = min(ivec2(gl_FragCoord.xy * (fs_ssr_full_res / fs_ssr_res)), ivec2(fs_ssr_full_res) - 1);
    vec2 tc = (vec2(full) + 0.5) / fs_ssr_full_res;

    float depth = getDepth(tc);
    if (isFarDepth(depth))
    { // sky
        frag_color = cur;
        return;
    }

    GBufferInfo gb = getGBuffer(tc);
    float roughness;
    if (GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_PBR))
        roughness = unpackRoughness(gb.specular.ga);
    else
        roughness = 1.0 - gb.specular.a;

    // 3x3 moments: mean doubles as spatial ray reuse, variance bounds history
    vec4 m1 = vec4(0.0);
    vec4 m2 = vec4(0.0);
    ivec2 sz = textureSize(fs_ssr_trace, 0);
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec4 s = texelFetch(fs_ssr_trace, clamp(px + ivec2(x, y), ivec2(0), sz - 1), 0);
            s = fsResolveFinite(s) ? s : vec4(0.0); // <FS:Trish/>
            m1 += s;
            m2 += s * s;
        }
    }
    vec4 mu = m1 / 9.0;
    vec4 sigma = sqrt(max(m2 / 9.0 - mu * mu, vec4(0.0)));

    // reuse neighbors' rays where the GGX lobe is wide; mirrors keep their own sample
    vec4 filtered = mix(cur, mu, smoothstep(0.0, 0.2, roughness));

    if (ssrHistoryBlend <= 0.0)
    {
        frag_color = filtered;
        return;
    }

    // camera-only reprojection into last frame's screen space
    vec3 viewPos  = getPositionWithDepth(tc, depth).xyz;
    vec3 prevPos  = (inv_modelview_delta * vec4(viewPos, 1.0)).xyz;
    vec4 prevClip = projection_matrix * vec4(prevPos, 1.0);
    if (prevClip.w <= 0.0)
    {
        frag_color = filtered;
        return;
    }
    vec2 prevUV = (prevClip.xy / prevClip.w) * 0.5 + 0.5;

    float w = ssrHistoryBlend;
    if (any(lessThan(prevUV, vec2(0.0))) || any(greaterThan(prevUV, vec2(1.0))))
    {
        w = 0.0;
    }

    // Salvi variance bound; min/max would clamp accumulation to the noise extremes
    // <FS:Trish> a bad history is no history, and without history weight it isn't touched at all
    //vec4 hist = clipToAABB(sampleHistoryCatmullRom(prevUV, vec2(textureSize(fs_ssr_history, 0))),
    //                       mu - sigma * 1.25, mu + sigma * 1.25);
    vec4 hist_raw = sampleHistoryCatmullRom(prevUV, vec2(textureSize(fs_ssr_history, 0)));
    if (!fsResolveFinite(hist_raw))
    {
        w = 0.0;
        hist_raw = filtered;
    }
    vec4 hist = clipToAABB(hist_raw, mu - sigma * 1.25, mu + sigma * 1.25);
    // </FS:Trish>

    // alpha blends too, so accumulated confidence stays coherent with color
    //frag_color = mix(filtered, hist, w);
    frag_color = w > 0.0 ? mix(filtered, hist, w) : filtered; // <FS:Trish/>
}
