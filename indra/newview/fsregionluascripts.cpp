/**
 * @file fsregionluascripts.cpp
 * @brief Region-wide Lua script maintenance tools.
 */

#include "llviewerprecompiledheaders.h"

#include "fsregionluascripts.h"

#include "fscommon.h"
#include "llagent.h"
#include "llassetstorage.h"
#include "llcallbacklist.h"
#include "llcompilequeue.h"
#include "llcoros.h"
#include "lleventcoro.h"
#include "llevents.h"
#include "llfilesystem.h" // <FS:Trish/>
#include "llfloaterreg.h"
#include "llinventorydefines.h"
#include "llinventorymodel.h"
#include "message.h"
#include "llnotificationsutil.h"
#include "lltrans.h"
#include "llviewerassetupload.h"
#include "llviewerinventory.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoinventorylistener.h"
#include "roles_constants.h"

#include <limits>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace
{
    constexpr F32 PROPERTY_SCAN_TIMEOUT = 15.f;
    constexpr F32 INVENTORY_FETCH_TIMEOUT = 60.f;
    constexpr F32 SCRIPT_UPLOAD_TIMEOUT = 60.f;
    constexpr S32 MAX_OBJECTS_PER_PACKET = 254;
    const std::string LUA_SOURCE_PRIM_NAME("Lua Script Source");

    class InventoryFetcher final : public LLVOInventoryListener
    {
    public:
        using ptr_t = std::shared_ptr<InventoryFetcher>;

        InventoryFetcher(LLEventPump& pump, LLViewerObject* object) :
            mPump(pump)
        {
            registerVOInventoryListener(object, nullptr);
        }

        void fetch()
        {
            requestVOInventory();
        }

        const LLInventoryObject::object_list_t& getInventory() const
        {
            return mInventory;
        }

        void inventoryChanged(LLViewerObject*, LLInventoryObject::object_list_t* inventory,
            S32, void*) override
        {
            mInventory.clear();
            if (inventory)
            {
                mInventory.assign(inventory->begin(), inventory->end());
            }
            mPump.post(LLSDMap("inventory", LLSD::Boolean(true)));
        }

    private:
        LLEventPump& mPump;
        LLInventoryObject::object_list_t mInventory;
    };

    class ExistingScriptAssetUpload final : public LLScriptAssetUpload
    {
    public:
        ExistingScriptAssetUpload(const LLUUID& task_id, const LLUUID& item_id,
            const LLUUID& asset_id, const std::string& target, bool running,
            const LLUUID& experience, // <FS:Trish/> keep the script's experience
            taskUploadFinish_f finish, uploadFailed_f failed) :
            LLScriptAssetUpload(task_id, item_id, target, running, experience,
                std::string(), finish, failed)
        {
            setAssetId(asset_id);
        }

        LLSD prepareUpload() override
        {
            return LLSDMap("success", LLSD::Boolean(true));
        }
    };

    // <FS:Trish> Each request waits on its own mail drop: a reply that comes after its wait timed out must not
    // wake a later wait (a mail drop keeps events nobody took).
    std::string ownPumpName(const char* prefix)
    {
        return std::string(prefix) + LLUUID::generateNewID().asString();
    }
    // </FS:Trish>

    void uploadSucceeded(const std::string& pump_name, const LLSD& result)
    {
        LLEventPumps::instance().post(pump_name, result);
    }

    void assetFetched(const LLUUID& asset_id, LLAssetType::EType, void* userdata,
        S32 status, LLExtStat)
    {
        std::unique_ptr<std::string> pump_name(static_cast<std::string*>(userdata));
        LLSD result;
        result["asset_id"] = asset_id;
        result["status"] = status;
        LLEventPumps::instance().post(*pump_name, result);
    }

    bool fetchScriptAsset(LLViewerObject* object, const LLViewerInventoryItem* item,
        LLEventPump& pump, LLUUID& fetched_asset_id, S32& status)
    {
        fetched_asset_id.setNull();
        status = LL_ERR_ASSET_REQUEST_FAILED;
        if (!object || !object->getRegion() || !item || !gAssetStorage)
        {
            return false;
        }

        LLEventMailDrop own(ownPumpName("FSScriptAsset-"), true); // <FS:Trish/>
        std::string* pump_name = new std::string(own.getName());
        // Match the compile queue's proven retrieval path. Passing the object
        // simulator here can route the transfer to a host that does not serve
        // task inventory assets on every grid.
        gAssetStorage->getInvItemAsset(LLHost(),
            gAgent.getID(), gAgent.getSessionID(), item->getPermissions().getOwner(),
            object->getID(), item->getUUID(), item->getAssetUUID(), item->getType(),
            &assetFetched, pump_name, true);

        LLSD result = llcoro::suspendUntilEventOnWithTimeout(own, INVENTORY_FETCH_TIMEOUT,
            LLSDMap("timeout", LLSD::Boolean(true)));
        if (result.has("timeout"))
        {
            return false;
        }

        status = result["status"].asInteger();
        fetched_asset_id = result["asset_id"].asUUID();
        return status == LL_ERR_NOERR && fetched_asset_id.notNull();
    }

    bool fetchInventory(LLViewerObject* object, LLEventPump& pump,
        LLInventoryObject::object_list_t& inventory);

    LLPointer<LLViewerInventoryItem> findInventoryItem(
        const LLInventoryObject::object_list_t& inventory, const LLUUID& item_id)
    {
        for (const LLPointer<LLInventoryObject>& inventory_object : inventory)
        {
            LLViewerInventoryItem* item = dynamic_cast<LLViewerInventoryItem*>(inventory_object.get());
            if (item && item->getUUID() == item_id)
            {
                return item;
            }
        }
        return nullptr;
    }

    LLPointer<LLViewerInventoryItem> findNewScriptItem(
        const LLInventoryObject::object_list_t& before,
        const LLInventoryObject::object_list_t& after)
    {
        std::set<LLUUID> existing_items;
        for (const LLPointer<LLInventoryObject>& inventory_object : before)
        {
            existing_items.insert(inventory_object->getUUID());
        }

        for (const LLPointer<LLInventoryObject>& inventory_object : after)
        {
            LLViewerInventoryItem* item = dynamic_cast<LLViewerInventoryItem*>(inventory_object.get());
            if (item && item->getType() == LLAssetType::AT_LSL_TEXT &&
                existing_items.find(item->getUUID()) == existing_items.end())
            {
                return item;
            }
        }
        return nullptr;
    }

    bool fetchInventoryAfterRemoval(LLViewerObject* object, const LLUUID& removed_item_id,
        LLEventPump& pump, LLInventoryObject::object_list_t& inventory)
    {
        for (S32 attempt = 0; attempt < 10; ++attempt)
        {
            object->dirtyInventory();
            inventory.clear();
            if (fetchInventory(object, pump, inventory) &&
                !findInventoryItem(inventory, removed_item_id))
            {
                return true;
            }
            llcoro::suspendUntilTimeout(0.25f);
        }
        return false;
    }

    LLPointer<LLViewerInventoryItem> copyTaskScriptToAgentInventory(
        LLViewerObject* source_object, const LLViewerInventoryItem* source_item)
    {
        if (!source_object || !source_item)
        {
            return nullptr;
        }

        const LLUUID folder_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_LSL_TEXT);
        if (folder_id.isNull())
        {
            return nullptr;
        }

        std::set<LLUUID> existing_items;
        LLInventoryModel::cat_array_t* categories = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(folder_id, categories, items);
        if (items)
        {
            for (const LLPointer<LLViewerInventoryItem>& item : *items)
            {
                existing_items.insert(item->getUUID());
            }
        }

        source_object->moveInventory(folder_id, source_item->getUUID());
        const F32 poll_interval = 0.25f;
        for (F32 elapsed = 0.f; elapsed < INVENTORY_FETCH_TIMEOUT; elapsed += poll_interval)
        {
            llcoro::suspendUntilTimeout(poll_interval);
            categories = nullptr;
            items = nullptr;
            gInventory.getDirectDescendentsOf(folder_id, categories, items);
            if (!items)
            {
                continue;
            }

            for (const LLPointer<LLViewerInventoryItem>& item : *items)
            {
                if (existing_items.find(item->getUUID()) == existing_items.end() &&
                    item->getType() == LLAssetType::AT_LSL_TEXT &&
                    item->getName() == source_item->getName() && item->isFinished())
                {
                    return item;
                }
            }
        }

        return nullptr;
    }

    bool uploadFailed(const std::string& pump_name, LLSD result, const std::string& reason)
    {
        result["compiled"] = false;
        if (!result.has("errors") || !result["errors"].isArray())
        {
            result["errors"] = LLSD::emptyArray();
        }
        result["errors"].append(reason);
        LLEventPumps::instance().post(pump_name, result);
        return true;
    }

    bool fetchInventory(LLViewerObject* object, LLEventPump& pump,
        LLInventoryObject::object_list_t& inventory)
    {
        if (!object)
        {
            return false;
        }

        LLEventMailDrop own(ownPumpName("FSPrimInventory-"), true); // <FS:Trish/>
        InventoryFetcher::ptr_t fetcher = std::make_shared<InventoryFetcher>(own, object);
        fetcher->fetch();
        LLSD result = llcoro::suspendUntilEventOnWithTimeout(own, INVENTORY_FETCH_TIMEOUT,
            LLSDMap("timeout", LLSD::Boolean(true)));
        if (result.has("timeout"))
        {
            return false;
        }

        inventory.assign(fetcher->getInventory().begin(), fetcher->getInventory().end());
        return true;
    }

    bool compileScript(LLViewerObject* object, const LLViewerInventoryItem* item,
        const std::string& target, const LLUUID& experience, LLEventPump& pump, LLSD& result)
    {
        if (!object || !item || !object->getRegion() || item->getAssetUUID().isNull())
        {
            return false;
        }

        const std::string url = object->getRegion()->getCapability("UpdateScriptTask");
        if (url.empty())
        {
            return false;
        }

        LLEventMailDrop own(ownPumpName("FSScriptCompile-"), true); // <FS:Trish/>
        LLResourceUploadInfo::ptr_t upload_info = std::make_shared<ExistingScriptAssetUpload>(
            object->getID(), item->getUUID(), item->getAssetUUID(), target, false, experience,
            boost::bind(&uploadSucceeded, own.getName(), _4),
            boost::bind(&uploadFailed, own.getName(), _3, _4));
        LLViewerAssetUpload::EnqueueInventoryUpload(url, upload_info);

        result = llcoro::suspendUntilEventOnWithTimeout(own, SCRIPT_UPLOAD_TIMEOUT,
            LLSDMap("timeout", LLSD::Boolean(true)));
        return !result.has("timeout") && result["compiled"].asBoolean() &&
            result["new_asset"].asUUID().notNull();
    }

    void setScriptRunning(LLViewerObject* object, const LLUUID& item_id, bool running)
    {
        if (!object || !object->getRegion())
        {
            return;
        }

        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_SetScriptRunning);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->nextBlockFast(_PREHASH_Script);
        msg->addUUIDFast(_PREHASH_ObjectID, object->getID());
        msg->addUUIDFast(_PREHASH_ItemID, item_id);
        msg->addBOOLFast(_PREHASH_Running, running);
        msg->sendReliable(object->getRegion()->getHost());
    }
}

