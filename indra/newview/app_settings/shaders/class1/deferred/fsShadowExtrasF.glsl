/**
 * @file class1/deferred/fsShadowExtrasF.glsl
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

// Extra terms of the deferred sun shadow, multiplied into the sun channel of the light map by
// sunLightF / sunLightSSAOF (fsSunShadowExtras). Built into the two deferred sun programs as one
// of their own files, so the program's permutation defines reach it:
//   FS_CLOUD_SHADOWS    the sky's cloud layer, projected along the light onto each pixel
// The effect is disabled in reflection probe captures (cube_snapshot).

uniform int cube_snapshot;
uniform vec3 sun_dir;   // view space
uniform vec3 moon_dir;  // view space
uniform int sun_up_factor;

#if defined(FS_CLOUD_SHADOWS)
// The cloud layer of cloudsF.glsl, anchored to the world instead of the camera's sky dome: the
// dome's planar uv of a direction d is about ((d.east, -d.north) / 2) / cloud_scale, so a layer at
// height H gives each metre on it 1 / (2 H cloud_scale) of uv. Same noise textures, scroll,
// scale, coverage (cloud_shadow), densities and variance as the sky.
uniform sampler2D cloud_noise_texture;
uniform sampler2D cloud_noise_texture_next;

uniform mat4 fs_cloud_view_to_world;  // inverse modelview: view space to agent space (x east, y north, z up)
uniform vec4 fs_cloud_map;            // xy: uv1 under the camera (wrapped), z: uv per metre, w: layer height above the camera (m)
uniform vec3 fs_cloud_pos_density1;   // cloud_pos_density1 with the scroll, as the sky gets it
uniform vec3 fs_cloud_pos_density2;   // cloud_pos_density2
uniform vec4 fs_cloud_params;         // x: cloud_scale, y: cloud_variance, z: cloud_shadow (coverage), w: blend_factor
uniform vec2 fs_cloud_light_offset;   // lightnorm.xz * 0.0125: the sky's self shadow offset
uniform vec2 fs_cloud_shadow;         // x: strength 0-1 (0: off), y: softness 0-1

// Explicit LOD: the uv comes from the world position, so implicit derivatives jump across
// depth edges and pick a wrong mip in a line along every silhouette.
vec4 fsCloudNoise(vec2 uv, float lod)
{
    // blend_factor is uniform: endpoint skies need only one lookup
    if (fs_cloud_params.w <= 0.0)
    {
        return textureLod(cloud_noise_texture, uv, lod);
    }
    if (fs_cloud_params.w >= 1.0)
    {
        return textureLod(cloud_noise_texture_next, uv, lod);
    }
    return mix(textureLod(cloud_noise_texture, uv, lod), textureLod(cloud_noise_texture_next, uv, lod), fs_cloud_params.w);
}

// fraction (0..1) of the light the cloud layer lets through to view space position pos
float fsCloudShadow(vec3 pos, vec3 light_dir)
{
    float strength = fs_cloud_shadow.x;
    float cloud_scale = fs_cloud_params.x;
    if (strength <= 0.0 || cloud_scale < 0.001)
    {
        return 1.0;
    }

    mat3 rot = mat3(fs_cloud_view_to_world);
    vec3 p = rot * pos;                           // metres from the camera
    vec3 l = rot * light_dir;
    // a light near the horizon would smear the layer across the land: fade the shadows out
    float horizon = smoothstep(0.05, 0.25, l.z);
    if (horizon <= 0.0)
    {
        return 1.0;
    }

    // along the light to the layer
    vec2 q = p.xy + l.xy * ((fs_cloud_map.w - p.z) / max(l.z, 0.05));

    float softness = clamp(fs_cloud_shadow.y, 0.0, 1.0);
    // Mip of the base (uv1) frequency: a pixel's footprint, taken as about 1 mrad of the
    // distance (smooth in depth, unlike derivatives), in noise texels, plus the softness bias.
    // The taps below add log2 of their own frequency to it.
    float texels_per_metre = fs_cloud_map.z * float(textureSize(cloud_noise_texture, 0).x);
    float lod = log2(max(length(p) * 0.001 * texels_per_metre, 1.0)) + softness * 3.0;

    // the uv sets of cloudsV.glsl (before the scroll)
    vec2 uv1 = fs_cloud_map.xy + vec2(q.x, -q.y) * fs_cloud_map.z;
    vec2 uv2 = uv1 + fs_cloud_light_offset;
    vec2 uv3 = uv1 * 16.0;
    vec2 uv4 = uv2 * 16.0;

    // the density of cloudsF.glsl
    float cloud_variance = fs_cloud_params.y;
    float cloudDensity = 2.0 * (fs_cloud_params.z - 0.25);

    vec2 disturbance  = vec2(fsCloudNoise(uv1 / 8.0, lod - 3.0).x, fsCloudNoise((uv3 + uv1) / 16.0, lod).x) * cloud_variance * (1.0 - cloud_scale * 0.25);
    vec2 disturbance2 = vec2(fsCloudNoise((uv1 + uv3) / 4.0, lod + 2.0).x, fsCloudNoise((uv4 + uv2) / 8.0, lod + 1.0).x) * cloud_variance * (1.0 - cloud_scale * 0.25);

    uv1 += fs_cloud_pos_density1.xy + (disturbance * 0.2);
    uv3 += fs_cloud_pos_density2.xy;

    float density_variance = min(1.0, (disturbance.x * 2.0 + disturbance.y * 2.0 + disturbance2.x + disturbance2.y) * 4.0);
    cloudDensity *= 1.0 - (density_variance * density_variance);

    float alpha1 = (fsCloudNoise(uv1, lod).x - 0.5) + (fsCloudNoise(uv3, lod + 4.0).x - 0.5) * fs_cloud_pos_density2.z;
    // the sky's gain of 10 gives hard edged clouds; softness lowers it
    alpha1 = min(max(alpha1 + cloudDensity, 0.0) * mix(10.0, 3.0, softness) * fs_cloud_pos_density1.z, 1.0);
    alpha1 = 1.0 - alpha1 * alpha1;
    alpha1 = 1.0 - alpha1 * alpha1;
    alpha1 = clamp(alpha1, 0.0, 1.0);

    return 1.0 - clamp(strength, 0.0, 1.0) * horizon * alpha1;
}
#endif // FS_CLOUD_SHADOWS


// sun shadow (light map red channel) of the pixel at view space position pos with normal norm
float fsSunShadowExtras(float shadow, vec3 pos, vec3 norm)
{
    if (cube_snapshot != 0)
    {
        return shadow; // probes keep the plain shadow: too rare for moving clouds, too coarse for contact
    }
#if defined(FS_CLOUD_SHADOWS)
    if (shadow > 0.0)
    {
        vec3 light_dir = normalize((sun_up_factor == 1) ? sun_dir : moon_dir);
        shadow *= fsCloudShadow(pos, light_dir);
    }
#endif
    return shadow;
}
