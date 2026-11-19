/**
 * @file fsparticlerender.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fsparticlerender.h"

#include "lldrawable.h"
#include "llglslshader.h"
#include "llspatialpartition.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerpartsim.h"
#include "llvopartgroup.h"
#include "pipeline.h"

#include <algorithm>
#include <unordered_set>

extern bool gCubeSnapshot;

namespace
{
    // Programs that hold a non-zero fs_particle. Only the legacy alpha program draws particles, the
    // fullbright one has no uniform; FSRenderCheapAvatarAlpha adds the rigged, material and PBR
    // programs (and the GPU-driven copies), so this holds a handful of entries at most.
    std::vector<LLGLSLShader*> sFlagged;

    // A zone per particle group per pass: a scene full of emitters would otherwise take the zone
    // budget of FSGPUTimers (MAX_ZONES_PER_FRAME) from the passes after the alpha pool
    constexpr U32 MAX_ZONES_PER_FRAME = 256;
    U32 sZonesThisFrame = 0;

    bool sStatsOn = false;
    std::unordered_set<const LLSpatialGroup*> sCountedGroups;   // this frame, for the coverage

    // summed over the interval
    U32 sFrames = 0;
    U64 sDraws = 0;
    U64 sCheapDraws = 0;
    U64 sEarlyOutDraws = 0;
    U64 sTriangles = 0;
    U64 sGroups = 0;
    U64 sParticles = 0;
    F64 sCoveredPixels = 0.0;   // particle quads' screen area, overlaps counted once per quad
    F64 sScreenPixels = 0.0;
    U64 sUntimedGroups = 0;

    void log_stats()
    {
        if (!sFrames)
        {
            return;
        }
        const F64 per_frame = 1.0 / (F64)sFrames;
        const F64 layers = sScreenPixels > 0.0 ? sCoveredPixels / sScreenPixels : 0.0;
        LL_INFOS("FSParticles") << "Particles per frame over " << sFrames << " frames: "
                                << llformat("%.1f groups, %.1f draws (%.1f cheap, %.1f with the early-out), %.0f triangles, %.0f particles; ",
                                            (F64)sGroups * per_frame, (F64)sDraws * per_frame, (F64)sCheapDraws * per_frame,
                                            (F64)sEarlyOutDraws * per_frame, (F64)sTriangles * per_frame, (F64)sParticles * per_frame)
                                << llformat("covering about %.2f Mpixels, %.1f times the screen (each quad's area in view, summed); ",
                                            sCoveredPixels * per_frame / 1.0e6, layers)
                                << "GPU time: zone \"alpha particles\" in the FSGPUTimings line"
                                << (sUntimedGroups ? llformat(" (%.1f groups per frame past the zone cap, untimed)", (F64)sUntimedGroups * per_frame) : std::string())
                                << LL_ENDL;
        sFrames = 0;
        sDraws = sCheapDraws = sEarlyOutDraws = sTriangles = sGroups = sParticles = sUntimedGroups = 0;
        sCoveredPixels = sScreenPixels = 0.0;
    }
}

S32 FSParticleRender::flags(bool may_early_out)
{
    static LLCachedControl<bool> cheap(gSavedSettings, "FSRenderCheapParticles", false);
    return (may_early_out ? EARLY_OUT : 0) | (cheap ? CHEAP : 0);
}

void FSParticleRender::apply(LLGLSLShader& shader, S32 flags)
{
    auto it = std::find(sFlagged.begin(), sFlagged.end(), &shader);
    const bool flagged = it != sFlagged.end();
    if (!flags && !flagged)
    {
        return; // holds 0 already
    }

    static LLStaticHashedString name("fs_particle");
    if (shader.getUniformLocation(name) < 0)
    {
        return; // not the legacy alpha program
    }
    shader.uniform1i(name, flags); // cached: no GL call when the value stays
    if (flags && !flagged)
    {
        sFlagged.push_back(&shader);
    }
    else if (!flags && flagged)
    {
        sFlagged.erase(it);
    }
}

void FSParticleRender::resetAll()
{
    if (sFlagged.empty())
    {
        return;
    }
    static LLStaticHashedString name("fs_particle");
    LLGLSLShader* bound = LLGLSLShader::sCurBoundShaderPtr;
    for (LLGLSLShader* shader : sFlagged)
    {
        if (!shader->mProgramObject)
        {
            continue; // unloaded since (a particle flag can be older than this pass); a new program holds 0
        }
        shader->bind();
        if (LLGLSLShader::sCurBoundShaderPtr == shader)
        {
            shader->uniform1i(name, 0);
        }
    }
    sFlagged.clear();
    if (bound && LLGLSLShader::sCurBoundShaderPtr != bound)
    {
        bound->bind();
    }
}

void FSParticleRender::frame(bool enabled, F32 interval_seconds)
{
    static LLTimer timer;
    sZonesThisFrame = 0;
    sCountedGroups.clear();
    if (enabled && sStatsOn)
    {
        ++sFrames;
    }
    if (enabled != sStatsOn)
    {
        sStatsOn = enabled;
        timer.reset();
        log_stats(); // what was gathered so far, or nothing
        sFrames = 0;
        return;
    }
    if (enabled && timer.getElapsedTimeF32() >= llmax(interval_seconds, 1.f))
    {
        timer.reset();
        log_stats();
    }
}

bool FSParticleRender::openZone()
{
    if (!LLFSGPUZone::sActive.load(std::memory_order_relaxed))
    {
        return false;
    }
    if (sZonesThisFrame >= MAX_ZONES_PER_FRAME)
    {
        if (sStatsOn)
        {
            ++sUntimedGroups;
        }
        return false;
    }
    ++sZonesThisFrame;
    return true;
}

void FSParticleRender::noteDraw(S32 flags, U32 indices)
{
    if (!sStatsOn)
    {
        return;
    }
    ++sDraws;
    sCheapDraws += (flags & CHEAP) ? 1 : 0;
    sEarlyOutDraws += (flags & EARLY_OUT) ? 1 : 0;
    sTriangles += indices / 3;
}

void FSParticleRender::noteGroup(LLSpatialGroup* group)
{
    // the main view only, once per frame (the pre and post water pools and the order independent
    // transparency phases each draw a group again)
    if (!sStatsOn || !group || gCubeSnapshot || LLPipeline::sReflectionRender || LLPipeline::sImpostorRender ||
        LLPipeline::sRenderingHUDs || gPipeline.mRT != &gPipeline.mMainRT || !sCountedGroups.insert(group).second)
    {
        return;
    }

    const LLViewerCamera& camera = *LLViewerCamera::getInstance();
    const F32 height = (F32)camera.getViewHeightInPixels();
    const F32 width = height * camera.getAspect();
    const F64 screen = (F64)width * (F64)height;
    const F32 px_per_m = height / (2.f * tanf(llmax(camera.getView(), 0.01f) * 0.5f)); // at 1 m
    const LLVector3& origin = camera.getOrigin();
    const LLVector3& at = camera.getAtAxis();
    const F32 near_clip = llmax(camera.getNear(), 0.1f);

    ++sGroups;
    if (sCountedGroups.size() == 1)
    {
        sScreenPixels += screen; // once per frame that has particles; the ratio is of those frames
    }

    for (LLSpatialGroup::element_iter i = group->getDataBegin(); i != group->getDataEnd(); ++i)
    {
        LLDrawable* drawable = (LLDrawable*)(*i)->getDrawable();
        LLViewerObject* vobj = drawable ? drawable->getVObj().get() : nullptr;
        if (!vobj || vobj->isDead() || vobj->getPCode() != LLViewerObject::LL_VO_PART_GROUP)
        {
            continue;
        }
        LLViewerPartGroup* parts = static_cast<LLVOPartGroup*>(vobj)->getViewerPartGroup();
        if (!parts)
        {
            continue;
        }
        for (const LLViewerPart* part : parts->mParticles)
        {
            if (!part)
            {
                continue;
            }
            const F32 radius = 0.5f * llmax(part->mScale.mV[VX], part->mScale.mV[VY]);
            const F32 depth = (part->mPosAgent - origin) * at;
            if (depth + radius <= 0.f || !camera.sphereInFrustum(part->mPosAgent, radius))
            {
                continue;
            }
            ++sParticles;
            const F32 d = llmax(depth, near_clip);
            const F64 area = (F64)(part->mScale.mV[VX] * part->mScale.mV[VY]) * (F64)(px_per_m / d) * (F64)(px_per_m / d);
            sCoveredPixels += llmin(area, screen);
        }
    }
}
