/**
 * @file fsmassobjectedit.cpp
 * @brief Region-wide multi-object task inventory editor.
 */

#include "llviewerprecompiledheaders.h"

#include "fsmassobjectedit.h"

#include "fscommon.h"
#include "llagent.h"
#include "llassetstorage.h"
#include "llbutton.h"
#include "llcallbacklist.h"
#include "llcoros.h"
#include "lleventcoro.h"
#include "llevents.h"
#include "llinventorydefines.h"
#include "llinventorymodel.h"
#include "message.h"
#include "llnotificationsutil.h"
#include "llscrolllistctrl.h"
#include "llselectmgr.h"
#include "lltextbox.h"
#include "lltooldraganddrop.h"
#include "llviewerinventory.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewerassetupload.h"
#include "llvoinventorylistener.h"
#include "roles_constants.h"

#include <algorithm>
#include <memory>
#include <set>

namespace
{
    constexpr F32 PROPERTY_TIMEOUT = 15.f;
    constexpr F32 INVENTORY_TIMEOUT = 60.f;
    constexpr S32 MAX_OBJECTS_PER_PACKET = 254;

    class InventoryFetcher final : public LLVOInventoryListener
    {
    public:
        InventoryFetcher(LLEventPump& pump, LLViewerObject* object) : mPump(pump)
        {
            registerVOInventoryListener(object, nullptr);
        }

        void fetch() { requestVOInventory(); }
        const LLInventoryObject::object_list_t& inventory() const { return mInventory; }

        void inventoryChanged(LLViewerObject*, LLInventoryObject::object_list_t* inventory,
            S32, void*) override
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

    class ExistingScriptAssetUpload final : public LLScriptAssetUpload
    {
    public:
        ExistingScriptAssetUpload(const LLUUID& task_id, const LLUUID& item_id,
            const LLUUID& asset_id, TargetType_t target, taskUploadFinish_f finish,
            uploadFailed_f failed) :
            LLScriptAssetUpload(task_id, item_id, target, false, LLUUID::null,
                std::string(), finish, failed)
        {
            setAssetId(asset_id);
        }

        LLSD prepareUpload() override { return LLSDMap("success", true); }
    };

    void assetFetched(const LLUUID& asset_id, LLAssetType::EType, void* userdata,
        S32 status, LLExtStat)
    {
        std::unique_ptr<std::string> pump_name(static_cast<std::string*>(userdata));
        LLEventPumps::instance().post(*pump_name,
            LLSDMap("asset_id", asset_id)("status", status));
    }

    bool fetchScriptAsset(LLViewerObject* object, LLViewerInventoryItem* item,
        LLEventPump& pump)
    {
        if (!object || !item || !gAssetStorage)
        {
            return false;
        }
        std::string* pump_name = new std::string(pump.getName());
        gAssetStorage->getInvItemAsset(LLHost(), gAgent.getID(), gAgent.getSessionID(),
            item->getPermissions().getOwner(), object->getID(), item->getUUID(),
            item->getAssetUUID(), item->getType(), &assetFetched, pump_name, true);
        LLSD result = llcoro::suspendUntilEventOnWithTimeout(pump, INVENTORY_TIMEOUT,
            LLSDMap("timeout", true));
        if (result.has("timeout") || result["status"].asInteger() != LL_ERR_NOERR ||
            result["asset_id"].asUUID().isNull())
        {
            return false;
        }
        item->setAssetUUID(result["asset_id"].asUUID());
        return true;
    }

    void uploadSucceeded(const std::string& pump_name, const LLSD& result)
    {
        LLEventPumps::instance().post(pump_name, result);
    }

    bool uploadFailed(const std::string& pump_name, LLSD result, const std::string& reason)
    {
        result["compiled"] = false;
        result["reason"] = reason;
        LLEventPumps::instance().post(pump_name, result);
        return true;
    }

