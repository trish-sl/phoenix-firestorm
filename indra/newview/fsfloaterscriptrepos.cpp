/**
 * @file fsfloaterscriptrepos.cpp
 * @brief Script Repositories window: syncs GitHub repositories into the Scripts folder and updates same-named
 *        scripts in the user's objects in the region.
 */

#include "llviewerprecompiledheaders.h"

#include "fsfloaterscriptrepos.h"

#include "fsscriptreposfetch.h"
#include "llagent.h"
#include "llagentbenefits.h"
#include "llbutton.h"
#include "llcallbacklist.h"
#include "llcheckboxctrl.h"
#include "llcoros.h"
#include "lldir.h"
#include "lleventcoro.h"
#include "llevents.h"
#include "llfile.h"
#include "llfloaterperms.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "lllineeditor.h"
#include "llnotecard.h"
#include "llprogressbar.h"
#include "llscrolllistctrl.h"
#include "llsdserialize.h"
#include "lltextbox.h"
#include "llviewerassettype.h"
#include "llviewerassetupload.h"
#include "llviewerinventory.h"
#include "llviewermenufile.h"
#include "llviewerregion.h"
#include "fsregionluascripts.h"
#include "fsregionobjectscanner.h"
#include "llexperiencecache.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "roles_constants.h"

using namespace FSScriptRepos;

namespace
{
    constexpr F32 FOLDER_TIMEOUT = 30.f;
    constexpr F32 UPLOAD_TIMEOUT = 60.f;
    constexpr F32 IMAGE_TIMEOUT = 120.f;
    const std::string STATE_FILE("script_repos.xml");

    std::string statePath()
    {
        return gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, STATE_FILE);
    }

    bool regionHasLua()
    {
        LLViewerRegion* region = gAgent.getRegion();
        if (!region || !region->simulatorFeaturesReceived())
        {
            return false;
        }
        LLSD features;
        region->getSimulatorFeatures(features);
        return features["LuaScriptsEnabled"].asBoolean();
    }

    std::string shortCommit(const std::string& commit)
    {
        return commit.substr(0, 7);
    }

    LLStringUtil::format_map_t fargs(std::initializer_list<std::pair<const char*, std::string>> list)
    {
        LLStringUtil::format_map_t args;
        for (const auto& [key, value] : list)
        {
            args[std::string("[") + key + "]"] = value;
        }
        return args;
    }

    // Each request waits on its own mail drop: a reply that comes after its wait timed out must not wake a later
    // wait (a mail drop keeps events nobody took).
    std::string ownPumpName(const char* prefix)
    {
        return std::string(prefix) + LLUUID::generateNewID().asString();
    }

    LLSD waitOn(LLEventPump& pump, F32 timeout)
    {
        return llcoro::suspendUntilEventOnWithTimeout(pump, timeout, LLSDMap("timeout", LLSD::Boolean(true)));
    }

    LLUUID findChildFolder(const LLUUID& parent, const std::string& name)
    {
        LLInventoryModel::cat_array_t* cats = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(parent, cats, items);
        if (cats)
        {
            for (const LLPointer<LLViewerInventoryCategory>& cat : *cats)
            {
                if (cat->getName() == name)
                {
                    return cat->getUUID();
                }
            }
        }
        return LLUUID::null;
    }

    // Suspends. Null on failure.
    LLUUID ensureFolder(const LLUUID& parent, const std::string& name, LLEventPump& pump)
    {
        LLUUID found = findChildFolder(parent, name);
        if (found.notNull())
        {
            return found;
        }
        LLEventMailDrop own(ownPumpName("ScriptReposFolder-"), true);
        const std::string pump_name = own.getName();
        gInventory.createNewCategory(parent, LLFolderType::FT_NONE, name,
            [pump_name](const LLUUID& id) { LLEventPumps::instance().post(pump_name, LLSDMap("id", id)); });
        LLSD result = waitOn(own, FOLDER_TIMEOUT);
        return result.has("timeout") ? LLUUID::null : result["id"].asUUID();
    }

    // Suspends. Null on failure.
    LLUUID createItem(const LLUUID& folder, const Entry& entry, LLEventPump& pump)
    {
        const bool script = entry.kind == Kind::LSL || entry.kind == Kind::LUA;
        const LLAssetType::EType asset_type = script ? LLAssetType::AT_LSL_TEXT : LLAssetType::AT_NOTECARD;
        std::string desc;
        LLViewerAssetType::generateDescriptionFor(asset_type, desc);
        LLEventMailDrop own(ownPumpName("ScriptReposItem-"), true);
        const std::string pump_name = own.getName();
        LLPointer<LLInventoryCallback> cb = new LLBoostFuncInventoryCallback(
            [pump_name](const LLUUID& id) { LLEventPumps::instance().post(pump_name, LLSDMap("id", id)); });
        create_inventory_item(gAgent.getID(), gAgent.getSessionID(), folder, LLTransactionID::tnull, entry.name, desc,
            asset_type, script ? LLInventoryType::IT_LSL : LLInventoryType::IT_NOTECARD,
            entry.kind == Kind::LUA ? SST_LUA : NO_INV_SUBTYPE,
            LLFloaterPerms::getNextOwnerPerms(script ? "Scripts" : "Notecards"), cb);
        LLSD result = waitOn(own, FOLDER_TIMEOUT);
        return result.has("timeout") ? LLUUID::null : result["id"].asUUID();
    }

    // Suspends. False when nothing was saved; compile_error is set when a script was saved but does not compile.
    bool uploadText(const LLUUID& item_id, const Entry& entry, LLEventPump& pump, std::string& error,
        std::string& compile_error, LLUUID& asset_id)
    {
        LLViewerRegion* region = gAgent.getRegion();
        if (!region)
        {
            error = "not connected";
            return false;
        }

        LLEventMailDrop own(ownPumpName("ScriptReposUpload-"), true);
        const std::string pump_name = own.getName();
        auto finished = [pump_name](LLUUID, LLUUID new_asset, LLUUID, LLSD response)
        {
            LLSD out = LLSDMap("done", LLSD::Boolean(true));
            out["asset"] = new_asset;
            out["compiled"] = !response.has("compiled") || response["compiled"].asBoolean();
            out["errors"] = response["errors"];
            LLEventPumps::instance().post(pump_name, out);
        };
        auto failed = [pump_name](LLUUID, LLUUID, LLSD, std::string reason)
        {
            LLEventPumps::instance().post(pump_name, LLSDMap("failed", reason));
            return true;
        };

        std::string url;
        LLResourceUploadInfo::ptr_t info;
        if (entry.kind == Kind::NOTECARD)
        {
            LLNotecard notecard;
            notecard.setText(entry.data);
            std::ostringstream buffer;
            if (!notecard.exportStream(buffer))
            {
                error = "notecard text";
                return false;
            }
            url = region->getCapability("UpdateNotecardAgentInventory");
            info = std::make_shared<LLBufferedAssetUploadInfo>(item_id, LLAssetType::AT_NOTECARD, buffer.str(),
                finished, failed);
        }
        else
        {
            url = region->getCapability("UpdateScriptAgent");
            info = std::make_shared<LLScriptAssetUpload>(item_id, entry.kind == Kind::LUA ? "luau" : "mono",
                entry.data, finished, failed);
        }
        if (url.empty())
        {
            error = "the region cannot save it";
            return false;
        }

        LLViewerAssetUpload::EnqueueInventoryUpload(url, info);
        LLSD result = waitOn(own, UPLOAD_TIMEOUT);
        if (result.has("failed"))
        {
            error = result["failed"].asString();
            return false;
        }
        if (!result.has("done"))
        {
            error = "no answer";
            return false;
        }
        asset_id = result["asset"].asUUID();
        if (!result["compiled"].asBoolean())
        {
            compile_error = result["errors"].size() > 0 ? result["errors"][0].asString() : std::string("?");
        }
        return true;
    }

    // Suspends. The new texture item, or null.
    LLUUID uploadImage(const LLUUID& folder, const Entry& entry, S32 cost)
    {
        const size_t dot = entry.path.rfind('.');
        const std::string file = gDirUtilp->getExpandedFilename(LL_PATH_TEMP,
            "script_repos_" + LLUUID::generateNewID().asString() + entry.path.substr(dot));
        {
            llofstream out(file.c_str(), std::ios::binary);
            out.write(entry.data.data(), entry.data.size());
            if (!out.good())
            {
                return LLUUID::null;
            }
        }

        std::set<LLUUID> before;
        LLInventoryModel::cat_array_t* cats = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(folder, cats, items);
        if (items)
        {
            for (const LLPointer<LLViewerInventoryItem>& item : *items)
            {
                before.insert(item->getUUID());
            }
        }

        LLResourceUploadInfo::ptr_t info = std::make_shared<LLNewFileResourceUploadInfo>(file, entry.name,
            std::string(), 0, LLFolderType::FT_TEXTURE, LLInventoryType::IT_TEXTURE,
            LLFloaterPerms::getNextOwnerPerms("Uploads"), LLFloaterPerms::getGroupPerms("Uploads"),
            LLFloaterPerms::getEveryonePerms("Uploads"), cost, folder, false);
        upload_new_resource(info);

        // The upload creates the item itself; find it by name in the folder
        LLUUID found;
        for (F32 elapsed = 0.f; elapsed < IMAGE_TIMEOUT && found.isNull(); elapsed += 0.5f)
        {
            llcoro::suspendUntilTimeout(0.5f);
            cats = nullptr;
            items = nullptr;
            gInventory.getDirectDescendentsOf(folder, cats, items);
            if (items)
            {
                for (const LLPointer<LLViewerInventoryItem>& item : *items)
                {
                    if (item->getType() == LLAssetType::AT_TEXTURE && item->getName() == entry.name &&
                        !before.count(item->getUUID()))
                    {
                        found = item->getUUID();
                    }
                }
            }
        }
        LLFile::remove(file);
        return found;
    }
}