FSRegionLuaScripts::FSRegionLuaScripts() = default;

FSRegionLuaScripts::~FSRegionLuaScripts()
{
    gIdleCallbacks.deleteFunction(onIdle, this);
}

void FSRegionLuaScripts::handleMenuAction(const LLSD& userdata)
{
    if (mOperation != Operation::NONE || mWindowLock) // <FS:Trish/> or the Script Repositories window
    {
        FSCommon::report_to_nearby_chat("A region Lua script operation is already running.");
        return;
    }

    Operation operation = Operation::NONE;
    std::string notification;
    const std::string action = userdata.asString();
    if (action == "recompile_owned")
    {
        operation = Operation::RECOMPILE_OWNED;
        notification = "ConfirmRegionLuaRecompile";
    }
    else if (action == "substitute_mine")
    {
        operation = Operation::SUBSTITUTE_MINE;
        notification = "ConfirmRegionLuaSubstituteMine";
    }
    else if (action == "substitute_others")
    {
        operation = Operation::SUBSTITUTE_OTHERS;
        notification = "ConfirmRegionLuaSubstituteOthers";
    }
    else if (action == "substitute_all")
    {
        operation = Operation::SUBSTITUTE_ALL;
        notification = "ConfirmRegionLuaSubstituteAll";
    }

    if (operation == Operation::NONE)
    {
        return;
    }

    LLSD payload;
    payload["operation"] = static_cast<S32>(operation);
    LLNotificationsUtil::add(notification, LLSD(), payload,
        boost::bind(&FSRegionLuaScripts::onConfirmation, this, _1, _2));
}