    bool compileScript(LLViewerObject* object, LLViewerInventoryItem* item,
        LLScriptAssetUpload::TargetType_t target, LLEventPump& pump, LLSD& result)
    {
        if (!object || !object->getRegion() || !item || item->getAssetUUID().isNull())
        {
            return false;
        }
        const std::string url = object->getRegion()->getCapability("UpdateScriptTask");
        if (url.empty())
        {
            return false;
        }
        LLResourceUploadInfo::ptr_t upload = std::make_shared<ExistingScriptAssetUpload>(
            object->getID(), item->getUUID(), item->getAssetUUID(), target,
            boost::bind(&uploadSucceeded, pump.getName(), _4),
            boost::bind(&uploadFailed, pump.getName(), _3, _4));
        LLViewerAssetUpload::EnqueueInventoryUpload(url, upload);
        result = llcoro::suspendUntilEventOnWithTimeout(pump, INVENTORY_TIMEOUT,
            LLSDMap("timeout", true));
        return !result.has("timeout") && result["compiled"].asBoolean() &&
            result["new_asset"].asUUID().notNull();
    }

    void setScriptRunning(LLViewerObject* object, const LLUUID& item_id, bool running)
    {
        if (!object || !object->getRegion()) return;
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

    bool fetchInventory(LLViewerObject* object, LLEventPump& pump,
        LLInventoryObject::object_list_t& inventory, bool force_server = false)
    {
        if (!object)
        {
            return false;
        }
        if (force_server)
        {
            object->dirtyInventory();
        }

        std::shared_ptr<InventoryFetcher> fetcher = std::make_shared<InventoryFetcher>(pump, object);
        fetcher->fetch();
        LLSD result = llcoro::suspendUntilEventOnWithTimeout(pump, INVENTORY_TIMEOUT,
            LLSDMap("timeout", true));
        if (result.has("timeout"))
        {
            return false;
        }
        inventory.assign(fetcher->inventory().begin(), fetcher->inventory().end());
        return true;
    }

    LLPointer<LLViewerInventoryItem> findNewItem(
        const LLInventoryObject::object_list_t& before,
        const LLInventoryObject::object_list_t& after,
        LLAssetType::EType type)
    {
        std::set<LLUUID> ids;
        for (const LLPointer<LLInventoryObject>& item : before)
        {
            ids.insert(item->getUUID());
        }
        for (const LLPointer<LLInventoryObject>& object : after)
        {
            LLViewerInventoryItem* item = dynamic_cast<LLViewerInventoryItem*>(object.get());
            if (item && item->getType() == type && ids.find(item->getUUID()) == ids.end())
            {
                return item;
            }
        }
        return nullptr;
    }

    LLPointer<LLViewerInventoryItem> copyTaskItemToAgent(LLViewerObject* source,
        const LLViewerInventoryItem* source_item)
    {
        if (!source || !source_item)
        {
            return nullptr;
        }

        LLUUID folder_id = gInventory.findCategoryUUIDForType(
            LLFolderType::assetTypeToFolderType(source_item->getType()));
        if (folder_id.isNull())
        {
            folder_id = gInventory.getRootFolderID();
        }

        std::set<LLUUID> before;
        LLInventoryModel::cat_array_t* categories = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(folder_id, categories, items);
        if (items)
        {
            for (const LLPointer<LLViewerInventoryItem>& item : *items)
            {
                before.insert(item->getUUID());
            }
        }

        source->moveInventory(folder_id, source_item->getUUID());
        for (F32 elapsed = 0.f; elapsed < INVENTORY_TIMEOUT; elapsed += 0.25f)
        {
            llcoro::suspendUntilTimeout(0.25f);
            categories = nullptr;
            items = nullptr;
            gInventory.getDirectDescendentsOf(folder_id, categories, items);
            if (!items)
            {
                continue;
            }
            for (const LLPointer<LLViewerInventoryItem>& item : *items)
            {
                if (before.find(item->getUUID()) == before.end() &&
                    item->getType() == source_item->getType() &&
                    item->getName() == source_item->getName() && item->isFinished())
                {
                    return item;
                }
            }
        }
        return nullptr;
    }

    std::string contentKey(const std::string& name, LLAssetType::EType type)
    {
        return llformat("%d|", static_cast<S32>(type)) + name;
    }

    std::string assetTypeName(LLAssetType::EType type)
    {
        std::string name = LLAssetType::lookupHumanReadable(type);
        return name.empty() ? LLAssetType::lookup(type) : name;
    }
}

FSMassObjectEdit::FSMassObjectEdit(const LLSD& key) : LLFloater(key)
{
}

FSMassObjectEdit::~FSMassObjectEdit()
{
    gIdleCallbacks.deleteFunction(onIdle, this);
}

bool FSMassObjectEdit::postBuild()
{
    mTargetList = getChild<LLScrollListCtrl>("target_list");
    mSourceList = getChild<LLScrollListCtrl>("source_list");
    mTargetContentsList = getChild<LLScrollListCtrl>("target_contents_list");
    mSourceLabel = getChild<LLTextBox>("source_label");
    mStatusText = getChild<LLTextBox>("status_text");
    mAddButton = getChild<LLButton>("add_btn");
    mReplaceButton = getChild<LLButton>("replace_btn");
    mDeleteButton = getChild<LLButton>("delete_btn");

    getChild<LLButton>("refresh_btn")->setCommitCallback(
        boost::bind(&FSMassObjectEdit::refreshObjects, this));
    getChild<LLButton>("use_source_btn")->setCommitCallback(
        boost::bind(&FSMassObjectEdit::useSelectedSource, this));
    getChild<LLButton>("scan_contents_btn")->setCommitCallback(
        boost::bind(&FSMassObjectEdit::scanTargetContents, this));
    mAddButton->setCommitCallback(boost::bind(&FSMassObjectEdit::beginOperation, this, Operation::ADD));
    mReplaceButton->setCommitCallback(boost::bind(&FSMassObjectEdit::beginOperation, this, Operation::REPLACE));
    mDeleteButton->setCommitCallback(boost::bind(&FSMassObjectEdit::beginOperation, this, Operation::DELETE_ITEMS));
    mTargetList->setCommitCallback(boost::bind(&FSMassObjectEdit::updateButtons, this));
    mSourceList->setCommitCallback(boost::bind(&FSMassObjectEdit::updateButtons, this));
    mTargetContentsList->setCommitCallback(boost::bind(&FSMassObjectEdit::updateButtons, this));
    updateButtons();
    return LLFloater::postBuild();
}

void FSMassObjectEdit::onOpen(const LLSD&)
{
    refreshObjects();
}

void FSMassObjectEdit::setStatus(const std::string& status)
{
    if (mStatusText)
    {
        mStatusText->setText(status);
    }
}

void FSMassObjectEdit::updateButtons()
{
    const bool targets = mTargetList && !mTargetList->getAllSelected().empty();
    const bool source = mSourceList && !mSourceList->getAllSelected().empty();
    const bool contents = mTargetContentsList && !mTargetContentsList->getAllSelected().empty();
    mAddButton->setEnabled(!mBusy && targets && source);
    mReplaceButton->setEnabled(!mBusy && targets && source);
    mDeleteButton->setEnabled(!mBusy && targets && contents);
}

void FSMassObjectEdit::refreshObjects()
{
    if (mBusy || mScanning)
    {
        return;
    }
    LLViewerRegion* region = gAgent.getRegion();
    if (!region)
    {
        return;
    }

    mTargetList->deleteAllItems();
    mTargetContentsList->deleteAllItems();
    mTargetContentKeys.clear();
    mObjects.clear();
    mRegionID = region->getRegionID();

    for (S32 i = 0; i < gObjectList.getNumObjects(); ++i)
    {
        LLViewerObject* object = gObjectList.getObject(i);
        if (!object || object->isDead() || object->getRegion() != region || object->isAvatar() ||
            object->isAttachment() || object->getPCode() == LLViewerObject::LL_VO_SURFACE_PATCH ||
            !object->mbCanSelect)
        {
            continue;
        }
        ObjectInfo info;
        info.id = object->getID();
        info.local_id = object->getLocalID();
        LLViewerObject* root = object->getRootEdit();
        info.root_id = root ? root->getID() : info.id;
        mObjects.emplace(info.id, info);
    }

    mPendingProperties = static_cast<S32>(mObjects.size());
    mScanning = mPendingProperties > 0;
    mScanTimer.reset();
    if (mScanning)
    {
        gIdleCallbacks.addFunction(onIdle, this);
        requestObjectProperties(true);
        requestObjectProperties(false);
        setStatus(llformat("Scanning %d current-region prims...", mPendingProperties));
    }
}

void FSMassObjectEdit::requestObjectProperties(bool select)
{
    LLViewerRegion* region = gAgent.getRegion();
    if (!region || region->getRegionID() != mRegionID)
    {
        return;
    }
    LLMessageSystem* msg = gMessageSystem;
    bool start = true;
    S32 blocks = 0;
    for (const auto& entry : mObjects)
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
        msg->addU32Fast(_PREHASH_ObjectLocalID, entry.second.local_id);
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

void FSMassObjectEdit::processObjectProperties(LLMessageSystem* msg)
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
        LLUUID creator, group, last_owner;
        U32 base = 0, owner = 0, everyone = 0, group_mask = 0, next = 0;
        msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_CreatorID, creator, i);
        msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_OwnerID, info.owner_id, i);
        msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_GroupID, group, i);
        msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_LastOwnerID, last_owner, i);
        msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_BaseMask, base, i);
        msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_OwnerMask, owner, i);
        msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_EveryoneMask, everyone, i);
        msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_GroupMask, group_mask, i);
        msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_NextOwnerMask, next, i);
        msg->getStringFast(_PREHASH_ObjectData, _PREHASH_Name, info.name, i);
        info.permissions.init(creator, info.owner_id, last_owner, group);
        info.permissions.initMasks(base, owner, everyone, group_mask, next);
        info.received = true;
        --mPendingProperties;
    }
}

