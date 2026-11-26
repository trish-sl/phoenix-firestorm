/**
 * @file fsscriptrepos_test.cpp
 * @brief Tests for the script repositories core (fsscriptrepos.cpp).
 */

#include "../llviewerprecompiledheaders.h"

#include "../test/lltut.h"

#include "../fsscriptrepos.h"

#include "fsyspath.h"

#include "minizip-ng/mz.h"
#include "minizip-ng/mz_strm.h"
#include "minizip-ng/mz_strm_mem.h"
#include "minizip-ng/mz_zip.h"
#include "minizip-ng/mz_zip_rw.h"

#include <cstring>
#include <filesystem>
#include <fstream>

using namespace FSScriptRepos;

namespace
{
    // Builds a zip in memory from (path, data) pairs.
    std::string makeZip(const std::vector<std::pair<std::string, std::string>>& files)
    {
        void* stream = mz_stream_mem_create();
        mz_stream_mem_set_grow_size(stream, 64 * 1024);
        mz_stream_open(stream, nullptr, MZ_OPEN_MODE_CREATE);
        void* writer = mz_zip_writer_create();
        mz_zip_writer_open(writer, stream, 0);
        for (const auto& file : files)
        {
            mz_zip_file info;
            memset(&info, 0, sizeof(info));
            info.filename = file.first.c_str();
            info.compression_method = MZ_COMPRESS_METHOD_DEFLATE;
            info.flag = MZ_ZIP_FLAG_UTF8;
            mz_zip_writer_add_buffer(writer, (void*)file.second.data(), (int32_t)file.second.size(), &info);
        }
        mz_zip_writer_close(writer);
        mz_zip_writer_delete(&writer);

        const void* buf = nullptr;
        int32_t len = 0;
        mz_stream_mem_get_buffer(stream, &buf);
        mz_stream_mem_get_buffer_length(stream, &len);
        std::string out(static_cast<const char*>(buf), len);
        mz_stream_mem_delete(&stream);
        return out;
    }

    Entry script(const std::string& path, const std::string& text)
    {
        return classify(path, text, {}, false);
    }

    // A folder of files under the system's temporary folder, removed with the object
    struct TempFolder
    {
        std::filesystem::path root;

        TempFolder()
        {
            root = std::filesystem::temp_directory_path() / ("fsscriptrepos_test_" + LLUUID::generateNewID().asString());
            std::filesystem::create_directories(root);
        }
        ~TempFolder()
        {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }
        void write(const std::string& relative, const std::string& data)
        {
            const std::filesystem::path file = root / fsyspath(relative);
            std::filesystem::create_directories(file.parent_path());
            std::ofstream out(file, std::ios::binary);
            out.write(data.data(), data.size());
        }
        std::string path() const
        {
            return fsyspath(root).string();
        }
    };
}

namespace tut
{
    struct fsscriptrepos_data {};
    typedef test_group<fsscriptrepos_data> fsscriptrepos_group;
    typedef fsscriptrepos_group::object fsscriptrepos_object;
    tut::fsscriptrepos_group fsscriptrepos_testgroup("fsscriptrepos");

    // parseRepoRef: accepted forms
    template<> template<> void fsscriptrepos_object::test<1>()
    {
        RepoRef r;
        ensure("plain", parseRepoRef("RheaMercury/sl-scripts", "", r));
        ensure_equals(r.owner, "RheaMercury");
        ensure_equals(r.repo, "sl-scripts");
        ensure_equals(r.ref, "");

        ensure("url .git", parseRepoRef("https://github.com/o/r.git", "", r));
        ensure_equals(r.owner, "o");
        ensure_equals(r.repo, "r");
        ensure_equals(r.ref, "");

        ensure("trailing slash", parseRepoRef("o/r/", "", r));
        ensure_equals(r.repo, "r");

        ensure("tree url", parseRepoRef("https://github.com/o/r/tree/feature/x", "", r));
        ensure_equals(r.repo, "r");
        ensure_equals(r.ref, "feature/x");

        ensure("branch wins", parseRepoRef("https://github.com/o/r/tree/feature/x", "main", r));
        ensure_equals(r.ref, "main");

        ensure("spaces trimmed", parseRepoRef("  o/r  ", " dev ", r));
        ensure_equals(r.ref, "dev");
    }

