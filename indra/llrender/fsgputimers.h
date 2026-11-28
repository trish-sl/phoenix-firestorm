/**
 * @file fsgputimers.h
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

#ifndef FS_GPUTIMERS_H
#define FS_GPUTIMERS_H

#include "stdtypes.h"

#include <string>
#include <utility>
#include <vector>

// Every LL_PROFILE_GPU_ZONE in the viewer (about a hundred: passes, shadow maps, post effects)
// writes a GPU timestamp when it opens and when it closes. The results are read back once they
// are ready, a few frames later, without ever waiting for the GPU (also with the render thread,
// which collects query results itself), and summed per zone name. Every interval the average
// GPU milliseconds per frame of each zone are logged, largest first. Times are inclusive: a
// zone includes the zones opened inside it.
namespace FSGPUTimers
{
    // Main thread, once per frame, at the start of the frame: turns timing on or off, closes the
    // previous frame, collects finished frames and logs every interval_seconds.
    void frame(bool enabled, F32 interval_seconds, bool live_frame = false);

    // Recent GPU frame timestamp span, or false while unavailable/stale. Does not wait for the GPU.
    bool getLiveFrameMs(F64& ms);

    // Main thread, GL context gone or going: forget all queries
    void releaseGL();

    // The settings tuner's measurement (fssettingstuner.cpp). While a capture runs, timing is on whether
    // frame() is asked for it or not (nothing is logged unless it is), and every frame read back is also
    // summed into the capture. startCapture clears it; stopCapture ends it. Main thread.
    void startCapture();
    void stopCapture();
    // Average GPU ms per frame of each zone and of the whole frame over the frames captured so far
    // (frames: how many). Zones are inclusive, as in the log.
    void getCapture(std::vector<std::pair<std::string, F64>>& zones_ms, F64& frame_ms, U32& frames);
}

#endif // FS_GPUTIMERS_H
