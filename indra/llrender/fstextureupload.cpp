/**
 * @file fstextureupload.cpp
 * @brief Streaming texture uploads through a persistently mapped pixel
 *        buffer object ring (FSPerfTextureUploadPBO).
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

#include "fstextureupload.h"
#include "fsglstream.h"

#include "llgl.h"
#include "llthread.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

namespace
{
    // 32 MB of host-visible staging memory. The largest level that may use it
    // is half of that: a 2048x2048 RGBA (or RGB expanded to RGBA) base level.
    constexpr U32 RING_SIZE = 32u * 1024u * 1024u;
    constexpr U32 MAX_UPLOAD = RING_SIZE / 2;
    // Below this the map/fence bookkeeping costs more than the copy it saves.
    constexpr U32 MIN_UPLOAD = 32u * 1024u;
    constexpr U32 OFFSET_ALIGN = 256;
    constexpr size_t MAX_IN_FLIGHT = 512;

    enum : U8
    {
        REGION_STAGED,      // written (or being written), its upload not recorded yet
        REGION_IN_FLIGHT,   // upload recorded, waiting for the GPU to have read it
        REGION_DONE         // free once every older region is too
    };

    struct UploadRegion
    {
        U64 mSeq;
        U32 mOffset;
        U32 mSize;
        GLsync mFence;      // without the render thread only
        U8 mState;
    };

    std::atomic<bool> sEnabled{ false };
    bool sFailed = false;   // creation failed or unsupported; stays off until releaseGL()
    std::atomic<bool> sStreamMode{ false }; // made while the render thread runs: its fences live there
    thread_local bool sInScope = false; // only ever set on the main thread
    GLuint sBuffer = 0;
    std::atomic<U8*> sMapped{ nullptr };
    FSTextureUpload::Stats sStats;      // main thread
    std::atomic<U32> sStagedCount{ 0 };
    std::atomic<U32> sStageFullCount{ 0 };

    // Region bookkeeping, shared by the main thread, the texture thread (stage) and the render
    // thread (fences). Held only for bookkeeping, never across a copy or a GL call.
    std::mutex sMutex;
    std::deque<UploadRegion> sRegions;        // in ring order
    U64 sNextSeq = 1;                   // never reset, so a stale Staged never matches
    U32 sHead = 0;
    // Render thread fences, oldest first (render thread only, plus releaseGL once it has stopped)
    std::deque<std::pair<U64, GLsync>> sRenderFences;
    std::atomic<bool> sPollQueued{ false };

    // Held by whoever copies into the mapping, so releaseGL() can't unmap it underneath them
    std::mutex sCopyMutex;

    // sMutex held
    UploadRegion* find_locked(U64 seq)
    {
        if (sRegions.empty() || seq < sRegions.front().mSeq)
        {
            return nullptr;
        }
        const U64 index = seq - sRegions.front().mSeq;
        if (index >= sRegions.size())
        {
            return nullptr;
        }
        UploadRegion& r = sRegions[(size_t)index];
        return r.mSeq == seq ? &r : nullptr;
    }

    bool is_supported()
    {
        return gGLManager.mGLVersion >= 4.39f &&
            glBufferStorage != nullptr &&
            glMapBufferRange != nullptr &&
            glFenceSync != nullptr &&
            glClientWaitSync != nullptr &&
            glDeleteSync != nullptr;
    }

    // Main thread
    bool ensure_buffer()
    {
        if (sMapped.load(std::memory_order_acquire))
        {
            return true;
        }
        if (sFailed)
        {
            return false;
        }
        if (!is_supported())
        {
            LL_INFOS("Texture") << "FSPerfTextureUploadPBO: needs OpenGL 4.4 (glBufferStorage); using normal uploads" << LL_ENDL;
            sFailed = true;
            return false;
        }

        // With the render thread, glMapBufferRange is one synchronous call, once
        const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
        glGenBuffers(1, &sBuffer);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, sBuffer);
        glBufferStorage(GL_PIXEL_UNPACK_BUFFER, RING_SIZE, nullptr, flags);
        U8* mapped = (U8*)glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, RING_SIZE, flags);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

        if (!mapped)
        {
            LL_WARNS("Texture") << "FSPerfTextureUploadPBO: could not map the upload ring; using normal uploads" << LL_ENDL;
            glDeleteBuffers(1, &sBuffer);
            sBuffer = 0;
            sFailed = true;
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(sMutex);
            sHead = 0;
        }
        sStreamMode.store(FSGLStream::isRecording(), std::memory_order_release);
        sMapped.store(mapped, std::memory_order_release);
        LL_INFOS("Texture") << "FSPerfTextureUploadPBO: " << (RING_SIZE >> 20) << " MB upload ring created"
                            << (sStreamMode.load() ? " (render thread)" : "") << LL_ENDL;
        return true;
    }

    // sMutex held. Frees the regions the GPU is done with, oldest first. Without the render
    // thread the main thread polls their fences here (and only it gets here then).
    void retire_locked()
    {
        while (!sRegions.empty())
        {
            UploadRegion& r = sRegions.front();
            if (r.mState == REGION_IN_FLIGHT && r.mFence)
            {
                const GLenum status = glClientWaitSync(r.mFence, 0, 0);
                if (status == GL_TIMEOUT_EXPIRED)
                {
                    break;
                }
                if (status == GL_WAIT_FAILED)
                {
                    // The fence can't tell us: make sure the GPU is done with the region before it
                    // is written again
                    glFinish();
                }
                glDeleteSync(r.mFence);
                r.mFence = nullptr;
                r.mState = REGION_DONE;
            }
            if (r.mState != REGION_DONE)
            {
                break;
            }
            sRegions.pop_front();
        }
        if (sRegions.empty())
        {
            sHead = 0;
        }
    }

    // sMutex held. Finds room for size bytes without overwriting anything still in use, and
    // adds it as a staged region.
    bool allocate_locked(U32 size, U32& offset, U64& seq)
    {
        bool ok = false;
        if (sRegions.empty())
        {
            offset = 0;
            ok = size <= RING_SIZE;
        }
        else if (sRegions.size() < MAX_IN_FLIGHT)
        {
            const U32 tail = sRegions.front().mOffset;
            const bool wrapped = sRegions.back().mOffset < tail;
            if (!wrapped)
            {
                // In use: [tail, sHead). Free: [sHead, end) and [0, tail).
                if (sHead + size <= RING_SIZE)
                {
                    offset = sHead;
                    ok = true;
                }
                else if (size <= tail)
                {
                    offset = 0;
                    ok = true;
                }
            }
            else if (sHead + size <= tail)
            {
                // In use: [tail, end) and [0, sHead). Free: [sHead, tail).
                offset = sHead;
                ok = true;
            }
        }
        if (!ok)
        {
            return false;
        }
        sHead = offset + size;
        seq = sNextSeq++;
        sRegions.push_back({ seq, offset, size, nullptr, REGION_STAGED });
        return true;
    }

    // Render thread, in stream order right after the upload from region seq
    void fence_on_render(U64 seq)
    {
        GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!fence)
        {
            // Can't track when the bytes are free again; make sure they are.
            glFinish();
        }
        std::lock_guard<std::mutex> lock(sMutex);
        if (fence)
        {
            sRenderFences.emplace_back(seq, fence);
        }
        else if (UploadRegion* r = find_locked(seq))
        {
            r->mState = REGION_DONE;
        }
    }

    // Render thread: marks the regions whose fences have signalled as done, oldest first.
    // Never waits.
    void poll_on_render(U64)
    {
        while (true)
        {
            std::pair<U64, GLsync> front;
            {
                std::lock_guard<std::mutex> lock(sMutex);
                if (sRenderFences.empty())
                {
                    break;
                }
                front = sRenderFences.front();
            }
            const GLenum status = glClientWaitSync(front.second, 0, 0);
            if (status == GL_TIMEOUT_EXPIRED)
            {
                break;
            }
            if (status == GL_WAIT_FAILED)
            {
                glFinish();
            }
            glDeleteSync(front.second);
            std::lock_guard<std::mutex> lock(sMutex);
            sRenderFences.pop_front();
            if (UploadRegion* r = find_locked(front.first))
            {
                r->mState = REGION_DONE;
            }
        }
        sPollQueued.store(false, std::memory_order_release);
    }

    // Render thread, in stream order: drops the fences (releaseGL() while the stream runs)
    void clear_on_render(U64)
    {
        std::deque<std::pair<U64, GLsync>> fences;
        {
            std::lock_guard<std::mutex> lock(sMutex);
            fences.swap(sRenderFences);
        }
        for (auto& fence : fences)
        {
            glDeleteSync(fence.second);
        }
    }

    // Main thread, after an upload from region seq was issued (recorded with the render thread)
    void fence_region(U64 seq)
    {
        if (sStreamMode.load(std::memory_order_acquire))
        {
            {
                std::lock_guard<std::mutex> lock(sMutex);
                if (UploadRegion* r = find_locked(seq))
                {
                    r->mState = REGION_IN_FLIGHT;
                }
            }
            FSGLStream::recordCall(&fence_on_render, seq);
            return;
        }
        GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!fence)
        {
            // Can't track when the bytes are free again; make sure they are.
            glFinish();
        }
        std::lock_guard<std::mutex> lock(sMutex);
        if (UploadRegion* r = find_locked(seq))
        {
            r->mFence = fence;
            r->mState = fence ? REGION_IN_FLIGHT : REGION_DONE;
        }
        else if (fence)
        {
            glDeleteSync(fence);
        }
    }

    // How a level is laid out in the ring: false when it can't go through it
    bool upload_format(U32 pixformat, U32 pixtype, S32 width, S32 height, U32& format, bool& expand_rgb, U64& bytes)
    {
        if (pixtype != GL_UNSIGNED_BYTE || width <= 0 || height <= 0)
        {
            return false;
        }
        // Formats the driver can copy straight from the buffer. 24-bit RGB is
        // expanded to RGBA here so the driver never has to read the
        // (uncached) buffer back to swizzle it. GL_UNPACK_ALIGNMENT is 1
        // (LLRender::init) and GL_UNPACK_ROW_LENGTH 0 in setImage, so rows are
        // tightly packed.
        U32 src_components = 0;
        format = pixformat;
        expand_rgb = false;
        switch (pixformat)
        {
        case GL_RGBA:
        case GL_BGRA:
            src_components = 4;
            break;
        case GL_RGB:
            src_components = 3;
            format = GL_RGBA;
            expand_rgb = true;
            break;
        case GL_RG:
            src_components = 2;
            break;
        case GL_RED:
            src_components = 1;
            break;
        default:
            return false;
        }
        bytes = (U64)width * (U64)height * (expand_rgb ? 4 : src_components);
        return bytes >= MIN_UPLOAD && bytes <= MAX_UPLOAD;
    }

    void copy_rgb_to_rgba(const U8* src, U8* dst, U32 pixel_count)
    {
        // Sequential 32-bit writes; the destination is write-combined memory.
        U32* out = (U32*)dst;
        for (U32 i = 0; i < pixel_count; ++i)
        {
            out[i] = (U32)src[0] | ((U32)src[1] << 8) | ((U32)src[2] << 16) | 0xFF000000u;
            src += 3;
        }
    }

    // sCopyMutex held. Allocates and fills a region; false when there is no room.
    bool fill(const void* pixels, S32 width, S32 height, bool expand_rgb, U64 bytes, U32& offset, U64& seq)
    {
        U8* mapped = sMapped.load(std::memory_order_acquire);
        if (!mapped)
        {
            return false;
        }
        const U32 aligned = ((U32)bytes + OFFSET_ALIGN - 1) & ~(OFFSET_ALIGN - 1);
        {
            std::lock_guard<std::mutex> lock(sMutex);
            retire_locked();
            if (!allocate_locked(aligned, offset, seq))
            {
                return false;
            }
        }
        if (expand_rgb)
        {
            copy_rgb_to_rgba((const U8*)pixels, mapped + offset, (U32)((U64)width * (U64)height));
        }
        else
        {
            memcpy(mapped + offset, pixels, (size_t)bytes);
        }
        return true;
    }
}

namespace FSTextureUpload
{
    void setPBOEnabled(bool enabled)
    {
        // A ring made with the render thread keeps its fences there, one made without it on the
        // main thread: start over when that changed
        if (sMapped.load(std::memory_order_acquire) && sStreamMode.load() != FSGLStream::isRecording())
        {
            releaseGL();
        }
        if (sEnabled && !enabled)
        {
            releaseGL();
        }
        sEnabled = enabled;

        if (enabled && FSGLStream::isRecording())
        {
            // The texture thread can't make the ring (no GL there), so it is made up front
            if (ensure_buffer())
            {
                // Fences are polled once a frame on the render thread
                bool pending = false;
                {
                    std::lock_guard<std::mutex> lock(sMutex);
                    pending = !sRenderFences.empty() || !sRegions.empty();
                }
                if (pending && !sPollQueued.exchange(true, std::memory_order_acq_rel))
                {
                    FSGLStream::recordCall(&poll_on_render, 0);
                }
            }
        }
    }

    bool isPBOEnabled()
    {
        return sEnabled;
    }

    void releaseGL()
    {
        // Wait out a texture thread copy into the mapping
        std::lock_guard<std::mutex> copy_lock(sCopyMutex);
        U8* mapped = sMapped.exchange(nullptr, std::memory_order_acq_rel);
        // No GL call while sMutex is held: a recorded one can wait for room in the stream while
        // the render thread waits for sMutex
        std::vector<GLsync> fences;
        {
            std::lock_guard<std::mutex> lock(sMutex);
            for (UploadRegion& r : sRegions)
            {
                if (r.mFence)
                {
                    fences.push_back(r.mFence);
                }
            }
            sRegions.clear();
            sHead = 0;
        }
        for (GLsync fence : fences)
        {
            glDeleteSync(fence);
        }
        if (FSGLStream::isRecording())
        {
            // The render thread may be polling them right now: drop them in stream order
            FSGLStream::recordCall(&clear_on_render, 0);
        }
        else
        {
            clear_on_render(0);
        }
        if (sBuffer)
        {
            if (mapped)
            {
                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, sBuffer);
                glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            }
            glDeleteBuffers(1, &sBuffer);
        }
        sBuffer = 0;
        sFailed = false;
        sStreamMode = false;
    }

    bool texImage2D(U32 target, S32 miplevel, S32 intformat, S32 width, S32 height,
                    U32 pixformat, U32 pixtype, const void* pixels)
    {
        if (!sEnabled || !sInScope || !pixels)
        {
            return false;
        }

        U32 format = 0;
        bool expand_rgb = false;
        U64 upload_bytes = 0;
        if (!upload_format(pixformat, pixtype, width, height, format, expand_rgb, upload_bytes))
        {
            return false;
        }

        if (!ensure_buffer())
        {
            return false;
        }

        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("glTexImage2D PBO");

        U32 offset = 0;
        U64 seq = 0;
        {
            std::lock_guard<std::mutex> copy_lock(sCopyMutex);
            if (!fill(pixels, width, height, expand_rgb, upload_bytes, offset, seq))
            {
                ++sStats.mRingFull;
                return false;
            }
        }

        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, sBuffer);
        glTexImage2D(target, miplevel, intformat, width, height, 0, format, GL_UNSIGNED_BYTE, (const void*)(uintptr_t)offset);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        fence_region(seq);

        sStats.mPBOBytes += upload_bytes;
        ++sStats.mPBOUploads;
        return true;
    }

    bool canStage()
    {
        return sEnabled.load(std::memory_order_relaxed) && sStreamMode.load(std::memory_order_acquire) &&
               sMapped.load(std::memory_order_acquire) != nullptr;
    }

    bool stage(const void* pixels, S32 width, S32 height, U32 pixformat, U32 pixtype, Staged& out)
    {
        out = Staged();
        if (!pixels || !canStage())
        {
            return false;
        }
        U32 format = 0;
        bool expand_rgb = false;
        U64 bytes = 0;
        if (!upload_format(pixformat, pixtype, width, height, format, expand_rgb, bytes))
        {
            return false;
        }

        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("stage upload");
        U32 offset = 0;
        U64 seq = 0;
        {
            std::lock_guard<std::mutex> copy_lock(sCopyMutex);
            if (!sStreamMode.load(std::memory_order_acquire) || !fill(pixels, width, height, expand_rgb, bytes, offset, seq))
            {
                sStageFullCount.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
        out.mSeq = seq;
        out.mOffset = offset;
        out.mFormat = format;
        out.mWidth = width;
        out.mHeight = height;
        sStagedCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool isStaged(const Staged& staged)
    {
        if (!staged.mSeq || !sMapped.load(std::memory_order_acquire))
        {
            return false;
        }
        std::lock_guard<std::mutex> lock(sMutex);
        const UploadRegion* r = find_locked(staged.mSeq);
        return r && r->mState == REGION_STAGED;
    }

    bool texImage2DStaged(Staged& staged, U32 target, S32 miplevel, S32 intformat)
    {
        const Staged s = staged;
        staged = Staged();
        if (!isStaged(s))
        {
            return false;
        }
        LL_PROFILE_ZONE_NAMED_CATEGORY_TEXTURE("glTexImage2D staged");
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, sBuffer);
        glTexImage2D(target, miplevel, intformat, s.mWidth, s.mHeight, 0, s.mFormat, GL_UNSIGNED_BYTE, (const void*)(uintptr_t)s.mOffset);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        fence_region(s.mSeq);
        const U64 components = (s.mFormat == GL_RGBA || s.mFormat == GL_BGRA) ? 4 : (s.mFormat == GL_RG ? 2 : 1);
        sStats.mPBOBytes += (U64)s.mWidth * (U64)s.mHeight * components;
        ++sStats.mPBOUploads;
        return true;
    }

    void release(Staged& staged)
    {
        if (staged.mSeq)
        {
            std::lock_guard<std::mutex> lock(sMutex);
            UploadRegion* r = find_locked(staged.mSeq);
            if (r && r->mState == REGION_STAGED)
            {
                r->mState = REGION_DONE;
            }
        }
        staged = Staged();
    }

    UploadScope::UploadScope()
        : mPrev(sInScope)
    {
        if (sEnabled && on_main_thread())
        {
            sInScope = true;
        }
    }

    UploadScope::~UploadScope()
    {
        sInScope = mPrev;
    }

    Stats takeStats()
    {
        Stats out = sStats;
        out.mStaged = sStagedCount.exchange(0, std::memory_order_relaxed);
        out.mStageFull = sStageFullCount.exchange(0, std::memory_order_relaxed);
        sStats = Stats();
        return out;
    }
}