void FSMassObjectEdit::onIdle(void* userdata)
{
    FSMassObjectEdit* self = static_cast<FSMassObjectEdit*>(userdata);
    if (self && self->mScanning && (self->mPendingProperties == 0 ||
        self->mScanTimer.getElapsedTimeF32() >= PROPERTY_TIMEOUT))
    {
        self->finishObjectScan();
    }
}

void FSMassObjectEdit::finishObjectScan()
{
    mScanning = false;
    gIdleCallbacks.deleteFunction(onIdle, this);
    for (const auto& entry : mObjects)
    {
        const ObjectInfo& info = entry.second;
        if (!info.received || !gAgent.allowOperation(PERM_MODIFY, info.permissions,
            GP_OBJECT_MANIPULATE))
        {
            continue;
        }
        LLSD row;
        row["id"] = info.id;
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = info.name;
        row["columns"][1]["column"] = "owner";
        row["columns"][1]["value"] = info.owner_id == gAgent.getID() ? "Me" : info.owner_id.asString();
        mTargetList->addElement(row, ADD_BOTTOM);
    }
    setStatus(llformat("Found %d editable prims.", mTargetList->getItemCount()));
    updateButtons();
}

uuid_vec_t FSMassObjectEdit::getSelectedTargets() const
{
    uuid_vec_t ids;
    for (LLScrollListItem* row : mTargetList->getAllSelected())
    {
        ids.push_back(row->getValue().asUUID());
    }
    return ids;
}