    // parseRepoRef: rejected forms
    template<> template<> void fsscriptrepos_object::test<2>()
    {
        RepoRef r;
        ensure("owner only", !parseRepoRef("o", "", r));
        ensure("empty", !parseRepoRef("", "", r));
        ensure("extra path", !parseRepoRef("o/r/extra", "", r));
        ensure("other host", !parseRepoRef("https://gitlab.com/o/r", "", r));
        ensure("bad char", !parseRepoRef("o/r?x", "", r));
        ensure("bad branch", !parseRepoRef("o/r", "../../user", r));
        ensure("bad tree ref", !parseRepoRef("https://github.com/o/r/tree/a..b", "", r));
    }

    // classify: what counts
    template<> template<> void fsscriptrepos_object::test<3>()
    {
        Entry e = classify("doors/trap.luau", "print(1)", {}, false);
        ensure("lua", e.kind == Kind::LUA);
        ensure_equals(e.folder, "doors");
        ensure_equals(e.name, "trap");
        ensure_equals(e.md5, md5Hex("print(1)"));

        e = classify("a.lsl", "default{}", {}, false);
        ensure("lsl", e.kind == Kind::LSL);
        ensure_equals(e.folder, "");
        ensure_equals(e.name, "a");

        e = classify("cfg/Settings.txt", "x=1", {}, false);
        ensure("notecard", e.kind == Kind::NOTECARD);
        ensure_equals(e.name, "Settings");

        e = classify("README.md", "# hi", {}, false);
        ensure("readme", e.kind == Kind::NOTECARD);
        ensure_equals(e.name, "README");

        e = classify("docs/Guide.MD", "x", {}, false);
        ensure("other md", e.kind == Kind::NOTECARD);
        ensure_equals(e.name, "Guide");
        ensure_equals(e.folder, "docs");

        e = classify("img/a.png", "\x89PNG", {}, false);
        ensure("image off", e.kind == Kind::SKIPPED);
        ensure_equals(e.skip_reason, "images off");
        ensure("image on", classify("img/a.PNG", "\x89PNG", {}, true).kind == Kind::IMAGE);
        ensure("tga", classify("a.tga", "x", {}, true).kind == Kind::IMAGE);
    }

    // classify: skips
    template<> template<> void fsscriptrepos_object::test<4>()
    {
        const std::vector<std::string> ignore = splitIgnore(DEFAULT_IGNORE);
        ensure("dot folder", classify(".github/x.lsl", "x", ignore, false).kind == Kind::SKIPPED);
        ensure("ignored", classify("a.test.lsl", "x", ignore, false).kind == Kind::SKIPPED);
        ensure("notecard too big", classify("big.txt", std::string(MAX_TEXT_BYTES + 1, 'a'), {}, false).kind == Kind::SKIPPED);
        ensure("notecard max size ok", classify("max.txt", std::string(MAX_TEXT_BYTES, 'a'), {}, false).kind == Kind::NOTECARD);
        ensure("big lsl ok", classify("big.lsl", std::string(4 * MAX_TEXT_BYTES, 'a'), {}, false).kind == Kind::LSL);
        ensure("big luau ok", classify("big.luau", std::string(4 * MAX_TEXT_BYTES, 'a'), {}, false).kind == Kind::LUA);

        Entry e = classify("bad.txt", "\xff\xfe", {}, false);
        ensure("not utf8", e.kind == Kind::SKIPPED);
        ensure_equals(e.skip_reason, "not UTF-8");
        ensure("utf8 ok", classify("ok.txt", "caf\xc3\xa9", {}, false).kind == Kind::NOTECARD);
        ensure("unknown ending", classify("x.py", "x", {}, false).kind == Kind::SKIPPED);
    }

    // ignore patterns
    template<> template<> void fsscriptrepos_object::test<5>()
    {
        ensure("nested", globMatch("*/original/*", "lsl/original/a.lsl"));
        ensure("test files", globMatch("*.test.*", "x/a.test.lsl"));
        ensure("no match", !globMatch("*.test.*", "a.lsl"));
        ensure("exact", globMatch("a.lsl", "a.lsl"));
        std::vector<std::string> parts = splitIgnore(" *.test.* , */original/* ,, ");
        ensure_equals(parts.size(), 2U);
        ensure_equals(parts[1], "*/original/*");
    }

