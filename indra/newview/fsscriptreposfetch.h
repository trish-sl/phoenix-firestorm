/**
 * @file fsscriptreposfetch.h
 * @brief Script repositories: GitHub requests and the GitHub token.
 */

#ifndef FS_SCRIPTREPOSFETCH_H
#define FS_SCRIPTREPOSFETCH_H

#include "fsscriptrepos.h"

#include <string>

namespace FSScriptRepos
{
    // The token lives in the viewer's protected data store, never in settings or logs.
    std::string loadToken();
    void saveToken(const std::string& token);
    void forgetToken();

    struct FetchResult
    {
        bool ok{ false };
        std::string commit;
        std::string body;
        std::string error;
    };

    // Both must be called from inside a coroutine.
    // The commit the ref (or the default branch) points to.
    FetchResult fetchCommit(const RepoRef& ref);
    // The zipball of that commit; body holds the zip bytes.
    FetchResult fetchZipball(const RepoRef& ref, const std::string& commit);
}

#endif // FS_SCRIPTREPOSFETCH_H
