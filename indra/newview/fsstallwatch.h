/**
 * @file fsstallwatch.h
 * @brief Finds out where the main thread is when a frame stalls for seconds
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

#ifndef FS_STALLWATCH_H
#define FS_STALLWATCH_H

#include "stdtypes.h"

#include <string>

namespace FSStallWatch
{
    // Main thread, once a frame. Closes the frame that just ended (see lastFrameCosts()) and
    // starts timing the next. While watching, a frame still running after stack_ms gets the main
    // thread's call stack logged (FSHitch), again at 3x and 8x that; 0 is off.
    void frameMark(bool watching, F32 stack_ms);

    // What the frame that just ended cost the main thread beyond wall time: CPU time used, page
    // faults and disk traffic of the process. A long frame with little CPU time was waiting.
    std::string lastFrameCosts();

    void shutdown();
}

#endif // FS_STALLWATCH_H
