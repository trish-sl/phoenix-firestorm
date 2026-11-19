/**
 * @file fsregionobjectscanner.cpp
 * @brief Finds every object in the current region and reads prim contents in parallel. Shared by Mass Object Edit
 *        and Script Repositories.
 */

#include "llviewerprecompiledheaders.h"

#include "fsregionobjectscanner.h"

#include "llagent.h"
#include "llapp.h"
#include "llcallbacklist.h"
#include "llcoros.h"
#include "lleventcoro.h"
#include "llevents.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoinventorylistener.h"
#include "message.h"

namespace
{
    constexpr F32 INVENTORY_TIMEOUT = 20.f;
    constexpr F32 INTEREST_LIST_DELAY = 2.f;
    constexpr S32 MAX_OBJECTS_PER_PACKET = 50;
    constexpr S32 MAX_PROPERTY_REQUESTS_IN_FLIGHT = MAX_OBJECTS_PER_PACKET * 3;
    constexpr S32 MAX_PROPERTY_RETRIES = 8;
    constexpr F32 PROPERTY_RETRY_INTERVAL = 8.f;
    constexpr F32 OBJECT_RECONCILE_INTERVAL = 1.5f;
    constexpr S32 CONTENT_WORKERS = 3;
    constexpr S32 CONTENT_RETRIES = 10;
    constexpr F32 CONTENT_RETRY_DELAY = 1.f;

    bool isScannable(LLViewerObject* object, LLViewerRegion* region)
    {
        return object && !object->isDead() && object->getRegion() == region && !object->isAvatar() &&
            !object->isAttachment() && object->getPCode() != LLViewerObject::LL_VO_SURFACE_PATCH &&
            object->mbCanSelect;
    }

    class InventoryFetcher final : public LLVOInventoryListener
    {
    public:
        InventoryFetcher(LLEventPump& pump, LLViewerObject* object) : mPump(pump)
        {
            registerVOInventoryListener(object, nullptr);
        }

        void fetch() { requestVOInventory(); }
        const LLInventoryObject::object_list_t& inventory() const { return mInventory; }

        void inventoryChanged(LLViewerObject*, LLInventoryObject::object_list_t* inventory, S32, void*) override
        {
            mInventory.clear();
            if (inventory)
            {
                mInventory.assign(inventory->begin(), inventory->end());
            }
            mPump.post(LLSDMap("inventory", true));
        }

    private:
        LLEventPump& mPump;
        LLInventoryObject::object_list_t mInventory;
    };

    // Each request waits on its own mail drop, so a late answer cannot wake another wait
    bool fetchInventory(LLViewerObject* object, LLInventoryObject::object_list_t& inventory, bool force_server)
    {
        if (!object)
        {
            return false;
        }
        if (force_server)
        {
            object->dirtyInventory();
        }
        LLEventMailDrop pump("FSRegionScanContents-" + LLUUID::generateNewID().asString(), true);
        std::shared_ptr<InventoryFetcher> fetcher = std::make_shared<InventoryFetcher>(pump, object);
        fetcher->fetch();
        LLSD result = llcoro::suspendUntilEventOnWithTimeout(pump, INVENTORY_TIMEOUT, LLSDMap("timeout", true));
        if (result.has("timeout"))
        {
            return false;
        }
        inventory.assign(fetcher->inventory().begin(), fetcher->inventory().end());
        return true;
    }
}

std::set<FSRegionObjectScanner*> FSRegionObjectScanner::sScanners;
S32 FSRegionObjectScanner::s360Users = 0;

FSRegionObjectScanner::FSRegionObjectScanner()
{
    sScanners.insert(this);
}

FSRegionObjectScanner::~FSRegionObjectScanner()
{
    close();
    sScanners.erase(this);
}

void FSRegionObjectScanner::status(const std::string& text) const
{
    if (onStatus)
    {
        onStatus(text);
    }
}

void FSRegionObjectScanner::stateChanged() const
{
    if (onStateChanged)
    {
        onStateChanged();
    }
}