bool FSRegionLuaScripts::onConfirmation(const LLSD& notification, const LLSD& response)
{
    if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
    {
        beginScan(static_cast<Operation>(notification["payload"]["operation"].asInteger()));
    }
    return false;
}

void FSRegionLuaScripts::beginScan(Operation operation)
{
    LLViewerRegion* region = gAgent.getRegion();
    if (!region)
    {
        FSCommon::report_to_nearby_chat("Cannot start region Lua script operation: no current region.");
        return;
    }

    mOperation = operation;
    mRegionID = region->getRegionID();
    mObjects.clear();

    const S32 object_count = gObjectList.getNumObjects();
    for (S32 index = 0; index < object_count; ++index)
    {
        LLViewerObject* object = gObjectList.getObject(index);
        if (!object || object->isDead() || object->getRegion() != region || object->isAvatar() ||
            object->isAttachment() || object->getPCode() == LLViewerObject::LL_VO_SURFACE_PATCH ||
            !object->mbCanSelect || !object->flagScripted())
        {
            continue;
        }

        ObjectInfo info;
        info.id = object->getID();
        info.local_id = object->getLocalID();
        info.scripted = true;
        LLViewerObject* root = object->getRootEdit();
        info.root_id = root ? root->getID() : info.id;
        mObjects.emplace(info.id, info);

        // A scripted child can have a non-scripted root. Include that root in the
        // property request so warnings identify both the object and the link.
        if (root && root != object && root->getRegion() == region && !root->isDead() &&
            root->mbCanSelect)
        {
            ObjectInfo root_info;
            root_info.id = root->getID();
            root_info.root_id = root_info.id;
            root_info.local_id = root->getLocalID();
            root_info.scripted = root->flagScripted();
            auto inserted = mObjects.emplace(root_info.id, root_info);
            if (!inserted.second && root_info.scripted)
            {
                inserted.first->second.scripted = true;
            }
        }
    }

    mPendingProperties = static_cast<S32>(mObjects.size());
    if (mPendingProperties == 0)
    {
        FSCommon::report_to_nearby_chat("No scripted rezzed prims known to the viewer were found in this region.");
        finishOperation();
        return;
    }

    mScanning = true;
    mScanTimer.reset();
    gIdleCallbacks.addFunction(onIdle, this);
    requestObjectProperties(true);
    requestObjectProperties(false);

    FSCommon::report_to_nearby_chat(llformat("Scanning %d scripted prims in the current region...",
        mPendingProperties));
}