    // unsafe zip paths
    template<> template<> void fsscriptrepos_object::test<6>()
    {
        ensure("dotdot", !isSafeZipPath("../a"));
        ensure("absolute", !isSafeZipPath("/a"));
        ensure("backslash", !isSafeZipPath("a\\b"));
        ensure("inner dotdot", !isSafeZipPath("a/../b"));
        ensure("drive", !isSafeZipPath("C:/a"));
        ensure("normal", isSafeZipPath("a/b.lsl"));
    }

    // sameText
    template<> template<> void fsscriptrepos_object::test<7>()
    {
        ensure("crlf", sameText("a\r\nb\r\n", "a\nb"));
        ensure("cr", sameText("a\rb", "a\nb"));
        ensure("differs", !sameText("a\nb", "a\nc"));
    }

    // readZip
    template<> template<> void fsscriptrepos_object::test<8>()
    {
        std::string zip = makeZip({ { "o-r-abc123/", "" },
                                    { "o-r-abc123/a.lsl", "default{}" },
                                    { "o-r-abc123/x/b.txt", "note" },
                                    { "o-r-abc123/../evil.lsl", "bad" } });
        std::vector<std::pair<std::string, std::string>> files;
        std::vector<std::string> unsafe;
        std::string error;
        ensure("read ok", readZip(zip, files, unsafe, error));
        ensure_equals(files.size(), 2U);
        ensure_equals(files[0].first, "a.lsl");
        ensure_equals(files[0].second, "default{}");
        ensure_equals(files[1].first, "x/b.txt");
        ensure_equals(unsafe.size(), 1U);

        files.clear();
        unsafe.clear();
        ensure("garbage", !readZip("not a zip at all", files, unsafe, error));
        ensure("error text", !error.empty());
    }

    // decideInventory
    template<> template<> void fsscriptrepos_object::test<9>()
    {
        Entry e = script("a.lsl", "x");
        FileState same{ e.md5, LLUUID::generateNewID() };
        FileState other{ md5Hex("y"), LLUUID::generateNewID() };
        ensure("new", decideInventory(&e, nullptr, false) == InvAction::CREATE);
        ensure("unchanged", decideInventory(&e, &same, true) == InvAction::UNCHANGED);
        ensure("item gone", decideInventory(&e, &same, false) == InvAction::CREATE);
        ensure("changed", decideInventory(&e, &other, true) == InvAction::UPDATE);
        ensure("changed, item gone", decideInventory(&e, &other, false) == InvAction::CREATE);
        ensure("file gone", decideInventory(nullptr, &same, true) == InvAction::GONE);
        Entry skipped = classify("x.py", "x", {}, false);
        ensure("skip", decideInventory(&skipped, nullptr, false) == InvAction::SKIP);
    }

    // decideRegion
    template<> template<> void fsscriptrepos_object::test<10>()
    {
        Entry foo_lsl = script("x/foo.lsl", "lsl foo");
        Entry foo_lua = script("y/foo.luau", "lua foo");
        Entry bar1 = script("a/bar.lsl", "bar one");
        Entry bar2 = script("b/bar.lsl", "bar two");
        Entry only = script("onlylsl.lsl", "only");
        Entry same1 = script("c/same.lsl", "same text");
        Entry same2 = script("d/same.lsl", "same text\r\n");
        std::vector<const Entry*> entries = { &foo_lsl, &foo_lua, &bar1, &bar2, &only, &same1, &same2 };
        const Entry* source = nullptr;

        ensure("current", decideRegion("foo", "lsl foo\r\n", false, entries, source) == RegionAction::CURRENT);
        ensure("current source", source == &foo_lsl);
        ensure("lsl change", decideRegion("foo", "old", false, entries, source) == RegionAction::WOULD_CHANGE);
        ensure("lsl source", source == &foo_lsl);
        ensure("lua change", decideRegion("foo", "old", true, entries, source) == RegionAction::WOULD_CHANGE);
        ensure("lua source", source == &foo_lua);
        ensure("language differs", decideRegion("onlylsl", "old", true, entries, source) == RegionAction::LANGUAGE_DIFFERS);
        ensure("ambiguous", decideRegion("bar", "old", false, entries, source) == RegionAction::AMBIGUOUS);
        // spec: equal to a repo version is current, checked before ambiguity
        ensure("equal to one of two", decideRegion("bar", "bar one", false, entries, source) == RegionAction::CURRENT);
        ensure("equal to one source", source == &bar1);
        ensure("same text twice", decideRegion("same", "old", false, entries, source) == RegionAction::WOULD_CHANGE);
        ensure("not in repo", decideRegion("nothere", "x", false, entries, source) == RegionAction::NOT_IN_REPO);
    }