bool FSRegionObjectScanner::anyScanning()
{
    for (const FSRegionObjectScanner* scanner : sScanners)
    {
        if (scanner->mScanning)
        {
            return true;
        }
    }
    return false;
}

void FSRegionObjectScanner::dispatchObjectProperties(LLMessageSystem* msg)
{
    for (FSRegionObjectScanner* scanner : sScanners)
    {
        scanner->processObjectProperties(msg);
    }
}

void FSRegionObjectScanner::open()
{
    if (mOpen)
    {
        return;
    }
    mOpen = true;
    ++s360Users;
    gAgent.setFSRegionScanActive(true);
    gAgent.changeInterestListMode(IL_MODE_360);

    // The InterestList capability does not report completion. Give the sim a
    // short head start before taking the first complete-region snapshot.
    mInterestListTimer.reset();
    mReconcileTimer.reset();
    mWaitingForInterestList = true;
    gIdleCallbacks.addFunction(onIdle, this);
    status("Requesting all region objects...");
    stateChanged();
}

void FSRegionObjectScanner::close(bool app_quitting)
{
    gIdleCallbacks.deleteFunction(onIdle, this);
    mWaitingForInterestList = false;
    mScanning = false;
    if (!mOpen)
    {
        return;
    }
    mOpen = false;
    if (--s360Users <= 0)
    {
        s360Users = 0;
        gAgent.setFSRegionScanActive(false);
        if (!app_quitting && !LLApp::isExiting())
        {
            gAgent.changeInterestListMode(IL_MODE_DEFAULT);
        }
    }
}

bool FSRegionObjectScanner::refresh()
{
    LLViewerRegion* region = gAgent.getRegion();
    if (mScanning || mWaitingForInterestList || !region)
    {
        return false;
    }

    mObjects.clear();
    mPropertyRequestIDs.clear();
    mPropertyRequestIndex = 0;
    mPropertyRequestsInFlight = 0;
    mPropertyRetryCount = 0;
    mRegionID = region->getRegionID();

    for (S32 i = 0; i < gObjectList.getNumObjects(); ++i)
    {
        LLViewerObject* object = gObjectList.getObject(i);
        if (!isScannable(object, region))
        {
            continue;
        }
        ObjectInfo info;
        info.id = object->getID();
        info.local_id = object->getLocalID();
        LLViewerObject* root = object->getRootEdit();
        info.root_id = root ? root->getID() : info.id;
        if (mObjects.emplace(info.id, info).second)
        {
            mPropertyRequestIDs.push_back(info.id);
        }
    }

    mPendingProperties = static_cast<S32>(mObjects.size());
    mScanning = mPendingProperties > 0;
    mScanTimer.reset();
    gIdleCallbacks.addFunction(onIdle, this);
    if (mScanning)
    {
        status(llformat("Scanning %d current-region prims...", mPendingProperties));
        stateChanged();
    }
    else
    {
        finishObjectScan();
    }
    return true;
}

void FSRegionObjectScanner::reconcileObjects()
{
    LLViewerRegion* region = gAgent.getRegion();
    if (!region || region->getRegionID() != mRegionID)
    {
        return;
    }

    S32 discovered = 0;
    for (S32 i = 0; i < gObjectList.getNumObjects(); ++i)
    {
        LLViewerObject* object = gObjectList.getObject(i);
        if (!isScannable(object, region) || mObjects.find(object->getID()) != mObjects.end())
        {
            continue;
        }

        ObjectInfo info;
        info.id = object->getID();
        info.local_id = object->getLocalID();
        LLViewerObject* root = object->getRootEdit();
        info.root_id = root ? root->getID() : info.id;
        if (mObjects.emplace(info.id, info).second)
        {
            mPropertyRequestIDs.push_back(info.id);
            ++mPendingProperties;
            ++discovered;
        }
    }

    // ObjectSelect/ObjectDeselect is used to request full properties. Do not
    // disturb an object the user is actively editing; queue it once selection
    // has moved elsewhere.
    for (auto& entry : mObjects)
    {
        ObjectInfo& info = entry.second;
        if (info.property_request != PropertyRequestState::DEFERRED)
        {
            continue;
        }
        LLViewerObject* object = gObjectList.findObject(info.id);
        if (object && !object->isDead() && !object->isSelected())
        {
            info.property_request = PropertyRequestState::NEED;
            mPropertyRequestIDs.push_back(info.id);
            ++discovered;
        }
    }

    if (discovered > 0)
    {
        mPropertyRetryCount = 0;
        mScanning = true;
        mScanTimer.reset();
        status(llformat("Queued %d new or deferred prims; fetching %d pending properties...",
            discovered, mPendingProperties));
        stateChanged();
    }
}

