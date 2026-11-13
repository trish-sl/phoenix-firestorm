/**
 * @file fsIndirectTemporalF.glsl
 * @brief Blends the half-resolution bent normals and indirect light with the last frames
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

// fsIndirectHalfF.glsl takes two slices with a new rotation every frame, so a single frame is
// noisy. Here each texel first averages its 3x3 neighbours on the same surface (by depth), then
// finds where its point was last frame and blends with the history there, kept within the range
// of this frame's neighbourhood (so a light switched off doesn't linger) and dropped where the
// history saw another surface. The result is the next frame's history and what the lighting pass
// upsamples.
//
// The bent normal offsets are view-space vectors: the history's are turned from last frame's
// camera to this one's before blending.

/*[EXTRA_CODE_HERE]*/

out vec4 frag_data[2];

in vec2 vary_fragcoord;

uniform sampler2D fs_indirect_gi;           // this frame: rgb light, a view depth
uniform sampler2D fs_indirect_bent;         // this frame: xyz bend, a view depth
uniform sampler2D fs_indirect_gi_history;   // the last frame's result, same layout
uniform sampler2D fs_indirect_bent_history;
uniform mat4 fs_ind_inv_proj;               // this frame's inverse projection
uniform mat4 fs_ind_to_prev_view;           // this frame's camera space to last frame's
uniform mat4 fs_ind_proj;                   // the projection (last frame's is taken to be the same)
uniform float fs_ind_history;               // weight of the history, 0 when there is none

void main()
{
    ivec2 size = textureSize(fs_indirect_gi, 0);
    ivec2 texel = ivec2(gl_FragCoord.xy);
    vec4 gi_c = texelFetch(fs_indirect_gi, texel, 0);
    vec4 bend_c = texelFetch(fs_indirect_bent, texel, 0);
    float z = gi_c.a;

    if (z <= 0.0)
    {
        frag_data[0] = vec4(0);
        frag_data[1] = vec4(0);
        return;
    }

    // 3x3 average over the same surface, and the range of the neighbourhood's light
    float inv_tol = 1.0 / (0.02 + 0.02 * z);
    vec3 gi_sum = vec3(0);
    vec3 bend_sum = vec3(0);
    float w_sum = 0.0;
    vec3 lo = gi_c.rgb;
    vec3 hi = gi_c.rgb;
    for (int i = 0; i < 9; ++i)
    {
        ivec2 t = clamp(texel + ivec2(i % 3, i / 3) - ivec2(1), ivec2(0), size - ivec2(1));
        vec4 g = texelFetch(fs_indirect_gi, t, 0);
        if (g.a <= 0.0)
        {
            continue;
        }
        float rel = abs(g.a - z) * inv_tol;
        float w = (i == 4 ? 2.0 : 1.0) / (1.0 + rel * rel * 4.0);
        gi_sum += w * g.rgb;
        bend_sum += w * texelFetch(fs_indirect_bent, t, 0).xyz;
        w_sum += w;
        if (rel < 1.0)
        {
            lo = min(lo, g.rgb);
            hi = max(hi, g.rgb);
        }
    }
    vec3 gi = gi_sum / max(w_sum, 1e-4);
    vec3 bend = bend_sum / max(w_sum, 1e-4);

    float w_hist = 0.0;
    vec4 gi_h = vec4(0);
    vec3 bend_h = vec3(0);
    if (fs_ind_history > 0.0)
    {
        // this texel's point, then where it was last frame
        vec2 tc = vary_fragcoord.xy;
        vec4 r = fs_ind_inv_proj * vec4(tc * 2.0 - 1.0, 1.0, 1.0);
        vec3 ray = r.xyz / r.w;
        vec3 pos = ray * (z / max(-ray.z, 1e-6));
        vec4 prev_view = fs_ind_to_prev_view * vec4(pos, 1.0);
        vec4 prev = fs_ind_proj * prev_view;
        if (prev.w > 0.0)
        {
            vec2 prev_tc = prev.xy / prev.w * 0.5 + 0.5;
            if (all(greaterThanEqual(prev_tc, vec2(0.0))) && all(lessThanEqual(prev_tc, vec2(1.0))))
            {
                gi_h = texture(fs_indirect_gi_history, prev_tc);
                bend_h = texture(fs_indirect_bent_history, prev_tc).xyz;
                // the history saw another surface there (disocclusion): start over
                float prev_z = -prev_view.z;
                float rel = abs(gi_h.a - prev_z) / max(prev_z, 0.1);
                w_hist = gi_h.a > 0.0 ? fs_ind_history * (1.0 - smoothstep(0.03, 0.12, rel)) : 0.0;
                // last frame's camera space to this one's: the inverse of the rotation above
                bend_h = transpose(mat3(fs_ind_to_prev_view)) * bend_h;
            }
        }
    }

    // the neighbourhood range widened a little: with this much noise a strict clamp throws away
    // most of the history
    vec3 margin = (hi - lo) * 0.25 + vec3(0.002);
    vec3 gi_hist = clamp(gi_h.rgb, lo - margin, hi + margin);

    frag_data[0] = vec4(max(mix(gi, gi_hist, w_hist), vec3(0.0)), z);
    frag_data[1] = vec4(mix(bend, bend_h, w_hist), z);
}