void FSRegionLuaScripts::requestObjectProperties(bool select)
{
    LLViewerRegion* region = gAgent.getRegion();
    if (!region || region->getRegionID() != mRegionID)
    {
        return;
    }

    LLMessageSystem* msg = gMessageSystem;
    bool start_message = true;
    S32 block_count = 0;
    for (const auto& entry : mObjects)
    {
        if (start_message)
        {
            msg->newMessageFast(select ? _PREHASH_ObjectSelect : _PREHASH_ObjectDeselect);
            msg->nextBlockFast(_PREHASH_AgentData);
            msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
            msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
            start_message = false;
        }

        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_ObjectLocalID, entry.second.local_id);
        ++block_count;

        if (msg->isSendFull(nullptr) || block_count >= MAX_OBJECTS_PER_PACKET)
        {
            msg->sendReliable(region->getHost());
            start_message = true;
            block_count = 0;
        }
    }

    if (!start_message)
    {
        msg->sendReliable(region->getHost());
    }
}

void FSRegionLuaScripts::processObjectProperties(LLMessageSystem* msg)
{
    if (!mScanning)
    {
        return;
    }

    const S32 count = msg->getNumberOfBlocksFast(_PREHASH_ObjectData);
    for (S32 index = 0; index < count; ++index)
    {
        LLUUID object_id;
        msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_ObjectID, object_id, index);
        auto found = mObjects.find(object_id);
        if (found == mObjects.end() || found->second.received)
        {
            continue;
        }

        ObjectInfo& info = found->second;
        LLUUID creator_id;
        LLUUID group_id;
        LLUUID last_owner_id;
        U32 base_mask = 0;
        U32 owner_mask = 0;
        U32 group_mask = 0;
        U32 everyone_mask = 0;
        U32 next_owner_mask = 0;

        msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_CreatorID, creator_id, index);
        msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_OwnerID, info.owner_id, index);
        msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_GroupID, group_id, index);
        msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_LastOwnerID, last_owner_id, index);
        msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_BaseMask, base_mask, index);
        msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_OwnerMask, owner_mask, index);
        msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_GroupMask, group_mask, index);
        msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_EveryoneMask, everyone_mask, index);
        msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_NextOwnerMask, next_owner_mask, index);
        msg->getStringFast(_PREHASH_ObjectData, _PREHASH_Name, info.name, index);

        info.permissions.init(creator_id, info.owner_id, last_owner_id, group_id);
        info.permissions.initMasks(base_mask, owner_mask, everyone_mask, group_mask, next_owner_mask);
        info.received = true;
        --mPendingProperties;
    }
}

void FSRegionLuaScripts::onIdle(void* userdata)
{
    FSRegionLuaScripts* self = static_cast<FSRegionLuaScripts*>(userdata);
    if (self && self->mScanning &&
        (self->mPendingProperties == 0 || self->mScanTimer.getElapsedTimeF32() >= PROPERTY_SCAN_TIMEOUT))
    {
        self->finishScan();
    }
}

void FSRegionLuaScripts::finishScan()
{
    mScanning = false;
    gIdleCallbacks.deleteFunction(onIdle, this);

    LLViewerRegion* region = gAgent.getRegion();
    if (!region || region->getRegionID() != mRegionID)
    {
        FSCommon::report_to_nearby_chat("Region Lua script operation cancelled because the region changed.");
        finishOperation();
        return;
    }

    if (mPendingProperties > 0)
    {
        FSCommon::report_to_nearby_chat(llformat(
            "Region Lua scan timed out waiting for %d prim properties; those prims will be skipped.",
            mPendingProperties));
    }

    if (mOperation == Operation::RECOMPILE_OWNED)
    {
        launchRecompile();
    }
    else
    {
        launchSubstitution();
    }
}