bool FSRegionObjectScanner::retryUnresolvedProperties()
{
    if (mPropertyRetryCount >= MAX_PROPERTY_RETRIES)
    {
        return false;
    }

    uuid_vec_t unresolved;
    unresolved.reserve(mPendingProperties);
    for (auto& entry : mObjects)
    {
        ObjectInfo& info = entry.second;
        if (!info.received && info.property_request != PropertyRequestState::DEFERRED)
        {
            info.property_request = PropertyRequestState::NEED;
            unresolved.push_back(info.id);
        }
    }
    if (unresolved.empty())
    {
        return false;
    }

    mPropertyRequestIDs.swap(unresolved);
    mPropertyRequestIndex = 0;
    mPropertyRequestsInFlight = 0;
    ++mPropertyRetryCount;
    mScanTimer.reset();
    status(llformat("Retrying %d unresolved prim properties (attempt %d/%d)...",
        mPendingProperties, mPropertyRetryCount, MAX_PROPERTY_RETRIES));
    return true;
}

void FSRegionObjectScanner::requestObjectProperties(const std::vector<U32>& local_ids, bool select)
{
    LLViewerRegion* region = gAgent.getRegion();
    if (!region || region->getRegionID() != mRegionID)
    {
        return;
    }
    LLMessageSystem* msg = gMessageSystem;
    bool start = true;
    S32 blocks = 0;
    for (const U32 local_id : local_ids)
    {
        if (start)
        {
            msg->newMessageFast(select ? _PREHASH_ObjectSelect : _PREHASH_ObjectDeselect);
            msg->nextBlockFast(_PREHASH_AgentData);
            msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
            msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
            start = false;
        }
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_ObjectLocalID, local_id);
        if (++blocks >= MAX_OBJECTS_PER_PACKET || msg->isSendFull(nullptr))
        {
            msg->sendReliable(region->getHost());
            start = true;
            blocks = 0;
        }
    }
    if (!start)
    {
        msg->sendReliable(region->getHost());
    }
}

void FSRegionObjectScanner::processPropertyRequestQueue()
{
    LLViewerRegion* region = gAgent.getRegion();
    if (!region || region->getRegionID() != mRegionID)
    {
        mPropertyRequestIndex = mPropertyRequestIDs.size();
        mPropertyRequestsInFlight = 0;
        return;
    }

    if (mPropertyRequestsInFlight >= MAX_PROPERTY_REQUESTS_IN_FLIGHT ||
        mPropertyRequestIndex >= mPropertyRequestIDs.size())
    {
        return;
    }

    std::vector<U32> request_ids;
    request_ids.reserve(MAX_OBJECTS_PER_PACKET);
    const S32 available = MAX_PROPERTY_REQUESTS_IN_FLIGHT - mPropertyRequestsInFlight;
    const S32 batch_size = llmin(MAX_OBJECTS_PER_PACKET, available);
    while (mPropertyRequestIndex < mPropertyRequestIDs.size() && static_cast<S32>(request_ids.size()) < batch_size)
    {
        const LLUUID id = mPropertyRequestIDs[mPropertyRequestIndex++];
        auto found = mObjects.find(id);
        if (found == mObjects.end() || found->second.property_request != PropertyRequestState::NEED)
        {
            continue;
        }
        LLViewerObject* object = gObjectList.findObject(id);
        if (object && object->isSelected())
        {
            found->second.property_request = PropertyRequestState::DEFERRED;
            continue;
        }
        found->second.property_request = PropertyRequestState::SENT;
        request_ids.push_back(found->second.local_id);
    }

    if (!request_ids.empty())
    {
        requestObjectProperties(request_ids, true);
        requestObjectProperties(request_ids, false);
        mPropertyRequestsInFlight += static_cast<S32>(request_ids.size());
        mScanTimer.reset();
    }
}

