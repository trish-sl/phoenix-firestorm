/**
 * @file fsscriptrepos.cpp
 * @brief Script repositories: repository references (GitHub, GitLab, Gitea/Forgejo, Bitbucket, zip links, local
 *        folders), zip and folder reading, file rules and sync decisions.
 */

#include "llviewerprecompiledheaders.h"

#include "fsscriptrepos.h"

#include "fsyspath.h"
#include "llmd5.h"
#include "llstring.h"

#include "minizip-ng/mz.h"
#include "minizip-ng/mz_strm.h"
#include "minizip-ng/mz_zip.h"
#include "minizip-ng/mz_zip_rw.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace FSScriptRepos
{
namespace
{
    bool validName(const std::string& s)
    {
        if (s.empty() || s == "." || s == "..")
        {
            return false;
        }
        for (char c : s)
        {
            if (!isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.')
            {
                return false;
            }
        }
        return true;
    }

    std::vector<std::string> splitPath(const std::string& path)
    {
        std::vector<std::string> parts;
        size_t start = 0;
        while (start <= path.size())
        {
            size_t slash = path.find('/', start);
            if (slash == std::string::npos)
            {
                slash = path.size();
            }
            parts.push_back(path.substr(start, slash - start));
            start = slash + 1;
        }
        return parts;
    }

    bool validUtf8(const std::string& s)
    {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
        const size_t n = s.size();
        for (size_t i = 0; i < n;)
        {
            const unsigned char c = p[i];
            size_t extra = 0;
            if (c < 0x80)
            {
                ++i;
                continue;
            }
            else if (c >= 0xc2 && c <= 0xdf)
            {
                extra = 1;
            }
            else if (c >= 0xe0 && c <= 0xef)
            {
                extra = 2;
            }
            else if (c >= 0xf0 && c <= 0xf4)
            {
                extra = 3;
            }
            else
            {
                return false;
            }
            for (size_t k = 1; k <= extra; ++k)
            {
                if (i + k >= n || (p[i + k] & 0xc0) != 0x80)
                {
                    return false;
                }
            }
            i += extra + 1;
        }
        return true;
    }

    std::string normalized(const std::string& s)
    {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '\r')
            {
                out += '\n';
                if (i + 1 < s.size() && s[i + 1] == '\n')
                {
                    ++i;
                }
            }
            else
            {
                out += s[i];
            }
        }
        while (!out.empty() && out.back() == '\n')
        {
            out.pop_back();
        }
        return out;
    }

    bool endsWith(const std::string& s, const std::string& end)
    {
        return s.size() >= end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0;
    }

    bool startsWith(const std::string& s, const std::string& start)
    {
        return s.compare(0, start.size(), start) == 0;
    }
}

std::string providerName(Provider provider)
{
    switch (provider)
    {
    case Provider::GITLAB: return "gitlab";
    case Provider::GITEA: return "gitea";
    case Provider::BITBUCKET: return "bitbucket";
    case Provider::ZIP: return "zip";
    case Provider::LOCAL: return "local";
    default: return "github";
    }
}

bool providerFromName(const std::string& name, Provider& out)
{
    for (Provider provider : { Provider::GITHUB, Provider::GITLAB, Provider::GITEA, Provider::BITBUCKET, Provider::ZIP,
                               Provider::LOCAL })
    {
        if (providerName(provider) == name)
        {
            out = provider;
            return true;
        }
    }
    return false;
}

std::string RepoRef::display() const
{
    if (provider == Provider::ZIP)
    {
        return url.substr(0, url.find_first_of("?#"));
    }
    if (provider == Provider::LOCAL)
    {
        return path;
    }
    return (host == "github.com" ? std::string() : host + "/") + owner + "/" + repo;
}

std::string RepoRef::key() const
{
    if (provider == Provider::LOCAL)
    {
        std::string out = path;
#if LL_WINDOWS || LL_DARWIN
        LLStringUtil::toLower(out);     // these file systems ignore case
#endif
        return providerName(provider) + " " + out;
    }
    std::string out = providerName(provider) + " " + (provider == Provider::ZIP ? url : host + "/" + owner + "/" + repo);
    LLStringUtil::toLower(out);     // hosts and (on every one of these hosts) owner and repository names ignore case
    return out + " " + ref;
}

