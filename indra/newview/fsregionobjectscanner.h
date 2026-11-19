/**
 * @file fsregionobjectscanner.h
 * @brief Finds every object in the current region and reads prim contents in parallel. Shared by Mass Object Edit
 *        and Script Repositories.
 */

#ifndef FS_REGIONOBJECTSCANNER_H
#define FS_REGIONOBJECTSCANNER_H

#include "llframetimer.h"
#include "llinventory.h"
#include "llpermissions.h"
#include "lluuid.h"

#include <functional>
#include <map>
#include <set>
#include <string>

class LLMessageSystem;

// <FS:Trish> While open, the interest list is in 360 mode so the viewer learns about every object in the region, not
// only those in draw distance. Object properties are asked for in throttled batches through ObjectSelect/Deselect
// (never for objects the user has selected), unresolved ones are retried, objects streaming in later are added, and a
// region change starts over.
class FSRegionObjectScanner
{
public:
    enum class PropertyRequestState { NEED, DEFERRED, SENT, RECEIVED };

    struct ObjectInfo
    {
        LLUUID id;
        LLUUID root_id;
        LLUUID creator_id;
        LLUUID owner_id;
        U32 local_id{ 0 };
        std::string name;
        LLPermissions permissions;
        PropertyRequestState property_request{ PropertyRequestState::NEED };
        bool received{ false };
        bool group_owned{ false };

        // Filled by Mass Object Edit for its name filters
        std::string creator_name;
        std::string creator_search_name;
        std::string owner_name;
        std::string owner_search_name;
    };
    using objects_t = std::map<LLUUID, ObjectInfo>;

    FSRegionObjectScanner();
    ~FSRegionObjectScanner();

    // Called on the main thread. onReady: the interest list had time to fill (after open or a region change); when
    // unset the scanner calls refresh() itself.
    std::function<void()> onReady;
    std::function<void(const LLUUID& id)> onObjectReceived;
    std::function<void()> onScanFinished;
    std::function<void(const std::string& status)> onStatus;
    std::function<void()> onStateChanged;

    void open();
    void close(bool app_quitting = false);
    bool isOpen() const { return mOpen; }
    // A new snapshot of the region's objects. False while a scan runs.
    bool refresh();
    // Mass Object Edit pauses the late-object pass while it changes prims
    std::function<bool()> isReconcilePaused;

    bool isScanning() const { return mScanning; }
    bool isWaiting() const { return mWaitingForInterestList; }
    const LLUUID& getRegionID() const { return mRegionID; }
    S32 getPendingProperties() const { return mPendingProperties; }
    objects_t& objects() { return mObjects; }
    const objects_t& objects() const { return mObjects; }

    static bool anyScanning();
    static void dispatchObjectProperties(LLMessageSystem* msg);

    // Coroutine only. Reads the contents of targets with a pool of workers (setting FSMassObjectEditInventoryWorkers)
    // and retries the ones that did not answer. on_contents is called once per prim that answered.
    struct ContentsResult
    {
        S32 total{ 0 };
        S32 succeeded{ 0 };
        S32 failed{ 0 };
        S32 retries_used{ 0 };
        uuid_vec_t failed_ids;  // no answer after the retries
    };
    using contents_f = std::function<void(const LLUUID& id, const LLInventoryObject::object_list_t& inventory)>;
    using progress_f = std::function<void(S32 processed, S32 total, S32 failed)>;
    using retry_f = std::function<void(S32 count, S32 attempt, S32 max_attempts)>;
    static ContentsResult fetchContents(const uuid_vec_t& targets, bool force_server, contents_f on_contents,
        progress_f on_progress = nullptr, retry_f on_retry = nullptr, std::function<bool()> cancelled = nullptr,
        S32 min_workers = 1);

private:
    void status(const std::string& text) const;
    void stateChanged() const;
    void reconcileObjects();
    bool retryUnresolvedProperties();
    void requestObjectProperties(const std::vector<U32>& local_ids, bool select);
    void processPropertyRequestQueue();
    void processObjectProperties(LLMessageSystem* msg);
    void finishObjectScan();
    static void onIdle(void* userdata);

    objects_t mObjects;
    uuid_vec_t mPropertyRequestIDs;
    LLUUID mRegionID;
    LLFrameTimer mScanTimer;
    LLFrameTimer mInterestListTimer;
    LLFrameTimer mReconcileTimer;
    size_t mPropertyRequestIndex{ 0 };
    S32 mPendingProperties{ 0 };
    S32 mPropertyRequestsInFlight{ 0 };
    S32 mPropertyRetryCount{ 0 };
    bool mOpen{ false };
    bool mScanning{ false };
    bool mWaitingForInterestList{ false };

    static std::set<FSRegionObjectScanner*> sScanners;
    static S32 s360Users;
};
// </FS:Trish>

#endif // FS_REGIONOBJECTSCANNER_H