void FSRegionObjectScanner::processObjectProperties(LLMessageSystem* msg)
{
    if (!mScanning)
    {
        return;
    }
    const S32 count = msg->getNumberOfBlocksFast(_PREHASH_ObjectData);
    for (S32 i = 0; i < count; ++i)
    {
        LLUUID id;
        msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_ObjectID, id, i);
        auto found = mObjects.find(id);
        if (found == mObjects.end() || found->second.received)
        {
            continue;
        }

        ObjectInfo& info = found->second;
        LLUUID group, last_owner;
        U32 base = 0, owner = 0, everyone = 0, group_mask = 0, next = 0;
        msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_CreatorID, info.creator_id, i);
        msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_OwnerID, info.owner_id, i);
        msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_GroupID, group, i);
        msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_LastOwnerID, last_owner, i);
        msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_BaseMask, base, i);
        msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_OwnerMask, owner, i);
        msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_EveryoneMask, everyone, i);
        msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_GroupMask, group_mask, i);
        msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_NextOwnerMask, next, i);
        msg->getStringFast(_PREHASH_ObjectData, _PREHASH_Name, info.name, i);

        info.permissions.init(info.creator_id, info.owner_id, last_owner, group);
        info.permissions.initMasks(base, owner, everyone, group_mask, next);
        info.permissions.getOwnership(info.owner_id, info.group_owned);
        if (info.property_request == PropertyRequestState::SENT && mPropertyRequestsInFlight > 0)
        {
            --mPropertyRequestsInFlight;
        }
        info.property_request = PropertyRequestState::RECEIVED;
        info.received = true;
        --mPendingProperties;
        mScanTimer.reset();
        if (onObjectReceived)
        {
            onObjectReceived(id);
        }
    }
}

void FSRegionObjectScanner::finishObjectScan()
{
    mScanning = false;
    mPropertyRequestsInFlight = 0;
    if (onScanFinished)
    {
        onScanFinished();
    }
    stateChanged();
}

void FSRegionObjectScanner::onIdle(void* userdata)
{
    FSRegionObjectScanner* self = static_cast<FSRegionObjectScanner*>(userdata);
    if (!self || !self->mOpen)
    {
        return;
    }

    // Full-region interest mode can be restored by another floater or a
    // region transition. Reclaim it while open, as Scene Explorer does.
    if (gAgent.getInterestListMode() != IL_MODE_360)
    {
        gAgent.changeInterestListMode(IL_MODE_360);
    }
    LLViewerRegion* region = gAgent.getRegion();
    if (region && self->mRegionID.notNull() && region->getRegionID() != self->mRegionID &&
        !self->mWaitingForInterestList)
    {
        self->mRegionID = region->getRegionID();
        self->mScanning = false;
        self->mInterestListTimer.reset();
        self->mWaitingForInterestList = true;
        self->status("Region changed; waiting for the new region object stream...");
        self->stateChanged();
    }

    if (self->mWaitingForInterestList && self->mInterestListTimer.getElapsedTimeF32() >= INTEREST_LIST_DELAY)
    {
        self->mWaitingForInterestList = false;
        if (self->onReady)
        {
            self->onReady();
        }
        else
        {
            self->refresh();
        }
        return;
    }

    if (self->mScanning)
    {
        self->processPropertyRequestQueue();
        const bool queue_drained = self->mPropertyRequestIndex >= self->mPropertyRequestIDs.size() &&
            self->mPropertyRequestsInFlight == 0;
        const bool retry_due = queue_drained || self->mScanTimer.getElapsedTimeF32() >= PROPERTY_RETRY_INTERVAL;
        if (self->mPendingProperties == 0 || (retry_due && !self->retryUnresolvedProperties()))
        {
            self->finishObjectScan();
        }
    }
    else if (!self->mWaitingForInterestList && !(self->isReconcilePaused && self->isReconcilePaused()) &&
        self->mRegionID.notNull() &&
        self->mReconcileTimer.getElapsedTimeF32() >= OBJECT_RECONCILE_INTERVAL)
    {
        // Objects that streamed in after the snapshot
        self->mReconcileTimer.reset();
        self->reconcileObjects();
    }
}

