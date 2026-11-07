/**
 * @file fsbuildhistory.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fsbuildhistory.h"

#include "llagent.h"
#include "llfocusmgr.h"
#include "llselectmgr.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "message.h"
#include "pipeline.h"

#include <set>

// Updates this close together, or while the mouse is captured (a drag, a held spinner arrow),
// extend the same step instead of adding one per frame
static const F32 MERGE_INTERVAL = 0.35f;

FSBuildHistory::FSBuildHistory()
{
}

//static
bool FSBuildHistory::enabled()
{
    static LLCachedControl<bool> client_undo(gSavedSettings, "FSBuildClientUndo", true);
    return client_undo;
}

//static
FSBuildHistory::Transform FSBuildHistory::capture(const LLViewerObject* object)
{
    Transform t;
    t.mPosition = object->getPosition();
    t.mRotation = object->getRotation();
    t.mScale = object->getScale();
    return t;
}

//static
bool FSBuildHistory::differs(const Transform& a, const Transform& b)
{
    return dist_vec_squared(a.mPosition, b.mPosition) > 1.0e-8f ||
           dist_vec_squared(a.mScale, b.mScale) > 1.0e-8f ||
           !a.mRotation.isEqualEps(b.mRotation, 1.0e-5f);
}

void FSBuildHistory::noteBaseline(LLViewerObject* object)
{
    if (!enabled() || !object || object->isDead() || object->isAvatar())
    {
        return;
    }
    mBaseline[object->getID()] = capture(object);
}

void FSBuildHistory::onUpdateSent(const std::vector<LLViewerObject*>& objects, U32 type)
{
    if (!enabled() || objects.empty() || !(type & (UPD_POSITION | UPD_ROTATION | UPD_SCALE)))
    {
        return;
    }

    std::vector<Entry> entries;
    for (LLViewerObject* object : objects)
    {
        if (!object || object->isDead() || object->isAvatar())
        {
            continue;
        }
        const Transform now = capture(object);
        auto base = mBaseline.find(object->getID());
        if (base != mBaseline.end() && differs(base->second, now))
        {
            entries.push_back({ object->getID(), base->second, now });
        }
        mBaseline[object->getID()] = now;
    }
    if (entries.empty())
    {
        return;
    }

    // A drag or held spinner sends many updates; keep them in one step
    const bool dragging = gFocusMgr.getMouseCapture() != nullptr;
    bool merged = false;
    if (mStepOpen && !mUndo.empty() && (dragging || mLastUpdate.getElapsedTimeF32() < MERGE_INTERVAL))
    {
        Step& last = mUndo.back();
        std::set<LLUUID> last_ids;
        for (const Entry& e : last.mEntries)
        {
            last_ids.insert(e.mID);
        }
        bool same = true;
        for (const Entry& e : entries)
        {
            same &= last_ids.count(e.mID) > 0;
        }
        if (same)
        {
            for (const Entry& e : entries)
            {
                for (Entry& le : last.mEntries)
                {
                    if (le.mID == e.mID)
                    {
                        le.mAfter = e.mAfter;
                    }
                }
            }
            last.mType |= type;
            merged = true;
        }
    }
    if (!merged)
    {
        Step step;
        step.mEntries = std::move(entries);
        step.mType = type;
        mUndo.push_back(std::move(step));
        trim();
    }
    mRedo.clear();
    mStepOpen = true;
    mLastUpdate.reset();
}

void FSBuildHistory::trim()
{
    static LLCachedControl<U32> depth(gSavedSettings, "FSBuildUndoDepth", 100);
    const size_t max_steps = llclamp((U32)depth, 1U, 1000U);
    while (mUndo.size() > max_steps)
    {
        mUndo.pop_front();
    }
}

// Set the stored transforms on the objects and send them like the build tools do. Works on objects
// that are no longer selected; ones that are gone or no longer modifiable are skipped.
void FSBuildHistory::apply(const Step& step, bool use_after)
{
    const U8 type = (U8)(step.mType & (UPD_POSITION | UPD_ROTATION | UPD_SCALE | UPD_LINKED_SETS | UPD_UNIFORM));
    LLMessageSystem* msg = gMessageSystem;

    for (const Entry& entry : step.mEntries)
    {
        LLViewerObject* object = gObjectList.findObject(entry.mID);
        if (!object || object->isDead() || !object->permMove() || !object->getRegion())
        {
            continue;
        }
        const Transform& t = use_after ? entry.mAfter : entry.mBefore;

        if (type & UPD_POSITION)
        {
            object->setPosition(t.mPosition);
        }
        if (type & UPD_ROTATION)
        {
            object->setRotation(t.mRotation);
        }
        if (type & UPD_SCALE)
        {
            object->setScale(t.mScale);
        }
        if (object->mDrawable.notNull())
        {
            gPipeline.markMoved(object->mDrawable, false);
        }
        mBaseline[entry.mID] = t;

        // Same layout as LLSelectMgr::packMultipleUpdate
        U8 data[256];
        S32 offset = 0;
        if (type & UPD_POSITION)
        {
            htolememcpy(&data[offset], &(t.mPosition.mV), MVT_LLVector3, 12);
            offset += 12;
        }
        if (type & UPD_ROTATION)
        {
            LLVector3 packed = t.mRotation.packToVector3();
            htolememcpy(&data[offset], &(packed.mV), MVT_LLQuaternion, 12);
            offset += 12;
        }
        if (type & UPD_SCALE)
        {
            htolememcpy(&data[offset], &(t.mScale.mV), MVT_LLVector3, 12);
            offset += 12;
        }

        msg->newMessageFast(_PREHASH_MultipleObjectUpdate);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_ObjectLocalID, object->getLocalID());
        msg->addU8Fast(_PREHASH_Type, type);
        msg->addBinaryDataFast(_PREHASH_Data, data, offset);
        msg->sendReliable(object->getRegion()->getHost());
    }

    LLSelectMgr::getInstance()->updateSelectionCenter();
    mStepOpen = false;
}

void FSBuildHistory::undo()
{
    if (mUndo.empty())
    {
        return;
    }
    Step step = std::move(mUndo.back());
    mUndo.pop_back();
    apply(step, false);
    mRedo.push_back(std::move(step));
}

void FSBuildHistory::redo()
{
    if (mRedo.empty())
    {
        return;
    }
    Step step = std::move(mRedo.back());
    mRedo.pop_back();
    apply(step, true);
    mUndo.push_back(std::move(step));
}