    // state round trip
    template<> template<> void fsscriptrepos_object::test<11>()
    {
        RepoState a;
        ensure("parse a", parseRepoRef("o/r", "main", a.ref));
        a.commit = "0123456789abcdef0123456789abcdef01234567";
        a.synced = LLDate(1763424000.0);
        a.files["x/a.lsl"] = { md5Hex("a"), LLUUID::generateNewID(), LLUUID::generateNewID(), true, true };
        a.files["y/fails.lsl"] = { md5Hex("f"), LLUUID::generateNewID(), LLUUID::generateNewID(), false, true };
        a.files["b.txt"] = { md5Hex("b"), LLUUID::generateNewID() };
        a.enabled = false;
        a.next_owner = 0x80000 | 0x4000;    // PERM_MOVE | PERM_MODIFY
        RepoState b;
        ensure("parse b", parseRepoRef("p/q", "", b.ref));

        std::vector<RepoState> back = fromLLSD(toLLSD({ a, b }));
        ensure_equals(back.size(), 2U);
        ensure_equals(back[0].ref.owner, "o");
        ensure_equals(back[0].ref.repo, "r");
        ensure_equals(back[0].ref.ref, "main");
        ensure_equals(back[0].commit, a.commit);
        ensure_equals(back[0].synced.secondsSinceEpoch(), a.synced.secondsSinceEpoch());
        ensure_equals(back[0].files.size(), 3U);
        ensure_equals(back[0].files["x/a.lsl"].md5, a.files["x/a.lsl"].md5);
        ensure_equals(back[0].files["x/a.lsl"].item_id, a.files["x/a.lsl"].item_id);
        ensure_equals(back[0].files["x/a.lsl"].asset_id, a.files["x/a.lsl"].asset_id);
        ensure("no asset recorded", back[0].files["b.txt"].asset_id.isNull());
        ensure("compiled kept", back[0].files["x/a.lsl"].compiled);
        ensure("compile checked kept", back[0].files["x/a.lsl"].compile_checked);
        ensure("failed compile kept", !back[0].files["y/fails.lsl"].compiled && back[0].files["y/fails.lsl"].compile_checked);
        ensure("unknown by default", !back[0].files["b.txt"].compiled && !back[0].files["b.txt"].compile_checked);
        ensure("unchecked kept", !back[0].enabled);
        ensure_equals(back[0].next_owner, a.next_owner);
        ensure_equals(back[1].ref.owner, "p");
        ensure_equals(back[1].files.size(), 0U);
        ensure("checked by default", back[1].enabled);
        ensure_equals(back[1].next_owner, 0U);
        ensure("empty llsd", fromLLSD(LLSD()).empty());

        // Entries written before other hosts: GitHub
        LLSD old_entry;
        old_entry["owner"] = "o";
        old_entry["repo"] = "r";
        old_entry["ref"] = "";
        LLSD old_list = LLSD::emptyArray();
        old_list.append(old_entry);
        back = fromLLSD(old_list);
        ensure_equals(back.size(), 1U);
        ensure("old is github", back[0].ref.provider == Provider::GITHUB);
        ensure_equals(back[0].ref.host, "github.com");
        ensure("old is checked", back[0].enabled);
        ensure_equals(back[0].next_owner, 0U);
    }

