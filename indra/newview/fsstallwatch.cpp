/**
 * @file fsstallwatch.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fsstallwatch.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#if LL_WINDOWS
#include "llwin32headers.h"
#include <psapi.h>
#include <dbghelp.h>
#include <tlhelp32.h>

// llrender/fsglstream.cpp: the render thread's handle and stack range
bool fs_render_thread_info(void*& handle, uintptr_t& stack_low, uintptr_t& stack_high);
#endif

namespace
{
S64 now_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::atomic<S64> sFrameStartUs{ 0 };
std::atomic<U64> sFrameId{ 0 };
std::atomic<U64> sFrameCpuStart{ 0 }; // main thread CPU time at frame start, 100 ns units
std::atomic<bool> sWatching{ false };
std::atomic<F32> sStackMs{ 0.f };
std::atomic<bool> sShutdown{ false };
std::string sLastCosts;

#if LL_WINDOWS
U64 filetime(const FILETIME& ft)
{
    return ((U64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

struct Counters
{
    U64 mUser = 0;   // 100 ns
    U64 mKernel = 0; // 100 ns
    U32 mFaults = 0;
    U64 mRead = 0;
    U64 mWrite = 0;
};

Counters read_counters(HANDLE thread)
{
    Counters c;
    FILETIME create, exit, kernel, user;
    if (GetThreadTimes(thread, &create, &exit, &kernel, &user))
    {
        c.mUser = filetime(user);
        c.mKernel = filetime(kernel);
    }
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
    {
        c.mFaults = pmc.PageFaultCount;
    }
    IO_COUNTERS io{};
    if (GetProcessIoCounters(GetCurrentProcess(), &io))
    {
        c.mRead = io.ReadTransferCount;
        c.mWrite = io.WriteTransferCount;
    }
    return c;
}

#if defined(_M_X64)
// Walks a copy of the stack (taken while the thread was suspended), so nothing that may take a
// lock the suspended thread holds runs while it is suspended. Values in the copy that point into
// the original stack were rebased onto the copy by the caller.
int unwind_copy(CONTEXT& ctx, const U8* copy, size_t len, DWORD64* pcs, int max_pcs)
{
    const DWORD64 lo = (DWORD64)copy;
    const DWORD64 hi = lo + len;
    int n = 0;
    __try
    {
        while (n < max_pcs && ctx.Rip)
        {
            pcs[n++] = ctx.Rip;
            if (ctx.Rsp < lo || ctx.Rsp + 8 > hi)
            {
                break;
            }
            DWORD64 image_base = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, NULL);
            if (fn)
            {
                PVOID handler_data = NULL;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data, &establisher, NULL);
            }
            else
            {
                // leaf function: the return address is on top of the stack
                ctx.Rip = *(const DWORD64*)ctx.Rsp;
                ctx.Rsp += 8;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return n;
}
#endif

class Watcher
{
public:
    void start()
    {
        if (mThread.joinable())
        {
            return;
        }
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &mMain,
                             THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        {
            mMain = NULL;
            return;
        }
        // This thread's stack: everything from its reservation up to the end of the committed
        // range holding a local (the stack's top; below it, uncommitted pages and the guard page).
        MEMORY_BASIC_INFORMATION mbi{};
        volatile U8 local = 0;
        if (!VirtualQuery((LPCVOID)&local, &mbi, sizeof(mbi)))
        {
            CloseHandle(mMain);
            mMain = NULL;
            return;
        }
        mStackLow = (ULONG_PTR)mbi.AllocationBase;
        mStackHigh = (ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;
        mThread = std::thread([this]() { run(); });
    }

    void stop()
    {
        if (mThread.joinable())
        {
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mStop = true;
            }
            mCv.notify_all();
            mThread.join();
        }
        if (mMain)
        {
            CloseHandle(mMain);
            mMain = NULL;
        }
    }

    HANDLE mainThread() const { return mMain; }

private:
    void run()
    {
        LL_INFOS("FSHitch") << "Stall watch started" << LL_ENDL;
        U64 frame = 0;
        int samples = 0;
        std::unique_lock<std::mutex> lock(mMutex);
        while (!mStop)
        {
            mCv.wait_for(lock, std::chrono::milliseconds(100));
            if (mStop)
            {
                break;
            }
            const F32 stack_ms = sStackMs.load();
            if (!sWatching.load() || stack_ms <= 0.f)
            {
                continue;
            }
            const U64 id = sFrameId.load();
            if (id != frame)
            {
                frame = id;
                samples = 0;
            }
            static const F32 FACTORS[] = { 1.f, 3.f, 8.f };
            const F64 elapsed_ms = (now_us() - sFrameStartUs.load()) / 1000.0;
            if (samples < 3 && elapsed_ms >= stack_ms * FACTORS[samples])
            {
                ++samples;
                lock.unlock();
                sample(id, elapsed_ms);
                lock.lock();
            }
        }
    }

    void sample(U64 id, F64 elapsed_ms)
    {
        const Counters counters = read_counters(mMain);
        const U64 cpu = counters.mUser + counters.mKernel;
        const F64 cpu_ms = cpu > sFrameCpuStart.load() ? (cpu - sFrameCpuStart.load()) / 10000.0 : 0.0;
        bool same_frame = true;
        const std::string stack = threadStack(mMain, mStackLow, mStackHigh, id, same_frame);
        if (!same_frame)
        {
            return; // it moved on meanwhile
        }
        // The render thread too: a main thread waiting on present is waiting for it
        std::string render_stack;
        void* render_handle = NULL;
        uintptr_t render_low = 0, render_high = 0;
        if (fs_render_thread_info(render_handle, render_low, render_high))
        {
            render_stack = threadStack((HANDLE)render_handle, render_low, render_high, id, same_frame);
        }
        LL_WARNS("FSHitch") << llformat("Main thread stuck %.0f ms in one frame, using %.0f ms of CPU in it", elapsed_ms, cpu_ms)
                            << (stack.empty() ? std::string() : "; it is in: " + stack)
                            << (render_stack.empty() ? std::string() : "; the render thread is in: " + render_stack) << LL_ENDL;

        // Waiting rather than working: what it waits for is likely another thread holding a lock, or a
        // disk read. List the threads that aren't idle, on the first sample of a frame only.
        if (cpu_ms < elapsed_ms * 0.5 && id != mOthersLoggedFrame)
        {
            mOthersLoggedFrame = id;
            logOtherThreads(id, render_handle);
        }
    }

    // Every thread of the process except this one, the main and the render thread, whose stack top
    // isn't an idle wait (a thread pool waiting for work, a sleep), with its name and a short stack.
    // Waits on a lock or a condition variable are listed: one of those may be what holds things up.
    void logOtherThreads(U64 id, void* render_handle)
    {
        const DWORD pid = GetCurrentProcessId();
        const DWORD self = GetCurrentThreadId();
        const DWORD main_id = GetThreadId(mMain);
        const DWORD render_id = render_handle ? GetThreadId((HANDLE)render_handle) : 0;
        std::vector<DWORD> ids;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE)
        {
            return;
        }
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        {
            if (te.th32OwnerProcessID == pid && te.th32ThreadID != self && te.th32ThreadID != main_id && te.th32ThreadID != render_id)
            {
                ids.push_back(te.th32ThreadID);
            }
        }
        CloseHandle(snap);

        // stack tops of a thread parked until there is work for it
        static const char* IDLE[] = { "NtWaitForSingleObject", "NtWaitForMultipleObjects", "NtWaitForWorkViaWorkerFactory",
                                      "NtRemoveIoCompletion", "NtDelayExecution", "ZwWaitForSingleObject", "ZwWaitForMultipleObjects",
                                      "ZwWaitForWorkViaWorkerFactory", "ZwRemoveIoCompletion", "ZwDelayExecution",
                                      "NtUserMsgWaitForMultipleObjectsEx", "ZwUserMsgWaitForMultipleObjectsEx",
                                      "NtUserGetMessage", "ZwUserGetMessage", "ZwWaitForAlertByThreadId", "NtWaitForAlertByThreadId" };
        // ...unless it is waiting on a lock, which may be the one the main thread needs
        static const char* LOCKS[] = { "EnterCriticalSection", "AcquireSRWLock", "_Mtx_lock", "mutex::lock" };
        std::string out;
        int listed = 0;
        int idle = 0;
        for (DWORD tid : ids)
        {
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
            if (!thread)
            {
                continue;
            }
            bool same_frame = true;
            // Walking and naming a hundred threads takes a while: the main thread often moves on
            // meanwhile, and what was holding it up is still worth seeing
            const std::string stack = threadStack(thread, 0, 0, id, same_frame, 10, true);
            std::string name;
            PWSTR desc = nullptr;
            if (SUCCEEDED(GetThreadDescription(thread, &desc)) && desc)
            {
                name = ll_convert_wide_to_string(desc);
                LocalFree(desc);
            }
            CloseHandle(thread);
            if (stack.empty())
            {
                continue;
            }
            bool parked = false;
            for (const char* top : IDLE)
            {
                if (stack.compare(0, strlen(top), top) == 0)
                {
                    parked = true;
                    break;
                }
            }
            if (parked)
            {
                for (const char* lock : LOCKS)
                {
                    if (stack.find(lock) != std::string::npos)
                    {
                        parked = false;
                        break;
                    }
                }
            }
            if (parked)
            {
                ++idle;
                continue;
            }
            if (listed < 16)
            {
                out += llformat("\n    thread %u", (U32)tid) + (name.empty() ? std::string() : " (" + name + ")") + ": " + stack;
            }
            ++listed;
        }
        LL_WARNS("FSHitch") << "While the main thread waited, " << listed << " other threads were not idle (" << idle
                            << " idle)" << (sFrameId.load() != id ? ", the main thread moved on while they were listed" : "")
                            << (listed > 16 ? ", the first 16:" : ":") << out << LL_ENDL;
    }

    // The call stack of a thread whose stack spans [stack_low, stack_high), copied while it is
    // briefly suspended. same_frame turns false when the main thread finished frame id meanwhile.
    // any_frame: take it even when the main thread has finished frame id meanwhile
    std::string threadStack(HANDLE thread, ULONG_PTR stack_low, ULONG_PTR stack_high, U64 id, bool& same_frame, int max_frames = 40, bool any_frame = false)
    {
        std::string stack;
#if defined(_M_X64)
        if (mCopy.empty())
        {
            mCopy.resize(4 * 1024 * 1024);
        }
        CONTEXT ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_FULL;
        size_t len = 0;
        DWORD64 orig_rsp = 0;
        if (SuspendThread(thread) == (DWORD)-1)
        {
            return stack;
        }
        // Only copying while it is suspended: no allocation, no lock, no logging.
        same_frame = any_frame || sFrameId.load() == id;
        bool have_context = same_frame && GetThreadContext(thread, &ctx);
        if (have_context && (ctx.Rsp < stack_low || ctx.Rsp >= stack_high))
        {
            // Running on another stack, a coroutine's: its committed range up from the stack pointer.
            // VirtualQuery takes no lock the suspended thread could hold.
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery((LPCVOID)ctx.Rsp, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT)
            {
                stack_low = (ULONG_PTR)mbi.BaseAddress;
                stack_high = (ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;
            }
        }
        if (have_context && ctx.Rsp >= stack_low && ctx.Rsp < stack_high)
        {
            orig_rsp = ctx.Rsp;
            len = llmin((size_t)(stack_high - orig_rsp), mCopy.size()) & ~(size_t)7;
            memcpy(mCopy.data(), (const void*)orig_rsp, len);
        }
        ResumeThread(thread);
        if (!same_frame)
        {
            return stack;
        }
        if (len)
        {
            // rebase whatever points into the stack (frame pointers, saved registers) onto the copy
            const DWORD64 delta = (DWORD64)mCopy.data() - orig_rsp;
            const DWORD64 hi = orig_rsp + len;
            for (size_t off = 0; off + 8 <= len; off += 8)
            {
                DWORD64* p = (DWORD64*)(mCopy.data() + off);
                if (*p >= orig_rsp && *p < hi)
                {
                    *p += delta;
                }
            }
            DWORD64* regs[] = { &ctx.Rsp, &ctx.Rbp, &ctx.Rbx, &ctx.Rsi, &ctx.Rdi, &ctx.R12, &ctx.R13, &ctx.R14, &ctx.R15 };
            for (DWORD64* r : regs)
            {
                if (*r >= orig_rsp && *r < hi)
                {
                    *r += delta;
                }
            }
            DWORD64 pcs[40];
            const int n = unwind_copy(ctx, mCopy.data(), len, pcs, llclamp(max_frames, 1, 40));
            stack = symbolize(pcs, n);
        }
#endif
        return stack;
    }

    std::string symbolize(const DWORD64* pcs, int n)
    {
        HANDLE process = GetCurrentProcess();
        if (!mSymInitDone)
        {
            mSymInitDone = true;
            SymSetOptions(SymGetOptions() | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
            SymInitialize(process, NULL, TRUE); // fails harmlessly when someone already did
        }
        std::string out;
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
        for (int i = 0; i < n; ++i)
        {
            // return addresses point after the call
            const DWORD64 pc = i == 0 ? pcs[i] : pcs[i] - 1;
            std::string name;
            SYMBOL_INFO* sym = (SYMBOL_INFO*)buffer;
            memset(buffer, 0, sizeof(buffer));
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 511;
            DWORD64 displacement = 0;
            if (SymFromAddr(process, pc, &displacement, sym))
            {
                name = sym->Name;
                IMAGEHLP_LINE64 line{};
                line.SizeOfStruct = sizeof(line);
                DWORD line_disp = 0;
                if (SymGetLineFromAddr64(process, pc, &line_disp, &line) && line.FileName)
                {
                    std::string file = line.FileName;
                    const size_t slash = file.find_last_of("\\/");
                    if (slash != std::string::npos)
                    {
                        file = file.substr(slash + 1);
                    }
                    name += llformat(" (%s:%u)", file.c_str(), (U32)line.LineNumber);
                }
            }
            else
            {
                HMODULE module = NULL;
                char path[MAX_PATH] = "?";
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCSTR)pc, &module) && module)
                {
                    GetModuleFileNameA(module, path, MAX_PATH);
                }
                std::string mod = path;
                const size_t slash = mod.find_last_of("\\/");
                if (slash != std::string::npos)
                {
                    mod = mod.substr(slash + 1);
                }
                name = llformat("%s+0x%llx", mod.c_str(), module ? (unsigned long long)(pc - (DWORD64)module) : (unsigned long long)pc);
            }
            if (!out.empty())
            {
                out += " <- ";
            }
            out += name;
        }
        return out;
    }

    std::thread mThread;
    std::mutex mMutex;
    std::condition_variable mCv;
    bool mStop = false;
    HANDLE mMain = NULL;
    ULONG_PTR mStackLow = 0;
    ULONG_PTR mStackHigh = 0;
    std::vector<U8> mCopy;
    bool mSymInitDone = false;
    U64 mOthersLoggedFrame = 0;
};

Watcher sWatcher;
Counters sFrameCounters;
bool sHaveCounters = false;
#endif // LL_WINDOWS
} // namespace

void FSStallWatch::frameMark(bool watching, F32 stack_ms)
{
    if (sShutdown.load())
    {
        return;
    }
#if LL_WINDOWS
    const Counters now = read_counters(GetCurrentThread());
    if (sHaveCounters)
    {
        sLastCosts = llformat("main thread CPU %.0f ms (kernel %.0f), page faults %u, process I/O read %.1f MB write %.1f MB",
                              (now.mUser + now.mKernel - sFrameCounters.mUser - sFrameCounters.mKernel) / 10000.0,
                              (now.mKernel - sFrameCounters.mKernel) / 10000.0, now.mFaults - sFrameCounters.mFaults,
                              (now.mRead - sFrameCounters.mRead) / 1048576.0, (now.mWrite - sFrameCounters.mWrite) / 1048576.0);
    }
    sFrameCounters = now;
    sHaveCounters = true;
    sFrameCpuStart.store(now.mUser + now.mKernel);
    if (watching && stack_ms > 0.f)
    {
        sWatcher.start();
    }
#endif
    sStackMs.store(stack_ms);
    sFrameStartUs.store(now_us());
    sFrameId.fetch_add(1);
    sWatching.store(watching);
}

std::string FSStallWatch::lastFrameCosts()
{
    return sLastCosts;
}

void FSStallWatch::shutdown()
{
    sShutdown.store(true);
    sWatching.store(false);
#if LL_WINDOWS
    sWatcher.stop();
#endif
}
