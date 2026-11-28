/**
 * @file fsgputimers.cpp
 * @brief FSRenderGPUTimings: GPU time of each LL_PROFILE_GPU_ZONE, averaged and logged
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
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

#include "linden_common.h"

#include "fsgputimers.h"

#include "fsglstream.h"

#include "llgl.h"
#include "llglheaders.h"
#include "llprofiler.h"
#include "lltimer.h"

#include <algorithm>
#include <deque>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
    // Bounds the cost and the queries in flight: a frame with more zones times only the first
    constexpr size_t MAX_ZONES_PER_FRAME = 1024;
    // Frames whose results are still on the GPU; past this the oldest is dropped unread
    constexpr size_t MAX_PENDING_FRAMES = 8;
    // A zone nested deeper than this (only recursion would) is not timed
    constexpr size_t MAX_DEPTH = 32;

    struct Rec
    {
        const char* mName = nullptr;
        GLuint mBegin = 0;
        GLuint mEnd = 0;
    };

    struct Frame
    {
        std::vector<Rec> mRecs;
        GLuint mFrameBegin = 0;
        GLuint mFrameEnd = 0;
    };

    struct Total
    {
        U64 mNs = 0;
        U32 mCalls = 0;
    };

    std::thread::id sMainThread;
    bool sOn = false;                       // timing this frame
    Frame sCur;
    std::vector<size_t> sStack;             // open zones, indices into sCur.mRecs
    std::deque<Frame> sPending;
    std::vector<GLuint> sFreeQueries;

    std::unordered_map<const char*, std::string*> sNameOf;  // literal pointer -> its text
    std::unordered_map<std::string, Total> sTotals;
    U64 sFrameNs = 0;
    U32 sFrames = 0;
    U32 sDropped = 0;
    U32 sDroppedZones = 0;                  // zones past MAX_ZONES_PER_FRAME, not timed
    LLTimer sLogTimer;
    bool sLive = false;
    bool sLiveValid = false;
    F64 sLiveMs = 0.0;
    LLTimer sLiveTimer;

    // The settings tuner's capture: the same sums over the whole measurement. A frame whose GPU time alone is
    // over the tuner's hitch limit (a driver shader build, VRAM paging) is left out, as the tuner leaves out the
    // hitch frames of the main thread: in, it outweighed the frames around it and the GPU looked the limit.
    constexpr U64 CAPTURE_HITCH_NS = 250000000ULL;  // fssettingstuner.cpp HITCH_SECONDS
    bool sCapture = false;
    std::unordered_map<std::string, Total> sCapTotals;
    U64 sCapFrameNs = 0;
    U32 sCapFrames = 0;

    GLuint takeQuery()
    {
        if (sFreeQueries.empty())
        {
            GLuint ids[64];
            glGenQueries(64, ids);
            sFreeQueries.insert(sFreeQueries.end(), ids, ids + 64);
        }
        GLuint q = sFreeQueries.back();
        sFreeQueries.pop_back();
        return q;
    }

    void giveBack(Frame& f)
    {
        for (const Rec& r : f.mRecs)
        {
            if (r.mBegin) sFreeQueries.push_back(r.mBegin);
            if (r.mEnd) sFreeQueries.push_back(r.mEnd);
        }
        if (f.mFrameBegin) sFreeQueries.push_back(f.mFrameBegin);
        if (f.mFrameEnd) sFreeQueries.push_back(f.mFrameEnd);
        f.mRecs.clear();
        f.mFrameBegin = f.mFrameEnd = 0;
    }

    void zoneBegin(const char* name)
    {
        if (std::this_thread::get_id() != sMainThread)
        {
            return; // only the main thread's GL calls are timed (zoneEnd ignores these too)
        }
        if (!sOn)
        {
            sStack.push_back(SIZE_MAX); // keep begin and end paired
            return;
        }
        if (sCur.mRecs.size() >= MAX_ZONES_PER_FRAME || sStack.size() >= MAX_DEPTH)
        {
            if (sCur.mRecs.size() >= MAX_ZONES_PER_FRAME)
            {
                ++sDroppedZones;
            }
            sStack.push_back(SIZE_MAX);
            return;
        }
        Rec r;
        r.mName = name;
        r.mBegin = takeQuery();
        glQueryCounter(r.mBegin, GL_TIMESTAMP);
        sStack.push_back(sCur.mRecs.size());
        sCur.mRecs.push_back(r);
    }

    void zoneEnd()
    {
        if (std::this_thread::get_id() != sMainThread)
        {
            return; // zones on other GL threads never pushed (see zoneBegin's thread test)
        }
        if (sStack.empty())
        {
            return;
        }
        const size_t idx = sStack.back();
        sStack.pop_back();
        if (idx == SIZE_MAX || idx >= sCur.mRecs.size())
        {
            return;
        }
        Rec& r = sCur.mRecs[idx];
        r.mEnd = takeQuery();
        glQueryCounter(r.mEnd, GL_TIMESTAMP);
    }

    // Timestamps finish in order, so the frame's last one being ready means all of it is.
    // With the render thread on, only what it has collected counts: a read the stream can't answer from
    // there (a query from before a DirectScope, or from before the render thread started) waited for
    // the render thread to catch up, a frame's worth at times. Such a frame stays pending and is dropped
    // (closeFrame); a zone of it is left out (timestamp).
    bool ready(const Frame& f)
    {
        if (FSGLStream::isRecording())
        {
            U64 t = 0;
            return FSGLStream::peekQueryResult(f.mFrameEnd, t);
        }
        GLint avail = 0;
        glGetQueryObjectiv(f.mFrameEnd, GL_QUERY_RESULT_AVAILABLE, &avail);
        return avail != 0;
    }

    bool timestamp(GLuint q, U64& t)
    {
        if (FSGLStream::isRecording())
        {
            return FSGLStream::peekQueryResult(q, t);
        }
        GLuint64 value = 0;
        glGetQueryObjectui64v(q, GL_QUERY_RESULT, &value);
        t = (U64)value;
        return true;
    }

    const std::string& nameOf(const char* name)
    {
        auto it = sNameOf.find(name);
        if (it == sNameOf.end())
        {
            it = sNameOf.emplace(name, new std::string(name ? name : "?")).first;
        }
        return *it->second;
    }

    void collect(Frame& f)
    {
        U64 t0 = 0;
        U64 t1 = 0;
        bool capture = sCapture;
        if (timestamp(f.mFrameBegin, t0) && timestamp(f.mFrameEnd, t1) && t1 > t0)
        {
            if (sLive)
            {
                const F64 ms = (F64)(t1 - t0) / 1.0e6;
                sLiveMs = sLiveValid ? sLiveMs * 0.9 + ms * 0.1 : ms;
                sLiveValid = true;
                sLiveTimer.reset();
            }
            sFrameNs += t1 - t0;
            capture = capture && t1 - t0 <= CAPTURE_HITCH_NS;
            if (capture)
            {
                sCapFrameNs += t1 - t0;
            }
        }
        for (const Rec& r : f.mRecs)
        {
            if (!r.mBegin || !r.mEnd)
            {
                continue; // still open at the end of the frame
            }
            U64 b = 0;
            U64 e = 0;
            if (!timestamp(r.mBegin, b) || !timestamp(r.mEnd, e))
            {
                continue;
            }
            Total& total = sTotals[nameOf(r.mName)];
            total.mNs += e > b ? e - b : 0;
            ++total.mCalls;
            if (capture)
            {
                Total& cap = sCapTotals[nameOf(r.mName)];
                cap.mNs += e > b ? e - b : 0;
                ++cap.mCalls;
            }
        }
        ++sFrames;
        if (capture)
        {
            ++sCapFrames;
        }
    }

    void logTotals()
    {
        if (!sFrames)
        {
            return;
        }
        std::vector<std::pair<const std::string*, Total>> rows;
        rows.reserve(sTotals.size());
        for (const auto& [name, total] : sTotals)
        {
            rows.emplace_back(&name, total);
        }
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.mNs > b.second.mNs; });

        const F64 per_frame = 1.0 / (F64)sFrames / 1.0e6;   // ns summed over frames -> ms per frame
        std::string line;
        size_t shown = 0;
        for (const auto& row : rows)
        {
            const F64 ms = (F64)row.second.mNs * per_frame;
            // the 30 biggest left most of a heavy pass in zones under the cut; 80 shows nearly all of it
            if (shown >= 80 || ms < 0.01)
            {
                break;
            }
            line += llformat("%s%s %.2f (x%.1f)", shown ? ", " : "", row.first->c_str(), ms, (F64)row.second.mCalls / (F64)sFrames);
            ++shown;
        }
        LL_INFOS("FSGPUTimings") << "GPU ms/frame over " << sFrames << " frames: frame " << llformat("%.2f", (F64)sFrameNs * per_frame)
                                 << "; zones, inclusive (x calls per frame): " << line
                                 << (sDropped ? llformat("; %u frames dropped unread", sDropped) : std::string())
                                 << (sDroppedZones ? llformat("; %u zones past the %u per frame limit untimed", sDroppedZones, (U32)MAX_ZONES_PER_FRAME) : std::string())
                                 << LL_ENDL;
        sTotals.clear();
        sFrameNs = 0;
        sFrames = 0;
        sDropped = 0;
        sDroppedZones = 0;
    }

    void closeFrame()
    {
        // A zone still open (a frame boundary inside a zone) just goes untimed
        sStack.clear();
        if (sCur.mFrameBegin)
        {
            sCur.mFrameEnd = takeQuery();
            glQueryCounter(sCur.mFrameEnd, GL_TIMESTAMP);
            sPending.push_back(std::move(sCur));
            sCur = Frame();
        }
        if (sPending.size() > MAX_PENDING_FRAMES)
        {
            giveBack(sPending.front());
            sPending.pop_front();
            ++sDropped;
        }
    }
}

namespace FSGPUTimers
{
    void frame(bool enabled, F32 interval_seconds, bool live_frame)
    {
        if (sMainThread == std::thread::id())
        {
            sMainThread = std::this_thread::get_id();
            LLFSGPUZone::sBegin = &zoneBegin;
            LLFSGPUZone::sEnd = &zoneEnd;
        }
        // A capture times frames without logging them
        const bool log = enabled;
        const bool zones = enabled || (sCapture && gGLManager.mInited);
        enabled = zones || (live_frame && gGLManager.mInited);
#if LL_WINDOWS // elsewhere these are prototypes, not loaded pointers
        if (!glQueryCounter || !glGetQueryObjectui64v)
        {
            enabled = false;
        }
#endif

        sLive = enabled && live_frame;
        if (!sLive)
        {
            sLiveValid = false;
        }

        if (sOn)
        {
            closeFrame();
        }

        while (!sPending.empty() && ready(sPending.front()))
        {
            collect(sPending.front());
            giveBack(sPending.front());
            sPending.pop_front();
        }

        if (enabled && sLogTimer.getElapsedTimeF32() >= llmax(interval_seconds, 1.f))
        {
            sLogTimer.reset();
            if (log)
            {
                logTotals();
            }
            else
            {   // capturing only: the log's sums are not wanted, only kept from growing
                sTotals.clear();
                sFrameNs = 0;
                sFrames = 0;
                sDropped = 0;
                sDroppedZones = 0;
            }
        }

        if (!enabled && sOn)
        {
            sTotals.clear();
            sFrameNs = 0;
            sFrames = 0;
            sDroppedZones = 0;
        }
        else if (enabled && !sOn)
        {
            sLogTimer.reset();
        }

        sOn = enabled;
        LLFSGPUZone::sActive.store(enabled && zones, std::memory_order_relaxed);
        if (sOn)
        {
            sCur.mFrameBegin = takeQuery();
            glQueryCounter(sCur.mFrameBegin, GL_TIMESTAMP);
        }
    }

    void releaseGL()
    {
        LLFSGPUZone::sActive.store(false, std::memory_order_relaxed);
        sOn = false;
        sLive = sLiveValid = false;
        sStack.clear();
        giveBack(sCur);
        for (Frame& f : sPending)
        {
            giveBack(f);
        }
        sPending.clear();
        if (!sFreeQueries.empty())
        {
            glDeleteQueries((GLsizei)sFreeQueries.size(), sFreeQueries.data());
        }
        sFreeQueries.clear();
    }

    bool getLiveFrameMs(F64& ms)
    {
        if (!sLive || !sLiveValid || sLiveTimer.getElapsedTimeF32() > 2.f)
        {
            return false;
        }
        ms = sLiveMs;
        return true;
    }

    void startCapture()
    {
        sCapture = true;
        sCapTotals.clear();
        sCapFrameNs = 0;
        sCapFrames = 0;
    }

    void stopCapture()
    {
        sCapture = false;
    }

    void getCapture(std::vector<std::pair<std::string, F64>>& zones_ms, F64& frame_ms, U32& frames)
    {
        zones_ms.clear();
        frames = sCapFrames;
        frame_ms = 0.0;
        if (!sCapFrames)
        {
            return;
        }
        const F64 per_frame = 1.0 / (F64)sCapFrames / 1.0e6;
        frame_ms = (F64)sCapFrameNs * per_frame;
        zones_ms.reserve(sCapTotals.size());
        for (const auto& [name, total] : sCapTotals)
        {
            zones_ms.emplace_back(name, (F64)total.mNs * per_frame);
        }
        std::sort(zones_ms.begin(), zones_ms.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    }
}
