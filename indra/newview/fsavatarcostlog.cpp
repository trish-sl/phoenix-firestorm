/**
 * @file fsavatarcostlog.cpp
 * @brief Logs what each nearby avatar puts into the frame (FSRenderGPUTimings)
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

#include "fsavatarcostlog.h"

#include "llagentcamera.h"
#include "lldrawable.h"
#include "lldrawpool.h"
#include "llface.h"
#include "llvoavatar.h"

#include <algorithm>
#include <vector>

namespace
{
    struct Row
    {
        LLVOAvatar* mAvatar = nullptr;
        F32 mDistance = 0.f;
        S32 mPrims = 0;
        S32 mFaces = 0;
        S32 mAlphaFaces = 0;
        U32 mTris = 0;
        U32 mAlphaTris = 0;
    };

    // The object, then its children: an avatar's children are its attachments
    void count(LLViewerObject* obj, Row& row, bool is_root)
    {
        if (!obj || obj->isDead() || (!is_root && obj->isHUDAttachment()))
        {
            return; // HUDs are drawn with the UI, not in the world passes
        }
        if (!is_root)
        {
            ++row.mPrims;
        }
        if (LLDrawable* drawable = obj->mDrawable)
        {
            for (S32 i = 0; i < drawable->getNumFaces(); ++i)
            {
                LLFace* face = drawable->getFace(i);
                if (!face || !face->hasGeometry())
                {
                    continue;
                }
                const U32 tris = face->getIndicesCount() / 3;
                ++row.mFaces;
                row.mTris += tris;
                if (face->getPoolType() == LLDrawPool::POOL_ALPHA)
                {
                    ++row.mAlphaFaces;
                    row.mAlphaTris += tris;
                }
            }
        }
        for (const auto& child : obj->getChildren())
        {
            count(child, row, false);
        }
    }

    const char* state_of(LLVOAvatar* avatar)
    {
        if (avatar->isVisuallyMuted())
        {
            return "muted or too complex";
        }
        if (avatar->isImpostor())
        {
            return "impostor";
        }
        if (!avatar->isVisible())
        {
            return "out of view";
        }
        return "drawn";
    }

    void log_costs()
    {
        const LLVector3d camera = gAgentCamera.getCameraPositionGlobal();
        std::vector<Row> rows;
        for (LLCharacter* character : LLCharacter::sInstances)
        {
            LLVOAvatar* avatar = static_cast<LLVOAvatar*>(character);
            if (!avatar || avatar->isDead() || avatar->isControlAvatar())
            {
                continue; // animesh is counted under the avatar wearing it
            }
            Row row;
            row.mAvatar = avatar;
            row.mDistance = (F32)dist_vec(avatar->getPositionGlobal(), camera);
            count(avatar, row, true);
            rows.push_back(row);
        }
        if (rows.empty())
        {
            return;
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b)
            {
                return a.mAlphaFaces != b.mAlphaFaces ? a.mAlphaFaces > b.mAlphaFaces : a.mTris > b.mTris;
            });

        S32 drawn = 0;
        S32 drawn_alpha_faces = 0;
        U32 drawn_tris = 0;
        for (const Row& row : rows)
        {
            if (!strcmp(state_of(row.mAvatar), "drawn"))
            {
                ++drawn;
                drawn_alpha_faces += row.mAlphaFaces;
                drawn_tris += row.mTris;
            }
        }

        std::string line;
        const size_t shown = llmin(rows.size(), (size_t)8);
        for (size_t i = 0; i < shown; ++i)
        {
            const Row& row = rows[i];
            LLVOAvatar* avatar = row.mAvatar;
            line += llformat("\n    %s%s: %s, %.0f m, complexity %u, %d attachments, %d prims, faces %d (alpha %d), triangles %uk (alpha %uk)",
                             avatar->getFullname().c_str(), avatar->isSelf() ? " (you)" : "", state_of(avatar), row.mDistance,
                             avatar->getVisualComplexity(), avatar->getAttachmentCount(), row.mPrims, row.mFaces, row.mAlphaFaces,
                             (row.mTris + 500) / 1000, (row.mAlphaTris + 500) / 1000);
        }
        LL_INFOS("FSAvatarCost") << "Avatars " << rows.size() << ", " << drawn << " drawn in full; those have " << drawn_alpha_faces
                                 << " alpha faces and " << (drawn_tris + 500) / 1000 << "k triangles; most alpha faces first:" << line << LL_ENDL;
    }
}

void FSAvatarCostLog::frame(bool enabled, float interval_seconds)
{
    static LLTimer timer;
    if (!enabled)
    {
        timer.reset();
        return;
    }
    if (timer.getElapsedTimeF32() >= llmax(interval_seconds, 1.f))
    {
        timer.reset();
        log_costs();
    }
}