void FSMassObjectEdit::useSelectedSource()
{
    LLViewerObject* object = LLSelectMgr::getInstance()->getSelection()->getFirstObject();
    if (!object || object->isAvatar() || object->isAttachment())
    {
        setStatus("Select one rezzed prim, then click Use Selected Prim as Source.");
        return;
    }
    mSourceObjectID = object->getID();
    mSourceLabel->setText(object->getID().asString());
    mSourceList->deleteAllItems();
    mSourceItems.clear();
    LLCoros::instance().launch("FSMassObjectEdit::fetchSourceInventory",
        boost::bind(&FSMassObjectEdit::fetchSourceInventoryCoro, this, mSourceObjectID));
}

void FSMassObjectEdit::fetchSourceInventoryCoro(LLUUID source_id)
{
    LLUUID pump_id = LLUUID::generateNewID();
    LLEventMailDrop pump("MassObjectSource-" + pump_id.asString(), true);
    LLViewerObject* object = gObjectList.findObject(source_id);
    LLInventoryObject::object_list_t inventory;
    if (!fetchInventory(object, pump, inventory))
    {
        setStatus("Could not retrieve source inventory.");
        return;
    }

    for (const LLPointer<LLInventoryObject>& inv_object : inventory)
    {
        LLViewerInventoryItem* item = dynamic_cast<LLViewerInventoryItem*>(inv_object.get());
        if (!item)
        {
            continue;
        }
        mSourceItems[item->getUUID()] = new LLViewerInventoryItem(item);
        LLSD row;
        row["id"] = item->getUUID();
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = item->getName();
        row["columns"][1]["column"] = "type";
        row["columns"][1]["value"] = assetTypeName(item->getType());
        row["columns"][2]["column"] = "permissions";
        row["columns"][2]["value"] = item->getPermissions().allowCopyBy(
            gAgent.getID(), gAgent.getGroupID()) ? "Yes" : "No";
        mSourceList->addElement(row, ADD_BOTTOM);
    }
    mSourceLabel->setText(llformat("%d source items", mSourceList->getItemCount()));
    setStatus("Source inventory loaded.");
    updateButtons();
}

