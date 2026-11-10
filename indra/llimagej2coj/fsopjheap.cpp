/**
 * @file fsopjheap.cpp
 * @brief Per thread reuse of the OpenJPEG decoder's large buffers (FSPerfOpenJPEGBufferCacheMB)
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

// Every OpenJPEG decode allocates its working buffers afresh (32 bit samples for every component of the
// image and of the tile, the stream buffer, code block data) and frees them at the end. Blocks that big
// bypass the heap's free lists on Windows: each comes straight from the OS and goes back to it, so every
// page of it is faulted in and zeroed by the kernel again on the next decode. With the decode threads
// all busy (logging in, arriving somewhere) that is gigabytes a second of page faults, and the memory
// manager work it causes stalls other programs too, not just the viewer.
//
// openjp2.dll takes its memory from the C runtime (malloc, calloc, realloc, free, _aligned_malloc,
// _aligned_free). Those six imports are pointed here: small blocks still come from the C runtime, large
// ones (256 KB and up) are kept by the thread that freed them, up to FSPerfOpenJPEGBufferCacheMB per
// thread, and handed out again to the next decode on that thread that asks for one of the same size
// class. Every block carries a header saying where it came from, so a block allocated before the imports
// were redirected still goes back to the C runtime.

#include "linden_common.h"

#include "fsopjheap.h"

#include "llimagej2c.h"

#include <mutex>

#if LL_WINDOWS
#include "llwin32headers.h"

namespace
{
    typedef void* (__cdecl* malloc_t)(size_t);
    typedef void* (__cdecl* calloc_t)(size_t, size_t);
    typedef void* (__cdecl* realloc_t)(void*, size_t);
    typedef void (__cdecl* free_t)(void*);
    typedef void* (__cdecl* aligned_malloc_t)(size_t, size_t);
    typedef void (__cdecl* aligned_free_t)(void*);

    malloc_t sRealMalloc = nullptr;
    calloc_t sRealCalloc = nullptr;
    realloc_t sRealRealloc = nullptr;
    free_t sRealFree = nullptr;
    aligned_malloc_t sRealAlignedMalloc = nullptr;
    aligned_free_t sRealAlignedFree = nullptr;

    constexpr U64 MAGIC = 0x50414548504F5346ull;   // "FSOPHEAP"
    constexpr size_t HEADER = 64;                   // before every block; keeps up to 64 byte alignment
    constexpr size_t LARGE_MIN = 256 * 1024;        // from here blocks are kept for reuse
    constexpr U32 CLASSES = 48;
    size_t sCacheLimit = 0;                         // bytes kept per thread

    struct Header
    {
        U64 mMagic;
        void* mBase;        // what was allocated
        size_t mCapacity;   // usable bytes from the block
        Header* mNext;      // in its thread's cache
        U32 mLarge;         // from VirtualAlloc, kept for reuse
        U32 mClass;
    };
    static_assert(sizeof(Header) <= HEADER, "FSOpjHeap header");

    inline Header* header_of(void* p)
    {
        return reinterpret_cast<Header*>(static_cast<char*>(p) - HEADER);
    }

    // size classes 256 KB, 384 KB, 512 KB, 768 KB, ...: at most a third more than asked for
    U32 size_class(size_t n, size_t& capacity)
    {
        size_t c = LARGE_MIN;
        for (U32 index = 0; index + 1 < CLASSES; index += 2, c <<= 1)
        {
            if (n <= c)
            {
                capacity = c;
                return index;
            }
            if (n <= c + c / 2)
            {
                capacity = c + c / 2;
                return index + 1;
            }
        }
        return CLASSES;
    }

    struct Cache
    {
        Header* mBins[CLASSES] = {};
        size_t mBytes = 0;

        ~Cache()
        {
            for (Header*& bin : mBins)
            {
                while (Header* h = bin)
                {
                    bin = h->mNext;
                    h->mMagic = 0;
                    VirtualFree(h->mBase, 0, MEM_RELEASE);
                }
            }
            mBytes = 0;
        }
    };
    thread_local Cache tCache;

    void* allocate(size_t n, size_t align, bool zero)
    {
        if (align < 16)
        {
            align = 16;
        }
        if (n >= LARGE_MIN && align <= HEADER)
        {
            size_t capacity = 0;
            const U32 cls = size_class(n, capacity);
            if (cls < CLASSES)
            {
                Cache& cache = tCache;
                if (Header* h = cache.mBins[cls])
                {
                    cache.mBins[cls] = h->mNext;
                    cache.mBytes -= h->mCapacity;
                    h->mNext = nullptr;
                    void* p = reinterpret_cast<char*>(h) + HEADER;
                    if (zero)
                    {
                        memset(p, 0, n);
                    }
                    return p;
                }
                void* base = VirtualAlloc(nullptr, capacity + HEADER, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (!base)
                {
                    return nullptr;
                }
                Header* h = static_cast<Header*>(base);
                h->mMagic = MAGIC;
                h->mBase = base;
                h->mCapacity = capacity;
                h->mNext = nullptr;
                h->mLarge = 1;
                h->mClass = cls;
                return static_cast<char*>(base) + HEADER;   // fresh pages are zero already
            }
        }

        if ((align & (align - 1)) != 0 || n > SIZE_MAX - HEADER - align)
        {
            return nullptr;
        }
        void* base = sRealMalloc(n + HEADER + align);
        if (!base)
        {
            return nullptr;
        }
        const uintptr_t user = (reinterpret_cast<uintptr_t>(base) + HEADER + align - 1) & ~(uintptr_t)(align - 1);
        Header* h = reinterpret_cast<Header*>(user - HEADER);
        h->mMagic = MAGIC;
        h->mBase = base;
        h->mCapacity = n;
        h->mNext = nullptr;
        h->mLarge = 0;
        h->mClass = 0;
        if (zero)
        {
            memset(reinterpret_cast<void*>(user), 0, n);
        }
        return reinterpret_cast<void*>(user);
    }

    // false: not one of ours (allocated before the imports were redirected)
    bool release(void* p)
    {
        Header* h = header_of(p);
        if (h->mMagic != MAGIC)
        {
            return false;
        }
        if (h->mLarge)
        {
            Cache& cache = tCache;
            if (cache.mBytes + h->mCapacity <= sCacheLimit)
            {
                h->mNext = cache.mBins[h->mClass];
                cache.mBins[h->mClass] = h;
                cache.mBytes += h->mCapacity;
                return true;
            }
            h->mMagic = 0;
            VirtualFree(h->mBase, 0, MEM_RELEASE);
            return true;
        }
        h->mMagic = 0;
        sRealFree(h->mBase);
        return true;
    }

    void* __cdecl fs_malloc(size_t n)
    {
        return allocate(n, 16, false);
    }

    void* __cdecl fs_calloc(size_t count, size_t size)
    {
        if (size && count > SIZE_MAX / size)
        {
            return nullptr;
        }
        return allocate(count * size, 16, true);
    }

    void __cdecl fs_free(void* p)
    {
        if (p && !release(p))
        {
            sRealFree(p);
        }
    }

    void* __cdecl fs_realloc(void* p, size_t n)
    {
        if (!p)
        {
            return allocate(n, 16, false);
        }
        Header* h = header_of(p);
        if (h->mMagic != MAGIC)
        {
            return sRealRealloc(p, n);
        }
        if (n == 0)
        {
            release(p);
            return nullptr;
        }
        if (n <= h->mCapacity)
        {
            return p;
        }
        void* q = allocate(n, 16, false);
        if (q)
        {
            memcpy(q, p, h->mCapacity);
            release(p);
        }
        return q;
    }

    void* __cdecl fs_aligned_malloc(size_t n, size_t align)
    {
        return allocate(n, align, false);
    }

    void __cdecl fs_aligned_free(void* p)
    {
        if (p && !release(p))
        {
            sRealAlignedFree(p);
        }
    }

    // Points openjp2.dll's C runtime heap imports here: all six or none
    bool redirect(HMODULE module)
    {
        char* base = reinterpret_cast<char*>(module);
        const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress)
        {
            return false;
        }

        struct Slot
        {
            const char* mName;
            void* mReplacement;
            ULONGLONG* mEntry = nullptr;
        };
        Slot slots[] =
        {
            { "malloc", reinterpret_cast<void*>(&fs_malloc) },
            { "calloc", reinterpret_cast<void*>(&fs_calloc) },
            { "realloc", reinterpret_cast<void*>(&fs_realloc) },
            { "free", reinterpret_cast<void*>(&fs_free) },
            { "_aligned_malloc", reinterpret_cast<void*>(&fs_aligned_malloc) },
            { "_aligned_free", reinterpret_cast<void*>(&fs_aligned_free) },
        };

        for (const IMAGE_IMPORT_DESCRIPTOR* imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
             imp->Name; ++imp)
        {
            const char* dll = base + imp->Name;
            if (_stricmp(dll, "api-ms-win-crt-heap-l1-1-0.dll") != 0 && _stricmp(dll, "ucrtbase.dll") != 0)
            {
                continue;
            }
            const IMAGE_THUNK_DATA* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(
                base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
            IMAGE_THUNK_DATA* funcs = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
            for (; names->u1.AddressOfData; ++names, ++funcs)
            {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                {
                    continue;
                }
                const IMAGE_IMPORT_BY_NAME* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                for (Slot& slot : slots)
                {
                    if (!strcmp(reinterpret_cast<const char*>(by_name->Name), slot.mName))
                    {
                        slot.mEntry = reinterpret_cast<ULONGLONG*>(&funcs->u1.Function);
                    }
                }
            }
        }
        for (const Slot& slot : slots)
        {
            if (!slot.mEntry)
            {
                LL_WARNS("Texture") << "FSPerfOpenJPEGBufferCacheMB: openjp2 doesn't import " << slot.mName
                                    << ", its buffers aren't kept" << LL_ENDL;
                return false;
            }
        }

        sRealMalloc = reinterpret_cast<malloc_t>(*slots[0].mEntry);
        sRealCalloc = reinterpret_cast<calloc_t>(*slots[1].mEntry);
        sRealRealloc = reinterpret_cast<realloc_t>(*slots[2].mEntry);
        sRealFree = reinterpret_cast<free_t>(*slots[3].mEntry);
        sRealAlignedMalloc = reinterpret_cast<aligned_malloc_t>(*slots[4].mEntry);
        sRealAlignedFree = reinterpret_cast<aligned_free_t>(*slots[5].mEntry);

        for (const Slot& slot : slots)
        {
            DWORD old_protect = 0;
            if (!VirtualProtect(slot.mEntry, sizeof(ULONGLONG), PAGE_READWRITE, &old_protect))
            {   // only possible for the first: they all sit in the same import table
                LL_WARNS("Texture") << "FSPerfOpenJPEGBufferCacheMB: can't write openjp2's import table" << LL_ENDL;
                return false;
            }
            InterlockedExchange64(reinterpret_cast<volatile LONG64*>(slot.mEntry), reinterpret_cast<LONG64>(slot.mReplacement));
            VirtualProtect(slot.mEntry, sizeof(ULONGLONG), old_protect, &old_protect);
        }
        return true;
    }
}

void FSOpjHeap::install()
{
    static std::once_flag once;
    std::call_once(once, []()
    {
        const U32 mb = LLImageJ2C::sFSOpenJPEGBufferCacheMB.load();
        if (!mb)
        {
            return;
        }
        HMODULE module = GetModuleHandleW(L"openjp2.dll");
        if (!module)
        {
            return;
        }
        sCacheLimit = (size_t)mb * 1024 * 1024;
        if (redirect(module))
        {
            LL_INFOS("Texture") << "OpenJPEG keeps up to " << mb << " MB of its large buffers per decode thread" << LL_ENDL;
        }
    });
}

#else // LL_WINDOWS

void FSOpjHeap::install()
{
}

#endif // LL_WINDOWS
