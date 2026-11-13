/**
 * @file fsexperimentalindicators.h
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

#ifndef FS_EXPERIMENTALINDICATORS_H
#define FS_EXPERIMENTALINDICATORS_H

#include <vector>

class LLPanel;
class FSSettingIndicator;

// A rough guide for people reading the Experimental tabs: beside each rated setting, a circle
// shows what the option costs in frame time (blue saves time, green little, yellow some, red a
// lot) and a square how much it changes the picture (green a lot, yellow some, grey little or
// nothing). The marks are small child views of the panel that holds each control, so they
// scroll and clip with it, and they follow the control's visibility and enabled state.
class FSExperimentalIndicators
{
public:
    // Adds a mark beside every rated control on the panel, and draws the legend into the
    // panel's experimental_indicator_legend placeholder. Level names and the tooltip come from
    // the panel's strings.
    void build(LLPanel* panel);

    // Keeps the marks beside their controls. Call from the panel's draw(), before its children
    // draw.
    void update();

private:
    std::vector<FSSettingIndicator*> mIndicators; // owned by the views they were added to
};

#endif // FS_EXPERIMENTALINDICATORS_H
