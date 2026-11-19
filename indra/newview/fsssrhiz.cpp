/**
 * @file fsssrhiz.cpp
 * @brief Scene pyramid for hierarchical-Z screen-space reflections (FSRenderSSRHiZ)
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

#include "fsssrhiz.h"

#include "llframetimer.h"
#include "llglslshader.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llshadermgr.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <vector>

extern bool gCubeSnapshot;

namespace
{
    LLGLSLShader sCopyProgram; // level 0: scene colour + linear depth
    LLGLSLShader sDownProgram; // level N from N-1: weighted colour, nearest depth

    const LLStaticHashedString sInvProj("ssr_inv_proj");
    const LLStaticHashedString sActive("ssrHiZActive");
    const LLStaticHashedString sSteps("ssrHiZSteps");
    const LLStaticHashedString sThickness("ssrThickness");
    const LLStaticHashedString sEdgeFade("ssrEdgeFade");
    const LLStaticHashedString sConeTrace("ssrConeTrace");

    // A size the pyramid could not be made at is not tried again every frame (each try makes and
    // frees the texture and waits for the render thread on the checks), only after a while
    constexpr F64 FAILED_RETRY_SECONDS = 30.0;

    // Set when the driver can't render a level while only the level above it is visible to the
    // sampler, and has no texture views to do it another way: hi-Z then stays off
    bool sUnsupported = false;

    // Per-level texture views (GL 4.3) let each level be rendered as its own texture while the
    // level above is sampled through another, so no level is ever outside the attached texture's
    // [BASE_LEVEL, MAX_LEVEL] range. AMD drivers reject the draw otherwise.
    bool levelViewsAvailable()
    {
        return glTextureView && glTexStorage2D && gGLManager.mGLVersion >= 4.29f;
    }

    struct Pyramid
    {
        U32 mTexture = 0;
        std::vector<U32> mViews; // one single-level view per mip level, empty without texture views
        std::vector<U32> mFBOs;  // one per mip level
        U32 mWidth = 0;
        U32 mHeight = 0;
        U32 mLevels = 0;
        U32 mFailedWidth = 0;
        U32 mFailedHeight = 0;
        F64 mFailedAt = 0.0;

        void release()
        {
            if (!mFBOs.empty())
            {
                glDeleteFramebuffers((GLsizei)mFBOs.size(), mFBOs.data());
                mFBOs.clear();
            }
            if (mTexture || !mViews.empty())
            {
                // Don't leave a texture unit pointing at a deleted name.
                for (U32 i = 0; i < LL_NUM_TEXTURE_LAYERS; ++i)
                {
                    LLTexUnit* unit = gGL.getTexUnit(i);
                    if (!unit || !unit->getCurrTexture())
                    {
                        continue;
                    }
                    const U32 bound = unit->getCurrTexture();
                    if (bound == mTexture || std::find(mViews.begin(), mViews.end(), bound) != mViews.end())
                    {
                        unit->unbind(LLTexUnit::TT_TEXTURE);
                    }
                }
            }
            // The views go first: they keep the parent's storage alive until they are deleted too
            if (!mViews.empty())
            {
                glDeleteTextures((GLsizei)mViews.size(), mViews.data());
                mViews.clear();
            }
            if (mTexture)
            {
                glDeleteTextures(1, &mTexture);
                mTexture = 0;
            }
            mWidth = 0;
            mHeight = 0;
            mLevels = 0;
        }

        bool allocate(U32 width, U32 height)
        {
            release();

            const F64 now = LLFrameTimer::getElapsedSeconds();
            if (sUnsupported || (mFailedWidth == width && mFailedHeight == height && now - mFailedAt < FAILED_RETRY_SECONDS))
            {
                return false;
            }

            U32 levels = 1;
            while ((llmax(width, height) >> levels) > 0)
            {
                ++levels;
            }

            glGenTextures(1, &mTexture);
            if (!mTexture)
            {
                return false;
            }

            const bool views = levelViewsAvailable();
            LLTexUnit* unit = gGL.getTexUnit(0);
            unit->bindManual(LLTexUnit::TT_TEXTURE, mTexture, true);
            clear_glerror();
            if (views)
            {
                glTexStorage2D(GL_TEXTURE_2D, (GLsizei)levels, GL_RGBA16F, (GLsizei)width, (GLsizei)height);
            }
            else
            {
                for (U32 level = 0; level < levels; ++level)
                {
                    glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA16F, llmax(width >> level, 1U), llmax(height >> level, 1U), 0, GL_RGBA,
                                 GL_HALF_FLOAT, nullptr);
                }
            }
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            unit->unbind(LLTexUnit::TT_TEXTURE);

            if (views)
            {
                // A view's name must never have been bound before glTextureView, so each is made
                // before it is bound for its parameters
                mViews.resize(levels, 0);
                glGenTextures((GLsizei)levels, mViews.data());
                for (U32 level = 0; level < levels; ++level)
                {
                    glTextureView(mViews[level], GL_TEXTURE_2D, mTexture, GL_RGBA16F, level, 1, 0, 1);
                    unit->bindManual(LLTexUnit::TT_TEXTURE, mViews[level], false);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                }
                unit->unbind(LLTexUnit::TT_TEXTURE);
            }
            bool ok = glGetError() == GL_NO_ERROR;

            mFBOs.resize(levels, 0);
            glGenFramebuffers((GLsizei)levels, mFBOs.data());
            {
                // Every level starts black at the far plane, so a level that is never drawn reads
                // as "nothing here" rather than as whatever was in video memory
                LLGLDisable scissor(GL_SCISSOR_TEST);
                const GLfloat clear[4] = { 0.f, 0.f, 0.f, 60000.f };
                for (U32 level = 0; ok && level < levels; ++level)
                {
                    glBindFramebuffer(GL_FRAMEBUFFER, mFBOs[level]);
                    if (views)
                    {
                        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mViews[level], 0);
                    }
                    else
                    {
                        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mTexture, level);
                    }
                    ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
                    if (ok)
                    {
                        glClearBufferfv(GL_COLOR, 0, clear);
                    }
                }
            }

            // Without views, the coarser levels are drawn with only the level above visible to the
            // sampler (BASE = MAX = level - 1). A driver that holds to the attachment range rejects
            // that; hi-Z is then switched off rather than traced through levels never drawn.
            if (ok && !views && levels > 1)
            {
                unit->bindManual(LLTexUnit::TT_TEXTURE, mTexture, true);
                glBindFramebuffer(GL_FRAMEBUFFER, mFBOs[1]);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
                const bool renderable = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
                unit->unbind(LLTexUnit::TT_TEXTURE);
                if (!renderable)
                {
                    LL_WARNS("Render") << "SSR hi-Z: the driver can't render the pyramid's levels and has no texture views, so hi-Z stays off" << LL_ENDL;
                    sUnsupported = true;
                    ok = false;
                }
            }
            glBindFramebuffer(GL_FRAMEBUFFER, LLRenderTarget::sCurFBO);

            if (!ok)
            {
                if (!sUnsupported && (mFailedWidth != width || mFailedHeight != height))
                {   // once per size, not at every retry
                    LL_WARNS("Render") << "SSR hi-Z pyramid " << width << "x" << height << " could not be allocated" << LL_ENDL;
                }
                mFailedWidth = width;
                mFailedHeight = height;
                mFailedAt = now;
                release();
                return false;
            }

            mFailedWidth = 0;
            mFailedHeight = 0;
            mWidth = width;
            mHeight = height;
            mLevels = levels;
            return true;
        }

        void drawLevel(U32 level, LLVertexBuffer* screen_triangle)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, mFBOs[level]);
            glViewport(0, 0, llmax(mWidth >> level, 1U), llmax(mHeight >> level, 1U));
            screen_triangle->setBuffer();
            screen_triangle->drawArrays(LLRender::TRIANGLES, 0, 3);
        }

        // Level 0 from color and depth_src (the same size), then the coarser levels
        void build(LLRenderTarget& color, LLRenderTarget& depth_src, LLVertexBuffer* screen_triangle)
        {
            const U32 width = color.getWidth();
            const U32 height = color.getHeight();
            if ((width != mWidth || height != mHeight || !mTexture) && !allocate(width, height))
            {
                return;
            }

            gGL.flush();

            LLGLDepthTest depth(GL_FALSE, GL_FALSE);
            LLGLDisable blend(GL_BLEND);
            LLGLDisable cull(GL_CULL_FACE);

            // Level 0. The sources are bound without touching their filtering: texelFetch ignores
            // it, and the classic SSR and the water keep sampling them as before.
            sCopyProgram.bind();
            S32 channel = sCopyProgram.enableTexture(LLShaderMgr::DEFERRED_DIFFUSE);
            if (channel > -1)
            {
                gGL.getTexUnit(channel)->bind(&color);
            }
            channel = sCopyProgram.enableTexture(LLShaderMgr::DEFERRED_DEPTH);
            if (channel > -1)
            {
                gGL.getTexUnit(channel)->bind(&depth_src, true);
            }
            // The sources were rendered with the current camera projection.
            glm::mat4 inv_proj = glm::inverse(get_current_projection());
            sCopyProgram.uniformMatrix4fv(sInvProj, 1, GL_FALSE, glm::value_ptr(inv_proj));
            sCopyProgram.uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, (F32)width, (F32)height);
            drawLevel(0, screen_triangle);
            sCopyProgram.unbind();

            // Levels 1..N. Only level N-1 is visible to the sampler while level N is rendered, so
            // the texture is never read and written at the same level: through the view of level
            // N-1 where there are views, else by narrowing the texture's base and max level to N-1.
            sDownProgram.bind();
            channel = sDownProgram.enableTexture(LLShaderMgr::DEFERRED_DIFFUSE);
            if (channel > -1)
            {
                LLTexUnit* unit = gGL.getTexUnit(channel);
                if (!mViews.empty())
                {
                    for (U32 level = 1; level < mLevels; ++level)
                    {
                        unit->bindManual(LLTexUnit::TT_TEXTURE, mViews[level - 1], false);
                        drawLevel(level, screen_triangle);
                    }
                }
                else
                {
                    unit->bindManual(LLTexUnit::TT_TEXTURE, mTexture, true);
                    for (U32 level = 1; level < mLevels; ++level)
                    {
                        unit->activate();
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, level - 1);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, level - 1);
                        drawLevel(level, screen_triangle);
                    }
                    unit->activate();
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, mLevels - 1);
                }
                unit->unbind(LLTexUnit::TT_TEXTURE);
            }
            sDownProgram.unbind();

            // Back to whatever target the pipeline had bound.
            glBindFramebuffer(GL_FRAMEBUFFER, LLRenderTarget::sCurFBO);
            if (LLRenderTarget::sCurFBO)
            {
                glViewport(0, 0, LLRenderTarget::sCurResX, LLRenderTarget::sCurResY);
            }
            else
            {
                glViewport(gGLViewport[0], gGLViewport[1], gGLViewport[2], gGLViewport[3]);
            }
        }

        // Binds this pyramid to a shader with the FS_SSR_HIZ permutation; active only while it was
        // built from a scene map of scene_map's size
        void bind(LLGLSLShader& shader, const LLRenderTarget& scene_map, bool uniforms = true) const
        {
            S32 channel = shader.getTextureChannel(LLShaderMgr::FS_SSR_HIZ_MAP);
            if (channel < 0)
            {
                return;
            }
            const bool active = mTexture != 0 && !gCubeSnapshot && FSSSRHiZ::enabled() && scene_map.getWidth() == mWidth &&
                                scene_map.getHeight() == mHeight;
            if (active)
            {
                shader.enableTexture(LLShaderMgr::FS_SSR_HIZ_MAP);
                gGL.getTexUnit(channel)->bindManual(LLTexUnit::TT_TEXTURE, mTexture, true);
            }
            if (uniforms)
            {
                shader.uniform1f(sActive, active ? 1.f : 0.f);
            }
        }
    };

    Pyramid sMain;  // the previous frame's lit scene (mSceneMap)
    Pyramid sWater; // this frame's scene under transparent water

    bool programsReady()
    {
        return sCopyProgram.isComplete() && sDownProgram.isComplete();
    }
}

bool FSSSRHiZ::enabled()
{
    static LLCachedControl<bool> ssr(gSavedSettings, "RenderScreenSpaceReflections", false);
    static LLCachedControl<bool> hiz(gSavedSettings, "FSRenderSSRHiZ", true);
    return ssr && hiz;
}

bool FSSSRHiZ::usable()
{
    return enabled() && !sUnsupported;
}

void FSSSRHiZ::loadShaders(S32 shader_level)
{
    sCopyProgram.unload();
    sDownProgram.unload();
    if (!enabled())
    {
        release(); // switched off: give the memory back
        return;
    }

    struct Program
    {
        LLGLSLShader* mShader;
        const char* mName;
        bool mCopy;
    };
    const Program programs[] = {
        { &sCopyProgram, "FS SSR Hi-Z Copy", true },
        { &sDownProgram, "FS SSR Hi-Z Downsample", false },
    };
    for (const Program& program : programs)
    {
        LLGLSLShader* shader = program.mShader;
        shader->mName = program.mName;
        shader->mShaderFiles.clear();
        shader->clearPermutations();
        if (program.mCopy)
        {
            shader->addPermutation("FS_SSR_HIZ_COPY", "1");
        }
        shader->mShaderFiles.push_back(std::make_pair("deferred/postDeferredNoTCV.glsl", GL_VERTEX_SHADER));
        shader->mShaderFiles.push_back(std::make_pair("deferred/fsSSRHiZF.glsl", GL_FRAGMENT_SHADER));
        shader->mShaderLevel = shader_level;
        if (!shader->createShader())
        {
            LL_WARNS("ShaderLoading") << program.mName << " failed to build, SSR keeps the classic march" << LL_ENDL;
            sCopyProgram.unload();
            sDownProgram.unload();
            return;
        }
    }
}

void FSSSRHiZ::build(LLRenderTarget& scene_map, LLVertexBuffer* screen_triangle)
{
    if (!enabled() || !programsReady() || !screen_triangle || !scene_map.isComplete() || !scene_map.getDepth())
    {
        release();
        return;
    }
    LL_PROFILE_GPU_ZONE("ssr hi-z");
    sMain.build(scene_map, scene_map, screen_triangle);
}

bool FSSSRHiZ::buildWater(LLRenderTarget& color, LLRenderTarget& depth, LLVertexBuffer* screen_triangle)
{
    if (!enabled() || !programsReady() || !screen_triangle || gCubeSnapshot || !color.isComplete() || !depth.isComplete() ||
        !depth.getDepth() || color.getWidth() != depth.getWidth() || color.getHeight() != depth.getHeight())
    {
        sWater.release();
        return false;
    }
    LL_PROFILE_GPU_ZONE("ssr hi-z water");
    sWater.build(color, depth, screen_triangle);
    return sWater.mTexture != 0;
}

void FSSSRHiZ::bind(LLGLSLShader& shader, const LLRenderTarget& scene_map, bool uniforms)
{
    if (shader.getTextureChannel(LLShaderMgr::FS_SSR_HIZ_MAP) < 0)
    {
        return; // no FS_SSR_HIZ permutation
    }
    sMain.bind(shader, scene_map, uniforms);
    if (!uniforms)
    {
        return;
    }

    static LLCachedControl<S32> steps(gSavedSettings, "FSRenderSSRHiZSteps", 32);
    static LLCachedControl<F32> thickness(gSavedSettings, "FSRenderSSRThickness", 0.3f);
    static LLCachedControl<F32> edge_fade(gSavedSettings, "FSRenderSSREdgeFade", 0.1f);
    static LLCachedControl<bool> cone_trace(gSavedSettings, "FSRenderSSRConeTrace", false);
    shader.uniform1f(sSteps, (F32)llclamp((S32)steps, 4, 256));
    shader.uniform1f(sThickness, llclamp((F32)thickness, 0.01f, 10.f));
    shader.uniform1f(sEdgeFade, llclamp((F32)edge_fade, 0.f, 0.5f));
    // The cone trace reads the pyramid's coarser levels: never from a pyramid the driver can't build
    shader.uniform1f(sConeTrace, cone_trace && usable() ? 1.f : 0.f);
}

void FSSSRHiZ::bindWater(LLGLSLShader& shader, const LLRenderTarget& color)
{
    sWater.bind(shader, color);
}

void FSSSRHiZ::disable(LLGLSLShader& shader)
{
    shader.uniform1f(sActive, 0.f);
}

void FSSSRHiZ::release()
{
    sMain.release();
    sWater.release();
}
