/**
 * @file fscutoutmesh.cpp
 * @brief Paper cutout: outline tracing
 *
 * $LicenseInfo:firstyear=2026:license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, Trish
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "fscutoutmesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <unordered_map>

namespace FSCutout
{
namespace
{
    struct Mask
    {
        S32 w, h;
        std::vector<U8> v;
        Mask(S32 w_, S32 h_) : w(w_), h(h_), v((size_t)w_ * h_, 0) {}
        bool get(S32 x, S32 y) const { return x >= 0 && y >= 0 && x < w && y < h && v[(size_t)y * w + x]; }
    };

    // Every pixel within a Euclidean disc of radius r of a solid pixel becomes solid.
    // shortcut: O(w*h*r), fine for snapshot sizes and margins of a few dozen pixels.
    Mask dilate(const Mask& src, F32 r)
    {
        Mask out = src;
        const S32 R = (S32)std::floor(r);
        std::vector<S32> prefix(src.w + 1);
        for (S32 dy = -R; dy <= R; ++dy)
        {
            const S32 hx = (S32)std::floor(std::sqrt(r * r - (F32)(dy * dy)));
            for (S32 y = 0; y < src.h; ++y)
            {
                const S32 ty = y + dy;
                if (ty < 0 || ty >= src.h)
                {
                    continue;
                }
                for (S32 x = 0; x < src.w; ++x)
                {
                    prefix[x + 1] = prefix[x] + src.v[(size_t)y * src.w + x];
                }
                if (prefix[src.w] == 0)
                {
                    continue;
                }
                for (S32 x = 0; x < src.w; ++x)
                {
                    if (prefix[std::min(x + hx + 1, src.w)] - prefix[std::max(x - hx, 0)] > 0)
                    {
                        out.v[(size_t)ty * src.w + x] = 1;
                    }
                }
            }
        }
        return out;
    }

    struct Island
    {
        S32 start_x, start_y;   // first pixel in raster order, so its top edge is on the outer contour
        S32 pixels;
    };

    // 4-connected background fill from seed i, marking seen; returns the component's pixels.
    std::vector<S32> floodBackground(const Mask& m, S32 i, std::vector<U8>& seen, bool& touches_pad, S32 pad)
    {
        std::vector<S32> stack, pixels;
        stack.push_back(i);
        seen[i] = 1;
        while (!stack.empty())
        {
            const S32 c = stack.back();
            stack.pop_back();
            pixels.push_back(c);
            const S32 x = c % m.w, y = c / m.w;
            // Next to the pad counts too: the frame cut below would open such a hole to the outside.
            touches_pad = touches_pad || x <= pad || y <= pad || x >= m.w - pad - 1 || y >= m.h - pad - 1;
            const S32 nx[4] = { x - 1, x + 1, x, x };
            const S32 ny[4] = { y, y, y - 1, y + 1 };
            for (S32 k = 0; k < 4; ++k)
            {
                if (nx[k] < 0 || ny[k] < 0 || nx[k] >= m.w || ny[k] >= m.h)
                {
                    continue;
                }
                const S32 j = ny[k] * m.w + nx[k];
                if (!seen[j] && !m.v[j])
                {
                    seen[j] = 1;
                    stack.push_back(j);
                }
            }
        }
        return pixels;
    }

    // Background the 4-connected fill from (0,0) cannot reach is enclosed. Enclosed regions that touch the pad,
    // are below min_pixels, or all of them when !keep, become solid. The rest are holes: hole_label holds their
    // 1-based id, and the result their first pixel in raster order.
    std::vector<Island> findHoles(Mask& m, S32 pad, F32 min_pixels, bool keep, std::vector<S32>& hole_label)
    {
        std::vector<U8> seen((size_t)m.w * m.h, 0);
        bool unused = false;
        floodBackground(m, 0, seen, unused, pad);
        std::vector<Island> holes;
        hole_label.assign(seen.size(), 0);
        for (S32 i = 0; i < m.w * m.h; ++i)
        {
            if (seen[i] || m.v[i])
            {
                continue;
            }
            bool touches_pad = false;
            const std::vector<S32> pixels = floodBackground(m, i, seen, touches_pad, pad);
            if (keep && !touches_pad && (F32)pixels.size() >= min_pixels)
            {
                holes.push_back({ i % m.w, i / m.w, (S32)pixels.size() });   // i is its first pixel in raster order
                for (S32 j : pixels)
                    hole_label[j] = (S32)holes.size();
            }
            else
            {
                for (S32 j : pixels)
                    m.v[j] = 1;
            }
        }
        return holes;
    }

    // 8-connected components; labels are written as 1-based ids.
    std::vector<Island> labelIslands(const Mask& m, std::vector<S32>& label)
    {
        std::vector<Island> islands;
        label.assign((size_t)m.w * m.h, 0);
        std::vector<S32> stack;
        for (S32 y = 0; y < m.h; ++y)
        {
            for (S32 x = 0; x < m.w; ++x)
            {
                if (!m.v[(size_t)y * m.w + x] || label[(size_t)y * m.w + x])
                {
                    continue;
                }
                const S32 id = (S32)islands.size() + 1;
                Island isl = { x, y, 0 };
                label[(size_t)y * m.w + x] = id;
                stack.push_back(y * m.w + x);
                while (!stack.empty())
                {
                    const S32 i = stack.back();
                    stack.pop_back();
                    ++isl.pixels;
                    const S32 cx = i % m.w, cy = i / m.w;
                    for (S32 dy = -1; dy <= 1; ++dy)
                    {
                        for (S32 dx = -1; dx <= 1; ++dx)
                        {
                            const S32 px = cx + dx, py = cy + dy;
                            if (m.get(px, py) && !label[(size_t)py * m.w + px])
                            {
                                label[(size_t)py * m.w + px] = id;
                                stack.push_back(py * m.w + px);
                            }
                        }
                    }
                }
                islands.push_back(isl);
            }
        }
        return islands;
    }

    // Follows the pixel edges with solid on the right, keeping diagonal solid neighbours joined, starting at
    // vertex (sx, sy) heading d0 (E, S, W, N). Returns every pixel corner on the way: Chaikin then rounds off a
    // pixel step, not a whole wall.
    template <typename Solid>
    Polygon traceContour(const Solid& solid, S32 sx, S32 sy, S32 d0)
    {
        // E, S, W, N; right turn = +1
        static const S32 DX[4] = { 1, 0, -1, 0 };
        static const S32 DY[4] = { 0, 1, 0, -1 };
        Polygon out;
        S32 vx = sx, vy = sy, d = d0;
        out.push_back(LLVector2((F32)vx, (F32)vy));
        do
        {
            vx += DX[d];
            vy += DY[d];
            // Pixels around the vertex: NW, NE, SE, SW
            const bool nw = solid(vx - 1, vy - 1), ne = solid(vx, vy - 1), se = solid(vx, vy), sw = solid(vx - 1, vy);
            bool ahead_left, ahead_right;
            switch (d)
            {
            case 0:  ahead_left = ne; ahead_right = se; break;
            case 1:  ahead_left = se; ahead_right = sw; break;
            case 2:  ahead_left = sw; ahead_right = nw; break;
            default: ahead_left = nw; ahead_right = ne; break;
            }
            const S32 nd = ahead_left ? (d + 3) % 4 : (ahead_right ? d : (d + 1) % 4);
            if (!(vx == sx && vy == sy))
            {
                out.push_back(LLVector2((F32)vx, (F32)vy));
            }
            d = nd;
        } while (!(vx == sx && vy == sy && d == d0));
        return out;
    }

    bool pointInPolygon(const LLVector2& p, const Polygon& poly)
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

    Polygon chaikin(const Polygon& in)
    {
        Polygon out;
        out.reserve(in.size() * 2);
        for (size_t i = 0; i < in.size(); ++i)
        {
            const LLVector2& a = in[i];
            const LLVector2& b = in[(i + 1) % in.size()];
            out.push_back(a * 0.75f + b * 0.25f);
            out.push_back(a * 0.25f + b * 0.75f);
        }
        return out;
    }

    F32 distToSegment(const LLVector2& p, const LLVector2& a, const LLVector2& b)
    {
        const LLVector2 ab = b - a;
        const F32 len2 = ab * ab;
        F32 t = len2 > 0.f ? ((p - a) * ab) / len2 : 0.f;
        t = llclamp(t, 0.f, 1.f);
        return (p - (a + ab * t)).length();
    }

    void dpRange(const Polygon& pts, size_t first, size_t last, F32 tol, std::vector<U8>& keep)
    {
        std::vector<std::pair<size_t, size_t> > stack;
        stack.push_back(std::make_pair(first, last));
        while (!stack.empty())
        {
            const std::pair<size_t, size_t> r = stack.back();
            stack.pop_back();
            F32 best = 0.f;
            size_t best_i = 0;
            for (size_t i = r.first + 1; i < r.second; ++i)
            {
                const F32 d = distToSegment(pts[i], pts[r.first], pts[r.second]);
                if (d > best)
                {
                    best = d;
                    best_i = i;
                }
            }
            if (best > tol)
            {
                keep[best_i] = 1;
                stack.push_back(std::make_pair(r.first, best_i));
                stack.push_back(std::make_pair(best_i, r.second));
            }
        }
    }

    // Douglas-Peucker on a closed polygon: split at the point farthest from point 0 and simplify both halves.
    // A shape thinner than tol collapses below 3 points: then the input comes back and collapsed is set.
    Polygon simplify(const Polygon& in, F32 tol, bool* collapsed = nullptr)
    {
        if (in.size() < 4 || tol <= 0.f)
        {
            return in;
        }
        size_t far_i = 0;
        F32 far_d = -1.f;
        for (size_t i = 1; i < in.size(); ++i)
        {
            const F32 d = (in[i] - in[0]).lengthSquared();
            if (d > far_d)
            {
                far_d = d;
                far_i = i;
            }
        }
        std::vector<U8> keep(in.size(), 0);
        keep[0] = keep[far_i] = 1;
        dpRange(in, 0, far_i, tol, keep);
        Polygon half2(in.begin() + far_i, in.end());
        half2.push_back(in[0]);
        std::vector<U8> keep2(half2.size(), 0);
        dpRange(half2, 0, half2.size() - 1, tol, keep2);
        for (size_t i = 1; i + 1 < half2.size(); ++i)
        {
            keep[far_i + i] = keep2[i];
        }
        Polygon out;
        for (size_t i = 0; i < in.size(); ++i)
        {
            if (keep[i])
            {
                out.push_back(in[i]);
            }
        }
        if (collapsed)
        {
            *collapsed = out.size() < 3;
        }
        return out.size() >= 3 ? out : in;
    }

    F32 cross(const LLVector2& o, const LLVector2& a, const LLVector2& b)
    {
        return (a[VX] - o[VX]) * (b[VY] - o[VY]) - (a[VY] - o[VY]) * (b[VX] - o[VX]);
    }

    bool onSegment(const LLVector2& a, const LLVector2& b, const LLVector2& p)
    {
        return p[VX] >= std::min(a[VX], b[VX]) && p[VX] <= std::max(a[VX], b[VX])
            && p[VY] >= std::min(a[VY], b[VY]) && p[VY] <= std::max(a[VY], b[VY]);
    }

    bool segmentsIntersect(const LLVector2& a, const LLVector2& b, const LLVector2& c, const LLVector2& d)
    {
        const F32 d1 = cross(c, d, a), d2 = cross(c, d, b), d3 = cross(a, b, c), d4 = cross(a, b, d);
        if (((d1 > 0.f && d2 < 0.f) || (d1 < 0.f && d2 > 0.f)) && ((d3 > 0.f && d4 < 0.f) || (d3 < 0.f && d4 > 0.f)))
        {
            return true;
        }
        return (d1 == 0.f && onSegment(c, d, a)) || (d2 == 0.f && onSegment(c, d, b))
            || (d3 == 0.f && onSegment(a, b, c)) || (d4 == 0.f && onSegment(a, b, d));
    }

    // Any edge of a touches or crosses any edge of b.
    bool polygonsTouch(const Polygon& a, const Polygon& b)
    {
        for (size_t i = 0; i < a.size(); ++i)
            for (size_t j = 0; j < b.size(); ++j)
                if (segmentsIntersect(a[i], a[(i + 1) % a.size()], b[j], b[(j + 1) % b.size()]))
                    return true;
        return false;
    }
}

F32 polygonArea(const Polygon& poly)
{
    F64 sum = 0.0;
    for (size_t i = 0; i < poly.size(); ++i)
    {
        const LLVector2& a = poly[i];
        const LLVector2& b = poly[(i + 1) % poly.size()];
        sum += (F64)a[VX] * b[VY] - (F64)b[VX] * a[VY];
    }
    return (F32)(sum * 0.5);
}

bool polygonSelfIntersects(const Polygon& poly)
{
    const size_t n = poly.size();
    for (size_t i = 0; i < n; ++i)
    {
        for (size_t j = i + 1; j < n; ++j)
        {
            if (j == i + 1 || (i == 0 && j == n - 1))
            {
                continue;   // neighbours share an endpoint
            }
            if (segmentsIntersect(poly[i], poly[(i + 1) % n], poly[j], poly[(j + 1) % n]))
            {
                return true;
            }
        }
    }
    return false;
}

std::vector<Outline> traceOutlines(const U8* rgba, S32 width, S32 height, const TraceParams& p)
{
    std::vector<Outline> result;
    if (!rgba || width <= 0 || height <= 0)
    {
        return result;
    }

    // Pad so shapes touching the border (and their dilation) close.
    const S32 pad = (S32)std::ceil(std::max(p.margin_px, 0.f)) + 1;
    Mask mask(width + 2 * pad, height + 2 * pad);
    for (S32 y = 0; y < height; ++y)
    {
        for (S32 x = 0; x < width; ++x)
        {
            if (rgba[((size_t)y * width + x) * 4 + 3] >= p.alpha_threshold)
            {
                mask.v[(size_t)(y + pad) * mask.w + x + pad] = 1;
            }
        }
    }
    if (p.margin_px > 0.f)
    {
        mask = dilate(mask, p.margin_px);
    }
    std::vector<S32> hole_label;
    const std::vector<Island> holes = findHoles(mask, pad, p.min_hole_frac * (F32)width * (F32)height, p.cut_holes, hole_label);
    // Cut straight along the frame: the margin must not grow past the picture (UVs would leave 0..1 and wrap).
    for (S32 y = 0; y < mask.h; ++y)
    {
        for (S32 x = 0; x < mask.w; ++x)
        {
            if (x < pad || y < pad || x >= pad + width || y >= pad + height)
            {
                mask.v[(size_t)y * mask.w + x] = 0;
            }
        }
    }

    std::vector<S32> label;
    const std::vector<Island> islands = labelIslands(mask, label);
    // A hole's first pixel has solid on its left: that pixel's island encloses the hole.
    std::vector<std::vector<S32>> island_holes(islands.size());
    std::vector<S32> island_area(islands.size());
    for (size_t i = 0; i < islands.size(); ++i)
        island_area[i] = islands[i].pixels;
    for (size_t h = 0; h < holes.size(); ++h)
    {
        const S32 id = label[(size_t)holes[h].start_y * mask.w + holes[h].start_x - 1];
        llassert(id > 0);
        if (id > 0)
        {
            island_holes[id - 1].push_back((S32)h);
            island_area[id - 1] += holes[h].pixels;   // islands are kept or dropped by their filled size, as before
        }
    }

    // Smooth and simplify. Simplifying can make a tight comb cross itself: retry with a finer tolerance, else (when
    // allow_raw) try it unsimplified, else give up (false). Holes never go unsimplified: a slit thinner than the
    // tolerance would keep every Chaikin point and fill its walls with sub-pixel triangles; it is filled instead.
    auto clean = [&p](Polygon& poly, bool allow_raw)
    {
        for (S32 i = 0; i < p.smooth_passes; ++i)
        {
            poly = chaikin(poly);
        }
        F32 tol = p.simplify_px;
        bool collapsed = false;
        Polygon simple = simplify(poly, tol, &collapsed);
        if (collapsed && !allow_raw)
        {
            return false;
        }
        for (S32 retry = 0; retry < 3 && polygonSelfIntersects(simple); ++retry)
        {
            tol *= 0.5f;
            simple = simplify(poly, tol);
        }
        if (polygonSelfIntersects(simple))
        {
            if (!allow_raw || polygonSelfIntersects(poly))
            {
                return false;
            }
            simple = poly;
        }
        poly.swap(simple);
        return true;
    };
    const LLVector2 offset((F32)pad, (F32)pad);

    const F32 min_pixels = p.min_island_frac * (F32)width * (F32)height;
    for (size_t i = 0; i < islands.size(); ++i)
    {
        const Island& isl = islands[i];
        if ((F32)island_area[i] < min_pixels)
        {
            continue;
        }
        Outline outline;
        outline.outer = traceContour([&mask](S32 x, S32 y) { return mask.get(x, y); }, isl.start_x, isl.start_y, 0);
        if (!clean(outline.outer, true))
        {
            LL_WARNS("Snapshot") << "Cutout outline still self-intersects, dropping island at " << isl.start_x - pad << ", "
                                 << isl.start_y - pad << LL_ENDL;
            continue;
        }
        for (LLVector2& v : outline.outer)
        {
            v -= offset;
        }
        if (polygonArea(outline.outer) < 0.f)
        {
            std::reverse(outline.outer.begin(), outline.outer.end());
        }

        for (S32 h : island_holes[i])
        {
            const Island& hole = holes[h];
            const S32 id = h + 1;
            // Solid = not this hole; start on the hole's first pixel's top edge, heading W so solid is on the right.
            auto solid = [&](S32 x, S32 y)
            { return x < 0 || y < 0 || x >= mask.w || y >= mask.h || hole_label[(size_t)y * mask.w + x] != id; };
            Polygon ring = traceContour(solid, hole.start_x + 1, hole.start_y, 2);
            if (!clean(ring, false))
            {
                LL_WARNS("Snapshot") << "Cutout hole at " << hole.start_x - pad << ", " << hole.start_y - pad
                                     << " still self-intersects after simplifying, filling it" << LL_ENDL;
                continue;
            }
            for (LLVector2& v : ring)
            {
                v -= offset;
            }
            if (polygonArea(ring) > 0.f)
            {
                std::reverse(ring.begin(), ring.end());
            }
            // Strictly inside the outline, apart from the other holes: anything else is not a valid cap, so fill it.
            // Under 1 px^2 is a collapsed sliver, not a gap worth cutting.
            bool ok = ring.size() >= 3 && -polygonArea(ring) >= 1.f && pointInPolygon(ring[0], outline.outer) &&
                      !polygonsTouch(ring, outline.outer);
            for (size_t k = 0; ok && k < outline.holes.size(); ++k)
            {
                const Polygon& other = outline.holes[k];
                ok = !polygonsTouch(ring, other) && !pointInPolygon(ring[0], other) && !pointInPolygon(other[0], ring);
            }
            if (!ok)
            {
                LL_WARNS("Snapshot") << "Cutout hole at " << hole.start_x - pad << ", " << hole.start_y - pad
                                     << " is a sliver or crosses the outline or another hole after simplifying, filling it"
                                     << LL_ENDL;
                continue;
            }
            outline.holes.push_back(ring);
        }
        result.push_back(outline);
    }
    return result;
}

namespace
{
    typedef std::array<U32, 3> Tri;
    struct P2 { F64 x, y; };

    const F64 PI_D = 3.14159265358979323846;
    const F64 OFF_CENTRE_APEX = 22.0 * PI_D / 180.0;   // a little over the 20 deg target, so float error stays above it

    F64 orient(const P2& a, const P2& b, const P2& c)
    {
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    }

    F64 angleAt(const P2& o, const P2& a, const P2& b)
    {
        const F64 ux = a.x - o.x, uy = a.y - o.y, vx = b.x - o.x, vy = b.y - o.y;
        return std::atan2(std::fabs(ux * vy - uy * vx), ux * vx + uy * vy);
    }

    // Cap triangulation in y-up pixel space: holes bridged into the outline, ear clip, Lawson flips to constrained
    // Delaunay, then circumcentre refinement. The outline and hole rings are the only constraints: a ring edge has
    // no twin (nothing outside it, nothing inside a hole), so it is never flipped. Bridges are ordinary edges.
    struct Cap
    {
        std::vector<P2> pts;    // first n are the rings (outline CCW, then each hole CW), then inserted points
        U32 n = 0;
        std::vector<Tri> tris;  // CCW
        std::vector<std::vector<U32>> bounds;   // the input rings: [0] outline, then holes; cardboard on the left
        std::vector<std::vector<U32>> rings;    // cap boundary: bounds plus split points on their segments
        std::unordered_map<U64, U32> edges;   // directed edge -> triangle holding it

        static U64 key(U32 a, U32 b) { return ((U64)a << 32) | b; }

        void link(U32 t)
        {
            for (S32 k = 0; k < 3; ++k)
                edges[key(tris[t][k], tris[t][(k + 1) % 3])] = t;
        }
        void unlink(U32 t)
        {
            for (S32 k = 0; k < 3; ++k)
                edges.erase(key(tris[t][k], tris[t][(k + 1) % 3]));
        }
        void add(U32 a, U32 b, U32 c)
        {
            tris.push_back({ a, b, c });
            link((U32)tris.size() - 1);
        }
        U32 third(U32 t, U32 a, U32 b) const
        {
            for (U32 v : tris[t])
                if (v != a && v != b)
                    return v;
            return a;
        }
        F64 minAngle(const Tri& t) const
        {
            const P2 &a = pts[t[0]], &b = pts[t[1]], &c = pts[t[2]];
            return std::min({ angleAt(a, b, c), angleAt(b, c, a), angleAt(c, a, b) });
        }

        // q lies strictly inside the corner of ring at i (cardboard side), so a bridge from ring[i] to q starts inward.
        bool locallyInside(const std::vector<U32>& ring, size_t i, const P2& q) const
        {
            const size_t m = ring.size();
            const P2 &a = pts[ring[(i + m - 1) % m]], &p = pts[ring[i]], &b = pts[ring[(i + 1) % m]];
            return orient(a, p, b) > 0.0 ? orient(a, p, q) > 0.0 && orient(p, b, q) > 0.0
                                         : orient(a, p, q) > 0.0 || orient(p, b, q) > 0.0;
        }

        // Segment a-b crosses or touches an edge of ring (edges at a or b excepted) or passes through one of its points.
        bool blocked(U32 a, U32 b, const std::vector<U32>& ring) const
        {
            const P2 &A = pts[a], &B = pts[b];
            for (size_t k = 0; k < ring.size(); ++k)
            {
                const U32 c = ring[k], d = ring[(k + 1) % ring.size()];
                if (c == a || c == b || d == a || d == b)
                    continue;
                const P2 &C = pts[c], &D = pts[d];
                const F64 o1 = orient(A, B, C), o2 = orient(A, B, D), o3 = orient(C, D, A), o4 = orient(C, D, B);
                if (((o1 > 0.0) != (o2 > 0.0) || o1 == 0.0 || o2 == 0.0) && ((o3 > 0.0) != (o4 > 0.0) || o3 == 0.0 || o4 == 0.0))
                    return true;
            }
            return false;
        }

        // Outline with each hole spliced in through a two-way bridge to the nearest point it can see (holes bridged
        // earlier count as outline), so one ear clip covers the cap. Empty, with failed = that hole, if one finds no bridge.
        std::vector<U32> bridged(size_t& failed) const
        {
            std::vector<U32> poly = bounds[0];
            for (size_t h = 1; h < bounds.size(); ++h)
            {
                const std::vector<U32>& hole = bounds[h];
                std::vector<std::pair<F64, std::pair<size_t, size_t>>> cand;   // (distance^2, (poly index, hole index))
                for (size_t k = 0; k < poly.size(); ++k)
                    for (size_t j = 0; j < hole.size(); ++j)
                    {
                        const P2 &P = pts[poly[k]], &M = pts[hole[j]];
                        cand.push_back({ (P.x - M.x) * (P.x - M.x) + (P.y - M.y) * (P.y - M.y), { k, j } });
                    }
                std::sort(cand.begin(), cand.end());
                bool found = false;
                for (const auto& c : cand)
                {
                    const size_t k = c.second.first, j = c.second.second;
                    const U32 P = poly[k], M = hole[j];
                    if (!locallyInside(poly, k, pts[M]) || !locallyInside(hole, j, pts[P]) || blocked(P, M, poly))
                        continue;
                    bool clear = true;
                    for (size_t o = h; o < bounds.size() && clear; ++o)
                        clear = !blocked(P, M, bounds[o]);
                    if (!clear)
                        continue;
                    // ..., P, M, rest of the hole, M, P, ...
                    std::vector<U32> splice;
                    for (size_t s = 0; s <= hole.size(); ++s)
                        splice.push_back(hole[(j + s) % hole.size()]);
                    splice.push_back(P);
                    poly.insert(poly.begin() + k + 1, splice.begin(), splice.end());
                    found = true;
                    break;
                }
                if (!found)
                {
                    failed = h - 1;   // index into the holes
                    return std::vector<U32>();
                }
            }
            return poly;
        }

        void earClip(std::vector<U32> left)
        {
            // shortcut: O(n^2) typical, O(n^3) worst; fine for simplified outlines of a few hundred points.
            size_t i = 0, misses = 0;
            while (left.size() > 3)
            {
                const size_t m = left.size();
                i %= m;
                bool ear = isEar(left, i);
                // A full lap without an ear only happens on numerically flat runs: clip a convex corner anyway so it ends.
                if (!ear && misses >= m)
                {
                    LL_WARNS("Snapshot") << "Cutout cap: no ear in a full lap of " << m << " points, clipping one anyway" << LL_ENDL;
                    for (size_t k = 0; k < m; ++k)
                    {
                        if (orient(pts[left[(k + m - 1) % m]], pts[left[k]], pts[left[(k + 1) % m]]) > 0.0)
                        {
                            i = k;
                            break;
                        }
                    }
                    ear = true;
                }
                if (ear)
                {
                    add(left[(i + m - 1) % m], left[i], left[(i + 1) % m]);
                    left.erase(left.begin() + i);
                    misses = 0;
                }
                else
                {
                    ++i;
                    ++misses;
                }
            }
            add(left[0], left[1], left[2]);
        }

        bool isEar(const std::vector<U32>& left, size_t i) const
        {
            const size_t m = left.size();
            const U32 p = left[(i + m - 1) % m], c = left[i], x = left[(i + 1) % m];
            if (orient(pts[p], pts[c], pts[x]) <= 0.0)
            {
                return false;
            }
            for (U32 v : left)
            {
                if (v != p && v != c && v != x && orient(pts[p], pts[c], pts[v]) >= 0.0 &&
                    orient(pts[c], pts[x], pts[v]) >= 0.0 && orient(pts[x], pts[p], pts[v]) >= 0.0)
                {
                    return false;
                }
            }
            return true;
        }

        // Flip every non-Delaunay interior edge on the stack until none is left.
        void legalize(std::vector<std::pair<U32, U32>>& stack)
        {
            while (!stack.empty())
            {
                const U32 a = stack.back().first, b = stack.back().second;
                stack.pop_back();
                auto i1 = edges.find(key(a, b)), i2 = edges.find(key(b, a));
                if (i1 == edges.end() || i2 == edges.end())
                {
                    continue;
                }
                const U32 t1 = i1->second, t2 = i2->second;
                const U32 c = third(t1, a, b), d = third(t2, a, b);
                const P2 &A = pts[a], &B = pts[b], &C = pts[c], &D = pts[d];
                // Delaunay iff the opposite angles sum to at most pi (scale free; ties, e.g. cocircular, stay).
                if (angleAt(C, A, B) + angleAt(D, B, A) <= PI_D + 1e-9 || orient(A, D, C) <= 0.0 || orient(D, B, C) <= 0.0)
                {
                    continue;
                }
                unlink(t1);
                unlink(t2);
                tris[t1] = { a, d, c };
                tris[t2] = { d, b, c };
                link(t1);
                link(t2);
                stack.insert(stack.end(), { { a, d }, { d, b }, { b, c }, { c, a } });
            }
        }

        void legalizeAll()
        {
            std::vector<std::pair<U32, U32>> stack;
            for (const Tri& t : tris)
                for (S32 k = 0; k < 3; ++k)
                    stack.emplace_back(t[k], t[(k + 1) % 3]);
            legalize(stack);
        }

        // false (nothing inserted) when p is not clearly inside some triangle, e.g. on a ring edge or across one.
        bool insert(const P2& p)
        {
            // The containing triangle is the one p is most inside of.
            U32 best = 0;
            F64 best_in = -1e300;
            for (U32 t = 0; t < tris.size(); ++t)
            {
                F64 in = 1e300;
                for (S32 k = 0; k < 3; ++k)
                {
                    const P2 &a = pts[tris[t][k]], &b = pts[tris[t][(k + 1) % 3]];
                    in = std::min(in, orient(a, b, p) / std::hypot(b.x - a.x, b.y - a.y));
                }
                if (in > best_in)
                {
                    best_in = in;
                    best = t;
                }
            }
            if (best_in <= 1e-6)
            {
                return false;
            }
            const U32 v = (U32)pts.size();
            pts.push_back(p);
            const Tri old = tris[best];
            unlink(best);
            tris[best] = { old[0], old[1], v };
            link(best);
            add(old[1], old[2], v);
            add(old[2], old[0], v);
            std::vector<std::pair<U32, U32>> stack = { { old[0], old[1] }, { old[1], old[2] }, { old[2], old[0] } };
            legalize(stack);
            return true;
        }

        bool hasTri(const Tri& t) const
        {
            auto it = edges.find(key(t[0], t[1]));
            return it != edges.end() && third(it->second, t[0], t[1]) == t[2];
        }

        // Inside the outline and outside every hole (even-odd over all input rings).
        bool insideCardboard(const P2& p) const
        {
            bool in = false;
            for (const std::vector<U32>& ring : bounds)
            {
                for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
                {
                    const P2 &a = pts[ring[i]], &b = pts[ring[j]];
                    if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
                        in = !in;
                }
            }
            return in;
        }

        // Splits boundary segment rings[r][k] -> [k + 1] at its midpoint. A boundary edge has no twin, so it is never flipped.
        void split(size_t r, size_t k)
        {
            std::vector<U32>& ring = rings[r];
            const U32 a = ring[k], b = ring[(k + 1) % ring.size()];
            auto it = edges.find(key(a, b));
            if (it == edges.end())
            {
                return;
            }
            const U32 t = it->second, c = third(t, a, b), v = (U32)pts.size();
            pts.push_back({ (pts[a].x + pts[b].x) * 0.5, (pts[a].y + pts[b].y) * 0.5 });
            unlink(t);
            tris[t] = { a, v, c };
            link(t);
            add(v, b, c);
            ring.insert(ring.begin() + k + 1, v);
            std::vector<std::pair<U32, U32>> stack = { { b, c }, { c, a } };
            legalize(stack);
        }

        // p lies strictly inside the circle with diameter a-b.
        static bool encroaches(const P2& p, const P2& a, const P2& b)
        {
            return (a.x - p.x) * (b.x - p.x) + (a.y - p.y) * (b.y - p.y) < 0.0;
        }

        // p sees a-b at more than 120 deg (diametral lens): an existing vertex that close to a segment needs a split.
        static bool encroachesLens(const P2& p, const P2& a, const P2& b)
        {
            const F64 ux = a.x - p.x, uy = a.y - p.y, vx = b.x - p.x, vy = b.y - p.y;
            return ux * vx + uy * vy < -0.5 * std::hypot(ux, uy) * std::hypot(vx, vy);
        }

        // Boundary segment (ring, index) whose diametral circle p is in; ring -1 if none.
        std::pair<S32, size_t> encroachedSegment(const P2& p) const
        {
            for (size_t r = 0; r < rings.size(); ++r)
            {
                const std::vector<U32>& ring = rings[r];
                for (size_t k = 0; k < ring.size(); ++k)
                {
                    if (encroaches(p, pts[ring[k]], pts[ring[(k + 1) % ring.size()]]))
                    {
                        return { (S32)r, k };
                    }
                }
            }
            return { -1, 0 };
        }

        // Ruppert refinement: a boundary segment (outline or hole) that a vertex or a wanted circumcentre encroaches
        // is split at its midpoint instead, so triangles beside long straight runs get fixed too. Split points stay
        // on their ring, and the edge face is built from the same rings, so caps and edge meet without T-junctions.
        void refine()
        {
            const F64 min_angle = 20.0 * PI_D / 180.0;
            rings = bounds;
            std::set<Tri> rejected;     // sorted vertex triples whose circumcentre cannot be used
            // Front triangles = ring sizes - 2 + 2 * holes + 2 * interior points: a split costs 1, an interior point
            // 2, so budget 3n + 2 - 2 * holes keeps them within 4n of the input rings (land impact). Guard only:
            // probe of a 1024 px avatar outline reaches 20 deg with 211 of 296 triangles; input corners sharper
            // than about 60 deg can cascade splits and end here.
            S64 budget = 3 * (S64)n + 2 - 2 * (S64)(bounds.size() - 1);
            while (budget > 0)
            {
                // A boundary edge whose opposite vertex sees it at more than 120 deg is encroached (lens, not the
                // diametral circle: fewer splits, so thin walls between a hole and the outline stay in budget).
                // Wanted circumcentres still use the full diametral circle below.
                bool split_one = false;
                for (size_t r = 0; r < rings.size() && !split_one; ++r)
                {
                    for (size_t k = 0; k < rings[r].size() && !split_one; ++k)
                    {
                        const U32 a = rings[r][k], b = rings[r][(k + 1) % rings[r].size()];
                        auto it = edges.find(key(a, b));
                        if (it != edges.end() && encroachesLens(pts[third(it->second, a, b)], pts[a], pts[b]))
                        {
                            split(r, k);
                            --budget;
                            split_one = true;
                        }
                    }
                }
                if (split_one)
                {
                    continue;
                }

                S32 worst = -1;
                F64 worst_angle = min_angle;
                for (U32 t = 0; t < tris.size(); ++t)
                {
                    const F64 ang = minAngle(tris[t]);
                    if (ang < worst_angle)
                    {
                        Tri sorted = tris[t];
                        std::sort(sorted.begin(), sorted.end());
                        if (!rejected.count(sorted))
                        {
                            worst_angle = ang;
                            worst = (S32)t;
                        }
                    }
                }
                if (worst < 0)
                {
                    break;
                }
                const Tri bad = tris[worst];
                Tri bad_sorted = bad;
                std::sort(bad_sorted.begin(), bad_sorted.end());
                const P2 &a = pts[bad[0]], &b = pts[bad[1]], &c = pts[bad[2]];
                const F64 bx = b.x - a.x, by = b.y - a.y, cx = c.x - a.x, cy = c.y - a.y, d = 2.0 * (bx * cy - by * cx);
                const F64 b2 = bx * bx + by * by, c2 = cx * cx + cy * cy;
                P2 centre = { a.x + (cy * b2 - by * c2) / d, a.y + (bx * c2 - cx * b2) / d };
                // Off-centre (Ungor): when the circumcentre is far, take the point on the shortest edge's bisector that
                // makes a good triangle with that edge instead. Same guarantees, far fewer inserted points.
                if (d > 0.0)
                {
                    const P2* e[3] = { &a, &b, &c };
                    S32 k = 0;
                    F64 shortest = 1e300;
                    for (S32 i = 0; i < 3; ++i)
                    {
                        const P2 &p = *e[i], &q = *e[(i + 1) % 3];
                        const F64 l = std::hypot(q.x - p.x, q.y - p.y);
                        if (l < shortest)
                        {
                            shortest = l;
                            k = i;
                        }
                    }
                    const P2 &p = *e[k], &q = *e[(k + 1) % 3];
                    const P2 mid = { (p.x + q.x) * 0.5, (p.y + q.y) * 0.5 };
                    const F64 to_centre = std::hypot(centre.x - mid.x, centre.y - mid.y);
                    const F64 reach = shortest * 0.5 / std::tan(OFF_CENTRE_APEX * 0.5);
                    if (to_centre > reach)
                    {
                        centre = { mid.x + (centre.x - mid.x) * reach / to_centre, mid.y + (centre.y - mid.y) * reach / to_centre };
                    }
                }
                // Only the point to insert is tested: one landing across a thin wall is still inside the cardboard, and
                // the guards below stop it piling up. Testing the circumcentre too costs splits and angle quality.
                const std::pair<S32, size_t> seg = d > 0.0 ? encroachedSegment(centre) : std::pair<S32, size_t>(-1, 0);
                if (seg.first >= 0)
                {
                    split((size_t)seg.first, seg.second);
                    --budget;
                    continue;
                }
                // Never a point inside a hole or outside the outline.
                if (d <= 0.0 || budget < 2 || !insideCardboard(centre) || !insert(centre))
                {
                    rejected.insert(bad_sorted);
                    continue;
                }
                budget -= 2;
                // The point landed where it does not fix this triangle: never try it again (it would pile up).
                if (hasTri(bad))
                {
                    rejected.insert(bad_sorted);
                }
            }
        }
    };
}

Mesh buildMesh(const std::vector<Outline>& outlines, S32 img_w, S32 img_h, F32 height_m, F32 thickness_m, F32 edge_tile_m)
{
    Mesh mesh;
    if (img_w <= 0 || img_h <= 0 || edge_tile_m <= 0.f)
    {
        return mesh;
    }
    const F64 s = (F64)height_m / img_h;
    const F32 zf = thickness_m * 0.5f;
    const F32 cos_crease = std::cos(30.f * F_PI / 180.f);

    for (const Outline& outline : outlines)
    {
        if (outline.outer.size() < 3)
        {
            continue;
        }
        std::vector<Polygon> holes;
        for (const Polygon& hole : outline.holes)
        {
            if (hole.size() >= 3)
                holes.push_back(hole);
        }
        // y-up pixel space, outline made CCW and holes CW seen from +Z, so the cardboard is left of every ring.
        Cap cap;
        auto add_ring = [&](const Polygon& in, bool ccw)
        {
            const size_t first = cap.pts.size();
            F64 area2 = 0.0;
            for (const LLVector2& v : in)
                cap.pts.push_back({ v[VX], (F64)img_h - v[VY] });
            for (size_t i = 0; i < in.size(); ++i)
                area2 += orient({ 0.0, 0.0 }, cap.pts[first + i], cap.pts[first + (i + 1) % in.size()]);
            if ((area2 > 0.0) != ccw)
            {
                std::reverse(cap.pts.begin() + first, cap.pts.end());
            }
            std::vector<U32> ring;
            for (size_t i = 0; i < in.size(); ++i)
                ring.push_back((U32)(first + i));
            cap.bounds.push_back(ring);
        };
        std::vector<U32> poly;
        while (true)
        {
            cap = Cap();
            add_ring(outline.outer, true);
            for (const Polygon& hole : holes)
                add_ring(hole, false);
            cap.n = (U32)cap.pts.size();
            size_t failed = 0;
            poly = cap.bridged(failed);
            if (!poly.empty())
            {
                break;
            }
            LL_WARNS("Snapshot") << "Cutout cap: hole " << failed << " has no bridge to the outline, filling it" << LL_ENDL;
            holes.erase(holes.begin() + failed);
        }
        cap.earClip(poly);
        cap.legalizeAll();
        cap.refine();

        auto position = [&](const P2& q, F32 z) { return LLVector3((F32)((q.x - img_w * 0.5) * s), (F32)(q.y * s), z); };

        // Front and back caps: same points and UVs, back with reversed winding.
        const U32 f0 = (U32)mesh.positions[FACE_FRONT].size(), b0 = (U32)mesh.positions[FACE_BACK].size();
        for (const P2& q : cap.pts)
        {
            const LLVector2 uv((F32)(q.x / img_w), (F32)((img_h - q.y) / img_h));
            mesh.positions[FACE_FRONT].push_back(position(q, zf));
            mesh.normals[FACE_FRONT].push_back(LLVector3(0.f, 0.f, 1.f));
            mesh.uvs[FACE_FRONT].push_back(uv);
            mesh.positions[FACE_BACK].push_back(position(q, -zf));
            mesh.normals[FACE_BACK].push_back(LLVector3(0.f, 0.f, -1.f));
            mesh.uvs[FACE_BACK].push_back(uv);
        }
        for (const Tri& t : cap.tris)
        {
            mesh.indices[FACE_FRONT].insert(mesh.indices[FACE_FRONT].end(), { f0 + t[0], f0 + t[1], f0 + t[2] });
            mesh.indices[FACE_BACK].insert(mesh.indices[FACE_BACK].end(), { b0 + t[0], b0 + t[2], b0 + t[1] });
        }

        // Edge: one quad per cap boundary segment, so it meets the caps at every split point too (no T-junctions).
        // Corners turning < 30 deg share an averaged normal, sharper ones split; split points turn 0 deg.
        // One strip per ring (outline and each hole), each starting at u = 0.
        for (const std::vector<U32>& ring : cap.rings)
        {
            const U32 n = (U32)ring.size();
            std::vector<LLVector2> seg_n(n);
            std::vector<F64> seg_len(n);
            for (U32 i = 0; i < n; ++i)
            {
                const P2 &a = cap.pts[ring[i]], &b = cap.pts[ring[(i + 1) % n]];
                seg_len[i] = std::hypot(b.x - a.x, b.y - a.y) * s;
                // Right of the walk = away from the cardboard: outward on the outline, into the hole on a hole.
                seg_n[i] = LLVector2((F32)(b.y - a.y), (F32)(a.x - b.x));
                seg_n[i].normalize();
            }
            std::vector<bool> smooth(n);
            std::vector<LLVector2> avg(n);
            for (U32 i = 0; i < n; ++i)
            {
                const LLVector2& prev = seg_n[(i + n - 1) % n];
                smooth[i] = prev * seg_n[i] > cos_crease;
                avg[i] = prev + seg_n[i];
                avg[i].normalize();
            }
            std::vector<LLVector3>& ep = mesh.positions[FACE_EDGE];
            const F32 v_back = thickness_m / edge_tile_m;   // same metres per repeat as u: square texels
            auto push_pair = [&](U32 corner, const LLVector2& nrm, F64 u) -> U32
            {
                const U32 idx = (U32)ep.size();
                const LLVector3 normal(nrm[VX], nrm[VY], 0.f);
                ep.push_back(position(cap.pts[ring[corner]], zf));
                ep.push_back(position(cap.pts[ring[corner]], -zf));
                mesh.normals[FACE_EDGE].insert(mesh.normals[FACE_EDGE].end(), { normal, normal });
                mesh.uvs[FACE_EDGE].insert(mesh.uvs[FACE_EDGE].end(), { LLVector2((F32)u, 0.f), LLVector2((F32)u, v_back) });
                return idx;
            };
            F64 u = 0.0;
            U32 start = 0;
            for (U32 i = 0; i < n; ++i)
            {
                const U32 j = (i + 1) % n;
                if (i == 0 || !smooth[i])
                {
                    start = push_pair(i, smooth[i] ? avg[i] : seg_n[i], u);
                }
                u += seg_len[i] / edge_tile_m;
                // Always a new pair at the end: shared forward only, and the seam back to corner 0 needs its own u.
                const U32 end = push_pair(j, smooth[j] ? avg[j] : seg_n[i], u);
                mesh.indices[FACE_EDGE].insert(mesh.indices[FACE_EDGE].end(),
                                               { start, start + 1, end + 1, start, end + 1, end });
                start = end;
            }
        }
    }
    return mesh;
}
bool hasUsableAlpha(const U8* data, S32 width, S32 height, S32 components, U8 threshold)
{
    if (components != 4)
        return false;
    bool any_solid = false, any_clear = false;
    for (size_t i = 3, n = (size_t)width * height * 4; i < n && !(any_solid && any_clear); i += 4)
    {
        if (data[i] >= threshold)
            any_solid = true;
        else
            any_clear = true;
    }
    return any_solid && any_clear;
}

Rect alphaBounds(const U8* rgba, S32 width, S32 height, U8 threshold, S32 margin_px)
{
    S32 x0 = width, y0 = height, x1 = -1, y1 = -1;
    for (S32 y = 0; y < height; ++y)
        for (S32 x = 0; x < width; ++x)
            if (rgba[((size_t)y * width + x) * 4 + 3] >= threshold)
            {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
    if (x1 < 0)
        return { 0, 0, 0, 0 };
    x0 = std::max(x0 - margin_px, 0);
    y0 = std::max(y0 - margin_px, 0);
    x1 = std::min(x1 + margin_px, width - 1);
    y1 = std::min(y1 + margin_px, height - 1);
    return { x0, y0, x1 - x0 + 1, y1 - y0 + 1 };
}

void flattenOnto(const U8* rgba, S32 width, S32 height, const LLColor3& paper, std::vector<U8>& rgb_out)
{
    const size_t count = (size_t)width * height;
    rgb_out.resize(count * 3);
    F32 bg[3];
    for (S32 c = 0; c < 3; ++c)
        bg[c] = std::round(paper.mV[c] * 255.f);
    for (size_t i = 0; i < count; ++i)
    {
        const F32 a = rgba[i * 4 + 3] / 255.f;
        for (S32 c = 0; c < 3; ++c)
            rgb_out[i * 3 + c] = (U8)std::lround(rgba[i * 4 + c] * a + bg[c] * (1.f - a));
    }
}
}

namespace FSCutout
{
void uploadTextureSize(S32 width, S32 height, S32 max_side, S32& out_w, S32& out_h)
{
    const S32 long_side = std::max(1, std::max(width, height));
    const F32 scale = long_side > max_side ? (F32)max_side / (F32)long_side : 1.f;
    auto pow2_up = [max_side](S32 v)
    {
        S32 p = 1;
        while (p < v && p < max_side)
        {
            p <<= 1;
        }
        return std::min(p, max_side);
    };
    out_w = pow2_up(std::max(1, (S32)std::lround(width * scale)));
    out_h = pow2_up(std::max(1, (S32)std::lround(height * scale)));
}
}