void FSRegionLuaScripts::launchRecompile()
{
    LLUUID queue_id;
    queue_id.generate();
    LLFloaterScriptQueue* queue = LLFloaterReg::getTypedInstance<LLFloaterScriptQueue>(
        "compile_queue", LLSD(queue_id));
    if (!queue)
    {
        FSCommon::report_to_nearby_chat("Unable to create the region Lua compile queue.");
        finishOperation();
        return;
    }

    S32 queued = 0;
    for (const auto& entry : mObjects)
    {
        const ObjectInfo& info = entry.second;
        LLViewerObject* object = gObjectList.findObject(info.id);
        if (!info.scripted || !info.received || info.owner_id != gAgent.getID() || !object ||
            !object->permModify() || !object->getRegion() ||
            object->getRegion()->getRegionID() != mRegionID)
        {
            continue;
        }

        std::string root_name = info.name;
        auto root = mObjects.find(info.root_id);
        if (root != mObjects.end() && root->second.received)
        {
            root_name = root->second.name;
        }
        queue->addObject(info.id, root_name, info.name);
        ++queued;
    }

    if (queued == 0)
    {
        queue->closeFloater();
        FSCommon::report_to_nearby_chat("No modifiable scripted prims owned by you were found in the region scan.");
        finishOperation();
        return;
    }

    // Lua scripts compile as luau, LSL scripts as lsl-luau with a luau fallback
    queue->setCompileTarget("auto-luau");
    queue->setLuaFallback(true);
    queue->setReportFailuresToChat(true);
    queue->setConfirmScriptModify(false);
    queue->setTitle(LLTrans::getString("CompileQueueTitle"));
    queue->start();

    FSCommon::report_to_nearby_chat(llformat(
        "Queued %d owned scripted prims for LSL-Luau compilation with Lua fallback.", queued));
    finishOperation();
}

void FSRegionLuaScripts::launchSubstitution()
{
    LLCoros::instance().launch("FSRegionLuaScripts::substitutionCoro",
        boost::bind(&FSRegionLuaScripts::substitutionCoro, this));
}

