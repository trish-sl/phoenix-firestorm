/**
 * @file fsscriptrepos.h
 * @brief Script repositories: repo references, zip reading, file rules and sync decisions.
 *
 * Pure logic (llcommon and minizip-ng only) so it can be unit tested.
 */

#ifndef FS_SCRIPTREPOS_H
#define FS_SCRIPTREPOS_H

#include "lldate.h"
#include "llsd.h"
#include "lluuid.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace FSScriptRepos
{
    constexpr size_t MAX_ZIP_BYTES = 52428800;  // 50 MB per zipball
    constexpr size_t MAX_ZIP_ENTRIES = 10000;
    constexpr size_t MAX_TEXT_BYTES = 65536;    // script and notecard text
    const std::string DEFAULT_IGNORE("*.test.*");

    struct RepoRef
    {
        std::string owner;
        std::string repo;
        std::string ref;    // branch or tag, empty = default branch
        std::string display() const { return owner + "/" + repo; }
    };

    // "owner/repo" or "https://github.com/owner/repo[.git][/tree/<ref>]". A non-empty branch wins over the URL's ref.
    bool parseRepoRef(const std::string& text, const std::string& branch, RepoRef& out);

    enum class Kind { LSL, LUA, NOTECARD, IMAGE, SKIPPED };

    struct Entry
    {
        std::string path;           // repo-relative, '/' separated
        std::string folder;         // repo-relative directory, "" at the root
        std::string name;           // in-world name
        std::string data;
        std::string md5;
        Kind kind{ Kind::SKIPPED };
        std::string skip_reason;
    };

    std::vector<std::string> splitIgnore(const std::string& csv);
    // '*' matches any run of characters, '/' included.
    bool globMatch(const std::string& pattern, const std::string& path);
    bool isSafeZipPath(const std::string& path);
    Entry classify(const std::string& path, std::string data, const std::vector<std::string>& ignore, bool images);
    // Line endings (CRLF, CR, LF) and trailing newlines ignored.
    bool sameText(const std::string& a, const std::string& b);
    std::string md5Hex(const std::string& data);

    // Reads a GitHub zipball held in memory. Strips the single top folder GitHub adds. Unsafe paths go to
    // unsafe_paths, not files. False (with error) on a bad zip or over MAX_ZIP_BYTES / MAX_ZIP_ENTRIES.
    bool readZip(const std::string& buffer, std::vector<std::pair<std::string, std::string>>& files,
        std::vector<std::string>& unsafe_paths, std::string& error);

    struct FileState
    {
        std::string md5;
        LLUUID item_id;
        LLUUID asset_id;    // the text asset the sync saved; a script in a prim holding it has exactly this text
        bool compiled{ false };        // the synced script compiled; required to update objects you don't own
        bool compile_checked{ false }; // compiled is known (state from before it was recorded: unknown)
    };

    struct RepoState
    {
        RepoRef ref;
        std::string commit;
        LLDate synced;
        std::map<std::string, FileState> files;   // by repo path
    };

    LLSD toLLSD(const std::vector<RepoState>& repos);
    std::vector<RepoState> fromLLSD(const LLSD& sd);

    enum class InvAction { CREATE, UPDATE, UNCHANGED, GONE, SKIP };
    // entry == nullptr: the file is no longer in the repo.
    InvAction decideInventory(const Entry* entry, const FileState* state, bool item_present);

    enum class RegionAction { CURRENT, WOULD_CHANGE, LANGUAGE_DIFFERS, AMBIGUOUS, NOT_IN_REPO };
    // entries: the repo's scripts. source is set for CURRENT and WOULD_CHANGE.
    RegionAction decideRegion(const std::string& script_name, const std::string& old_text, bool old_is_lua,
        const std::vector<const Entry*>& entries, const Entry*& source);
}

#endif // FS_SCRIPTREPOS_H
