/**
 * @file fsscriptrepos_test.cpp
 * @brief Tests for the script repositories core (fsscriptrepos.cpp).
 */

#include "../llviewerprecompiledheaders.h"

#include "../test/lltut.h"

#include "../fsscriptrepos.h"

#include "minizip-ng/mz.h"
#include "minizip-ng/mz_strm.h"
#include "minizip-ng/mz_strm_mem.h"
#include "minizip-ng/mz_zip.h"
#include "minizip-ng/mz_zip_rw.h"

#include <cstring>

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

        ensure("other md", classify("docs/x.md", "x", {}, false).kind == Kind::SKIPPED);

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
        ensure("too big", classify("big.lsl", std::string(MAX_TEXT_BYTES + 1, 'a'), {}, false).kind == Kind::SKIPPED);
        ensure("max size ok", classify("max.lsl", std::string(MAX_TEXT_BYTES, 'a'), {}, false).kind == Kind::LSL);

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
        a.ref = { "o", "r", "main" };
        a.commit = "0123456789abcdef0123456789abcdef01234567";
        a.synced = LLDate(1763424000.0);
        a.files["x/a.lsl"] = { md5Hex("a"), LLUUID::generateNewID(), LLUUID::generateNewID(), true, true };
        a.files["y/fails.lsl"] = { md5Hex("f"), LLUUID::generateNewID(), LLUUID::generateNewID(), false, true };
        a.files["b.txt"] = { md5Hex("b"), LLUUID::generateNewID() };
        RepoState b;
        b.ref = { "p", "q", "" };

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
        ensure_equals(back[1].ref.owner, "p");
        ensure_equals(back[1].files.size(), 0U);
        ensure("empty llsd", fromLLSD(LLSD()).empty());
    }
}
