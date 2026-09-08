/**
 * @file fsnativepopout.h
 * @brief Experimental native hosts for selected existing floaters.
 *
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#ifndef FS_NATIVE_POPOUT_H
#define FS_NATIVE_POPOUT_H

#include "stdtypes.h"

class LLVector2;

namespace FSNativePopout
{
    // Main thread only. Other platforms implement these as no-ops.
    void update(bool fullscreen, const LLVector2& display_scale);
    void draw();
    // Queue a toggle of the focused instance; never reparent during key dispatch.
    bool handleKey(KEY key, MASK mask, bool repeated);
    // Return floaters and release surfaces before a graphics-context reset.
    void reset();
    void shutdown();
}

#endif