    // git ref names
    template<> template<> void fsscriptrepos_object::test<12>()
    {
        ensure("branch", isValidRef("main"));
        ensure("nested", isValidRef("feature/x-1.2"));
        ensure("tag", isValidRef("v1.0.0"));
        ensure("dotdot", !isValidRef("a..b"));
        ensure("up", !isValidRef("../x"));
        ensure("space", !isValidRef("a b"));
        ensure("query", !isValidRef("a?b"));
        ensure("reflog", !isValidRef("a@{1}"));
        ensure("lock", !isValidRef("a.lock"));
        ensure("hidden part", !isValidRef("a/.b"));
        ensure("double slash", !isValidRef("a//b"));
        ensure("empty", !isValidRef(""));
    }

    // in-world names: as the inventory stores them
    template<> template<> void fsscriptrepos_object::test<13>()
    {
        ensure_equals(inWorldName("  door|opener "), "door opener");
        ensure_equals(inWorldName("caf\xc3\xa9"), "caf");
        ensure_equals(inWorldName(std::string(80, 'a')).size(), MAX_NAME_BYTES);

        Entry e = classify("my|lib/sub dir/long|name.lsl", "x", {}, false);
        ensure("kept", e.kind == Kind::LSL);
        ensure_equals(e.name, "long name");
        ensure_equals(e.folder, "my lib/sub dir");

        e = classify(std::string(70, 'f') + "/a.lsl", "x", {}, false);
        ensure_equals(e.folder, std::string(MAX_NAME_BYTES, 'f'));

        e = classify("\xe6\x97\xa5/a.lsl", "x", {}, false);
        ensure("unusable folder", e.kind == Kind::SKIPPED);
        ensure("no usable name", classify("\xe6\x97\xa5.lsl", "x", {}, false).kind == Kind::SKIPPED);
    }

    // other hosts: detection and links
    template<> template<> void fsscriptrepos_object::test<14>()
    {
        RepoRef r;
        ensure("github bare", parseRepoRef("o/r", "", r) && r.provider == Provider::GITHUB && r.host == "github.com");
        ensure_equals(r.display(), "o/r");
        ensure("github www", parseRepoRef("https://www.GitHub.com/o/r?tab=readme-ov-file", "", r) &&
            r.provider == Provider::GITHUB && r.host == "github.com" && r.repo == "r");

        ensure("gitlab", parseRepoRef("https://gitlab.com/group/sub/proj.git", "", r));
        ensure("gitlab kind", r.provider == Provider::GITLAB);
        ensure_equals(r.owner, "group/sub");
        ensure_equals(r.repo, "proj");
        ensure_equals(r.display(), "gitlab.com/group/sub/proj");
        ensure("gitlab tree", parseRepoRef("https://gitlab.com/g/p/-/tree/dev/x", "", r));
        ensure_equals(r.owner, "g");
        ensure_equals(r.ref, "dev/x");
        ensure("gitlab other page", !parseRepoRef("https://gitlab.com/g/p/-/issues/3", "", r));
        ensure("gitlab one part", !parseRepoRef("https://gitlab.com/p", "", r));
        ensure("gitlab by name", parseRepoRef("https://gitlab.example.org/g/p", "", r) &&
            r.provider == Provider::GITLAB && r.host == "gitlab.example.org");

        ensure("codeberg", parseRepoRef("https://codeberg.org/o/r/src/branch/main", "", r));
        ensure("codeberg kind", r.provider == Provider::GITEA);
        ensure_equals(r.ref, "main");
        ensure("gitea tag", parseRepoRef("https://gitea.com/o/r/src/tag/v1.2", "", r) && r.ref == "v1.2");
        ensure("gitea commit page", !parseRepoRef("https://codeberg.org/o/r/src/commit/abc", "", r));
        ensure("forgejo by name", parseRepoRef("https://forgejo.example.net:3000/o/r", "", r) &&
            r.provider == Provider::GITEA && r.host == "forgejo.example.net:3000");

        ensure("bitbucket", parseRepoRef("https://bitbucket.org/ws/repo/src/main/", "", r));
        ensure("bitbucket kind", r.provider == Provider::BITBUCKET);
        ensure_equals(r.owner, "ws");
        ensure_equals(r.ref, "main");

        ensure("unknown host refused", !parseRepoRef("https://git.example.com/o/r", "", r));
        const Provider gitea = Provider::GITEA;
        ensure("unknown host chosen", parseRepoRef("https://git.example.com/o/r", "", &gitea, r) &&
            r.provider == Provider::GITEA && r.host == "git.example.com");
        const Provider gitlab = Provider::GITLAB;
        ensure("forced gitlab", parseRepoRef("git.example.com/a/b/c", "", &gitlab, r) && r.owner == "a/b");
        ensure("bad host", !parseRepoRef("https://exa_mple.com/o/r", "", &gitea, r));
        ensure("bad port", !parseRepoRef("https://example.com:http/o/r", "", &gitea, r));
    }