void FSRegionLuaScripts::substitutionCoro()
{
    LLCoros::set_consuming(true);
    LLUUID pump_id;
    pump_id.generate();
    LLEventMailDrop pump(std::string("RegionLuaSubstitution-") + pump_id.asString(), true);

    LLViewerObject* source_object = nullptr;
    ObjectInfo* source_info = nullptr;
    F64 closest_distance = std::numeric_limits<F64>::max();
    S32 source_count = 0;
    const LLVector3d agent_position = gAgent.getPositionGlobal();

    for (auto& entry : mObjects)
    {
        ObjectInfo& info = entry.second;
        LLViewerObject* object = gObjectList.findObject(info.id);
        if (!info.received || info.name != LUA_SOURCE_PRIM_NAME ||
            info.owner_id != gAgent.getID() || !object || !object->permModify())
        {
            continue;
        }

        ++source_count;
        const F64 distance = dist_vec_squared(object->getPositionGlobal(), agent_position);
        if (distance < closest_distance)
        {
            closest_distance = distance;
            source_object = object;
            source_info = &info;
        }
    }

    if (!source_object || !source_info)
    {
        FSCommon::report_to_nearby_chat(
            "No owned, modifiable prim named \"Lua Script Source\" was found in the region scan.");
        finishOperation();
        return;
    }

    if (source_count > 1)
    {
        FSCommon::report_to_nearby_chat(llformat(
            "Found %d prims named \"Lua Script Source\"; using the closest one.", source_count));
    }

    LLInventoryObject::object_list_t source_inventory;
    if (!fetchInventory(source_object, pump, source_inventory))
    {
        FSCommon::report_to_nearby_chat("Unable to retrieve inventory from \"Lua Script Source\".");
        finishOperation();
        return;
    }

    std::map<std::string, LLPointer<LLViewerInventoryItem>> source_scripts;
    S32 script_items = 0;
    S32 noncopy_scripts = 0;
    for (const LLPointer<LLInventoryObject>& inventory_object : source_inventory)
    {
        LLViewerInventoryItem* item = dynamic_cast<LLViewerInventoryItem*>(inventory_object.get());
        if (!item || item->getType() != LLAssetType::AT_LSL_TEXT)
        {
            continue;
        }

        ++script_items;
        if (!item->getPermissions().allowCopyBy(gAgent.getID(), gAgent.getGroupID()))
        {
            ++noncopy_scripts;
            continue;
        }
        if (source_scripts.find(item->getName()) != source_scripts.end())
        {
            FSCommon::report_to_nearby_chat(std::string("Warning: duplicate source script name \"") +
                item->getName() + "\"; only the first will be used.");
            continue;
        }
        source_scripts[item->getName()] = new LLViewerInventoryItem(item);
    }

    if (source_scripts.empty())
    {
        FSCommon::report_to_nearby_chat(llformat(
            "\"Lua Script Source\" has %d script items, but none are copyable (%d non-copy).",
            script_items, noncopy_scripts));
        finishOperation();
        return;
    }

    for (auto source = source_scripts.begin(); source != source_scripts.end();)
    {
        LLUUID fetched_asset_id;
        S32 fetch_status = LL_ERR_NOERR;
        if (!fetchScriptAsset(source_object, source->second, pump, fetched_asset_id, fetch_status))
        {
            FSCommon::report_to_nearby_chat(std::string("Warning: could not retrieve source text for script \"") +
                source->first + "\" (asset status " + llformat("%d", fetch_status) +
                "); it will not be substituted.");
            source = source_scripts.erase(source);
        }
        else
        {
            source->second->setAssetUUID(fetched_asset_id);
            ++source;
        }
    }

    if (source_scripts.empty())
    {
        FSCommon::report_to_nearby_chat(
            "No source scripts could be downloaded from \"Lua Script Source\"; see the warnings above.");
        finishOperation();
        return;
    }

    uuid_vec_t temporary_agent_items;
    for (auto source = source_scripts.begin(); source != source_scripts.end();)
    {
        LLPointer<LLViewerInventoryItem> agent_item =
            copyTaskScriptToAgentInventory(source_object, source->second);
        if (!agent_item)
        {
            FSCommon::report_to_nearby_chat(std::string(
                "Warning: could not make a temporary inventory copy of source script \"") +
                source->first + "\"; it will not be substituted.");
            source = source_scripts.erase(source);
        }
        else
        {
            temporary_agent_items.push_back(agent_item->getUUID());
            source->second = new LLViewerInventoryItem(agent_item.get());
            ++source;
        }
    }

    if (source_scripts.empty())
    {
        FSCommon::report_to_nearby_chat(
            "No source scripts could be copied to temporary agent inventory.");
        finishOperation();
        return;
    }

    S32 replacements = 0;
    S32 failures = 0;
    S32 changed_prims = 0;
    for (const auto& entry : mObjects)
    {
        const ObjectInfo& info = entry.second;
        if (!info.scripted || !info.received || info.root_id == source_info->root_id ||
            (mOperation == Operation::SUBSTITUTE_MINE && info.owner_id != gAgent.getID()) ||
            (mOperation == Operation::SUBSTITUTE_OTHERS && info.owner_id == gAgent.getID()) ||
            !gAgent.allowOperation(PERM_MODIFY, info.permissions, GP_OBJECT_MANIPULATE))
        {
            continue;
        }

        LLViewerObject* object = gObjectList.findObject(info.id);
        if (!object || !object->getRegion() || object->getRegion()->getRegionID() != mRegionID)
        {
            continue;
        }

        LLInventoryObject::object_list_t target_inventory;
        if (!fetchInventory(object, pump, target_inventory))
        {
            FSCommon::report_to_nearby_chat(std::string("Warning: could not read inventory for object \"") +
                info.name + "\"; skipping it.");
            ++failures;
            continue;
        }

        bool prim_changed = false;
        for (const LLPointer<LLInventoryObject>& inventory_object : target_inventory)
        {
            LLViewerInventoryItem* old_script = dynamic_cast<LLViewerInventoryItem*>(inventory_object.get());
            if (!old_script || old_script->getType() != LLAssetType::AT_LSL_TEXT)
            {
                continue;
            }

            auto source = source_scripts.find(old_script->getName());
            if (source == source_scripts.end())
            {
                continue;
            }

            // <FS:Trish> The swap itself is shared with the Script Repositories window
            const std::string original_name = old_script->getName();
            bool original_removed = false;
            std::string error;
            if (substituteScript(object, toScanned(info), old_script, source->second, "lsl-luau", "luau", true,
                LLUUID::null, pump, original_removed, error))
            {
                ++replacements;
                prim_changed = true;
            }
            else
            {
                ++failures;
                if (error == "removal")
                {
                    FSCommon::report_to_nearby_chat(std::string(
                        "Warning: could not confirm removal of script \"") + original_name +
                        "\" from other-owned object \"" + info.name + "\"; replacement was not added.");
                }
                else
                {
                    FSCommon::report_to_nearby_chat(std::string("Warning: Lua substitution failed for object \"") +
                        toScanned(info).root_name + "\", link \"" + info.name + "\", script \"" + original_name +
                        (original_removed ? "\"; the original script had already been removed. " :
                            "\"; the original script was kept. ") + error);
                }
            }
            // </FS:Trish>
            llcoro::suspend();
        }

        if (prim_changed)
        {
            ++changed_prims;
        }
    }

    FSCommon::report_to_nearby_chat(llformat(
        "Lua script substitution complete: %d scripts replaced in %d prims, %d failures.",
        replacements, changed_prims, failures));
    for (const LLUUID& item_id : temporary_agent_items)
    {
        remove_inventory_item(item_id, nullptr, true);
    }
    finishOperation();
}

