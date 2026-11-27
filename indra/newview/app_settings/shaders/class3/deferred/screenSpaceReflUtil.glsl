/**
 * @file class3/deferred/screenSpaceReflUtil.glsl
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

// Based on https://imanolfotia.com/blog/1

uniform sampler2D sceneMap;
uniform sampler2D sceneDepth;

uniform vec2 screen_res;
uniform mat4 projection_matrix;
uniform mat4 inv_proj;
uniform mat4 modelview_delta;
uniform mat4 inv_modelview_delta;

uniform int iterationCount;
uniform float maxThickness;
uniform float depthBias;
uniform float glossySampleCount;
uniform float noiseSine;
uniform float maxZDepth;
uniform float maxRoughness;
uniform vec2 ssrJitterOffset;
uniform int hizMipCount;
uniform int ssrPrecise; // <FS:Trish/> FSRenderSSRPrecise
// <FS:Trish> GCP hit tests: surface thickness as a fraction of view depth (FSRenderSSRThicknessFraction,
// capped by maxThickness, which stays in metres), and how much a hit's pixels may be magnified in the
// reflection before it fades (RenderScreenSpaceReflectionMaxStretch)
uniform float thicknessFraction;
uniform float maxStretch;
// </FS:Trish>

vec4 getPositionWithDepth(vec2 pos_screen, float depth);

// <FS:Trish> FSRenderReverseZ: the trace below works in the standard depth convention (0 near,
// 1 far, as the care package's Hi-Z pyramid holds it). Under REVERSE_Z the depth buffers hold
// 1 at the near plane and 0 at the far one, and hiZReduceF keeps the largest (nearest) depth;
// 1 - d turns either into the standard convention and back again.
float ssrStdDepth(float d)
{
#ifdef REVERSE_Z
    return 1.0 - d;
#else
    return d;
#endif
}
// </FS:Trish>

float random(vec2 uv)
{
    return fract(sin(dot(uv, vec2(12.9898, 78.233))) * 43758.5453123);
}

vec2 generateProjectedPosition(vec3 pos)
{
    vec4 samplePosition = projection_matrix * vec4(pos, 1.0);
    samplePosition.xy = (samplePosition.xy / samplePosition.w) * 0.5 + 0.5;
    // Compensate for SMAA T2x jitter difference between current and previous frame.
    // mSceneMap was rendered with the previous frame's jittered projection;
    // offset the UV so depth/color lookups hit the correct texels.
    samplePosition.xy += ssrJitterOffset;
    return samplePosition.xy;
}

float getLinearDepth(vec2 tc)
{
    // Force LOD 0: sceneDepth now carries the Hi-Z mip chain,
    // and reflected UVs have chaotic derivatives that could cause
    // auto-LOD selection to sample coarse Hi-Z levels.
    float depth = textureLod(sceneDepth, tc, 0).r;
    vec4 pos = getPositionWithDepth(tc, depth);
    return -pos.z;
}

float projectDepth(vec3 viewPos)
{
    vec4 clip = projection_matrix * vec4(viewPos, 1.0);
    // <FS:Trish> FSRenderReverseZ: projection_matrix then yields the stored depth itself
    //return (clip.z / clip.w) * 0.5 + 0.5;
#ifdef REVERSE_Z
    return ssrStdDepth(clip.z / clip.w);
#else
    return (clip.z / clip.w) * 0.5 + 0.5;
#endif
    // </FS:Trish>
}

// <FS:Trish> GCP: the thickness a surface is assumed to have at a view depth. A fixed thickness was too
// thick close up, where rays passing just behind a pole or a railing hit it and drew a copy of it, and too
// thin far away, where depth precision runs out. A fraction of the distance, capped by maxThickness.
float ssrThickness(float viewZ)
{
    if (thicknessFraction <= 0.0)
        return maxThickness;
    return min(maxThickness, max(thicknessFraction * abs(viewZ), 1e-3));
}
// </FS:Trish>

// <FS:Trish> Validate candidates while marching, so rejecting a foreground surface does not terminate
// the entire reflection ray. Depth and normals belong to the scene map's camera space.
// crossed: the ray went behind a nearer surface in front of this one and came out behind this one,
// so it passed through it where the screen cannot show (FSRenderSSRPrecise); no thickness test.
// overshoot: how far behind the surface the ray went at the hit cell, in thickness allowances (GCP)
float ssrHitConfidence(vec3 hit, vec3 viewRay, bool crossed, float overshoot)
{
    if (any(lessThan(hit.xy, vec2(0.0))) || any(greaterThanEqual(hit.xy, vec2(1.0))))
        return 0.0;

    ivec2 maxPixel = ivec2(screen_res) - 1;
    ivec2 px = clamp(ivec2(hit.xy * screen_res), ivec2(0), maxPixel);
    float depth = texelFetch(sceneDepth, px, 0).r;
    if (ssrStdDepth(depth) >= 1.0)
        return 0.0;

    vec3 surface = getPositionWithDepth(hit.xy, depth).xyz;
    if (-surface.z > maxZDepth)
        return 0.0;
    vec3 ray = getPositionWithDepth(hit.xy, ssrStdDepth(hit.z)).xyz;
    // <FS:Trish> GCP: the end depth error is ramped against the depth relative thickness and the overshoot
    // fades the hit from one allowance to two, so near misses around silhouettes fade instead of dropping
    // out in blocks under the old hard cut
    //float confidence = 1.0 - smoothstep(0.0, maxThickness, length(surface - ray));
    //confidence *= confidence;
    //if (crossed)
    //    confidence = 1.0;
    //if (confidence < 0.95)
    //    return 0.0;
    float confidence = 1.0;
    if (!crossed)
    {
        float endThickness = ssrThickness(surface.z);
        float endError = length(surface - ray);
        if (endError > endThickness)
            return 0.0;
        confidence = (1.0 - smoothstep(0.1 * endThickness, endThickness, endError)) *
                     (1.0 - smoothstep(1.0, 2.0, overshoot));
        if (confidence < 0.01)
            return 0.0;
    }
    // </FS:Trish>

    // All five samples are at pixel centres. Using the subpixel ray hit as the centre
    // skewed these derivatives as the ray crossed the pixel, especially at silhouettes.
    vec3 centre = getPositionWithDepth((vec2(px) + 0.5) / screen_res, depth).xyz;
    ivec2 pR = min(px + ivec2(1, 0), maxPixel);
    ivec2 pL = max(px - ivec2(1, 0), ivec2(0));
    ivec2 pD = min(px + ivec2(0, 1), maxPixel);
    ivec2 pU = max(px - ivec2(0, 1), ivec2(0));
    vec3 posR = getPositionWithDepth((vec2(pR) + 0.5) / screen_res, texelFetch(sceneDepth, pR, 0).r).xyz;
    vec3 posL = getPositionWithDepth((vec2(pL) + 0.5) / screen_res, texelFetch(sceneDepth, pL, 0).r).xyz;
    vec3 posD = getPositionWithDepth((vec2(pD) + 0.5) / screen_res, texelFetch(sceneDepth, pD, 0).r).xyz;
    vec3 posU = getPositionWithDepth((vec2(pU) + 0.5) / screen_res, texelFetch(sceneDepth, pU, 0).r).xyz;
    vec3 dx = abs(posR.z - centre.z) < abs(centre.z - posL.z) ? posR - centre : centre - posL;
    vec3 dy = abs(posD.z - centre.z) < abs(centre.z - posU.z) ? posD - centre : centre - posU;
    vec3 normal = cross(dx, dy);
    if (normal.z < 0.0) normal = -normal;
    return dot(viewRay, normal) < 0.0 ? confidence : 0.0;
}
// </FS:Trish>

// Hi-Z hierarchical screen-space ray trace (Godot parametric-T approach).
// origin/dir are in UV+depth space: (u, v, rawDepth) where depth is [0,1].
// Returns hit position (uv + raw depth) or vec3(-1) on miss.
//
// Uses parametric T along the ray for all comparisons, avoiding the AMD
// absolute-depth comparison that breaks for camera-facing rays (dir.z < 0).
// Z is normalized so dir.z = +/-1, making depth_t = (cellDepth - origin.z) / dir.z
// trivially comparable with the edge_t from cell boundary intersections.
//
// Our Hi-Z stores MIN depth (standard GL: 0=near, 1=far), so min = closest.
// Godot uses reversed-Z with MAX depth, but the logic maps cleanly:
//   Godot: max(a,b,c,d) with reversed-Z -> closest surface
//   Ours:  min(a,b,c,d) with standard-Z -> closest surface
// The comparison flips accordingly.
//
// References:
//   Godot Engine SSR: screen_space_reflection.glsl (MIT license)
//   Sugulee/GPU Pro 5: Hi-Z Screen-Space Cone-Traced Reflections
// <FS:Trish> Candidates validated while marching (ssrHitConfidence)
//// Returns vec4: xyz = hit position (UV + raw depth), w = 1.0 on hit.
// Returns vec4: xyz = hit position (UV + raw depth), w = validated hit confidence.
// </FS:Trish>
// Returns vec4(-1) on miss (ray exits screen or runs out of iterations).
// Thickness-rejected surfaces are skipped (mip-0 march) but don't kill the trace.
// <FS:Trish> viewRay: the reflection direction in the scene map's camera space
//vec4 hiZTrace(vec3 origin, vec3 dir, int maxIterations)
vec4 hiZTrace(vec3 origin, vec3 dir, vec3 viewRay, int maxIterations)
// </FS:Trish>
{
    int maxLevel = hizMipCount - 1;

    // Guard near-zero Z direction: treat as Z+ epsilon.
    if (abs(dir.z) < 1e-7)
        dir.z = 1e-7;

    // Normalize direction so |dir.z| = 1.
    // This makes depth_t = (cellDepth - origin.z) * zDir directly comparable
    // with edge_t values along the XY axes.
    // Reference: https://hacksoflife.blogspot.com/2020/10/a-tip-for-hiz-ssr-parametric-t-tracing.html
    vec3 rayDir = dir / abs(dir.z);

    // Standard GL: 0=near, 1=far. Camera-facing rays move toward 0 (dir.z < 0).
    // Godot (reversed-Z): facing_camera = rayDir.z >= 0 (toward near=1.0).
    // For standard GL: facing_camera = dir.z < 0 (toward near=0.0).
    bool facingCamera = dir.z < 0.0;
    float zDir = rayDir.z;  // +/-1 after normalization

    // Cell step direction: +1 or -1 per axis (matches Godot cell_step).
    vec2 cellStep = vec2(rayDir.x < 0.0 ? -1.0 : 1.0,
                         rayDir.y < 0.0 ? -1.0 : 1.0);

    int curLevel = 0;
    float t = 0.0;
    // <FS:Trish>
    float hitConfidence = 0.0;
    // FSRenderSSRPrecise: whether the ray is behind a surface nearer the camera (an avatar standing
    // before a wall, say), and that surface's view depth at the last cell it covered
    bool occluded = false;
    float occluderZ = 0.0;
    // </FS:Trish>

    // Compute t_max: parametric T to the screen edge where we stop tracing.
    vec2 t0 = (vec2(0.0) - origin.xy) / rayDir.xy;
    vec2 t1 = (vec2(1.0) - origin.xy) / rayDir.xy;
    vec2 t2 = max(t0, t1);
    float tMax = min(t2.x, t2.y);

    // Initial advance: push past origin cell to avoid self-intersection.
    // Matches Godot's initial advance exactly.
    {
        vec2 cellIndex = floor(origin.xy * screen_res);
        vec2 newCellIndex = cellIndex + clamp(cellStep, vec2(0.0), vec2(1.0));
        vec2 newCellPos = (newCellIndex / screen_res) + cellStep * 0.000001;
        vec2 posT = (newCellPos - origin.xy) / rayDir.xy;
        t = min(posT.x, posT.y);
    }

    for (int i = 0; i < maxIterations && curLevel >= 0 && t < tMax; i++)
    {
        vec3 pos = origin + rayDir * t;

        // Cell lookup at current mip level.
        vec2 cellCount = vec2(max(1, int(screen_res.x) >> curLevel),
                              max(1, int(screen_res.y) >> curLevel));
        ivec2 cellIndex = ivec2(floor(pos.xy * cellCount));
        cellIndex = clamp(cellIndex, ivec2(0), ivec2(cellCount) - 1);

        // Min depth in this cell (closest surface, standard GL).
        // <FS:Trish> FSRenderReverseZ
        //float cellDepth = texelFetch(sceneDepth, cellIndex, curLevel).r;
        float cellDepth = ssrStdDepth(texelFetch(sceneDepth, cellIndex, curLevel).r);
        // </FS:Trish>

        // Parametric T to the depth surface.
        // Since rayDir.z = +/-1, this is (cellDepth - origin.z) * (+/-1).
        float depthT = (cellDepth - origin.z) * zDir;

        // Parametric T to the nearest cell boundary in XY.
        vec2 newCellIndex = vec2(cellIndex) + clamp(cellStep, vec2(0.0), vec2(1.0));
        vec2 newCellPos = (newCellIndex / cellCount) + cellStep * 0.000001;
        vec2 posT = (newCellPos - origin.xy) / rayDir.xy;
        float edgeT = min(posT.x, posT.y);

        // Hit detection (matches Godot exactly):
        // Forward rays: hit if depth surface is reached before cell boundary.
        // Camera-facing rays: hit if ray hasn't traveled past the depth surface.
        bool hit = facingCamera ? (t <= depthT) : (depthT <= edgeT);

        int mipOffset = hit ? -1 : 1;

        // <FS:Trish> FSRenderSSRPrecise: in front of everything in this cell, so no longer behind anything
        bool crossed = false;
        if (!hit)
            occluded = false;
        float overshoot = 0.0; // GCP: behind the surface at this cell, in thickness allowances
        // </FS:Trish>

        // Thickness gate at mip 0 (matches Godot depth_tolerance check):
        // Linearize depths and reject if surface is too far behind ray.
        if (curLevel == 0 && hit)
        {
            // <FS:Trish> FSRenderReverseZ: back to the stored convention
            //float z0 = getPositionWithDepth(pos.xy, cellDepth).z;
            //float z1 = getPositionWithDepth(pos.xy, pos.z).z;
            float z0 = getPositionWithDepth(pos.xy, ssrStdDepth(cellDepth)).z;
            float z1 = getPositionWithDepth(pos.xy, ssrStdDepth(pos.z)).z;
            // </FS:Trish>
            // <FS:Trish> FSRenderSSRPrecise: a ray that passes behind a nearer surface and comes out
            // behind a farther one went through the farther one somewhere the nearer one hides. That
            // is a hit on the farther surface, taken where it shows again, rather than a miss that
            // showed the probes in the outline of the nearer surface: a hole in the shape of an avatar
            // behind its reflection.
            //if ((z0 - z1) > maxThickness)
            //{
            //    hit = false;
            //    mipOffset = 0;  // Stay at mip 0, march cell-by-cell.
            //}
            // GCP: the thickness grows with the view depth, and a grazing ray, which moves further in depth
            // across one cell than the surface is thick, may have crossed it within that step. Full weight
            // within one allowance, faded by two, passed behind it beyond that.
            vec3 exitPos = origin + rayDir * edgeT;
            float rayStepZ = abs(getPositionWithDepth(exitPos.xy, ssrStdDepth(exitPos.z)).z - z1);
            float thickness = max(ssrThickness(z0), rayStepZ);
            overshoot = max(z0 - z1, 0.0) / max(thickness, 1e-4);
            if (overshoot > 2.0)
            {
                if (ssrPrecise != 0 && occluded && z0 < occluderZ - max(thickness, 0.5))
                {
                    crossed = true;
                }
                else
                {
                    hit = false;
                    mipOffset = 0;  // Stay at mip 0, march cell-by-cell.
                    occluded = true;
                    occluderZ = z0;
                }
            }
            // </FS:Trish>
        }

        // <FS:Trish>
        if (curLevel == 0 && hit)
        {
            float candidateT = facingCamera ? t : max(t, depthT);
            hitConfidence = ssrHitConfidence(origin + rayDir * candidateT, viewRay, crossed, overshoot);
            if (hitConfidence <= 0.0)
            {
                // The ray may pass behind a camera-visible foreground surface without
                // intersecting its front. Keep searching instead of projecting its silhouette
                // into the SSR miss mask. Reuse the same iteration budget; no unbounded retries.
                hit = false;
                mipOffset = 0;
            }
        }
        // </FS:Trish>

        // Advance parametric T (matches Godot exactly):
        // Only advance to depthT for non-facing-camera hits.
        // For facing-camera hits, descend without advancing.
        if (hit)
        {
            if (!facingCamera)
                t = max(t, depthT);
        }
        else
        {
            t = edgeT;
        }

        curLevel = min(curLevel + mipOffset, maxLevel);
    }

    vec3 hitPos = origin + rayDir * t;

    // Final bounds check.
    // <FS:Trish> Out of iterations before descending past mip 0 is a miss too: the position was
    // only on a coarse cell's boundary, and returning it as a hit drew the reflection as blocks
    // of that cell's size, with staircase edges, and picked up colours from the wrong surfaces
    //if (hitPos.x < 0.0 || hitPos.x > 1.0 ||
    //    hitPos.y < 0.0 || hitPos.y > 1.0 ||
    //    t >= tMax)
    if (curLevel >= 0 ||
        hitPos.x < 0.0 || hitPos.x > 1.0 ||
        hitPos.y < 0.0 || hitPos.y > 1.0 ||
        t >= tMax)
    // </FS:Trish>
        return vec4(-1.0);

    // <FS:Trish>
    //return vec4(hitPos, 1.0);
    return vec4(hitPos, hitConfidence);
    // </FS:Trish>
}

float calculateEdgeFade(vec2 screenPos)
{
    vec2 distFromCenter = abs(screenPos * 2.0 - 1.0);
    vec2 fade = smoothstep(0.85, 1.0, distFromCenter);
    return 1.0 - max(fade.x, fade.y);
}

float tapScreenSpaceReflection(
    int totalSamples,
    vec2 tc,
    vec3 viewPos,
    vec3 n,
    inout vec4 collectedColor,
    sampler2D source,
    float glossiness)
{
    float roughness = 1.0 - glossiness;

    if (roughness >= maxRoughness)
        return 0.0;

    vec3 viewDir = normalize(viewPos);
    vec3 normal = normalize(n);

    float viewDotNormal = dot(-viewDir, normal);
    if (viewDotNormal <= 0.0)
    {
        collectedColor = vec4(0.0);
        return 0.0;
    }

    vec2 distFromCenter = abs(tc * 2.0 - 1.0);
    float baseEdgeFade = 1.0 - smoothstep(0.85, 1.0, max(distFromCenter.x, distFromCenter.y));
    // <FS:Trish> Alchemy's fade by how directly the camera looks at the reflecting surface. Seen head
    // on, a surface reflects what stands between it and the camera, and the screen only holds that
    // from the camera's side: glass showed the back of an avatar standing before it, dark where its
    // front should be, and a copy of whatever stood there, sliding as the camera turned. Reflections
    // fade out from about 70 degrees off the surface normal to 45, to the probes.
    baseEdgeFade *= clamp((dot(viewDir, normal) * 0.5 + 0.5) * 5.5 - 0.8, 0.0, 1.0);
    // </FS:Trish>
    if (baseEdgeFade <= 0.001)
    {
        collectedColor = vec4(0.0);
        return 0.0;
    }

    // <FS:Trish> GCP: the reflected ray starts on the surface itself; any offset along the normal shifted the
    // whole reflection (5 cm and more here, a gap under everything standing on a reflecting floor). The
    // self-hit test below and the trace's first cell skip keep it off its own surface.
    //// Bias the ray origin along the normal, scaled by distance.
    //// Prevents grazing-angle rays from scraping the originating surface
    //// at distance where depth precision breaks down.
    //float biasAmount = max(0.01, -viewPos.z * depthBias);
    //vec3 biasedPos = viewPos - normal * biasAmount;
    //
    //vec3 transformedPos = (inv_modelview_delta * vec4(biasedPos, 1.0)).xyz;
    vec3 transformedPos = (inv_modelview_delta * vec4(viewPos, 1.0)).xyz;
    // </FS:Trish>
    float startDepth = -transformedPos.z;

    if (startDepth > maxZDepth)
    {
        collectedColor = vec4(0.0);
        return 0.0;
    }

    vec3 perfectReflDir = normalize(reflect(viewDir, normal));

    // <FS:Trish> One ray along the mirror direction. The care package scatters its rays per pixel by
    // roughness and lets its temporal anti-aliasing average the noise away; without it, neighbouring
    // pixels hit and missed in turn and the reflection broke into speckled patches of the scene (with
    // its shadows) and of the brighter probes, whose borders slid with the camera. The roughness blur
    // comes from the SSR buffer's mips instead (filterSSRBuffer and the lighting pass's lookup).
    //int numSamples = max(1, int(glossySampleCount));
    const int numSamples = 1;
    // </FS:Trish>
    vec3 accumColor = vec3(0.0);
    float accumFade = 0.0;
    int hits = 0;

    for (int s = 0; s < numSamples; s++)
    {
        vec3 reflectDir = perfectReflDir;

        // Jitter reflection direction based on roughness (importance-sampled GGX)
        // <FS:Trish> Not jittered (see numSamples)
        //if (roughness > 0.001)
        if (false)
        // </FS:Trish>
        {
            float alpha = roughness * roughness;
            float u1 = random(tc * screen_res + noiseSine + float(s) * 0.123);
            float u2 = random(tc * screen_res * 1.7 + noiseSine + float(s) * 0.456 + 0.5);

            float theta = atan(alpha * sqrt(u1) / sqrt(1.0 - u1));
            float phi = 2.0 * 3.14159265 * u2;

            vec3 up = abs(reflectDir.y) < 0.999 ? vec3(0, 1, 0) : vec3(1, 0, 0);
            vec3 tangent = normalize(cross(up, reflectDir));
            vec3 bitangent = cross(reflectDir, tangent);

            vec3 h = normalize(
                sin(theta) * cos(phi) * tangent +
                sin(theta) * sin(phi) * bitangent +
                cos(theta) * reflectDir
            );

            reflectDir = normalize(reflect(-reflectDir, h));
        }

        // <FS:Trish> Transform a direction, not an endpoint relative to the biased origin. Subtracting
        // transformedPos added normal * biasAmount to the ray, bending the reflection as the
        // camera depth changed. Origin clearance must not change the mirror direction.
        //vec3 reflTarget = viewPos + reflectDir;
        //vec3 transformedTarget = (inv_modelview_delta * vec4(reflTarget, 1.0)).xyz;
        //vec3 transformedReflDir = normalize(transformedTarget - transformedPos);
        vec3 transformedReflDir = normalize(mat3(inv_modelview_delta) * reflectDir);
        // </FS:Trish>

        if (transformedReflDir.z >= 0.5)
            continue;

        // Fade rays pointing back toward camera to avoid sharp SSR boundary.
        float cameraFacingFade = 1.0 - smoothstep(0.45, 0.5, transformedReflDir.z);

        // <FS:Trish> GCP: only a nudge off the surface against reprojection noise, scaled with the depth and
        // small enough to shift the reflection by about a pixel at most
        //// Push ray origin along surface normal to prevent self-intersection.
        //// Deterministic (not random) to avoid per-pixel noise.
        //// Scales with distance to match depth-buffer precision degradation.
        //float clearance = max(0.05, -viewPos.z * 0.002);
        //vec3 clearedPos = biasedPos + normal * clearance;
        //vec3 transformedClearedPos = (inv_modelview_delta * vec4(clearedPos, 1.0)).xyz;
        float nudge = max(0.001, -viewPos.z * 0.001);
        vec3 transformedClearedPos = (inv_modelview_delta * vec4(viewPos + normal * nudge, 1.0)).xyz;
        // </FS:Trish>

        // Project ray origin and a nearby point to screen space (UV + raw depth).
        // Use a short step (fraction of distance-to-camera) to ensure the end
        // point stays in front of the camera: projecting at maxZDepth can wrap
        // behind the camera for reflections going toward the viewer.
        vec3 ssOrigin = vec3(generateProjectedPosition(transformedClearedPos),
                             projectDepth(transformedClearedPos));
        float stepDist = max(0.1, -transformedClearedPos.z * 0.1);
        vec3 transformedEnd = transformedClearedPos + transformedReflDir * stepDist;
        vec3 ssFar = vec3(generateProjectedPosition(transformedEnd),
                          projectDepth(transformedEnd));
        vec3 ssDir = ssFar - ssOrigin;

        // <FS:Trish> GCP: the trace reads the scene map's depth; under camera motion the reprojected start
        // can land a hair behind that surface and read as an immediate self-hit, so its depth is pulled onto
        // the surface (the direction is unchanged). Standard depth convention: smaller is nearer.
        ivec2 originPixel = clamp(ivec2(ssOrigin.xy * screen_res), ivec2(0), ivec2(screen_res) - 1);
        ssOrigin.z = min(ssOrigin.z, ssrStdDepth(texelFetch(sceneDepth, originPixel, 0).r));
        // </FS:Trish>

        // <FS:Trish>
        //vec4 result = hiZTrace(ssOrigin, ssDir, iterationCount);
        vec4 result = hiZTrace(ssOrigin, ssDir, transformedReflDir, iterationCount);
        // </FS:Trish>

        if (result.x < 0.0)
            continue;

        vec2 hitTC = result.xy;

        // Read actual surface depth at the hit pixel (mip 0, exact pixel).
        ivec2 hitPixel = ivec2(hitTC * screen_res);
        hitPixel = clamp(hitPixel, ivec2(0), ivec2(screen_res) - 1);
        float hitSurfaceDepth = texelFetch(sceneDepth, hitPixel, 0).r;

        // Reject sky / far-plane hits (forward-Z: 1.0 = far).
        float hitDepth = -getPositionWithDepth(hitTC, hitSurfaceDepth).z;
        if (hitDepth > maxZDepth)
            continue;

        // <FS:Trish> The march already validated depth, confidence and facing before accepting this hit
        // (ssrHitConfidence), for every hit rather than only rays under 3 pixels
        //// Confidence validation (matches Godot):
        //// Compare the trace's final depth with the actual surface depth at that pixel.
        //// No dead zone: smoothstep from 0 penalizes any mismatch.
        //// Squared for strong near-hits with rapid falloff.
        //vec3 viewSpaceSurface = getPositionWithDepth(hitTC, hitSurfaceDepth).xyz;
        //vec3 viewSpaceHit = getPositionWithDepth(hitTC, ssrStdDepth(result.z)).xyz;
        //float hitDistance = length(viewSpaceSurface - viewSpaceHit);
        //float confidence = 1.0 - smoothstep(0.0, maxThickness, hitDistance);
        //confidence *= confidence;
        //
        //if (confidence < 0.95)
        //    continue;
        //
        //// Short-ray back-face check (Godot-style):
        //// For rays that traveled < 3 pixels, compute a geometric normal at the
        //// hit point from depth buffer cross-derivatives. Reject if the reflected
        //// ray exits the surface (dot >= 0), which indicates self-intersection.
        //float rayLen = length(result.xy - ssOrigin.xy);
        //float rayPixelLen = rayLen * max(screen_res.x, screen_res.y);
        //if (rayPixelLen < 3.0)
        //{
        //    float dR = texelFetch(sceneDepth, hitPixel + ivec2(1, 0), 0).r;
        //    float dD = texelFetch(sceneDepth, hitPixel + ivec2(0, 1), 0).r;
        //    vec3 posR = getPositionWithDepth((vec2(hitPixel) + vec2(1.5, 0.5)) / screen_res, dR).xyz;
        //    vec3 posD = getPositionWithDepth((vec2(hitPixel) + vec2(0.5, 1.5)) / screen_res, dD).xyz;
        //    vec3 hitGeomNormal = cross(posR - viewSpaceSurface, posD - viewSpaceSurface);
        //    // Ensure normal faces toward camera (positive Z in view space).
        //    if (hitGeomNormal.z < 0.0) hitGeomNormal = -hitGeomNormal;
        //    hitGeomNormal = normalize(hitGeomNormal);
        //
        //    if (dot(reflectDir, hitGeomNormal) >= 0.0)
        //        continue;
        //}
        float confidence = result.w;
        float rayLen = length(result.xy - ssOrigin.xy);
        // </FS:Trish>

        // <FS:Trish> GCP hit tests, in the scene map's camera space
        vec3 viewSpaceSurface = getPositionWithDepth(hitTC, hitSurfaceDepth).xyz;

        // self-hit: a hit lying in the origin surface's own plane is that surface finding itself (the ray no
        // longer starts away from it)
        vec3 originNormal = normalize(mat3(inv_modelview_delta) * normal);
        if (dot(viewSpaceSurface - transformedPos, originNormal) < max(0.005, -transformedPos.z * 0.002))
            continue;

        // the hit pixel's four neighbours
        ivec2 maxPixel = ivec2(screen_res) - 1;
        ivec2 pxL = clamp(hitPixel - ivec2(1, 0), ivec2(0), maxPixel);
        ivec2 pxR = clamp(hitPixel + ivec2(1, 0), ivec2(0), maxPixel);
        ivec2 pxD = clamp(hitPixel - ivec2(0, 1), ivec2(0), maxPixel);
        ivec2 pxU = clamp(hitPixel + ivec2(0, 1), ivec2(0), maxPixel);
        vec3 nbL = getPositionWithDepth((vec2(pxL) + 0.5) / screen_res, texelFetch(sceneDepth, pxL, 0).r).xyz;
        vec3 nbR = getPositionWithDepth((vec2(pxR) + 0.5) / screen_res, texelFetch(sceneDepth, pxR, 0).r).xyz;
        vec3 nbD = getPositionWithDepth((vec2(pxD) + 0.5) / screen_res, texelFetch(sceneDepth, pxD, 0).r).xyz;
        vec3 nbU = getPositionWithDepth((vec2(pxU) + 0.5) / screen_res, texelFetch(sceneDepth, pxU, 0).r).xyz;

        // fringe: the second difference of 1/z is zero on any plane at any slope and rises where the pixel
        // straddles an edge, whose colour is half the object and half what lies behind it
        float edgeFraction = thicknessFraction > 0.0 ? thicknessFraction : 0.02;
        float invC = 1.0 / max(-viewSpaceSurface.z, 1e-4);
        float bendX = abs(1.0 / max(-nbL.z, 1e-4) + 1.0 / max(-nbR.z, 1e-4) - 2.0 * invC);
        float bendY = abs(1.0 / max(-nbD.z, 1e-4) + 1.0 / max(-nbU.z, 1e-4) - 2.0 * invC);
        float fringe = 1.0 - smoothstep(edgeFraction, 2.0 * edgeFraction, max(bendX, bendY) / invC);

        // skim: a ray meeting the surface near edge on reads a sliver of it
        vec3 hitSurfaceNormal = normalize(cross(nbR - nbL, nbU - nbD));
        float skim = smoothstep(0.05, 0.2, abs(dot(transformedReflDir, hitSurfaceNormal)));

        // stretch: the captured texel's footprint over the reflected ray cone's at the hit (same pixel angle).
        // A face the camera barely sees, which the reflecting surface looks straight at, would smear a few
        // pixels across the whole reflection.
        float camDist = length(viewSpaceSurface);
        float coneDist = length(transformedPos) + length(viewSpaceSurface - transformedPos);
        float camFacing = max(abs(dot(viewSpaceSurface / max(camDist, 1e-4), hitSurfaceNormal)), 1e-3);
        float stretch = (camDist * abs(dot(transformedReflDir, hitSurfaceNormal))) / max(coneDist * camFacing, 1e-4);
        float stretchFade = maxStretch > 0.0 ? 1.0 - smoothstep(maxStretch, 2.0 * maxStretch, stretch) : 1.0;

        confidence *= fringe * skim * stretchFade;
        if (confidence <= 0.001)
            continue;
        // </FS:Trish>

        float edgeFade = calculateEdgeFade(hitTC);

        // <FS:Trish> GCP: hits that used most of the screen room in their direction fade gradually, so the
        // coverage approaches the screen exit softly instead of stopping on a line
        vec2 roomDir = normalize(ssDir.xy + vec2(1e-7));
        vec2 roomT = max((vec2(0.0) - ssOrigin.xy) / roomDir, (vec2(1.0) - ssOrigin.xy) / roomDir);
        float roomLen = max(min(roomT.x, roomT.y), 1e-4);
        edgeFade *= 1.0 - smoothstep(0.5, 1.0, rayLen / roomLen);
        // </FS:Trish>

        float zFadeStart = maxZDepth * 0.8;
        float zFade = 1.0 - smoothstep(zFadeStart, maxZDepth, hitDepth);

        vec4 sampledColor = textureLod(source, hitTC, 0.0);

        // Tone map hit color (Reinhard on luminance) to compress brights
        // before mip chain averaging: prevents fireflies.
        float luma = dot(sampledColor.rgb, vec3(0.2126, 0.7152, 0.0722));
        sampledColor.rgb /= (1.0 + luma);

        // Fade-in: suppress near-origin artifacts from self-intersection.
        float fadeIn = smoothstep(0.0, 0.01, rayLen);
        float rayFade = 1.0 - smoothstep(0.6, 1.0, rayLen);

        float sampleFade = edgeFade * zFade * fadeIn * rayFade * confidence * cameraFacingFade;

        accumColor += sampledColor.rgb * sampleFade;
        accumFade += sampleFade;
        hits++;
    }

    if (hits == 0)
    {
        collectedColor = vec4(0.0);
        return 0.0;
    }

    accumColor /= float(numSamples);
    accumFade /= float(numSamples);

    float combinedFade = accumFade * baseEdgeFade;

    collectedColor = vec4(accumColor * baseEdgeFade, combinedFade);
    return 1.0;
}
