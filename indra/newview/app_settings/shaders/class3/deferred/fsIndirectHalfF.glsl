/**
 * @file fsIndirectHalfF.glsl
 * @brief Bent normals and screen-space indirect light at half resolution (FSRenderBentNormals, FSRenderSSGI)
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

// Runs before the lighting pass, one texel per 2x2 pixels (texel t evaluates full-resolution
// pixel 2t). Walks the depth buffer the way GTAO does (fsGTAOF.glsl): a few slices around the view
// vector, and on both sides of the pixel the highest horizon seen within a radius.
//
// Bent normal: the middle of the open arc between the two horizons is the direction this pixel
// sees the most sky in that slice. Its offset from the unoccluded middle (the normal's own
// direction in the slice), averaged over the slices, is how far the ambient light lookup should
// lean. In a crease or under an arm it leans out of the crease, so the probe's light from inside
// the wall stops leaking in. Stored as an offset, so the lighting pass can add it to the shading
// normal and keep the normal map.
//
// Indirect light (FS_SSGI): every sample that raises a horizon hides a part of the arc that the
// ambient light would have filled, and puts that sample's surface there instead. That surface's
// colour in the last lit frame (sceneMap) is the light it sends this way, if it faces the pixel.
// Weighted by the cosine-weighted share of the arc it covers (the same integral GTAO uses for the
// visibility), the sum is the light that bounces in, in the same units as the ambient irradiance.
// Because the last frame already holds the last bounce, light keeps bouncing over the frames.
//
// Noisy by design (two slices, jittered every frame); fsIndirectTemporalF.glsl blends the frames.
//
// Output 0: rgb indirect light, a positive view depth (0 for sky)
// Output 1: xyz bent normal offset in view space, a positive view depth

/*[EXTRA_CODE_HERE]*/

out vec4 frag_data[2];

uniform vec2 fs_ind_full_res;   // full resolution in pixels
uniform mat4 inv_proj;
uniform sampler2D sceneMap;     // the last frame, lit
uniform vec4 fs_ind_params;     // x: radius (m), y: max radius in full-resolution pixels, z: frame offset 0-1, w: unused

float getDepth(vec2 pos_screen);
bool isFarDepth(float d);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);
vec4 getNorm(vec2 pos_screen);
vec3 getGeoNorm(vec2 pos_screen);

#ifndef FS_IND_SLICES
#define FS_IND_SLICES 2
#endif
#define FS_IND_STEPS 8

#define FS_IND_PI      3.14159265
#define FS_IND_HALF_PI 1.57079633

float fsIndAcos(float x)
{
    float ax = abs(x);
    float r = (-0.156583 * ax + FS_IND_HALF_PI) * sqrt(max(1.0 - ax, 0.0));
    return x >= 0.0 ? r : FS_IND_PI - r;
}

// The cosine-weighted visible arc from the view vector to horizon angle h on one side of a
// slice (Jimenez 2016, eq. 10, one side); the difference between two horizons is the share of
// the hemisphere between them
float fsIndArc(float h, float n_ang, float cos_n, float sin_n)
{
    return 0.25 * (cos_n + 2.0 * h * sin_n - cos(2.0 * h - n_ang));
}

float fsIndNoise(vec2 frag)
{
    return fract(52.9829189 * fract(0.06711056 * frag.x + 0.00583715 * frag.y));
}

