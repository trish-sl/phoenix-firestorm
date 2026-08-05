/**
 * @file fsregionluascripts.h
 * @brief Region-wide Lua script maintenance tools.
 */

#ifndef FS_REGIONLUASCRIPTS_H
#define FS_REGIONLUASCRIPTS_H

#include "llframetimer.h"
#include "llpermissions.h"
#include "llsingleton.h"
#include "lluuid.h"

#include <map>
#include <string>

class LLMessageSystem;
class LLSD;

class FSRegionLuaScripts : public LLSingleton<FSRegionLuaScripts>
{
    LLSINGLETON(FSRegionLuaScripts);
    ~FSRegionLuaScripts();

public:
    void handleMenuAction(const LLSD& userdata);
    void processObjectProperties(LLMessageSystem* msg);
    bool isScanning() const { return mScanning; }

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

    static void onIdle(void* userdata);
    void requestObjectProperties(bool select);

    Operation mOperation{ Operation::NONE };
    std::map<LLUUID, ObjectInfo> mObjects;
    LLUUID mRegionID;
    LLFrameTimer mScanTimer;
    S32 mPendingProperties{ 0 };
    bool mScanning{ false };
};

#endif // FS_REGIONLUASCRIPTS_H
