/**
 * @file fsregionluascripts.h
 * @brief Region-wide Lua script maintenance tools.
 */

#ifndef FS_REGIONLUASCRIPTS_H
#define FS_REGIONLUASCRIPTS_H

#include "llframetimer.h"
#include "llinventory.h"
#include "llpermissions.h"
#include "llsingleton.h"
#include "lluuid.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

class LLEventPump;
class LLMessageSystem;
class LLSD;
class LLViewerInventoryItem;
class LLViewerObject;

class FSRegionLuaScripts : public LLSingleton<FSRegionLuaScripts>
{
    LLSINGLETON(FSRegionLuaScripts);
    ~FSRegionLuaScripts();

public:
    void handleMenuAction(const LLSD& userdata);
    void processObjectProperties(LLMessageSystem* msg);
    bool isScanning() const { return mScanning; }

    // <FS:Trish> Shared with the Script Repositories window
    struct ScannedPrim
    {
        LLUUID id;
        LLUUID root_id;
        LLUUID owner_id;
        std::string name;
        std::string root_name;
        LLPermissions permissions;
        bool scripted{ false };
    };

    // Held by the Script Repositories window for its whole region check and apply, so the menu's operations
    // cannot swap scripts in the same prims at the same time. False if anything else is running.
    bool lockForWindow();
    void unlockForWindow() { mWindowLock = false; }

    // Coroutine only.
    static bool fetchPrimInventory(LLViewerObject* object, LLEventPump& pump, LLInventoryObject::object_list_t& inventory);
    static bool fetchScriptText(LLViewerObject* object, const LLViewerInventoryItem* item, LLEventPump& pump,
        std::string& text);
    // Running state and compile target ("mono", "lsl2", "lsl-luau" or "luau") from the simulator.
    static bool queryScriptRunning(LLViewerObject* object, const LLUUID& item_id, LLEventPump& pump, bool& running,
        std::string& target);
    // From LLLiveLSLEditor::processScriptRunningReply, for queryScriptRunning.
    static void onScriptRunningReply(const LLUUID& object_id, const LLUUID& item_id, bool running, bool mono,
        bool luau, bool luau_language);

    // Replaces old_script in object with a copy of agent_source. The copy is added under a temporary name and
    // compiled (target, then fallback_target if given); only once it compiles is the original removed and the copy
    // renamed. Objects owned by someone else take the delete-first path (no rename right there). On failure the
    // staged copy is removed; original_removed tells whether the original was already gone.
    static bool substituteScript(LLViewerObject* object, const ScannedPrim& prim, LLViewerInventoryItem* old_script,
        const LLViewerInventoryItem* agent_source, const std::string& target, const std::string& fallback_target,
        bool running, const LLUUID& experience, LLEventPump& pump, bool& original_removed, std::string& error);
    // </FS:Trish>

private:
    enum class Operation
    {
        NONE,
        RECOMPILE_OWNED,
        SUBSTITUTE_MINE,
        SUBSTITUTE_OTHERS,
        SUBSTITUTE_ALL
    };

    struct ObjectInfo
    {
        LLUUID id;
        LLUUID root_id;
        LLUUID owner_id;
        U32 local_id{ 0 };
        std::string name;
        LLPermissions permissions;
        bool scripted{ false };
        bool received{ false };
    };

    bool onConfirmation(const LLSD& notification, const LLSD& response);
    void beginScan(Operation operation);
    void finishScan();
    void launchRecompile();
    void launchSubstitution();
    void substitutionCoro();
    void finishOperation();

    ScannedPrim toScanned(const ObjectInfo& info) const; // <FS:Trish/>

    static void onIdle(void* userdata);
    void requestObjectProperties(bool select);

    Operation mOperation{ Operation::NONE };
    std::map<LLUUID, ObjectInfo> mObjects;
    LLUUID mRegionID;
    LLFrameTimer mScanTimer;
    S32 mPendingProperties{ 0 };
    bool mScanning{ false };
    bool mWindowLock{ false }; // <FS:Trish/>
};

#endif // FS_REGIONLUASCRIPTS_H
