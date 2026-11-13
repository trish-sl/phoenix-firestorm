/**
 * @file fsdiskthread.cpp
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

#include "linden_common.h"

#include "fsdiskthread.h"

#include "llprofiler.h"
#include "workqueue.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#if LL_WINDOWS
#include "llwin32headers.h"
#endif

namespace
{
    std::mutex sMutex;
    std::condition_variable sWake;
    std::deque<std::function<void()>> sJobs;    // sMutex
    std::thread sThread;
    bool sStarted = false;                      // sMutex
    bool sQuit = false;                         // sMutex
    std::atomic<bool> sEnabled{ false };

    void threadLoop()
    {
        LL_PROFILER_SET_THREAD_NAME("FS Disk");
#if LL_WINDOWS
        // Disk waits, little CPU: never take a core from the main or render thread
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
        std::unique_lock<std::mutex> lock(sMutex);
        while (true)
        {
            sWake.wait(lock, [] { return sQuit || !sJobs.empty(); });
            if (sJobs.empty())
            {
                return;     // quitting, everything queued is done
            }
            std::function<void()> job = std::move(sJobs.front());
            sJobs.pop_front();
            lock.unlock();
            {
                LL_PROFILE_ZONE_NAMED("fs disk job");
                job();
            }
            lock.lock();
        }
    }

    bool enqueue(std::function<void()> job, bool urgent = false)
    {
        if (!sEnabled.load(std::memory_order_relaxed))
        {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(sMutex);
            if (!sStarted || sQuit)
            {
                return false;
            }
            if (urgent)
            {
                sJobs.push_front(std::move(job));
            }
            else
            {
                sJobs.push_back(std::move(job));
            }
        }
        sWake.notify_one();
        return true;
    }
}

namespace FSDiskThread
{
    void setEnabled(bool enabled)
    {
        if (enabled)
        {
            std::lock_guard<std::mutex> lock(sMutex);
            if (!sStarted && !sQuit)
            {
                sStarted = true;
                sThread = std::thread(&threadLoop);
            }
        }
        sEnabled.store(enabled, std::memory_order_relaxed);
    }

    bool isEnabled()
    {
        return sEnabled.load(std::memory_order_relaxed);
    }

    bool post(std::function<void()> work, std::function<void()> done)
    {
        LL::WorkQueue::weak_t mainq = LL::WorkQueue::getInstance("mainloop");
        if (mainq.expired())
        {
            return false;
        }
        // done captures main thread objects (LLPointer counts aren't atomic), so the disk thread
        // must never copy or destroy it: it only passes this holder on (an atomic count), and the
        // main thread takes done out, runs it and destroys it.
        auto holder = std::make_shared<std::function<void()>>(std::move(done));
        return enqueue([work = std::move(work), holder, mainq]() mutable
            {
                work();
                // If the main loop's queue is gone (shutting down), the follow up is dropped, as any
                // other work posted to it then
                if (!LL::WorkQueue::postMaybe(mainq, [holder]()
                        {
                            std::function<void()> fn;
                            fn.swap(*holder);
                            fn();
                        }))
                {
                    // shortcut: leaks done at shutdown rather than destroy it on this thread
                    new std::shared_ptr<std::function<void()>>(holder);
                }
            });
    }

    bool post(std::function<void()> work)
    {
        return enqueue(std::move(work));
    }

    bool postUrgent(std::function<void()> work)
    {
        return enqueue(std::move(work), true);
    }

    void shutdown()
    {
        sEnabled.store(false, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(sMutex);
            sQuit = true;
        }
        sWake.notify_all();
        if (sThread.joinable())
        {
            sThread.join();
        }
    }
}
