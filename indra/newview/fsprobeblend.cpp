/**
 * @file fsprobeblend.cpp
 * @brief Reflection probe refreshes blended in over a fraction of a second (FSRenderProbeRefreshBlend)
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

#include "fsprobeblend.h"

#include "llappviewer.h"
#include "llcubemaparray.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llreflectionmap.h"
#include "llreflectionmapmanager.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llshadermgr.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llstaticstringtable.h"
#include "v3math.h"
#include "llcoordframe.h"

namespace
{
    // the SH strip of one probe: LL_SH_COEFF_COUNT texels in a row
    constexpr U32 SH_COEFFS = 9;

    LLGLSLShader sRadianceProgram;  // radianceGenV.glsl + fsProbeBlendF.glsl
    LLGLSLShader sSHProgram;        // irradianceGenV.glsl + fsProbeBlendSHF.glsl
    bool sProgramsTried = false;

    // Rows 0 and 1 hold the old and new coefficients; row 2 saves the on-screen blend during capture.
    LLRenderTarget sSHStash;

    struct Blend
    {
        LLPointer<LLReflectionMap> mProbe;
        LLPointer<LLCubeMapArray> mArray;   // radiance: the cube array it was copied in
        S32 mIndex = -1;                    // the probe's slot (cube index, SH row)
        F32 mStart = 0.f;
        bool mActive = false;

        void clear()
        {
            mProbe = nullptr;
            mArray = nullptr;
            mIndex = -1;
            mActive = false;
        }
    };
    Blend sRadiance;
    Blend sIrradiance;

    bool enabled()
    {
        static LLCachedControl<bool> on(gSavedSettings, "FSRenderProbeRefreshBlend", false);
        return on;
    }

    F32 blend_time()
    {
        static LLCachedControl<F32> t(gSavedSettings, "FSRenderProbeRefreshBlendTime", 0.3f);
        return llclamp((F32)t, 0.05f, 2.f);
    }

    bool copies_possible()
    {
        return gGLManager.mHasCubeMapArray && gGLManager.mGLVersion >= 4.29f && glCopyImageSubData != nullptr;
    }

    bool programs_ready()
    {
        if (sProgramsTried)
        {
            return sRadianceProgram.isComplete() && sSHProgram.isComplete();
        }
        sProgramsTried = true;

        const S32 level = LLViewerShaderMgr::instance()->getShaderLevel(LLViewerShaderMgr::SHADER_INTERFACE);
        sRadianceProgram.mName = "FS Probe Refresh Blend";
        sRadianceProgram.mShaderFiles.clear();
        sRadianceProgram.mShaderFiles.push_back(std::make_pair("interface/radianceGenV.glsl", GL_VERTEX_SHADER));
        sRadianceProgram.mShaderFiles.push_back(std::make_pair("interface/fsProbeBlendF.glsl", GL_FRAGMENT_SHADER));
        sRadianceProgram.mShaderLevel = level;

        sSHProgram.mName = "FS Probe Refresh Blend SH";
        sSHProgram.mShaderFiles.clear();
        sSHProgram.mShaderFiles.push_back(std::make_pair("interface/irradianceGenV.glsl", GL_VERTEX_SHADER));
        sSHProgram.mShaderFiles.push_back(std::make_pair("interface/fsProbeBlendSHF.glsl", GL_FRAGMENT_SHADER));
        sSHProgram.mShaderLevel = level;

        if (!sRadianceProgram.createShader() || !sSHProgram.createShader())
        {
            LL_WARNS("ShaderLoading") << "Probe refresh blend programs failed to build, probes refresh at once" << LL_ENDL;
            sRadianceProgram.unload();
            sSHProgram.unload();
            return false;
        }
        return true;
    }

    // all filtered mips of cube src into cube dst of the same array
    void copy_cube(LLCubeMapArray* array, U32 src, U32 dst, U32 mips, U32 res)
    {
        const GLuint name = array->getGLName();
        for (U32 i = 0; i < mips; ++i)
        {
            glCopyImageSubData(name, GL_TEXTURE_CUBE_MAP_ARRAY, i, 0, 0, src * 6,
                               name, GL_TEXTURE_CUBE_MAP_ARRAY, i, 0, 0, dst * 6,
                               res, res, 6);
            res = llmax(res / 2, 1U);
        }
    }

    bool stash_ready()
    {
        if (sSHStash.isComplete())
        {
            return true;
        }
        if (!sSHStash.allocate(SH_COEFFS, 3, GL_RGBA16F))
        {
            return false;
        }
        // defined contents before the first copy (some drivers hand out garbage, NaNs included)
        sSHStash.bindTarget();
        glClearColor(0.f, 0.f, 0.f, 0.f);
        sSHStash.clear(GL_COLOR_BUFFER_BIT);
        sSHStash.flush();
        return true;
    }

    // one SH row of src (bound as the read framebuffer) into row dst_row of the texture of dst
    void copy_sh_row(LLRenderTarget& src, U32 src_row, LLRenderTarget& dst, U32 dst_row)
    {
        src.bindTarget();
        LLTexUnit* unit = gGL.getTexUnit(0);
        unit->bind(&dst);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, dst_row, 0, src_row, SH_COEFFS, 1);
        unit->unbind(LLTexUnit::TT_TEXTURE);
        src.flush();
    }
}

U32 FSProbeBlend::extraLayers()
{
    return (enabled() && copies_possible()) ? 2 : 0;
}

// static
U32 FSProbeBlend::oldLayer(const LLReflectionMapManager& mgr)
{
    return mgr.mReflectionProbeCount + 2;
}

// static
U32 FSProbeBlend::newLayer(const LLReflectionMapManager& mgr)
{
    return mgr.mReflectionProbeCount + 3;
}

// static
bool FSProbeBlend::beginRadiance(LLReflectionMapManager& mgr, LLReflectionMap* probe)
{
    // Ends a running blend first: its new maps are in the spare cube about to be reused
    if (sRadiance.mActive)
    {
        sRadiance.mStart = -1.e6f;
        step(mgr);
    }
    if (!enabled() || !copies_possible() || !probe || mgr.mRealtimeUpdating || probe != mgr.mUpdatingProbe ||
        !probe->mComplete || probe->mFadeIn < 1.f || probe->mCubeIndex < 0 || mgr.mTexture.isNull() ||
        mgr.mTexture->getCount() < mgr.mReflectionProbeCount + 4 || mgr.mMipChain.empty() ||
        (U32)probe->mCubeIndex >= mgr.mReflectionProbeCount || !programs_ready())
    {
        return false;
    }
    copy_cube(mgr.mTexture, (U32)probe->mCubeIndex, oldLayer(mgr), (U32)mgr.mMipChain.size(), mgr.mMipChain[0].getWidth());
    return true;
}

// static
void FSProbeBlend::endRadiance(LLReflectionMapManager& mgr, LLReflectionMap* probe)
{
    const U32 slot = (U32)probe->mCubeIndex;
    const U32 mips = (U32)mgr.mMipChain.size();
    const U32 res = mgr.mMipChain[0].getWidth();
    copy_cube(mgr.mTexture, slot, newLayer(mgr), mips, res);
    // the old maps stay in the slot this frame; step() mixes the new ones in from the next
    copy_cube(mgr.mTexture, oldLayer(mgr), slot, mips, res);
    sRadiance.mProbe = probe;
    sRadiance.mArray = mgr.mTexture;
    sRadiance.mIndex = (S32)slot;
    sRadiance.mStart = (F32)gFrameTimeSeconds;
    sRadiance.mActive = true;
}

// static
bool FSProbeBlend::beginIrradiance(LLReflectionMapManager& mgr, LLReflectionMap* probe)
{
    if (sIrradiance.mActive)
    {
        sIrradiance.mStart = -1.e6f;
        step(mgr);
    }
    if (!enabled() || !copies_possible() || !probe || mgr.mRealtimeUpdating || probe != mgr.mUpdatingProbe ||
        !probe->mComplete || probe->mFadeIn < 1.f || probe->mCubeIndex < 0 ||
        (U32)probe->mCubeIndex >= mgr.mReflectionProbeCount || !mgr.mSHCoeffs.isComplete() ||
        !programs_ready() || !stash_ready())
    {
        return false;
    }
    copy_sh_row(mgr.mSHCoeffs, (U32)probe->mCubeIndex, sSHStash, 0);
    return true;
}

// static
void FSProbeBlend::endIrradiance(LLReflectionMapManager& mgr, LLReflectionMap* probe)
{
    const U32 row = (U32)probe->mCubeIndex;
    copy_sh_row(mgr.mSHCoeffs, row, sSHStash, 1);
    copy_sh_row(sSHStash, 0, mgr.mSHCoeffs, row);
    sIrradiance.mProbe = probe;
    sIrradiance.mIndex = (S32)row;
    sIrradiance.mStart = (F32)gFrameTimeSeconds;
    sIrradiance.mActive = true;
}

// The radiance pass must see the new irradiance, even while the main view is
// fading from the old coefficients. Restore the displayed blend after each face.
bool FSProbeBlend::beginRadianceCapture(LLReflectionMapManager& mgr, LLReflectionMap* probe)
{
    if (!sIrradiance.mActive || sIrradiance.mProbe != probe || !probe ||
        probe->mCubeIndex != sIrradiance.mIndex || !mgr.mSHCoeffs.isComplete() || !sSHStash.isComplete())
    {
        return false;
    }
    const U32 row = (U32)sIrradiance.mIndex;
    copy_sh_row(mgr.mSHCoeffs, row, sSHStash, 2);
    copy_sh_row(sSHStash, 1, mgr.mSHCoeffs, row);
    return true;
}

void FSProbeBlend::endRadianceCapture(LLReflectionMapManager& mgr, LLReflectionMap* probe)
{
    if (sIrradiance.mActive && sIrradiance.mProbe == probe && probe &&
        probe->mCubeIndex == sIrradiance.mIndex && mgr.mSHCoeffs.isComplete() && sSHStash.isComplete())
    {
        copy_sh_row(sSHStash, 2, mgr.mSHCoeffs, (U32)sIrradiance.mIndex);
    }
}

// static
void FSProbeBlend::step(LLReflectionMapManager& mgr)
{
    if (!sRadiance.mActive && !sIrradiance.mActive)
    {
        return;
    }
    LL_PROFILE_ZONE_SCOPED_CATEGORY_DISPLAY;

    // a blend only writes a slot its probe still owns
    auto owns = [&mgr](const Blend& b)
    {
        return b.mProbe.notNull() && b.mIndex >= 0 && b.mProbe->mCubeIndex == b.mIndex &&
               (size_t)b.mIndex < mgr.mSlotOwner.size() && mgr.mSlotOwner[b.mIndex] == b.mProbe.get();
    };
    const F32 duration = blend_time();
    const F32 now = (F32)gFrameTimeSeconds;

    gGL.setColorMask(true, true);
    LLGLDepthTest depth(GL_FALSE, GL_FALSE);
    LLGLDisable cull(GL_CULL_FACE);
    LLGLDisable blend(GL_BLEND);

    if (sRadiance.mActive)
    {
        LL_PROFILE_GPU_ZONE("probe refresh blend");
        if (!owns(sRadiance) || sRadiance.mArray != mgr.mTexture || mgr.mMipChain.empty() || mgr.mVertexBuffer.isNull())
        {
            sRadiance.clear(); // the slot's maps are someone else's now, or gone
        }
        else
        {
            const U32 slot = (U32)sRadiance.mIndex;
            const U32 mips = (U32)mgr.mMipChain.size();
            const U32 res0 = mgr.mMipChain[0].getWidth();
            const F32 t = (now - sRadiance.mStart) / duration;
            if (t >= 1.f || !programs_ready())
            {
                copy_cube(mgr.mTexture, newLayer(mgr), slot, mips, res0);
                sRadiance.clear();
            }
            else
            {
                static LLStaticHashedString sMipLevel("mipLevel");
                static LLStaticHashedString sOld("fs_blend_old");
                static LLStaticHashedString sNew("fs_blend_new");
                static LLStaticHashedString sT("fs_blend_t");

                gGL.matrixMode(LLRender::MM_MODELVIEW);
                gGL.pushMatrix();

                mgr.mMipChain[0].bindTarget();
                sRadianceProgram.bind();
                mgr.mVertexBuffer->setBuffer();
                const S32 channel = sRadianceProgram.enableTexture(LLShaderMgr::REFLECTION_PROBES, LLTexUnit::TT_CUBE_MAP_ARRAY);
                mgr.mTexture->bind(channel);
                sRadianceProgram.uniform1i(sOld, (S32)oldLayer(mgr));
                sRadianceProgram.uniform1i(sNew, (S32)newLayer(mgr));
                // eased, so the change starts and ends gently
                sRadianceProgram.uniform1f(sT, t * t * (3.f - 2.f * t));

                U32 res = res0;
                for (U32 i = 0; i < mips; ++i)
                {
                    glViewport(0, 0, res, res);
                    sRadianceProgram.uniform1f(sMipLevel, (GLfloat)i);
                    for (S32 cf = 0; cf < 6; ++cf)
                    {
                        LLCoordFrame frame;
                        frame.lookAt(LLVector3(0, 0, 0), LLCubeMapArray::sClipToCubeLookVecs[cf], LLCubeMapArray::sClipToCubeUpVecs[cf]);
                        F32 mat[16];
                        frame.getOpenGLRotation(mat);
                        gGL.loadMatrix(mat);
                        mgr.mVertexBuffer->drawArrays(gGL.TRIANGLE_STRIP, 0, 4);
                        glCopyTexSubImage3D(GL_TEXTURE_CUBE_MAP_ARRAY, i, 0, 0, slot * 6 + cf, 0, 0, res, res);
                    }
                    res = llmax(res / 2, 1U);
                }

                sRadianceProgram.disableTexture(LLShaderMgr::REFLECTION_PROBES, LLTexUnit::TT_CUBE_MAP_ARRAY);
                sRadianceProgram.unbind();
                mgr.mMipChain[0].flush();

                gGL.matrixMode(LLRender::MM_MODELVIEW);
                gGL.popMatrix();
            }
        }
    }

    if (sIrradiance.mActive)
    {
        if (!owns(sIrradiance) || !mgr.mSHCoeffs.isComplete() || !sSHStash.isComplete() || mgr.mVertexBuffer.isNull())
        {
            sIrradiance.clear();
        }
        else
        {
            const U32 row = (U32)sIrradiance.mIndex;
            const F32 t = (now - sIrradiance.mStart) / duration;
            if (t >= 1.f || !programs_ready())
            {
                copy_sh_row(sSHStash, 1, mgr.mSHCoeffs, row);
                sIrradiance.clear();
            }
            else
            {
                static LLStaticHashedString sT("fs_blend_t");
                mgr.mSHCoeffs.bindTarget();
                // this probe's row only: the viewport is the write mask, as for the projection
                glViewport(0, row, SH_COEFFS, 1);
                sSHProgram.bind();
                const S32 channel = sSHProgram.enableTexture(LLShaderMgr::DEFERRED_DIFFUSE);
                gGL.getTexUnit(channel)->bind(&sSHStash);
                sSHProgram.uniform1f(sT, t * t * (3.f - 2.f * t));
                mgr.mVertexBuffer->setBuffer();
                mgr.mVertexBuffer->drawArrays(gGL.TRIANGLE_STRIP, 0, 4);
                gGL.getTexUnit(channel)->unbind(LLTexUnit::TT_TEXTURE);
                sSHProgram.unbind();
                mgr.mSHCoeffs.flush();
            }
        }
    }
}

// static
void FSProbeBlend::cancel()
{
    sRadiance.clear();
    sIrradiance.clear();
}

// static
void FSProbeBlend::loadShaders()
{
    sRadianceProgram.unload();
    sSHProgram.unload();
    sProgramsTried = false;
}

// static
void FSProbeBlend::release()
{
    cancel();
    loadShaders();
    sSHStash.release();
}