FSFloaterScriptRepos::FSFloaterScriptRepos(const LLSD& key) :
    LLFloater(key)
{
}

bool FSFloaterScriptRepos::postBuild()
{
    mRepoList = getChild<LLScrollListCtrl>("repos");
    mResults = getChild<LLScrollListCtrl>("results");
    mRepoEdit = getChild<LLLineEditor>("repo_edit");
    mBranchEdit = getChild<LLLineEditor>("branch_edit");
    mIgnoreEdit = getChild<LLLineEditor>("ignore_edit");
    mPrefixEdit = getChild<LLLineEditor>("prefix_edit");
    mTokenEdit = getChild<LLLineEditor>("token_edit");
    mImages = getChild<LLCheckBoxCtrl>("images");
    mStatus = getChild<LLTextBox>("status");
    mProgress = getChild<LLProgressBar>("progress");

    childSetAction("add", [this](LLUICtrl*, const LLSD&) { onAdd(); });
    childSetAction("remove", [this](LLUICtrl*, const LLSD&) { onRemove(); });
    childSetAction("forget", [this](LLUICtrl*, const LLSD&) { onForget(); });
    childSetAction("check", [this](LLUICtrl*, const LLSD&) { onCheck(); });
    childSetAction("check_region", [this](LLUICtrl*, const LLSD&) { onRegionCheck(); });
    childSetAction("apply", [this](LLUICtrl*, const LLSD&) { onApply(); });
    childSetAction("cancel", [this](LLUICtrl*, const LLSD&) { onCancel(); });
    mRepoEdit->setCommitCallback([this](LLUICtrl*, const LLSD&) { onAdd(); });
    mTokenEdit->setCommitOnFocusLost(true);
    mTokenEdit->setCommitCallback([this](LLUICtrl*, const LLSD&) { onTokenCommit(); });
    mIgnoreEdit->setCommitOnFocusLost(true);
    mIgnoreEdit->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSettingsChanged(); });
    mImages->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSettingsChanged(); });
    mPrefixEdit->setCommitOnFocusLost(true);
    mPrefixEdit->setCommitCallback([this](LLUICtrl*, const LLSD&) { onPrefixChanged(); });

    load();
    refreshRepos();
    refreshToken();
    setBusy(false);
    setProgress(0, 0);
    return true;
}

void FSFloaterScriptRepos::onClose(bool app_quitting)
{
    onCancel(); // wakes a waiting region check, which then lets go of the region tools
    if (mRegionScanner)
    {
        mRegionScanner->close(app_quitting);
        mRegionScanner.reset();
    }
}

FSFloaterScriptRepos* FSFloaterScriptRepos::self(LLHandle<LLFloater>& handle)
{
    return static_cast<FSFloaterScriptRepos*>(handle.get());
}

//
// State file
//

void FSFloaterScriptRepos::load()
{
    LLSD sd;
    llifstream in(statePath().c_str());
    if (in.is_open())
    {
        LLSDSerialize::fromXMLDocument(sd, in);
    }
    mRepos.clear();
    for (const RepoState& state : fromLLSD(sd["repos"]))
    {
        Repo repo;
        repo.state = state;
        mRepos.push_back(repo);
    }
    mIgnoreEdit->setText(sd.has("ignore") ? sd["ignore"].asString() : DEFAULT_IGNORE);
    mImages->set(sd["images"].asBoolean());
    mPrefix = sd["prefix"].asString();
    mPrefixEdit->setText(mPrefix);
}

void FSFloaterScriptRepos::save()
{
    std::vector<RepoState> states;
    for (const Repo& repo : mRepos)
    {
        states.push_back(repo.state);
    }
    LLSD sd;
    sd["repos"] = toLLSD(states);
    sd["ignore"] = mIgnoreEdit->getText();
    sd["images"] = mImages->get();
    sd["prefix"] = mPrefix;

    llofstream out(statePath().c_str());
    if (out.is_open())
    {
        LLSDSerialize::toPrettyXML(sd, out);
    }
    else
    {
        LL_WARNS("ScriptRepos") << "Could not write " << statePath() << LL_ENDL;
    }
}

//
// UI
//

void FSFloaterScriptRepos::refreshRepos()
{
    mRepoList->deleteAllItems();
    for (size_t i = 0; i < mRepos.size(); ++i)
    {
        const RepoState& state = mRepos[i].state;
        LLSD row;
        row["value"] = (S32)i;
        row["columns"][0]["column"] = "repo";
        row["columns"][0]["value"] = state.ref.display();
        row["columns"][1]["column"] = "branch";
        row["columns"][1]["value"] = state.ref.ref.empty() ? getString("default_branch") : state.ref.ref;
        row["columns"][2]["column"] = "synced";
        row["columns"][2]["value"] = state.commit.empty() && state.files.empty() ? getString("never_synced") :
            state.synced.toHTTPDateString("%Y-%m-%d %H:%M") + (state.commit.empty() ? "" : " @ " + shortCommit(state.commit));
        mRepoList->addElement(row);
    }
}

void FSFloaterScriptRepos::refreshToken()
{
    mTokenEdit->clear();
    mTokenEdit->setLabel(getString(loadToken().empty() ? "token_none" : "token_saved"));
}