void main()
{
    ivec2 full = min(ivec2(gl_FragCoord.xy) * 2, ivec2(fs_ind_full_res) - 1);
    vec2 tc = (vec2(full) + 0.5) / fs_ind_full_res;

    float depth = getDepth(tc);
    if (isFarDepth(depth))
    {
        frag_data[0] = vec4(0);
        frag_data[1] = vec4(0);
        return;
    }

    vec3 pos = getPositionWithDepth(tc, depth).xyz;
    float view_depth = max(-pos.z, 1e-4);
    vec3 v = normalize(-pos);
    vec3 n = normalize(getGeoNorm(tc));
    if (dot(n, v) < 0.0)
    {
        n = -n;
    }

    // World radius to screen (inv_proj holds 1/P00 and 1/P11), capped in pixels
    vec2 radius_tc = fs_ind_params.x * 0.5 / (view_depth * vec2(inv_proj[0][0], inv_proj[1][1]));
    float radius_px = radius_tc.y * fs_ind_full_res.y;
    float clamp_scale = min(max(fs_ind_params.y, 1.0) / max(radius_px, 1e-4), 1.0);
    radius_tc *= clamp_scale;
    radius_px *= clamp_scale;
    if (radius_px < 2.0)
    {
        frag_data[0] = vec4(0.0, 0.0, 0.0, view_depth);
        frag_data[1] = vec4(0.0, 0.0, 0.0, view_depth);
        return;
    }
    float radius = fs_ind_params.x * clamp_scale;

    // A different rotation and step offset every frame (golden ratio), so the frames blended
    // later cover different directions and distances
    vec2 frag_px = gl_FragCoord.xy;
    float slice_noise = fract(fsIndNoise(frag_px) + fs_ind_params.z);
    float step_noise = fract(fsIndNoise(frag_px.yx + 17.0) + fs_ind_params.z * 1.618034);
    float min_s = 2.0 / radius_px;

    vec3 bend = vec3(0.0);
    vec3 gi = vec3(0.0);

    for (int slice = 0; slice < FS_IND_SLICES; ++slice)
    {
        float phi = (float(slice) + slice_noise) * (FS_IND_PI / float(FS_IND_SLICES));
        vec2 omega = vec2(cos(phi), sin(phi));

        vec3 dir = vec3(omega, 0.0);
        vec3 ortho_dir = normalize(dir - dot(dir, v) * v);
        vec3 axis = normalize(cross(ortho_dir, v));
        vec3 proj_n = n - axis * dot(n, axis);
        float proj_n_len = length(proj_n);

        float sign_n = sign(dot(ortho_dir, proj_n));
        float cos_n = clamp(dot(proj_n, v) / max(proj_n_len, 1e-4), 0.0, 1.0);
        float n_ang = sign_n * fsIndAcos(cos_n);
        float sin_n = sin(n_ang);

        // side 0 runs along +omega (positive angles), side 1 along -omega; each starts at the
        // tangent plane
        float h_side[2];
        h_side[0] = n_ang + FS_IND_HALF_PI;
        h_side[1] = n_ang - FS_IND_HALF_PI;

        for (int side = 0; side < 2; ++side)
        {
            float sgn = side == 0 ? 1.0 : -1.0;
            float h = h_side[side];
            for (int i = 0; i < FS_IND_STEPS; ++i)
            {
                float s = (float(i) + step_noise) / float(FS_IND_STEPS);
                s = s * s + min_s;
                vec2 stc = tc + sgn * omega * radius_tc * s;
                if (any(lessThan(stc, vec2(0.0))) || any(greaterThan(stc, vec2(1.0))))
                {
                    break;
                }
                float sd = getDepth(stc);
                if (isFarDepth(sd))
                {
                    continue;
                }
                vec3 delta = getPositionWithDepth(stc, sd).xyz - pos;
                float dist = length(delta);
                if (dist < 1e-4 || dist > radius)
                {
                    continue;
                }
                // the sample's angle from the view vector within the slice, clamped to the
                // normal's hemisphere
                float sample_h = sgn * fsIndAcos(clamp(dot(delta, v) / dist, -1.0, 1.0));
                sample_h = n_ang + clamp(sample_h - n_ang, -FS_IND_HALF_PI, FS_IND_HALF_PI);
                bool raises = side == 0 ? sample_h < h : sample_h > h;
                if (!raises)
                {
                    continue;
                }
#ifdef FS_SSGI
                // the share of the arc this sample now hides, and the light its surface sends
                // back toward the pixel
                float share = abs(fsIndArc(h, n_ang, cos_n, sin_n) - fsIndArc(sample_h, n_ang, cos_n, sin_n));
                vec3 ldir = -delta / dist;
                float facing = clamp(dot(getNorm(stc).xyz, ldir), 0.0, 1.0);
                // fades out toward the edge of the radius, so the bounce has no hard edge there
                float falloff = 1.0 - smoothstep(0.6 * radius, radius, dist);
                vec3 light = textureLod(sceneMap, stc, 0.0).rgb;
                // a single very bright sample (a lamp, the sun's glint) would speckle the result
                float lum = dot(light, vec3(0.2126, 0.7152, 0.0722));
                light *= min(1.0, 8.0 / max(lum, 1e-4));
                gi += light * (share * facing * falloff * proj_n_len);
#endif
                h = sample_h;
            }
            h_side[side] = h;
        }

        // the middle of the open arc against the middle of the unoccluded one (the normal)
        float h_mid = 0.5 * (h_side[0] + h_side[1]);
        vec3 bent_dir = v * cos(h_mid) + ortho_dir * sin(h_mid);
        vec3 normal_dir = v * cos(n_ang) + ortho_dir * sin(n_ang);
        bend += (bent_dir - normal_dir) * proj_n_len;
    }

    // Both sides of each slice already add up to the whole hemisphere's share, so the slices
    // average
    gi /= float(FS_IND_SLICES);
    bend /= float(FS_IND_SLICES);

    frag_data[0] = vec4(max(gi, vec3(0.0)), view_depth);
    frag_data[1] = vec4(bend, view_depth);
}
