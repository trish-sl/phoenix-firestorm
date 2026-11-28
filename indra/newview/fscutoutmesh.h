/**
 * @file fscutoutmesh.h
 * @brief Paper cutout: traces the outline of a snapshot with a transparent background and builds the cutout mesh
 *
 * $LicenseInfo:firstyear=2026:license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, Trish
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

#ifndef FS_CUTOUTMESH_H
#define FS_CUTOUTMESH_H

#include "stdtypes.h"
#include "v2math.h"
#include "v3color.h"
#include "v3math.h"

#include <vector>

// Pure logic (llcommon and llmath only) so it can be unit tested.
namespace FSCutout
{
    typedef std::vector<LLVector2> Polygon;   // pixel coords, origin top-left, closed implicitly, CCW in y-up sense

    struct TraceParams
    {
        U8  alpha_threshold = 128;
        F32 margin_px = 0.f;            // dilation radius
        F32 simplify_px = 2.f;          // Douglas-Peucker tolerance
        S32 smooth_passes = 2;          // Chaikin passes before simplify
        F32 min_island_frac = 0.002f;   // islands below this fraction of image area are dropped
        bool cut_holes = true;          // enclosed gaps become holes; false fills them (one solid piece)
        F32 min_hole_frac = 0.0005f;    // holes below this fraction of image area are filled
    };

    // One piece of cardboard: outer outline CCW (polygonArea > 0), holes CW (polygonArea < 0), each hole strictly
    // inside the outer outline and apart from the other holes.
    struct Outline
    {
        Polygon outer;
        std::vector<Polygon> holes;
    };

    // One outline per island. rgba is width*height*4 bytes, top row first.
    std::vector<Outline> traceOutlines(const U8* rgba, S32 width, S32 height, const TraceParams& p);
    F32  polygonArea(const Polygon& poly);          // signed, positive = CCW (y-up)
    bool polygonSelfIntersects(const Polygon& poly);

    enum EFace { FACE_FRONT = 0, FACE_BACK = 1, FACE_EDGE = 2, FACE_COUNT = 3 };
    struct Mesh
    {
        // One vertex stream per face (no sharing between faces: hard edges, separate UVs).
        std::vector<LLVector3> positions[FACE_COUNT];
        std::vector<LLVector3> normals[FACE_COUNT];
        std::vector<LLVector2> uvs[FACE_COUNT];
        std::vector<U32>       indices[FACE_COUNT];
    };
    // glTF axes: X right, Y up, Z toward the viewer (front). Height maps image height to height_m.
    // Edge UVs repeat every edge_tile_m in both directions: u along the outline, v from front (0) to back.
    // Each hole gets its own wall, normals facing into the hole.
    Mesh buildMesh(const std::vector<Outline>& outlines, S32 img_w, S32 img_h,
                   F32 height_m, F32 thickness_m, F32 edge_tile_m);

    struct Rect { S32 x, y, w, h; };
    // false when components != 4, or no pixel is >= threshold, or no pixel is < threshold
    bool hasUsableAlpha(const U8* data, S32 width, S32 height, S32 components, U8 threshold = 128);
    // bounding box of alpha >= threshold, grown by margin_px, clamped to the image; {0,0,0,0} if none
    Rect alphaBounds(const U8* rgba, S32 width, S32 height, U8 threshold, S32 margin_px);
    // straight-alpha composite over paper (0..255 per channel); rgb_out sized width*height*3
    void flattenOnto(const U8* rgba, S32 width, S32 height, const LLColor3& paper, std::vector<U8>& rgb_out);
    // Upload size: long side at most max_side (SL max 2048), then each side rounded UP to a power of two
    // (at most max_side) so the uploader never shrinks a side. UVs run 0..1, so the stretch still lines up.
    void uploadTextureSize(S32 width, S32 height, S32 max_side, S32& out_w, S32& out_h);
}

#endif // FS_CUTOUTMESH_H
