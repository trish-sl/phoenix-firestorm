/**
 * @file fsbuildhistory.h
 * @brief Client side undo/redo history for build tool moves, rotations and resizes
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, Trish_sl
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
 * $/LicenseInfo$
 */

#ifndef FS_BUILDHISTORY_H
#define FS_BUILDHISTORY_H

#include "llsingleton.h"
#include "lltimer.h"
#include "lluuid.h"
#include "llquaternion.h"
#include "v3math.h"

#include <deque>
#include <unordered_map>

class LLViewerObject;

class FSBuildHistory : public LLSingleton<FSBuildHistory>
{
    LLSINGLETON(FSBuildHistory);

public:
    static bool enabled();

    // The transform an object had when it was selected or last committed; undo goes back to it
    void noteBaseline(LLViewerObject* object);
    // The build tools sent a MultipleObjectUpdate for these objects (type: UPD_* flags)
    void onUpdateSent(const std::vector<LLViewerObject*>& objects, U32 type);

    bool canUndo() const { return !mUndo.empty(); }
    bool canRedo() const { return !mRedo.empty(); }
    void undo();
    void redo();

private:
    struct Transform
    {
        LLVector3       mPosition;  // local to the parent; the region for roots
        LLQuaternion    mRotation;  // likewise
        LLVector3       mScale;
    };
    struct Entry
    {
        LLUUID      mID;
        Transform   mBefore;
        Transform   mAfter;
    };
    struct Step
    {
        std::vector<Entry>  mEntries;
        U32                 mType = 0;      // UPD_* flags to send when applying it
    };

    static Transform capture(const LLViewerObject* object);
    static bool differs(const Transform& a, const Transform& b);
    void apply(const Step& step, bool use_after);
    void trim();

    std::unordered_map<LLUUID, Transform> mBaseline;
    std::deque<Step>    mUndo;
    std::deque<Step>    mRedo;
    bool                mStepOpen = false;  // the last step still collects a drag in progress
    LLFrameTimer        mLastUpdate;
};

#endif // FS_BUILDHISTORY_H
