/**
 * @file class1/deferred/skyF.glsl
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

// Outputs
out vec4 frag_data[4];

// Inputs
in vec3 pos;

#ifdef HAS_HDRI
in vec4 vary_position;
uniform float sky_hdr_scale;
uniform float hdri_split_screen;
uniform mat3 env_mat;
uniform sampler2D environmentMap;
#endif

uniform sampler2D rainbow_map;
uniform sampler2D halo_map;

uniform float moisture_level;
uniform float droplet_radius;
uniform float ice_level;

uniform vec3 camPosLocal;

uniform vec3  lightnorm;
uniform vec3  sunlight_color;
uniform vec3  moonlight_color;
uniform int   sun_up_factor;
uniform vec3  ambient_color;
uniform vec3  blue_horizon;
uniform vec3  blue_density;
uniform float haze_horizon;
uniform float haze_density;

uniform float cloud_shadow;
uniform float density_multiplier;
uniform float distance_multiplier;
uniform float max_y;

uniform vec3  glow;
uniform float sun_moon_glow_factor;

uniform int cube_snapshot;

uniform float fs_moon_corona;   // strength, 0 when off or while the moon isn't the light
uniform vec4 fs_twilight;       // xyz the sun's direction (sky space, y up), w strength through twilight, 0 otherwise
uniform vec4 fs_sea_horizon;    // rgb the water fog colour, w 1 to draw the sea below the horizon, 0 otherwise

vec3 srgb_to_linear(vec3 c);
vec3 linear_to_srgb(vec3 c);

#define PI 3.14159265

/////////////////////////////////////////////////////////////////////////
// The fragment shader for the sky
/////////////////////////////////////////////////////////////////////////


vec3 rainbow(float d)
{
    // 'Interesting' values of d are -0.75 .. -0.825, i.e. when view vec nearly opposite of sun vec
    // Rainbox tex is mapped with REPEAT, so -.75 as tex coord is same as 0.25.  -0.825 -> 0.175. etc.
    // SL-13629
    // Unfortunately the texture is inverted, so we need to invert the y coord, but keep the 'interesting'
    // part within the same 0.175..0.250 range, i.e. d = (1 - d) - 1.575
    d         = clamp(-0.575 - d, 0.0, 1.0);

    // With the colors in the lower 1/4 of the texture, inverting the coords leaves most of it inaccessible.
    // So, we can stretch the texcoord above the colors (ie > 0.25) to fill the entire remaining coordinate
    // space. This improves gradation, reduces banding within the rainbow interior. (1-0.25) / (0.425/0.25) = 4.2857
    float interior_coord = max(0.0, d - 0.25) * 4.2857;
    d = clamp(d, 0.0, 0.25) + interior_coord;

    float rad = (droplet_radius - 5.0f) / 1024.0f;
    return pow(texture(rainbow_map, vec2(rad+0.5, d)).rgb, vec3(1.8)) * moisture_level;
}

vec3 halo22(float d)
{
    d       = clamp(d, 0.1, 1.0);
    float v = sqrt(clamp(1 - (d * d), 0, 1));
    return texture(halo_map, vec2(0, v)).rgb * ice_level;
}

// The moon's aureole and corona. The sky's haze glow is turned off for the moon (SL-13768), which
// left it hanging in a black sky. Haze and thin cloud scatter its light forward into a soft glow,
// and cloud droplets diffract it into a faint ring, blue inside and red outside. cos_a is the
// cosine of the angle from the moon.
vec3 fsMoonCorona(float cos_a)
{
    float a = sqrt(max(2.0 * (1.0 - cos_a), 0.0));  // the angle, close enough this near the moon
    float wet = clamp(cloud_shadow * 1.5 + haze_density * 0.3, 0.0, 1.0);
    float aureole = exp(-a / 0.035) * 0.3 + exp(-a / 0.15) * 0.05;
    vec3 d = (vec3(a) - vec3(0.075, 0.068, 0.06)) / 0.012;
    vec3 ring = exp(-d * d);
    return moonlight_color * (aureole * (0.4 + 0.6 * wet) + ring * (0.12 * wet)) * fs_moon_corona;
}

// Twilight bands: around sunset, the Earth's own shadow rises in the sky opposite the sun as a
// blue-grey band on the horizon, and above it the Belt of Venus, a pink band lit by the reddened
// sun through the air. The shadow climbs as the sun sinks. Tints the sky's colour, so it never
// lights a sky that is dark.
vec3 fsTwilight(vec3 dir, vec3 color)
{
    vec2 sun_h = fs_twilight.xz;
    vec2 dir_h = dir.xz;
    if (dot(sun_h, sun_h) < 1e-6 || dot(dir_h, dir_h) < 1e-6)
    {
        return color;
    }
    float opposite = max(dot(normalize(dir_h), -normalize(sun_h)), 0.0);
    opposite *= opposite;
    float top = clamp(0.02 - fs_twilight.y * 0.8, 0.01, 0.15);   // the shadow's upper edge
    float h = dir.y;
    float shadow = opposite * (1.0 - smoothstep(top * 0.7, top, h));
    float belt = opposite * smoothstep(top * 0.8, top * 1.1, h) * (1.0 - smoothstep(top * 1.4, top * 2.6, h));
    color *= mix(vec3(1.0), vec3(0.6, 0.66, 0.85), shadow * fs_twilight.w);
    color *= mix(vec3(1.0), vec3(1.3, 0.95, 1.05), belt * fs_twilight.w);
    return color;
}

void main()
{
    // Get relative position
    vec3 rel_pos = pos.xyz - camPosLocal.xyz + vec3(0, 50, 0);

    // The sea to the horizon: below the horizon, what the distant sea shows at that angle is the sky
    // mirrored in it, so the sky is worked out for the mirrored direction and then dimmed and tinted
    // by how much of it the water reflects (Fresnel, about all of it at grazing angles).
    float sea_reflect = 1.0;
    bool sea = fs_sea_horizon.w > 0.0 && rel_pos.y < 0.0;
    if (sea)
    {
        float cos_v = clamp(-rel_pos.y / max(length(rel_pos), 1e-4), 0.0, 1.0);
        sea_reflect = 0.02 + 0.98 * pow(1.0 - cos_v, 5.0);
        rel_pos.y = -rel_pos.y;
    }

    // Adj position vector to clamp altitude
    if (rel_pos.y > 0.)
    {
        rel_pos *= (max_y / rel_pos.y);
    }
    if (rel_pos.y < 0.)
    {
        rel_pos *= (-32000. / rel_pos.y);
    }

    vec3 color;
#ifdef HAS_HDRI
    vec3 frag_coord = vary_position.xyz/vary_position.w;
    if (-frag_coord.x > ((1.0-hdri_split_screen)*2.0-1.0))
    {
        vec3 pos = normalize(rel_pos);
        pos = env_mat * pos;
        vec2 texCoord = vec2(atan(pos.z, pos.x) + PI, acos(pos.y)) / vec2(2.0 * PI, PI);
        color = textureLod(environmentMap, texCoord.xy, 0).rgb * sky_hdr_scale;
        color = min(color, vec3(8192*8192*16)); // stupidly large value arrived at by binary search -- avoids framebuffer corruption from some HDRIs

        frag_data[2] = vec4(0.0,0.0,0.0,GBUFFER_FLAG_HAS_HDRI);
    }
    else
#endif
    {
        // Normalized
        vec3  rel_pos_norm = normalize(rel_pos);
        float rel_pos_len  = length(rel_pos);

        // Grab this value and pass to frag shader for rainbows
        float rel_pos_lightnorm_dot = dot(rel_pos_norm, lightnorm.xyz);

        // Initialize temp variables
        vec3 sunlight = (sun_up_factor == 1) ? sunlight_color : moonlight_color * 0.7; //magic 0.7 to match legacy color

        // Sunlight attenuation effect (hue and brightness) due to atmosphere
        // this is used later for sunlight modulation at various altitudes
        vec3 light_atten = (blue_density + vec3(haze_density * 0.25)) * (density_multiplier * max_y);

        // Calculate relative weights
        vec3 combined_haze = max(abs(blue_density) + vec3(abs(haze_density)), vec3(1e-6));
        vec3 blue_weight   = blue_density / combined_haze;
        vec3 haze_weight   = haze_density / combined_haze;

        // Compute sunlight from rel_pos & lightnorm (for long rays like sky)
        float off_axis = 1.0 / max(1e-6, max(0., rel_pos_norm.y) + lightnorm.y);
        sunlight *= exp(-light_atten * off_axis);

        // Distance
        float density_dist = rel_pos_len * density_multiplier;

        // Transparency (-> combined_haze)
        // ATI Bugfix -- can't store combined_haze*density_dist in a variable because the ati
        // compiler gets confused.
        combined_haze = exp(-combined_haze * density_dist);

        // Compute haze glow
        float haze_glow = 1.0 - rel_pos_lightnorm_dot;
        // haze_glow is 0 at the sun and increases away from sun
        haze_glow = max(haze_glow, .001);
        // Set a minimum "angle" (smaller glow.y allows tighter, brighter hotspot)
        haze_glow *= glow.x;
        // Higher glow.x gives dimmer glow (because next step is 1 / "angle")
        haze_glow = pow(haze_glow, glow.z);
        // glow.z should be negative, so we're doing a sort of (1 / "angle") function

        // Add "minimum anti-solar illumination"
        // For sun, add to glow.  For moon, remove glow entirely. SL-13768
        haze_glow = (sun_moon_glow_factor < 1.0) ? 0.0 : (sun_moon_glow_factor * (haze_glow + 0.25));

        // Haze color above cloud
        color = (blue_horizon * blue_weight * (sunlight + ambient_color)
                + (haze_horizon * haze_weight) * (sunlight * haze_glow + ambient_color));

        // Final atmosphere additive
        color *= (1. - combined_haze);

        // Increase ambient when there are more clouds
        vec3 ambient = ambient_color + max(vec3(0), (1. - ambient_color)) * cloud_shadow * 0.5;

        // Dim sunlight by cloud shadow percentage
        sunlight *= max(0.0, (1. - cloud_shadow));

        // Haze color below cloud
        vec3 add_below_cloud = (blue_horizon * blue_weight * (sunlight + ambient)
                            + (haze_horizon * haze_weight) * (sunlight * haze_glow + ambient));

        // Attenuate cloud color by atmosphere
        combined_haze = sqrt(combined_haze);  // less atmos opacity (more transparency) below clouds

        // At horizon, blend high altitude sky color towards the darker color below the clouds
        color += (add_below_cloud - color) * (1. - sqrt(combined_haze));

        if (fs_twilight.w > 0.0)
        {
            color = fsTwilight(rel_pos_norm, color);
        }

        float optic_d = rel_pos_lightnorm_dot;
        // Both terms are scaled by their sky setting, which is zero for most skies. The
        // conditions are uniform, so skipping the fetches (and the rainbow's pow) leaves
        // control flow uniform and changes nothing where the setting is zero.
        if (moisture_level > 0.0)
        {
            color.rgb += rainbow(optic_d);
        }
        if (ice_level > 0.0)
        {
            color.rgb += halo22(optic_d);
        }
        if (fs_moon_corona > 0.0)
        {
            color.rgb += fsMoonCorona(optic_d);
        }
        color.rgb *= 2.;
        // the sea: what it doesn't reflect is the water's own colour, lit like the sky
        if (sea)
        {
            vec3 body = fs_sea_horizon.rgb * (ambient_color + sunlight * 0.5);
            color.rgb = mix(body, color.rgb, sea_reflect);
        }
        color.rgb = clamp(color.rgb, vec3(0), vec3(5));

        frag_data[2] = vec4(0.0,0.0,0.0,GBUFFER_FLAG_SKIP_ATMOS);
    }

    frag_data[1] = vec4(0);

#if defined(HAS_EMISSIVE)
    frag_data[0] = vec4(0);
    frag_data[3] = vec4(color.rgb, 1.0);
#else
    frag_data[0] = vec4(color.rgb, 1.0);
#endif
}

