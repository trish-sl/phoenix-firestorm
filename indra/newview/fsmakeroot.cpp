/**
 * @file fsmakeroot.cpp
 * @brief Make a linked part the root of its linkset by relinking it first
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

#include "fsmakeroot.h"

#include "llagent.h"
#include "llcallbacklist.h"
#include "llnotificationsutil.h"
#include "llselectmgr.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "message.h"

// How long to wait for the region to confirm each step
static const F32 STEP_TIMEOUT = 10.f;

FSMakeRoot::FSMakeRoot()
{
}

FSMakeRoot::~FSMakeRoot()
{
    gIdleCallbacks.deleteFunction(onIdle, this);
}

LLViewerObject* FSMakeRoot::getCandidate(std::string* reason) const
{
    auto fail = [reason](const char* why) -> LLViewerObject*
    {
        if (reason)
        {
            *reason = why;
        }
        return nullptr;
    };

    if (mState != STATE_IDLE)
    {
        return fail("busy");
    }
    if (!gSavedSettings.getBOOL("EditLinkedParts"))
    {
        return fail("Select the part with Edit linked on.");
    }
    LLObjectSelectionHandle selection = LLSelectMgr::getInstance()->getSelection();
    if (selection->getObjectCount() != 1)
    {
        return fail("Select exactly one linked part.");
    }
    LLViewerObject* part = selection->getFirstObject();
    LLViewerObject* root = part ? part->getRootEdit() : nullptr;
    if (!part || !root || part == root || part->isDead())
    {
        return fail("The selected part is already the root, or isn't linked.");
    }
    if (root->isAttachment() || root->isAvatar() || part->isAvatar())
    {
        return fail("Attachments and avatars can't be relinked.");
    }

    auto editable = [](const LLViewerObject* prim)
    {
        return prim && !prim->isDead() && prim->permModify() && !prim->isPermanentEnforced();
    };
    if (!editable(root))
    {
        return fail("You need modify rights on every part of the linkset.");
    }
    for (const LLPointer<LLViewerObject>& child : root->getChildren())
    {
        if (child->isAvatar())
        {
            return fail("Someone is sitting on the linkset.");
        }
        if (!editable(child))
        {
            return fail("You need modify rights on every part of the linkset.");
        }
    }
    return part;
}

void FSMakeRoot::makeRoot()
{
    std::string reason;
    LLViewerObject* part = getCandidate(&reason);
    if (!part)
    {
        LLSD args;
        args["REASON"] = reason;
        LLNotificationsUtil::add("FSMakeRootFailed", args);
        return;
    }

    S32 link_count = (S32)part->getRootEdit()->getChildren().size() + 1;
    LLSD args;
    args["COUNT"] = link_count;
    LLSD payload;
    payload["new_root"] = part->getID();
    LLNotificationsUtil::add("FSMakeRootConfirm", args, payload, [](const LLSD& notification, const LLSD& response)
    {
        if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
        {
            FSMakeRoot::instance().start(notification["payload"]["new_root"].asUUID());
        }
    });
}

static void fs_send_link_message(const char* message_name, const std::vector<U32>& local_ids, const LLHost& host)
{
    LLMessageSystem* msg = gMessageSystem;
    msg->newMessage(message_name);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    for (U32 local_id : local_ids)
    {
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_ObjectLocalID, local_id);
    }
    msg->sendReliable(host);
}

void FSMakeRoot::start(const LLUUID& new_root_id)
{
    LLViewerObject* part = gObjectList.findObject(new_root_id);
    std::string reason;
    // The selection may have changed while the dialog was up
    if (!part || getCandidate(&reason) != part)
    {
        LLSD args;
        args["REASON"] = reason.empty() ? std::string("The selection changed.") : reason;
        LLNotificationsUtil::add("FSMakeRootFailed", args);
        return;
    }

    LLViewerObject* root = part->getRootEdit();
    LLViewerRegion* region = root->getRegion();
    if (!region)
    {
        return;
    }

    mNewRootID = new_root_id;
    mPrimIDs.clear();
    mPrimIDs.push_back(part->getID());
    std::vector<U32> local_ids;
    local_ids.push_back(root->getLocalID());
    if (root != part)
    {
        mPrimIDs.push_back(root->getID());
    }
    for (const LLPointer<LLViewerObject>& child : root->getChildren())
    {
        local_ids.push_back(child->getLocalID());
        if (child != part)
        {
            mPrimIDs.push_back(child->getID());
        }
    }

    LLSelectMgr::getInstance()->deselectAll();
    fs_send_link_message("ObjectDelink", local_ids, region->getHost());

    mState = STATE_WAIT_UNLINK;
    mTimer.reset();
    gIdleCallbacks.addFunction(onIdle, this);
}

//static
void FSMakeRoot::onIdle(void* data)
{
    static_cast<FSMakeRoot*>(data)->update();
}

void FSMakeRoot::update()
{
    std::vector<LLViewerObject*> prims;
    for (const LLUUID& id : mPrimIDs)
    {
        LLViewerObject* prim = gObjectList.findObject(id);
        if (!prim || prim->isDead())
        {
            finish(false, "A part of the linkset disappeared.");
            return;
        }
        prims.push_back(prim);
    }
    LLViewerObject* new_root = prims.front();

    if (mState == STATE_WAIT_UNLINK)
    {
        bool all_unlinked = true;
        for (LLViewerObject* prim : prims)
        {
            all_unlinked &= prim->getParent() == nullptr;
        }
        if (!all_unlinked)
        {
            if (mTimer.getElapsedTimeF32() > STEP_TIMEOUT)
            {
                finish(false, "The region didn't unlink the linkset in time; nothing was changed.");
            }
            return;
        }

        // The first prim of an ObjectLink becomes the root
        std::vector<U32> local_ids;
        for (LLViewerObject* prim : prims)
        {
            local_ids.push_back(prim->getLocalID());
        }
        LLViewerRegion* region = new_root->getRegion();
        if (!region)
        {
            finish(false, "The region went away; the parts are unlinked.");
            return;
        }
        fs_send_link_message("ObjectLink", local_ids, region->getHost());
        mState = STATE_WAIT_LINK;
        mTimer.reset();
        return;
    }

    if (mState == STATE_WAIT_LINK)
    {
        bool all_linked = new_root->getParent() == nullptr;
        for (size_t i = 1; i < prims.size() && all_linked; ++i)
        {
            all_linked = prims[i]->getRootEdit() == new_root;
        }
        if (!all_linked)
        {
            if (mTimer.getElapsedTimeF32() > STEP_TIMEOUT)
            {
                finish(false, "The region didn't relink the parts in time. They are unlinked now: select them, with the new root last, and link them again.");
            }
            return;
        }

        LLSelectMgr::getInstance()->deselectAll();
        LLSelectMgr::getInstance()->selectObjectAndFamily(new_root);
        finish(true);
    }
}

void FSMakeRoot::finish(bool success, const std::string& reason)
{
    gIdleCallbacks.deleteFunction(onIdle, this);
    mState = STATE_IDLE;
    LLSD args;
    args["COUNT"] = (S32)mPrimIDs.size();
    if (success)
    {
        LLNotificationsUtil::add("FSMakeRootDone", args);
    }
    else
    {
        args["REASON"] = reason;
        LLNotificationsUtil::add("FSMakeRootFailed", args);
    }
    mPrimIDs.clear();
    mNewRootID.setNull();
}
