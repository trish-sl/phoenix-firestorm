/**
 * @file fsscriptrepos.cpp
 * @brief Script repositories: repo references, zip reading, file rules and sync decisions.
 */

#include "llviewerprecompiledheaders.h"

#include "fsscriptrepos.h"

#include "llmd5.h"
#include "llstring.h"

#include "minizip-ng/mz.h"
#include "minizip-ng/mz_strm.h"
#include "minizip-ng/mz_zip.h"
#include "minizip-ng/mz_zip_rw.h"

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

bool parseRepoRef(const std::string& text, const std::string& branch, RepoRef& out)
{
    std::string s = text;
    LLStringUtil::trim(s);
    std::string ref_override = branch;
    LLStringUtil::trim(ref_override);

    for (const std::string prefix : { "https://", "http://" })
    {
        if (startsWith(s, prefix))
        {
            s.erase(0, prefix.size());
            break;
        }
    }
    for (const std::string host : { "www.github.com/", "github.com/" })
    {
        if (startsWith(s, host))
        {
            s.erase(0, host.size());
            break;
        }
    }
    while (!s.empty() && s.back() == '/')
    {
        s.pop_back();
    }

    std::vector<std::string> parts = splitPath(s);
    if (parts.size() < 2 || (parts.size() > 2 && (parts[2] != "tree" || parts.size() < 4)))
    {
        return false;
    }

    std::string repo = parts[1];
    if (endsWith(repo, ".git"))
    {
        repo.erase(repo.size() - 4);
    }
    if (!validName(parts[0]) || !validName(repo))
    {
        return false;
    }

    std::string ref;
    for (size_t i = 3; i < parts.size(); ++i)
    {
        ref += (i > 3 ? "/" : "") + parts[i];
    }

    out.owner = parts[0];
    out.repo = repo;
    out.ref = ref_override.empty() ? ref : ref_override;
    return true;
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
    e.folder = slash == std::string::npos ? std::string() : path.substr(0, slash);
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
    else if (lower == "readme.md")
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

    e.name = file.substr(0, file.size() - ending);
    if (e.name.empty())
    {
        return skip("no name");
    }

    if (e.kind == Kind::IMAGE)
    {
        return images ? e : skip("images off");
    }
    if (e.data.size() > MAX_TEXT_BYTES)
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

        // GitHub puts everything under one "<owner>-<repo>-<commit>/" folder
        const std::string full = info->filename;
        const size_t slash = full.find('/');
        const std::string path = slash == std::string::npos ? full : full.substr(slash + 1);
        if (!isSafeZipPath(full) || !isSafeZipPath(path))
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
        files.emplace_back(path, std::move(data));
    }
    if (ok && status != MZ_END_OF_LIST && status != MZ_OK)
    {
        error = "The zip file is damaged.";
        ok = false;
    }

    mz_zip_reader_close(reader);
    mz_zip_reader_delete(&reader);
    return ok;
}

LLSD toLLSD(const std::vector<RepoState>& repos)
{
    LLSD out = LLSD::emptyArray();
    for (const RepoState& repo : repos)
    {
        LLSD r;
        r["owner"] = repo.ref.owner;
        r["repo"] = repo.ref.repo;
        r["ref"] = repo.ref.ref;
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
        repo.ref.owner = r["owner"].asString();
        repo.ref.repo = r["repo"].asString();
        repo.ref.ref = r["ref"].asString();
        if (!validName(repo.ref.owner) || !validName(repo.ref.repo))
        {
            continue;
        }
        repo.commit = r["commit"].asString();
        repo.synced = r["synced"].asDate();
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
