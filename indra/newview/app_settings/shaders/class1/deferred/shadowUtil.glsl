/**
 * @file class1/deferred/shadowUtil.glsl
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

uniform sampler2D   normalMap;

#if defined(SUN_SHADOW)
uniform sampler2DShadow shadowMap0;
uniform sampler2DShadow shadowMap1;
uniform sampler2DShadow shadowMap2;
uniform sampler2DShadow shadowMap3;
#endif

#if defined(SPOT_SHADOW)
uniform sampler2DShadow shadowMap4;
uniform sampler2DShadow shadowMap5;
#endif

uniform int cube_snapshot;

uniform vec3 sun_dir;
uniform vec3 moon_dir;
uniform vec2 shadow_res;
uniform vec2 proj_shadow_res;
uniform mat4 shadow_matrix[6];
uniform vec4 shadow_clip;
uniform float shadow_bias;
uniform float shadow_offset;
uniform float spot_shadow_bias;
uniform float spot_shadow_offset;
uniform float spot_shadow_reach; // metres: projector shadows fade out towards this camera distance
uniform mat4 inv_proj;
uniform vec2 screen_res;
uniform int sun_up_factor;

// Receiver-plane depth bias of the sun PCF: the depth slope of the receiver in shadow texture space
// (d depth / d uv), from two 5 cm tangent steps of the view space position pos (normal norm) through
// the cascade's matrix m; c is pos projected and divided. Each tap's reference depth is lowered by
// the slope times the tap's distance from the receiver point, so a slanted lit surface doesn't shadow
// itself between the taps (acne on walls under a low sun). Replaces the altitude term of the bias
// (RenderShadowBiasError), which only widened a constant bias with height and detached shadows from
// their casters in skyboxes. From Vulkanstorm, by Anne Skydancer.
#define PCF_RPDB_STEP 0.05      // metres
#define PCF_RPDB_MAX  0.004     // cap per tap, shadow depth units
vec2 pcfDepthGradient(mat4 m, vec3 pos, vec3 norm, vec3 c)
{
    if (dot(norm, norm) < 1.0e-8)
    {
        return vec2(0.0);
    }
    vec3 n = normalize(norm);
    vec3 t1 = normalize(cross(n, abs(n.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0)));
    vec3 t2 = cross(n, t1);
    vec4 a = m * vec4(pos + t1 * PCF_RPDB_STEP, 1.0);
    vec4 b = m * vec4(pos + t2 * PCF_RPDB_STEP, 1.0);
    vec3 d1 = a.xyz / a.w - c;
    vec3 d2 = b.xyz / b.w - c;
    float det = d1.x * d2.y - d1.y * d2.x;
    if (abs(det) < 1.0e-14)
    {
        return vec2(1.0e4);
    }
    return abs(vec2(d1.z * d2.y - d1.y * d2.z, d1.x * d2.z - d1.z * d2.x) / det);
}

float pcfShadow(sampler2DShadow shadowMap, vec3 norm, vec4 stc, float bias_mul, vec2 pos_screen, vec3 light_dir, mat4 m, vec3 pos)
{
#if defined(SUN_SHADOW)
    float offset = shadow_bias * bias_mul;
    stc.xyz /= stc.w;
    if (cube_snapshot != 0)
    {
        return texture(shadowMap, stc.xyz + vec3(0.0, 0.0, offset * 2.0));
    }
    vec2 grad = pcfDepthGradient(m, pos, norm, stc.xyz);
    stc.z += offset * 2.0;
    // Each map's own size: the far cascades can be smaller than the near ones
    vec2 res = vec2(textureSize(shadowMap, 0));
    vec2 texel = 1.0 / res;
    float x0 = stc.x;
    stc.x = floor(stc.x*res.x + fract(pos_screen.y*res.y))/res.x; // add some chaotic jitter to X sample pos according to Y to disguise the snapping going on here
    // Per-tap receiver-plane bias: the snap's own shift plus the tap's offset and half a texel of
    // bilinear footprint, in uv
    vec2 snap = vec2(abs(stc.x - x0), 0.0) + 0.5 * texel;
    float cs = texture(shadowMap, vec3(stc.xy, stc.z - min(dot(grad, snap), PCF_RPDB_MAX)));
    float shadow = cs * 4.0;
    float a = texture(shadowMap, vec3(stc.xy + vec2( 1.5,  0.5) * texel, stc.z - min(dot(grad, snap + vec2(1.5, 0.5) * texel), PCF_RPDB_MAX)));
    float b = texture(shadowMap, vec3(stc.xy + vec2(-1.5, -0.5) * texel, stc.z - min(dot(grad, snap + vec2(1.5, 0.5) * texel), PCF_RPDB_MAX)));
#ifdef FS_SHADOW_FAST_PCF
    // Two opposite taps agree with the centre and it is fully lit or fully shadowed: the other
    // two taps would agree too, so skip them. Only shadow edges pay for all five.
    // The PCSS build of the sun program (PCSS_SHADOW) never calls pcfShadow, so this has no
    // effect there: its contact hardening path has its own early-out (no blocker found: one
    // comparison tap), and a wide penumbra can't be judged from three taps 1.5 texels apart.
    if ((cs < 0.001 || cs > 0.999) && abs(a - cs) + abs(b - cs) < 0.002)
    {
        return cs;
    }
#endif
    shadow += a + b;
    shadow += texture(shadowMap, vec3(stc.xy + vec2( 0.5, -1.5) * texel, stc.z - min(dot(grad, snap + vec2(0.5, 1.5) * texel), PCF_RPDB_MAX)));
    shadow += texture(shadowMap, vec3(stc.xy + vec2(-0.5,  1.5) * texel, stc.z - min(dot(grad, snap + vec2(0.5, 1.5) * texel), PCF_RPDB_MAX)));
    return clamp(shadow * 0.125, 0.0, 1.0);
#else
    return 1.0;
#endif
}

float pcfSpotShadow(sampler2DShadow shadowMap, vec4 stc, float bias_scale, vec2 pos_screen)
{
#if defined(SPOT_SHADOW)
    if (stc.w <= 0.0)
    {
        return 1.0; // behind the projector: the divide would mirror the map
    }
    stc.xyz /= stc.w;
    stc.z += spot_shadow_bias * bias_scale;
    if (cube_snapshot != 0)
    {
        return texture(shadowMap, stc.xyz);
    }
    stc.x = floor(proj_shadow_res.x * stc.x + fract(pos_screen.y*0.666666666)) / proj_shadow_res.x; // snap

    float cs = texture(shadowMap, stc.xyz);
    float shadow = cs;

    vec2 off = 1.0/proj_shadow_res;
    off.y *= 1.5;

    shadow += texture(shadowMap, stc.xyz+vec3(off.x*2.0, off.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3(off.x, -off.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3(-off.x, off.y, 0.0));
    shadow += texture(shadowMap, stc.xyz+vec3(-off.x*2.0, -off.y, 0.0));
    return shadow*0.2;
#else
    return 1.0;
#endif
}

#if defined(PCSS_SHADOW)
// <Vulkanstorm> PCSS stage 1: rotated Poisson PCF with a receiver-plane depth bias.
// Compiled only into the deferred sun program (gDeferredSunProgram) when RenderShadowPCSS is on;
// every other consumer of this file gets the PCF above, unchanged. Licensed LGPL-2.1 or GPL-3.

// angular size of the shadowing light, L = 2 tan(half angle) (physical sun times the sky's
// sun_scale times RenderShadowPCSSLightSize); the penumbra width uses it from stage 2 on
uniform float shadow_light_size;

#define PCSS_PCF_TAPS   12
#define PCSS_PCF_RADIUS 1.5     // kernel radius in shadow map texels
#define PCSS_PLANE_STEP 0.05    // metres: tangent step that measures the receiver plane
#define PCSS_RPDB_MAX   0.004   // cap of the receiver-plane bias, shadow depth units
#define PCSS_GRAD_MAX   1.0e4   // depth gradient used where the plane is edge-on to the light

// Best-candidate Poisson disk, mean at the centre, farthest point on the unit circle:
// gen_poisson_disk.py 12 (a Vulkanstorm tool, not part of this tree)
const vec2 pcss_poisson12[12] = vec2[12](
    vec2(-0.789262,  0.127632),
    vec2( 0.406628, -0.132460),
    vec2(-0.032561,  0.702528),
    vec2(-0.350013, -0.648809),
    vec2( 0.580484,  0.508811),
    vec2(-0.653347,  0.747330),
    vec2( 0.265165, -0.741572),
    vec2(-0.182968,  0.036783),
    vec2(-0.897737, -0.440532),
    vec2( 0.819877,  0.077022),
    vec2( 0.186699,  0.290105),
    vec2( 0.647037, -0.526838));

// interleaved gradient noise (Jimenez 2014) of the pixel: the kernel's rotation
float pcssNoise(vec2 frag)
{
    return fract(52.9829189 * fract(0.06711056 * frag.x + 0.00583715 * frag.y));
}

// Receiver plane in shadow texture space: d(depth)/d(uv) of the surface (view space position
// pos, normal norm) under the projective shadow matrix m, from two tangent steps. c is the
// pixel's own projected and divided position.
vec2 pcssDepthGradient(mat4 m, vec3 pos, vec3 norm, vec3 c)
{
    if (dot(norm, norm) < 1.0e-8)
    {
        return vec2(0.0); // no normal (sky): constant bias only
    }
    vec3 n = normalize(norm);
    vec3 t1 = normalize(cross(n, abs(n.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0)));
    vec3 t2 = cross(n, t1);
    vec4 a = m * vec4(pos + t1 * PCSS_PLANE_STEP, 1.0);
    vec4 b = m * vec4(pos + t2 * PCSS_PLANE_STEP, 1.0);
    vec3 d1 = a.xyz / a.w - c;
    vec3 d2 = b.xyz / b.w - c;
    float det = d1.x * d2.y - d1.y * d2.x;
    if (abs(det) < 1.0e-14)
    {
        return vec2(PCSS_GRAD_MAX);
    }
    vec2 g = vec2(d1.z * d2.y - d1.y * d2.z, d1.x * d2.z - d1.z * d2.x) / det;
    return clamp(g, vec2(-PCSS_GRAD_MAX), vec2(PCSS_GRAD_MAX));
}

// Fraction (0..1) of the kernel radius that keeps every tap, with its bilinear footprint,
// inside the map: the kernel shrinks towards the map's edges (the cascade blend hides it).
float pcssEdgeScale(vec2 uv, vec2 extent, vec2 texel)
{
    vec2 room = min(uv, vec2(1.0) - uv) - 0.5 * texel;
    return clamp(min(room.x / extent.x, room.y / extent.y), 0.0, 1.0);
}

// the kernel's per pixel rotation (cos, sin)
vec2 pcssRotation()
{
    float angle = 6.28318531 * pcssNoise(gl_FragCoord.xy);
    return vec2(cos(angle), sin(angle));
}

// 12 rotated Poisson taps through the compare sampler (each tap a hardware 2x2 PCF) over an
// ellipse of half axes extent (uv). c: divided position with the constant bias already added
// to its depth; grad: |receiver plane gradient|.
float pcssPCF(sampler2DShadow shadowMap, vec3 c, vec2 grad, vec2 extent, vec2 rot)
{
    float shadow = 0.0;
    for (int i = 0; i < PCSS_PCF_TAPS; ++i)
    {
        vec2 p = pcss_poisson12[i];
        vec2 off = vec2(rot.x * p.x - rot.y * p.y, rot.y * p.x + rot.x * p.y) * extent;
        float rpdb = min(dot(grad, abs(off)), PCSS_RPDB_MAX);
        shadow += texture(shadowMap, vec3(c.xy + off, c.z - rpdb));
    }
    return shadow * (1.0 / float(PCSS_PCF_TAPS));
}

// Fixed radius rotated PCF (stage 1): cascades 2-3, and everything without PCSS_SHADOW_FETCH.
// stc: projected, not yet divided; z_bias: the constant bias added to the reference depth.
float pcssPoissonPCF(sampler2DShadow shadowMap, mat4 m, vec4 stc, vec3 pos, vec3 norm, float z_bias, vec2 res)
{
    if (stc.w <= 0.0)
    {
        return 1.0; // behind the projector: the divide would mirror the map
    }
    stc.xyz /= stc.w;
    vec2 grad = abs(pcssDepthGradient(m, pos, norm, stc.xyz));
    stc.z += z_bias;

    vec2 texel = 1.0 / res;
    vec2 extent = PCSS_PCF_RADIUS * texel;
    extent *= pcssEdgeScale(stc.xy, extent, texel);

    return pcssPCF(shadowMap, stc.xyz, grad, extent, pcssRotation());
}

#if defined(PCSS_SHADOW_FETCH)
// Stage 2, contact hardening (sun cascades 0-1 and the spot shadows): a blocker search through a
// plain depth read of the same map (shadowDepthN, bound with a GL 3.3 sampler object that has
// compare mode off), then a penumbra from the blockers' distance and the light's size, then the
// rotated PCF above with that radius.
#if defined(SUN_SHADOW)
uniform sampler2D shadowDepth0;
uniform sampler2D shadowDepth1;
#endif
#if defined(SPOT_SHADOW)
uniform sampler2D shadowDepth4;
uniform sampler2D shadowDepth5;
#endif

#define PCSS_BLOCKER_DIST0     4.0   // metres: farthest blocker that softens cascade 0
#define PCSS_BLOCKER_DIST1     8.0   // metres: the same for cascade 1
#define PCSS_BLOCKER_DIST_SPOT 4.0   // metres: the same for the spot shadows
#define PCSS_SPOT_LIGHT_SIZE   0.15  // metres: width of a projector's emitter
#define PCSS_PCF_RADIUS_MIN    1.0   // texels: narrowest PCF (contact)
#define PCSS_PCF_RADIUS_MAX    12.0  // texels at shadow resolution scale 1: widest PCF and search
#define PCSS_SEARCH_RADIUS_MIN 2.0   // texels: narrowest blocker search
#define PCSS_SCALE_STEP        0.25  // metres: step that measures the map's scale at the pixel

// texture space position (uv, depth) of a view space point
vec3 pcssProject(mat4 m, vec3 p)
{
    vec4 s = m * vec4(p, 1.0);
    return s.xyz / s.w;
}

// uv per metre, per map axis, across the light ray dir at pos (c: pos projected): the bounding
// box of the image of a unit disc perpendicular to the ray, so right for warped and perspective maps
vec2 pcssUVScale(mat4 m, vec3 pos, vec3 c, vec3 dir)
{
    vec3 t1 = normalize(cross(dir, abs(dir.x) < 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0)));
    vec3 t2 = cross(dir, t1);
    vec2 a = (pcssProject(m, pos + t1 * PCSS_SCALE_STEP).xy - c.xy) / PCSS_SCALE_STEP;
    vec2 b = (pcssProject(m, pos + t2 * PCSS_SCALE_STEP).xy - c.xy) / PCSS_SCALE_STEP;
    return vec2(length(vec2(a.x, b.x)), length(vec2(a.y, b.y)));
}

// the map's resolution scale (RenderShadowResolutionScale): its width over the screen's
float pcssResScale(vec2 res)
{
    return max(res.x / screen_res.x, 1.0);
}

vec2 pcssClampExtent(vec2 extent, vec2 texel, float lo, float hi)
{
    return clamp(extent, texel * lo, texel * hi);
}

// mean raw depth of the blockers within the search ellipse (half axes extent, uv), -1 if none:
// texels nearer the light than the receiver plane at the tap, less the bias
float pcssBlockerDepth(sampler2D depthMap, vec3 c, float z_bias, vec2 grad, vec2 extent, vec2 rot)
{
    // the centre first: the disk has no tap there, and a thin caster right under the
    // receiver would otherwise slip between the taps of a wide search and read as lit
    float sum = 0.0;
    float count = 0.0;
    float centre = texture(depthMap, c.xy).r;
    if (centre < c.z + z_bias)
    {
        sum += centre;
        count += 1.0;
    }
    for (int i = 0; i < PCSS_PCF_TAPS; ++i)
    {
        vec2 p = pcss_poisson12[i];
        vec2 off = vec2(rot.x * p.x - rot.y * p.y, rot.y * p.x + rot.x * p.y) * extent;
        float d = texture(depthMap, c.xy + off).r;
        if (d < c.z + z_bias - min(dot(grad, abs(off)), PCSS_RPDB_MAX))
        {
            sum += d;
            count += 1.0;
        }
    }
    return count > 0.0 ? sum / count : -1.0;
}

// Sun cascade with contact hardening. light_dir: towards the light; d_max: farthest blocker.
// Penumbra (full width, metres on the receiver) = blocker distance * light size; the PCF disc
// takes that width as its diameter, which gives a disc light's edge profile.
float pcssSunContactShadow(sampler2DShadow shadowMap, sampler2D depthMap, mat4 m, vec4 stc, vec3 pos, vec3 norm, vec3 light_dir, float d_max)
{
    vec3 c = stc.xyz / stc.w;
    vec2 grad = abs(pcssDepthGradient(m, pos, norm, c));
    float z_bias = shadow_bias * 2.0;
    vec2 texel = 1.0 / shadow_res;
    float r_max = PCSS_PCF_RADIUS_MAX * pcssResScale(shadow_res);
    vec2 rot = pcssRotation();

    vec2 s_uv = pcssUVScale(m, pos, c, light_dir);
    float s_z = abs(pcssProject(m, pos + light_dir * PCSS_SCALE_STEP).z - c.z) / PCSS_SCALE_STEP;

    vec2 search = pcssClampExtent(s_uv * shadow_light_size * d_max, texel, PCSS_SEARCH_RADIUS_MIN, r_max);
    search *= pcssEdgeScale(c.xy, search, texel);
    float z_blocker = pcssBlockerDepth(depthMap, c, z_bias, grad, search, rot);
    if (z_blocker < 0.0)
    {
        // no blocker in the search: one comparison tap at the centre still catches a caster
        // thinner than the search taps' spacing inside the bilinear footprint
        return texture(shadowMap, vec3(c.xy, c.z + z_bias));
    }

    float w_world = max(c.z - z_blocker, 0.0) / max(s_z, 1.0e-8) * shadow_light_size;
    vec2 extent = pcssClampExtent(0.5 * w_world * s_uv, texel, PCSS_PCF_RADIUS_MIN, r_max);
    extent *= pcssEdgeScale(c.xy, extent, texel);
    return pcssPCF(shadowMap, vec3(c.xy, c.z + z_bias), grad, extent, rot);
}

// a projector's position in view space: the point its matrix maps to x = y = w = 0
vec3 pcssProjectorEye(mat4 m)
{
    vec4 r0 = vec4(m[0][0], m[1][0], m[2][0], m[3][0]);
    vec4 r1 = vec4(m[0][1], m[1][1], m[2][1], m[3][1]);
    vec4 r3 = vec4(m[0][3], m[1][3], m[2][3], m[3][3]);
    mat3 rows_t = mat3(r0.xyz, r1.xyz, r3.xyz); // its columns are the rows
    return -vec3(r0.w, r1.w, r3.w) * inverse(rows_t);
}

// Spot shadow with contact hardening, the textbook perspective form: an emitter of width
// PCSS_SPOT_LIGHT_SIZE at the projector, blockers at linear distance d_b, receiver at d_r:
// penumbra (full width, metres on the receiver) = PCSS_SPOT_LIGHT_SIZE * (d_r - d_b) / d_b.
float pcssSpotContactShadow(sampler2DShadow shadowMap, sampler2D depthMap, mat4 m, vec4 stc, vec3 pos, vec3 norm, float bias_scale)
{
    if (stc.w <= 0.0)
    {
        return 1.0; // behind the projector: the divide would mirror the map, clamp(d_b) inverts
    }
    vec3 c = stc.xyz / stc.w;
    float d_r = stc.w;                       // linear distance along the projector's axis
    vec2 grad = abs(pcssDepthGradient(m, pos, norm, c));
    float z_bias = spot_shadow_bias * bias_scale;
    vec2 texel = 1.0 / proj_shadow_res;
    float r_max = PCSS_PCF_RADIUS_MAX * pcssResScale(proj_shadow_res);
    vec2 rot = pcssRotation();

    vec3 eye = pcssProjectorEye(m);
    vec2 s_uv = pcssUVScale(m, pos, c, normalize(eye - pos));
    // texture z (before the divide) at the projector itself: linearises a raw depth below
    float z_eye = m[0][2] * eye.x + m[1][2] * eye.y + m[2][2] * eye.z + m[3][2];

    float d_near = max(d_r - PCSS_BLOCKER_DIST_SPOT, 0.25 * d_r);
    vec2 search = pcssClampExtent(s_uv * PCSS_SPOT_LIGHT_SIZE * (d_r - d_near) / d_near, texel, PCSS_SEARCH_RADIUS_MIN, r_max);
    search *= pcssEdgeScale(c.xy, search, texel);
    float z_blocker = pcssBlockerDepth(depthMap, c, z_bias, grad, search, rot);
    if (z_blocker < 0.0)
    {
        // no blocker in the search: one comparison tap at the centre still catches a caster
        // thinner than the search taps' spacing inside the bilinear footprint
        return texture(shadowMap, vec3(c.xy, c.z + z_bias));
    }

    // along the ray eye + s (pos - eye): w = s d_r and z = z_eye + s (stc.z - z_eye); z / w = z_blocker
    // (z_eye and the denominator share their sign: negative for a GL style projection)
    float denom = z_blocker * d_r - stc.z + z_eye;
    float d_b = abs(denom) > 1.0e-7 ? z_eye * d_r / denom : d_r;
    d_b = clamp(d_b, 1.0e-3, d_r);
    float w_world = PCSS_SPOT_LIGHT_SIZE * (d_r - d_b) / d_b;
    vec2 extent = pcssClampExtent(0.5 * w_world * s_uv, texel, PCSS_PCF_RADIUS_MIN, r_max);
    extent *= pcssEdgeScale(c.xy, extent, texel);
    return pcssPCF(shadowMap, vec3(c.xy, c.z + z_bias), grad, extent, rot);
}
#endif // PCSS_SHADOW_FETCH

// sun cascade index (0-3): the constant bias as pcfShadow applies it, and the map's own size
// (the far cascades can be smaller than the near ones)
float pcssShadow(sampler2DShadow shadowMap, int index, vec4 stc, vec3 pos, vec3 norm)
{
    return pcssPoissonPCF(shadowMap, shadow_matrix[index], stc, pos, norm, shadow_bias * 2.0, vec2(textureSize(shadowMap, 0)));
}

// spot shadow index (4-5): the constant bias as pcfSpotShadow applies it
float pcssSpotShadow(sampler2DShadow shadowMap, int index, vec4 stc, vec3 pos, vec3 norm, float bias_scale)
{
    return pcssPoissonPCF(shadowMap, shadow_matrix[index], stc, pos, norm, spot_shadow_bias * bias_scale, proj_shadow_res);
}
// </Vulkanstorm>
#endif

// Smooth fade (fsShadowFadeF.glsl): a fade at the far end of the range and dithered cascade
// transitions. Without it the blend fractions pass through unchanged.
#if defined(FS_SHADOW_EDGE_FADE)
float fsCascadeFadeOut(float t);
float fsCascadeFadeIn(float f);
float fsShadowEdgeFade(float shadow, float z);
#define FS_CASCADE_FADE_OUT(t) fsCascadeFadeOut(t)
#define FS_CASCADE_FADE_IN(f) fsCascadeFadeIn(f)
#else
#define FS_CASCADE_FADE_OUT(t) (t)
#define FS_CASCADE_FADE_IN(f) (f)
#endif

float sampleDirectionalShadow(vec3 pos, vec3 norm, vec2 pos_screen)
{
#if defined(SUN_SHADOW)
    float shadow = 0.0f;
    vec3 light_dir = normalize((sun_up_factor == 1) ? sun_dir : moon_dir);

    float dp_directional_light = max(0.0, dot(norm.xyz, light_dir));
          dp_directional_light = clamp(dp_directional_light, 0.0, 1.0);

    vec3 shadow_pos = pos.xyz;

    vec3 offset = light_dir.xyz * (1.0 - dp_directional_light);

    shadow_pos += offset * shadow_offset * 2.0;

    vec4 spos = vec4(shadow_pos.xyz, 1.0);

    if (spos.z > -shadow_clip.w)
    {
        vec4 lpos;
        vec4 near_split = shadow_clip*-0.75;
        vec4 far_split = shadow_clip*-1.25;
        vec4 transition_domain = near_split-far_split;
        float weight = 0.0;

        if (spos.z < near_split.z)
        {
            lpos = shadow_matrix[3]*spos;

            float w = 1.0;
            w -= FS_CASCADE_FADE_IN(max(spos.z-far_split.z, 0.0)/transition_domain.z);
            //w = clamp(w, 0.0, 1.0);
#if defined(PCSS_SHADOW)
            float contrib = pcssShadow(shadowMap3, 3, lpos, spos.xyz, norm)*w;
#else
            float contrib = pcfShadow(shadowMap3, norm, lpos, 1.0, pos_screen, light_dir, shadow_matrix[3], spos.xyz)*w;
#endif
            //if (contrib > 0)
            {
                shadow += contrib;
                weight += w;
            }
#if !defined(FS_SHADOW_EDGE_FADE)
            shadow += max((pos.z+shadow_clip.z)/(shadow_clip.z-shadow_clip.w)*2.0-1.0, 0.0);
#endif
        }

        if (spos.z < near_split.y && spos.z > far_split.z)
        {
            lpos = shadow_matrix[2]*spos;

            float w = 1.0;
            w -= FS_CASCADE_FADE_IN(max(spos.z-far_split.y, 0.0)/transition_domain.y);
            w -= FS_CASCADE_FADE_OUT(max(near_split.z-spos.z, 0.0)/transition_domain.z);
            //w = clamp(w, 0.0, 1.0);
#if defined(PCSS_SHADOW)
            float contrib = pcssShadow(shadowMap2, 2, lpos, spos.xyz, norm)*w;
#else
            float contrib = pcfShadow(shadowMap2, norm, lpos, 1.0, pos_screen, light_dir, shadow_matrix[2], spos.xyz)*w;
#endif
            //if (contrib > 0)
            {
                shadow += contrib;
                weight += w;
            }
        }

        if (spos.z < near_split.x && spos.z > far_split.y)
        {
            lpos = shadow_matrix[1]*spos;

            float w = 1.0;
            w -= FS_CASCADE_FADE_IN(max(spos.z-far_split.x, 0.0)/transition_domain.x);
            w -= FS_CASCADE_FADE_OUT(max(near_split.y-spos.z, 0.0)/transition_domain.y);
            //w = clamp(w, 0.0, 1.0);
#if defined(PCSS_SHADOW) && defined(PCSS_SHADOW_FETCH)
            float contrib = pcssSunContactShadow(shadowMap1, shadowDepth1, shadow_matrix[1], lpos, spos.xyz, norm, light_dir, PCSS_BLOCKER_DIST1)*w;
#elif defined(PCSS_SHADOW)
            float contrib = pcssShadow(shadowMap1, 1, lpos, spos.xyz, norm)*w;
#else
            float contrib = pcfShadow(shadowMap1, norm, lpos, 1.0, pos_screen, light_dir, shadow_matrix[1], spos.xyz)*w;
#endif
            //if (contrib > 0)
            {
                shadow += contrib;
                weight += w;
            }
        }

        if (spos.z > far_split.x)
        {
            lpos = shadow_matrix[0]*spos;

            float w = 1.0;
            w -= FS_CASCADE_FADE_OUT(max(near_split.x-spos.z, 0.0)/transition_domain.x);
            //w = clamp(w, 0.0, 1.0);
#if defined(PCSS_SHADOW) && defined(PCSS_SHADOW_FETCH)
            float contrib = pcssSunContactShadow(shadowMap0, shadowDepth0, shadow_matrix[0], lpos, spos.xyz, norm, light_dir, PCSS_BLOCKER_DIST0)*w;
#elif defined(PCSS_SHADOW)
            float contrib = pcssShadow(shadowMap0, 0, lpos, spos.xyz, norm)*w;
#else
            float contrib = pcfShadow(shadowMap0, norm, lpos, 1.0, pos_screen, light_dir, shadow_matrix[0], spos.xyz)*w;
#endif
            //if (contrib > 0)
            {
                shadow += contrib;
                weight += w;
            }
        }

        shadow /= weight;
#if defined(FS_SHADOW_EDGE_FADE)
        shadow = fsShadowEdgeFade(shadow, spos.z); // replaces the linear ramp over the far half of cascade 3
#endif
    }
    else
    {
        return 1.0f; // lit beyond the far split...
    }
    //shadow = min(dp_directional_light,shadow);
    return shadow;
#else
    return 1.0;
#endif
}

float sampleSpotShadow(vec3 pos, vec3 norm, int index, vec2 pos_screen)
{
#if defined(SPOT_SHADOW)
    pos += norm * spot_shadow_offset;

    vec4 spos = vec4(pos,1.0);

    // The projector shadow fades out with the distance from the camera to the pixel (Euclidean, so
    // it doesn't depend on which way the camera looks) over the last 20 % of spot_shadow_reach. It
    // used to fade with the sun's cascade splits, which follow what the camera sees: the fade-out
    // distance went from 16 m to 256 m with the camera's pitch and yaw, and projector shadows came
    // and went with the view. (From Vulkanstorm, by Anne Skydancer.)
    float reach = spot_shadow_reach > 1.0 ? spot_shadow_reach : 128.0;
    float fade = clamp((reach - length(pos)) / (0.2 * reach), 0.0, 1.0);
    if (fade <= 0.0)
    {
        return 1.0;
    }

    float shadow;
    vec4 lpos;
    if (index == 0)
    {
        lpos = shadow_matrix[4]*spos;
#if defined(PCSS_SHADOW) && defined(PCSS_SHADOW_FETCH)
        shadow = pcssSpotContactShadow(shadowMap4, shadowDepth4, shadow_matrix[4], lpos, spos.xyz, norm, 0.8);
#elif defined(PCSS_SHADOW)
        shadow = pcssSpotShadow(shadowMap4, 4, lpos, spos.xyz, norm, 0.8);
#else
        shadow = pcfSpotShadow(shadowMap4, lpos, 0.8, spos.xy);
#endif
    }
    else
    {
        lpos = shadow_matrix[5]*spos;
#if defined(PCSS_SHADOW) && defined(PCSS_SHADOW_FETCH)
        shadow = pcssSpotContactShadow(shadowMap5, shadowDepth5, shadow_matrix[5], lpos, spos.xyz, norm, 0.8);
#elif defined(PCSS_SHADOW)
        shadow = pcssSpotShadow(shadowMap5, 5, lpos, spos.xyz, norm, 0.8);
#else
        shadow = pcfSpotShadow(shadowMap5, lpos, 0.8, spos.xy);
#endif
    }
    return mix(1.0, shadow, fade);
#else
    return 1.0;
#endif
}
