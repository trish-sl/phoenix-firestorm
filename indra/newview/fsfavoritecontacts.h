/**
 * @file fsfavoritecontacts.h
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

#ifndef FS_FAVORITECONTACTS_H
#define FS_FAVORITECONTACTS_H

#include "llsingleton.h"
#include "lluuid.h"
#include <boost/signals2.hpp>
#include <set>

// The friends-list counterpart of FSFavoriteGroups: friends pinned to the top of the
// Contacts floater's friends list, stored per account.
class FSFavoriteContacts : public LLSingleton<FSFavoriteContacts>
{
    LLSINGLETON(FSFavoriteContacts);
    ~FSFavoriteContacts();

public:
    bool isFavorite(const LLUUID& avatar_id) const;
    void addFavorite(const LLUUID& avatar_id);
    void removeFavorite(const LLUUID& avatar_id);
    void toggleFavorite(const LLUUID& avatar_id);
    const std::set<LLUUID>& getFavorites() const { return mFavoriteContacts; }
    bool hasFavorites() const { return !mFavoriteContacts.empty(); }
    void loadFavorites();
    void saveFavorites();

    typedef boost::signals2::signal<void()> favorites_changed_signal_t;
    boost::signals2::connection setFavoritesChangedCallback(const favorites_changed_signal_t::slot_type& cb)
    {
        return mFavoritesChangedSignal.connect(cb);
    }

private:
    std::set<LLUUID> mFavoriteContacts;
    favorites_changed_signal_t mFavoritesChangedSignal;
    bool mLoaded = false;
};

#endif // FS_FAVORITECONTACTS_H
