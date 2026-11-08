/**
 * @file deferred/fullbrightF.glsl
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

#ifdef FS_GDD_LEGACY
#extension GL_ARB_bindless_texture : require
// FSPerfGPUDrivenLegacy (diffuseV.glsl, fullbrightV.glsl): the texture the stock diffuseLookup would pick,
// as a bindless handle, and the batch's minimum_alpha
flat in uvec2 fs_gdd_diffuse;
flat in float fs_gdd_minimum_alpha;
#define HAS_DIFFUSE_LOOKUP
vec4 diffuseLookup(vec2 texcoord)
{
    return texture(sampler2D(fs_gdd_diffuse), texcoord);
}
#endif

#ifdef FS_WBOIT
// Weighted blended order-independent transparency (FSRenderAlphaWBOIT, see fsWBOITCompositeF).
// Same block in class2 alphaF, class1 fullbrightF, class3 materialF and class2 pbralphaF.
layout(location = 0) out vec4 frag_color;
layout(location = 1) out vec4 fs_wboit_weight;
uniform int fs_wboit; // 1 while accumulating, 0 for the stock sorted output
#else
out vec4 frag_color;
#endif

#ifdef FS_HAIR
// Two-pass rigged alpha (FSRenderHairTwoPass, see LLDrawPoolAlpha::renderRiggedAlphaTwoPass).
// Same block in class2 alphaF, class1 fullbrightF, class3 materialF and class2 pbralphaF.
// fs_hair_pass: 0 stock, 1 solid core only (coverage >= fs_hair_cut, writes depth),
// 2 soft fringe only (coverage < fs_hair_cut, depth tested only), 3 depth only.
uniform int fs_hair_pass;
uniform float fs_hair_cut;
#endif

#if !defined(HAS_DIFFUSE_LOOKUP)
uniform sampler2D diffuseMap;
#endif

in vec3 vary_position;
in vec4 vertex_color;
in vec2 vary_texcoord0;

vec3 srgb_to_linear(vec3 cs);
vec3 linear_to_srgb(vec3 cl);

#ifdef HAS_ALPHA_MASK
#ifdef FS_GDD_LEGACY
#define minimum_alpha fs_gdd_minimum_alpha
#else
uniform float minimum_alpha;
#endif
#endif

#ifdef IS_ALPHA
uniform vec4 waterPlane;
void waterClip(vec3 pos);
void calcAtmosphericVars(vec3 inPositionEye, vec3 light_dir, float ambFactor, out vec3 sunlit, out vec3 amblit, out vec3 additive,
                         out vec3 atten);
vec4 applySkyAndWaterFog(vec3 pos, vec3 additive, vec3 atten, vec4 color);
#endif

void mirrorClip(vec3 pos);

void main()
{
    mirrorClip(vary_position);
#ifdef IS_ALPHA
    waterClip(vary_position.xyz);
#endif

#ifdef HAS_DIFFUSE_LOOKUP
    vec4 color = diffuseLookup(vary_texcoord0.xy);
#else
    vec4 color = texture(diffuseMap, vary_texcoord0.xy);
#endif

    float final_alpha = color.a * vertex_color.a;

#ifdef HAS_ALPHA_MASK
    if (color.a < minimum_alpha)
    {
        discard;
    }
#endif

#ifdef FS_HAIR
    if ((fs_hair_pass == 1 && final_alpha < fs_hair_cut) || (fs_hair_pass == 2 && final_alpha >= fs_hair_cut))
    {
        discard;
    }
    if (fs_hair_pass == 3)
    { // depth only: the colour mask is off, skip the lighting
        frag_color = vec4(0.0);
        return;
    }
#endif

    color.rgb *= vertex_color.rgb;

    vec3 pos = vary_position;

    color.a = final_alpha;
#ifndef IS_HUD
    color.rgb = srgb_to_linear(color.rgb);
#ifdef IS_ALPHA

    vec3 sunlit;
    vec3 amblit;
    vec3 additive;
    vec3 atten;
    calcAtmosphericVars(pos.xyz, vec3(0), 1.0, sunlit, amblit, additive, atten);

    color.rgb = applySkyAndWaterFog(pos, additive, atten, color).rgb;

#endif

#endif

    frag_color = max(color, vec4(0));
#ifdef FS_WBOIT
    fs_wboit_weight = vec4(0.0);
    if (fs_wboit != 0)
    { // weight falls with the square of the view distance in metres (McGuire and Bavoil eq. 7)
        float wboit_a = clamp(frag_color.a, 0.0, 1.0);
        float wboit_z = 1.0 / gl_FragCoord.w;
        float wboit_w = wboit_a * clamp(1.0 / (1e-5 + pow(wboit_z / 5.0, 2.0) + pow(wboit_z / 200.0, 6.0)), 1e-3, 300.0);
        frag_color = vec4(min(frag_color.rgb, vec3(64.0)) * wboit_a * wboit_w, wboit_a);
        fs_wboit_weight = vec4(wboit_a * wboit_w, 0.0, 0.0, 0.0);
    }
#endif
}

