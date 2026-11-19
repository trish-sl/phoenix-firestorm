/**
 * @file fsgddcommon.cpp
 * @brief The GPU-driven passes' upload ring (FSPerfGPUDrivenUploadRing) and shared matrix tables
 * (FSPerfGPUDrivenSharedMatrices)
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

#include "fsgddcommon.h"

#include "fsglstream.h"
#include "llspatialpartition.h"
#include "llviewercontrol.h"
#include "llviewertexture.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>

LLTrace::BlockTimerStatHandle FTM_FS_GDD_BUILD("FS GDD Build");
LLTrace::BlockTimerStatHandle FTM_FS_GDD_UPLOAD("FS GDD Upload");
LLTrace::BlockTimerStatHandle FTM_FS_GDD_BUILD_SHADOW("FS GDD Build Shadow");
LLTrace::BlockTimerStatHandle FTM_FS_GDD_BUILD_SHADOW_ALPHA("FS GDD Build Shadow Alpha");
LLTrace::BlockTimerStatHandle FTM_FS_GDD_BUILD_MATERIALS("FS GDD Build Materials");
LLTrace::BlockTimerStatHandle FTM_FS_GDD_BUILD_LEGACY("FS GDD Build Legacy");
LLTrace::BlockTimerStatHandle FTM_FS_GDD_BUILD_PBR("FS GDD Build PBR");
LLTrace::BlockTimerStatHandle FTM_FS_GDD_BUILD_ALPHA("FS GDD Build Alpha");
LLTrace::BlockTimerStatHandle FTM_FS_GDD_COLLECT_SHADOW_ALPHA("FS GDD Collect Shadow Alpha");

U64 gFSGDDStatModelsComputed = 0;
U64 gFSGDDStatModelsFound = 0;

U64* gFSGDDStageTicks = nullptr;
U64 gFSGDDStageLast = 0;

//-----------------------------------------------------------------------------
// The build statistics (FSGDDBuildScope, fsGDDLap)

namespace
{
    struct BuilderStats
    {
        U64 mTicks = 0;                         // every timed pass
        U64 mPasses = 0;
        U64 mBatches = 0;
        U64 mSplitTicks = 0;                    // the passes split into stages
        U64 mStageTicks[FS_GDD_STAGE_COUNT] = {};
        U64 mReusedTicks = 0;                   // FSPerfGPUDrivenBuildCache: the passes a kept pass was used for
        U64 mReusedPasses = 0;
        U64 mReusedBatches = 0;
        U32 mSplitCounter = 0;
    };

    // Every builder's cache (FSGDDBuildCache registers itself); never destroyed
    std::vector<FSGDDBuildCache*>& build_caches()
    {
        static std::vector<FSGDDBuildCache*>* caches = new std::vector<FSGDDBuildCache*>();
        return *caches;
    }

    BuilderStats sBuilders[FS_GDD_BUILDER_COUNT];
    U64 sUploadTicks = 0;   // FSGDDBuffer writes, while the statistics are on
    bool sBuildStatsOn = false; // this frame's answer of build_stats_wanted
    bool sBuildStatsRestart = true; // just turned on: the next line starts counting from here

    const char* builder_name(U32 builder)
    {
        static const char* names[FS_GDD_BUILDER_COUNT] = { "shadow", "shadow alpha", "shadow alpha collect", "materials",
                                                           "legacy", "PBR", "alpha" };
        return builder < FS_GDD_BUILDER_COUNT ? names[builder] : "?";
    }

    const char* stage_name(U32 stage)
    {
        static const char* names[FS_GDD_STAGE_COUNT] = { "walk", "arena", "textures", "matrices", "records", "cache" };
        return stage < FS_GDD_STAGE_COUNT ? names[stage] : "?";
    }

    // Any of the GPU-driven statistics or verify settings: the build statistics come with theirs
    bool build_stats_wanted()
    {
        static LLCachedControl<bool> materials(gSavedSettings, "FSPerfGPUDrivenMaterialsStats", false);
        static LLCachedControl<bool> legacy(gSavedSettings, "FSPerfGPUDrivenLegacyStats", false);
        static LLCachedControl<bool> pbr(gSavedSettings, "FSPerfGPUDrivenPBRStats", false);
        static LLCachedControl<bool> shadow(gSavedSettings, "FSPerfShadowBatchStats", false);
        static LLCachedControl<bool> draws_verify(gSavedSettings, "FSPerfGPUDrivenDrawsVerify", false);
        static LLCachedControl<bool> materials_verify(gSavedSettings, "FSPerfGPUDrivenMaterialsVerify", false);
        static LLCachedControl<bool> legacy_verify(gSavedSettings, "FSPerfGPUDrivenLegacyVerify", false);
        static LLCachedControl<bool> pbr_verify(gSavedSettings, "FSPerfGPUDrivenPBRVerify", false);
        return materials || legacy || pbr || shadow || draws_verify || materials_verify || legacy_verify || pbr_verify;
    }

    void log_build_stats()
    {
        static LLFrameTimer timer;
        static U32 s_first_frame = 0;
        if (sBuildStatsRestart)
        {   // what the builders counted before was dropped when the statistics went off
            sBuildStatsRestart = false;
            timer.reset();
            s_first_frame = LLFrameTimer::getFrameCount();
        }
        if (timer.getElapsedTimeF32() < 10.f)
        {
            return;
        }
        timer.reset();
        const U32 frame = LLFrameTimer::getFrameCount();
        const U32 frames = llmax(frame - s_first_frame, 1u);
        s_first_frame = frame;
        const F64 ms_per_tick = 1000.0 / (F64)llmax(LLTrace::BlockTimer::countsPerSecond(), (U64)1);

        std::ostringstream out;
        out << std::fixed;
        out.precision(2);
        out << "GPU-driven build, last 10 s (" << frames << " frames), ms a frame:";
        F64 total_ms = 0.0;
        for (U32 b = 0; b < FS_GDD_BUILDER_COUNT; ++b)
        {
            BuilderStats& s = sBuilders[b];
            if (!s.mPasses)
            {
                continue;
            }
            const F64 ms = (F64)s.mTicks * ms_per_tick / frames;
            if (b != FS_GDD_BUILDER_SHADOW_ALPHA_COLLECT)
            {
                total_ms += ms;
            }
            out << " " << builder_name(b) << " " << ms << " (" << (s.mPasses / frames) << " passes, "
                << (s.mBatches / frames) << " batches a frame";
            if (s.mBatches)
            {
                out.precision(3);
                out << ", " << ((F64)s.mTicks * ms_per_tick * 1000.0 / (F64)s.mBatches) << " us a batch";
                out.precision(2);
            }
            if (s.mSplitTicks)
            {
                out << "; split:";
                for (U32 st = 0; st < FS_GDD_STAGE_COUNT; ++st)
                {
                    if (s.mStageTicks[st])
                    {
                        out << " " << stage_name(st) << " " << (U32)(100 * s.mStageTicks[st] / s.mSplitTicks) << "%";
                    }
                }
            }
            out << ");";
        }
        out << " all but the collect " << total_ms << "; uploads " << ((F64)sUploadTicks * ms_per_tick / frames);
        sUploadTicks = 0;

        // FSPerfGPUDrivenBuildCache: how often a kept pass stood in for a build, and the time that saved: the
        // batches it served at the builder's cost per batch of the passes it built, less the time the checks took
        static LLCachedControl<bool> cache_on(gSavedSettings, "FSPerfGPUDrivenBuildCache", false);
        bool cache_counted = false;
        for (FSGDDBuildCache* cache : build_caches())
        {
            U64 hits = 0;
            U64 kept = 0;
            U64 unkept = 0;
            cache->takeCounts(hits, kept, unkept);
            if (!cache_on && !hits && !kept && !unkept)
            {
                continue;
            }
            const BuilderStats& s = sBuilders[cache->builder()];
            out << (cache_counted ? "; " : "; build cache: ") << builder_name(cache->builder()) << " ";
            cache_counted = true;
            const U64 passes = hits + kept + unkept;
            out << hits << " of " << passes << " passes used a kept one ("
                << (passes ? (U32)(100 * hits / passes) : 0) << "%), " << kept << " built and kept, " << unkept
                << " built and not kept";
            const U64 built_ticks = s.mTicks - s.mReusedTicks;
            const U64 built_batches = s.mBatches - s.mReusedBatches;
            if (s.mReusedPasses && built_batches)
            {
                const F64 saved = ((F64)s.mReusedBatches * (F64)built_ticks / (F64)built_batches - (F64)s.mReusedTicks) *
                                  ms_per_tick / frames;
                out << ", the kept ones took " << ((F64)s.mReusedTicks * ms_per_tick / frames) << " ms a frame, about "
                    << saved << " ms a frame saved";
            }
        }
        for (BuilderStats& s : sBuilders)
        {
            const U32 counter = s.mSplitCounter;
            s = BuilderStats();
            s.mSplitCounter = counter;
        }
        LL_INFOS("FSGPUDrivenBuild") << out.str() << LL_ENDL;
    }
}

FSGDDBuildScope::FSGDDBuildScope(EFSGDDBuilder builder)
{
    static U32 s_frame = U32_MAX;
    const U32 frame = LLFrameTimer::getFrameCount();
    if (frame != s_frame)
    {   // the settings once a frame
        s_frame = frame;
        const bool was_on = sBuildStatsOn;
        sBuildStatsOn = build_stats_wanted();
        if (sBuildStatsOn && !was_on)
        {
            for (BuilderStats& s : sBuilders)
            {
                s = BuilderStats();
            }
            sUploadTicks = 0;
            sBuildStatsRestart = true;
        }
    }
    if (!sBuildStatsOn || builder >= FS_GDD_BUILDER_COUNT || gFSGDDStageTicks)
    {
        return;
    }
    mBuilder = builder;
    BuilderStats& s = sBuilders[builder];
    mSplit = (++s.mSplitCounter & 7) == 0;
    mStart = LLTrace::BlockTimer::getCPUClockCount64();
    if (mSplit)
    {
        gFSGDDStageTicks = s.mStageTicks;
        gFSGDDStageLast = mStart;
    }
}

FSGDDBuildScope::~FSGDDBuildScope()
{
    if (mBuilder >= FS_GDD_BUILDER_COUNT)
    {
        return;
    }
    const U64 end = LLTrace::BlockTimer::getCPUClockCount64();
    BuilderStats& s = sBuilders[mBuilder];
    s.mTicks += end - mStart;
    if (!mCheckOnly)
    {   // a check that found nothing to use: its time is the builder's, the pass is counted with the build after it
        ++s.mPasses;
    }
    s.mBatches += mBatches;
    if (mReused)
    {
        s.mReusedTicks += end - mStart;
        ++s.mReusedPasses;
        s.mReusedBatches += mBatches;
    }
    if (mSplit)
    {   // what came after the last lap
        s.mStageTicks[mCheckOnly ? FS_GDD_STAGE_CACHE : FS_GDD_STAGE_WALK] += end - gFSGDDStageLast;
        s.mSplitTicks += end - mStart;
        gFSGDDStageTicks = nullptr;
    }
    log_build_stats();
}

//-----------------------------------------------------------------------------
// The upload ring. Slots used one per frame in turn, each a buffer the frame's uploads take consecutive slices
// of. A slot is written again RING_SLOTS_PERSISTENT or RING_SLOTS_SUB_DATA frames later:
//  - persistent (OpenGL 4.4, the main thread makes the GL calls): the buffers are mapped once, coherent, and
//    written with memcpy; a fence after each frame's commands, waited on (rarely: three frames is far) before the
//    slot is written again.
//  - sub data (the render thread replays the GL calls, or no glBufferStorage): glBufferSubData into the slice. The
//    data is copied into the command stream when recorded and lands in order. With the render thread a frame
//    behind and the driver's queue behind that, the GPU could still be reading a slot three frames on, and a
//    driver then stalls in glBufferSubData or copies the buffer aside (AMD). So there are five slots, and a fence
//    after each frame's commands that the GL thread tests without waiting (in stream order, like FSGLRetire): a
//    slot whose fence isn't known to have passed when it comes round gets new storage for its buffer (orphaned
//    with glBufferData), which the driver gives without waiting for the GPU. The main thread only reads what the
//    GL thread found, so nothing waits for the render thread.
// A slot that runs out of room mid frame gets a buffer twice the size; the old one stays (its slices are still read
// by the frame's draws, and may still be written by writeAt) until the slot comes round again.
namespace
{
    constexpr U32 RING_SLOTS_PERSISTENT = 3;
    constexpr U32 RING_SLOTS_SUB_DATA = 5;
    constexpr U32 RING_SLOTS_MAX = 5;
    constexpr size_t RING_FIRST_BYTES = 4u << 20;
    constexpr size_t RING_MAX_BYTES = 256u << 20;

    enum ERingMode
    {
        RING_NONE,
        RING_PERSISTENT,
        RING_SUB_DATA
    };

    struct RingBuffer
    {
        GLuint mName = 0;
        U8* mMapped = nullptr;
    };

    struct RingSlot
    {
        RingBuffer mBuffer;
        size_t mCapacity = 0;
        size_t mUsed = 0;
        GLsync mFence = nullptr;
        U32 mSerial = 0;                    // sub data: the GL thread's fence after the slot's frame, 0 none
        std::vector<RingBuffer> mRetired;   // outgrown: deleted when the slot comes round again
    };

    struct RingSlice
    {
        GLuint mName = 0;
        size_t mOffset = 0;
        size_t mSize = 0;
        U8* mMapped = nullptr;
    };

    RingSlot sSlots[RING_SLOTS_MAX];
    U32 sSlot = 0;
    U32 sRingFrame = 0;
    U32 sRingGeneration = 1;            // changes at each release, so slices of a released ring are never written
    bool sRingStarted = false;
    ERingMode sRingMode = RING_NONE;
    bool sPersistentFailed = false;     // a mapping failed: sub data from then on
    size_t sAlign = 256;

    // statistics, every 10 seconds while either setting is on
    U64 sStatRingSlices = 0;
    U64 sStatRingBytes = 0;
    U64 sStatOwnUploads = 0;
    U64 sStatOwnBytes = 0;
    U64 sStatTailWrites = 0;
    U64 sStatGrowths = 0;
    U64 sStatFenceWaits = 0;
    U64 sStatSlotsBusy = 0;     // sub data: slots the GPU wasn't known to be done with, given new storage

    // Sub data mode's fences. The main thread numbers them; the GL thread (the render thread, or the main
    // thread without it) makes and tests them in stream order and stores the highest one passed.
    U32 sRingNextSerial = 1;
    std::atomic<U32> sRingDone{ 0 };
    std::deque<std::pair<U32, GLsync>> sRingFences;    // GL thread: made, not yet seen passed, oldest first

    // serials only go up; differences keep that true across a wrap
    bool ring_reached(U32 done, U32 serial)
    {
        return (S32)(done - serial) >= 0;
    }

    void ring_store_done(U32 serial)
    {
        U32 current = sRingDone.load(std::memory_order_relaxed);
        while (!ring_reached(current, serial) &&
               !sRingDone.compare_exchange_weak(current, serial, std::memory_order_release, std::memory_order_relaxed))
        {
        }
    }

    // GL thread, after a frame's commands
    void ring_gl_fence(U64 serial)
    {
        if (GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0))
        {
            sRingFences.emplace_back((U32)serial, fence);
        }
    }

    // GL thread: tests the fences oldest first, never waiting
    void ring_gl_poll(U64)
    {
        while (!sRingFences.empty())
        {
            const GLenum status = glClientWaitSync(sRingFences.front().second, 0, 0);
            if (status == GL_TIMEOUT_EXPIRED)
            {
                return;
            }
            if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED)
            {
                ring_store_done(sRingFences.front().first);
            }
            // GL_WAIT_FAILED: a broken fence clears nothing; the slot it covers just gets new storage
            glDeleteSync(sRingFences.front().second);
            sRingFences.pop_front();
        }
    }

    // GL thread: the fences are not needed any more (the ring was released)
    void ring_gl_drop(U64)
    {
        for (auto& fence : sRingFences)
        {
            glDeleteSync(fence.second);
        }
        sRingFences.clear();
    }

    bool ring_fences_supported()
    {
        return glFenceSync != nullptr && glClientWaitSync != nullptr && glDeleteSync != nullptr;
    }

    U32 ring_slots()
    {
        return sRingMode == RING_PERSISTENT ? RING_SLOTS_PERSISTENT : RING_SLOTS_SUB_DATA;
    }

    bool persistent_supported()
    {
        return gGLManager.mGLVersion >= 4.39f &&
               glBufferStorage != nullptr &&
               glMapBufferRange != nullptr &&
               glFenceSync != nullptr &&
               glClientWaitSync != nullptr &&
               glDeleteSync != nullptr;
    }

    // With the render thread recording, a mapped write would land before the commands recorded ahead of it run:
    // the stream's order only holds for GL calls
    ERingMode wanted_mode()
    {
        if (FSGLStream::isRecording() || sPersistentFailed || !persistent_supported())
        {
            return RING_SUB_DATA;
        }
        return RING_PERSISTENT;
    }

    void free_ring_buffer(RingBuffer& buffer)
    {
        if (buffer.mName)
        {
            glDeleteBuffers(1, &buffer.mName);  // unmaps it too
        }
        buffer = RingBuffer();
    }

    void release_ring()
    {
        bool serials = false;
        for (RingSlot& slot : sSlots)
        {
            if (slot.mFence)
            {
                glDeleteSync(slot.mFence);
                slot.mFence = nullptr;
            }
            serials = serials || slot.mSerial;
            slot.mSerial = 0;
            free_ring_buffer(slot.mBuffer);
            for (RingBuffer& retired : slot.mRetired)
            {
                free_ring_buffer(retired);
            }
            slot.mRetired.clear();
            slot.mCapacity = 0;
            slot.mUsed = 0;
        }
        if (serials)
        {
            FSGLStream::recordCall(&ring_gl_drop, 0);
        }
        sSlot = 0;
        sRingStarted = false;
        sRingMode = RING_NONE;
        if (++sRingGeneration == 0)
        {
            sRingGeneration = 1;
        }
    }

    bool create_ring_buffer(RingBuffer& buffer, size_t bytes)
    {
        glGenBuffers(1, &buffer.mName);
        glBindBuffer(GL_COPY_WRITE_BUFFER, buffer.mName);
        if (sRingMode == RING_PERSISTENT)
        {
            const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
            glBufferStorage(GL_COPY_WRITE_BUFFER, (GLsizeiptr)bytes, nullptr, flags);
            buffer.mMapped = (U8*)glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, (GLsizeiptr)bytes, flags);
            glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
            if (!buffer.mMapped)
            {
                LL_WARNS("Rendering") << "FSPerfGPUDrivenUploadRing: a " << (bytes >> 20) << " MB buffer could not be mapped; "
                                      << "the ring writes with glBufferSubData from now on" << LL_ENDL;
                free_ring_buffer(buffer);
                sPersistentFailed = true;
                return false;
            }
            return true;
        }
        glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)bytes, nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
        return true;
    }

    // Waits until the GPU is done with what the fence follows
    void wait_fence(GLsync fence)
    {
        GLenum status = glClientWaitSync(fence, 0, 0);
        if (status == GL_TIMEOUT_EXPIRED)
        {
            ++sStatFenceWaits;
            for (U32 tries = 0; tries < 10 && status == GL_TIMEOUT_EXPIRED; ++tries)
            {
                status = glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 100000000ull);   // 100 ms
            }
        }
        if (status == GL_WAIT_FAILED || status == GL_TIMEOUT_EXPIRED)
        {   // the fence can't tell: make sure the GPU is done before the slot is written again
            glFinish();
        }
    }

    // The first upload of a frame moves to the next slot
    void begin_ring_frame()
    {
        const U32 frame = LLFrameTimer::getFrameCount();
        if (sRingStarted && frame == sRingFrame)
        {
            return;
        }
        if (sRingStarted)
        {
            RingSlot& done = sSlots[sSlot];
            if (sRingMode == RING_PERSISTENT)
            {   // after every command that reads the slot's slices
                if (done.mFence)
                {
                    glDeleteSync(done.mFence);
                }
                done.mFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            }
            else if (ring_fences_supported())
            {   // made and tested by the GL thread in stream order: recorded, never waited for
                done.mSerial = sRingNextSerial++;
                if (sRingNextSerial == 0)
                {
                    sRingNextSerial = 1;
                }
                FSGLStream::recordCall(&ring_gl_fence, done.mSerial);
                FSGLStream::recordCall(&ring_gl_poll, 0);
            }
            sSlot = (sSlot + 1) % ring_slots();
        }
        sRingStarted = true;
        sRingFrame = frame;

        RingSlot& slot = sSlots[sSlot];
        if (slot.mFence)
        {
            wait_fence(slot.mFence);
            glDeleteSync(slot.mFence);
            slot.mFence = nullptr;
        }
        if (slot.mSerial)
        {
            if (slot.mBuffer.mName && !ring_reached(sRingDone.load(std::memory_order_acquire), slot.mSerial))
            {   // the GPU may still read it: new storage under the same name, the old kept by the driver until then
                glBindBuffer(GL_COPY_WRITE_BUFFER, slot.mBuffer.mName);
                glBufferData(GL_COPY_WRITE_BUFFER, (GLsizeiptr)slot.mCapacity, nullptr, GL_DYNAMIC_DRAW);
                glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
                ++sStatSlotsBusy;
            }
            slot.mSerial = 0;
        }
        for (RingBuffer& retired : slot.mRetired)
        {
            free_ring_buffer(retired);
        }
        slot.mRetired.clear();
        slot.mUsed = 0;
    }

    bool ring_alloc(size_t bytes, RingSlice& out)
    {
        const ERingMode mode = wanted_mode();
        if (mode != sRingMode)
        {   // first use, or the render thread started or stopped: the old buffers can't be used the new way
            release_ring();
            sRingMode = mode;
            GLint align = 0;
            glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &align);
            sAlign = llmax((size_t)llmax(align, 16), (size_t)16);
            if (sAlign & (sAlign - 1))
            {   // not a power of two: unheard of, but the rounding below needs one
                sAlign = 256;
            }
        }
        begin_ring_frame();

        const size_t size = (llmax(bytes, (size_t)16) + sAlign - 1) & ~(sAlign - 1);
        RingSlot& slot = sSlots[sSlot];
        size_t offset = (slot.mUsed + sAlign - 1) & ~(sAlign - 1);
        if (!slot.mBuffer.mName || offset + size > slot.mCapacity)
        {
            size_t capacity = llmax(RING_FIRST_BYTES, slot.mCapacity * 2);
            while (capacity < size && capacity <= RING_MAX_BYTES)
            {
                capacity *= 2;
            }
            if (capacity > RING_MAX_BYTES)
            {
                return false;
            }
            RingBuffer grown;
            if (!create_ring_buffer(grown, capacity))
            {
                return false;
            }
            if (slot.mBuffer.mName)
            {
                slot.mRetired.push_back(slot.mBuffer);
                ++sStatGrowths;
            }
            slot.mBuffer = grown;
            slot.mCapacity = capacity;
            offset = 0;
        }
        slot.mUsed = offset + size;
        out.mName = slot.mBuffer.mName;
        out.mOffset = offset;
        out.mSize = size;
        out.mMapped = slot.mBuffer.mMapped ? slot.mBuffer.mMapped + offset : nullptr;
        ++sStatRingSlices;
        sStatRingBytes += bytes;
        return true;
    }

    // A slice from this frame of the ring as it is now, which its buffer still holds
    bool ring_slice_current(U32 frame, U32 generation)
    {
        return sRingStarted && generation == sRingGeneration && frame == sRingFrame;
    }

    void log_stats()
    {
        static LLCachedControl<bool> ring(gSavedSettings, "FSPerfGPUDrivenUploadRing", false);
        static LLCachedControl<bool> shared(gSavedSettings, "FSPerfGPUDrivenSharedMatrices", false);
        static LLFrameTimer timer;
        if ((!ring && !shared) || timer.getElapsedTimeF32() < 10.f)
        {
            return;
        }
        timer.reset();
        size_t ring_bytes = 0;
        for (const RingSlot& slot : sSlots)
        {
            ring_bytes += slot.mCapacity;
        }
        const char* mode = sRingMode == RING_PERSISTENT ? "persistent mapped" : sRingMode == RING_SUB_DATA ? "sub data" : "unused";
        LL_INFOS("FSGPUDrivenUpload") << "GPU-driven uploads, last 10 s: ring (" << mode << ", " << (ring_bytes >> 10) << " KB in "
                                      << ring_slots() << " slots) " << sStatRingSlices << " slices, " << (sStatRingBytes >> 10)
                                      << " KB, " << sStatGrowths << " growths, " << sStatFenceWaits << " fence waits, "
                                      << sStatSlotsBusy << " slots still in use renewed; own buffers "
                                      << sStatOwnUploads << " uploads, " << (sStatOwnBytes >> 10) << " KB; matrix tables: "
                                      << gFSGDDStatModelsComputed << " models computed, " << gFSGDDStatModelsFound
                                      << " found already there, " << sStatTailWrites << " shared table appends" << LL_ENDL;
        sStatRingSlices = sStatRingBytes = sStatOwnUploads = sStatOwnBytes = sStatTailWrites = sStatGrowths = sStatFenceWaits = sStatSlotsBusy = 0;
        gFSGDDStatModelsComputed = gFSGDDStatModelsFound = 0;
    }
}

//-----------------------------------------------------------------------------
// FSGDDBuffer

void FSGDDBuffer::allocate(GLenum target, size_t capacity, const void* data, size_t bytes)
{
    LL_RECORD_BLOCK_TIME(FTM_FS_GDD_UPLOAD);
    log_stats();
    struct UploadClock     // the build statistics' upload time
    {
        U64 mStart = sBuildStatsOn ? LLTrace::BlockTimer::getCPUClockCount64() : 0;
        ~UploadClock()
        {
            if (mStart)
            {
                sUploadTicks += LLTrace::BlockTimer::getCPUClockCount64() - mStart;
            }
        }
    } upload_clock;
    static LLCachedControl<bool> use_ring(gSavedSettings, "FSPerfGPUDrivenUploadRing", false);
    if (use_ring && !mOwnStorage)
    {
        RingSlice slice;
        if (ring_alloc(capacity, slice))
        {
            mName = slice.mName;
            mOffset = slice.mOffset;
            mCapacity = slice.mSize;
            mMapped = slice.mMapped;
            mRingFrame = sRingFrame;
            mRingGeneration = sRingGeneration;
            glBindBuffer(target, mName);
            if (bytes)
            {
                if (mMapped)
                {
                    memcpy(mMapped, data, bytes);
                }
                else
                {
                    glBufferSubData(target, (GLintptr)mOffset, (GLsizeiptr)bytes, data);
                }
            }
            return;
        }
    }

    // the own buffer, new storage: what the previous pass's draws read stays theirs
    if (!mOwn)
    {
        glGenBuffers(1, &mOwn);
    }
    glBindBuffer(target, mOwn);
    const size_t size = llmax(capacity, (size_t)16);
    // a kept pass's buffers are drawn from again over many frames
    const GLenum usage = mOwnStorage ? GL_DYNAMIC_DRAW : GL_STREAM_DRAW;
    if (bytes == capacity)
    {
        glBufferData(target, size, bytes ? data : nullptr, usage);
    }
    else
    {
        glBufferData(target, size, nullptr, usage);
        if (bytes)
        {
            glBufferSubData(target, 0, (GLsizeiptr)bytes, data);
        }
    }
    mName = mOwn;
    mOffset = 0;
    mCapacity = size;
    mMapped = nullptr;
    ++sStatOwnUploads;
    sStatOwnBytes += bytes;
}

bool FSGDDBuffer::writeAt(GLenum target, size_t offset, const void* data, size_t bytes)
{
    if (!mName || offset + bytes > mCapacity)
    {
        return false;
    }
    if (mName != mOwn && !ring_slice_current(mRingFrame, mRingGeneration))
    {
        return false;
    }
    LL_RECORD_BLOCK_TIME(FTM_FS_GDD_UPLOAD);
    const U64 start = sBuildStatsOn ? LLTrace::BlockTimer::getCPUClockCount64() : 0;
    ++sStatTailWrites;
    glBindBuffer(target, mName);
    if (bytes)
    {
        if (mMapped)
        {
            memcpy(mMapped + offset, data, bytes);
        }
        else
        {
            glBufferSubData(target, (GLintptr)(mOffset + offset), (GLsizeiptr)bytes, data);
        }
    }
    if (start)
    {
        sUploadTicks += LLTrace::BlockTimer::getCPUClockCount64() - start;
    }
    return true;
}

void FSGDDBuffer::bindBase(GLenum target, GLuint index) const
{
    if (mName == mOwn)
    {
        glBindBufferBase(target, index, mOwn);
    }
    else
    {
        glBindBufferRange(target, index, mName, (GLintptr)mOffset, (GLsizeiptr)mCapacity);
    }
}

void FSGDDBuffer::release()
{
    if (mOwn)
    {
        glDeleteBuffers(1, &mOwn);
    }
    mOwn = 0;
    mName = 0;
    mOffset = 0;
    mCapacity = 0;
    mMapped = nullptr;
}

//-----------------------------------------------------------------------------
// Shared matrix tables

namespace
{
    constexpr U32 SHARED_TABLES = 8;    // the camera, shadow maps, probe faces, water: the least recently used goes

    struct SharedTable
    {
        FSGDDMatrixStore mStore;
        glm::mat4 mProj = glm::mat4(1.f);
        glm::mat4 mView = glm::mat4(1.f);
        U32 mFrame = 0;
        U32 mCull = 0;
        U64 mLastUse = 0;
        bool mFull = false;
        bool mValid = false;
    };

    SharedTable sTables[SHARED_TABLES];
    U32 sCull = 0;
    U64 sUse = 0;
}

void fsGDDNewCull()
{
    ++sCull;
    fsGDDBuildCachesTick();
}

FSGDDMatrixStore* fsGDDSharedMatrixStore(const glm::mat4& proj, const glm::mat4& view, bool full)
{
    const U32 frame = LLFrameTimer::getFrameCount();
    SharedTable* oldest = &sTables[0];
    for (SharedTable& table : sTables)
    {
        if (table.mValid && table.mFrame == frame && table.mCull == sCull && table.mFull == full &&
            !memcmp(&table.mProj, &proj, sizeof(glm::mat4)) && !memcmp(&table.mView, &view, sizeof(glm::mat4)))
        {
            table.mLastUse = ++sUse;
            return &table.mStore;
        }
        // an unused table, else the least recently used (tables of earlier frames and culls are older than any of now)
        if (oldest->mValid && (!table.mValid || table.mLastUse < oldest->mLastUse))
        {
            oldest = &table;
        }
    }
    // A new table. Its buffer keeps its storage: the next allocate gives it new storage (or a new ring slice),
    // and the draws of its old contents keep reading what they had.
    SharedTable& table = *oldest;
    table.mStore.clear();
    table.mStore.mMatrices.push_back(glm::mat4(1.f));  // texture matrix 0: identity
    table.mProj = proj;
    table.mView = view;
    table.mFrame = frame;
    table.mCull = sCull;
    table.mFull = full;
    table.mValid = true;
    table.mLastUse = ++sUse;
    return &table.mStore;
}

void fsGDDCommonReleaseGL()
{
    fsGDDBuildCachesRelease();
    release_ring();
    for (SharedTable& table : sTables)
    {
        table.mStore.mBuffer.release();
        table.mStore.clear();
        table.mValid = false;
    }
}

//-----------------------------------------------------------------------------
// FSPerfGPUDrivenBuildCache
//
// In a scene that holds still, most passes are given the same draw list frame after frame, and the builders turn it
// into the same commands, records and materials every time: the arena lookups, the bindless handles, the material
// records and the matrix lookups of thousands of batches, for the same result. With the setting on, the opaque
// shadow casters (fsgpudrivendraws.cpp), the legacy material passes (fsgpudrivenmaterials.cpp) and the legacy
// passes (fsgpudrivenlegacy.cpp) keep what they built for a pass, and while the pass is given the same list again
// they use it instead of building:
//  - The input: a pass with the same key (program and render type, or shadow kind) whose list holds the same words
//    (the draw info pointers, and the cascade sets of the single pass list), hashed and then compared. A kept pass
//    holds a reference to each of its draw infos, so none of them is freed while it is kept and no other can be
//    made at its address: the same pointer is the same batch. Draw infos aren't changed once their group's
//    rebuild made them (a rebuild makes new ones), so the same batch reads the same vertex buffer, range, textures,
//    matrices and material values; the draw infos keep those alive.
//  - What changes under the same batches is checked again, the way the build would have found it: every texture
//    handle the build looked up is looked up again (fsGDDPassTextureHandle: textures dropped and remade, made non
//    resident, or given a handle only now, all show as a different handle; the lookups also keep the textures and
//    their handles in use, as a build would), and every arena copy a command reads is found again
//    (FSGDDArena::find: a copy evicted, dropped with the arena or made again for a changed buffer shows as a
//    different place; finding it also marks it used this frame, so it isn't evicted or given back while the frame
//    still draws from it). Anything different and the pass is built again.
//  - The matrices aren't kept, they change with the camera and the objects: the lookups of the matrices the
//    records name are made again (FSGDDMatrixTable::replay), once per matrix rather than once per batch. With
//    FSPerfGPUDrivenSharedMatrices a matrix may land at another index than last frame (the passes before added
//    others); the records are then rewritten with the new indices and uploaded again.
//  - The commands, records and materials live in the kept pass's own buffers (FSGDDBuffer::setOwnStorage), not in
//    the upload ring, whose slices are written over three frames later. They are written only when the pass is
//    built, with new storage each time, so draws of earlier frames still in flight keep reading what they had;
//    a kept pass that is used again uploads nothing, its buffers are only bound again. The GPU occlusion culling
//    (fsgpuocclusion.cpp) copies its inputs from wherever they are, so it reads the kept ones the same way.
//  - Not kept: rigged passes (their palettes move every frame), passes with a batch fading in, rigged in a static
//    pass, or turned down by the arena (no room, copy budget: it may fit next frame), and anything while a verify
//    setting is on.
// Passes not used for a while are let go of (they hold draw infos, and through them vertex buffers and textures),
// all of them at fsGDDReleaseGL.

namespace
{
    constexpr U32 KEPT_PASS_IDLE_FRAMES = 8;    // a kept pass not used for this many frames is let go of
}

void FSGDDMatrixTable::finishUses(std::vector<FSGDDMatrixUse>& uses)
{
    std::sort(uses.begin(), uses.end(), [](const FSGDDMatrixUse& a, const FSGDDMatrixUse& b) { return a.mIndex < b.mIndex; });
    uses.erase(std::unique(uses.begin(), uses.end(), [](const FSGDDMatrixUse& a, const FSGDDMatrixUse& b) { return a.mIndex == b.mIndex; }),
               uses.end());
}

bool FSGDDMatrixTable::replay(std::vector<FSGDDMatrixUse>& uses, const glm::mat4& proj, const glm::mat4& view, std::vector<U32>& remap)
{
    // by old index, which is the order the table took them in: an own table gives each the index it had
    static std::vector<U32> now;
    now.resize(uses.size());
    bool same = true;
    for (size_t i = 0; i < uses.size(); ++i)
    {
        const FSGDDMatrixUse& use = uses[i];
        now[i] = use.mTexture ? textureIndex((const LLMatrix4*)use.mMatrix)
                              : lookupModel((const LLMatrix4*)use.mMatrix, proj, view, mFull);
        same = same && now[i] == use.mIndex;
    }
    if (same)
    {
        return true;
    }
    const U32 old_end = uses.empty() ? 0 : uses.back().mIndex + 1;
    remap.resize(old_end);
    for (U32 i = 0; i < old_end; ++i)
    {
        remap[i] = i;
    }
    for (size_t i = 0; i < uses.size(); ++i)
    {
        remap[uses[i].mIndex] = now[i];
        uses[i].mIndex = now[i];
    }
    finishUses(uses);   // sorted by the new indices: the order of this frame's table
    return false;
}

FSGDDKeptPass::FSGDDKeptPass()
{
    mCommandBuffer.setOwnStorage(true);
    mRecordBuffer.setOwnStorage(true);
    mDataBuffer.setOwnStorage(true);
}

FSGDDKeptPass::~FSGDDKeptPass()
{
}

void FSGDDKeptPass::keepInput(U64 key, U64 hash, const uintptr_t* words, size_t count, size_t stride)
{
    mKey = key;
    mHash = hash;
    mInput.assign(words, words + count);
    // assigned in place: a batch kept again keeps its reference instead of letting go and taking it back
    const size_t held = stride ? count / stride : 0;
    mHeld.resize(held);
    for (size_t i = 0; i < held; ++i)
    {
        mHeld[i] = (LLDrawInfo*)words[i * stride];
    }
}

void FSGDDKeptPass::clear()
{
    mValid = false;
    mKey = 0;
    mHash = 0;
    mInput.clear();
    mInput.shrink_to_fit();
    mHeld.clear();
    mHeld.shrink_to_fit();
    mTextures.clear();
    mArena.clear();
    mMatrices.clear();
    mCommandBuffer.release();
    mRecordBuffer.release();
    mDataBuffer.release();
    mBatches = 0;
}

FSGDDBuildCache::FSGDDBuildCache(EFSGDDBuilder builder, U32 size)
    : mBuilder(builder)
{
    for (U32 i = 0; i < size; ++i)
    {
        mPasses.push_back(new FSGDDKeptPass());
    }
    build_caches().push_back(this);
}

// static
bool FSGDDBuildCache::enabled()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "FSPerfGPUDrivenBuildCache", false);
    return enabled;
}

// static
U64 FSGDDBuildCache::hash(const uintptr_t* words, size_t count)
{
    U64 h = 0x9E3779B97F4A7C15ull ^ (U64)count;
    for (size_t i = 0; i < count; ++i)
    {
        h = (h ^ (U64)words[i]) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    return h;
}

void FSGDDBuildCache::sweep(U32 frame)
{
    if (mSweptFrame == frame)
    {
        return;
    }
    mSweptFrame = frame;
    for (FSGDDKeptPass* pass : mPasses)
    {
        if (pass->mValid && frame - pass->mLastFrame > KEPT_PASS_IDLE_FRAMES)
        {
            pass->clear();
        }
    }
}

FSGDDKeptPass* FSGDDBuildCache::find(U64 key, const uintptr_t* words, size_t count, U64 hash)
{
    const U32 frame = LLFrameTimer::getFrameCount();
    sweep(frame);
    for (FSGDDKeptPass* pass : mPasses)
    {
        if (pass->mValid && pass->mKey == key && pass->mHash == hash && pass->mInput.size() == count &&
            !memcmp(pass->mInput.data(), words, count * sizeof(uintptr_t)))
        {
            pass->mLastFrame = frame;
            return pass;
        }
    }
    return nullptr;
}

FSGDDKeptPass* FSGDDBuildCache::claim(U64 key)
{
    const U32 frame = LLFrameTimer::getFrameCount();
    sweep(frame);
    FSGDDKeptPass* oldest = nullptr;
    for (FSGDDKeptPass* pass : mPasses)
    {
        if (!pass->mValid)
        {
            if (!oldest || oldest->mValid)
            {
                oldest = pass;
            }
            continue;
        }
        if (pass->mKey == key && pass->mLastFrame != frame)
        {   // most likely this pass as it was in an earlier frame
            oldest = pass;
            break;
        }
        if (!oldest || (oldest->mValid && pass->mLastFrame - oldest->mLastFrame > 0x80000000u))
        {   // older (frames wrap)
            oldest = pass;
        }
    }
    if (!oldest)
    {
        return nullptr;
    }
    // what it held goes, the GL buffers stay (the next upload gives them new storage)
    oldest->mValid = false;
    oldest->mTextures.clear();
    oldest->mArena.clear();
    oldest->mMatrices.clear();
    oldest->mLastFrame = frame;
    return oldest;
}

U32 FSGDDBuildCache::slotOf(const FSGDDKeptPass* pass) const
{
    for (U32 i = 0; i < (U32)mPasses.size(); ++i)
    {
        if (mPasses[i] == pass)
        {
            return i;
        }
    }
    return U32_MAX;
}

void FSGDDBuildCache::release()
{
    for (FSGDDKeptPass* pass : mPasses)
    {
        pass->clear();
    }
}

void FSGDDBuildCache::takeCounts(U64& hits, U64& kept, U64& unkept)
{
    hits = mHits;
    kept = mKept;
    unkept = mUnkept;
    mHits = mKept = mUnkept = 0;
}

void fsGDDBuildCachesRelease()
{
    for (FSGDDBuildCache* cache : build_caches())
    {
        cache->release();
    }
}

void fsGDDBuildCachesTick()
{
    static bool s_was_on = false;
    const bool on = FSGDDBuildCache::enabled();
    if (!on)
    {   // turned off: nothing is kept any more
        if (s_was_on)
        {
            fsGDDBuildCachesRelease();
        }
        s_was_on = false;
        return;
    }
    s_was_on = true;
    const U32 frame = LLFrameTimer::getFrameCount();
    for (FSGDDBuildCache* cache : build_caches())
    {   // also those whose builder didn't run this frame
        cache->sweep(frame);
    }
}
