/**
 * @file fsphotoshadows.h
 * @brief Photo shadows: sun shadows blended over many frames while the camera is still
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

#ifndef FS_PHOTO_SHADOWS_H
#define FS_PHOTO_SHADOWS_H

#include "stdtypes.h"
#include "v3math.h"

#include <glm/mat4x4.hpp>

class LLGLSLShader;
class LLRenderTarget;
class LLVertexBuffer;

extern LLGLSLShader gFSPhotoShadowAccumProgram;   // fsPhotoShadowAccumF.glsl
extern LLGLSLShader gFSPhotoShadowApplyProgram;   // fsPhotoShadowApplyF.glsl

// FSRenderPhotoShadows: sun shadows of the quality a path tracer would give a still picture, by
// blending many frames, while the camera is still and in snapshots.
//
// The sun is a disc half a degree across, not a point, which is what makes shadows sharp where an
// object touches the ground and softer the further they fall. One shadow map can only give the
// shadow of a point light, softened by a filter (PCF, PCSS) that guesses the penumbra, plus the
// stair steps of its texels. Here, every frame the near sun cascades (0 and 1, those around the
// camera) are drawn with the light coming from another point of the sun's disc (a Halton sequence
// over the disc) and with the shadow map shifted by a fraction of a texel. The sun shadow each
// pixel ends up with (the light map's sun channel, after the blur) is
// blended with the frames before it in a history of its own, weighted 1/n up to
// FSRenderPhotoShadowsFrames, and written back before the lighting pass reads it. The average of
// those hard shadows converges to the disc's real shadow: contact hard, widening with distance, and
// free of texel steps.
//
// The blend starts over:
//  - for the whole picture when the camera moves, turns or zooms (its unjittered matrices), the
//    picture size changes, on a teleport or region crossing, and when a snapshot tile starts;
//  - per pixel where its view depth changed (something moved there) or where the history is far
//    outside what this frame shows around the pixel (a shadow moved across a still surface).
// The first frame after the camera stops uses the sun's centre, so nothing changes until frames
// are blended.
//
// Snapshots draw FSRenderPhotoShadowsSnapshotFrames frames more per tile before the one they read
// (display() doesn't run the world's idle updates, so the scene is frozen between them), each tile
// starting over. Avatars only snapshots don't blend.
//
// The far cascades (2, 3) keep their light direction and texel grid: the static shadow cache only
// covers those, so it keeps working. PCSS keeps its filter; with it on, the penumbrae come out a
// little wider than the disc's.
//
// Main thread only. With the setting off, or the camera moving, nothing is jittered and nothing is
// drawn; the two RGBA16F history targets exist only while the setting is on and it has blended.
namespace FSPhotoShadows
{
    // display(), once a frame before the sun shadow maps (after FSTAA::beginFrame): whether this
    // frame blends, and its point of the sun's disc
    void beginFrame(const LLRenderTarget& screen);
    bool frameActive();

    // generateSunShadow: the light direction of the cascade (toward the ground), moved within the
    // sun's disc for cascades 0 and 1 while blending; unchanged otherwise
    LLVector3 jitterLightDir(U32 cascade, const LLVector3& light_dir);
    // ...and its projection moved by a fraction of a shadow map texel
    void jitterProjection(U32 cascade, glm::mat4& proj, F32 map_width, F32 map_height);

    // renderDeferredLighting, main camera, once the light map's sun channel is final: blends it
    // into the history and writes the blend back. depth_source owns the scene depth.
    void accumulate(LLRenderTarget& light_map, LLRenderTarget& depth_source, LLVertexBuffer* screen_triangle);

    // rawSnapshot: the frames to draw before each tile's read, and the start of a tile
    U32 snapshotExtraFrames();
    void beginSnapshotTile();

    // The render targets go (resize, shader reload, shutdown).
    void release();
}

#endif // FS_PHOTO_SHADOWS_H