// Text meant as a path on this computer: a drive ("C:\" or "C:/"), a UNC share ("\\server\share") or a root ("/")
static bool isAbsolutePathText(const std::string& s)
{
    if (s.size() >= 3 && isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':' && (s[2] == '\\' || s[2] == '/'))
    {
        return true;
    }
    if (s.size() >= 2 && s[0] == '\\' && s[1] == '\\')
    {
        return true;
    }
    return !s.empty() && s[0] == '/';
}

bool parseLocalFolder(const std::string& text, RepoRef& out)
{
    std::string s = text;
    LLStringUtil::trim(s);
    if (s.empty() || s.size() > 1000 || !isAbsolutePathText(s))
    {
        return false;
    }
    for (char c : s)
    {
        if (static_cast<unsigned char>(c) < 0x20)
        {
            return false;
        }
    }

    // One form for the same folder however it was typed ("C:/a/../b/", "C:\b"): the key and the display agree
    std::filesystem::path normalized = fsyspath(s).lexically_normal();
    normalized.make_preferred();
    if (normalized.filename().empty())
    {
        normalized = normalized.parent_path();  // a trailing separator
    }
    if (!normalized.is_absolute() || normalized.filename().empty() || normalized.filename() == ".." ||
        normalized.filename() == ".")
    {
        return false;   // relative to something, or a root
    }
    const std::string name = fsyspath(normalized.filename()).string();
    const std::filesystem::path parent = normalized.parent_path();
    const std::string parent_name = parent.filename().empty() ? fsyspath(parent.root_name()).string() :
                                                                fsyspath(parent.filename()).string();

    out.provider = Provider::LOCAL;
    out.host.clear();
    out.owner = parent_name.empty() ? std::string("root") : parent_name;
    out.repo = name;
    out.ref.clear();
    out.url.clear();
    out.path = fsyspath(normalized).string();
    return true;
}

bool isValidHost(const std::string& host)
{
    if (host.empty() || host.size() > 253 || host[0] == '.' || host[0] == '-' || host.find("..") != std::string::npos)
    {
        return false;
    }
    const size_t colon = host.find(':');
    const std::string name = host.substr(0, colon);
    if (name.empty() || name.back() == '.' || name.find('.') == std::string::npos)
    {
        return false;
    }
    for (char c : name)
    {
        if (!isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '.')
        {
            return false;
        }
    }
    if (colon != std::string::npos)
    {
        const std::string port = host.substr(colon + 1);
        if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos)
        {
            return false;
        }
    }
    return true;
}

namespace
{
    bool knownHost(const std::string& host, Provider& out)
    {
        if (host == "github.com")
        {
            out = Provider::GITHUB;
        }
        else if (host == "gitlab.com" || host.find("gitlab") != std::string::npos)
        {
            out = Provider::GITLAB;
        }
        else if (host == "codeberg.org" || host == "gitea.com" || host.find("gitea") != std::string::npos ||
            host.find("forgejo") != std::string::npos)
        {
            out = Provider::GITEA;
        }
        else if (host == "bitbucket.org")
        {
            out = Provider::BITBUCKET;
        }
        else
        {
            return false;
        }
        return true;
    }

    // parts: the path after the host. GitLab's "/-/" separates the project from the page.
    bool parseRepoPath(Provider provider, std::vector<std::string> parts, std::string& owner, std::string& repo,
        std::string& ref)
    {
        size_t page = parts.size();     // first part after owner/repo
        size_t ref_at = parts.size();   // first part of the ref
        if (provider == Provider::GITLAB)
        {
            for (size_t i = 0; i < parts.size(); ++i)
            {
                if (parts[i] == "-")
                {
                    page = i;
                    break;
                }
            }
            if (page < 2)
            {
                return false;
            }
            if (page < parts.size())
            {
                if (parts.size() < page + 3 || parts[page + 1] != "tree")
                {
                    return false;
                }
                ref_at = page + 2;
            }
        }
        else
        {
            if (parts.size() < 2)
            {
                return false;
            }
            page = 2;
            if (parts.size() > 2)
            {
                if (provider == Provider::GITHUB && parts[2] == "tree" && parts.size() >= 4)
                {
                    ref_at = 3;
                }
                else if (provider == Provider::GITEA && parts[2] == "src" && parts.size() >= 5 &&
                    (parts[3] == "branch" || parts[3] == "tag"))
                {
                    ref_at = 4;
                }
                else if (provider == Provider::BITBUCKET && parts[2] == "src" && parts.size() >= 4)
                {
                    ref_at = 3;
                }
                else
                {
                    return false;
                }
            }
        }

        owner.clear();
        for (size_t i = 0; i + 1 < page; ++i)
        {
            if (!validName(parts[i]))
            {
                return false;
            }
            owner += (i ? "/" : "") + parts[i];
        }
        repo = parts[page - 1];
        if (endsWith(repo, ".git"))
        {
            repo.erase(repo.size() - 4);
        }
        if (!validName(repo))
        {
            return false;
        }
        ref.clear();
        for (size_t i = ref_at; i < parts.size(); ++i)
        {
            ref += (i > ref_at ? "/" : "") + parts[i];
        }
        return true;
    }