    // zip links
    template<> template<> void fsscriptrepos_object::test<15>()
    {
        RepoRef r;
        ensure("zip", parseRepoRef("https://example.com/dl/My-Scripts.zip?token=abc", "", r));
        ensure("zip kind", r.provider == Provider::ZIP);
        ensure_equals(r.host, "example.com");
        ensure_equals(r.repo, "My-Scripts");
        ensure_equals(r.url, "https://example.com/dl/My-Scripts.zip?token=abc");
        ensure_equals(r.display(), "https://example.com/dl/My-Scripts.zip");
        ensure("github archive link is a zip",
            parseRepoRef("https://github.com/o/r/archive/refs/heads/main.zip", "", r) && r.provider == Provider::ZIP);
        ensure("http refused", !parseRepoRef("http://example.com/a.zip", "", r));
        ensure("credentials refused", !parseRepoRef("https://user:pw@example.com/a.zip", "", r));
        ensure("not a zip", !parseRepoRef("https://example.com/a.tar.gz", "", r));
        const Provider zip = Provider::ZIP;
        ensure("forced zip needs a zip", !parseRepoRef("https://github.com/o/r", "", &zip, r));

        ensure("keys", parseRepoRef("O/R", "", r));
        RepoRef same;
        ensure("keys 2", parseRepoRef("https://github.com/o/r", "", same));
        ensure_equals(r.key(), same.key());
        RepoRef other;
        ensure("keys 3", parseRepoRef("https://codeberg.org/o/r", "", other));
        ensure("other host differs", other.key() != r.key());

        RepoState a;
        ensure("zip state", parseRepoRef("https://example.com/x/pack.zip", "", a.ref));
        a.commit = "md5:0123";
        RepoState g;
        ensure("gitlab state", parseRepoRef("https://gitlab.com/g/s/p", "dev", g.ref));
        std::vector<RepoState> back = fromLLSD(toLLSD({ a, g }));
        ensure_equals(back.size(), 2U);
        ensure("zip back", back[0].ref.provider == Provider::ZIP && back[0].ref.url == a.ref.url);
        ensure_equals(back[0].commit, "md5:0123");
        ensure("gitlab back", back[1].ref.provider == Provider::GITLAB && back[1].ref.owner == "g/s" &&
            back[1].ref.ref == "dev" && back[1].ref.host == "gitlab.com");
    }

    // readZip: the shared top folder only
    template<> template<> void fsscriptrepos_object::test<16>()
    {
        std::vector<std::pair<std::string, std::string>> files;
        std::vector<std::string> unsafe;
        std::string error;
        ensure("flat", readZip(makeZip({ { "a.lsl", "x" }, { "dir/b.lsl", "y" } }), files, unsafe, error));
        ensure_equals(files.size(), 2U);
        ensure_equals(files[0].first, "a.lsl");
        ensure_equals(files[1].first, "dir/b.lsl");

        files.clear();
        ensure("two tops", readZip(makeZip({ { "x/a.lsl", "x" }, { "y/b.lsl", "y" } }), files, unsafe, error));
        ensure_equals(files[0].first, "x/a.lsl");

        files.clear();
        ensure("one top", readZip(makeZip({ { "repo-abc/a.lsl", "x" }, { "repo-abc/d/b.lsl", "y" } }), files, unsafe,
            error));
        ensure_equals(files[0].first, "a.lsl");
        ensure_equals(files[1].first, "d/b.lsl");
    }

