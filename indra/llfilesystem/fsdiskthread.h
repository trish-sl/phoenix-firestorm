/**
 * @file fsdiskthread.h
 * @brief A thread for disk cache work the main thread would otherwise wait on
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
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

#ifndef FS_DISKTHREAD_H
#define FS_DISKTHREAD_H

#include <functional>

// FSPerfDiskThread: one thread, below normal priority, for the asset disk cache work the main thread
// used to do itself: whether an asset is cached (and reading it once so the main thread's later read
// comes from the OS file cache) and the last access time update when a cached file is opened. Each of
// those is a few filesystem calls through every file system filter (antivirus, cloud sync), which with
// a slow disk or real time scanning cost milliseconds each, in bursts when content arrives.
// Nothing runs on it while the setting is off.
namespace FSDiskThread
{
    // Main thread. Starts the thread the first time it is enabled; disabling only stops new work.
    void setEnabled(bool enabled);
    bool isEnabled();

    // Runs work on the disk thread and then done on the main thread (the main loop's work queue).
    // False when the thread isn't enabled or is shutting down: the caller does the work itself.
    bool post(std::function<void()> work, std::function<void()> done);

    // Runs work on the disk thread, nothing afterwards. False as above.
    bool post(std::function<void()> work);

    // As post(work), ahead of what is queued: for short jobs something is waiting on (an asset download
    // writing its cache file, an index the next lookups use), which would otherwise wait behind
    // hundreds of cache checks while the disk is busy.
    bool postUrgent(std::function<void()> work);

    // Finishes the queued work and stops the thread (application shutdown).
    void shutdown();
}

#endif // FS_DISKTHREAD_H