void FSFloaterScriptRepos::setBusy(bool busy)
{
    mBusy = busy;
    for (const char* name : { "add", "remove", "forget", "check", "check_region", "repo_edit", "branch_edit",
                              "ignore_edit", "token_edit", "images", "prefix_edit" })
    {
        getChildView(name)->setEnabled(!busy);
    }
    // A region check lists rows while it reads: Apply stops the reading and applies what is listed
    getChildView("apply")->setEnabled((!busy && mMode == Mode::INVENTORY && !mInvRows.empty()) ||
        (mMode == Mode::REGION && !mRegionRows.empty() && !mApplyAfterCheck));
    getChildView("cancel")->setEnabled(busy);
}

void FSFloaterScriptRepos::setStatus(const std::string& text)
{
    mStatus->setText(text);
}

void FSFloaterScriptRepos::setProgress(size_t done, size_t total)
{
    mProgress->setValue(total ? 100.f * (F32)done / (F32)total : 0.f);
}

void FSFloaterScriptRepos::addResult(const std::string& action, const std::string& what, const std::string& kind,
    const std::string& note, const LLSD& value)
{
    LLSD row;
    row["value"] = value;
    row["columns"][0]["column"] = "action";
    row["columns"][0]["value"] = action;
    row["columns"][1]["column"] = "what";
    row["columns"][1]["value"] = what;
    row["columns"][2]["column"] = "kind";
    row["columns"][2]["value"] = kind;
    row["columns"][3]["column"] = "note";
    row["columns"][3]["value"] = note;
    mResults->addElement(row);
}

void FSFloaterScriptRepos::clearResults()
{
    mResults->deleteAllItems();
    mInvRows.clear();
    mRegionRows.clear();
    mRegionScanner.reset(); // back to the normal interest list
    mMode = Mode::NONE;
}

std::string FSFloaterScriptRepos::kindLabel(Kind kind) const
{
    switch (kind)
    {
    case Kind::LSL: return getString("kind_lsl");
    case Kind::LUA: return getString("kind_lua");
    case Kind::NOTECARD: return getString("kind_notecard");
    case Kind::IMAGE: return getString("kind_image");
    default: return std::string();
    }
}

void FSFloaterScriptRepos::onAdd()
{
    RepoRef ref;
    if (!parseRepoRef(mRepoEdit->getText(), mBranchEdit->getText(), ref))
    {
        setStatus(getString("bad_repo"));
        return;
    }
    for (const Repo& repo : mRepos)
    {
        if (LLStringUtil::compareInsensitive(repo.state.ref.display(), ref.display()) == 0 &&
            repo.state.ref.ref == ref.ref)
        {
            setStatus(getString("duplicate_repo"));
            return;
        }
    }
    Repo repo;
    repo.state.ref = ref;
    mRepos.push_back(repo);
    mRepoEdit->clear();
    mBranchEdit->clear();
    clearResults();
    save();
    refreshRepos();
    setStatus(std::string());
    setBusy(false);
}

void FSFloaterScriptRepos::onRemove()
{
    LLScrollListItem* item = mRepoList->getFirstSelected();
    if (!item)
    {
        return;
    }
    const S32 index = item->getValue().asInteger();
    if (index < 0 || index >= (S32)mRepos.size())
    {
        return;
    }
    // The synced items stay in the inventory; only the entry and its sync state go
    mRepos.erase(mRepos.begin() + index);
    clearResults();
    save();
    refreshRepos();
    setBusy(false);
}

void FSFloaterScriptRepos::onForget()
{
    forgetToken();
    refreshToken();
}

void FSFloaterScriptRepos::onTokenCommit()
{
    std::string token = mTokenEdit->getText();
    LLStringUtil::trim(token);
    if (!token.empty())
    {
        saveToken(token);
    }
    refreshToken();
}

void FSFloaterScriptRepos::onSettingsChanged()
{
    // Other files may count now: the next check downloads every repository again
    for (Repo& repo : mRepos)
    {
        repo.state.commit.clear();
        repo.loaded = false;
    }
    clearResults();
    save();
    setBusy(false);
}

void FSFloaterScriptRepos::onPrefixChanged()
{
    const std::string prefix = mPrefixEdit->getText();
    if (prefix == mPrefix || mBusy)
    {
        mPrefixEdit->setText(mPrefix);
        return;
    }
    // Rename the folders already synced, so their items stay found
    const LLUUID scripts = gInventory.findCategoryUUIDForType(LLFolderType::FT_LSL_TEXT);
    std::vector<std::pair<LLUUID, std::string>> renames;
    for (size_t i = 0; i < mRepos.size(); ++i)
    {
        const LLUUID folder = findRepoFolder(i);
        if (folder.notNull())
        {
            renames.emplace_back(folder, prefix + repoFolderName(i).substr(mPrefix.size()));
        }
    }
    for (const auto& [folder, name] : renames)
    {
        if (scripts.notNull() && findChildFolder(scripts, name).isNull())
        {
            rename_category(&gInventory, folder, name);
        }
    }
    mPrefix = prefix;
    clearResults();
    save();
    setBusy(false);
}

void FSFloaterScriptRepos::onCheck()
{
    if (mBusy || mRepos.empty())
    {
        return;
    }
    if (!gInventory.isInventoryUsable())
    {
        setStatus(getString("no_inventory"));
        return;
    }
    clearResults();
    mCancel = false;
    setBusy(true);
    LLHandle<LLFloater> handle = getHandle();
    LLCoros::instance().launch("FSFloaterScriptRepos::check", [handle]() { checkCoro(handle); });
}

void FSFloaterScriptRepos::onRegionCheck()
{
    if (mBusy || mRepos.empty())
    {
        return;
    }
    if (!gAgent.getRegion())
    {
        setStatus(getString("no_region"));
        return;
    }
    if (!gInventory.isInventoryUsable())
    {
        setStatus(getString("no_inventory"));
        return;
    }
    // Keep the scan of the last check in this region: closing it would end the 360 interest list, the region
    // would drop far objects, and a new scan would wait for all of them again
    std::unique_ptr<FSRegionObjectScanner> keep = std::move(mRegionScanner);
    clearResults();
    if (keep && keep->isOpen() && keep->getRegionID() == gAgent.getRegion()->getRegionID())
    {
        mRegionScanner = std::move(keep);
    }
    mCancel = false;
    setBusy(true);
    LLHandle<LLFloater> handle = getHandle();
    LLCoros::instance().launch("FSFloaterScriptRepos::regionCheck", [handle]() { regionCheckCoro(handle); });
}

void FSFloaterScriptRepos::onApply()
{
    const bool inventory = mMode == Mode::INVENTORY && !mInvRows.empty();
    const bool region = mMode == Mode::REGION && !mRegionRows.empty();
    if (mBusy && region && !mApplyAfterCheck)
    {
        // The region check is still reading: stop it; it starts Apply on the rows listed when it ends
        mApplyAfterCheck = true;
        onCancel();
        setBusy(true);
        return;
    }
    if (mBusy || (!inventory && !region))
    {
        setStatus(getString("nothing_listed"));
        return;
    }
    if (!gAgent.getRegion())
    {
        setStatus(getString("no_region"));
        return;
    }
    mCancel = false;
    setBusy(true);
    LLHandle<LLFloater> handle = getHandle();
    if (inventory)
    {
        LLCoros::instance().launch("FSFloaterScriptRepos::apply", [handle]() { applyInventoryCoro(handle); });
    }
    else
    {
        LLCoros::instance().launch("FSFloaterScriptRepos::applyRegion", [handle]() { applyRegionCoro(handle); });
    }
}

