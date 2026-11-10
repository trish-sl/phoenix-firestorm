/**
 * @file fstextureupload.h
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

#ifndef FS_TEXTUREUPLOAD_H
#define FS_TEXTUREUPLOAD_H

#include "stdtypes.h"

// Main-thread texture uploads normally hand glTexImage2D a pointer to client
// memory. The driver must copy (and for 24-bit RGB, usually swizzle to RGBA
// on the CPU) the whole level before the call returns, inside the frame.
//
// With the ring enabled, LLImageGL::setImage() copies each eligible level
// into a persistently mapped GL_PIXEL_UNPACK_BUFFER (expanding RGB to RGBA
// while copying, so the driver can DMA it without conversion) and issues
// glTexImage2D from the buffer offset. A fence per upload guards the bytes
// until the GPU has consumed them. When the ring is full the upload falls
// back to the old client-memory path rather than waiting, so this can never
// add a stall.
//
// Only for uncompressed 8-bit formats, and only on GL 4.4+ (glBufferStorage).
//
// With the render thread (FSRenderThread) the uploads and their fences are
// recorded like any other GL call: the fences are made and polled on the render
// thread, so nothing on the main thread ever waits for one. The texture thread
// can then also stage levels in the ring without a GL context of its own
// (stage()), and the main thread records the upload from there
// (texImage2DStaged()): only the render thread's context touches the driver.
//
// Main thread only, except stage() and release().
namespace FSTextureUpload
{
    struct Stats
    {
        U64 mPBOBytes = 0;      // bytes written to the ring
        U32 mPBOUploads = 0;    // levels uploaded from the ring
        U32 mRingFull = 0;      // eligible levels that fell back because the ring was busy
        U32 mStaged = 0;        // levels the texture thread staged (no GL context used)
        U32 mStageFull = 0;     // levels it couldn't stage because the ring was busy
    };

    // Call once per frame from the main thread with the setting value.
    // Turning it off releases the ring.
    void setPBOEnabled(bool enabled);
    bool isPBOEnabled();

    // A level the texture thread copied into the ring, waiting for the main
    // thread to upload it.
    struct Staged
    {
        U64 mSeq = 0;           // 0: nothing staged
        U32 mOffset = 0;
        U32 mFormat = 0;        // format of the bytes in the ring (RGB is expanded to RGBA)
        S32 mWidth = 0;
        S32 mHeight = 0;
    };

    // Whether stage() can be used now: the ring exists and its uploads run on the render thread.
    bool canStage();
    // Any thread. Copies a level into the ring; false (nothing staged) when it isn't eligible
    // or there is no room right now.
    bool stage(const void* pixels, S32 width, S32 height, U32 pixformat, U32 pixtype, Staged& out);
    // Main thread. Whether staged is still in the ring, waiting for its upload (releaseGL()
    // drops everything staged).
    bool isStaged(const Staged& staged);
    // Main thread. glTexImage2D of a staged level into the texture bound to target. False when
    // it is no longer staged. Consumes it either way.
    bool texImage2DStaged(Staged& staged, U32 target, S32 miplevel, S32 intformat);
    // Any thread. Gives back a staged level that won't be uploaded.
    void release(Staged& staged);

    // Releases the buffer and fences (GL shutdown or context loss).
    void releaseGL();

    // Issues glTexImage2D from the ring when the upload is eligible and there
    // is room. Returns false if the caller must do the upload itself.
    bool texImage2D(U32 target, S32 miplevel, S32 intformat, S32 width, S32 height,
                    U32 pixformat, U32 pixtype, const void* pixels);

    // Uploads are only eligible inside this scope (LLImageGL::setImage), so
    // render targets, noise textures and other direct callers of
    // LLImageGL::setManualImage are untouched.
    class UploadScope
    {
    public:
        UploadScope();
        ~UploadScope();
    private:
        bool mPrev;
    };

    // Counters since the last call.
    Stats takeStats();
}

#endif // FS_TEXTUREUPLOAD_H
