/**
 * @file class3\environment\underWaterF.glsl
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

out vec4 frag_color;

uniform sampler2D bumpMap;
uniform sampler2D exclusionTex;

#ifdef TRANSPARENT_WATER
uniform sampler2D screenTex;
#endif

uniform vec4 fogCol;
uniform vec3 lightDir;
uniform vec3 specular;
uniform float lightExp;
uniform vec2 fbScale;
uniform float refScale;
uniform float znear;
uniform float zfar;
uniform float kd;
uniform vec4 waterPlane;
uniform vec3 eyeVec;
uniform vec4 waterFogColor;
uniform vec3 waterFogColorLinear;
uniform float waterFogKS;
uniform vec2 screenRes;
uniform float fs_snell_window;    // strength of Snell's window, 0 is off

//bigWave is (refCoord.w, view.w);
in vec4 refCoord;
in vec4 littleWave;
in vec4 view;
in vec3 vary_position;

#ifdef FS_WATER_FOAM
in vec3 vary_normal;
float fsWaterFoam(vec2 screen_tc, vec3 pos, vec3 up, vec2 view_offset);
#endif

vec4 applyWaterFogViewLinearNoClip(vec3 pos, vec4 color);

// Snell's window: from below, light from above the water only gets through within the critical
// angle of the surface normal (48.6 degrees for water into air), a cone about 97 degrees wide.
// Toward its rim the surface reflects more and more (Fresnel from the water side), and past it
// the reflection is total: the underside of the surface mirrors the depths. The waves tilt the
// normal, so the rim ripples. Returns how much of the view from above gets through.
float fsSnellTransmission(vec3 pos, vec3 wavef)
{
    vec3 n = normalize(waterPlane.xyz);
    vec3 t = cross(n, vec3(0.0, 0.0, 1.0));
    t = dot(t, t) > 1e-4 ? normalize(t) : vec3(1.0, 0.0, 0.0);
    vec3 b = cross(n, t);
    n = normalize(n + 0.12 * (wavef.x * t + wavef.y * b));

    float c = dot(normalize(pos), n);         // cosine of the angle to the normal, inside the water
    if (c <= 0.0)
    {
        return 1.0;                           // not looking up at the surface
    }
    const float eta = 1.333;
    float sin_t2 = eta * eta * (1.0 - c * c);
    if (sin_t2 >= 1.0)
    {
        return 0.0;
    }
    float ct = sqrt(1.0 - sin_t2);
    float rs = (eta * c - ct) / (eta * c + ct);
    float rp = (eta * ct - c) / (eta * ct + c);
    return 1.0 - 0.5 * (rs * rs + rp * rp);
}
void mirrorClip(vec3 position);

void main()
{
    mirrorClip(vary_position);
    vec2 screen_tc = (refCoord.xy/refCoord.z) * 0.5 + 0.5;
    float water_mask = texture(exclusionTex, screen_tc).r;

    vec4 color;

    //get detail normals
    vec3 wave1 = texture(bumpMap, vec2(refCoord.w, view.w)).xyz*2.0-1.0;
    vec3 wave2 = texture(bumpMap, littleWave.xy).xyz*2.0-1.0;
    vec3 wave3 = texture(bumpMap, littleWave.zw).xyz*2.0-1.0;
    vec3 wavef = normalize(wave1+wave2+wave3);

    //figure out distortion vector (ripply)
    vec2 distort = screen_tc;
    distort = mix(distort, distort+wavef.xy*refScale, water_mask);

#ifdef TRANSPARENT_WATER
    vec4 fb = texture(screenTex, distort);
#else
    vec4 fb = vec4(waterFogColorLinear, 0.0);
#endif

#ifdef FS_WATER_FOAM
    // Shoreline foam seen from below (fsWaterFoamF.glsl): it scatters the light coming down
    // through it, so it shows as a pale, washed out patch on the underside of the surface
    float foam = fsWaterFoam(screen_tc, vary_position, normalize(vary_normal), view.xy) * water_mask;
    if (foam > 0.0)
    {
        float luma = dot(fb.rgb, vec3(0.2126, 0.7152, 0.0722));
        fb.rgb = mix(fb.rgb, vec3(luma * 1.4 + 0.02), foam * 0.7);
    }
#endif

    if (fs_snell_window > 0.0)
    {
        // Outside the window the surface mirrors the deep water below it
        float transmit = fsSnellTransmission(vary_position, wavef);
        transmit = mix(1.0, transmit, clamp(fs_snell_window, 0.0, 1.0) * water_mask);
        fb.rgb = mix(waterFogColorLinear, fb.rgb, transmit);
    }

    fb = applyWaterFogViewLinearNoClip(vary_position, fb);

    frag_color = max(fb, vec4(0));
}
