/**
 * @file fsprobeblend.h
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

#ifndef FS_PROBEBLEND_H
#define FS_PROBEBLEND_H

#include "stdtypes.h"

class LLReflectionMap;
class LLReflectionMapManager;

// A reflection probe refresh used to replace the probe's maps in one go: its irradiance (nine SH
// coefficients) when the six irradiance faces were done, and its radiance (the filtered cube) six
// faces later, so a glossy floor changed twice per refresh, and a sun move that refreshes every
// probe in turn sent a wave of such jumps across the scene.
//
// With FSRenderProbeRefreshBlend the new maps are written as before, and copied aside together with
// the maps they replace; for the next FSRenderProbeRefreshBlendTime seconds the probe's slot is
// written again every frame as a mix of the two, from all old to all new. Radiance: two spare cubes
// at the end of the probe cube array (allocated only while the setting is on; that costs a reset of
// the probes when it is switched); irradiance: a 9 x 3 target of its own. Only probes that already
// had complete maps blend (a new probe fades in as before), never the realtime probe (it changes every
// frame anyway). One probe blends at a time: a refresh that completes while another blends ends that
// blend at once. A blend is dropped, leaving the new maps, when the probe loses its slot, the cube
// array is reallocated, or the maps are reset.
//
// Needs glCopyImageSubData (OpenGL 4.3); without it probes refresh as before.
class FSProbeBlend
{
public:
    // spare cubes the probe cube array needs at its end (after the two scratch cubes): 2 while the
    // setting is on and copies are possible, else 0
    static U32 extraLayers();

    // LLReflectionMapManager::updateProbeFace, the last face of a radiance pass: before the
    // radiance maps are generated into the probe's slot (keeps the old ones) and after (keeps the
    // new ones and puts the old ones back, the blend then brings the new ones in)
    static bool beginRadiance(LLReflectionMapManager& mgr, LLReflectionMap* probe);
    static void endRadiance(LLReflectionMapManager& mgr, LLReflectionMap* probe);

    // The same for the irradiance coefficients of the last face of an irradiance pass
    static bool beginIrradiance(LLReflectionMapManager& mgr, LLReflectionMap* probe);
    static void endIrradiance(LLReflectionMapManager& mgr, LLReflectionMap* probe);
    // Temporarily expose the new irradiance while capturing the probe's radiance.
    static bool beginRadianceCapture(LLReflectionMapManager& mgr, LLReflectionMap* probe);
    static void endRadianceCapture(LLReflectionMapManager& mgr, LLReflectionMap* probe);

    // LLReflectionMapManager::update, once a frame: writes the running blends
    static void step(LLReflectionMapManager& mgr);

    // drops every running blend (the maps were reset or reallocated)
    static void cancel();

    // shader reload: the programs are built again on first use
    static void loadShaders();

    // frees the programs and the irradiance target
    static void release();

private:
    // the spare cubes of the probe cube array: the maps before a refresh, and after
    static U32 oldLayer(const LLReflectionMapManager& mgr);
    static U32 newLayer(const LLReflectionMapManager& mgr);
};

#endif // FS_PROBEBLEND_H
