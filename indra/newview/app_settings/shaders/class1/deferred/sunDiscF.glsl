/**
 * @file sunDiscF.glsl
 *
 * $LicenseInfo:firstyear=2005&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2005, Linden Research, Inc.
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

out vec4 frag_data[4];

vec3 srgb_to_linear(vec3 c);

uniform sampler2D diffuseMap;
uniform sampler2D altDiffuseMap;
uniform float blend_factor; // interp factor between sunDisc A/B
uniform float fs_sun_limb;  // limb darkening coefficient, 0 is off
in vec2 vary_texcoord0;
in float sun_fade;

void main()
{
    vec4 sunDiscA = texture(diffuseMap, vary_texcoord0.xy);
    vec4 sunDiscB = texture(altDiffuseMap, vary_texcoord0.xy);
    vec4 c     = mix(sunDiscA, sunDiscB, blend_factor);

    // Limb darkening: toward the disc's edge we look through the sun's cooler outer layers, so
    // it is darker and a little redder there (I = 1 - u (1 - mu), mu the cosine of the angle
    // to the surface normal). The disc fills most of the texture; the glow past it dims too.
    if (fs_sun_limb > 0.0)
    {
        float r = length(vary_texcoord0.xy - 0.5) * 2.2;
        float mu = sqrt(max(1.0 - r * r, 0.0));
        c.rgb *= vec3(1.0) - fs_sun_limb * vec3(0.85, 1.0, 1.15) * (1.0 - mu);
    }


    // SL-9806 stars poke through
    //c.a *= sun_fade;

    frag_data[0] = vec4(0);
    frag_data[1] = vec4(0.0f);
    frag_data[2] = vec4(0.0, 1.0, 0.0, GBUFFER_FLAG_SKIP_ATMOS);
#if defined(HAS_EMISSIVE)
    frag_data[0] = vec4(0);
    frag_data[3] = c;
#else
    frag_data[0] = c;
#endif
}

