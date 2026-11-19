/**
 * @file fsfloaterscriptrepos.h
 * @brief Script Repositories window: syncs GitHub repositories into the Scripts folder and updates same-named
 *        scripts in the user's objects in the region.
 */

#ifndef FS_FLOATERSCRIPTREPOS_H
#define FS_FLOATERSCRIPTREPOS_H

#include "fsregionobjectscanner.h"
#include "fsscriptrepos.h"
#include "llfloater.h"

class LLButton;
class LLCheckBoxCtrl;
class LLEventPump;
class LLLineEditor;
class LLProgressBar;
class LLScrollListCtrl;
class LLTextBox;

// <FS:Trish> Every write is listed first: "Check" only reads, "Apply" acts on the list shown.
class FSFloaterScriptRepos : public LLFloater
{
public:
    FSFloaterScriptRepos(const LLSD& key);

    bool postBuild() override;
    void onClose(bool app_quitting) override;

private:
    struct Repo
    {
        FSScriptRepos::RepoState state;
        std::string fetched_commit;
        std::vector<FSScriptRepos::Entry> entries;
        bool loaded{ false };       // entries hold fetched_commit's files
        bool held_back{ false };    // the last check left files out (no Lua here): don't record the commit
    };

    enum class Mode { NONE, INVENTORY, REGION };

    struct InvRow
    {
        size_t repo{ 0 };
        size_t entry{ 0 };
        FSScriptRepos::InvAction action{ FSScriptRepos::InvAction::SKIP };
    };

    // A script in an object that would get a repository script's text
    struct RegionRow
    {
        LLUUID object_id;
        LLUUID item_id;
        LLUUID source_item;     // the synced inventory copy
        LLUUID experience;
        LLUUID owner_id;        // not you: the original is deleted before the copy is added
        std::string object_name;
        std::string link_name;
        std::string script_name;
        std::string old_text;
        LLUUID old_asset;       // the text asset the check read; any save changes it
        std::string target;     // the script's compile target, kept
        bool running{ false };
        bool lua{ false };
    };

    // UI
    void onAdd();
    void onRemove();
    void onForget();
    void onTokenCommit();
    void onSettingsChanged();
    void onPrefixChanged();
    void onCheck();
    void onRegionCheck();
    void onApply();
    void onCancel();
    void refreshRepos();
    void refreshToken();
    void setBusy(bool busy);
    void setStatus(const std::string& text);
    void setProgress(size_t done, size_t total);
    void addResult(const std::string& action, const std::string& what, const std::string& kind,
        const std::string& note, const LLSD& value = LLSD());
    void clearResults();

    // State file
    void load();
    void save();

    // Coroutines (static, so a closed window is noticed through the handle)
    static void checkCoro(LLHandle<LLFloater> handle);
    static void applyInventoryCoro(LLHandle<LLFloater> handle);
    static void regionCheckCoro(LLHandle<LLFloater> handle);
    static void applyRegionCoro(LLHandle<LLFloater> handle);

    // Helpers used by the coroutines. fetchRepo suspends: the window may be gone when it returns false.
    static bool fetchRepo(LLHandle<LLFloater> handle, size_t index, bool force_download, std::string& error);
    static FSFloaterScriptRepos* self(LLHandle<LLFloater>& handle);
    std::string repoFolderName(size_t index) const;
    LLUUID findRepoFolder(size_t index) const;
    bool itemPresent(const LLUUID& item_id, const LLUUID& repo_folder) const;
    std::string kindLabel(FSScriptRepos::Kind kind) const;

    LLScrollListCtrl* mRepoList{ nullptr };
    LLScrollListCtrl* mResults{ nullptr };
    LLLineEditor* mRepoEdit{ nullptr };
    LLLineEditor* mBranchEdit{ nullptr };
    LLLineEditor* mIgnoreEdit{ nullptr };
    LLLineEditor* mPrefixEdit{ nullptr };
    LLLineEditor* mTokenEdit{ nullptr };
    LLCheckBoxCtrl* mImages{ nullptr };
    LLTextBox* mStatus{ nullptr };
    LLProgressBar* mProgress{ nullptr };

    std::vector<Repo> mRepos;
    std::vector<InvRow> mInvRows;
    std::vector<RegionRow> mRegionRows;
    LLUUID mRegionID;               // region of the last region check
    // Holds the 360 interest list from the region check until Apply is done, so far objects stay known
    std::unique_ptr<FSRegionObjectScanner> mRegionScanner;
    std::string mRegionPumpName;    // the region check's wait while it scans, for Cancel
    Mode mMode{ Mode::NONE };
    bool mBusy{ false };
    bool mCancel{ false };
    bool mApplyAfterCheck{ false }; // Apply pressed while the region check was still reading
    std::string mPrefix;            // put in front of every repository folder name
};
// </FS:Trish>

#endif // FS_FLOATERSCRIPTREPOS_H
