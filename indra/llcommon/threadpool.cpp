/**
 * @file   threadpool.cpp
 * @author Nat Goodspeed
 * @date   2021-10-21
 * @brief  Implementation for threadpool.
 *
 * $LicenseInfo:firstyear=2021&license=viewerlgpl$
 * Copyright (c) 2021, Linden Research, Inc.
 * $/LicenseInfo$
 */

// Precompiled header
#include "linden_common.h"
// associated header
#include "threadpool.h"
// STL headers
// std headers
// external library headers
// other Linden headers
#include "commoncontrol.h"
#include "llerror.h"
#include "llevents.h"
#include "llsd.h"
#include "stringize.h"

#include <boost/fiber/algo/round_robin.hpp>
#include <condition_variable> // <FS:Perf>
#include <mutex> // <FS:Perf>

/*****************************************************************************
*   Custom fiber scheduler for worker threads
*****************************************************************************/
// As of 2022-12-06, each of our worker threads only runs a single (default)
// fiber: we don't launch explicit fibers within worker threads, nor do we
// anticipate doing so. So a worker thread that's simply waiting for incoming
// tasks should really sleep a little. Override the default fiber scheduler to
// implement that.
// <FS:Perf> The previous implementation slept for 1 ms in a loop and
// ignored notify(), so every task posted to an idle worker (image decode,
// texture fetch/cache, mesh LOD processing, ...) waited 1-2 ms before it
// even started, and replies to the main thread were delayed the same way.
// Block on a condition variable instead and wake up on notify().
//
// round_robin's own implementation calls wait_until() with
// time_point::max() when there is nothing scheduled; on MSVC converting
// that to an absolute system time overflows and the wait returns
// immediately, which is the "busier than it ought to be" behaviour seen
// on Windows. Wait without a deadline in that case, and cap long waits.
struct sleepy_robin: public boost::fibers::algo::round_robin
{
    virtual void suspend_until( std::chrono::steady_clock::time_point const& abs_time) noexcept
    {
        std::unique_lock<std::mutex> lock(mMutex);
        if (abs_time == (std::chrono::steady_clock::time_point::max)())
        {
            mCondition.wait(lock, [this]() { return mNotified; });
        }
        else
        {
            // bounded wait; spurious early wake-ups are harmless here
            const auto max_wait = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            mCondition.wait_until(lock, (std::min)(abs_time, max_wait), [this]() { return mNotified; });
        }
        mNotified = false;
    }

    virtual void notify() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mNotified = true;
        }
        mCondition.notify_all();
    }

private:
    std::mutex mMutex;
    std::condition_variable mCondition;
    bool mNotified = false;
};
// </FS:Perf>

/*****************************************************************************
*   ThreadPoolBase
*****************************************************************************/
LL::ThreadPoolBase::ThreadPoolBase(const std::string& name,
                                   size_t threads,
                                   WorkQueueBase* queue,
                                   bool auto_shutdown):
    super(name),
    mName("ThreadPool:" + name),
    mThreadCount(getConfiguredWidth(name, threads)),
    mQueue(queue),
    mAutomaticShutdown(auto_shutdown)
{}

void LL::ThreadPoolBase::start()
{
    for (size_t i = 0; i < mThreadCount; ++i)
    {
        std::string tname{ stringize(mName, ':', (i+1), '/', mThreadCount) };
        mThreads.emplace_back(tname, [this, tname]()
            {
                set_thread_name(tname.c_str());
                LL_PROFILER_SET_THREAD_NAME(tname.c_str());
                LL_INFOS("THREAD") << "Started thread " << tname << LL_ENDL;
                run(tname);
            });
    }

    if (!mAutomaticShutdown)
    {
        // Some threads, like main window's might need to run a bit longer
        // to wait for a proper shutdown message
        return;
    }

    // Listen on "LLApp", and when the app is shutting down, close the queue
    // and join the workers.
    LLEventPumps::instance().obtain("LLApp").listen(
        mName,
        [this](const LLSD& stat)
        {
            std::string status(stat["status"]);
            if (status != "running")
            {
                // viewer is starting shutdown -- proclaim the end is nigh!
                LL_DEBUGS("ThreadPool") << mName << " saw " << status << LL_ENDL;
                close();
            }
            return false;
        });
}