    bool parseZipLink(const std::string& text, RepoRef& out)
    {
        if (!startsWith(text, "https://") || text.size() > 2000)
        {
            return false;
        }
        for (char c : text)
        {
            const unsigned char u = static_cast<unsigned char>(c);
            if (u <= 0x20 || u >= 0x7f || c == '\\' || c == '"' || c == '<' || c == '>')
            {
                return false;
            }
        }
        const size_t host_end = text.find_first_of("/?#", 8);
        std::string host = text.substr(8, host_end == std::string::npos ? std::string::npos : host_end - 8);
        LLStringUtil::toLower(host);
        if (host.find('@') != std::string::npos || !isValidHost(host))
        {
            return false;   // no user:password@ in links
        }
        const std::string path = host_end == std::string::npos ? std::string() :
            text.substr(host_end, text.find_first_of("?#", host_end) - host_end);
        std::string file = path.substr(path.rfind('/') + 1);
        std::string lower = file;
        LLStringUtil::toLower(lower);
        if (!endsWith(lower, ".zip") || file.size() <= 4)
        {
            return false;
        }
        out.provider = Provider::ZIP;
        out.host = host;
        out.owner = host;
        out.repo = file.substr(0, file.size() - 4);
        out.ref.clear();
        out.url = "https://" + host + text.substr(8 + host.size());
        return true;
    }
}

bool parseRepoRef(const std::string& text, const std::string& branch, RepoRef& out)
{
    return parseRepoRef(text, branch, nullptr, out);
}

bool parseRepoRef(const std::string& text, const std::string& branch, const Provider* forced, RepoRef& out)
{
    std::string s = text;
    LLStringUtil::trim(s);
    std::string ref_override = branch;
    LLStringUtil::trim(ref_override);

    // A folder on this computer: an absolute path, no branch
    if ((!forced || *forced == Provider::LOCAL) && isAbsolutePathText(s))
    {
        return parseLocalFolder(s, out);
    }
    if (forced && *forced == Provider::LOCAL)
    {
        return false;
    }
    // A link to a zip file: any host, nothing else to read from it
    if ((!forced || *forced == Provider::ZIP) && parseZipLink(s, out))
    {
        return true;
    }
    if (forced && *forced == Provider::ZIP)
    {
        return false;
    }

    bool has_scheme = false;
    for (const std::string prefix : { "https://", "http://" })
    {
        if (startsWith(s, prefix))
        {
            s.erase(0, prefix.size());
            has_scheme = true;
            break;
        }
    }
    // A page link may carry a query or an anchor ("?tab=readme-ov-file")
    const size_t query = has_scheme ? s.find_first_of("?#") : std::string::npos;
    if (query != std::string::npos)
    {
        s.erase(query);
    }
    while (!s.empty() && s.back() == '/')
    {
        s.pop_back();
    }

    std::vector<std::string> parts = splitPath(s);
    // "owner/repo" alone is GitHub; a first part with a dot (or after a scheme) is a host
    std::string host = "github.com";
    if (has_scheme || (!parts.empty() && parts[0].find('.') != std::string::npos))
    {
        host = parts.empty() ? std::string() : parts[0];
        LLStringUtil::toLower(host);
        if (startsWith(host, "www."))
        {
            host.erase(0, 4);
        }
        parts.erase(parts.begin());
    }
    if (!isValidHost(host))
    {
        return false;
    }

    Provider provider = Provider::GITHUB;
    if (forced)
    {
        provider = *forced;
    }
    else if (!knownHost(host, provider))
    {
        return false;   // a self-hosted instance: the kind must be chosen
    }

    std::string owner, repo, ref;
    if (!parseRepoPath(provider, parts, owner, repo, ref))
    {
        return false;
    }
    if (!ref_override.empty())
    {
        ref = ref_override;
    }
    if (!ref.empty() && !isValidRef(ref))
    {
        return false;
    }

    out.provider = provider;
    out.host = host;
    out.owner = owner;
    out.repo = repo;
    out.ref = ref;
    out.url.clear();
    out.path.clear();
    return true;
}