    // local folders: parsing, keys and the state file
    template<> template<> void fsscriptrepos_object::test<17>()
    {
        RepoRef r;
#if LL_WINDOWS
        ensure("drive path", parseRepoRef("C:/Work/Scripts/", "", r));
        ensure_equals(r.path, "C:\\Work\\Scripts");
        ensure_equals(r.owner, "Work");
        RepoRef same;
        ensure("other spelling", parseRepoRef("c:\\work\\.\\scripts", "", same));
        ensure_equals(r.key(), same.key());     // Windows ignores case
        ensure("drive root refused", !parseRepoRef("C:\\", "", r));
        ensure("posix root is not absolute here", !parseRepoRef("/scripts", "", r));
#else
        ensure("root path", parseRepoRef("/home/me/Scripts/", "", r));
        ensure_equals(r.path, "/home/me/Scripts");
        ensure_equals(r.owner, "me");
        RepoRef same;
        ensure("other spelling", parseRepoRef("/home/me/./Scripts", "", same));
        ensure_equals(r.key(), same.key());
        ensure("root refused", !parseRepoRef("/", "", r));
#endif
        ensure("local kind", r.provider == Provider::LOCAL);
        ensure_equals(r.repo, "Scripts");
        ensure_equals(r.display(), r.path);
        ensure("no branch", r.ref.empty());

        const Provider local = Provider::LOCAL;
        ensure("forced local needs a path", !parseRepoRef("o/r", "", &local, r));
        ensure("forced local refuses links", !parseRepoRef("https://example.com/a.zip", "", &local, r));
        ensure("relative refused", !parseRepoRef("scripts", "", &local, r));
        ensure("empty refused", !parseLocalFolder("", r));

        RepoState a;
#if LL_WINDOWS
        ensure("state", parseRepoRef("D:\\lsl\\pack", "", a.ref));
#else
        ensure("state", parseRepoRef("/srv/lsl/pack", "", a.ref));
#endif
        a.commit = "md5:4567";
        std::vector<RepoState> back = fromLLSD(toLLSD({ a }));
        ensure_equals(back.size(), 1U);
        ensure("local back", back[0].ref.provider == Provider::LOCAL);
        ensure_equals(back[0].ref.path, a.ref.path);
        ensure_equals(back[0].ref.key(), a.ref.key());
        ensure_equals(back[0].commit, "md5:4567");

        // A local entry without a path is dropped, the others kept
        LLSD list = toLLSD({ a });
        list[0].erase("path");
        ensure("no path dropped", fromLLSD(list).empty());
    }

    // readFolder: files below the folder, hidden folders left out, hidden files listed unread, a stable version
    template<> template<> void fsscriptrepos_object::test<18>()
    {
        TempFolder folder;
        folder.write("a.lsl", "x");
        folder.write("sub/deeper/b.luau", "y");
        folder.write(".gitignore", "*.o");
        folder.write(".git/objects/c.lsl", "z");

        std::vector<std::pair<std::string, std::string>> files;
        std::vector<std::string> unsafe;
        std::string error;
        ensure("read", readFolder(folder.path(), files, unsafe, error));
        ensure_equals(files.size(), 3U);
        ensure_equals(files[0].first, ".gitignore");
        ensure("hidden file unread", files[0].second.empty());
        ensure_equals(files[1].first, "a.lsl");
        ensure_equals(files[1].second, "x");
        ensure_equals(files[2].first, "sub/deeper/b.luau");
        ensure_equals(files[2].second, "y");
        ensure("nothing unsafe", unsafe.empty());
        ensure("hidden file skipped", classify(files[0].first, files[0].second, {}, false).kind == Kind::SKIPPED);

        const std::string version = filesVersion(files);
        ensure("version form", version.compare(0, 4, "md5:") == 0);
        std::vector<std::pair<std::string, std::string>> again;
        ensure("read again", readFolder(folder.path(), again, unsafe, error));
        ensure_equals(filesVersion(again), version);
        folder.write("a.lsl", "changed");
        again.clear();
        ensure("read changed", readFolder(folder.path(), again, unsafe, error));
        ensure("version changed", filesVersion(again) != version);

        files.clear();
        ensure("missing folder", !readFolder(folder.path() + "_missing", files, unsafe, error));
        ensure("missing error", !error.empty());
        ensure("empty path", !readFolder("", files, unsafe, error));
    }
}
