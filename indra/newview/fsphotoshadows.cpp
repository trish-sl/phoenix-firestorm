/**
 * @file fsphotoshadows.cpp
 * @brief Photo shadows: sun shadows blended over many frames while the camera is still
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
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
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "fsphotoshadows.h"

#include "llenvironment.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llshadermgr.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerdisplay.h"
#include "pipeline.h"

LLGLSLShader gFSPhotoShadowAccumProgram;
LLGLSLShader gFSPhotoShadowApplyProgram;

extern bool gSnapshot;
extern bool gShiftFrame;

namespace
{
    constexpr U32 NEAR_CASCADES = 2;            // cascades 0 and 1 are jittered
    // The sun's angular radius: the physical 0.53 degree disc, as the PCSS light size takes it
    constexpr F32 SUN_HALF_ANGLE = 0.5f * 0.53f * DEG_TO_RAD;

    bool sActive = false;           // this frame blends
    bool sHistoryValid = false;     // the history holds the blend so far for this view
    bool sResetPending = true;      // start over at the next active frame (a snapshot tile)
    U32 sSample = 0;                // this frame's sample index: 0 is the sun's centre
    F32 sDiscX = 0.f;               // this frame's point on the unit disc
    F32 sDiscY = 0.f;
    F32 sTexelX = 0.f;              // and its shadow map shift, in texels (-0.5 to 0.5)
    F32 sTexelY = 0.f;

    // The camera of the last frame, to tell a still one
    LLVector3 sPrevOrigin;
    LLVector3 sPrevAt;
    LLVector3 sPrevUp;
    F32 sPrevView = 0.f;
    F32 sPrevAspect = 0.f;
    U32 sPrevWidth = 0;
    U32 sPrevHeight = 0;

    // Heap allocated: a static LLRenderTarget would release GL objects after the context is gone
    LLRenderTarget* sHistory[2] = { nullptr, nullptr };
    U32 sHistoryIndex = 0;          // the one written last

    LLStaticHashedString sParams("fs_photo_params");

    bool setting_on()
    {
        static LLCachedControl<bool> photo(gSavedSettings, "FSRenderPhotoShadows", false);
        return photo;
    }

    F32 halton(U32 index, U32 base)
    {
        F32 f = 1.f;
        F32 r = 0.f;
        while (index > 0)
        {
            f /= (F32)base;
            r += f * (F32)(index % base);
            index /= base;
        }
        return r;
    }

    // Allocated at size and cleared (a new target's contents are undefined, on AMD they can hold
    // NaN, which the history would then carry on forever). True when (re)allocated.
    bool ensure_target(LLRenderTarget*& target, U32 width, U32 height)
    {
        if (target && target->isComplete() && target->getWidth() == width && target->getHeight() == height)
        {
            return false;
        }
        if (!target)
        {
            target = new LLRenderTarget();
        }
        target->release();
        if (!target->allocate(width, height, GL_RGBA16F))
        {
            target->release();
            return true;
        }
        target->bindTarget();
        glClearColor(0.f, 0.f, 0.f, 0.f);
        target->clear(GL_COLOR_BUFFER_BIT);
        target->flush();
        return true;
    }

    void delete_target(LLRenderTarget*& target)
    {
        if (target)
        {
            target->release();
            delete target;
            target = nullptr;
        }
    }

    bool same_vec(const LLVector3& a, const LLVector3& b, F32 eps)
    {
        return fabsf(a.mV[VX] - b.mV[VX]) <= eps && fabsf(a.mV[VY] - b.mV[VY]) <= eps && fabsf(a.mV[VZ] - b.mV[VZ]) <= eps;
    }

    // The sun's angular radius as drawn (the sky's sun scale) times FSRenderPhotoShadowsSoftness
    F32 disc_radius()
    {
        static LLCachedControl<F32> softness(gSavedSettings, "FSRenderPhotoShadowsSoftness", 1.f);
        LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
        const F32 sun_scale = sky ? llclamp(sky->getSunScale(), 0.25f, 20.f) : 1.f;
        return tanf(SUN_HALF_ANGLE * sun_scale * llclamp((F32)softness, 0.f, 8.f));
    }
}

namespace FSPhotoShadows
{
    void beginFrame(const LLRenderTarget& screen)
    {
        if (!setting_on() || !LLPipeline::sRenderDeferred || LLPipeline::RenderShadowDetail <= 0)
        {
            if (sHistory[0] || sHistory[1])
            {
                release();
            }
            sActive = false;
            sHistoryValid = false;
            sSample = 0;
            return;
        }

        const LLViewerCamera* camera = LLViewerCamera::getInstance();
        const LLVector3 origin = camera->getOrigin();
        const LLVector3 at = camera->getAtAxis();
        const LLVector3 up = camera->getUpAxis();
        const F32 view = camera->getView();
        const F32 aspect = camera->getAspect();
        const U32 width = screen.getWidth();
        const U32 height = screen.getHeight();

        bool still = same_vec(origin, sPrevOrigin, 1.e-4f) && same_vec(at, sPrevAt, 1.e-6f) && same_vec(up, sPrevUp, 1.e-6f) &&
                     fabsf(view - sPrevView) <= 1.e-6f && fabsf(aspect - sPrevAspect) <= 1.e-6f &&
                     width == sPrevWidth && height == sPrevHeight;
        if (gTeleportDisplay || gShiftFrame)
        {
            still = false;
        }
        sPrevOrigin = origin;
        sPrevAt = at;
        sPrevUp = up;
        sPrevView = view;
        sPrevAspect = aspect;
        sPrevWidth = width;
        sPrevHeight = height;

        // A snapshot blends from its first frame (rawSnapshot starts each tile over); an avatars only
        // one doesn't blend at all
        bool active = false;
        if (LLPipeline::isAvatarOnlyRender())
        {
            active = false;
        }
        else if (gSnapshot)
        {
            active = snapshotExtraFrames() > 0;
            if (!still)
            {
                sResetPending = true;
            }
        }
        else
        {
            active = still;
        }

        if (!active || width == 0 || height == 0)
        {
            sActive = false;
            sHistoryValid = false;
            sSample = 0;
            sResetPending = true;
            return;
        }
        sActive = true;

        if (sResetPending)
        {
            sHistoryValid = false;
            sSample = 0;
            sResetPending = false;
        }
        else
        {
            ++sSample;
        }

        if (sSample == 0)
        {
            // the sun's centre and the shadow map as it is: nothing changes until frames are blended
            sDiscX = sDiscY = 0.f;
            sTexelX = sTexelY = 0.f;
        }
        else
        {
            // Halton (2, 3) over the unit square, onto the disc with the same area everywhere
            // (Shirley's concentric map), so the samples cover it evenly at any count
            const F32 a = 2.f * halton(sSample, 2) - 1.f;
            const F32 b = 2.f * halton(sSample, 3) - 1.f;
            F32 r = 0.f;
            F32 phi = 0.f;
            if (a * a > b * b)
            {
                r = a;
                phi = F_PI_BY_TWO * 0.5f * (b / a);
            }
            else if (b != 0.f)
            {
                r = b;
                phi = F_PI_BY_TWO - F_PI_BY_TWO * 0.5f * (a / b);
            }
            sDiscX = r * cosf(phi);
            sDiscY = r * sinf(phi);
            // Halton (5, 7) for the texel shift, independent of the disc point
            sTexelX = halton(sSample, 5) - 0.5f;
            sTexelY = halton(sSample, 7) - 0.5f;
        }
    }

    bool frameActive()
    {
        return sActive;
    }

    LLVector3 jitterLightDir(U32 cascade, const LLVector3& light_dir)
    {
        if (!sActive || cascade >= NEAR_CASCADES || (sDiscX == 0.f && sDiscY == 0.f))
        {
            return light_dir;
        }
        LLVector3 dir = light_dir;
        if (dir.normVec() < 1.e-6f)
        {
            return light_dir;
        }
        LLVector3 u = dir % (fabsf(dir.mV[VZ]) < 0.9f ? LLVector3::z_axis : LLVector3::x_axis);
        u.normVec();
        LLVector3 v = dir % u;
        const F32 r = disc_radius();
        LLVector3 out = dir + (u * sDiscX + v * sDiscY) * r;
        out.normVec();
        return out;
    }

    void jitterProjection(U32 cascade, glm::mat4& proj, F32 map_width, F32 map_height)
    {
        if (!sActive || cascade >= NEAR_CASCADES || (sTexelX == 0.f && sTexelY == 0.f) || map_width < 1.f || map_height < 1.f)
        {
            return;
        }
        // a shift of the clip space x and y by this many NDC units: 2 / size per texel
        const F32 dx = sTexelX * 2.f / map_width;
        const F32 dy = sTexelY * 2.f / map_height;
        for (int c = 0; c < 4; ++c)
        {
            proj[c][0] += dx * proj[c][3];
            proj[c][1] += dy * proj[c][3];
        }
    }

    void accumulate(LLRenderTarget& light_map, LLRenderTarget& depth_source, LLVertexBuffer* screen_triangle)
    {
        if (!sActive || !screen_triangle || !gFSPhotoShadowAccumProgram.isComplete() || !gFSPhotoShadowApplyProgram.isComplete())
        {
            return;
        }
        const U32 width = light_map.getWidth();
        const U32 height = light_map.getHeight();
        if (!width || !height)
        {
            return;
        }
        LL_PROFILE_GPU_ZONE("photo shadows");

        bool reallocated = ensure_target(sHistory[0], width, height);
        reallocated = ensure_target(sHistory[1], width, height) || reallocated;
        if (!sHistory[0]->isComplete() || !sHistory[1]->isComplete())
        {
            return;
        }
        if (reallocated)
        {
            sHistoryValid = false;
        }

        LLRenderTarget* prev = sHistory[sHistoryIndex];
        LLRenderTarget* next = sHistory[1 - sHistoryIndex];

        static LLCachedControl<U32> max_frames(gSavedSettings, "FSRenderPhotoShadowsFrames", 64);
        // a snapshot weighs all its frames the same
        const F32 frames = (F32)llmax(llclamp((U32)max_frames, 4U, 1024U), gSnapshot ? snapshotExtraFrames() + 1 : 0U);

        LLGLDisable blend(GL_BLEND);
        LLGLDepthTest depth(GL_FALSE, GL_FALSE);

        // the blend into the next history
        {
            LLGLSLShader& shader = gFSPhotoShadowAccumProgram;
            next->bindTarget();
            shader.bind();
            shader.bindTexture(LLShaderMgr::DEFERRED_LIGHT, &light_map, false, LLTexUnit::TFO_POINT);
            shader.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, prev, false, LLTexUnit::TFO_POINT);
            shader.bindTexture(LLShaderMgr::DEFERRED_DEPTH, &depth_source, true);
            shader.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)width, (F32)height);
            // x: the history is valid, y: most frames blended, z: view depth change that starts a
            // pixel over (share of its depth), w: how far outside this frame's neighbourhood the
            // history may be before the pixel starts over
            shader.uniform4f(sParams, sHistoryValid ? 1.f : 0.f, frames, 0.02f, 0.5f);
            screen_triangle->setBuffer();
            screen_triangle->drawArrays(LLRender::TRIANGLES, 0, 3);
            shader.unbind();
            next->flush();
        }

        // the blend back into the light map's sun channel; the other channels stay
        {
            LLGLSLShader& shader = gFSPhotoShadowApplyProgram;
            light_map.bindTarget();
            gGL.setColorMask(true, false, false, false);
            shader.bind();
            shader.bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, next, false, LLTexUnit::TFO_POINT);
            screen_triangle->setBuffer();
            screen_triangle->drawArrays(LLRender::TRIANGLES, 0, 3);
            shader.unbind();
            gGL.setColorMask(true, true);
            light_map.flush();
        }

        sHistoryIndex = 1 - sHistoryIndex;
        sHistoryValid = true;
    }

    U32 snapshotExtraFrames()
    {
        static LLCachedControl<U32> snapshot_frames(gSavedSettings, "FSRenderPhotoShadowsSnapshotFrames", 24);
        if (!setting_on() || !LLPipeline::sRenderDeferred || LLPipeline::RenderShadowDetail <= 0)
        {
            return 0;
        }
        return llmin((U32)snapshot_frames, 256U);
    }

    void beginSnapshotTile()
    {
        sResetPending = true;
    }

    void release()
    {
        delete_target(sHistory[0]);
        delete_target(sHistory[1]);
        sHistoryIndex = 0;
        sHistoryValid = false;
        sResetPending = true;
    }
}