bool isValidRef(const std::string& ref)
{
    if (ref.empty() || ref.size() > 255 || ref[0] == '/' || ref[0] == '-' || ref.back() == '/' ||
        ref.back() == '.' || ref.find("..") != std::string::npos || ref.find("//") != std::string::npos ||
        ref.find("@{") != std::string::npos || endsWith(ref, ".lock"))
    {
        return false;
    }
    for (char c : ref)
    {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7f || c == '~' || c == '^' || c == ':' || c == '?' || c == '*' || c == '[' ||
            c == '\\')
        {
            return false;
        }
    }
    for (const std::string& part : splitPath(ref))
    {
        if (!part.empty() && part[0] == '.')
        {
            return false;
        }
    }
    return true;
}

std::string inWorldName(const std::string& name)
{
    std::string out = name;
    LLStringUtil::replaceNonstandardASCII(out, ' ');
    LLStringUtil::replaceChar(out, '|', ' ');
    LLStringUtil::trim(out);
    LLStringUtil::truncate(out, MAX_NAME_BYTES);
    return out;
}

std::vector<std::string> splitIgnore(const std::string& csv)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= csv.size())
    {
        size_t comma = csv.find(',', start);
        if (comma == std::string::npos)
        {
            comma = csv.size();
        }
        std::string part = csv.substr(start, comma - start);
        LLStringUtil::trim(part);
        if (!part.empty())
        {
            out.push_back(part);
        }
        start = comma + 1;
    }
    return out;
}