void FSMassObjectEdit::scanTargetContents()
{
    if (mBusy || getSelectedTargets().empty())
    {
        return;
    }
    mBusy = true;
    updateButtons();
    LLCoros::instance().launch("FSMassObjectEdit::scanTargetContents",
        boost::bind(&FSMassObjectEdit::scanTargetContentsCoro, this));
}

void FSMassObjectEdit::scanTargetContentsCoro()
{
    LLUUID pump_id = LLUUID::generateNewID();
    LLEventMailDrop pump("MassObjectTargets-" + pump_id.asString(), true);
    mTargetContentKeys.clear();
    mTargetContentsList->deleteAllItems();
    const uuid_vec_t targets = getSelectedTargets();
    for (const LLUUID& id : targets)
    {
        LLInventoryObject::object_list_t inventory;
        if (!fetchInventory(gObjectList.findObject(id), pump, inventory))
        {
            continue;
        }
        std::set<std::string> seen;
        for (const LLPointer<LLInventoryObject>& object : inventory)
        {
            LLViewerInventoryItem* item = dynamic_cast<LLViewerInventoryItem*>(object.get());
            if (!item)
            {
                continue;
            }
            const std::string key = contentKey(item->getName(), item->getType());
            if (seen.insert(key).second)
            {
                ContentKey& content = mTargetContentKeys[key];
                content.name = item->getName();
                content.type = item->getType();
                ++content.count;
            }
        }
        llcoro::suspend();
    }

    for (const auto& entry : mTargetContentKeys)
    {
        const ContentKey& content = entry.second;
        LLSD row;
        row["id"] = entry.first;
        row["columns"][0]["column"] = "name";
        row["columns"][0]["value"] = content.name;
        row["columns"][1]["column"] = "type";
        row["columns"][1]["value"] = assetTypeName(content.type);
        row["columns"][2]["column"] = "count";
        row["columns"][2]["value"] = content.count;
        mTargetContentsList->addElement(row, ADD_BOTTOM);
    }
    mBusy = false;
    setStatus(llformat("Scanned contents of %d targets.", static_cast<S32>(targets.size())));
    updateButtons();
}

void FSMassObjectEdit::beginOperation(Operation operation)
{
    uuid_vec_t targets = getSelectedTargets();
    uuid_vec_t source_items;
    std::vector<std::string> delete_keys;
    if (operation == Operation::DELETE_ITEMS)
    {
        for (LLScrollListItem* row : mTargetContentsList->getAllSelected())
        {
            delete_keys.push_back(row->getValue().asString());
        }
    }
    else
    {
        for (LLScrollListItem* row : mSourceList->getAllSelected())
        {
            source_items.push_back(row->getValue().asUUID());
        }
    }
    if (targets.empty() || (source_items.empty() && delete_keys.empty()))
    {
        return;
    }

    LLSD args;
    args["OPERATION"] = operation == Operation::ADD ? "Add" :
        (operation == Operation::REPLACE ? "Replace" : "Delete");
    args["ITEM_COUNT"] = static_cast<S32>(operation == Operation::DELETE_ITEMS ?
        delete_keys.size() : source_items.size());
    args["TARGET_COUNT"] = static_cast<S32>(targets.size());
    args["WARNING"] = operation == Operation::REPLACE ?
        "Other-owned objects require delete-before-add replacement and cannot be rolled back if adding fails." :
        (operation == Operation::DELETE_ITEMS ? "Deletion cannot be undone." : "");
    LLSD payload;
    payload["operation"] = static_cast<S32>(operation);
    for (const LLUUID& id : targets) payload["targets"].append(id);
    for (const LLUUID& id : source_items) payload["source_items"].append(id);
    for (const std::string& key : delete_keys) payload["delete_keys"].append(key);
    LLNotificationsUtil::add("ConfirmMassObjectEdit", args, payload,
        boost::bind(&FSMassObjectEdit::confirmOperation, this, _1, _2));
}