void FSFloaterScriptRepos::onCancel()
{
    mCancel = true;
    if (!mRegionPumpName.empty())
    {
        LLEventPumps::instance().post(mRegionPumpName, LLSDMap("cancel", LLSD::Boolean(true)));
    }
}

//
// Inventory
//

std::string FSFloaterScriptRepos::repoFolderName(size_t index) const
{
    const RepoRef& ref = mRepos[index].state.ref;
    for (size_t i = 0; i < mRepos.size(); ++i)
    {
        if (i != index && LLStringUtil::compareInsensitive(mRepos[i].state.ref.repo, ref.repo) == 0)
        {
            return mPrefix + ref.owner + "-" + ref.repo;
        }
    }
    return mPrefix + ref.repo;
}

LLUUID FSFloaterScriptRepos::findRepoFolder(size_t index) const
{
    const LLUUID scripts = gInventory.findCategoryUUIDForType(LLFolderType::FT_LSL_TEXT);
    return scripts.isNull() ? LLUUID::null : findChildFolder(scripts, repoFolderName(index));
}

bool FSFloaterScriptRepos::itemPresent(const LLUUID& item_id, const LLUUID& repo_folder) const
{
    return item_id.notNull() && repo_folder.notNull() && gInventory.getItem(item_id) &&
        gInventory.isObjectDescendentOf(item_id, repo_folder);
}

bool FSFloaterScriptRepos::fetchRepo(LLHandle<LLFloater> handle, size_t index, bool force_download, std::string& error)
{
    FSFloaterScriptRepos* floater = self(handle);
    if (!floater)
    {
        return false;
    }
    const RepoRef ref = floater->mRepos[index].state.ref;

    FetchResult commit = fetchCommit(ref);
    if (!(floater = self(handle)) || floater->mCancel)
    {
        return false;
    }
    if (!commit.ok)
    {
        error = commit.error;
        return false;
    }

    Repo& repo = floater->mRepos[index];
    if (repo.loaded && repo.fetched_commit == commit.commit)
    {
        return true;
    }
    repo.fetched_commit = commit.commit;
    repo.loaded = false;
    repo.entries.clear();

    if (!force_download && repo.state.commit == commit.commit)
    {
        const LLUUID folder = floater->findRepoFolder(index);
        bool all_present = true;
        for (const auto& file : repo.state.files)
        {
            all_present = all_present && floater->itemPresent(file.second.item_id, folder);
        }
        if (all_present)
        {
            return true;    // nothing changed since the last sync: no download
        }
    }

    FetchResult zip = fetchZipball(ref, commit.commit);
    if (!(floater = self(handle)) || floater->mCancel)
    {
        return false;
    }
    if (!zip.ok)
    {
        error = zip.error;
        return false;
    }

    std::vector<std::pair<std::string, std::string>> files;
    std::vector<std::string> unsafe;
    // shortcut: unpacked on the main thread (coroutine); fine for script repos, move to a worker if 50 MB repos stall
    if (!readZip(zip.body, files, unsafe, error))
    {
        return false;
    }

    Repo& loaded = floater->mRepos[index];
    const std::vector<std::string> ignore = splitIgnore(floater->mIgnoreEdit->getText());
    const bool images = floater->mImages->get();
    for (auto& file : files)
    {
        loaded.entries.push_back(classify(file.first, std::move(file.second), ignore, images));
    }
    for (const std::string& path : unsafe)
    {
        Entry entry;
        entry.path = path;
        entry.skip_reason = floater->getString("note_unsafe");
        loaded.entries.push_back(entry);
    }
    loaded.loaded = true;
    return true;
}

void FSFloaterScriptRepos::checkCoro(LLHandle<LLFloater> handle)
{
    LLCoros::set_consuming(true);
    FSFloaterScriptRepos* floater = self(handle);
    if (!floater)
    {
        return;
    }
    floater->mMode = Mode::INVENTORY;
    const bool lua = regionHasLua();
    const size_t count = floater->mRepos.size();

    for (size_t i = 0; i < count; ++i)
    {
        if (!(floater = self(handle)) || floater->mCancel)
        {
            break;
        }
        floater->setProgress(i, count);
        floater->setStatus(floater->getString("checking", fargs({ { "REPO", floater->mRepos[i].state.ref.display() } })));

        std::string error;
        const bool fetched = fetchRepo(handle, i, false, error);
        if (!(floater = self(handle)))
        {
            return;
        }
        if (floater->mCancel)
        {
            break;
        }
        Repo& repo = floater->mRepos[i];
        if (!fetched)
        {
            floater->addResult(floater->getString("action_error"), repo.state.ref.display(), "", error);
            continue;
        }

        LLStringUtil::format_map_t args;
        args["[REPO]"] = repo.state.ref.display();
        args["[COMMIT]"] = shortCommit(repo.fetched_commit);
        if (!repo.loaded)
        {
            floater->addResult("", floater->getString("nothing_to_do", args), "", "");
            continue;
        }

        // Count first, so the summary line comes before its items
        const LLUUID folder = floater->findRepoFolder(i);
        std::vector<std::pair<size_t, InvAction>> decided;
        std::set<std::string> paths;
        S32 counts[5] = { 0, 0, 0, 0, 0 };
        repo.held_back = false;
        for (size_t e = 0; e < repo.entries.size(); ++e)
        {
            const Entry& entry = repo.entries[e];
            paths.insert(entry.path);
            auto state = repo.state.files.find(entry.path);
            const FileState* file_state = state == repo.state.files.end() ? nullptr : &state->second;
            InvAction action = decideInventory(&entry, file_state,
                file_state && floater->itemPresent(file_state->item_id, folder));
            // Synced before compile results were recorded: save once more to learn whether it compiles
            if (action == InvAction::UNCHANGED && (entry.kind == Kind::LSL || entry.kind == Kind::LUA) &&
                file_state && !file_state->compile_checked)
            {
                action = InvAction::UPDATE;
            }
            if (entry.kind == Kind::LUA && !lua && (action == InvAction::CREATE || action == InvAction::UPDATE))
            {
                action = InvAction::SKIP;
                repo.held_back = true;
            }
            decided.emplace_back(e, action);
            ++counts[(int)action];
        }
        for (const auto& file : repo.state.files)
        {
            if (!paths.count(file.first))
            {
                ++counts[(int)InvAction::GONE];
            }
        }

        args["[CREATE]"] = llformat("%d", counts[(int)InvAction::CREATE]);
        args["[UPDATE]"] = llformat("%d", counts[(int)InvAction::UPDATE]);
        args["[UNCHANGED]"] = llformat("%d", counts[(int)InvAction::UNCHANGED]);
        args["[GONE]"] = llformat("%d", counts[(int)InvAction::GONE]);
        args["[SKIP]"] = llformat("%d", counts[(int)InvAction::SKIP]);
        floater->addResult("", floater->getString("inventory_summary", args), "", "");

        for (const auto& [e, action] : decided)
        {
            const Entry& entry = repo.entries[e];
            switch (action)
            {
            case InvAction::CREATE:
            case InvAction::UPDATE:
                floater->addResult(floater->getString(action == InvAction::CREATE ? "action_create" : "action_update"),
                    entry.path, floater->kindLabel(entry.kind), "");
                floater->mInvRows.push_back({ i, e, action });
                break;
            case InvAction::SKIP:
                floater->addResult(floater->getString("action_skip"), entry.path, floater->kindLabel(entry.kind),
                    entry.kind == Kind::SKIPPED ? entry.skip_reason : floater->getString("note_no_lua"));
                break;
            default:
                break;
            }
        }
        for (const auto& file : repo.state.files)
        {
            if (!paths.count(file.first))
            {
                floater->addResult(floater->getString("action_gone"), file.first, "", floater->getString("note_kept"));
            }
        }
    }

    if (!(floater = self(handle)))
    {
        return;
    }
    floater->setProgress(0, 0);
    if (floater->mCancel)
    {
        floater->setStatus(floater->getString("cancelled"));
        floater->mInvRows.clear();
    }
    else
    {
        std::string status = floater->getString("inventory_ready");
        S32 images = 0;
        for (const InvRow& row : floater->mInvRows)
        {
            images += floater->mRepos[row.repo].entries[row.entry].kind == Kind::IMAGE ? 1 : 0;
        }
        if (images)
        {
            // shortcut: base price per image; the grid may charge more for large ones, upgrade with per-size cost
            status += " " + floater->getString("images_cost",
                fargs({ { "COST", llformat("%d", images * LLAgentBenefitsMgr::current().getTextureUploadCost()) } }));
        }
        floater->setStatus(status);
    }
    floater->setBusy(false);
}

