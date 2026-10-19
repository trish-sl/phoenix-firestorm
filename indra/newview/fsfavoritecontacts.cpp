/**
 * @file fsfavoritecontacts.cpp
 * @brief Favorite (pinned) friends storage management
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

#include "fsfavoritecontacts.h"

#include "llviewercontrol.h"

FSFavoriteContacts::FSFavoriteContacts()
{
}

FSFavoriteContacts::~FSFavoriteContacts()
{
    // Only write back what was actually loaded: saving an empty set before the per-account
    // settings were read would wipe the stored pins.
    if (mLoaded)
    {
        saveFavorites();
    }
}

bool FSFavoriteContacts::isFavorite(const LLUUID& avatar_id) const
{
    return mFavoriteContacts.find(avatar_id) != mFavoriteContacts.end();
}

void FSFavoriteContacts::addFavorite(const LLUUID& avatar_id)
{
    if (avatar_id.isNull())
    {
        return;
    }

    if (mFavoriteContacts.insert(avatar_id).second)
    {
        saveFavorites();
        mFavoritesChangedSignal();
    }
}

void FSFavoriteContacts::removeFavorite(const LLUUID& avatar_id)
{
    if (mFavoriteContacts.erase(avatar_id) > 0)
    {
        saveFavorites();
        mFavoritesChangedSignal();
    }
}

void FSFavoriteContacts::toggleFavorite(const LLUUID& avatar_id)
{
    if (isFavorite(avatar_id))
    {
        removeFavorite(avatar_id);
    }
    else
    {
        addFavorite(avatar_id);
    }
}

void FSFavoriteContacts::loadFavorites()
{
    mFavoriteContacts.clear();

    LLSD favorites = gSavedPerAccountSettings.getLLSD("FSFavoriteContacts");

    if (favorites.isArray())
    {
        for (LLSD::array_const_iterator it = favorites.beginArray(); it != favorites.endArray(); ++it)
        {
            if (it->isUUID())
            {
                mFavoriteContacts.insert(it->asUUID());
            }
        }
    }

    mLoaded = true;
    LL_INFOS("FavoriteContacts") << "Loaded " << mFavoriteContacts.size() << " pinned friends from per-account settings" << LL_ENDL;

    // The contacts floater can be built before login completes; let it re-style and re-sort.
    mFavoritesChangedSignal();
}

void FSFavoriteContacts::saveFavorites()
{
    // Unlike FSFavoriteGroups, friend pins are not removed here:
    // buddy list is not necessarily loaded when this runs, it would drop all the pins if we pruned here.
    LLSD favorites_array = LLSD::emptyArray();

    for (const auto& avatar_id : mFavoriteContacts)
    {
        favorites_array.append(avatar_id);
    }

    gSavedPerAccountSettings.setLLSD("FSFavoriteContacts", favorites_array);

    LL_DEBUGS("FavoriteContacts") << "Saved " << mFavoriteContacts.size() << " pinned friends to per-account settings" << LL_ENDL;
}
