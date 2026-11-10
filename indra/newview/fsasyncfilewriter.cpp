/**
 * @file fsasyncfilewriter.cpp
 * @brief Writes small files (chat logs) on a thread of their own, in order
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

#include "fsasyncfilewriter.h"

#include "llfile.h"
#include "llviewercontrol.h"
#include "threadpool.h"
#include "workqueue.h"

#include <memory>
#include <mutex>

namespace
{
std::mutex sMutex;
std::unique_ptr<LL::ThreadPool> sPool;
bool sShutdown = false;

// Under sMutex. Null when writing here is off or over.
LL::ThreadPool* pool()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "FSAsyncChatLogWrites", true);
    if (sShutdown || !enabled)
    {
        return nullptr;
    }
    if (!sPool)
    {
        sPool = std::make_unique<LL::ThreadPool>("FSAsyncFileWriter", 1);
        sPool->start();
    }
    return sPool.get();
}

bool post(LL::WorkQueueBase::Work work)
{
    std::lock_guard<std::mutex> lock(sMutex);
    LL::ThreadPool* writer = pool();
    return writer && writer->getQueue().post(work);
}
}

bool FSAsyncFileWriter::append(const std::string& path, std::string text, std::function<void()> on_written)
{
    return post([path, text = std::move(text), on_written = std::move(on_written)]()
        {
            if (LLFILE* fp = LLFile::fopen(path, LLFILE_MODE("ab")))
            {
                if (fwrite(text.data(), 1, text.size(), fp) != text.size())
                {
                    LL_WARNS() << "Couldn't write all of " << path << LL_ENDL;
                }
                LLFile::close(fp);
            }
            else
            {
                LL_WARNS() << "Couldn't open " << path << " to append to it" << LL_ENDL;
            }
            if (on_written)
            {
                LL::WorkQueue::postToMainLoop(on_written);
            }
        });
}

bool FSAsyncFileWriter::replace(const std::string& path, std::string contents)
{
    return post([path, contents = std::move(contents)]()
        {
            const std::string temp_path = path + ".tmp";
            bool ok = false;
            if (LLFILE* fp = LLFile::fopen(temp_path, LLFILE_MODE("wb")))
            {
                ok = fwrite(contents.data(), 1, contents.size(), fp) == contents.size();
                LLFile::close(fp);
            }
            if (ok)
            {
                LLFile::remove(path, ENOENT);
                ok = LLFile::rename(temp_path, path) == 0;
            }
            if (!ok)
            {
                LL_WARNS() << "Couldn't write " << path << LL_ENDL;
                LLFile::remove(temp_path, ENOENT);
            }
        });
}

void FSAsyncFileWriter::shutdown()
{
    std::unique_ptr<LL::ThreadPool> writer;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sShutdown = true;
        writer = std::move(sPool);
    }
    if (writer)
    {
        writer->close(); // runs what is queued first
    }
}
