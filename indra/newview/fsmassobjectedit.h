/**
 * @file fsmassobjectedit.h
 * @brief Region-wide multi-object task inventory editor.
 */

#ifndef FS_MASSOBJECTEDIT_H
#define FS_MASSOBJECTEDIT_H

#include "llfloater.h"
#include "llframetimer.h"
#include "llpermissions.h"
#include "llviewerassettype.h"

#include <map>
#include <vector>

class LLButton;
class LLMessageSystem;
class LLScrollListCtrl;
class LLTextBox;
class LLViewerInventoryItem;

class FSMassObjectEdit final : public LLFloater
{
    friend class LLFloaterReg;

public:
    FSMassObjectEdit(const LLSD& key);
    ~FSMassObjectEdit() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void processObjectProperties(LLMessageSystem* msg);
    bool isScanning() const { return mScanning; }

private:
    enum class Operation { ADD, REPLACE, DELETE_ITEMS };

    struct ObjectInfo
    {
        LLUUID id;
        LLUUID root_id;
        LLUUID owner_id;
        U32 local_id{ 0 };
        std::string name;
        LLPermissions permissions;
        bool received{ false };
    };

    struct ContentKey
    {
        std::string name;
        LLAssetType::EType type{ LLAssetType::AT_NONE };
        S32 count{ 0 };
    };

    void refreshObjects();
    void useSelectedSource();
    void scanTargetContents();
    void beginOperation(Operation operation);
    bool confirmOperation(const LLSD& notification, const LLSD& response);
    void scanTargetContentsCoro();
    void operationCoro(Operation operation, uuid_vec_t targets,
        uuid_vec_t source_items, std::vector<std::string> delete_keys);
    void fetchSourceInventoryCoro(LLUUID source_id);
    void updateButtons();
    void setStatus(const std::string& status);

    static void onIdle(void* userdata);
    void requestObjectProperties(bool select);
    void finishObjectScan();
    uuid_vec_t getSelectedTargets() const;

    LLScrollListCtrl* mTargetList{ nullptr };
    LLScrollListCtrl* mSourceList{ nullptr };
    LLScrollListCtrl* mTargetContentsList{ nullptr };
    LLTextBox* mSourceLabel{ nullptr };
    LLTextBox* mStatusText{ nullptr };
    LLButton* mAddButton{ nullptr };
    LLButton* mReplaceButton{ nullptr };
    LLButton* mDeleteButton{ nullptr };

    std::map<LLUUID, ObjectInfo> mObjects;
    std::map<LLUUID, LLPointer<LLViewerInventoryItem>> mSourceItems;
    std::map<std::string, ContentKey> mTargetContentKeys;
    LLUUID mRegionID;
    LLUUID mSourceObjectID;
    LLFrameTimer mScanTimer;
    S32 mPendingProperties{ 0 };
    bool mScanning{ false };
    bool mBusy{ false };
};

#endif