FSRegionObjectScanner::ContentsResult FSRegionObjectScanner::fetchContents(const uuid_vec_t& targets,
    bool force_server, contents_f on_contents, progress_f on_progress, retry_f on_retry,
    std::function<bool()> cancelled, S32 min_workers)
{
    ContentsResult result;
    result.total = static_cast<S32>(targets.size());
    // Shared with the workers, so nothing they touch lives on this frame
    struct Counters
    {
        S32 processed{ 0 };
        S32 failed_first_pass{ 0 };
    };
    auto counters = std::make_shared<Counters>();
    const S32 total = result.total;

    uuid_vec_t pass_targets = targets;
    for (S32 attempt = 0; !pass_targets.empty() && attempt <= CONTENT_RETRIES; ++attempt)
    {
        if (cancelled && cancelled())
        {
            break;
        }
        const bool retry_pass = attempt > 0;
        if (retry_pass)
        {
            result.retries_used = attempt;
            if (on_retry)
            {
                on_retry(static_cast<S32>(pass_targets.size()), attempt, CONTENT_RETRIES);
            }
            llcoro::suspendUntilTimeout(CONTENT_RETRY_DELAY * static_cast<F32>(attempt));
        }

        static LLCachedControl<S32> inventory_workers(gSavedSettings, "FSMassObjectEditInventoryWorkers",
            CONTENT_WORKERS);
        const S32 pass_count = static_cast<S32>(pass_targets.size());
        const S32 worker_count = llmin(llclamp(llmax(static_cast<S32>(inventory_workers), min_workers), 1, 8),
            pass_count);
        std::vector<uuid_vec_t> work(static_cast<size_t>(worker_count));
        for (S32 i = 0; i < pass_count; ++i)
        {
            work[static_cast<size_t>(i % worker_count)].push_back(pass_targets[static_cast<size_t>(i)]);
        }

        // Shared with the workers; they finish before this frame returns (each posts once to done)
        auto retry_ids = std::make_shared<uuid_vec_t>();
        LLEventMailDrop done("FSRegionScanDone-" + LLUUID::generateNewID().asString(), true);
        const std::string done_name = done.getName();
        for (S32 w = 0; w < worker_count; ++w)
        {
            uuid_vec_t mine = work[static_cast<size_t>(w)];
            LLCoros::instance().launch("FSRegionObjectScanner::contentsWorker",
                [mine, done_name, retry_ids, retry_pass, force_server, on_contents, on_progress, cancelled,
                    counters, total]()
                {
                    for (const LLUUID& id : mine)
                    {
                        if (cancelled && cancelled())
                        {
                            break;
                        }
                        LLInventoryObject::object_list_t inventory;
                        if (fetchInventory(gObjectList.findObject(id), inventory, force_server || retry_pass))
                        {
                            if (on_contents)
                            {
                                on_contents(id, inventory);
                            }
                        }
                        else
                        {
                            retry_ids->push_back(id);
                            if (!retry_pass)
                            {
                                ++counters->failed_first_pass;
                            }
                        }
                        if (!retry_pass)
                        {
                            ++counters->processed;
                            if (on_progress)
                            {
                                on_progress(counters->processed, total, counters->failed_first_pass);
                            }
                        }
                    }
                    LLEventPumps::instance().post(done_name, LLSDMap("done", true));
                });
        }
        for (S32 w = 0; w < worker_count; ++w)
        {
            llcoro::suspendUntilEventOn(done);
        }
        pass_targets = *retry_ids;
    }

    result.failed = static_cast<S32>(pass_targets.size());
    result.failed_ids = pass_targets;
    result.succeeded = result.total - result.failed;
    return result;
}
