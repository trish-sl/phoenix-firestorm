/**
 * @file fsmirrorplanar.cpp
 * @brief Planar reflections for flat mirrors (FSRenderMirrorPlanar)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
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

#include "fsmirrorplanar.h"

#include "llcamera.h"
#include "llcubemaparray.h"
#include "llframetimer.h"
#include "lldrawable.h"
#include "llglslshader.h"
#include "llplane.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llshadermgr.h"
#include "llspatialpartition.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llvovolume.h"
#include "pipeline.h"

extern bool gCubeSnapshot;
extern bool gSnapshot;
extern U32 gFrameCount;

namespace
{
    // The mirror is clipped to a view this much wider than the camera's before the planar view is
    // fitted to it, so the edge fade, the temporal jitter and normal mapped mirrors stay inside it
    constexpr F32 SCREEN_MARGIN = 1.1f;
    // and the planar view is this much wider than the fitted part of the mirror
    constexpr F32 FIT_MARGIN = 1.04f;
    // Widest planar view, as the tangent of its half angle: about 76 degrees to keep a planar view
    // once it is used, 72 to start using one, so a mirror at the limit doesn't flip between the two
    constexpr F32 MAX_HALF_TAN_KEEP = 4.f;
    constexpr F32 MAX_HALF_TAN_START = 3.f;
    // Mirrors smaller than this on screen get the ordinary reflection probes; up to FADE_SCREEN_PIXELS
    // the planar view fades in over them, so a mirror shrinking into the distance doesn't pop
    constexpr F32 MIN_SCREEN_PIXELS = 8.f;
    constexpr F32 FADE_SCREEN_PIXELS = 24.f;
    // Below this size on screen the planar view may use a 256 render target; larger mirrors get at
    // least 512, as the mirror view's shadow maps take the render target's size
    constexpr F32 SMALL_TARGET_PIXELS = 64.f;
    // Thickness against the smaller side for a mirror to count as flat
    constexpr F32 MAX_THICKNESS = 0.2f;
    // Planar view updates in a row that want a smaller render target before it shrinks
    constexpr U32 SHRINK_DELAY = 90;
    // Cube renders in a row before the mirror render target goes back to the cube's size
    constexpr U32 CUBE_RESTORE_DELAY = 30;
    // Width of the fade into the ordinary reflections at the edge of the planar view, in texture space
    constexpr F32 EDGE_FADE = 0.02f;

    FSMirrorPlanar::View sView;
    bool sInView = false;
    bool sRecorded = false;

    // What the shaders read: 0 the cube, 1 the planar view, -1 neither (off screen or tiny)
    S32 sMode = 0;
    F32 sFade = 1.f;            // the planar view's weight, below 1 for mirrors tiny on screen
    bool sPlanarValid = false;
    F32 sMatrix[16] = { 0.f };
    F32 sTexelRatio = 1.f;     // the cube's texel angle over the planar view's
    F32 sCubeMipSlope = 9.f;   // the cube's mip count - 1
    LLRenderTarget sTarget;

    // The planar view's own allocation of mHeroProbeRT, to tell it from the pipeline's
    U32 sRequestedRes = 0;
    U32 sAllocatedTex = 0;
    U32 sAllocatedWidth = 0;
    U32 sShrinkCount = 0;
    U32 sCubeCount = 0;

    // Update rate
    U32 sLastRenderFrame = 0;
    const LLVOVolume* sLastHero = nullptr;
    LLVector3 sLastEye;

    // The mirror plane for culling, behind it is outside
    bool sCullValid = false;
    LLPlane sCullPlane;

    bool fs_hero_rt_is_ours()
    {
        const LLRenderTarget& rt = gPipeline.mHeroProbeRT.deferredScreen;
        return sRequestedRes != 0 && rt.isComplete() && rt.getTexture() == sAllocatedTex && rt.getWidth() == sAllocatedWidth;
    }

    void fs_alloc_hero_rt(U32 res)
    {
        LLPipeline::RenderTargetPack* saved_rt = gPipeline.mRT;
        const bool saved_cube = gCubeSnapshot;
        // the cube capture rules of allocateScreenBufferInternal: only the pack's own targets
        gCubeSnapshot = true;
        gPipeline.mRT = &gPipeline.mHeroProbeRT;
        gPipeline.allocateScreenBufferInternal(res, res);
        gPipeline.mRT = saved_rt;
        gCubeSnapshot = saved_cube;

        sRequestedRes = res;
        sAllocatedTex = gPipeline.mHeroProbeRT.deferredScreen.getTexture();
        sAllocatedWidth = gPipeline.mHeroProbeRT.deferredScreen.getWidth();
        sShrinkCount = 0;
    }

    void fs_set_cull_plane(const LLVector3& mirror_pos, const LLVector3& normal)
    {
        // A little behind the plane: the mirror itself may be thicker than the plane through its middle
        sCullPlane.setVec(mirror_pos - normal * 0.1f, -normal);
        sCullValid = true;
    }

    // Sutherland-Hodgman: keeps the part of poly where dot(n, p - o) - c >= 0
    void fs_clip(std::vector<LLVector3>& poly, const LLVector3& n, const LLVector3& o, F32 c)
    {
        if (poly.empty())
        {
            return;
        }
        std::vector<LLVector3> out;
        out.reserve(poly.size() + 4);
        const size_t count = poly.size();
        for (size_t i = 0; i < count; ++i)
        {
            const LLVector3& a = poly[i];
            const LLVector3& b = poly[(i + 1) % count];
            const F32 da = n * (a - o) - c;
            const F32 db = n * (b - o) - c;
            if (da >= 0.f)
            {
                out.push_back(a);
            }
            if ((da >= 0.f) != (db >= 0.f))
            {
                const F32 t = da / (da - db);
                out.push_back(a + (b - a) * t);
            }
        }
        poly.swap(out);
    }

    // The mirror reflects every glossy surface inside its probe box, not only the ones on the plane
    // through its middle, and a surface off that plane reads the reflection along directions that
    // cross the middle plane elsewhere: nearer the camera than the middle plane, further out (from the
    // camera's foot on the plane), by D / (D - offset) with D the camera's distance to the plane.
    // Carries p, on a surface parallel to the mirror, along its reflected ray from virtual_eye to the
    // middle plane. False when the reflected ray doesn't reach it (the camera inside the box's
    // thickness), so no view through the mirror covers it.
    bool fs_to_middle_plane(const LLVector3& p, const LLVector3& cam, const LLVector3& virtual_eye,
                            const LLVector3& mirror_pos, const LLVector3& normal, LLVector3& q)
    {
        LLVector3 r = p - cam;
        r -= normal * (2.f * (r * normal));
        const F32 rn = r * normal;
        const F32 depth = (mirror_pos - virtual_eye) * normal;
        if (rn <= 0.02f * r.length() || depth <= 0.f)
        {
            return false;
        }
        q = virtual_eye + r * (depth / rn);
        return true;
    }

    // The rectangle of the mirror's sides around centre (the middle plane, or the probe box's front
    // or back face)
    void fs_face_rect(std::vector<LLVector3>& poly, const LLVector3& centre, const LLVector3& ax, const LLVector3& ay)
    {
        poly.clear();
        poly.push_back(centre - ax - ay);
        poly.push_back(centre + ax - ay);
        poly.push_back(centre + ax + ay);
        poly.push_back(centre - ax + ay);
    }
}

namespace FSMirrorPlanar
{
    bool enabled()
    {
        static LLCachedControl<bool> planar(gSavedSettings, "FSRenderMirrorPlanar", false);
        return planar && LLPipeline::RenderMirrors;
    }

    const View* currentView()
    {
        return sInView ? &sView : nullptr;
    }

    void recordCamera(const LLCamera& camera)
    {
        if (!sInView)
        {
            return;
        }
        const F32 tan_y = tanf(camera.getView() * 0.5f);
        const F32 tan_x = tan_y * camera.getAspect();
        if (tan_x <= 0.f || tan_y <= 0.f)
        {
            return;
        }
        const LLVector3 right = -camera.getLeftAxis();
        const LLVector3 up = camera.getUpAxis();
        const LLVector3 at = camera.getAtAxis();

        // Row major, uploaded transposed: a world space direction d goes to (x, y, -, w) with
        // x / w and y / w the planar view's normalized device coordinates. Only x, y and w, so the
        // depth convention (reverse-Z or not) doesn't enter into it.
        const F32 m[16] = {
            right.mV[VX] / tan_x, right.mV[VY] / tan_x, right.mV[VZ] / tan_x, 0.f,
            up.mV[VX] / tan_y,    up.mV[VY] / tan_y,    up.mV[VZ] / tan_y,    0.f,
            0.f,                  0.f,                  0.f,                  0.f,
            at.mV[VX],            at.mV[VY],            at.mV[VZ],            0.f
        };
        memcpy(sMatrix, m, sizeof(sMatrix));
        sRecorded = true;
    }

    F32 drawDistance(F32 main_far)
    {
        static LLCachedControl<F32> distance(gSavedSettings, "FSRenderMirrorDrawDistance", 128.f);
        if (distance <= 0.f)
        {
            return main_far;
        }
        return llmin(main_far, llclamp((F32)distance, 16.f, 1024.f));
    }

    bool forceDynamic()
    {
        static LLCachedControl<bool> avatars(gSavedSettings, "FSRenderMirrorAvatars", false);
        return avatars;
    }

    F32 selectionScore(LLVOVolume* vo, const LLVector3& normal, const LLVector3& camera_pos)
    {
        const LLVector3 offset = camera_pos - vo->getPositionAgent();
        const F32 dist2 = llmax(offset.lengthSquared(), 0.01f);
        const LLVector3 scale = vo->getScale();
        const F32 area = scale.mV[VX] * scale.mV[VY];
        const F32 facing = (normal * offset) / sqrtf(dist2);
        // roughly the solid angle it covers; a mirror seen edge on still counts a little
        return area * llmax(facing, 0.05f) / dist2;
    }

    bool render(LLVOVolume* hero, const LLVector3& mirror_pos, const LLVector3& mirror_normal,
                const LLVector3& virtual_eye, LLCubeMapArray* cube, bool dynamic, U32 cube_res, U32 cube_mips)
    {
        if (!enabled() || !hero || hero->isDead() || hero->mDrawable.isNull())
        {
            return false;
        }
        LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;

        // Only flat mirrors: a thick or curved one reflects in directions far outside the mirror
        const LLVector3 scale = hero->getScale();
        const F32 smaller = llmin(scale.mV[VX], scale.mV[VY]);
        if (smaller <= 0.f || scale.mV[VZ] > smaller * MAX_THICKNESS)
        {
            return false;
        }

        fs_set_cull_plane(mirror_pos, mirror_normal);

        const LLQuaternion rot = hero->mDrawable->getWorldRotation();
        const LLVector3 ax = LLVector3::x_axis * rot * (scale.mV[VX] * 0.5f);
        const LLVector3 ay = LLVector3::y_axis * rot * (scale.mV[VY] * 0.5f);
        std::vector<LLVector3> poly;
        poly.reserve(12);
        poly.push_back(mirror_pos - ax - ay);
        poly.push_back(mirror_pos + ax - ay);
        poly.push_back(mirror_pos + ax + ay);
        poly.push_back(mirror_pos - ax + ay);

        // The part of the mirror the camera sees. A snapshot may be wider than the window and its
        // tiles see other parts, so then the whole mirror in front of the camera.
        const LLViewerCamera& cam = LLViewerCamera::instance();
        const LLVector3 cam_pos = cam.getOrigin();
        const LLVector3 at = cam.getAtAxis();
        const LLVector3 right = -cam.getLeftAxis();
        const LLVector3 up = cam.getUpAxis();
        const F32 tan_y = tanf(cam.getView() * 0.5f);
        const F32 tan_x = tan_y * cam.getAspect();
        const bool snapshot = gSnapshot;

        auto clip_to_view = [&](std::vector<LLVector3>& face)
        {
            fs_clip(face, at, cam_pos, cam.getNear());
            if (!snapshot)
            {
                const F32 kx = tan_x * SCREEN_MARGIN;
                const F32 ky = tan_y * SCREEN_MARGIN;
                fs_clip(face, at * kx - right, cam_pos, 0.f);
                fs_clip(face, at * kx + right, cam_pos, 0.f);
                fs_clip(face, at * ky - up, cam_pos, 0.f);
                fs_clip(face, at * ky + up, cam_pos, 0.f);
            }
        };
        clip_to_view(poly);
        if (poly.size() < 3)
        {
            sMode = -1;
            return true;
        }

        // Its size on screen
        F32 min_x = F32_MAX, max_x = -F32_MAX, min_y = F32_MAX, max_y = -F32_MAX;
        for (const LLVector3& p : poly)
        {
            const LLVector3 d = p - cam_pos;
            const F32 z = llmax(d * at, 1e-4f);
            const F32 x = (d * right) / (z * tan_x);
            const F32 y = (d * up) / (z * tan_y);
            min_x = llmin(min_x, x);
            max_x = llmax(max_x, x);
            min_y = llmin(min_y, y);
            max_y = llmax(max_y, y);
        }
        const LLRect view_rect = gViewerWindow->getWorldViewRectRaw();
        const F32 view_w = (F32)llmax(view_rect.getWidth(), 1);
        const F32 view_h = (F32)llmax(view_rect.getHeight(), 1);
        const F32 extent_px = llmin(llmax((max_x - min_x) * 0.5f * view_w, (max_y - min_y) * 0.5f * view_h), llmax(view_w, view_h));
        if (extent_px < MIN_SCREEN_PIXELS)
        {
            sMode = -1;
            return true;
        }
        sFade = llclamp((extent_px - MIN_SCREEN_PIXELS) / (FADE_SCREEN_PIXELS - MIN_SCREEN_PIXELS), 0.f, 1.f);

        // What the view has to cover: the directions read by every glossy surface in the probe box
        // the camera sees, not only by the ones on its middle plane. A surface nearer the camera
        // reads further out on the middle plane, up to D / (D - half its thickness) times as far from
        // the camera's foot on it, so the box's front and back faces, carried along their reflected
        // rays to the middle plane, bound them. (Fitted to the middle plane alone, a mirror surface
        // in front of it showed the ordinary reflections past a line across it when seen up close.)
        std::vector<LLVector3> reach(poly);
        const LLVector3 thick = mirror_normal * (scale.mV[VZ] * 0.5f);
        if (thick.lengthSquared() > 1e-8f)
        {
            std::vector<LLVector3> face;
            face.reserve(12);
            for (F32 side : { 1.f, -1.f })
            {
                fs_face_rect(face, mirror_pos + thick * side, ax, ay);
                clip_to_view(face);
                for (const LLVector3& p : face)
                {
                    LLVector3 q;
                    if (!fs_to_middle_plane(p, cam_pos, virtual_eye, mirror_pos, mirror_normal, q))
                    {
                        return false; // too close to the mirror for one view: the cube
                    }
                    reach.push_back(q);
                }
            }
        }

        // The planar view from the reflected eye, around those points. The camera stays an ordinary
        // (not mirrored) camera at the reflected eye, as for the cube faces, so winding and culling
        // are the same as for any other view.
        LLVector3 look;
        for (const LLVector3& p : reach)
        {
            LLVector3 d = p - virtual_eye;
            const F32 len = d.normVec();
            if (len < 1e-3f)
            {
                return false;
            }
            look += d;
        }
        if (look.normVec() < 1e-3f)
        {
            return false;
        }
        // the camera's up seen in the mirror, so the view is about as upright as the reflection
        LLVector3 up_hint = up - mirror_normal * (2.f * (up * mirror_normal));
        if (fabsf(up_hint * look) > 0.9f)
        {
            const LLVector3 left = cam.getLeftAxis();
            up_hint = left - mirror_normal * (2.f * (left * mirror_normal));
        }
        LLVector3 view_left = up_hint % look;
        if (view_left.normVec() < 1e-4f)
        {
            return false;
        }
        const LLVector3 view_up = look % view_left;

        F32 fit_x = 0.f, fit_y = 0.f, z_min = F32_MAX;
        for (const LLVector3& p : reach)
        {
            const LLVector3 d = p - virtual_eye;
            const F32 z = d * look;
            if (z <= 0.02f * d.length())
            {
                return false;
            }
            fit_x = llmax(fit_x, fabsf(d * view_left) / z);
            fit_y = llmax(fit_y, fabsf(d * view_up) / z);
            z_min = llmin(z_min, z);
        }
        fit_x = fit_x * FIT_MARGIN + 0.002f;
        fit_y = fit_y * FIT_MARGIN + 0.002f;
        const F32 max_tan = (sMode == 1 && sLastHero == hero) ? MAX_HALF_TAN_KEEP : MAX_HALF_TAN_START;
        if (fit_x > max_tan || fit_y > max_tan)
        {
            return false;
        }

        // Update rate: a mirror small on screen every second or third frame, but at once when the
        // mirror changes or the camera moves enough for the old view to be visibly off
        static LLCachedControl<bool> adaptive(gSavedSettings, "FSRenderMirrorPlanarAdaptiveRate", true);
        U32 interval = 1;
        if (adaptive && !snapshot)
        {
            const F32 frac = extent_px / view_h;
            interval = frac >= 0.25f ? 1 : (frac >= 0.1f ? 2 : 3);
        }
        const bool due = !sPlanarValid || sMode != 1 || hero != sLastHero || snapshot ||
                         (gFrameCount - sLastRenderFrame) >= interval ||
                         dist_vec(virtual_eye, sLastEye) > 0.01f * z_min;
        if (!due)
        {
            return true;
        }

        // Hidden behind walls in the main view's last occlusion test (the whole group the mirror is in):
        // keep the last planar view rather than render one nobody sees
        if (sPlanarValid && sMode == 1 && hero == sLastHero && !snapshot && LLPipeline::sUseOcclusion > 0)
        {
            LLSpatialGroup* group = hero->mDrawable->getSpatialGroup();
            if (group && group->fsHiddenFor(LLFrameTimer::getElapsedSeconds(), 0.f))
            {
                return true;
            }
        }

        // Render target size from the size on screen
        static LLCachedControl<F32> res_scale(gSavedSettings, "FSRenderMirrorPlanarScale", 1.f);
        static LLCachedControl<U32> max_res(gSavedSettings, "FSRenderMirrorPlanarMaxResolution", 1024);
        U32 cap = 256;
        while (cap * 2 <= llclamp((U32)max_res, 256U, 2048U))
        {
            cap *= 2;
        }
        const F32 want = extent_px * llclamp((F32)res_scale, 0.25f, 2.f);
        U32 res = want < SMALL_TARGET_PIXELS ? 256 : llmin(512U, cap);
        while ((F32)res < want && res < cap)
        {
            res *= 2;
        }
        const bool ours = fs_hero_rt_is_ours();
        if (ours && res < sRequestedRes)
        {
            // shrink only after it has wanted less for a while, and never in a snapshot
            if (snapshot || ++sShrinkCount < SHRINK_DELAY)
            {
                res = sRequestedRes;
            }
        }
        else
        {
            sShrinkCount = 0;
        }
        if (!ours || res != sRequestedRes)
        {
            fs_alloc_hero_rt(res);
        }
        sCubeCount = 0;
        if (!gPipeline.mHeroProbeRT.screen.isComplete())
        {
            sMode = -1;
            return true;
        }

        static LLCachedControl<S32> probe_detail(gSavedSettings, "RenderReflectionProbeDetail", 1);
        dynamic = dynamic || (probe_detail > 0 && hero->getReflectionProbeIsDynamic());

        sView.mAt = look;
        sView.mUp = view_up;
        sView.mFov = 2.f * atanf(fit_y);
        sView.mAspect = fit_x / fit_y;
        // Everything nearer than the mirror is behind it: a near plane at half its nearest point
        // gives the view far better depth precision than the cube's 1 cm
        const F32 near_clip = llclamp(z_min * 0.5f, 0.1f, 512.f);

        sInView = true;
        sRecorded = false;
        // display_cube_face is never meant to run inside a snapshot render; the planar view is an
        // ordinary capture of its own, the snapshot's state comes back with the flag
        const bool saved_snapshot = gSnapshot;
        gSnapshot = false;
        gPipeline.mRT = &gPipeline.mHeroProbeRT;
        gViewerWindow->cubeSnapshot(virtual_eye, cube, 0, 0, near_clip, dynamic, false, LLPlane(LLVector3(0, 0, 0), LLVector3(0, 0, 1)));
        gPipeline.mRT = &gPipeline.mMainRT;
        gSnapshot = saved_snapshot;
        sInView = false;

        if (!sRecorded)
        {
            sMode = -1;
            sPlanarValid = false;
            return true;
        }

        // Keep it, with mips for rough mirrors: the mirror target is drawn over again next time
        LLRenderTarget& screen = gPipeline.mHeroProbeRT.screen;
        const U32 w = screen.getWidth();
        const U32 h = screen.getHeight();
        static LLCachedControl<bool> render_hdr(gSavedSettings, "RenderHDREnabled", true);
        const U32 fmt = render_hdr ? GL_RGBA16F : GL_RGBA8;
        static U32 s_fmt = 0;
        if (!sTarget.isComplete() || sTarget.getWidth() != w || sTarget.getHeight() != h || s_fmt != fmt)
        {
            sTarget.release();
            if (!sTarget.allocate(w, h, fmt, false, LLTexUnit::TT_TEXTURE, LLTexUnit::TMG_MANUAL))
            {
                sMode = -1;
                sPlanarValid = false;
                return true;
            }
            s_fmt = fmt;
            // new texture memory is not cleared on every driver (AMD)
            sTarget.bindTarget();
            glClearColor(0.f, 0.f, 0.f, 0.f);
            sTarget.clear();
            sTarget.flush();
        }
        sTarget.copyContents(screen, 0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        {
            LL_PROFILE_GPU_ZONE("mirror planar mips");
            sTarget.bindTexture(0, 0, LLTexUnit::TFO_TRILINEAR);
            glGenerateMipmap(GL_TEXTURE_2D);
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        }

        // Roughness blur to match the cube: the cube spends 90 degrees on cube_res texels, the
        // planar view 2 * atan(fit) on its width
        const F32 cube_texel = F_PI_BY_TWO / (F32)llmax(cube_res, 1U);
        const F32 planar_texel = 2.f * llmax(fit_x, fit_y) / (F32)llmax(llmax(w, h), 1U);
        sTexelRatio = cube_texel / llmax(planar_texel, 1e-6f);
        sCubeMipSlope = (F32)llmax((S32)cube_mips - 1, 0);

        sMode = 1;
        sPlanarValid = true;
        sLastHero = hero;
        sLastEye = virtual_eye;
        sLastRenderFrame = gFrameCount;
        return true;
    }

    bool useCube(U32 cube_res, const LLVector3& mirror_pos, const LLVector3& mirror_normal)
    {
        // the cube wasn't rendered while the planar view (or nothing) was in use
        const bool stale = sMode != 0;
        sMode = 0;
        sPlanarValid = false;
        sLastHero = nullptr;
        if (enabled())
        {
            fs_set_cull_plane(mirror_pos, mirror_normal);
        }
        else
        {
            sCullValid = false;
        }

        // The cube works with any size of mirror target, so it goes back to the cube's size only
        // once the cube has been in use for a while
        if (sRequestedRes != 0 && ++sCubeCount >= CUBE_RESTORE_DELAY)
        {
            if (fs_hero_rt_is_ours() && sRequestedRes != cube_res)
            {
                fs_alloc_hero_rt(cube_res);
            }
            sRequestedRes = 0;
            sCubeCount = 0;
            sTarget.release();
        }
        return stale;
    }

    U32 unreachedCubeFaces(LLVOVolume* hero, const LLVector3& mirror_pos, const LLVector3& virtual_eye)
    {
        if (!hero || hero->isDead() || hero->mDrawable.isNull())
        {
            return 0;
        }
        // Only a flat mirror limits its reflections to the directions through it; a curved one can
        // send them anywhere
        const LLVector3 scale = hero->getScale();
        const F32 smaller = llmin(scale.mV[VX], scale.mV[VY]);
        if (smaller <= 0.f || scale.mV[VZ] > smaller * MAX_THICKNESS)
        {
            return 0;
        }
        // the mirror, a little larger: normal maps bend the reflection past its edges
        constexpr F32 GROW = 1.15f;
        const LLQuaternion rot = hero->mDrawable->getWorldRotation();
        const LLVector3 ax = LLVector3::x_axis * rot * (scale.mV[VX] * 0.5f * GROW);
        const LLVector3 ay = LLVector3::y_axis * rot * (scale.mV[VY] * 0.5f * GROW);
        const LLVector3 corners[4] = { mirror_pos - ax - ay, mirror_pos + ax - ay, mirror_pos + ax + ay, mirror_pos - ax + ay };

        // Face order and orientation of LLViewerWindow::cubeSnapshot: +X, -X, +Y, -Y, +Z, -Z. A face
        // sees the directions d with |d.u| <= d.look and |d.v| <= d.look.
        static const LLVector3 axes[3] = { LLVector3::x_axis, LLVector3::y_axis, LLVector3::z_axis };
        U32 mask = 0;
        std::vector<LLVector3> poly;
        for (U32 face = 0; face < 6; ++face)
        {
            const U32 a = face / 2;
            const LLVector3 look = (face & 1) ? -axes[a] : axes[a];
            const LLVector3& u = axes[(a + 1) % 3];
            const LLVector3& v = axes[(a + 2) % 3];
            poly.assign(corners, corners + 4);
            fs_clip(poly, look - u, virtual_eye, 0.f);
            fs_clip(poly, look + u, virtual_eye, 0.f);
            fs_clip(poly, look - v, virtual_eye, 0.f);
            fs_clip(poly, look + v, virtual_eye, 0.f);
            if (poly.size() < 3)
            {
                mask |= 1U << face;
            }
        }
        // never all six: something went wrong with the geometry, render as before
        return mask == 0x3f ? 0 : mask;
    }

    bool cullPlane(LLPlane& plane)
    {
        if (!sCullValid || !enabled() || !gPipeline.mHeroProbeManager.isMirrorPass() ||
            LLViewerCamera::sCurCameraID != LLViewerCamera::CAMERA_WORLD)
        {
            return false;
        }
        plane = sCullPlane;
        return true;
    }

    void bind(LLGLSLShader& shader, bool set_uniforms)
    {
        if (!enabled())
        {
            return;
        }
        // Reflection probe captures and the mirror's own view don't read the planar view: it is
        // for the main camera's eye (and the mirror would see itself a frame late). The cube isn't
        // rendered while the planar view is in use, so they don't read that either.
        S32 mode = sMode;
        if (mode == 1 && !sPlanarValid)
        {
            mode = -1;
        }
        if (gCubeSnapshot && mode != 0)
        {
            mode = -1;
        }

        if (set_uniforms)
        {
            static LLStaticHashedString s_mat("fs_hero_planar_mat");
            static LLStaticHashedString s_params("fs_hero_planar_params");
            shader.uniformMatrix4fv(s_mat, 1, GL_TRUE, sMatrix);
            // the planar view's mode carries its weight: 1 + weight
            shader.uniform4f(s_params, mode == 1 ? 1.f + sFade : (F32)mode, sCubeMipSlope, sTexelRatio, EDGE_FADE);
        }

        if (mode == 1)
        {
            const S32 channel = shader.enableTexture(LLShaderMgr::FS_HERO_PLANAR_MAP);
            if (channel > -1)
            {
                sTarget.bindTexture(0, channel, LLTexUnit::TFO_TRILINEAR);
            }
        }
    }

    bool updateDuringSnapshot()
    {
        return enabled();
    }

    void cleanup()
    {
        sTarget.release();
        sMode = 0;
        sFade = 1.f;
        sPlanarValid = false;
        sRequestedRes = 0;
        sAllocatedTex = 0;
        sAllocatedWidth = 0;
        sShrinkCount = 0;
        sCubeCount = 0;
        sLastHero = nullptr;
        sCullValid = false;
    }
}
