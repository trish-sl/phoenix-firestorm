/**
 * @file class3/deferred/softenLightF.glsl
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

#define FLT_MAX 3.402823466e+38

out vec4 frag_color;

const float M_PI = 3.14159265;

#if defined(HAS_SUN_SHADOW) || defined(HAS_SSAO)
uniform sampler2D lightMap;
#endif

uniform float blur_size;
uniform float blur_fidelity;

#if defined(HAS_SSAO)
uniform float ssao_irradiance_scale;
uniform float ssao_irradiance_max;
#endif

// Inputs
uniform vec4 clipPlane;
uniform mat3 env_mat;
uniform mat3  ssao_effect_mat;
uniform vec3 sun_dir;
uniform vec3 moon_dir;
uniform int  sun_up_factor;
uniform int classic_mode;

in vec2 vary_fragcoord;

uniform mat4 inv_proj;
uniform vec2 screen_res;

vec4 getNorm(vec2 pos_screen);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);

void calcAtmosphericVarsLinear(vec3 inPositionEye, vec3 norm, vec3 light_dir, out vec3 sunlit, out vec3 amblit, out vec3 atten, out vec3 additive);
vec3  atmosFragLightingLinear(vec3 l, vec3 additive, vec3 atten);
vec3  scaleSoftClipFragLinear(vec3 l);

// reflection probe interface
void sampleReflectionProbes(inout vec3 ambenv, inout vec3 glossenv,
    vec2 tc, vec3 pos, vec3 norm, float glossiness, bool transparent, vec3 amblit_linear);
void sampleReflectionProbesLegacy(inout vec3 ambenv, inout vec3 glossenv, inout vec3 legacyenv,
        vec2 tc, vec3 pos, vec3 norm, float glossiness, float envIntensity, bool transparent, vec3 amblit_linear);
void applyGlossEnv(inout vec3 color, vec3 glossenv, vec4 spec, vec3 pos, vec3 norm);
void applyLegacyEnv(inout vec3 color, vec3 legacyenv, vec4 spec, vec3 pos, vec3 norm, float envIntensity);
float getDepth(vec2 pos_screen);

#if defined(FS_COLORED_SHADOW)
vec3 fsColoredShadowTransmittance(vec3 pos); // fsColoredShadowF.glsl: sunlight through translucent casters
#endif

vec3 linear_to_srgb(vec3 c);
float unpackRoughness(vec2 p);
vec3 srgb_to_linear(vec3 c);

uniform vec4 waterPlane;

#ifdef FS_WATER_CAUSTICS
float fsWaterCaustics(vec3 pos, vec3 norm, vec3 light_dir, vec4 water_plane);
#endif
#ifdef FS_FOLIAGE
uniform float fs_foliage; // strength of the sunlight seen through backlit leaves, 0 off
#endif
#ifdef FS_WET_SHORE
float fsWetShore(vec3 pos, vec3 norm, vec4 water_plane);
#endif

uniform int cube_snapshot;

uniform float sky_hdr_scale;

#ifdef FS_SSR_HALF
// Screen-space reflections traced at half resolution beforehand (fsSSRHalfF.glsl): texel t
// holds the march of full-resolution pixel 2t. Each pixel blends the 2x2 texels around it by
// distance, and by how well the traced pixel's depth and normal match its own, so a
// reflection doesn't bleed across a silhouette or onto a differently facing surface.
uniform sampler2D fs_ssr_half_map;
uniform vec2 fs_ssr_full_res;

bool isFarDepth(float d);

vec4 fsSSRHalfUpsample(vec3 pos, vec3 norm)
{
    if (cube_snapshot != 0)
    {
        return vec4(0); // probe captures don't use screen-space reflections
    }
    ivec2 hi_max = textureSize(fs_ssr_half_map, 0) - 1;
    vec2 c = (gl_FragCoord.xy - 0.5) * 0.5;
    vec2 base_f = floor(c);
    vec2 f = c - base_f;
    ivec2 base = ivec2(base_f);

    float z = max(-pos.z, 1e-4);
    float inv_tol = 1.0 / (0.01 + 0.02 * z);
    vec3 n = normalize(norm);

    vec4 sum = vec4(0);
    float sum_w = 0.0;
    vec4 best = vec4(0);
    float best_score = 1e30;
    for (int i = 0; i < 4; ++i)
    {
        ivec2 o = ivec2(i & 1, i >> 1);
        ivec2 t = clamp(base + o, ivec2(0), hi_max);
        vec2 stc = (vec2(min(t * 2, ivec2(fs_ssr_full_res) - 1)) + 0.5) / fs_ssr_full_res;
        float sd = getDepth(stc);
        if (isFarDepth(sd))
        {
            continue;
        }
        float rel = abs(-getPositionWithDepth(stc, sd).z - z) * inv_tol;
        float nd = max(dot(getNorm(stc).xyz, n), 0.0);
        nd *= nd;
        nd *= nd;
        nd *= nd; // nd^8: reflections change fast with the normal

        vec2 tent = mix(vec2(1.0) - f, f, vec2(o));
        float w = tent.x * tent.y * nd / (1.0 + rel * rel);
        vec4 s = texelFetch(fs_ssr_half_map, t, 0);
        sum += w * s;
        sum_w += w;

        float score = rel + 4.0 * (1.0 - nd);
        if (score < best_score)
        {
            best_score = score;
            best = s;
        }
    }
    return sum_w > 1e-4 ? sum / sum_w : best;
}
#endif

#if defined(FS_SSR_HALF) || defined(FS_INDIRECT)
void sampleReflectionProbesFS(inout vec3 ambenv, inout vec3 glossenv,
        vec2 tc, vec3 pos, vec3 norm, vec3 amb_norm, float glossiness, vec3 amblit, bool ssr_given, vec4 ssr);
void sampleReflectionProbesLegacyFS(inout vec3 ambenv, inout vec3 glossenv, inout vec3 legacyenv,
        vec2 tc, vec3 pos, vec3 norm, vec3 amb_norm, float glossiness, float envIntensity, vec3 amblit, bool ssr_given, vec4 ssr);
#endif

#ifdef FS_INDIRECT
// Bent normals and one bounce of screen-space indirect light (FSRenderBentNormals,
// FSRenderSSGI), found at half resolution beforehand (fsIndirectHalfF.glsl) and blended with the
// last frames (fsIndirectTemporalF.glsl). Texel t holds full-resolution pixel 2t:
//  fs_indirect_gi:   rgb the light bouncing in from nearby surfaces, a the view depth
//  fs_indirect_bent: xyz how far the open directions lean away from the normal (view space),
//                    a the view depth
// Each pixel blends the 2x2 texels around it by distance and by how close their depth is to its
// own, so neither bleeds across a silhouette.
uniform sampler2D fs_indirect_gi;
uniform sampler2D fs_indirect_bent;
uniform vec4 fs_indirect_params;    // x: indirect light strength (0: off), y: bent normal strength (0: off)

void fsIndirectUpsample(vec3 pos, out vec3 gi, out vec3 bend)
{
    gi = vec3(0);
    bend = vec3(0);
    if (cube_snapshot != 0)
    {
        return; // probe captures: the pass does not run for them
    }
    ivec2 hi_max = textureSize(fs_indirect_gi, 0) - 1;
    vec2 c = (gl_FragCoord.xy - 0.5) * 0.5;
    vec2 base_f = floor(c);
    vec2 f = c - base_f;
    ivec2 base = ivec2(base_f);

    float z = max(-pos.z, 1e-4);
    float inv_tol = 1.0 / (0.01 + 0.02 * z);

    vec3 sum_gi = vec3(0);
    vec3 sum_bend = vec3(0);
    float sum_w = 0.0;
    float best_rel = 1e30;
    for (int i = 0; i < 4; ++i)
    {
        ivec2 o = ivec2(i & 1, i >> 1);
        ivec2 t = clamp(base + o, ivec2(0), hi_max);
        vec4 g = texelFetch(fs_indirect_gi, t, 0);
        if (g.a <= 0.0)
        {
            continue; // sky under that texel
        }
        vec4 b = texelFetch(fs_indirect_bent, t, 0);
        float rel = abs(g.a - z) * inv_tol;
        vec2 tent = mix(vec2(1.0) - f, f, vec2(o));
        float w = tent.x * tent.y / (1.0 + rel * rel * 4.0);
        sum_gi += w * g.rgb;
        sum_bend += w * b.xyz;
        sum_w += w;
        if (rel < best_rel)
        {
            best_rel = rel;
            gi = g.rgb;
            bend = b.xyz;
        }
    }
    if (sum_w > 1e-4)
    {
        gi = sum_gi / sum_w;
        bend = sum_bend / sum_w;
    }
    gi *= fs_indirect_params.x;
    bend *= fs_indirect_params.y;
}
#endif

void calcHalfVectors(vec3 lv, vec3 n, vec3 v, out vec3 h, out vec3 l, out float nh, out float nl, out float nv, out float vh, out float lightDist);
float blinnPhongLobe(float nh, float glossiness);
void calcDiffuseSpecular(vec3 baseColor, float metallic, inout vec3 diffuseColor, inout vec3 specularColor);

vec3 pbrBaseLight(vec3 diffuseColor,
                  vec3 specularColor,
                  float metallic,
                  vec3 pos,
                  vec3 norm,
                  float perceptualRoughness,
                  vec3 light_dir,
                  vec3 sunlit,
                  float scol,
                  vec3 radiance,
                  vec3 irradiance,
                  vec3 colorEmissive,
                  float ao,
                  vec3 additive,
                  vec3 atten,
                  float micro);

GBufferInfo getGBuffer(vec2 screenpos);
float horizonOcclusion(vec3 r, vec3 geometricNormal);
float fsSpecularOcclusion(float ao, float nv, float perceptualRoughness); // deferredUtil.glsl
float fsMicroShadow(float nl, float ao);
vec3 clampHDRRange(vec3 color);

void adjustIrradiance(inout vec3 irradiance, float ambocc)
{
    // use sky settings ambient or irradiance map sample, whichever is brighter
    //irradiance = max(amblit_linear, irradiance);

#if defined(HAS_SSAO)
    irradiance = mix(ssao_effect_mat * min(irradiance.rgb*ssao_irradiance_scale, vec3(ssao_irradiance_max)), irradiance.rgb, ambocc);
#endif
}

// The screen-space occlusion as a scalar visibility, on the response curve adjustIrradiance
// gives the diffuse lobe.
//
// The diffuse path spends this buffer through ssao_irradiance_scale and the value factor of
// ssao_effect_mat, which is what "SSAO strength" means to anyone turning those knobs. Handing
// the specular lobe the raw buffer instead left reflections at full occlusion however far down
// the strength was dialled -- one buffer, two different curves, and only one of them responding
// to the settings.
//
// The value factor is the matrix's row sum: it takes grey to grey scaled by exactly that, which
// is the whole of what it does to magnitude. Its other factor is saturation, which is chroma and
// has no place in a visibility scalar.
//
// ssao_irradiance_max is deliberately not folded in either. It bounds a radiometric quantity in
// the diffuse expression -- there is nothing for it to bound here, and the value it would have
// to be compared against is radiance rather than irradiance.
float ssaoVisibility(float ambocc)
{
#if defined(HAS_SSAO)
    float value_factor = dot(ssao_effect_mat * vec3(1.0), vec3(1.0 / 3.0));
    float occluded = clamp(ssao_irradiance_scale * value_factor, 0.0, 1.0);
    return clamp(mix(occluded, 1.0, ambocc), 0.0, 1.0);
#else
    return 1.0;
#endif
}

void main()
{
    vec2  tc           = vary_fragcoord.xy;
    float depth        = getDepth(tc.xy);
    vec4  pos          = getPositionWithDepth(tc, depth);

    GBufferInfo gb = getGBuffer(tc);

    vec3 colorEmissive = gb.emissive.rgb;
    float envIntensity = gb.envIntensity;
    vec3  light_dir   = (sun_up_factor == 1) ? sun_dir : moon_dir;

    vec4 baseColor     = gb.albedo;
    vec4 spec        = gb.specular; // NOTE: PBR linear Emissive

#if defined(HAS_SUN_SHADOW) || defined(HAS_SSAO)
    vec2 scol_ambocc = texture(lightMap, vary_fragcoord.xy).rg;
#endif

#if defined(HAS_SUN_SHADOW)
    float scol       = max(scol_ambocc.r, baseColor.a);
#else
    float scol = 1.0;
#endif
#if defined(HAS_SSAO)
    float ambocc     = scol_ambocc.g;
#else
    float ambocc = 1.0;
#endif

    vec3  color = vec3(0);
    float bloom = 0.0;

    vec3 sunlit = vec3(0);
    vec3 amblit = vec3(0);
    vec3 additive = vec3(0);
    vec3 atten = vec3(1);

    // The HDRI and WindLight sky branches below copy a stored colour and read none of the
    // atmospheric terms, so skip the per-fragment sky model for them. Sky is routinely a large
    // share of the screen, and the precedence here matches the branch order below.
    bool is_pbr = GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_PBR);
    bool needs_atmos = is_pbr ||
        !(GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_HDRI) || GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_SKIP_ATMOS));

    if (needs_atmos)
    {
        calcAtmosphericVarsLinear(pos.xyz, gb.normal, light_dir, sunlit, amblit, additive, atten);

        if (classic_mode > 0)
            sunlit *= 1.35;
    }

    vec3 sunlit_linear = sunlit;
#if defined(FS_COLORED_SHADOW)
    sunlit_linear *= fsColoredShadowTransmittance(pos.xyz);
#endif
    vec3 amblit_linear = amblit;

    // Backlit foliage: alpha masked surfaces (leaves, grass, flagged through FS_MASK_FLAG) are thin,
    // so with the sun behind them they glow with the light coming through. That is the case when
    // the sun and the eye are on opposite sides of the surface; the normal's sign does not matter,
    // as single sided leaves seen from behind keep their front normal. Strongest looking toward
    // the sun, under the leaf's own sun shadow so a leaf deep in the canopy stays dark.
    vec3 fs_leaf_light = vec3(0);
#ifdef FS_FOLIAGE
    if (fs_foliage > 0.0 &&
        (abs(gb.gbufferFlag - (GBUFFER_FLAG_HAS_ATMOS - 0.05)) < 0.02 || abs(gb.gbufferFlag - (GBUFFER_FLAG_HAS_PBR - 0.05)) < 0.02))
    {
        vec3 to_eye = -normalize(pos.xyz);
        float nl = dot(gb.normal, light_dir);
        float nv = dot(gb.normal, to_eye);
        float through = nl * nv < 0.0 ? abs(nl) : 0.0;
        float toward_sun = pow(max(dot(-to_eye, light_dir), 0.0), 4.0);
        fs_leaf_light = sunlit_linear * (scol * through * mix(0.35, 1.0, toward_sun) * fs_foliage);
    }
#endif

#ifdef FS_WATER_CAUSTICS
    // Sunlight focused by the waves onto surfaces under the water, on the diffuse albedo and
    // under the same sun shadow (fsCausticsF.glsl). Taken before the branches below modify scol.
    vec3 fs_caustics = vec3(0);
    if (needs_atmos)
    {
        vec3 fs_albedo = is_pbr ? baseColor.rgb * (1.0 - spec.b) : srgb_to_linear(baseColor.rgb) * (1.0 - baseColor.a);
        fs_caustics = fsWaterCaustics(pos.xyz, gb.normal, light_dir, waterPlane) * scol * sunlit_linear * fs_albedo;
    }
#endif

    // Wet shoreline band (fsWetShoreF.glsl): how wet the surface is, applied to the material in
    // the branches below before any lighting, so every term sees the wet surface
    float fs_wet = 0.0;
#ifdef FS_WET_SHORE
    if (needs_atmos)
    {
        fs_wet = fsWetShore(pos.xyz, gb.normal, waterPlane);
    }
#endif

    vec3  radiance  = vec3(0);

    // Firestorm inputs to the ambient and reflection terms below: the half-resolution SSR, the
    // bent normal the irradiance lobe looks along, and the bounced light
    vec4 fs_ssr = vec4(0);
    bool fs_ssr_given = false;
    vec3 fs_amb_norm = gb.normal;
    vec3 fs_gi = vec3(0);
    if (needs_atmos)
    {
#ifdef FS_SSR_HALF
        fs_ssr = fsSSRHalfUpsample(pos.xyz, gb.normal);
        fs_ssr_given = true;
#endif
#ifdef FS_INDIRECT
        vec3 fs_bend;
        fsIndirectUpsample(pos.xyz, fs_gi, fs_bend);
        // The bend was measured on the depth buffer's surface; added to the shading normal, it
        // keeps the normal map's detail and leans it the way the open sky is
        fs_amb_norm = normalize(gb.normal + fs_bend);
#endif
    }

    if (is_pbr)
    {
        vec3 orm = spec.rgb;
        float perceptualRoughness = unpackRoughness(spec.ga);
        float metallic = orm.b;
        float ao = orm.r;

        // Wet: water fills the pores, so porous (non-metal) surfaces darken, and the film on top
        // makes every surface smoother
        baseColor.rgb *= mix(1.0, 0.55, fs_wet * (1.0 - metallic));
        perceptualRoughness = mix(perceptualRoughness, min(perceptualRoughness, 0.15), fs_wet);

        vec3  irradiance = amblit_linear;

        // PBR IBL
        float gloss      = 1.0 - perceptualRoughness;

#if defined(FS_SSR_HALF) || defined(FS_INDIRECT)
        sampleReflectionProbesFS(irradiance, radiance, tc, pos.xyz, gb.normal, fs_amb_norm, gloss, amblit_linear, fs_ssr_given, fs_ssr);
#else
        sampleReflectionProbes(irradiance, radiance, tc, pos.xyz, gb.normal, gloss, false, amblit_linear);
#endif

        // A normal map can aim the reflection below the surface it sits on, where the probe
        // returns radiance arriving through the geometry. The forward path tests against
        // vary_normal; here the geometric normal comes out of the GBuffer channel that used to
        // hold environment intensity.
        radiance *= horizonOcclusion(reflect(normalize(pos.xyz), gb.normal), gb.geoNormal);

        // One ambient visibility for the whole shading model, rather than adjustIrradiance
        // multiplying the screen-space half into irradiance before the fact. The two terms
        // measure the same thing -- how much of the ambient hemisphere this fragment can see --
        // at two different scales, so they combine by taking the stronger rather than by
        // multiplying, which would count the overlap they share twice.
        //
        // adjustIrradiance stays on the legacy branch below, where its saturation matrix and
        // absolute clamp are part of the intended look rather than an obstacle to a physical one.
        float visibility = min(ao, ssaoVisibility(ambocc));

        vec3 diffuseColor = vec3(0.0);
        vec3 specularColor = vec3(0.0);
        calcDiffuseSpecular(baseColor.rgb, metallic, diffuseColor, specularColor);

        vec3 v = -normalize(pos.xyz);
        // Normal-map micro-shadows, 1 when off. The cone comes from the normal map's own
        // occlusion; SSAO measures larger scale geometry, not the bumps the normal map draws.
        float micro = fsMicroShadow(dot(gb.normal, light_dir), ao);
        color = pbrBaseLight(diffuseColor, specularColor, metallic, v, gb.normal, perceptualRoughness, light_dir, sunlit_linear, scol, radiance, irradiance, colorEmissive, visibility, additive, atten, micro);
        // Light through the leaf takes its colour twice, so it comes out more saturated
        color += fs_leaf_light * diffuseColor * diffuseColor / max(dot(diffuseColor, vec3(0.3333)), 0.05);
        // Light bounced from nearby surfaces, on the diffuse albedo like the ambient light
        color += fs_gi * diffuseColor;
    }
    else if (GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_HAS_HDRI))
    {
        // actual HDRI sky, just copy color value
        color = colorEmissive.rgb;
    }
    else if (GET_GBUFFER_FLAG(gb.gbufferFlag, GBUFFER_FLAG_SKIP_ATMOS))
    {
        //should only be true of WL sky, port over base color value and scale for fake HDR
#if defined(HAS_EMISSIVE)
        color = colorEmissive.rgb;
#else
        color = baseColor.rgb;
#endif
        color = srgb_to_linear(color);
        color *= sky_hdr_scale;
    }
    else
    {
        // legacy shaders are still writng sRGB to gbuffer
        baseColor.rgb = srgb_to_linear(baseColor.rgb);

        spec.rgb = srgb_to_linear(spec.rgb);

        // Wet (see the PBR branch): darker, glossier, and reflecting the environment
        baseColor.rgb *= mix(1.0, 0.6, fs_wet);
        spec.a = mix(spec.a, max(spec.a, 0.8), fs_wet);
        spec.rgb = max(spec.rgb, vec3(0.04 * fs_wet));
        envIntensity = max(envIntensity, 0.35 * fs_wet);

        float da          = clamp(dot(gb.normal, light_dir.xyz), 0.0, 1.0);

        vec3 irradiance = amblit;
        vec3 glossenv = vec3(0);
        vec3 legacyenv = vec3(0);

#if defined(FS_SSR_HALF) || defined(FS_INDIRECT)
        sampleReflectionProbesLegacyFS(irradiance, glossenv, legacyenv, tc, pos.xyz, gb.normal, fs_amb_norm, spec.a, envIntensity, amblit_linear, fs_ssr_given, fs_ssr);
#else
        sampleReflectionProbesLegacy(irradiance, glossenv, legacyenv, tc, pos.xyz, gb.normal, spec.a, envIntensity, false, amblit_linear);
#endif

        adjustIrradiance(irradiance, ambocc);
        irradiance += fs_gi; // light bounced from nearby surfaces (FSRenderSSGI)

        // The environment lobes get the same ambient visibility the diffuse one does.
        // adjustIrradiance spends the screen-space buffer on irradiance alone, so a legacy
        // surface down in a crevice had its diffuse ambient occluded while its reflection
        // carried on at full strength -- the brighter of the two terms, and the one the eye
        // reads as contact. On the response curve the settings describe, not the raw buffer;
        // see ssaoVisibility.
        float envVisibility = ssaoVisibility(ambocc);
        // Specular occlusion (see fsSpecularOcclusion): envVisibility itself when off. glossenv
        // is filtered by (1 - glossiness), legacyenv is the mirror lookup.
        float fs_nv = dot(gb.normal, -normalize(pos.xyz));
        glossenv *= fsSpecularOcclusion(envVisibility, fs_nv, 1.0 - spec.a);
        legacyenv *= fsSpecularOcclusion(envVisibility, fs_nv, 0.0);

        // No normal-map micro-shadows here: the cone needs the material's own occlusion, which
        // legacy materials do not have, and SSAO is the wrong scale for it.

        // apply lambertian IBL only (see pbrIbl)
        color.rgb = irradiance;

        if (classic_mode > 0)
        {
            da = pow(da,1.2);
            vec3 sun_contrib = vec3(min(da, scol));

            color.rgb = srgb_to_linear(color.rgb * 0.9 + (linear_to_srgb(sun_contrib) * sunlit_linear * 0.7));
            sunlit_linear = srgb_to_linear(sunlit_linear);
        }
        else
        {
            vec3 sun_contrib = min(da, scol) * sunlit_linear;
            color.rgb += sun_contrib;
        }

        color.rgb *= baseColor.rgb;
        // Backlit foliage, more saturated as in the PBR branch
        color.rgb += fs_leaf_light * baseColor.rgb * baseColor.rgb / max(dot(baseColor.rgb, vec3(0.3333)), 0.05);

        vec3 refnormpersp = reflect(pos.xyz, gb.normal);

        if (spec.a > 0.0)
        {
            vec3  lv = light_dir.xyz;
            vec3  h, l, v = -normalize(pos.xyz);
            float nh, nl, nv, vh, lightDist;
            vec3 n = gb.normal;
            calcHalfVectors(lv, n, v, h, l, nh, nl, nv, vh, lightDist);

            if (nl > 0.0 && nh > 0.0)
            {
                float lit = min(nl*6.0, 1.0);

                float sa = nh;
                float f = 1 - vh;
                float f2 = f * f;
                float fres = (f2 * f2 * f) * 0.4 + 0.5;
                float gtdenom = 2 * nh;
                float gt = max(0,(min(gtdenom * nv / vh, gtdenom * nl / vh)));

                scol *= fres*blinnPhongLobe(nh, spec.a)*gt/(nh*max(nl, 1e-6));
                color.rgb += lit*scol*sunlit_linear.rgb*spec.rgb;
            }

            // add radiance map
            applyGlossEnv(color, glossenv, spec, pos.xyz, gb.normal);

        }

        color.rgb = mix(color.rgb, baseColor.rgb, baseColor.a);

        if (envIntensity > 0.0)
        {  // add environment map
            applyLegacyEnv(color, legacyenv, spec, pos.xyz, gb.normal, envIntensity);
        }
   }

#ifdef FS_WATER_CAUSTICS
    color.rgb += fs_caustics;
#endif

    //color.r = classic_mode > 0 ? 1.0 : 0.0;
    float final_scale = 1;
    if (classic_mode > 0)
        final_scale = 1.1;

    frag_color.rgb = clampHDRRange(color.rgb * final_scale); //output linear since local lights will be added to this shader's results
    frag_color.a = 0.0;
}