// <FS:Trish> Shared with the Script Repositories window
namespace
{
    std::set<std::string> sRunningWaiters;

    std::string runningPumpName(const LLUUID& object_id, const LLUUID& item_id)
    {
        return "FSScriptRunning-" + object_id.asString() + "-" + item_id.asString();
    }
}

FSRegionLuaScripts::ScannedPrim FSRegionLuaScripts::toScanned(const ObjectInfo& info) const
{
    ScannedPrim prim;
    prim.id = info.id;
    prim.root_id = info.root_id;
    prim.owner_id = info.owner_id;
    prim.name = info.name;
    prim.root_name = info.name;
    auto root = mObjects.find(info.root_id);
    if (root != mObjects.end() && root->second.received)
    {
        prim.root_name = root->second.name;
    }
    prim.permissions = info.permissions;
    prim.scripted = info.scripted;
    return prim;
}

bool FSRegionLuaScripts::lockForWindow()
{
    if (mOperation != Operation::NONE || mWindowLock)
    {
        return false;
    }
    mWindowLock = true;
    return true;
}

bool FSRegionLuaScripts::fetchPrimInventory(LLViewerObject* object, LLEventPump& pump,
    LLInventoryObject::object_list_t& inventory)
{
    return fetchInventory(object, pump, inventory);
}

bool FSRegionLuaScripts::fetchScriptText(LLViewerObject* object, const LLViewerInventoryItem* item, LLEventPump& pump,
    std::string& text)
{
    LLUUID asset_id;
    S32 status = LL_ERR_NOERR;
    if (!fetchScriptAsset(object, item, pump, asset_id, status))
    {
        return false;
    }
    LLFileSystem file(asset_id, LLAssetType::AT_LSL_TEXT, LLFileSystem::READ);
    const S32 size = file.getSize();
    if (size <= 0)
    {
        return false;
    }
    text.assign((size_t)size, '\0');
    if (!file.read(reinterpret_cast<U8*>(text.data()), size))
    {
        return false;
    }
    // Script assets end with a NUL
    while (!text.empty() && text.back() == '\0')
    {
        text.pop_back();
    }
    return true;
}

bool FSRegionLuaScripts::queryScriptRunning(LLViewerObject* object, const LLUUID& item_id, LLEventPump& pump,
    bool& running, std::string& target)
{
    if (!object || !object->getRegion())
    {
        return false;
    }
    const std::string name = runningPumpName(object->getID(), item_id);
    LLEventMailDrop reply(name, true);
    sRunningWaiters.insert(name);

    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(_PREHASH_GetScriptRunning);
    msg->nextBlockFast(_PREHASH_Script);
    msg->addUUIDFast(_PREHASH_ObjectID, object->getID());
    msg->addUUIDFast(_PREHASH_ItemID, item_id);
    msg->sendReliable(object->getRegion()->getHost());

    LLSD result = llcoro::suspendUntilEventOnWithTimeout(reply, 15.f, LLSDMap("timeout", LLSD::Boolean(true)));
    sRunningWaiters.erase(name);
    if (result.has("timeout"))
    {
        return false;
    }
    running = result["running"].asBoolean();
    target = result["target"].asString();
    return true;
}

void FSRegionLuaScripts::onScriptRunningReply(const LLUUID& object_id, const LLUUID& item_id, bool running, bool mono,
    bool luau, bool luau_language)
{
    const std::string name = runningPumpName(object_id, item_id);
    if (!sRunningWaiters.count(name))
    {
        return;
    }
    LLSD result;
    result["running"] = running;
    result["target"] = luau ? (luau_language ? "luau" : "lsl-luau") : (mono ? "mono" : "lsl2");
    LLEventPumps::instance().post(name, result);
}

