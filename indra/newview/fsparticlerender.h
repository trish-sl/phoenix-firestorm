/**
 * @file fsparticlerender.h
 * @brief Particle draws in the alpha pass: cheap lighting (FSRenderCheapParticles), the early-out of
 *        faded fragments, and their share of the frame in the GPU timings log
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
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

#ifndef FS_PARTICLERENDER_H
#define FS_PARTICLERENDER_H

#include "stdtypes.h"

class LLGLSLShader;
class LLSpatialGroup;

// In-world particles are drawn by the alpha pool (LLDrawPoolAlpha::renderAlpha) with the lit legacy
// alpha program (class2/deferred/alphaF.glsl), each fragment paying for the sun shadow, the walk over
// the reflection probes and seven local lights. Thick fog stacks dozens of large quads on a pixel.
// The alpha pool tells that program per draw what it may skip for a particle (uniform fs_particle,
// 0 for every other draw, so nothing else changes):
// - EARLY_OUT: the draw blends by source alpha and writes no depth, so a fragment the soft particle
//   fade leaves below the minimum alpha is discarded before the lighting (the picture is the same:
//   the stock minimum alpha test already drops such fragments when their texture alpha is that low);
// - CHEAP: FSRenderCheapParticles, lit from the sky probe's ambient, one shadow tap and the four
//   nearest lights. FSRenderCheapAvatarAlpha (fsavataralpha.h) sets it for distant avatars' alpha too,
//   on the same uniform, which class2 pbralphaF and class3 materialF also have for that.
// With FSRenderGPUTimings on, each in-world particle group's draws (and glow) are a GPU zone
// "alpha particles", and a line next to the timings gives the draws and the screen they cover.
// Main thread only, like the rest of the render pipeline.
namespace FSParticleRender
{
    enum : S32
    {
        EARLY_OUT = 1,
        CHEAP = 2
    };

    // The flags of a draw of an in-world particle group; may_early_out: source alpha blending,
    // colour pass, no depth written
    S32 flags(bool may_early_out);

    // Before a draw: the flags (0 for anything that isn't a particle). Programs without the uniform
    // are left alone; a program is only touched again once it holds a non-zero value.
    void apply(LLGLSLShader& shader, S32 flags);

    // Every program that holds a non-zero value back to 0 (binding each, then the program bound before
    // again), so a flag set for the alpha pool's draws never reaches whatever else draws with the program
    // later (FSRenderCheapAvatarAlpha flags the rigged and PBR programs too, which other passes use)
    void resetAll();

    // Main thread, every frame (llappviewer.cpp, with FSGPUTimers): stats on or off, logged every interval
    void frame(bool enabled, F32 interval_seconds);

    // A particle group is about to be drawn: whether to open its GPU zone (timings on, and under the
    // per frame cap that keeps FSGPUTimers' zone budget for the rest of the frame)
    bool openZone();

    // Stats, no-ops while they are off: a particle draw, and the group once per frame for its coverage
    void noteDraw(S32 flags, U32 indices);
    void noteGroup(LLSpatialGroup* group);
}

#endif // FS_PARTICLERENDER_H
