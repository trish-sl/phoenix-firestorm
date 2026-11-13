/**
 * @file fsSpotVolumetricTemporalF.glsl
 * @brief Blends the projector volumetric light with the last frames' (FSRenderSpotVolumetricTemporal)
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

// With the temporal option the march takes half the steps and moves their start offset every
// frame, so each frame samples between the last one's steps. Here each texel finds where its
// point along the view ray was last frame, takes the history there, keeps it within the range of
// this frame's neighbourhood (so a moving light or object doesn't leave a trail), and blends.
// The result is next frame's history, and what the composite filters up.

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

in vec2 vary_fragcoord;

uniform sampler2D diffuseRect;      // this frame's march: rgb light, a distance marched to
uniform sampler2D specularRect;     // the resolved result of the last frame, same layout
uniform mat4 fs_vol_inv_proj;       // this frame's inverse projection
uniform mat4 fs_vol_reproject;      // this frame's camera space to last frame's clip space
uniform float fs_vol_history;       // weight of the history, 0 when there is none

const float SKY_DIST = 65000.0;

void main()
{
    ivec2 size = textureSize(diffuseRect, 0);
    ivec2 texel = ivec2(gl_FragCoord.xy);
    vec4 cur = texelFetch(diffuseRect, texel, 0);

    if (fs_vol_history <= 0.0)
    {
        frag_color = cur;
        return;
    }

    // the neighbourhood's range of this frame's light
    vec3 lo = cur.rgb;
    vec3 hi = cur.rgb;
    for (int i = 0; i < 9; ++i)
    {
        if (i == 4)
        {
            continue;
        }
        ivec2 o = ivec2(i % 3, i / 3) - ivec2(1);
        vec3 n = texelFetch(diffuseRect, clamp(texel + o, ivec2(0), size - ivec2(1)), 0).rgb;
        lo = min(lo, n);
        hi = max(hi, n);
    }

    // where this texel's point (the view ray at the distance it was marched to) was last frame
    vec2 tc = vary_fragcoord.xy;
    vec4 v = fs_vol_inv_proj * vec4(tc * 2.0 - 1.0, 1.0, 1.0);
    vec3 dir = normalize(v.xyz / v.w);
    float dist = min(cur.a, SKY_DIST);
    vec4 prev = fs_vol_reproject * vec4(dir * dist, 1.0);
    if (prev.w <= 0.0)
    {
        frag_color = cur;
        return;
    }
    vec2 prev_tc = prev.xy / prev.w * 0.5 + 0.5;
    if (any(lessThan(prev_tc, vec2(0.0))) || any(greaterThan(prev_tc, vec2(1.0))))
    {
        frag_color = cur;
        return;
    }

    vec4 hist = texture(specularRect, prev_tc);
    // the history saw another surface there (disocclusion): start over
    float rel = abs(hist.a - cur.a) / max(cur.a, 0.1);
    float w = fs_vol_history * (1.0 - smoothstep(0.05, 0.2, rel));

    vec3 clamped = clamp(hist.rgb, lo, hi);
    frag_color = vec4(mix(cur.rgb, clamped, w), cur.a);
}