bool FSRegionLuaScripts::substituteScript(LLViewerObject* object, const ScannedPrim& prim,
    LLViewerInventoryItem* old_script, const LLViewerInventoryItem* agent_source, const std::string& target,
    const std::string& fallback_target, bool running, const LLUUID& experience, LLEventPump& pump,
    bool& original_removed, std::string& error)
{
    original_removed = false;
    if (!object || !old_script || !agent_source || !object->getRegion())
    {
        error = "Nothing to substitute.";
        return false;
    }
    // Held across every suspend below; a teleport or the object's removal stops the swap at the next step
    LLPointer<LLViewerObject> keep(object);
    const LLUUID region_id = object->getRegion()->getRegionID();
    auto gone = [&keep, &region_id, &error]()
    {
        if (keep->isDead() || !keep->getRegion() || keep->getRegion()->getRegionID() != region_id)
        {
            error = "The object is gone or the region changed.";
            return true;
        }
        return false;
    };

    LLPointer<LLViewerInventoryItem> replacement = new LLViewerInventoryItem(agent_source);
    const std::string original_name = old_script->getName();
    const bool can_rename_task_inventory = prim.owner_id == gAgent.getID();
    // Read fresh: an earlier swap in the same prim added an item the caller's list does not have
    LLInventoryObject::object_list_t inventory_before_add;
    if (!fetchInventory(object, pump, inventory_before_add))
    {
        error = "The object's contents could not be read.";
        return false;
    }
    if (gone())
    {
        return false;
    }

    if (can_rename_task_inventory)
    {
        replacement->rename(std::string("__FS_LUA_SWAP_") + replacement->getUUID().asString());
    }
    else
    {
        // Delegated edit rights permit task inventory add/remove but
        // not rename. Remove the collision first, confirm the server
        // inventory changed, then add the replacement with its final
        // name directly.
        replacement->rename(original_name);
        object->removeInventory(old_script->getUUID());
        if (!fetchInventoryAfterRemoval(object, old_script->getUUID(), pump, inventory_before_add))
        {
            error = "removal";
            return false;
        }
        original_removed = true;
        if (gone())
        {
            return false;
        }
    }

    object->saveScript(replacement, false, true, LLUUID::null);
    LLInventoryObject::object_list_t staged_inventory;
    LLPointer<LLViewerInventoryItem> staged_item;
    for (S32 attempt = 0; attempt < 10 && !staged_item; ++attempt)
    {
        // saveScript() adds an optimistic local item using the agent
        // inventory UUID. Force a server refresh so we see the actual
        // task item UUID assigned by the simulator.
        object->dirtyInventory();
        staged_inventory.clear();
        if (fetchInventory(object, pump, staged_inventory))
        {
            staged_item = findNewScriptItem(inventory_before_add, staged_inventory);
        }
        if (!staged_item)
        {
            llcoro::suspendUntilTimeout(0.25f);
        }
        if (gone())
        {
            return false;
        }
    }

    LLUUID staged_asset_id;
    S32 staged_fetch_status = LL_ERR_NOERR;
    const bool staged_asset_fetched = staged_item &&
        fetchScriptAsset(object, staged_item, pump, staged_asset_id, staged_fetch_status);
    if (staged_asset_fetched)
    {
        staged_item->setAssetUUID(staged_asset_id);
    }

    LLSD compile_result;
    bool compiled = staged_asset_fetched && compileScript(object, staged_item, target, experience, pump, compile_result);
    if (staged_asset_fetched && !compiled && !fallback_target.empty() && !gone())
    {
        compiled = compileScript(object, staged_item, fallback_target, experience, pump, compile_result);
    }
    if (gone())
    {
        return false;
    }

    if (compiled)
    {
        if (can_rename_task_inventory)
        {
            object->removeInventory(old_script->getUUID());
            LLPointer<LLViewerInventoryItem> renamed_item = new LLViewerInventoryItem(staged_item.get());
            renamed_item->setAssetUUID(compile_result["new_asset"].asUUID());
            renamed_item->rename(original_name);
            object->updateInventory(renamed_item, TASK_INVENTORY_ITEM_KEY, false);
        }
        if (running)
        {
            setScriptRunning(object, staged_item->getUUID(), true);
        }
        return true;
    }

    if (staged_item)
    {
        object->removeInventory(staged_item->getUUID());
    }
    if (!staged_item)
    {
        error = "The staged task item could not be identified.";
    }
    else if (!staged_asset_fetched)
    {
        error = llformat("The staged source could not be downloaded (asset status %d).", staged_fetch_status);
    }
    else if (!fallback_target.empty())
    {
        error = "LSL-Luau and Lua compilation both failed.";
    }
    else
    {
        const LLSD& errors = compile_result["errors"];
        error = errors.isArray() && errors.size() > 0 ? errors[0].asString() : std::string("It does not compile.");
    }
    return false;
}
// </FS:Trish>

void FSRegionLuaScripts::finishOperation()
{
    mScanning = false;
    gIdleCallbacks.deleteFunction(onIdle, this);
    mObjects.clear();
    mPendingProperties = 0;
    mRegionID.setNull();
    mOperation = Operation::NONE;
}
