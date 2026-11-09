/**
 * @file lldrawpoolalpha.h
 * @brief LLDrawPoolAlpha class definition
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLDRAWPOOLALPHA_H
#define LL_LLDRAWPOOLALPHA_H

#include "lldrawpool.h"
#include "llrender.h"
#include "llframetimer.h"

class LLFace;
class LLColor4;
class LLGLSLShader;

class LLDrawPoolAlpha final: public LLRenderPass
{
public:

    // set by llsettingsvo so lldrawpoolalpha has quick access to the water plane in eye space
    static LLVector4 sWaterPlane;

    enum
    {
        VERTEX_DATA_MASK =  LLVertexBuffer::MAP_VERTEX |
                            LLVertexBuffer::MAP_NORMAL |
                            LLVertexBuffer::MAP_COLOR |
                            LLVertexBuffer::MAP_TEXCOORD0
    };
    virtual U32 getVertexDataMask() { return VERTEX_DATA_MASK; }

    LLDrawPoolAlpha(U32 type);
    /*virtual*/ ~LLDrawPoolAlpha();

    /*virtual*/ S32 getNumPostDeferredPasses();
    /*virtual*/ void renderPostDeferred(S32 pass);
    /*virtual*/ S32  getNumPasses() { return 1; }

    // <FS:Trish> Weighted blended order-independent transparency (FSRenderAlphaWBOIT)
    enum EWBOITPhase
    {
        WBOIT_OFF = 0,      // stock sorted alpha, every draw
        WBOIT_ACCUMULATE,   // source-over draws only, into the WBOIT targets, no glow, no depth writes
        WBOIT_RESIDUAL,     // custom blend draws only, sorted over the composite, plus every draw's glow
        WBOIT_DEPTH,        // depth only for the accumulated draws, where stock alpha writes depth
    };
    // </FS:Trish>

    // <FS:Trish> WBOIT phase
    //void forwardRender(bool write_depth = false);
    void forwardRender(bool write_depth = false, EWBOITPhase wboit_phase = WBOIT_OFF);
    // </FS:Trish>
    /*virtual*/ void prerender();

    void renderDebugAlpha();

    void renderGroupAlpha(LLSpatialGroup* group, U32 type, U32 mask, bool texture = true);
    // <FS:Trish> WBOIT phase
    //void renderAlpha(U32 mask, bool depth_only = false, bool rigged = false);
    void renderAlpha(U32 mask, bool depth_only = false, bool rigged = false, EWBOITPhase wboit_phase = WBOIT_OFF);
    // </FS:Trish>
    void renderAlphaHighlight();

    static bool sShowDebugAlpha;
    static bool sShowDebugAlphaRigged;

private:
    LLGLSLShader* target_shader;

    // setup by beginFooPass, [0] is static variant, [1] is rigged variant
    LLGLSLShader* simple_shader = nullptr;
    LLGLSLShader* fullbright_shader = nullptr;
    LLGLSLShader* emissive_shader = nullptr;
    LLGLSLShader* pbr_emissive_shader = nullptr;
    LLGLSLShader* pbr_shader = nullptr;

    void drawEmissive(LLDrawInfo* draw);
    void renderEmissives(std::vector<LLDrawInfo*>& emissives);
    void renderRiggedEmissives(std::vector<LLDrawInfo*>& emissives);
    void renderPbrEmissives(std::vector<LLDrawInfo*>& emissives);
    void renderRiggedPbrEmissives(std::vector<LLDrawInfo*>& emissives);
    bool TexSetup(LLDrawInfo* draw, bool use_material);
    void RestoreTexSetup(bool tex_setup);

    // <FS:Trish> Switch the non-rigged alpha shaders between stock and WBOIT output
    void setWBOITOutput(bool enable);
    // </FS:Trish>

    // <FS:Trish> Two-pass rigged alpha (FSRenderHairTwoPass). The values are the shaders' fs_hair_pass.
    enum EHairPass
    {
        HAIR_OFF = 0,   // stock: one sorted, blended, depth writing pass
        HAIR_CORE,      // coverage at or above the threshold only, blended, writes depth
        HAIR_FRINGE,    // coverage below the threshold only, blended over the core, no depth writes
        HAIR_DEPTH,     // depth only, every fragment the stock pass would have written
    };
    bool renderRiggedAlphaTwoPass();
    void setHairUniforms(F32 threshold, bool reset);
    EHairPass mHairPass = HAIR_OFF;
    // </FS:Trish>

    // <FS:Trish> Whether the current forwardRender pass writes depth (FSPerfAlphaGlowSkip)
    bool mFSWriteDepth = false;
    // </FS:Trish>

    // our 'normal' alpha blend function for this pass
    LLRender::eBlendFactor mColorSFactor;
    LLRender::eBlendFactor mColorDFactor;
    LLRender::eBlendFactor mAlphaSFactor;
    LLRender::eBlendFactor mAlphaDFactor;

    // if true, we're executing a rigged render pass
    bool mRigged = false;
};

#endif // LL_LLDRAWPOOLALPHA_H