void FSFloaterScriptRepos::applyInventoryCoro(LLHandle<LLFloater> handle)
{
    LLCoros::set_consuming(true);
    LLEventMailDrop pump(std::string("ScriptReposApply-") + LLUUID::generateNewID().asString(), true);

    FSFloaterScriptRepos* floater = self(handle);
    if (!floater)
    {
        return;
    }
    const std::vector<InvRow> rows = floater->mInvRows;
    floater->mResults->deleteAllItems();
    std::set<size_t> failed_repos;
    std::set<size_t> touched_repos;
    S32 ok = 0;
    S32 failed = 0;
    const S32 image_cost = LLAgentBenefitsMgr::current().getTextureUploadCost();
    const LLUUID scripts = gInventory.findCategoryUUIDForType(LLFolderType::FT_LSL_TEXT);

    for (size_t r = 0; r < rows.size(); ++r)
    {
        if (!(floater = self(handle)))
        {
            return;
        }
        if (floater->mCancel)
        {
            break;
        }
        floater->setProgress(r, rows.size());
        floater->setStatus(floater->getString("applying", fargs({ { "DONE", llformat("%d", (S32)r + 1) }, { "TOTAL", llformat("%d", (S32)rows.size()) } })));

        const InvRow row = rows[r];
        touched_repos.insert(row.repo);
        // Copies: the window can go away while this suspends
        const Entry entry = floater->mRepos[row.repo].entries[row.entry];
        const std::string repo_folder_name = floater->repoFolderName(row.repo);
        FileState old_state;
        auto found = floater->mRepos[row.repo].state.files.find(entry.path);
        if (found != floater->mRepos[row.repo].state.files.end())
        {
            old_state = found->second;
        }

        std::string error;
        std::string compile_error;
        LLUUID asset_id;
        LLUUID item_id = row.action == InvAction::UPDATE ? old_state.item_id : LLUUID::null;

        // Folders: Scripts/<repo>/<path>/
        LLUUID folder = scripts.isNull() ? LLUUID::null : ensureFolder(scripts, repo_folder_name, pump);
        if (folder.notNull() && !entry.folder.empty())
        {
            size_t start = 0;
            while (folder.notNull() && start <= entry.folder.size())
            {
                size_t slash = entry.folder.find('/', start);
                if (slash == std::string::npos)
                {
                    slash = entry.folder.size();
                }
                folder = ensureFolder(folder, entry.folder.substr(start, slash - start), pump);
                start = slash + 1;
            }
        }

        bool saved = false;
        if (folder.isNull())
        {
            error = "folder";
        }
        else if (entry.kind == Kind::IMAGE)
        {
            item_id = uploadImage(folder, entry, image_cost);
            saved = item_id.notNull();
            if (saved && row.action == InvAction::UPDATE)
            {
                // The old texture goes to the Trash, never deleted
                if (LLViewerInventoryItem* old_item = gInventory.getItem(old_state.item_id))
                {
                    gInventory.changeItemParent(old_item,
                        gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH), false);
                }
            }
        }
        else
        {
            if (item_id.isNull())
            {
                item_id = createItem(folder, entry, pump);
            }
            saved = item_id.notNull() && uploadText(item_id, entry, pump, error, compile_error, asset_id);
        }

        if (!(floater = self(handle)))
        {
            return;
        }

        std::string note;
        if (saved)
        {
            ++ok;
            floater->mRepos[row.repo].state.files[entry.path] = { entry.md5, item_id, asset_id, compile_error.empty(), true };
            floater->save();
            if (!compile_error.empty())
            {
                note = floater->getString("note_not_compiled", fargs({ { "ERROR", compile_error } }));
            }
        }
        else
        {
            ++failed;
            failed_repos.insert(row.repo);
            if (item_id.notNull() && row.action == InvAction::CREATE)
            {
                // Created but empty: remember it so the next sync updates it rather than adding a second one
                floater->mRepos[row.repo].state.files[entry.path] = { std::string(), item_id, LLUUID::null, false, false };
                floater->save();
            }
            note = item_id.isNull() && entry.kind != Kind::IMAGE ? floater->getString("note_create_failed") :
                floater->getString("note_upload_failed", fargs({ { "ERROR", error } }));
        }
        floater->addResult(floater->getString(saved ? "action_done" : "action_failed"), entry.path,
            floater->kindLabel(entry.kind), note);
    }

    if (!(floater = self(handle)))
    {
        return;
    }
    const bool cancelled = floater->mCancel;
    for (size_t index : touched_repos)
    {
        Repo& repo = floater->mRepos[index];
        repo.state.synced = LLDate::now();
        // Only a complete sync records the commit: otherwise the next check must download again
        repo.state.commit = (!cancelled && !failed_repos.count(index) && !repo.held_back) ? repo.fetched_commit
                                                                                         : std::string();
    }
    floater->save();
    floater->refreshRepos();
    floater->mInvRows.clear();
    floater->mMode = Mode::NONE;
    floater->setProgress(0, 0);
    floater->setStatus(cancelled ? floater->getString("cancelled") :
        floater->getString("inventory_done", fargs({ { "OK", llformat("%d", ok) }, { "FAILED", llformat("%d", failed) } })));
    floater->setBusy(false);
}

//
// Region
//

namespace
{
    // Suspends. False when the experience could not be read; experience is null when the script has none.
    bool lookupExperience(const LLUUID& object_id, const LLUUID& item_id, LLUUID& experience)
    {
        LLViewerRegion* region = gAgent.getRegion();
        if (!region)
        {
            return false;
        }
        // Own pump: a late answer must not wake another wait
        LLEventMailDrop pump(std::string("ScriptReposExperience-") + LLUUID::generateNewID().asString(), true);
        const std::string pump_name = pump.getName();
        LLExperienceCache::instance().fetchAssociatedExperience(object_id, item_id, region->getCapability("GetMetadata"),
            [pump_name](const LLSD& result) { LLEventPumps::instance().post(pump_name, LLSDMap("result", result)); });
        LLSD answer = waitOn(pump, 15.f);
        if (answer.has("timeout"))
        {
            return false;
        }
        const LLSD& result = answer["result"];
        if (result.has(LLExperienceCache::EXPERIENCE_ID))
        {
            experience = result[LLExperienceCache::EXPERIENCE_ID].asUUID();
            return true;
        }
        // -1 = the script has no experience
        experience.setNull();
        return result.has("error") && result["error"].asInteger() == -1;
    }

    // The region tools of Develop > Region Lua Scripts must not swap scripts in the same prims meanwhile
    struct RegionToolLock
    {
        bool held{ FSRegionLuaScripts::instance().lockForWindow() };
        ~RegionToolLock()
        {
            if (held)
            {
                FSRegionLuaScripts::instance().unlockForWindow();
            }
        }
    };

