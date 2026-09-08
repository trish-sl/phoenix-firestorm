/**
 * @brief Explicit opt-in policy for experimental native floater hosts.
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 * SPDX-License-Identifier: LGPL-2.1-only
 */
#ifndef FS_NATIVE_POPOUT_POLICY_H
#define FS_NATIVE_POPOUT_POLICY_H

#include <string>
#include "llpreprocessor.h"
#include "indra_constants.h"
#include <array>
#include <string_view>

namespace FSNativePopoutPolicy
{
struct Spec
{
    std::string_view name;
    const char* setting; // Optional convenience toggle for a singleton.
};

inline constexpr std::array<Spec, 19> SPECS = {{
    { "fs_im_container", "FSExperimentalPopoutChat" },
    { "world_map", "FSExperimentalPopoutMap" },
    { "fs_radar", "FSExperimentalPopoutNearby" },
    { "area_search", "FSExperimentalPopoutAreaSearch" },
    { "notification_well_window", "FSExperimentalPopoutNotifications" },
    { "preferences", "FSExperimentalPopoutPreferences" },
    { "settings_debug", "FSExperimentalPopoutSettings" },
    { "inventory", "FSExperimentalPopoutInventory" },
    { "secondary_inventory", nullptr },
    { "preview_script", nullptr },
    { "preview_scriptedit", nullptr },
    { "preview_notecard", nullptr },
    { "gestures", nullptr },
    { "stats", nullptr },
    { "people", nullptr },
    { "imcontacts", nullptr },
    { "preview_sound", nullptr },
    { "inventory_settings", nullptr },
    { "region_tracker", nullptr }
}};

inline const Spec* find(std::string_view name)
{
    for (const auto& spec : SPECS)
    {
        if (spec.name == name) return &spec;
    }
    return nullptr;
}

inline bool isToggleShortcut(KEY key, MASK mask)
{
    // Ctrl+P belongs to Preferences. All existing P variants are occupied.
    return key == KEY_F12 && mask == (MASK_CONTROL | MASK_SHIFT);
}
}

#endif
