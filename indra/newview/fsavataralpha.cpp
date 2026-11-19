/**
 * @file fsavataralpha.cpp
 * @brief Cheap lighting for the blended alpha of distant avatars (FSRenderCheapAvatarAlpha), and the share
 *        of avatar alpha in the GPU timings log
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

#include "fsavataralpha.h"

#include "fsparticlerender.h"
#include "llagentcamera.h"
#include "lldrawable.h"
#include "llspatialpartition.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llvoavatar.h"
#include "pipeline.h"

#include <unordered_map>

extern bool gCubeSnapshot;

namespace
{
    // Each avatar's flags, worked out at its first alpha draw of the frame (the camera to avatar distance
    // once per avatar per frame, as the probe, mirror and main passes all ask). Cleared when the frame changes;
    // avatars are only deleted outside rendering, so a pointer can't come back as another avatar meanwhile.
    std::unordered_map<const LLVOAvatar*, S32> sCache;
    U32 sCacheFrame = 0;
    const LLVOAvatar* sLastAvatar = nullptr;    // consecutive draws are mostly one avatar's
    S32 sLastFlags = 0;

    bool sFlaggedThisPass = false;

    // The rigged passes are a zone each; the non-rigged attachments are interleaved with the rest of the
    // alpha, so their groups are, up to this many a frame (FSGPUTimers times 1024 zones a frame in all,
    // particles take up to 256)
    constexpr U32 MAX_ZONES_PER_FRAME = 256;
    U32 sZonesThisFrame = 0;

    bool sStatsOn = false;
    std::unordered_map<const LLVOAvatar*, bool> sSeen;  // this frame: avatars with alpha drawn, cheaply or not

    // summed over the interval
    U32 sFrames = 0;
    U64 sDraws = 0;
    U64 sCheapDraws = 0;
    U64 sAvatars = 0;
    U64 sCheapAvatars = 0;
    U64 sUntimedGroups = 0;

    bool main_view()
    {
        return !gCubeSnapshot && !LLPipeline::sReflectionRender && !LLPipeline::sImpostorRender &&
            !LLPipeline::sRenderingHUDs && gPipeline.mRT == &gPipeline.mMainRT;
    }

    void end_frame()
    {
        for (const auto& seen : sSeen)
        {
            ++sAvatars;
            sCheapAvatars += seen.second ? 1 : 0;
        }
        sSeen.clear();
    }

    void log_stats()
    {
        if (!sFrames)
        {
            return;
        }
        static LLCachedControl<bool> cheap(gSavedSettings, "FSRenderCheapAvatarAlpha", false);
        static LLCachedControl<F32> distance(gSavedSettings, "FSRenderCheapAvatarAlphaDistance", 10.f);
        const F64 per_frame = 1.0 / (F64)sFrames;
        LL_INFOS("FSAvatarAlpha") << "Avatar alpha per frame over " << sFrames << " frames: "
                                  << llformat("%.1f draws (%.1f cheap, %.1f full), %.1f avatars (%.1f cheap, %.1f full); ",
                                              (F64)sDraws * per_frame, (F64)sCheapDraws * per_frame, (F64)(sDraws - sCheapDraws) * per_frame,
                                              (F64)sAvatars * per_frame, (F64)sCheapAvatars * per_frame, (F64)(sAvatars - sCheapAvatars) * per_frame)
                                  << (cheap ? llformat("FSRenderCheapAvatarAlpha on, beyond %.1f m; ", llclamp((F32)distance, 2.f, 64.f))
                                            : std::string("FSRenderCheapAvatarAlpha off; "))
                                  << "GPU time: zone \"alpha avatars\" in the FSGPUTimings line"
                                  << (sUntimedGroups ? llformat(" (%.1f attachment groups per frame past the zone cap, untimed)", (F64)sUntimedGroups * per_frame) : std::string())
                                  << LL_ENDL;
        sFrames = 0;
        sDraws = sCheapDraws = sAvatars = sCheapAvatars = sUntimedGroups = 0;
    }
}

bool FSAvatarAlpha::beginPass()
{
    static LLCachedControl<bool> cheap(gSavedSettings, "FSRenderCheapAvatarAlpha", false);
    sFlaggedThisPass = false;
    // An impostor bake is lit flat by its programs (FOR_IMPOSTOR) anyway
    return cheap && !LLPipeline::sRenderingHUDs && !LLPipeline::sImpostorRender;
}

const LLVOAvatar* FSAvatarAlpha::groupAvatar(LLSpatialGroup* group)
{
    LLSpatialPartition* part = group ? group->getSpatialPartition() : nullptr;
    if (!part || part->mPartitionType != LLViewerRegion::PARTITION_AVATAR)
    {
        return nullptr;
    }
    LLSpatialBridge* bridge = part->asBridge();
    LLDrawable* root = bridge ? bridge->mDrawable : nullptr;     // the attachment's root
    LLViewerObject* vobj = root ? root->getVObj().get() : nullptr;
    if (!vobj || vobj->isDead())
    {
        return nullptr;
    }
    const LLVOAvatar* avatar = vobj->getAvatar();
    return (avatar && !avatar->isDead()) ? avatar : nullptr;
}

S32 FSAvatarAlpha::flags(const LLVOAvatar* avatar)
{
    if (!avatar)
    {
        return 0;
    }
    const U32 frame = LLFrameTimer::getFrameCount();
    if (frame != sCacheFrame)
    {
        sCache.clear();
        sCacheFrame = frame;
        sLastAvatar = nullptr;
    }
    if (avatar != sLastAvatar)
    {
        auto it = sCache.find(avatar);
        if (it != sCache.end())
        {
            sLastFlags = it->second;
        }
        else
        {
            S32 flags = 0;
            if (!avatar->isSelf() && !avatar->isControlAvatar() && !avatar->isDead())
            {
                static LLCachedControl<F32> distance_setting(gSavedSettings, "FSRenderCheapAvatarAlphaDistance", 10.f);
                const F32 distance = llclamp((F32)distance_setting, 2.f, 64.f);
                const F32 dist_sq = dist_vec_squared(avatar->getPositionAgent(), gAgentCamera.getCameraPositionAgent());
                flags = dist_sq > distance * distance ? FSParticleRender::CHEAP : 0;
            }
            sCache.emplace(avatar, flags);
            sLastFlags = flags;
        }
        sLastAvatar = avatar;
    }
    sFlaggedThisPass = sFlaggedThisPass || sLastFlags != 0;
    return sLastFlags;
}

bool FSAvatarAlpha::endPass()
{
    const bool flagged = sFlaggedThisPass;
    sFlaggedThisPass = false;
    return flagged;
}

bool FSAvatarAlpha::statsOn()
{
    return sStatsOn;
}

void FSAvatarAlpha::noteDraw(const LLVOAvatar* avatar, S32 flags)
{
    if (!sStatsOn || !avatar || avatar->isControlAvatar() || !main_view())
    {
        return; // animesh is never lit cheaply, and left out of the counts
    }
    ++sDraws;
    sCheapDraws += flags ? 1 : 0;
    sSeen.emplace(avatar, flags != 0);
}

bool FSAvatarAlpha::openZone()
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

void FSAvatarAlpha::frame(bool enabled, F32 interval_seconds)
{
    static LLTimer timer;
    sZonesThisFrame = 0;
    if (enabled && sStatsOn)
    {
        end_frame();
        ++sFrames;
    }
    sSeen.clear();
    if (enabled != sStatsOn)
    {
        sStatsOn = enabled;
        timer.reset();
        log_stats(); // what was gathered so far, or nothing
        sFrames = 0;
        sDraws = sCheapDraws = sAvatars = sCheapAvatars = sUntimedGroups = 0;
        return;
    }
    if (enabled && timer.getElapsedTimeF32() >= llmax(interval_seconds, 1.f))
    {
        timer.reset();
        log_stats();
    }
}
