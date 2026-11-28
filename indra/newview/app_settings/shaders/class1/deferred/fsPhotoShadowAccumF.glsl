/**
 * @file class1/deferred/fsPhotoShadowAccumF.glsl
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

// FSRenderPhotoShadows (fsphotoshadows.cpp): blends this frame's sun shadow (the light map's red
// channel, after the blur) into the history of the frames before, each
// drawn with the light from another point of the sun's disc. Output, the next history:
//  r: the blended sun shadow, g: frames blended (1 when it started over), b: the view depth (m)

/*[EXTRA_CODE_HERE]*/

out vec4 frag_color;

uniform sampler2D lightMap;     // this frame's light map
uniform sampler2D diffuseRect;  // the history so far
// x: 1 when the history is valid, y: most frames blended, z: change of view depth (share of it)
// that starts a pixel over, w: how far outside this frame's neighbourhood the history may be
uniform vec4 fs_photo_params;

in vec2 vary_fragcoord;

float getDepth(vec2 pos_screen);
bool isFarDepth(float d);
vec4 getPositionWithDepth(vec2 pos_screen, float depth);

void main()
{
    ivec2 px = ivec2(gl_FragCoord.xy);
    ivec2 hi = textureSize(lightMap, 0) - 1;
    float cur = texelFetch(lightMap, px, 0).r;

    vec2 tc = vary_fragcoord.xy;
    float d = getDepth(tc);
    if (isFarDepth(d))
    {
        frag_color = vec4(cur, 0.0, 0.0, 0.0); // the sky: no history
        return;
    }
    float z = -getPositionWithDepth(tc, d).z;

    // What this frame shows two pixels around: a history far outside it is a shadow that moved
    // across a still surface. The sun shadow is already blurred over a dozen pixels, so a frame's
    // own shift within the disc moves it by much less than that margin.
    float lo = cur;
    float hi_v = cur;
    float n0 = texelFetch(lightMap, clamp(px + ivec2( 2,  0), ivec2(0), hi), 0).r;
    float n1 = texelFetch(lightMap, clamp(px + ivec2(-2,  0), ivec2(0), hi), 0).r;
    float n2 = texelFetch(lightMap, clamp(px + ivec2( 0,  2), ivec2(0), hi), 0).r;
    float n3 = texelFetch(lightMap, clamp(px + ivec2( 0, -2), ivec2(0), hi), 0).r;
    lo = min(lo, min(min(n0, n1), min(n2, n3)));
    hi_v = max(hi_v, max(max(n0, n1), max(n2, n3)));

    vec4 h = texelFetch(diffuseRect, px, 0);
    // every test is false for NaN, so a NaN history starts over too
    bool keep = fs_photo_params.x > 0.5 && h.g >= 1.0 &&
                h.r >= 0.0 && h.r <= 1.0 &&
                abs(h.b - z) <= z * fs_photo_params.z + 0.005 &&
                h.r >= lo - fs_photo_params.w && h.r <= hi_v + fs_photo_params.w;
    float n = keep ? min(h.g + 1.0, max(fs_photo_params.y, 1.0)) : 1.0;
    float blended = keep ? mix(h.r, cur, 1.0 / n) : cur;
    frag_color = vec4(clamp(blended, 0.0, 1.0), n, z, 0.0);
}
