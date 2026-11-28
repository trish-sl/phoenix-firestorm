/**
 * @file fscutoutmaterials_test.cpp
 * @brief Tests for the paper cutout material store (fscutoutmaterials.cpp).
 */

#include "../llviewerprecompiledheaders.h"

#include "../test/lltut.h"

#include "../fscutoutmaterials.h"

namespace tut
{
    struct fscutoutmaterials_data {};
    typedef test_group<fscutoutmaterials_data> fscutoutmaterials_group;
    typedef fscutoutmaterials_group::object fscutoutmaterials_object;
    tut::fscutoutmaterials_group fscutoutmaterials_testgroup("fscutoutmaterials");

    template<> template<> void fscutoutmaterials_object::test<1>()
    {
        set_test_name("pending_material_mesh_flow");
        FSCutoutMaterialStore s;
        LLUUID mat, m, m2;
        mat.generate(); m.generate(); m2.generate();
        s.addPending("A");
        ensure("no material yet", s.learnMesh("A", m) == nullptr);
        ensure("set material", s.setMaterial("A", mat));
        const FSCutoutMaterialStore::Entry* e = s.learnMesh("A", m);
        ensure("learned", e != nullptr);
        ensure_equals("mesh recorded", e->mesh_id, m);
        const FSCutoutMaterialStore::Entry* f = s.findByMesh(m);
        ensure("found", f != nullptr);
        ensure_equals("material", f->material_id, mat);
        ensure("second learn refused", s.learnMesh("A", m2) == nullptr);
        ensure("m2 unknown", s.findByMesh(m2) == nullptr);
    }

    template<> template<> void fscutoutmaterials_object::test<2>()
    {
        set_test_name("unknown_name");
        FSCutoutMaterialStore s;
        LLUUID mat;
        mat.generate();
        ensure("no entry", !s.setMaterial("B", mat));
        ensure("no learn", s.learnMesh("B", mat) == nullptr);
    }

    template<> template<> void fscutoutmaterials_object::test<3>()
    {
        set_test_name("cap_200");
        FSCutoutMaterialStore s;
        for (int i = 0; i < 205; ++i)
            s.addPending("N" + std::to_string(i));
        ensure_equals("size", s.asLLSD().size(), 200);
        LLUUID mat;
        mat.generate();
        ensure("N0 gone", !s.setMaterial("N0", mat));
        ensure("N4 gone", !s.setMaterial("N4", mat));
        ensure("N5 kept", s.setMaterial("N5", mat));
        ensure("N204 kept", s.setMaterial("N204", mat));
    }

    template<> template<> void fscutoutmaterials_object::test<4>()
    {
        set_test_name("trim_rebuilds_mesh_index");
        FSCutoutMaterialStore s;
        LLUUID mat, m0, mk;
        mat.generate(); m0.generate(); mk.generate();
        s.addPending("N0");
        s.setMaterial("N0", mat);
        s.learnMesh("N0", m0);
        s.addPending("Keep");
        s.setMaterial("Keep", mat);
        s.learnMesh("Keep", mk);
        for (int i = 0; i < 199; ++i)   // 201 entries: N0 trimmed, Keep shifts down one slot
            s.addPending("X" + std::to_string(i));
        ensure("trimmed entry unindexed", s.findByMesh(m0) == nullptr);
        const FSCutoutMaterialStore::Entry* e = s.findByMesh(mk);
        ensure("index survives trim", e != nullptr && e->name == "Keep");
    }

    template<> template<> void fscutoutmaterials_object::test<5>()
    {
        set_test_name("llsd_round_trip");
        FSCutoutMaterialStore s;
        LLUUID mat, m;
        mat.generate(); m.generate();
        s.addPending("A");
        s.setMaterial("A", mat);
        s.learnMesh("A", m);
        s.addPending("B");
        LLSD sd = s.asLLSD();
        sd.append(LLSD().with("material_id", mat));   // missing name
        FSCutoutMaterialStore t;
        t.addPending("old");
        t.fromLLSD(sd);
        ensure_equals("malformed skipped", t.asLLSD().size(), 2);
        ensure("old replaced", !t.setMaterial("old", mat));
        const FSCutoutMaterialStore::Entry* e = t.findByMesh(m);
        ensure("found", e != nullptr && e->name == "A" && e->material_id == mat);
        ensure("pending B survives", t.setMaterial("B", mat));
    }

    template<> template<> void fscutoutmaterials_object::test<6>()
    {
        set_test_name("llsd_skips_bad_ids");
        LLSD sd;
        LLSD row;
        row["name"] = "A";
        row["material_id"] = "not-a-uuid";
        sd.append(row);
        row["name"] = "";
        row["material_id"] = LLUUID::null;
        sd.append(row);
        FSCutoutMaterialStore s;
        s.fromLLSD(sd);
        ensure_equals("both skipped", s.asLLSD().size(), 0);
        s.fromLLSD(LLSD());
        ensure_equals("undefined ok", s.asLLSD().size(), 0);
    }

    template<> template<> void fscutoutmaterials_object::test<7>()
    {
        set_test_name("add_pending_existing_name_moves_to_newest");
        FSCutoutMaterialStore s;
        LLUUID mat;
        mat.generate();
        s.addPending("A");
        s.setMaterial("A", mat);
        s.addPending("B");
        s.addPending("A");
        LLSD sd = s.asLLSD();
        ensure_equals("single entry", sd.size(), 2);
        ensure_equals("A newest", sd[1]["name"].asString(), std::string("A"));
        ensure_equals("A keeps material", sd[1]["material_id"].asUUID(), mat);
    }

    template<> template<> void fscutoutmaterials_object::test<8>()
    {
        set_test_name("find_null_mesh");
        FSCutoutMaterialStore s;
        s.addPending("A");
        ensure("null mesh", s.findByMesh(LLUUID::null) == nullptr);
    }

    template<> template<> void fscutoutmaterials_object::test<9>()
    {
        set_test_name("apply_plan_skips_textured_faces");
        LLUUID mat, edge, x;
        mat.generate(); edge.generate(); x.generate();
        LLUUID cur[FSCutout::FACE_COUNT] = { LLUUID::null, x, LLUUID::null };
        auto plan = cutoutApplyPlan(cur, mat, LLUUID::null);
        ensure_equals("one", plan.size(), (size_t)1);
        ensure("front", plan[0].first == 0 && plan[0].second == mat);
        plan = cutoutApplyPlan(cur, mat, edge);
        ensure_equals("two", plan.size(), (size_t)2);
        ensure("edge", plan[1].first == 2 && plan[1].second == edge);
        LLUUID all[FSCutout::FACE_COUNT] = { x, x, x };
        ensure("all textured", cutoutApplyPlan(all, mat, edge).empty());
        LLUUID none[FSCutout::FACE_COUNT];
        plan = cutoutApplyPlan(none, mat, edge);
        ensure_equals("three", plan.size(), (size_t)3);
        ensure("order", plan[0].first == 0 && plan[1].first == 1 && plan[2].first == 2);
        ensure("edge id", plan[2].second == edge && plan[1].second == mat);
    }
}
