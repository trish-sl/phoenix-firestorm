/**
 * @file fsexperimentalindicators.cpp
 * @brief Cost and visual impact marks beside the settings of Preferences > Experimental
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

#include "fsexperimentalindicators.h"

#include "llcontrol.h"
#include "llfontgl.h"
#include "llpanel.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "lluicolortable.h"
#include "lluictrl.h"
#include "llview.h"

#include <list>
#include <unordered_map>

namespace
{
    // Frame time an option costs when on. For tuning values, what raising them costs. Saves:
    // the option is there to make frames faster.
    enum class Cost : U8 { Saves, Low, Moderate, High, Count };

    // How much the option improves the picture. None also covers options that trade a little
    // quality for speed, and everything that isn't about the picture at all.
    enum class Visual : U8 { Large, Moderate, None, Count };

    constexpr Cost CS = Cost::Saves;
    constexpr Cost CL = Cost::Low;
    constexpr Cost CM = Cost::Moderate;
    constexpr Cost CH = Cost::High;
    constexpr Visual VL = Visual::Large;
    constexpr Visual VM = Visual::Moderate;
    constexpr Visual VN = Visual::None;

    struct Rating
    {
        const char* mSetting;
        Cost mCost;
        Visual mVisual;
    };

    // Rough GPU costs measured on an RTX 5060 Ti at about 1920x1000 set the scale: high is a
    // millisecond or more (screen space reflections about 2.4 ms, sun shadow filtering about
    // 1 ms), moderate a few tenths (projector light cones 0.85 ms, GTAO 0.5 ms, skin scattering
    // 0.27 ms, dust motes 0.25 ms), low less than that (sun rays 0.15 ms, bloom, lens effects).
    // Settings with a control on the tabs but no entry here get no mark.
    const Rating RATINGS[] =
    {
        // General
        { "FollowCamFollowSubjectRotation", CL, VN },
        { "FSReportCameraAtAvatar", CL, VN },
        { "FSMouseLookPassonFix", CL, VN },
        { "FSRenderNeighborAdjacentOnly", CS, VN },
        { "FSExperimentalReleaseNonAdjacentRegions", CS, VN },
        { "FSFetchOfflineIMsWhileOnline", CL, VN },
        { "FSOfflineIMFetchIntervalSeconds", CL, VN },
        { "FSAvatarPositionRefreshIntervalSeconds", CL, VN },
        { "FSDebugDcThreshold", CL, VN },
        { "FSExperimentalPopoutChat", CL, VN },
        { "FSExperimentalPopoutInventory", CL, VN },
        { "FSExperimentalPopoutMap", CL, VN },
        { "FSExperimentalPopoutNearby", CL, VN },
        { "FSExperimentalPopoutAreaSearch", CL, VN },
        { "FSExperimentalPopoutPreferences", CL, VN },
        { "FSExperimentalPopoutSettings", CL, VN },
        { "FSExperimentalPopoutNotifications", CL, VN },
        { "FSHideUIFully", CL, VN },
        { "RLVaExperimentalLoginPrivacy", CL, VN },
        { "FSRLVaProfileCommands", CL, VN },
        { "ExternalWebsocketSyncEnable", CL, VN },
        { "ExternalWebsocketSyncLocal", CL, VN },
        { "ExternalWebsocketForwardDebug", CL, VN },
        { "ExternalWebsocketSyncPort", CL, VN },
        { "FSMassObjectEditInventoryWorkers", CL, VN },

        // Photo
        { "RenderResolutionMultiplier", CH, VL },
        { "FSSnapshotWaitForTextures", CL, VM },
        { "FSSnapshotTextureWaitTimeout", CL, VM },
        { "RenderScreenSpaceReflections", CH, VL },
        { "RenderScreenSpaceReflectionIterations", CM, VM },
        { "RenderScreenSpaceReflectionRayStep", CL, VM },
        { "RenderScreenSpaceReflectionGlossySamples", CH, VM },
        { "RenderScreenSpaceReflectionDistanceBias", CL, VM },
        { "RenderScreenSpaceReflectionDepthRejectBias", CL, VM },
        { "FSRenderSSRHiZ", CL, VM },
        { "FSRenderSSRConeTrace", CS, VM },
        { "FSRenderSSRHalfRes", CS, VN },
        { "FSRenderSSRHiZSteps", CM, VM },
        { "FSRenderSSRThickness", CL, VM },
        { "FSRenderSSREdgeFade", CL, VM },

        // Effects: sun rays and dust
        { "FSRenderSunRays", CL, VL },
        { "FSRenderSunRaysStrength", CL, VM },
        { "FSRenderSunRaysDensity", CL, VM },
        { "FSRenderSunRaysAnisotropy", CL, VM },
        { "FSRenderSunRaysDistance", CL, VM },
        { "FSRenderSunRaysSteps", CM, VM },
        { "FSRenderSunRaysResolution", CM, VM },
        { "FSRenderSunRaysHeightFalloff", CL, VM },
        { "FSRenderSunRaysSkyColor", CL, VM },
        { "FSRenderSunRaysTint", CL, VM },
        { "FSRenderSunRaysSkyHaze", CL, VM },
        { "FSRenderSunRaysMoon", CL, VM },
        { "FSRenderDustMotes", CM, VM },
        { "FSRenderDustMotesDensity", CM, VM },
        { "FSRenderDustMotesSize", CL, VM },
        { "FSRenderDustMotesBrightness", CL, VM },
        { "FSRenderDustMotesRange", CM, VM },
        { "FSRenderDustMotesTurbulence", CL, VM },

        // Effects: bloom, colour grading, lens
        { "FSRenderBloom", CL, VL },
        { "FSRenderBloomStrength", CL, VM },
        { "FSRenderBloomThreshold", CL, VM },
        { "FSRenderBloomKnee", CL, VM },
        { "FSRenderBloomLevels", CL, VM },
        { "FSColorGrade", CL, VL },
        { "FSColorGradeTemperature", CL, VM },
        { "FSColorGradeTint", CL, VM },
        { "FSColorGradeLift", CL, VM },
        { "FSColorGradeGamma", CL, VM },
        { "FSColorGradeGain", CL, VM },
        { "FSColorGradeContrast", CL, VM },
        { "FSColorGradeSaturation", CL, VM },
        { "FSColorGradeVibrance", CL, VM },
        { "FSColorGradeShadowTint", CL, VM },
        { "FSColorGradeHighlightTint", CL, VM },
        { "FSColorGradeSplitBalance", CL, VM },
        { "FSColorGradeLUT", CL, VL },
        { "FSColorGradeLUTStrength", CL, VM },
        { "FSRenderChromaticAberration", CL, VM },
        { "FSRenderChromaticAberrationDoFOnly", CL, VM },
        { "FSRenderLensFlare", CL, VM },
        { "FSRenderFilmGrain", CL, VM },
        { "FSRenderFilmGrainSize", CL, VM },

        // Effects: projector light cones
        { "FSRenderSpotVolumetric", CM, VL },
        { "FSRenderSpotVolumetricStrength", CL, VM },
        { "FSRenderSpotVolumetricDensity", CL, VM },
        { "FSRenderSpotVolumetricMaxLights", CM, VM },
        { "FSRenderSpotVolumetricResolution", CM, VM },
        { "FSRenderSpotVolumetricSteps", CM, VM },
        { "FSRenderSpotVolumetricUnshadowed", CM, VM },
        { "FSRenderSpotVolumetricTemporal", CS, VN },
        { "FSRenderSpotVolumetricNearRange", CL, VM },
        { "FSRenderSpotVolumetricAnisotropy", CL, VM },

        // Effects: water
        { "FSRenderWaterCaustics", CL, VM },
        { "FSRenderWaterCausticsStrength", CL, VM },
        { "FSRenderWaterCausticsScale", CL, VM },
        { "FSRenderWaterCausticsSpeed", CL, VM },
        { "FSRenderWaterFoam", CL, VM },
        { "FSRenderWaterFoamWidth", CL, VM },
        { "FSRenderWaterFoamStrength", CL, VM },
        { "FSRenderSunGlints", CL, VM },
        { "FSRenderSunGlintsIntensity", CL, VM },
        { "FSRenderSunGlintsDensity", CL, VM },
        { "FSRenderWetShore", CL, VM },
        { "FSRenderWetShoreStrength", CL, VM },
        { "FSRenderWetShoreHeight", CL, VM },
        { "FSRenderWetShoreSwell", CL, VM },
        { "FSRenderWaterFogVariation", CL, VM },
        { "FSRenderWaterFogPatchSize", CL, VM },

        // Effects: lens dirt, exposure and tone mapping, output
        { "FSRenderBloomLensDirt", CL, VM },
        { "FSRenderBloomLensDirtStrength", CL, VM },
        { "FSRenderLocalTonemap", CL, VL },
        { "FSRenderExposureSpeedToBright", CL, VM },
        { "FSRenderExposureSpeedToDark", CL, VM },
        { "FSRenderExposureCenterWeight", CL, VM },
        { "FSRenderExposureRangeLimit", CL, VM },
        { "FSRenderLocalTonemapStrength", CL, VM },
        { "FSRenderLocalTonemapDetail", CL, VM },
        { "FSRenderDeband", CL, VM },
        { "FSRenderDebandStrength", CL, VM },

        // Effects: height fog
        { "FSRenderHeightFog", CL, VL },
        { "FSRenderHeightFogSkyColor", CL, VM },
        { "FSRenderHeightFogBaseAbsolute", CL, VM },
        { "FSRenderHeightFogSkyHaze", CL, VM },
        { "FSRenderHeightFogDensity", CL, VM },
        { "FSRenderHeightFogFalloff", CL, VM },
        { "FSRenderHeightFogBaseHeight", CL, VM },
        { "FSRenderHeightFogMaxOpacity", CL, VM },
        { "FSRenderHeightFogAnisotropy", CL, VM },
        { "FSRenderHeightFogTint", CL, VM },
        { "FSRenderHeightFogInteriors", CL, VM },
        { "FSRenderHeightFogInteriorRange", CL, VM },
        { "FSRenderHeightFogInteriorStrength", CL, VM },

        // Effects: underwater light
        { "FSRenderUnderwaterRays", CM, VM },
        { "FSRenderUnderwaterRaysStrength", CL, VM },
        { "FSRenderUnderwaterRaysDensity", CL, VM },
        { "FSRenderUnderwaterRaysClarity", CL, VM },
        { "FSRenderUnderwaterRaysShafts", CL, VM },
        { "FSRenderUnderwaterRaysSteps", CM, VM },
        { "FSRenderUnderwaterRaysDistance", CL, VM },

        // Effects: sky and small touches
        { "FSRenderLensFlareMoon", CL, VM },
        { "FSRenderMoonCorona", CL, VM },
        { "FSRenderMoonGlade", CL, VM },
        { "FSRenderNightVision", CL, VM },
        { "FSRenderCloudSilverLining", CL, VM },
        { "FSRenderSunDiscDetail", CL, VM },
        { "FSRenderStarExtinction", CL, VM },
        { "FSRenderTwilightBands", CL, VM },
        { "FSRenderSnellsWindow", CL, VM },
        { "FSRenderHalation", CL, VM },
        { "FSRenderAnamorphicStreaks", CL, VM },
        { "FSRenderFoliage", CL, VM },
        { "FSRenderFoliageStrength", CL, VM },

        // Rendering
        { "FSRenderSpecularAA", CL, VM },
        { "FSRenderSpecularAAStrength", CL, VM },
        { "FSRenderSpecularAAThreshold", CL, VM },
        { "FSRenderGTAO", CM, VL },
        { "FSRenderGTAORadius", CL, VM },
        { "FSRenderGTAOIntensity", CL, VM },
        { "FSRenderGTAOSlices", CM, VM },
        { "FSRenderGTAOHalfRes", CS, VN },
        { "FSRenderDynamicResolution", CS, VN },
        { "FSRenderDynamicResolutionTargetFPS", CS, VN },
        { "FSRenderDynamicResolutionMinScale", CS, VN },
        { "FSRenderDynamicResolutionSharpness", CL, VM },
        { "FSRenderDynamicResolutionInterval", CL, VN },
        { "FSRenderUpscaler", CL, VM },
        { "FSRenderVRS", CS, VN },
        { "FSRenderVRSQuality", CL, VM },
        { "FSRenderVRSPeriphery", CS, VN },
        { "FSPerfProbeUpdatePriority", CS, VN },
        { "FSPerfProbeSkipUnchanged", CS, VN },
        { "FSPerfProbeMaxStaleSeconds", CS, VN },
        { "FSPerfProbeSunAngleThreshold", CS, VN },
        { "FSPerfProbeRealtimeFacesPerFrame", CM, VM },
        { "FSRenderProbeFilterQuality", CM, VM },
        { "FSRenderProbeBlendSmooth", CL, VM },
        { "FSRenderSpecularOcclusion", CL, VM },
        { "FSRenderMicroShadows", CL, VM },
        { "FSRenderSpecularOcclusionStrength", CL, VM },
        { "FSRenderMicroShadowsStrength", CL, VM },
        { "RenderAlphaBlendShadows", CM, VM },
        { "RLVaDebugSphereUseOpaqueDepth", CL, VN },
        { "RenderAMDDisableShaderCache", CL, VN },
        { "RenderAMDDisableTextureThreading", CM, VN },
        { "RenderRiggedDirectNormals", CL, VN },
        { "RenderRiggedLocalOrigin", CL, VN },
        { "RenderRiggedPreciseMath", CL, VN },

        // Shadows
        { "RenderShadowCullMode", CS, VN },
        { "RenderShadowCullBridgesPerCascade", CS, VN },
        { "RenderShadowMinCasterTexels", CS, VN },
        { "FSRenderShadowMaxDistance", CM, VM },
        { "RenderShadowSplits", CM, VM },
        { "FSRenderShadowFarCascadeInterval", CS, VN },
        { "FSRenderShadowFarCascadeResolution", CS, VN },
        { "FSRenderShadowFarCascadeMinCasterScale", CS, VN },
        { "FSRenderShadowAlphaCascades", CM, VM },
        { "FSRenderShadowReduceWhileMoving", CS, VN },
        { "FSImpostorUpdateBudget", CM, VN },
        { "FSImpostorMinInterval", CS, VN },
        { "FSRenderAvatarShadowDistance", CS, VN },
        { "FSRenderAvatarShadowCascades", CS, VN },
        { "FSRenderSpotShadowUpdateInterval", CS, VN },
        { "FSRenderShadowFastFilter", CS, VN },
        { "FSRenderShadowTexelSnap", CL, VM },
        { "FSRenderAlphaWBOIT", CM, VM },
        { "RenderShadowPCSS", CH, VL },
        { "RenderShadowPCSSLightSize", CL, VM },
        { "FSPerfShadowMergedBatches", CS, VN },
        { "FSPerfShadowStaticCache", CS, VN },
        { "FSPerfShadowStaticCacheSunAngle", CS, VN },
        { "FSPerfShadowSinglePass", CS, VN },
        { "FSPerfSpotShadowCache", CS, VN },
        { "FSPerfSpotShadowSplit", CS, VN },
        { "FSRenderCloudShadows", CL, VM },
        { "FSRenderCloudShadowStrength", CL, VM },
        { "FSRenderCloudShadowSoftness", CL, VM },
        { "FSRenderContactShadows", CM, VM },
        { "FSRenderContactShadowLength", CL, VM },
        { "FSRenderContactShadowStrength", CL, VM },
        { "FSRenderShadowSmoothFade", CL, VM },
        { "FSRenderShadowFadeLength", CL, VM },
        { "FSRenderColoredShadows", CM, VM },
        { "FSRenderColoredShadowAbsorption", CL, VM },
        { "FSRenderColoredShadowsProjectors", CM, VM },
        { "FSRenderSpotShadowCount", CH, VM },

        // Textures
        { "FSDecodedTextureCache", CS, VN },
        { "FSDecodedTextureCacheMode", CL, VN },
        { "FSDecodedTextureCacheRegions", CL, VN },
        { "FSDecodedTextureCacheSizeMB", CL, VN },
        { "FSDecodedTextureCacheMinSize", CL, VN },
        { "FSDecodedTextureCacheUncompressed", CS, VN },
        { "FSPrioritizeTextureDecodes", CS, VN },
        { "FSLowPriorityTextureThreads", CS, VN },
        { "FSTextureUseLiveVRAM", CL, VN },
        { "FSTextureBiasSoftZone", CL, VN },
        { "FSPerfTextureUploadPBO", CS, VN },
        { "FSPerfTextureUploadBudgetMB", CS, VN },
        { "RenderTextureMemoryUsageBias", CM, VM },
        { "TextureFetchPipelineDiagnostics", CL, VN },
        { "FSTextureFetchSelfHeal", CL, VN },

        // Avatar
        { "FSExperimentalRepairAnimations", CL, VN },
        { "FSExperimentalSyncAnimationClock", CL, VN },
        { "FSAvatarCloudFastRetry", CL, VN },
        { "FSAvatarRetryEmptyAppearance", CL, VN },
        { "FSAvatarDropStaleAppearance", CL, VN },
        { "FSFastSelfRez", CL, VN },
        { "FSPerfAvatarRankHysteresis", CS, VN },
        { "FSPerfRiggedPaletteCache", CS, VN },
        { "FSPerfAttachmentExtentsCache", CS, VN },
        { "FSPerfParallelSkinPalettes", CS, VN },
        { "FSRenderSkinSSS", CM, VL },
        { "FSRenderSkinSSSStrength", CL, VM },
        { "FSRenderSkinSSSRadius", CL, VM },
        { "FSRenderSkinSSSTint", CL, VM },
        { "FSRenderSkinSSSToneMatch", CL, VM },
        { "FSRenderSkinSSSTranslucency", CL, VM },
        { "FSRenderHairTwoPass", CM, VL },
        { "FSRenderHairAlphaThreshold", CL, VM },

        // Performance
        { "FSPerfAlphaResortOnly", CS, VN },
        { "FSPerfRebuildTimeBudget", CS, VN },
        { "FSPerfShadowPointCloudFromCull", CS, VN },
        { "FSPerfRepresentativeFragmentTest", CS, VN },
        { "FSPerfAlphaGlowSkip", CS, VN },
        { "FSPerfLightRescanThrottle", CS, VN },
        { "FSPerfHeroProbeSkipHiddenFaces", CS, VN },
        { "FSPerfProbeUniformCache", CS, VN },
        { "FSPerfBatchedLocalLights", CS, VN },
        { "FSPerfParallelShadowCull", CS, VN },
        { "FSPerfParallelShadowCullVerify", CH, VN },
        { "FSPerfParallelGeometry", CS, VN },
        { "FSPerfParallelGeometryVerify", CH, VN },
        { "FSPerfParallelGeometryMinVerts", CL, VN },
        { "FSPerfParallelAvatarUpdate", CS, VN },
        { "FSPerfParallelAvatarUpdateVerify", CH, VN },
        { "FSPerfParallelMainCull", CS, VN },
        { "FSPerfParallelMainCullVerify", CH, VN },
        { "FSPerfParallelPostSort", CS, VN },
        { "FSPerfParallelPostSortVerify", CH, VN },
        { "FSPerfParallelPostSortMinGroups", CL, VN },
        { "FSPerfParallelTexturePriority", CS, VN },
        { "FSPerfParallelTexturePriorityVerify", CH, VN },
        { "FSPerfDiskThread", CS, VN },
        { "FSPerfBackgroundVolumeFree", CS, VN },
        { "FSPerfAudioThread", CS, VN },
        { "FSPerfOpenJPEGBufferCacheMB", CS, VN },
        { "FSPerfImageBufferPoolMB", CS, VN },
        { "FSPerfGPUDrivenDraws", CS, VN },
        { "FSPerfGPUDrivenDrawsVerify", CH, VN },
        { "FSPerfGPUDrivenDrawsArenaMB", CL, VN },
        { "FSPerfGPUDrivenPBR", CS, VN },
        { "FSPerfGPUDrivenPBRBindless", CS, VN },
        { "FSPerfGPUDrivenPBRArenaMB", CL, VN },
        { "FSPerfGPUDrivenLegacy", CS, VN },
        { "FSPerfGPUDrivenLegacyArenaMB", CL, VN },
        { "FSPerfGPUDrivenPBRGroupRuns", CS, VN },
        { "FSPerfGPUDrivenMaterials", CS, VN },
        { "FSPerfGPUDrivenMaterialsArenaMB", CL, VN },
        { "FSPerfGPUDrivenShadowAlpha", CS, VN },
        { "FSPerfGPUDrivenShadowAlphaArenaMB", CL, VN },
        { "FSRenderThread", CS, VN },
        { "FSPerfParallelShaderCompile", CS, VN },
        { "FSRenderUICap", CS, VN },
        { "FSRenderUIIdleRate", CL, VN },
        { "FSPerfParallelTextureEncode", CS, VN },
        { "FSPerfInventoryLazySort", CS, VN },
        { "FSPerfAudioThrottleChecks", CS, VN },
        { "FSPerfInventoryIdleThrottle", CS, VN },
        { "FSPerfHoverPickThrottle", CS, VN },

        // Logging
        { "FSHitchLogThresholdMs", CL, VN },
        { "FSHitchStackMs", CL, VN },
        { "FSPerfProfileLogInterval", CL, VN },
        { "FSRenderGPUTimings", CM, VN },
        { "FSRenderGPUTimingsInterval", CL, VN },
        { "FSRenderThreadCheck", CM, VN },
    };

    const LLColor4& costColor(Cost cost)
    {
        static const LLColor4 colors[(S32)Cost::Count] =
        {
            LLColor4(0.25f, 0.65f, 1.f, 1.f),   // saves time: blue
            LLColor4(0.3f, 0.8f, 0.3f, 1.f),    // low: green
            LLColor4(0.95f, 0.8f, 0.2f, 1.f),   // moderate: yellow
            LLColor4(0.9f, 0.25f, 0.2f, 1.f)    // high: red
        };
        return colors[(S32)cost];
    }

    const LLColor4& visualColor(Visual visual)
    {
        static const LLColor4 colors[(S32)Visual::Count] =
        {
            LLColor4(0.3f, 0.8f, 0.3f, 1.f),    // large: green
            LLColor4(0.95f, 0.8f, 0.2f, 1.f),   // moderate: yellow
            LLColor4(0.55f, 0.55f, 0.55f, 1.f)  // little or none: grey
        };
        return colors[(S32)visual];
    }

    constexpr F32 CIRCLE_RADIUS = 4.f;
    constexpr S32 SQUARE_SIZE = 8;
    constexpr S32 MARK_GAP = 3;    // between the circle and the square
    constexpr S32 MARK_HEIGHT = 10;
    constexpr S32 MARK_WIDTH = (S32)(2.f * CIRCLE_RADIUS) + MARK_GAP + SQUARE_SIZE;
    constexpr S32 RIGHT_MARGIN = 1; // controls on the tabs end 20 pixels before the panel edge

    // A darker outline keeps the marks readable on light and dark skins alike
    void drawCircle(F32 center_x, F32 center_y, const LLColor4& color, F32 alpha)
    {
        gGL.color4f(color.mV[VRED], color.mV[VGREEN], color.mV[VBLUE], alpha);
        gl_circle_2d(center_x, center_y, CIRCLE_RADIUS, 16, true);
        gGL.color4f(color.mV[VRED] * 0.5f, color.mV[VGREEN] * 0.5f, color.mV[VBLUE] * 0.5f, alpha);
        gl_circle_2d(center_x, center_y, CIRCLE_RADIUS, 16, false);
    }

    void drawSquare(S32 left, S32 center_y, const LLColor4& color, F32 alpha)
    {
        const S32 top = center_y + SQUARE_SIZE / 2;
        const S32 right = left + SQUARE_SIZE;
        const S32 bottom = top - SQUARE_SIZE;
        gl_rect_2d(left, top, right, bottom, LLColor4(color.mV[VRED], color.mV[VGREEN], color.mV[VBLUE], alpha), true);
        gl_rect_2d(left, top, right, bottom,
                   LLColor4(color.mV[VRED] * 0.5f, color.mV[VGREEN] * 0.5f, color.mV[VBLUE] * 0.5f, alpha), false);
    }

    const char* const COST_STRINGS[(S32)Cost::Count] =
    {
        "indicator_cost_saves", "indicator_cost_low", "indicator_cost_moderate", "indicator_cost_high"
    };
    const char* const VISUAL_STRINGS[(S32)Visual::Count] =
    {
        "indicator_visual_large", "indicator_visual_moderate", "indicator_visual_none"
    };

    std::string panelString(LLPanel* panel, const char* name)
    {
        return panel->hasString(name) ? panel->getString(name) : std::string();
    }

    // Draws "Cost: o saves o low ... Visual: [] large ..." on one line
    class IndicatorLegend : public LLView
    {
    public:
        IndicatorLegend(const LLView::Params& p, LLPanel* panel)
        :   LLView(p)
        {
            mCostLabel = panelString(panel, "indicator_legend_cost");
            mVisualLabel = panelString(panel, "indicator_legend_visual");
            for (S32 i = 0; i < (S32)Cost::Count; ++i)
            {
                mCostNames[i] = panelString(panel, COST_STRINGS[i]);
            }
            for (S32 i = 0; i < (S32)Visual::Count; ++i)
            {
                mVisualNames[i] = panelString(panel, VISUAL_STRINGS[i]);
            }
        }

        void draw() override
        {
            const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
            const LLColor4 text_color = LLUIColorTable::instance().getColor("LabelTextColor");
            const F32 alpha = getDrawContext().mAlpha;
            const S32 center_y = getRect().getHeight() / 2;
            constexpr S32 SHAPE_TEXT_GAP = 3;
            constexpr S32 ITEM_GAP = 9;
            constexpr S32 GROUP_GAP = 20;
            S32 x = 0;

            auto text = [&](const std::string& label)
            {
                font->renderUTF8(label, 0, x, center_y, text_color % alpha, LLFontGL::LEFT, LLFontGL::VCENTER);
                x += font->getWidth(label);
            };

            text(mCostLabel);
            for (S32 i = 0; i < (S32)Cost::Count; ++i)
            {
                x += ITEM_GAP;
                drawCircle((F32)x + CIRCLE_RADIUS, (F32)center_y, costColor((Cost)i), alpha);
                x += (S32)(2.f * CIRCLE_RADIUS) + SHAPE_TEXT_GAP;
                text(mCostNames[i]);
            }

            x += GROUP_GAP;
            text(mVisualLabel);
            for (S32 i = 0; i < (S32)Visual::Count; ++i)
            {
                x += ITEM_GAP;
                drawSquare(x, center_y, visualColor((Visual)i), alpha);
                x += SQUARE_SIZE + SHAPE_TEXT_GAP;
                text(mVisualNames[i]);
            }
        }

    private:
        std::string mCostLabel;
        std::string mVisualLabel;
        std::string mCostNames[(S32)Cost::Count];
        std::string mVisualNames[(S32)Visual::Count];
    };
}

// The circle and square beside one control
class FSSettingIndicator : public LLView
{
public:
    FSSettingIndicator(const LLView::Params& p, LLUICtrl* ctrl, Cost cost, Visual visual)
    :   LLView(p),
        mCtrl(ctrl->getHandle()),
        mCost(cost),
        mVisual(visual)
    {
    }

    LLUICtrl* getCtrl() const { return mCtrl.get(); }

    void draw() override
    {
        // Dimmed with the control while it is disabled
        const LLUICtrl* ctrl = mCtrl.get();
        const F32 alpha = getDrawContext().mAlpha * ((ctrl && ctrl->getEnabled()) ? 1.f : 0.4f);
        const S32 center_y = getRect().getHeight() / 2;
        drawCircle(CIRCLE_RADIUS, (F32)center_y, costColor(mCost), alpha);
        drawSquare((S32)(2.f * CIRCLE_RADIUS) + MARK_GAP, center_y, visualColor(mVisual), alpha);
    }

private:
    LLHandle<LLUICtrl> mCtrl;
    Cost mCost;
    Visual mVisual;
};

void FSExperimentalIndicators::build(LLPanel* panel)
{
    std::unordered_map<std::string, const Rating*> ratings;
    for (const Rating& rating : RATINGS)
    {
        ratings.emplace(rating.mSetting, &rating);
    }

    // Collect first: adding the marks while walking would change the child lists being walked
    std::vector<std::pair<LLUICtrl*, const Rating*>> rated;
    std::list<LLView*> view_stack{ panel };
    while (!view_stack.empty())
    {
        LLView* view = view_stack.front();
        view_stack.pop_front();
        if (LLUICtrl* ctrl = dynamic_cast<LLUICtrl*>(view))
        {
            if (LLControlVariable* control = ctrl->getControlVariable())
            {
                auto it = ratings.find(control->getName());
                if (it != ratings.end() && ctrl->getParent())
                {
                    rated.emplace_back(ctrl, it->second);
                }
            }
        }
        for (LLView* child : *view->getChildList())
        {
            view_stack.push_back(child);
        }
    }

    for (const auto& [ctrl, rating] : rated)
    {
        LLStringUtil::format_map_t args;
        args["[COST]"] = panelString(panel, COST_STRINGS[(S32)rating->mCost]);
        args["[VISUAL]"] = panelString(panel, VISUAL_STRINGS[(S32)rating->mVisual]);

        LLView::Params p;
        p.name("setting_indicator");
        p.mouse_opaque(true);
        p.follows.flags(FOLLOWS_LEFT | FOLLOWS_TOP);
        p.rect(LLRect(0, MARK_HEIGHT, MARK_WIDTH, 0));
        if (panel->hasString("indicator_tooltip"))
        {
            p.tool_tip(panel->getString("indicator_tooltip", args));
        }
        FSSettingIndicator* indicator = new FSSettingIndicator(p, ctrl, rating->mCost, rating->mVisual);
        ctrl->getParent()->addChild(indicator);
        mIndicators.push_back(indicator);
    }

    if (LLView* holder = panel->findChild<LLView>("experimental_indicator_legend"))
    {
        LLView::Params p;
        p.name("indicator_legend");
        p.mouse_opaque(false); // the holder shows the explanation as its tooltip
        p.follows.flags(FOLLOWS_ALL);
        p.rect(holder->getLocalRect());
        holder->addChild(new IndicatorLegend(p, panel));
    }

    update();
}

void FSExperimentalIndicators::update()
{
    for (FSSettingIndicator* indicator : mIndicators)
    {
        LLUICtrl* ctrl = indicator->getCtrl();
        LLView* parent = indicator->getParent();
        const bool show = ctrl && parent && ctrl->getVisible();
        if (show)
        {
            // In a fixed column at the right edge of the panel holding the control, level with
            // the middle of the control
            LLRect ctrl_rect;
            ctrl->localRectToOtherView(ctrl->getLocalRect(), &ctrl_rect, parent);
            LLRect rect;
            rect.setLeftTopAndSize(parent->getRect().getWidth() - MARK_WIDTH - RIGHT_MARGIN,
                                   ctrl_rect.getCenterY() + MARK_HEIGHT / 2, MARK_WIDTH, MARK_HEIGHT);
            if (rect != indicator->getRect())
            {
                indicator->setRect(rect);
            }
        }
        if (indicator->getVisible() != show)
        {
            indicator->setVisible(show);
        }
    }
}
