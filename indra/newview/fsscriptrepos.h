/**
 * @file fsscriptrepos.h
 * @brief Script repositories: repository references (GitHub, GitLab, Gitea/Forgejo, Bitbucket, zip links, local
 *        folders), zip and folder reading, file rules and sync decisions.
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
    constexpr size_t MAX_ZIP_BYTES = 52428800;  // 50 MB per archive (downloaded or unpacked) or folder
    constexpr size_t MAX_ZIP_ENTRIES = 10000;   // files per archive or folder
    constexpr size_t MAX_TEXT_BYTES = 65536;    // notecard text (scripts have no limit here)
    constexpr size_t MAX_NAME_BYTES = 63;       // inventory names (DB_INV_ITEM_NAME_STR_LEN)
    const std::string DEFAULT_IGNORE("*.test.*");

    // Where a repository lives. ZIP: a plain https link to a zip file, no API and no commits. LOCAL: a folder on
    // this computer, read from disk, no download.
    enum class Provider { GITHUB, GITLAB, GITEA, BITBUCKET, ZIP, LOCAL };
    std::string providerName(Provider provider);    // stable, for the state file
    bool providerFromName(const std::string& name, Provider& out);

    struct RepoRef
    {
        Provider provider{ Provider::GITHUB };
        std::string host;   // lower case, may end in ":port"; github.com, gitlab.com, codeberg.org, bitbucket.org, ...
        std::string owner;  // GitLab: the whole namespace ("group/subgroup"); ZIP: the host; LOCAL: the parent folder's name
        std::string repo;   // ZIP: the file name without .zip; LOCAL: the folder's name
        std::string ref;    // branch or tag, empty = default branch (not used for ZIP and LOCAL)
        std::string url;    // ZIP only: the https link
        std::string path;   // LOCAL only: the folder's absolute path, normalized, in the platform's form
        // "owner/repo" on github.com; "host/owner/repo" elsewhere; the link (without its query) for ZIP; the path
        // for LOCAL
        std::string display() const;
        // Same repository, same branch: same key. A folder path ignores case where the file system does (Windows,
        // macOS).
        std::string key() const;
    };

    // Accepted forms, a non-empty branch winning over the link's ref:
    //   owner/repo                                    GitHub
    //   https://github.com/owner/repo[.git][/tree/<ref>]
    //   https://gitlab.com/group[/subgroup...]/repo[.git][/-/tree/<ref>]
    //   https://codeberg.org/owner/repo[.git][/src/branch/<ref> | /src/tag/<ref>]   (Gitea, Forgejo)
    //   https://bitbucket.org/workspace/repo[.git][/src/<ref>]
    //   https://host/.../file.zip                     any host: a zip link
    //   C:/path/to/folder, /path/to/folder          a folder on this computer (an absolute path, either slash)
    // The kind is told by the host (gitlab.com, codeberg.org, gitea.com, bitbucket.org, github.com; a host with
    // "gitlab", "gitea" or "forgejo" in its name), by a link ending in .zip, or by an absolute path. A self-hosted
    // instance with any other name needs forced (the window's kind choice); without it the text is refused.
    bool parseRepoRef(const std::string& text, const std::string& branch, RepoRef& out);
    bool parseRepoRef(const std::string& text, const std::string& branch, const Provider* forced, RepoRef& out);
    // An absolute path of a folder on this computer (not checked to exist): normalized, trailing separators dropped.
    // A drive or file system root is refused (it has no name).
    bool parseLocalFolder(const std::string& text, RepoRef& out);
    // A host name as typed in a link: letters, digits, '-', '.', and an optional ":port"
    bool isValidHost(const std::string& host);
    // A name git accepts for a branch or tag (no "..", spaces, control characters, ~^:?*[\ or "@{").
    bool isValidRef(const std::string& ref);

    // The name the inventory keeps for an item or folder called name (as LLInventoryObject::correctInventoryName:
    // characters outside printable ASCII and '|' become spaces, trimmed, at most MAX_NAME_BYTES).
    std::string inWorldName(const std::string& name);

    enum class Kind { LSL, LUA, NOTECARD, IMAGE, SKIPPED };

    struct Entry
    {
        std::string path;           // repo-relative, '/' separated
        std::string folder;         // in-world folder path under the repository folder, '/' separated, "" at the root
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

    // Reads a zip held in memory. When every file sits in one top folder (as in the archives GitHub, GitLab, Gitea
    // and Bitbucket make) that folder is left out of the paths. Unsafe paths go to unsafe_paths, not files. False
    // (with error) on a bad zip or over MAX_ZIP_BYTES / MAX_ZIP_ENTRIES.
    bool readZip(const std::string& buffer, std::vector<std::pair<std::string, std::string>>& files,
        std::vector<std::string>& unsafe_paths, std::string& error);
    // Reads a folder on disk, every file below it, paths folder-relative and '/' separated, sorted. Hidden folders
    // (".git" and the like) are not entered, hidden files are listed unread (classify skips them), symbolic links and
    // junctions are never followed. Unsafe paths go to unsafe_paths, not files. False (with error) when the folder
    // is missing or unreadable, or over MAX_ZIP_BYTES / MAX_ZIP_ENTRIES. Blocks: call it off the main thread.
    bool readFolder(const std::string& dir, std::vector<std::pair<std::string, std::string>>& files,
        std::vector<std::string>& unsafe_paths, std::string& error);
    // The version of a set of files ("md5:..."): the same files with the same text give the same version
    std::string filesVersion(const std::vector<std::pair<std::string, std::string>>& files);

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
        bool enabled{ true };   // checked in the list: checks and syncs include it
        // Next owner permissions (PERM_MODIFY, PERM_COPY, PERM_TRANSFER, PERM_MOVE) for every item synced from it.
        // 0: not chosen, each item gets the viewer's default permissions for its kind and is left as it is
        U32 next_owner{ 0 };
    };

    LLSD toLLSD(const std::vector<RepoState>& repos);
    std::vector<RepoState> fromLLSD(const LLSD& sd);

    // PERMS: the text is current but the item's next owner permissions are not the repository's (decided by the
    // window, which can see the item; decideInventory never returns it)
    enum class InvAction { CREATE, UPDATE, UNCHANGED, GONE, SKIP, PERMS };
    // entry == nullptr: the file is no longer in the repo.
    InvAction decideInventory(const Entry* entry, const FileState* state, bool item_present);

    enum class RegionAction { CURRENT, WOULD_CHANGE, LANGUAGE_DIFFERS, AMBIGUOUS, NOT_IN_REPO };
    // entries: the repo's scripts. source is set for CURRENT and WOULD_CHANGE.
    RegionAction decideRegion(const std::string& script_name, const std::string& old_text, bool old_is_lua,
        const std::vector<const Entry*>& entries, const Entry*& source);
}

#endif // FS_SCRIPTREPOS_H