    bool ensureDir(const std::string& path)
    {
        return LLFile::isdir(path) || LLFile::mkdir(path) == 0 || LLFile::isdir(path);
    }
}

void FSFloaterScriptRepos::regionCheckCoro(LLHandle<LLFloater> handle)
{
    LLCoros::set_consuming(true);
    LLEventMailDrop pump(std::string("ScriptReposRegion-") + LLUUID::generateNewID().asString(), true);

    FSFloaterScriptRepos* floater = self(handle);
    if (!floater)
    {
        return;
    }
    floater->mMode = Mode::REGION;
    floater->mRegionID = gAgent.getRegion() ? gAgent.getRegion()->getRegionID() : LLUUID::null;
    RegionToolLock lock;
    if (!lock.held)
    {
        floater->setStatus(floater->getString("region_busy"));
        floater->setBusy(false);
        return;
    }
    auto finish = [&handle](const std::string& status)
    {
        if (FSFloaterScriptRepos* f = self(handle))
        {
            f->setProgress(0, 0);
            f->setStatus(status);
            // The scan stays open while the window is (the next check starts reading at once)
            f->setBusy(false);
            if (f->mApplyAfterCheck)
            {
                // Apply was pressed while reading: the rows listed so far. Next frame, once this check has let go
                // of the region tools (Apply takes them too)
                f->mApplyAfterCheck = false;
                LLHandle<LLFloater> apply_handle = handle;
                doOnIdleOneTime([apply_handle]() mutable
                {
                    if (FSFloaterScriptRepos* floater = self(apply_handle))
                    {
                        floater->onApply();
                    }
                });
            }
        }
    };

    // 1. The repositories' text (downloaded even when the inventory is up to date)
    const size_t count = floater->mRepos.size();
    for (size_t i = 0; i < count; ++i)
    {
        floater->setStatus(floater->getString("checking", fargs({ { "REPO", floater->mRepos[i].state.ref.display() } })));
        std::string error;
        const bool fetched = fetchRepo(handle, i, true, error);
        if (!(floater = self(handle)))
        {
            return;
        }
        if (floater->mCancel)
        {
            finish(std::string());
            return;
        }
        if (!fetched || !floater->mRepos[i].loaded)
        {
            floater->addResult(floater->getString("action_error"), floater->mRepos[i].state.ref.display(), "",
                error.empty() ? floater->getString("nothing_listed") : error);
        }
    }

    std::vector<const Entry*> entries;
    std::map<const Entry*, size_t> repo_of;
    std::set<std::string> names;
    for (size_t i = 0; i < floater->mRepos.size(); ++i)
    {
        if (!floater->mRepos[i].loaded)
        {
            continue;
        }
        for (const Entry& entry : floater->mRepos[i].entries)
        {
            if (entry.kind == Kind::LSL || entry.kind == Kind::LUA)
            {
                entries.push_back(&entry);
                repo_of[&entry] = i;
                names.insert(entry.name);
            }
        }
    }

    // 2. A stream, like Mass Object Edit's target list: every object in the region (360 interest list, not only
    //    draw distance) is found by the shared scanner, every prim you may modify is read as soon as its properties
    //    are in, and each script with a repository name is checked and listed as soon as its prim's contents arrive.
    //    No guess about which prims hold scripts: the scripted flag is stale for objects loaded from the object
    //    cache, and the inventory serial is 0 for rezzed objects.
    floater->setStatus(floater->getString("region_scanning"));
    if (!floater->mRegionScanner)
    {
        floater->mRegionScanner = std::make_unique<FSRegionObjectScanner>();
    }
    FSRegionObjectScanner* scanner = floater->mRegionScanner.get();
    scanner->open(); // nothing when already open

    // A script holding the text asset a sync saved for a repository file has exactly that file's text: no download.
    // Only for names with a single repository script (no other language, nothing ambiguous), and the asset recorded
    // at sync time, not the inventory copy's current one (it may have been edited since).
    std::map<std::string, S32> name_count;
    for (const Entry* entry : entries)
    {
        ++name_count[entry->name];
    }
    std::map<LLUUID, const Entry*> synced_assets;
    for (const Entry* entry : entries)
    {
        const Repo& repo = floater->mRepos[repo_of[entry]];
        auto file = repo.state.files.find(entry->path);
        if (name_count[entry->name] == 1 && file != repo.state.files.end() && file->second.md5 == entry->md5 &&
            file->second.asset_id.notNull())
        {
            synced_assets[file->second.asset_id] = entry;
        }
    }

    struct Counts
    {
        S32 change{ 0 }, current{ 0 }, other{ 0 }, others{ 0 }, read{ 0 }, queued{ 0 };
        std::set<LLUUID> objects;
    };
    auto counts = std::make_shared<Counts>();
    auto show_progress = [handle, counts]() mutable
    {
        if (FSFloaterScriptRepos* f = self(handle))
        {
            f->setProgress(counts->read, counts->queued);
            f->setStatus(f->getString("region_progress", fargs({ { "READ", llformat("%d", counts->read) },
                { "TOTAL", llformat("%d", counts->queued) },
                { "LISTED", llformat("%d", counts->change + counts->current + counts->other) } })));
        }
    };

    // Runs inside the content workers (several at once) and suspends: re-check the window after every wait
    auto check_script = [handle, counts, &entries, &repo_of, &synced_assets](
        const FSRegionLuaScripts::ScannedPrim& prim, LLPointer<LLViewerInventoryItem> script) mutable
    {
        FSFloaterScriptRepos* floater = self(handle);
        if (!floater || floater->mCancel)
        {
            return;
        }
        // In an object you don't own, only scripts you created are yours to update
        const bool own_object = prim.owner_id == gAgent.getID();
        if (!own_object && script->getPermissions().getCreator() != gAgent.getID())
        {
            LL_INFOS("ScriptRepos") << "Region check: skipped '" << script->getName() << "' in '" << prim.root_name
                                    << "' (" << prim.id << "): not your object and the script's creator is "
                                    << script->getPermissions().getCreator() << LL_ENDL;
            return;
        }
        const std::string what = prim.root_name + (prim.root_id == prim.id ? "" : " / " + prim.name) + " / " +
            script->getName();
        auto untouched = [&](const std::string& note)
        {
            floater->addResult(floater->getString("state_untouched"), what, "", note);
            ++counts->other;
        };

        auto same = synced_assets.find(script->getAssetUUID());
        if (script->getAssetUUID().notNull() && same != synced_assets.end() &&
            same->second->name == script->getName())
        {
            floater->addResult(floater->getString("state_current"), what, floater->kindLabel(same->second->kind),
                floater->getString("note_from", fargs({ { "PATH", same->second->path } })));
            ++counts->current;
            return;
        }
        if (!gAgent.allowOperation(PERM_MODIFY, script->getPermissions(), GP_OBJECT_MANIPULATE))
        {
            untouched(floater->getString("note_no_modify"));
            return;
        }

        RegionRow row;
        row.object_id = prim.id;
        row.item_id = script->getUUID();
        row.object_name = prim.root_name;
        row.link_name = prim.name;
        row.script_name = script->getName();
        row.old_asset = script->getAssetUUID();

        LLEventMailDrop pump(std::string("ScriptReposCheck-") + LLUUID::generateNewID().asString(), true);
        LLViewerObject* object = gObjectList.findObject(prim.id);
        const bool read = object && FSRegionLuaScripts::fetchScriptText(object, script, pump, row.old_text);
        if (!(floater = self(handle)) || floater->mCancel)
        {
            return;
        }
        if (!read)
        {
            untouched(floater->getString("note_unreadable"));
            return;
        }

        object = gObjectList.findObject(prim.id);
        const bool queried = object &&
            FSRegionLuaScripts::queryScriptRunning(object, row.item_id, pump, row.running, row.target);
        if (!(floater = self(handle)) || floater->mCancel)
        {
            return;
        }
        if (!queried)
        {
            untouched(floater->getString("note_running_unknown"));
            return;
        }
        row.lua = row.target == "luau";

        const Entry* source = nullptr;
        switch (decideRegion(row.script_name, row.old_text, row.lua, entries, source))
        {
        case RegionAction::CURRENT:
            floater->addResult(floater->getString("state_current"), what, floater->kindLabel(source->kind),
                floater->getString("note_from", fargs({ { "PATH", source->path } })));
            ++counts->current;
            break;
        case RegionAction::LANGUAGE_DIFFERS:
            untouched(floater->getString("note_language",
                fargs({ { "OTHER", floater->getString(row.lua ? "language_lsl" : "language_lua") } })));
            break;
        case RegionAction::AMBIGUOUS:
            untouched(floater->getString("note_ambiguous"));
            break;
        case RegionAction::WOULD_CHANGE:
        {
            const Repo& repo = floater->mRepos[repo_of[source]];
            auto file = repo.state.files.find(source->path);
            if (file == repo.state.files.end() || file->second.md5 != source->md5 ||
                !gInventory.getItem(file->second.item_id))
            {
                untouched(floater->getString("note_sync_first", fargs({ { "PATH", source->path } })));
                break;
            }
            // There the original goes before the copy compiles: only a copy known to compile
            if (!own_object && !file->second.compiled)
            {
                untouched(floater->getString("note_others_not_compiled"));
                break;
            }
            row.source_item = file->second.item_id;
            row.owner_id = prim.owner_id;
            const std::string source_path = source->path;
            const std::string kind = floater->kindLabel(source->kind);
            const bool known = lookupExperience(row.object_id, row.item_id, row.experience);
            if (!(floater = self(handle)) || floater->mCancel)
            {
                return;
            }
            if (!known)
            {
                untouched(floater->getString("note_experience_unknown"));
                break;
            }
            floater->addResult(floater->getString("state_change"), what, kind,
                floater->getString(own_object ? "note_from" : "note_from_others", fargs({ { "PATH", source_path } })));
            floater->mRegionRows.push_back(row);
            floater->setBusy(true); // Apply becomes available
            counts->objects.insert(prim.root_id);
            ++counts->change;
            counts->others += own_object ? 0 : 1;
            break;
        }
        default:
            break;
        }
    };

    // The scanner keeps adding objects as the region sends them; each pass reads what came in since the last one.
    // It keeps watching for new objects until Cancel, Apply, the window closing or a region change: the region can
    // send objects at any time, so "quiet for a while" is never proof that all are in.
    std::set<LLUUID> seen;
    std::map<LLUUID, FSRegionLuaScripts::ScannedPrim> prims;

    bool region_changed = false;
    while (true)
    {
        if (!(floater = self(handle)) || !(scanner = floater->mRegionScanner.get()))
        {
            return;
        }
        if (floater->mCancel)
        {
            break;
        }
        if (!gAgent.getRegion() || gAgent.getRegion()->getRegionID() != floater->mRegionID ||
            (scanner->getRegionID().notNull() && scanner->getRegionID() != floater->mRegionID))
        {
            region_changed = true;
            break;
        }

        uuid_vec_t batch;
        S32 no_properties = 0, gone = 0, no_modify = 0;
        for (const auto& [id, info] : scanner->objects())
        {
            if (seen.count(id))
            {
                continue;
            }
            LLViewerObject* object = gObjectList.findObject(id);
            // Yours, or someone else's (a friend's with edit rights, or group-deeded) that you may modify. Judged
            // from the permissions in the object properties the simulator just sent, as Mass Object Edit does: the
            // object's own flags (permModify) are stale for objects loaded from the object cache.
            if (!info.received || !object || object->isAttachment() ||
                !gAgent.allowOperation(PERM_MODIFY, info.permissions, GP_OBJECT_MANIPULATE))
            {
                no_properties += info.received ? 0 : 1;
                gone += info.received && (!object || object->isAttachment()) ? 1 : 0;
                no_modify += info.received && object && !object->isAttachment() ? 1 : 0;
                continue;
            }
            seen.insert(id);
            FSRegionLuaScripts::ScannedPrim prim;
            prim.id = id;
            prim.root_id = info.root_id;
            prim.owner_id = info.owner_id;
            prim.name = info.name;
            auto root = scanner->objects().find(info.root_id);
            prim.root_name = root != scanner->objects().end() && root->second.received ? root->second.name : info.name;
            prim.permissions = info.permissions;
            prim.scripted = true;
            prims[id] = prim;
            batch.push_back(id);
        }

        if (batch.empty())
        {
            show_progress();
            llcoro::suspendUntilTimeout(0.5f);
            continue;
        }

        LL_INFOS("ScriptRepos") << "Region check: " << scanner->objects().size() << " objects; not read: "
                                << no_properties << " without properties yet, " << gone << " gone or attached, "
                                << no_modify << " you may not modify" << LL_ENDL;
        counts->queued += static_cast<S32>(batch.size());
        const S32 read_before = counts->read;
        show_progress();
        const FSRegionObjectScanner::ContentsResult contents = FSRegionObjectScanner::fetchContents(batch, false,
            [&prims, &names, check_script](const LLUUID& id, const LLInventoryObject::object_list_t& inventory) mutable
            {
                const FSRegionLuaScripts::ScannedPrim prim = prims[id];
                std::vector<LLPointer<LLViewerInventoryItem>> scripts;
                for (const LLPointer<LLInventoryObject>& inventory_object : inventory)
                {
                    LLViewerInventoryItem* item = dynamic_cast<LLViewerInventoryItem*>(inventory_object.get());
                    if (item && item->getType() == LLAssetType::AT_LSL_TEXT && names.count(item->getName()))
                    {
                        scripts.push_back(item);
                    }
                }
                for (const LLPointer<LLViewerInventoryItem>& script : scripts)
                {
                    check_script(prim, script);
                }
            },
            [counts, read_before, show_progress](S32 processed, S32, S32) mutable
            {
                counts->read = read_before + processed;
                show_progress();
            },
            nullptr,
            [handle]() mutable
            {
                FSFloaterScriptRepos* f = self(handle);
                return !f || f->mCancel;
            },
            6); // a whole region's prims: more readers than Mass Object Edit's default 3
        counts->read = read_before + contents.total;

        LL_INFOS("ScriptRepos") << "Region check pass: " << batch.size() << " prims, " << contents.failed
                                << " did not answer; " << counts->read << " of " << counts->queued << " read, "
                                << (counts->change + counts->current + counts->other) << " scripts listed" << LL_ENDL;
        for (size_t k = 0; k < contents.failed_ids.size() && k < 20; ++k)
        {
            LL_INFOS("ScriptRepos") << "Region check: no contents from " << contents.failed_ids[k] << " '"
                                    << prims[contents.failed_ids[k]].name << "'" << LL_ENDL;
        }
    }

    if (!(floater = self(handle)))
    {
        return;
    }
    if (region_changed)
    {
        floater->mRegionRows.clear();
        finish(floater->getString("region_changed"));
        return;
    }
    if (counts->change + counts->current + counts->other == 0)
    {
        finish(floater->getString(floater->mCancel ? "cancelled" : "region_none"));
        return;
    }
    finish((floater->mCancel ? floater->getString("region_stopped") + " " : std::string()) +
        floater->getString("region_summary", fargs({ { "CHANGE", llformat("%d", counts->change) },
            { "CURRENT", llformat("%d", counts->current) }, { "OTHER", llformat("%d", counts->other) },
            { "OBJECTS", llformat("%d", (S32)counts->objects.size()) } })) + " " + floater->getString("region_ready") +
        (counts->others ? " " + floater->getString("region_others",
            fargs({ { "COUNT", llformat("%d", counts->others) } })) : ""));
}
void FSFloaterScriptRepos::applyRegionCoro(LLHandle<LLFloater> handle)
{
    LLCoros::set_consuming(true);
    LLEventMailDrop pump(std::string("ScriptReposRegionApply-") + LLUUID::generateNewID().asString(), true);

    FSFloaterScriptRepos* floater = self(handle);
    if (!floater)
    {
        return;
    }
    RegionToolLock lock;
    if (!lock.held)
    {
        floater->mRegionRows.clear();
        floater->mRegionScanner.reset();
        floater->mMode = Mode::NONE;
        floater->setStatus(floater->getString("region_busy"));
        floater->setBusy(false);
        return;
    }
    const std::vector<RegionRow> rows = floater->mRegionRows;
    const LLUUID region_id = floater->mRegionID;
    floater->mResults->deleteAllItems();

    const std::string root = gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "script_repos");
    const std::string backups = root + gDirUtilp->getDirDelimiter() + "backups";
    const std::string run = backups + gDirUtilp->getDirDelimiter() + LLDate::now().toHTTPDateString("%Y-%m-%d_%H%M%S");
    const bool have_run_dir = ensureDir(root) && ensureDir(backups) && ensureDir(run);

    S32 ok = 0;
    S32 failed = 0;
    bool region_changed = false;
    for (size_t r = 0; r < rows.size(); ++r)
    {
        if (!(floater = self(handle)))
        {
            return;
        }
        if (floater->mCancel)
        {
            break;
        }
        if (!gAgent.getRegion() || gAgent.getRegion()->getRegionID() != region_id)
        {
            region_changed = true;
            break;
        }
        floater->setProgress(r, rows.size());
        floater->setStatus(floater->getString("applying", fargs({ { "DONE", llformat("%d", (S32)r + 1) },
            { "TOTAL", llformat("%d", (S32)rows.size()) } })));

        const RegionRow& row = rows[r];
        const std::string what = row.object_name + (row.link_name == row.object_name ? "" : " / " + row.link_name) +
            " / " + row.script_name;

        // The script as it is now: changed since the check means the user did not see this swap
        LLViewerObject* object = gObjectList.findObject(row.object_id);
        LLInventoryObject::object_list_t inventory;
        const bool listed = object && FSRegionLuaScripts::fetchPrimInventory(object, pump, inventory);
        if (!(floater = self(handle)))
        {
            return;
        }
        LLPointer<LLViewerInventoryItem> old_script;
        for (const LLPointer<LLInventoryObject>& inventory_object : inventory)
        {
            if (inventory_object->getUUID() == row.item_id)
            {
                old_script = dynamic_cast<LLViewerInventoryItem*>(inventory_object.get());
            }
        }
        if (!listed || !old_script)
        {
            ++failed;
            LL_INFOS("ScriptRepos") << "Apply: " << what << ": " << (listed ? "script gone" : "contents not read")
                                    << LL_ENDL;
            floater->addResult(floater->getString("action_failed"), what, "",
                floater->getString(listed ? "note_script_gone" : "note_contents_unread"));
            continue;
        }
        // Unchanged since the check: same text asset (any save makes a new one), else the text itself
        bool unchanged = row.old_asset.notNull() && old_script->getAssetUUID() == row.old_asset;
        if (!unchanged)
        {
            std::string current_text;
            object = gObjectList.findObject(row.object_id);
            const bool read = object && FSRegionLuaScripts::fetchScriptText(object, old_script, pump, current_text);
            if (!(floater = self(handle)))
            {
                return;
            }
            if (!read)
            {
                ++failed;
                LL_INFOS("ScriptRepos") << "Apply: " << what << ": text not read" << LL_ENDL;
                floater->addResult(floater->getString("action_failed"), what, "", floater->getString("note_unreadable"));
                continue;
            }
            unchanged = sameText(current_text, row.old_text);
        }
        if (!unchanged)
        {
            ++failed;
            floater->addResult(floater->getString("action_failed"), what, "", floater->getString("note_changed"));
            continue;
        }

        // The old text first: no backup, no swap. The object's ID keeps copies with the same names apart.
        const std::string dir_object = run + gDirUtilp->getDirDelimiter() +
            LLDir::getScrubbedFileName(row.object_name + " " + row.object_id.asString());
        const std::string dir_link = dir_object + gDirUtilp->getDirDelimiter() + LLDir::getScrubbedFileName(row.link_name);
        const std::string backup = dir_link + gDirUtilp->getDirDelimiter() +
            LLDir::getScrubbedFileName(row.script_name) + (row.lua ? ".luau" : ".lsl");
        std::string backup_problem;
        if (!have_run_dir || !ensureDir(dir_object) || !ensureDir(dir_link))
        {
            backup_problem = "folder";
        }
        else if (LLFile::isfile(backup))
        {
            backup_problem = "exists";
        }
        else
        {
            llofstream out(backup.c_str(), std::ios::binary);
            out.write(row.old_text.data(), row.old_text.size());
            if (!out.good())
            {
                backup_problem = "write";
            }
        }
        if (!backup_problem.empty())
        {
            ++failed;
            LL_INFOS("ScriptRepos") << "Apply: " << what << ": backup " << backup_problem << " (" << backup << ")"
                                    << LL_ENDL;
            floater->addResult(floater->getString("action_failed"), what, "",
                floater->getString(backup_problem == "exists" ? "note_backup_exists" : "note_backup_failed"));
            continue;
        }

        LLViewerInventoryItem* source = gInventory.getItem(row.source_item);
        object = gObjectList.findObject(row.object_id);

        bool swapped = false;
        bool original_removed = false;
        std::string error;
        if (!listed || !old_script || !source || !object)
        {
            error = !source ? "the synced inventory copy is gone" : "the script or object is gone";
        }
        else
        {
            FSRegionLuaScripts::ScannedPrim prim;
            prim.id = row.object_id;
            prim.owner_id = row.owner_id; // not you: the delete-first swap (no rename right there)
            prim.name = row.link_name;
            prim.root_name = row.object_name;
            swapped = FSRegionLuaScripts::substituteScript(object, prim, old_script, source,
                row.lua ? std::string("luau") : row.target, std::string(), row.running, row.experience, pump,
                original_removed, error);
        }
        if (!(floater = self(handle)))
        {
            return;
        }
        if (swapped)
        {
            ++ok;
            floater->addResult(floater->getString("action_done"), what, "", "");
        }
        else
        {
            ++failed;
            floater->addResult(floater->getString("action_failed"), what, "",
                floater->getString(original_removed ? "note_removed_original" : "note_kept_original",
                    fargs({ { "ERROR", error } })));
        }
    }

    if (!(floater = self(handle)))
    {
        return;
    }
    floater->mRegionRows.clear();
    floater->mMode = Mode::NONE;
    floater->setProgress(0, 0);
    floater->setStatus(floater->mCancel ? floater->getString("cancelled") :
        region_changed ? floater->getString("region_changed") :
        floater->getString("region_done", fargs({ { "OK", llformat("%d", ok) }, { "FAILED", llformat("%d", failed) },
            { "FOLDER", run } })));
    floater->setBusy(false);
}