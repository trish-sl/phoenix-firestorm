/**
 * @file fscutoutmesh_test.cpp
 * @brief Tests for the paper cutout outline tracer and mesh builder (fscutoutmesh.cpp).
 */

#include "../llviewerprecompiledheaders.h"

#include "../test/lltut.h"

#include "../fscutoutmesh.h"

#include <cmath>
#include <iostream>
#include <map>

// Not "using namespace FSCutout": its Polygon clashes with the Windows GDI Polygon().
using FSCutout::TraceParams;
using FSCutout::Outline;
using FSCutout::traceOutlines;
using FSCutout::polygonArea;
using FSCutout::polygonSelfIntersects;
using FSCutout::Mesh;
using FSCutout::buildMesh;
using FSCutout::FACE_FRONT;
using FSCutout::FACE_BACK;
using FSCutout::FACE_EDGE;
using FSCutout::FACE_COUNT;
using FSCutout::Rect;
using FSCutout::hasUsableAlpha;
using FSCutout::alphaBounds;
using FSCutout::flattenOnto;
typedef FSCutout::Polygon Poly;

namespace
{
    const F32 PI_F = 3.14159265f;

    struct Img
    {
        S32 w, h;
        std::vector<U8> px;
        Img(S32 w_, S32 h_) : w(w_), h(h_), px((size_t)w_ * h_ * 4, 0) {}
        void set(S32 x, S32 y) { px[((size_t)y * w + x) * 4 + 3] = 255; }
        void rect(S32 x0, S32 y0, S32 x1, S32 y1)  // [x0,x1) x [y0,y1)
        {
            for (S32 y = y0; y < y1; ++y)
                for (S32 x = x0; x < x1; ++x)
                    set(x, y);
        }
        void clear(S32 x0, S32 y0, S32 x1, S32 y1)  // [x0,x1) x [y0,y1)
        {
            for (S32 y = y0; y < y1; ++y)
                for (S32 x = x0; x < x1; ++x)
                    px[((size_t)y * w + x) * 4 + 3] = 0;
        }
        void disc(F32 cx, F32 cy, F32 rmin, F32 rmax)
        {
            for (S32 y = 0; y < h; ++y)
                for (S32 x = 0; x < w; ++x)
                {
                    F32 d = std::sqrt((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
                    if (d >= rmin && d <= rmax)
                        set(x, y);
                }
        }
        std::vector<Outline> trace(const TraceParams& p = TraceParams()) const { return traceOutlines(px.data(), w, h, p); }
    };

    // What the triangulation downstream needs: at least 3 points, no repeated neighbours.
    void ensure_usable(const Poly& poly)
    {
        tut::ensure("at least 3 points", poly.size() >= 3);
        for (size_t i = 0; i < poly.size(); ++i)
            tut::ensure("no duplicate neighbour", poly[i] != poly[(i + 1) % poly.size()]);
    }

    // One solid piece without holes.
    std::vector<Outline> one(const Poly& poly)
    {
        return { Outline{ poly, {} } };
    }

    bool inside(const LLVector2& p, const Poly& poly)
    {
        bool in = false;
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        {
            const LLVector2 &a = poly[i], &b = poly[j];
            if ((a[VY] > p[VY]) != (b[VY] > p[VY]) && p[VX] < (b[VX] - a[VX]) * (p[VY] - a[VY]) / (b[VY] - a[VY]) + a[VX])
                in = !in;
        }
        return in;
    }

    bool near_frac(F32 actual, F32 expected, F32 frac)
    {
        return std::fabs(actual - expected) <= frac * expected;
    }

    // Pixel outlines in the tracer's orientation (polygonArea > 0).
    const Poly SQUARE = { LLVector2(0, 0), LLVector2(100, 0), LLVector2(100, 100), LLVector2(0, 100) };
    const Poly L_SHAPE = { LLVector2(40, 30), LLVector2(80, 30), LLVector2(80, 130),
                           LLVector2(160, 130), LLVector2(160, 170), LLVector2(40, 170) };   // in 200 x 200
    Poly ngon(S32 n, F32 cx, F32 cy, F32 r)
    {
        Poly poly;
        for (S32 i = 0; i < n; ++i)
            poly.push_back(LLVector2(cx + r * std::cos(2 * PI_F * i / n), cy + r * std::sin(2 * PI_F * i / n)));
        return poly;
    }

    LLVector3 tri_cross(const Mesh& m, S32 face, size_t t)
    {
        const std::vector<LLVector3>& p = m.positions[face];
        const std::vector<U32>& ix = m.indices[face];
        return (p[ix[t + 1]] - p[ix[t]]) % (p[ix[t + 2]] - p[ix[t]]);
    }

    F32 tri_min_angle_deg(const Mesh& m, S32 face, size_t t)
    {
        F32 best = 180.f;
        for (S32 k = 0; k < 3; ++k)
        {
            LLVector3 o = m.positions[face][m.indices[face][t + k]];
            LLVector3 a = m.positions[face][m.indices[face][t + (k + 1) % 3]] - o;
            LLVector3 b = m.positions[face][m.indices[face][t + (k + 2) % 3]] - o;
            best = llmin(best, std::atan2((a % b).length(), a * b) * 180.f / PI_F);
        }
        return best;
    }

    // Crude standing avatar in a w x h picture: head, neck, torso, slanted arms, two long straight legs apart
    // (long Douglas-Peucker segments next to short ones), feet. Same shape as the reviewer's Python probe.
    Img avatar(S32 w, S32 h)
    {
        Img img(w, h);
        const F32 H = (F32)h, cx = w * 0.5f;
        auto sq = [](F32 v) { return v * v; };
        for (S32 py = 0; py < h; ++py)
            for (S32 px = 0; px < w; ++px)
            {
                const F32 x = px + 0.5f, y = py + 0.5f;
                bool in = sq(x - cx) + sq(y - 0.10f * H) < sq(0.06f * H)                        // head
                       || (std::fabs(x - cx) < 0.025f * H && y > 0.15f * H && y < 0.2f * H);   // neck
                if (y > 0.19f * H && y < 0.52f * H)                                             // torso
                    in = in || std::fabs(x - cx) < 0.11f * H - 0.03f * H * std::sin((y - 0.19f * H) / (0.33f * H) * PI_F);
                for (F32 side : { -1.f, 1.f })
                {
                    const F32 ax = cx + side * 0.10f * H, ay = 0.21f * H;                       // arms
                    const F32 dx = side * 0.07f * H, dy = 0.34f * H;
                    const F32 t = llclamp(((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy), 0.f, 1.f);
                    in = in || sq(x - ax - t * dx) + sq(y - ay - t * dy) < sq(0.028f * H);
                    const F32 lx = cx + side * 0.05f * H;                                       // legs
                    in = in || (y > 0.5f * H && y < 0.97f * H &&
                                std::fabs(x - lx - side * 0.02f * H * (y - 0.5f * H) / (0.47f * H)) < 0.035f * H);
                    const F32 fx = side * (x - cx) - 0.03f * H;                                 // feet
                    in = in || (y > 0.95f * H && y < 0.985f * H && fx > 0.f && fx < 0.13f * H);
                }
                if (in)
                    img.set(px, py);
            }
        return img;
    }

    // Interior angle in degrees at each outline corner.
    std::vector<F32> corner_angles_deg(const Poly& poly)
    {
        std::vector<F32> out;
        const size_t n = poly.size();
        const bool ccw = polygonArea(poly) > 0.f;
        for (size_t i = 0; i < n; ++i)
        {
            const LLVector2 a = poly[(i + n - 1) % n] - poly[i], b = poly[(i + 1) % n] - poly[i];
            const F32 cr = a[VX] * b[VY] - a[VY] * b[VX];
            F32 ang = std::atan2(std::fabs(cr), a * b) * 180.f / PI_F;
            if ((cr < 0.f) != ccw)   // seen from inside, the turn from a to b goes the long way: a reflex corner
                ang = 360.f - ang;
            out.push_back(ang);
        }
        return out;
    }

    // Weld positions of all faces and count the triangles on each undirected edge.
    bool is_watertight(const Mesh& m)
    {
        std::vector<LLVector3> welded;
        std::map<std::pair<U32, U32>, S32> uses;
        for (S32 f = 0; f < FACE_COUNT; ++f)
        {
            std::vector<U32> id;
            for (const LLVector3& p : m.positions[f])
            {
                U32 k = 0;
                while (k < welded.size() && (welded[k] - p).length() > 1e-5f)
                    ++k;
                if (k == welded.size())
                    welded.push_back(p);
                id.push_back(k);
            }
            for (size_t t = 0; t < m.indices[f].size(); t += 3)
                for (S32 e = 0; e < 3; ++e)
                {
                    U32 a = id[m.indices[f][t + e]], b = id[m.indices[f][t + (e + 1) % 3]];
                    ++uses[std::make_pair(llmin(a, b), llmax(a, b))];
                }
        }
        for (const auto& u : uses)
            if (u.second != 2)
                return false;
        return !uses.empty();
    }
}

namespace tut
{
    struct fscutoutmesh_data {};
    typedef test_group<fscutoutmesh_data> fscutoutmesh_group;
    typedef fscutoutmesh_group::object fscutoutmesh_object;
    tut::fscutoutmesh_group fscutoutmesh_testgroup("fscutoutmesh");

    template<> template<> void fscutoutmesh_object::test<1>()
    {
        set_test_name("filled_circle");
        Img img(200, 200);
        img.disc(100, 100, 0, 60);
        auto polys = img.trace();
        ensure_equals("one polygon", polys.size(), (size_t)1);
        ensure("simple", !polygonSelfIntersects(polys[0].outer));
        ensure_usable(polys[0].outer);
        ensure("ccw", polygonArea(polys[0].outer) > 0.f);
        ensure("area", near_frac(std::fabs(polygonArea(polys[0].outer)), PI_F * 60 * 60, 0.05f));
    }

    template<> template<> void fscutoutmesh_object::test<2>()
    {
        set_test_name("margin_grows_outline");
        Img img(200, 200);
        img.disc(100, 100, 0, 60);
        TraceParams p;
        p.margin_px = 6.f;
        auto polys = img.trace(p);
        ensure_equals("one polygon", polys.size(), (size_t)1);
        ensure("area", near_frac(std::fabs(polygonArea(polys[0].outer)), PI_F * 66 * 66, 0.05f));
    }

    template<> template<> void fscutoutmesh_object::test<3>()
    {
        set_test_name("ring_holes_filled");
        Img img(200, 200);
        img.disc(100, 100, 30, 60);
        TraceParams p;
        p.cut_holes = false;
        auto polys = img.trace(p);
        ensure_equals("one polygon", polys.size(), (size_t)1);
        ensure("no holes", polys[0].holes.empty());
        ensure("area", near_frac(std::fabs(polygonArea(polys[0].outer)), PI_F * 60 * 60, 0.05f));
    }

    template<> template<> void fscutoutmesh_object::test<4>()
    {
        set_test_name("l_shape");
        Img img(200, 200);
        img.rect(40, 30, 80, 170);      // 40 x 140
        img.rect(80, 130, 160, 170);    // 80 x 40
        auto polys = img.trace();
        ensure_equals("one polygon", polys.size(), (size_t)1);
        ensure_usable(polys[0].outer);
        ensure("area", near_frac(std::fabs(polygonArea(polys[0].outer)), 40.f * 140 + 80 * 40, 0.03f));
    }

    template<> template<> void fscutoutmesh_object::test<5>()
    {
        set_test_name("two_islands_and_speck");
        Img img(300, 200);
        img.rect(20, 40, 60, 120);
        img.rect(220, 40, 260, 120);
        img.rect(140, 10, 142, 12);
        auto polys = img.trace();
        ensure_equals("two polygons", polys.size(), (size_t)2);
        for (const Outline& ol : polys)
            ensure_usable(ol.outer);
    }

    template<> template<> void fscutoutmesh_object::test<6>()
    {
        set_test_name("border_touching_shape_closes");
        Img img(100, 80);
        img.rect(0, 30, 50, 80);        // touches left and bottom edges
        // The margin must not grow the outline past the picture: it is cut straight along the frame.
        for (F32 margin : { 0.f, 6.f })
        {
            TraceParams p;
            p.margin_px = margin;
            auto polys = img.trace(p);
            ensure_equals("one polygon", polys.size(), (size_t)1);
            ensure("simple", !polygonSelfIntersects(polys[0].outer));
            ensure_usable(polys[0].outer);
            for (const LLVector2& v : polys[0].outer)
            {
                ensure("x in [0,w]", v[VX] >= 0.f && v[VX] <= 100.f);
                ensure("y in [0,h]", v[VY] >= 0.f && v[VY] <= 80.f);
            }
            for (const LLVector2& uv : buildMesh(polys, 100, 80, 2.f, 0.03f, 0.1f).uvs[FACE_FRONT])
                ensure("front uv in [0,1]", uv[VX] >= 0.f && uv[VX] <= 1.f && uv[VY] >= 0.f && uv[VY] <= 1.f);
        }
    }

    template<> template<> void fscutoutmesh_object::test<7>()
    {
        set_test_name("comb_shape_no_self_intersection");
        Img img(120, 80);
        img.rect(5, 50, 115, 70);                       // spine
        for (S32 i = 0; i < 10; ++i)
            img.rect(10 + i * 5, 10, 13 + i * 5, 50);   // 3 px teeth, 2 px gaps
        auto polys = img.trace();
        ensure("has polygon", !polys.empty());
        for (const Outline& ol : polys)
        {
            ensure("simple", !polygonSelfIntersects(ol.outer));
            ensure_usable(ol.outer);
        }
    }

    template<> template<> void fscutoutmesh_object::test<8>()
    {
        set_test_name("empty_image");
        Img img(64, 64);
        ensure_equals("no polygons", img.trace().size(), (size_t)0);
    }

    template<> template<> void fscutoutmesh_object::test<9>()
    {
        set_test_name("polygonSelfIntersects_detects_crossings");
        Poly square = { LLVector2(0, 0), LLVector2(4, 0), LLVector2(4, 4), LLVector2(0, 4) };
        ensure("square is simple", !polygonSelfIntersects(square));
        Poly bowtie = { LLVector2(0, 0), LLVector2(4, 4), LLVector2(4, 0), LLVector2(0, 4) };
        ensure("bowtie crosses", polygonSelfIntersects(bowtie));
        // vertex (2,0) touches the non-adjacent edge (0,0)-(4,0)
        Poly touch = { LLVector2(0, 0), LLVector2(4, 0), LLVector2(4, 4), LLVector2(2, 0), LLVector2(0, 4) };
        ensure("T-touch counts", polygonSelfIntersects(touch));
    }

    template<> template<> void fscutoutmesh_object::test<10>()
    {
        set_test_name("unfixable_island_is_dropped");
        // Two pixels meeting only at a corner: the raw outline pinches itself. With no smoothing it stays
        // that way, so the island must be dropped rather than returned self-touching.
        Img img(16, 16);
        img.set(5, 5);
        img.set(6, 6);
        TraceParams p;
        p.smooth_passes = 0;
        p.simplify_px = 0.f;
        p.min_island_frac = 0.f;
        auto polys = img.trace(p);
        for (const Outline& ol : polys)
            ensure("returned outlines are simple", !polygonSelfIntersects(ol.outer));
        ensure_equals("dropped", polys.size(), (size_t)0);
    }

    template<> template<> void fscutoutmesh_object::test<11>()
    {
        set_test_name("square_mesh_counts");
        Mesh m = buildMesh(one(SQUARE), 100, 100, 2.f, 0.03f, 0.1f);
        ensure_equals("front tris", m.indices[FACE_FRONT].size(), (size_t)6);
        ensure_equals("back tris", m.indices[FACE_BACK].size(), (size_t)6);
        ensure_equals("edge tris", m.indices[FACE_EDGE].size(), (size_t)24);
        for (const LLVector2& uv : m.uvs[FACE_FRONT])
            ensure("front uv in [0,1]", uv[VX] >= 0.f && uv[VX] <= 1.f && uv[VY] >= 0.f && uv[VY] <= 1.f);
        ensure("back uvs == front uvs", m.uvs[FACE_BACK] == m.uvs[FACE_FRONT]);
    }

    template<> template<> void fscutoutmesh_object::test<12>()
    {
        set_test_name("l_shape_triangulation_area");
        Mesh m = buildMesh(one(L_SHAPE), 200, 200, 2.f, 0.03f, 0.1f);
        const F32 s = 2.f / 200;
        F32 area = 0.f;
        for (size_t t = 0; t < m.indices[FACE_FRONT].size(); t += 3)
            area += 0.5f * tri_cross(m, FACE_FRONT, t).length();
        ensure("area", near_frac(area, std::fabs(polygonArea(L_SHAPE)) * s * s, 0.01f));
    }

    template<> template<> void fscutoutmesh_object::test<13>()
    {
        set_test_name("watertight");
        ensure("square", is_watertight(buildMesh(one(SQUARE), 100, 100, 2.f, 0.03f, 0.1f)));
        ensure("L", is_watertight(buildMesh(one(L_SHAPE), 200, 200, 2.f, 0.03f, 0.1f)));
        ensure("64-gon", is_watertight(buildMesh(one(ngon(64, 100, 100, 100)), 200, 200, 2.f, 0.03f, 0.1f)));
    }

    template<> template<> void fscutoutmesh_object::test<14>()
    {
        set_test_name("normals_face_out");
        Mesh m = buildMesh(one(SQUARE), 100, 100, 2.f, 0.03f, 0.1f);
        ensure("has front", !m.normals[FACE_FRONT].empty());
        for (const LLVector3& n : m.normals[FACE_FRONT])
            ensure("front +Z", (n - LLVector3(0, 0, 1)).length() < 1e-6f);
        for (const LLVector3& n : m.normals[FACE_BACK])
            ensure("back -Z", (n - LLVector3(0, 0, -1)).length() < 1e-6f);
        ensure("has edge", !m.normals[FACE_EDGE].empty());
        LLVector3 centroid;
        for (const LLVector3& p : m.positions[FACE_EDGE])
            centroid += p;
        centroid /= (F32)m.positions[FACE_EDGE].size();
        for (size_t i = 0; i < m.normals[FACE_EDGE].size(); ++i)
        {
            const LLVector3& n = m.normals[FACE_EDGE][i];
            ensure("edge z = 0", n[VZ] == 0.f);
            ensure("edge outward", n * (m.positions[FACE_EDGE][i] - centroid) > 0.f);
        }
    }

    template<> template<> void fscutoutmesh_object::test<15>()
    {
        set_test_name("edge_u_spacing");
        Mesh m = buildMesh(one(SQUARE), 100, 100, 1.f, 0.03f, 0.1f);
        F32 max_u = 0.f;
        for (const LLVector2& uv : m.uvs[FACE_EDGE])
            max_u = llmax(max_u, uv[VX]);
        ensure("max u", std::fabs(max_u - 40.f) <= 1e-4f);
    }

    template<> template<> void fscutoutmesh_object::test<16>()
    {
        set_test_name("caps_do_not_share_edge_vertices");
        Mesh m = buildMesh(one(L_SHAPE), 200, 200, 2.f, 0.03f, 0.1f);
        for (S32 f = 0; f < FACE_COUNT; ++f)
        {
            ensure("has vertices", !m.positions[f].empty());
            ensure("normals per vertex", m.normals[f].size() == m.positions[f].size());
            ensure("uvs per vertex", m.uvs[f].size() == m.positions[f].size());
            for (U32 i : m.indices[f])
                ensure("index inside own stream", i < m.positions[f].size());
        }
        for (const LLVector3& n : m.normals[FACE_FRONT])
            ensure("front exactly +Z", n == LLVector3(0, 0, 1));
        for (const LLVector3& n : m.normals[FACE_BACK])
            ensure("back exactly -Z", n == LLVector3(0, 0, -1));
        for (const LLVector3& n : m.normals[FACE_EDGE])
            ensure("edge never a cap normal", n[VZ] == 0.f);
    }

    template<> template<> void fscutoutmesh_object::test<17>()
    {
        set_test_name("edge_normals_crease_30");
        // Edge vertices grouped by position: one group per corner per cap ring.
        auto groups = [](const Mesh& m)
        {
            std::vector<std::vector<size_t>> g;
            for (size_t i = 0; i < m.positions[FACE_EDGE].size(); ++i)
            {
                size_t k = 0;
                while (k < g.size() && (m.positions[FACE_EDGE][g[k][0]] - m.positions[FACE_EDGE][i]).length() > 1e-5f)
                    ++k;
                if (k == g.size())
                    g.emplace_back();
                g[k].push_back(i);
            }
            return g;
        };
        Mesh sq = buildMesh(one(SQUARE), 100, 100, 2.f, 0.03f, 0.1f);
        auto sg = groups(sq);
        ensure_equals("square corners x 2 rings", sg.size(), (size_t)8);
        for (const auto& g : sg)
        {
            ensure_equals("split corner", g.size(), (size_t)2);
            ensure("different normals", (sq.normals[FACE_EDGE][g[0]] - sq.normals[FACE_EDGE][g[1]]).length() > 0.5f);
        }

        Mesh circle = buildMesh(one(ngon(64, 100, 100, 100)), 200, 200, 2.f, 0.03f, 0.1f);
        auto cg = groups(circle);
        ensure_equals("64 corners x 2 rings", cg.size(), (size_t)128);
        const LLVector3 centre(0.f, 1.f, 0.f);   // pixel (100, 100) in metres
        for (const auto& g : cg)
        {
            LLVector3 radial = circle.positions[FACE_EDGE][g[0]] - centre;
            radial.mV[VZ] = 0.f;
            radial.normalize();
            for (size_t i : g)
            {
                ensure("one normal per ring vertex", (circle.normals[FACE_EDGE][i] - circle.normals[FACE_EDGE][g[0]]).length() < 1e-6f);
                ensure("radial", (circle.normals[FACE_EDGE][i] - radial).length() < 1e-3f);
            }
        }
    }

    template<> template<> void fscutoutmesh_object::test<18>()
    {
        set_test_name("cap_triangles_even");
        const Poly shapes[] = { ngon(64, 100, 100, 100), L_SHAPE };
        for (const Poly& shape : shapes)
        {
            Mesh m = buildMesh(one(shape), 200, 200, 2.f, 0.03f, 0.1f);
            const size_t tris = m.indices[FACE_FRONT].size() / 3;
            ensure("has triangles", tris > 0);
            ensure("bounded triangle count", tris <= 4 * shape.size());
            for (size_t t = 0; t < m.indices[FACE_FRONT].size(); t += 3)
            {
                ensure("smallest angle >= 20 deg", tri_min_angle_deg(m, FACE_FRONT, t) >= 20.f);
                ensure("not degenerate", 0.5f * tri_cross(m, FACE_FRONT, t).length() >= 1e-6f);
            }
        }
    }

    template<> template<> void fscutoutmesh_object::test<19>()
    {
        set_test_name("cap_winding");
        // glTF front faces are CCW: front caps CCW seen from +Z, back caps CCW seen from -Z.
        Mesh sq = buildMesh(one(SQUARE), 100, 100, 2.f, 0.03f, 0.1f);
        Mesh l = buildMesh(one(L_SHAPE), 200, 200, 2.f, 0.03f, 0.1f);
        for (const Mesh* m : { &sq, &l })
        {
            ensure("has front", !m->indices[FACE_FRONT].empty());
            ensure("has back", !m->indices[FACE_BACK].empty());
            for (size_t t = 0; t < m->indices[FACE_FRONT].size(); t += 3)
                ensure("front CCW from +Z", tri_cross(*m, FACE_FRONT, t)[VZ] > 0.f);
            for (size_t t = 0; t < m->indices[FACE_BACK].size(); t += 3)
                ensure("back CW from +Z", tri_cross(*m, FACE_BACK, t)[VZ] < 0.f);
        }
    }

    template<> template<> void fscutoutmesh_object::test<20>()
    {
        set_test_name("insert_cap_keeps_triangle_budget");
        // Fine outline, so refinement inserts close to n points. Front triangles = n - 2 + 2 * inserted.
        const Poly circle = ngon(256, 210, 110, 100);
        Mesh m = buildMesh(one(circle), 420, 220, 2.f, 0.03f, 0.1f);
        const size_t n = circle.size();
        ensure("front triangles <= 4n", m.indices[FACE_FRONT].size() / 3 <= 4 * n);
        ensure("watertight", is_watertight(m));
    }
    template<> template<> void fscutoutmesh_object::test<21>()
    {
        set_test_name("has_usable_alpha");
        Img img(4, 4);
        ensure("3 components", !hasUsableAlpha(img.px.data(), 4, 4, 3));
        ensure("all alpha 0", !hasUsableAlpha(img.px.data(), 4, 4, 4));
        img.rect(0, 0, 4, 4);
        ensure("all alpha 255", !hasUsableAlpha(img.px.data(), 4, 4, 4));
        img.px[3] = 0;
        ensure("mixed", hasUsableAlpha(img.px.data(), 4, 4, 4));
    }

    template<> template<> void fscutoutmesh_object::test<22>()
    {
        set_test_name("alpha_bounds_margin");
        Img img(200, 200);
        img.rect(50, 50, 60, 60);
        Rect r = alphaBounds(img.px.data(), 200, 200, 128, 5);
        ensure_equals("x", r.x, 45);
        ensure_equals("y", r.y, 45);
        ensure_equals("w", r.w, 20);
        ensure_equals("h", r.h, 20);
        Img corner(200, 200);
        corner.rect(0, 0, 10, 10);
        r = alphaBounds(corner.px.data(), 200, 200, 128, 5);
        ensure_equals("corner x", r.x, 0);
        ensure_equals("corner y", r.y, 0);
        ensure_equals("corner w", r.w, 15);
        ensure_equals("corner h", r.h, 15);
        Img empty(8, 8);
        r = alphaBounds(empty.px.data(), 8, 8, 128, 5);
        ensure("empty is zero", r.x == 0 && r.y == 0 && r.w == 0 && r.h == 0);
    }

    template<> template<> void fscutoutmesh_object::test<23>()
    {
        set_test_name("flatten_half_alpha_red_on_white");
        const U8 px[3 * 4] = { 255, 0, 0, 128,   10, 20, 30, 0,   40, 50, 60, 255 };
        std::vector<U8> out;
        flattenOnto(px, 3, 1, LLColor3(1.f, 1.f, 1.f), out);
        ensure_equals("size", out.size(), (size_t)9);
        ensure("r within 1", std::abs((S32)out[0] - 255) <= 1);
        ensure("g within 1", std::abs((S32)out[1] - 127) <= 1);
        ensure("b within 1", std::abs((S32)out[2] - 127) <= 1);
        ensure("alpha 0 is paper", out[3] == 255 && out[4] == 255 && out[5] == 255);
        ensure("alpha 255 unchanged", out[6] == 40 && out[7] == 50 && out[8] == 60);
    }

    template<> template<> void fscutoutmesh_object::test<24>()
    {
        set_test_name("edge_v_is_thickness_over_tile");
        Mesh m = buildMesh(one(SQUARE), 100, 100, 1.f, 0.03f, 0.27f);
        ensure("has edge", !m.uvs[FACE_EDGE].empty());
        for (size_t i = 0; i < m.uvs[FACE_EDGE].size(); ++i)
        {
            const bool front = m.positions[FACE_EDGE][i][VZ] > 0.f;
            const F32 expected = front ? 0.f : 0.03f / 0.27f;
            ensure("v front 0, back thickness/tile", std::fabs(m.uvs[FACE_EDGE][i][VY] - expected) <= 1e-4f);
        }
    }

    template<> template<> void fscutoutmesh_object::test<25>()
    {
        set_test_name("realistic_avatar_cap_triangles_even");
        // A traced 1024 px avatar outline has straight legs ~436 px long next to 3 px segments: refinement must
        // split outline segments to fix the triangles beside them. Triangles touching an outline corner sharper
        // than the 20 deg target could not be fixed by any triangulation and are exempt (counted; none here).
        const S32 W = 520, H = 1024;
        const Img img = avatar(W, H);
        for (F32 margin : { 0.f, 15.f })   // 15 px = the default 1.5 % margin at 1024 px
        {
            TraceParams p;
            p.margin_px = margin;
            const std::vector<Outline> polys = img.trace(p);
            ensure_equals("one outline", polys.size(), (size_t)1);
            const Poly& outline = polys[0].outer;
            size_t boundary = outline.size();
            for (const Poly& hole : polys[0].holes)
                boundary += hole.size();
            const std::vector<F32> corners = corner_angles_deg(outline);
            const F32 s = 2.f / H;
            std::vector<LLVector3> sharp;
            F32 min_corner = 360.f;
            for (size_t i = 0; i < outline.size(); ++i)
            {
                min_corner = llmin(min_corner, corners[i]);
                if (corners[i] < 20.f)
                    sharp.push_back(LLVector3((outline[i][VX] - W * 0.5f) * s, (H - outline[i][VY]) * s, 0.f));
            }

            const Mesh m = buildMesh(polys, W, H, 2.f, 0.03f, 0.27f);
            const size_t tris = m.indices[FACE_FRONT].size() / 3;
            F32 min_angle = 180.f;
            size_t exempt = 0, below20 = 0;
            for (size_t t = 0; t < m.indices[FACE_FRONT].size(); t += 3)
            {
                bool at_sharp = false;
                for (S32 k = 0; k < 3; ++k)
                {
                    LLVector3 v = m.positions[FACE_FRONT][m.indices[FACE_FRONT][t + k]];
                    v.mV[VZ] = 0.f;
                    for (const LLVector3& c : sharp)
                        at_sharp = at_sharp || (v - c).length() < 1e-4f;
                }
                ensure("not degenerate", 0.5f * tri_cross(m, FACE_FRONT, t).length() >= 1e-6f);
                if (at_sharp)
                {
                    ++exempt;
                    continue;
                }
                const F32 ang = tri_min_angle_deg(m, FACE_FRONT, t);
                min_angle = llmin(min_angle, ang);
                below20 += ang < 20.f ? 1 : 0;
            }
            std::cerr << "realistic avatar, margin " << margin << " px: outline " << outline.size() << " pts (sharpest corner "
                      << min_corner << " deg), holes " << polys[0].holes.size() << ", front tris " << tris << " (4n = " << 4 * boundary << "), min angle "
                      << min_angle << " deg, " << below20 << " below 20 deg, " << exempt << " exempt" << std::endl;
            ensure("smallest angle >= 15 deg", min_angle >= 15.f);
            ensure("front triangles <= 4n", tris <= 4 * boundary);
            ensure("watertight", is_watertight(m));
            for (const LLVector3& n : m.normals[FACE_FRONT])
                ensure("front exactly +Z", n == LLVector3(0, 0, 1));
            for (const LLVector3& n : m.normals[FACE_BACK])
                ensure("back exactly -Z", n == LLVector3(0, 0, -1));
            for (const LLVector3& n : m.normals[FACE_EDGE])
                ensure("edge normal flat", n[VZ] == 0.f && std::fabs(n.length() - 1.f) < 1e-4f);
            // Every edge vertex sits on a cap vertex: no T-junction where the caps meet the edge.
            for (const LLVector3& e : m.positions[FACE_EDGE])
            {
                bool on_cap = false;
                const std::vector<LLVector3>& cap = m.positions[e[VZ] > 0.f ? FACE_FRONT : FACE_BACK];
                for (size_t i = 0; i < cap.size() && !on_cap; ++i)
                    on_cap = (cap[i] - e).length() < 1e-6f;
                ensure("edge vertex on cap", on_cap);
            }
        }
    }

    template<> template<> void fscutoutmesh_object::test<26>()
    {
        set_test_name("upload_texture_size_rounds_up_to_power_of_two");
        S32 w = 0, h = 0;
        FSCutout::uploadTextureSize(290, 1024, 2048, w, h);
        ensure_equals("narrow w", w, 512);
        ensure_equals("narrow h", h, 1024);
        FSCutout::uploadTextureSize(1160, 4096, 2048, w, h);
        ensure_equals("big w", w, 1024);
        ensure_equals("big h", h, 2048);
        FSCutout::uploadTextureSize(512, 512, 2048, w, h);
        ensure_equals("pow2 w", w, 512);
        ensure_equals("pow2 h", h, 512);
    }

    template<> template<> void fscutoutmesh_object::test<27>()
    {
        set_test_name("ring_hole_cut");
        Img img(200, 200);
        img.disc(100, 100, 30, 60);
        TraceParams p;
        p.margin_px = 3.f;
        auto polys = img.trace(p);
        ensure_equals("one outline", polys.size(), (size_t)1);
        ensure_equals("one hole", polys[0].holes.size(), (size_t)1);
        const Poly& hole = polys[0].holes[0];
        ensure_usable(hole);
        ensure("hole simple", !polygonSelfIntersects(hole));
        ensure("outer ccw", polygonArea(polys[0].outer) > 0.f);
        ensure("hole cw", polygonArea(hole) < 0.f);
        ensure("outer area", near_frac(polygonArea(polys[0].outer), PI_F * 63 * 63, 0.05f));
        ensure("hole area shrunk by the margin", near_frac(-polygonArea(hole), PI_F * 27 * 27, 0.08f));
        for (const LLVector2& v : hole)
            ensure("hole inside outer", inside(v, polys[0].outer));
    }

    template<> template<> void fscutoutmesh_object::test<28>()
    {
        set_test_name("small_hole_filled");
        Img img(200, 200);
        img.rect(40, 40, 160, 160);
        img.clear(98, 98, 102, 102);    // 16 px, below 0.05 % of 40000 = 20 px
        auto polys = img.trace();
        ensure_equals("one outline", polys.size(), (size_t)1);
        ensure("filled", polys[0].holes.empty());
        TraceParams p;
        p.min_hole_frac = 0.0003f;      // 12 px: now it is kept
        polys = img.trace(p);
        ensure_equals("one outline, lower threshold", polys.size(), (size_t)1);
        ensure_equals("kept", polys[0].holes.size(), (size_t)1);
    }

    template<> template<> void fscutoutmesh_object::test<29>()
    {
        set_test_name("legs_frame_hole_mesh");
        // Two legs joined at the hips and at the touching feet: a "0" frame with one enclosed gap.
        const S32 W = 300, H = 400;
        Img img(W, H);
        img.rect(50, 40, 250, 360);
        img.clear(110, 100, 190, 300);
        const F32 s = 2.f / H;
        for (F32 margin : { 0.f, 6.f })
        {
            TraceParams p;
            p.margin_px = margin;
            const std::vector<Outline> polys = img.trace(p);
            ensure_equals("one outline", polys.size(), (size_t)1);
            ensure_equals("one hole", polys[0].holes.size(), (size_t)1);
            const Poly& hole = polys[0].holes[0];
            ensure("hole cw", polygonArea(hole) < 0.f);
            ensure("hole simple", !polygonSelfIntersects(hole));
            const size_t boundary = polys[0].outer.size() + hole.size();

            const Mesh m = buildMesh(polys, W, H, 2.f, 0.03f, 0.27f);
            const size_t tris = m.indices[FACE_FRONT].size() / 3;
            ensure("has triangles", tris > 0);
            F32 min_angle = 180.f;
            for (size_t t = 0; t < m.indices[FACE_FRONT].size(); t += 3)
            {
                ensure("not degenerate", 0.5f * tri_cross(m, FACE_FRONT, t).length() >= 1e-6f);
                min_angle = llmin(min_angle, tri_min_angle_deg(m, FACE_FRONT, t));
                LLVector3 c;
                for (S32 k = 0; k < 3; ++k)
                    c += m.positions[FACE_FRONT][m.indices[FACE_FRONT][t + k]];
                c /= 3.f;
                ensure("no front triangle inside the hole", !inside(LLVector2(c[VX] / s + W * 0.5f, H - c[VY] / s), hole));
            }
            std::cerr << "legs frame, margin " << margin << " px: outer " << polys[0].outer.size() << " + hole " << hole.size()
                      << " pts, front tris " << tris << " (4n = " << 4 * boundary << "), min angle " << min_angle << " deg"
                      << std::endl;
            ensure("smallest angle >= 20 deg", min_angle >= 20.f);
            ensure("front triangles <= 4n", tris <= 4 * boundary);
            ensure("watertight", is_watertight(m));
            for (const LLVector3& n : m.normals[FACE_FRONT])
                ensure("front exactly +Z", n == LLVector3(0, 0, 1));
            for (const LLVector3& n : m.normals[FACE_BACK])
                ensure("back exactly -Z", n == LLVector3(0, 0, -1));

            // Hole wall: edge vertices on the hole outline, normals pointing into the hole (away from the cardboard).
            F32 x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
            for (const LLVector2& v : hole)
            {
                x0 = llmin(x0, v[VX]);
                x1 = llmax(x1, v[VX]);
                y0 = llmin(y0, v[VY]);
                y1 = llmax(y1, v[VY]);
            }
            const LLVector3 hole_centre(((x0 + x1) * 0.5f - W * 0.5f) * s, (H - (y0 + y1) * 0.5f) * s, 0.f);
            size_t wall = 0;
            for (size_t i = 0; i < m.positions[FACE_EDGE].size(); ++i)
            {
                const LLVector3& e = m.positions[FACE_EDGE][i];
                const F32 px = e[VX] / s + W * 0.5f, py = H - e[VY] / s;
                const LLVector3& n = m.normals[FACE_EDGE][i];
                ensure("edge normal flat", n[VZ] == 0.f && std::fabs(n.length() - 1.f) < 1e-4f);
                if (px < x0 - 2.f || px > x1 + 2.f || py < y0 - 2.f || py > y1 + 2.f)
                    continue;
                ++wall;
                LLVector3 to_centre = hole_centre - e;
                to_centre.mV[VZ] = 0.f;
                ensure("hole wall normal into the hole", n * to_centre > 0.f);
            }
            ensure("has a hole wall", wall >= 2 * hole.size());
        }
    }

    template<> template<> void fscutoutmesh_object::test<30>()
    {
        set_test_name("thin_slit_hole_mesh_clean");
        // A narrow enclosed slit (legs nearly touching): refinement points near its walls must not cross into
        // the slit or pile up on one spot. A slit thinner than the simplify tolerance after the margin (2 px here)
        // is filled rather than cut with every Chaikin point kept.
        const S32 W = 300, H = 400;
        const F32 s = 2.f / H;
        const struct { F32 margin; S32 gap; bool cut; } cases[] = { { 0.f, 3, true }, { 0.f, 4, true }, { 2.f, 8, true },
                                                                    { 2.f, 6, false } };
        for (const auto& c : cases)
        {
            Img img(W, H);
            img.rect(50, 40, 250, 360);
            img.clear(150 - c.gap / 2, 100, 150 + c.gap - c.gap / 2, 300);
            TraceParams p;
            p.margin_px = c.margin;
            const std::vector<Outline> polys = img.trace(p);
            ensure_equals("one outline", polys.size(), (size_t)1);
            ensure_equals("slit cut or filled", polys[0].holes.size(), (size_t)(c.cut ? 1 : 0));
            if (!c.cut)
                continue;
            const size_t boundary = polys[0].outer.size() + polys[0].holes[0].size();

            const Mesh m = buildMesh(polys, W, H, 2.f, 0.03f, 0.27f);
            const std::vector<LLVector3>& fp = m.positions[FACE_FRONT];
            for (size_t i = 0; i < fp.size(); ++i)
                for (size_t j = i + 1; j < fp.size(); ++j)
                    ensure("no duplicate front positions", (fp[i] - fp[j]).length() > 1e-6f);
            const size_t tris = m.indices[FACE_FRONT].size() / 3;
            F32 min_angle = 180.f;
            for (size_t t = 0; t < m.indices[FACE_FRONT].size(); t += 3)
            {
                ensure("no zero-area triangle", 0.5f * tri_cross(m, FACE_FRONT, t).length() >= 1e-6f);
                min_angle = llmin(min_angle, tri_min_angle_deg(m, FACE_FRONT, t));
            }
            std::cerr << "thin slit, margin " << c.margin << " px, gap " << c.gap << " px: outer " << polys[0].outer.size()
                      << " + hole " << polys[0].holes[0].size() << " pts, front tris " << tris << " (4n = " << 4 * boundary
                      << "), min angle " << min_angle << " deg" << std::endl;
            ensure("front triangles <= 4n", tris <= 4 * boundary);
            ensure("watertight", is_watertight(m));
        }
    }
}