LL::ThreadPoolBase::~ThreadPoolBase()
{
    close();
    if (!LLEventPumps::wasDeleted())
    {
        LLEventPumps::instance().obtain("LLApp").stopListening(mName);
    }
}

void LL::ThreadPoolBase::close()
{
    if (! mQueue->isClosed())
    {
        LL_DEBUGS("ThreadPool") << mName << " closing queue and joining threads" << LL_ENDL;
        mQueue->close();
        for (auto& pair: mThreads)
        {
            if (pair.second.joinable())
            {
                LL_DEBUGS("ThreadPool") << mName << " waiting on thread " << pair.first << LL_ENDL;
                pair.second.join();
            }
        }
        LL_DEBUGS("ThreadPool") << mName << " shutdown complete" << LL_ENDL;
    }
}

void LL::ThreadPoolBase::run(const std::string& name)
{
#if LL_WINDOWS
    // Try using sleepy_robin fiber scheduler.
    boost::fibers::use_scheduling_algorithm<sleepy_robin>();
#endif // LL_WINDOWS

    LL_DEBUGS("ThreadPool") << name << " starting" << LL_ENDL;
    run();
    LL_DEBUGS("ThreadPool") << name << " stopping" << LL_ENDL;
}

void LL::ThreadPoolBase::run()
{
    mQueue->runUntilClose();
}

//static
size_t LL::ThreadPoolBase::getConfiguredWidth(const std::string& name, size_t dft)
{
    LLSD poolSizes;
    try
    {
        poolSizes = LL::CommonControl::get("Global", "ThreadPoolSizes");
        // "ThreadPoolSizes" is actually a map containing the sizes of
        // interest -- or should be, if this process has an
        // LLViewerControlListener instance and its settings include
        // "ThreadPoolSizes". If we failed to retrieve it, perhaps we're in a
        // program that doesn't define that, or perhaps there's no such
        // setting, or perhaps we're asking too early, before the LLEventAPI
        // itself has been instantiated. In any of those cases, it seems worth
        // warning.
        if (! poolSizes.isDefined())
        {
            // Note: we don't warn about absence of an override key for a
            // particular ThreadPool name, that's fine. This warning is about
            // complete absence of a ThreadPoolSizes setting, which we expect
            // in a normal viewer session.
            LL_WARNS("ThreadPool") << "No 'ThreadPoolSizes' setting for ThreadPool '"
                                   << name << "'" << LL_ENDL;
        }
    }
    catch (const LL::CommonControl::Error& exc)
    {
        // We don't want ThreadPool to *require* LLViewerControlListener.
        // Just log it and carry on.
        LL_WARNS("ThreadPool") << "Can't check 'ThreadPoolSizes': " << exc.what() << LL_ENDL;
    }

    LL_DEBUGS("ThreadPool") << "ThreadPoolSizes = " << poolSizes << LL_ENDL;
    // LLSD treats an undefined value as an empty map when asked to retrieve a
    // key, so we don't need this to be conditional.
    LLSD sizeSpec{ poolSizes[name] };
    // We retrieve sizeSpec as LLSD, rather than immediately as LLSD::Integer,
    // so we can distinguish the case when it's undefined.
    return sizeSpec.isInteger() ? sizeSpec.asInteger() : dft;
}

//static
size_t LL::ThreadPoolBase::getWidth(const std::string& name, size_t dft)
{
    auto instance{ getInstance(name) };
    if (instance)
    {
        return instance->getWidth();
    }
    else
    {
        return getConfiguredWidth(name, dft);
    }
}