bool globMatch(const std::string& pattern, const std::string& path)
{
    size_t p = 0, s = 0, star = std::string::npos, mark = 0;
    while (s < path.size())
    {
        if (p < pattern.size() && pattern[p] == '*')
        {
            star = p++;
            mark = s;
        }
        else if (p < pattern.size() && pattern[p] == path[s])
        {
            ++p;
            ++s;
        }
        else if (star != std::string::npos)
        {
            p = star + 1;
            s = ++mark;
        }
        else
        {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*')
    {
        ++p;
    }
    return p == pattern.size();
}

bool isSafeZipPath(const std::string& path)
{
    if (path.empty() || path[0] == '/' || path.find('\\') != std::string::npos ||
        path.find(':') != std::string::npos)
    {
        return false;
    }
    for (const std::string& part : splitPath(path))
    {
        if (part == "..")
        {
            return false;
        }
    }
    return true;
}

Entry classify(const std::string& path, std::string data, const std::vector<std::string>& ignore, bool images)
{
    Entry e;
    e.path = path;
    e.md5 = md5Hex(data);
    e.data = std::move(data);

    const size_t slash = path.rfind('/');
    const std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
    std::string lower = file;
    LLStringUtil::toLower(lower);

    auto skip = [&e](const std::string& reason)
    {
        e.kind = Kind::SKIPPED;
        e.skip_reason = reason;
        return e;
    };

    for (const std::string& part : splitPath(path))
    {
        if (!part.empty() && part[0] == '.')
        {
            return skip("hidden folder or file");
        }
    }
    for (const std::string& pattern : ignore)
    {
        if (globMatch(pattern, path))
        {
            return skip("ignored (" + pattern + ")");
        }
    }

    size_t ending = 0;
    if (endsWith(lower, ".lsl"))
    {
        e.kind = Kind::LSL;
        ending = 4;
    }
    else if (endsWith(lower, ".luau"))
    {
        e.kind = Kind::LUA;
        ending = 5;
    }
    else if (endsWith(lower, ".md"))
    {
        e.kind = Kind::NOTECARD;
        ending = 3;
    }
    else if (endsWith(lower, ".txt"))
    {
        e.kind = Kind::NOTECARD;
        ending = 4;
    }
    else if (endsWith(lower, ".png") || endsWith(lower, ".jpg") || endsWith(lower, ".tga"))
    {
        e.kind = Kind::IMAGE;
        ending = 4;
    }
    else if (endsWith(lower, ".jpeg"))
    {
        e.kind = Kind::IMAGE;
        ending = 5;
    }
    else
    {
        return skip("not a script, notecard or image");
    }

    // Named as the inventory will name them, so the folders, items and same-named scripts in objects are found
    e.name = inWorldName(file.substr(0, file.size() - ending));
    if (e.name.empty())
    {
        return skip("no name");
    }
    if (slash != std::string::npos)
    {
        std::vector<std::string> folders = splitPath(path.substr(0, slash));
        for (std::string& folder : folders)
        {
            folder = inWorldName(folder);
            if (folder.empty())
            {
                return skip("a folder name has no usable characters");
            }
            e.folder += (e.folder.empty() ? "" : "/") + folder;
        }
    }

    if (e.kind == Kind::IMAGE)
    {
        return images ? e : skip("images off");
    }
    // Scripts go up whatever their size: the region compiles them and says when one is too big
    if (e.kind == Kind::NOTECARD && e.data.size() > MAX_TEXT_BYTES)
    {
        return skip("over 64 KB");
    }
    if (!validUtf8(e.data))
    {
        return skip("not UTF-8");
    }
    return e;
}

bool sameText(const std::string& a, const std::string& b)
{
    return normalized(a) == normalized(b);
}

std::string md5Hex(const std::string& data)
{
    LLMD5 md5;
    md5.update(data);
    md5.finalize();
    char hex[33];
    md5.hex_digest(hex);
    return std::string(hex);
}

bool readZip(const std::string& buffer, std::vector<std::pair<std::string, std::string>>& files,
    std::vector<std::string>& unsafe_paths, std::string& error)
{
    if (buffer.size() > MAX_ZIP_BYTES)
    {
        error = "The download is over 50 MB.";
        return false;
    }

    void* reader = mz_zip_reader_create();
    if (!reader)
    {
        error = "Could not read the download.";
        return false;
    }
    if (mz_zip_reader_open_buffer(reader, (uint8_t*)buffer.data(), (int32_t)buffer.size(), 0) != MZ_OK)
    {
        mz_zip_reader_delete(&reader);
        error = "The download is not a zip file.";
        return false;
    }

    bool ok = true;
    size_t entries = 0;
    size_t total = 0;
    int32_t status = mz_zip_reader_goto_first_entry(reader);
    for (; status == MZ_OK; status = mz_zip_reader_goto_next_entry(reader))
    {
        if (++entries > MAX_ZIP_ENTRIES)
        {
            error = "The repository has over 10,000 files.";
            ok = false;
            break;
        }

        mz_zip_file* info = nullptr;
        if (mz_zip_reader_entry_get_info(reader, &info) != MZ_OK || !info || !info->filename)
        {
            error = "The zip file is damaged.";
            ok = false;
            break;
        }
        if (mz_zip_reader_entry_is_dir(reader) == MZ_OK)
        {
            continue;
        }

        const std::string full = info->filename;
        if (!isSafeZipPath(full))
        {
            unsafe_paths.push_back(full);
            continue;
        }

        if (info->uncompressed_size < 0 || (size_t)info->uncompressed_size > MAX_ZIP_BYTES - total)
        {
            error = "The repository is over 50 MB unpacked.";
            ok = false;
            break;
        }
        total += (size_t)info->uncompressed_size;

        std::string data((size_t)info->uncompressed_size, '\0');
        if (!data.empty() &&
            mz_zip_reader_entry_save_buffer(reader, data.data(), (int32_t)data.size()) != MZ_OK)
        {
            error = "The zip file is damaged.";
            ok = false;
            break;
        }
        files.emplace_back(full, std::move(data));
    }
    if (ok && status != MZ_END_OF_LIST && status != MZ_OK)
    {
        error = "The zip file is damaged.";
        ok = false;
    }

    mz_zip_reader_close(reader);
    mz_zip_reader_delete(&reader);

    // Archives made by GitHub ("<owner>-<repo>-<commit>/"), GitLab, Gitea and Bitbucket put everything in one top
    // folder: leave it out. A zip with files at its top, or several top folders, keeps its paths.
    if (ok && !files.empty())
    {
        const size_t slash = files.front().first.find('/');
        const std::string top = slash == std::string::npos ? std::string() : files.front().first.substr(0, slash + 1);
        bool shared = !top.empty();
        for (const auto& file : files)
        {
            shared = shared && startsWith(file.first, top) && file.first.size() > top.size();
        }
        if (shared)
        {
            for (auto& file : files)
            {
                file.first.erase(0, top.size());
            }
        }
    }
    return ok;
}

bool readFolder(const std::string& dir, std::vector<std::pair<std::string, std::string>>& files,
    std::vector<std::string>& unsafe_paths, std::string& error)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = fsyspath(dir);
    if (dir.empty() || !fs::is_directory(root, ec))
    {
        error = "The folder was not found.";
        return false;
    }

    size_t entries = 0;
    size_t total = 0;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    while (!ec && it != end)
    {
        const fs::directory_entry entry = *it;
        const std::string name = fsyspath(entry.path().filename()).string();
        const bool hidden = !name.empty() && name[0] == '.';

        // A link could lead anywhere on the disk: never followed, whether to a file or a folder (a junction too)
        if (entry.is_symlink(ec))
        {
            it.disable_recursion_pending();
            it.increment(ec);
            continue;
        }
        if (entry.is_directory(ec))
        {
            if (hidden)
            {
                it.disable_recursion_pending(); // .git and the like: not entered, not listed
            }
            it.increment(ec);
            continue;
        }
        if (!entry.is_regular_file(ec))
        {
            it.increment(ec);
            continue;
        }
        if (++entries > MAX_ZIP_ENTRIES)
        {
            error = "The folder has over 10,000 files.";
            return false;
        }

        std::string path;
        for (const fs::path& part : entry.path().lexically_relative(root))
        {
            path += (path.empty() ? "" : "/") + fsyspath(part).string();
        }
        if (!isSafeZipPath(path))
        {
            unsafe_paths.push_back(path);
            it.increment(ec);
            continue;
        }

        // A hidden file is listed (classify says why it is skipped) but not read
        std::string data;
        if (!hidden)
        {
            std::error_code size_ec;
            const uintmax_t size = entry.file_size(size_ec);
            if (size_ec)
            {
                error = "Could not read " + path + ".";
                return false;
            }
            if (size > MAX_ZIP_BYTES - total)
            {
                error = "The folder is over 50 MB.";
                return false;
            }
            total += static_cast<size_t>(size);
            data.resize(static_cast<size_t>(size));
            std::ifstream in(entry.path(), std::ios::binary);
            if (!in.is_open() || (size && !in.read(data.data(), static_cast<std::streamsize>(size))))
            {
                error = "Could not read " + path + ".";
                return false;
            }
        }
        files.emplace_back(std::move(path), std::move(data));
        it.increment(ec);
    }
    if (ec)
    {
        error = "The folder could not be read.";
        return false;
    }

    // The disk's order is arbitrary: sorted, so the listing and the version are stable
    std::sort(files.begin(), files.end(),
        [](const std::pair<std::string, std::string>& a, const std::pair<std::string, std::string>& b)
        {
            return a.first < b.first;
        });
    return true;
}

std::string filesVersion(const std::vector<std::pair<std::string, std::string>>& files)
{
    LLMD5 md5;
    for (const auto& file : files)
    {
        md5.update(file.first);
        md5.update(std::string(1, '\0'));
        md5.update(md5Hex(file.second));
        md5.update(std::string(1, '\n'));
    }
    md5.finalize();
    char hex[33];
    md5.hex_digest(hex);
    return "md5:" + std::string(hex);
}

LLSD toLLSD(const std::vector<RepoState>& repos)
{
    LLSD out = LLSD::emptyArray();
    for (const RepoState& repo : repos)
    {
        LLSD r;
        r["provider"] = providerName(repo.ref.provider);
        r["host"] = repo.ref.host;
        r["owner"] = repo.ref.owner;
        r["repo"] = repo.ref.repo;
        r["ref"] = repo.ref.ref;
        if (repo.ref.provider == Provider::ZIP)
        {
            r["url"] = repo.ref.url;
        }
        if (repo.ref.provider == Provider::LOCAL)
        {
            r["path"] = repo.ref.path;
        }
        r["commit"] = repo.commit;
        r["synced"] = repo.synced;
        LLSD files = LLSD::emptyMap();
        for (const auto& file : repo.files)
        {
            files[file.first]["md5"] = file.second.md5;
            files[file.first]["item"] = file.second.item_id;
            if (file.second.asset_id.notNull())
            {
                files[file.first]["asset"] = file.second.asset_id;
            }
            if (file.second.compile_checked)
            {
                files[file.first]["compiled"] = file.second.compiled;
            }
        }
        r["files"] = files;
        r["enabled"] = repo.enabled;
        if (repo.next_owner)
        {
            r["next_owner"] = static_cast<LLSD::Integer>(repo.next_owner);
        }
        out.append(r);
    }
    return out;
}

std::vector<RepoState> fromLLSD(const LLSD& sd)
{
    std::vector<RepoState> out;
    if (!sd.isArray())
    {
        return out;
    }
    for (const LLSD& r : llsd::inArray(sd))
    {
        RepoState repo;
        // Entries from before other hosts were supported are GitHub's
        Provider provider = Provider::GITHUB;
        if (r.has("provider") && !providerFromName(r["provider"].asString(), provider))
        {
            continue;
        }
        if (provider == Provider::ZIP)
        {
            if (!parseZipLink(r["url"].asString(), repo.ref))
            {
                continue;
            }
        }
        else if (provider == Provider::LOCAL)
        {
            if (!parseLocalFolder(r["path"].asString(), repo.ref))
            {
                continue;
            }
        }
        else
        {
            const std::string host = r.has("host") ? r["host"].asString() : std::string("github.com");
            const std::string text = "https://" + host + "/" + r["owner"].asString() + "/" + r["repo"].asString();
            if (!parseRepoRef(text, r["ref"].asString(), &provider, repo.ref))
            {
                continue;
            }
        }
        repo.commit = r["commit"].asString();
        repo.synced = r["synced"].asDate();
        repo.enabled = !r.has("enabled") || r["enabled"].asBoolean();     // entries from before the choice: on
        repo.next_owner = static_cast<U32>(r["next_owner"].asInteger());
        for (const auto& file : llsd::inMap(r["files"]))
        {
            repo.files[file.first] = { file.second["md5"].asString(), file.second["item"].asUUID(),
                file.second["asset"].asUUID(), file.second["compiled"].asBoolean(), file.second.has("compiled") };
        }
        out.push_back(repo);
    }
    return out;
}

InvAction decideInventory(const Entry* entry, const FileState* state, bool item_present)
{
    if (!entry)
    {
        return InvAction::GONE;
    }
    if (entry->kind == Kind::SKIPPED)
    {
        return InvAction::SKIP;
    }
    if (!state || !item_present)
    {
        return InvAction::CREATE;
    }
    return state->md5 == entry->md5 ? InvAction::UNCHANGED : InvAction::UPDATE;
}

RegionAction decideRegion(const std::string& script_name, const std::string& old_text, bool old_is_lua,
    const std::vector<const Entry*>& entries, const Entry*& source)
{
    source = nullptr;
    std::vector<const Entry*> same_language;
    bool other_language = false;
    for (const Entry* e : entries)
    {
        if (!e || e->name != script_name || (e->kind != Kind::LSL && e->kind != Kind::LUA))
        {
            continue;
        }
        if ((e->kind == Kind::LUA) == old_is_lua)
        {
            same_language.push_back(e);
        }
        else
        {
            other_language = true;
        }
    }

    if (same_language.empty())
    {
        return other_language ? RegionAction::LANGUAGE_DIFFERS : RegionAction::NOT_IN_REPO;
    }
    for (const Entry* e : same_language)
    {
        if (sameText(old_text, e->data))
        {
            source = e;
            return RegionAction::CURRENT;
        }
    }
    for (const Entry* e : same_language)
    {
        if (!sameText(e->data, same_language.front()->data))
        {
            return RegionAction::AMBIGUOUS;
        }
    }
    source = same_language.front();
    return RegionAction::WOULD_CHANGE;
}
}