bool FSMassObjectEdit::confirmOperation(const LLSD& notification, const LLSD& response)
{
    if (LLNotificationsUtil::getSelectedOption(notification, response) != 0 || mBusy)
    {
        return false;
    }
    const LLSD& payload = notification["payload"];
    uuid_vec_t targets, source_items;
    std::vector<std::string> delete_keys;
    for (const LLSD& value : llsd::inArray(payload["targets"])) targets.push_back(value.asUUID());
    for (const LLSD& value : llsd::inArray(payload["source_items"])) source_items.push_back(value.asUUID());
    for (const LLSD& value : llsd::inArray(payload["delete_keys"])) delete_keys.push_back(value.asString());
    mBusy = true;
    updateButtons();
    LLCoros::instance().launch("FSMassObjectEdit::operation",
        boost::bind(&FSMassObjectEdit::operationCoro, this,
            static_cast<Operation>(payload["operation"].asInteger()), targets,
            source_items, delete_keys));
    return false;
}

void FSMassObjectEdit::operationCoro(Operation operation, uuid_vec_t targets,
    uuid_vec_t source_item_ids, std::vector<std::string> delete_keys)
{
    LLUUID pump_id = LLUUID::generateNewID();
    LLEventMailDrop pump("MassObjectApply-" + pump_id.asString(), true);
    LLViewerObject* source_object = gObjectList.findObject(mSourceObjectID);
    std::map<LLUUID, LLPointer<LLViewerInventoryItem>> agent_items;
    uuid_vec_t temporary_items;
    S32 changes = 0;
    S32 failures = 0;

    if (operation != Operation::DELETE_ITEMS)
    {
        for (const LLUUID& source_id : source_item_ids)
        {
            auto found = mSourceItems.find(source_id);
            if (found == mSourceItems.end() ||
                !found->second->getPermissions().allowCopyBy(gAgent.getID(), gAgent.getGroupID()))
            {
                ++failures;
                continue;
            }
            LLPointer<LLViewerInventoryItem> item = copyTaskItemToAgent(source_object, found->second);
            if (!item)
            {
                ++failures;
                continue;
            }
            agent_items[source_id] = item;
            temporary_items.push_back(item->getUUID());
        }
    }

    for (const LLUUID& target_id : targets)
    {
        LLViewerObject* target = gObjectList.findObject(target_id);
        auto object_info = mObjects.find(target_id);
        if (!target || object_info == mObjects.end() || !gAgent.allowOperation(
            PERM_MODIFY, object_info->second.permissions, GP_OBJECT_MANIPULATE))
        {
            ++failures;
            continue;
        }

        LLInventoryObject::object_list_t inventory;
        if (!fetchInventory(target, pump, inventory))
        {
            ++failures;
            continue;
        }

        if (operation == Operation::DELETE_ITEMS)
        {
            std::set<std::string> selected(delete_keys.begin(), delete_keys.end());
            for (const LLPointer<LLInventoryObject>& object : inventory)
            {
                LLViewerInventoryItem* item = dynamic_cast<LLViewerInventoryItem*>(object.get());
                if (item && selected.find(contentKey(item->getName(), item->getType())) != selected.end())
                {
                    target->removeInventory(item->getUUID());
                    ++changes;
                }
            }
            continue;
        }

        for (const LLUUID& source_id : source_item_ids)
        {
            auto found = agent_items.find(source_id);
            if (found == agent_items.end())
            {
                continue;
            }
            LLPointer<LLViewerInventoryItem> agent_item = found->second;
            std::vector<LLUUID> matches;
            for (const LLPointer<LLInventoryObject>& object : inventory)
            {
                LLViewerInventoryItem* item = dynamic_cast<LLViewerInventoryItem*>(object.get());
                if (item && item->getName() == agent_item->getName() &&
                    item->getType() == agent_item->getType())
                {
                    matches.push_back(item->getUUID());
                }
            }

            const bool replace = operation == Operation::REPLACE && !matches.empty();
            const bool own_object = object_info->second.owner_id == gAgent.getID();
            LLInventoryObject::object_list_t before_add = inventory;
            LLPointer<LLViewerInventoryItem> item_to_add = new LLViewerInventoryItem(agent_item.get());
            if (replace && !own_object)
            {
                for (const LLUUID& id : matches) target->removeInventory(id);
                bool removal_confirmed = false;
                for (S32 attempt = 0; attempt < 10 && !removal_confirmed; ++attempt)
                {
                    before_add.clear();
                    if (fetchInventory(target, pump, before_add, true))
                    {
                        removal_confirmed = true;
                        for (const LLPointer<LLInventoryObject>& object : before_add)
                        {
                            if (std::find(matches.begin(), matches.end(), object->getUUID()) != matches.end())
                            {
                                removal_confirmed = false;
                                break;
                            }
                        }
                    }
                    if (!removal_confirmed) llcoro::suspendUntilTimeout(0.25f);
                }
                if (!removal_confirmed)
                {
                    ++failures;
                    continue;
                }
            }
            else if (replace)
            {
                item_to_add->rename("__FS_MASS_EDIT_" + item_to_add->getUUID().asString());
            }

            if (item_to_add->getType() == LLAssetType::AT_LSL_TEXT)
            {
                target->saveScript(item_to_add, false, true);
            }
            else
            {
                LLToolDragAndDrop::dropInventory(target, item_to_add,
                    LLToolDragAndDrop::SOURCE_AGENT, LLUUID::null);
            }

            LLInventoryObject::object_list_t after_add;
            LLPointer<LLViewerInventoryItem> added;
            for (S32 attempt = 0; attempt < 10 && !added; ++attempt)
            {
                if (fetchInventory(target, pump, after_add, true))
                {
                    added = findNewItem(before_add, after_add, item_to_add->getType());
                }
                if (!added) llcoro::suspendUntilTimeout(0.25f);
            }
            if (!added)
            {
                ++failures;
                continue;
            }

            if (added->getType() == LLAssetType::AT_LSL_TEXT)
            {
                LLSD compile_result;
                bool compiled = fetchScriptAsset(target, added, pump) &&
                    compileScript(target, added, LLScriptAssetUpload::LSL_LUAU,
                        pump, compile_result);
                if (!compiled)
                {
                    compiled = compileScript(target, added, LLScriptAssetUpload::LUAU,
                        pump, compile_result);
                }
                if (!compiled)
                {
                    target->removeInventory(added->getUUID());
                    ++failures;
                    FSCommon::report_to_nearby_chat(std::string(
                        "Warning: mass edit script compilation failed for object \"") +
                        object_info->second.name + "\", script \"" + agent_item->getName() +
                        "\" using both LSL-Luau and Lua." +
                        (replace && !own_object ? " The original had already been removed." : ""));
                    continue;
                }
                added->setAssetUUID(compile_result["new_asset"].asUUID());
            }

            if (replace && own_object)
            {
                for (const LLUUID& id : matches) target->removeInventory(id);
                LLPointer<LLViewerInventoryItem> renamed = new LLViewerInventoryItem(added.get());
                renamed->rename(agent_item->getName());
                target->updateInventory(renamed, TASK_INVENTORY_ITEM_KEY, false);
            }
            if (added->getType() == LLAssetType::AT_LSL_TEXT)
            {
                setScriptRunning(target, added->getUUID(), true);
            }
            ++changes;
            inventory = after_add;
            llcoro::suspend();
        }
    }

    for (const LLUUID& id : temporary_items)
    {
        remove_inventory_item(id, nullptr, true);
    }
    mBusy = false;
    setStatus(llformat("Mass object edit complete: %d changes, %d failures.", changes, failures));
    updateButtons();
}
