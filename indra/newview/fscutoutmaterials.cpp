/**
 * @file fscutoutmaterials.cpp
 * @brief Paper cutout: remembers which uploaded material belongs to which cutout mesh
 *
 * $LicenseInfo:firstyear=2026:license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, Trish
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

#include "fscutoutmaterials.h"

void FSCutoutMaterialStore::rebuildIndex()
{
    mByMesh.clear();
    for (size_t i = 0; i < mEntries.size(); ++i)
    {
        if (mEntries[i].mesh_id.notNull())
        {
            mByMesh[mEntries[i].mesh_id] = i;
        }
    }
}

void FSCutoutMaterialStore::trim()
{
    if (mEntries.size() > MAX_ENTRIES)
    {
        mEntries.erase(mEntries.begin(), mEntries.end() - MAX_ENTRIES);
    }
}

void FSCutoutMaterialStore::addPending(const std::string& name)
{
    Entry entry;
    for (auto it = mEntries.begin(); it != mEntries.end(); ++it)
    {
        if (it->name == name)
        {
            entry = *it;    // same name again: keep its fields, move it to newest
            mEntries.erase(it);
            break;
        }
    }
    entry.name = name;
    mEntries.push_back(entry);
    trim();
    rebuildIndex();
}

bool FSCutoutMaterialStore::setMaterial(const std::string& name, const LLUUID& material_id)
{
    for (Entry& e : mEntries)
    {
        if (e.name == name)
        {
            e.material_id = material_id;
            return true;
        }
    }
    return false;
}

const FSCutoutMaterialStore::Entry* FSCutoutMaterialStore::learnMesh(const std::string& name, const LLUUID& mesh_id)
{
    if (mesh_id.isNull())
    {
        return nullptr;
    }
    for (Entry& e : mEntries)
    {
        if (e.name == name)
        {
            if (e.material_id.isNull() || e.mesh_id.notNull())
            {
                return nullptr;
            }
            e.mesh_id = mesh_id;
            rebuildIndex();
            return &e;
        }
    }
    return nullptr;
}

const FSCutoutMaterialStore::Entry* FSCutoutMaterialStore::findByMesh(const LLUUID& mesh_id) const
{
    if (mesh_id.isNull())
    {
        return nullptr;
    }
    auto it = mByMesh.find(mesh_id);
    return it == mByMesh.end() ? nullptr : &mEntries[it->second];
}

LLSD FSCutoutMaterialStore::asLLSD() const
{
    LLSD sd = LLSD::emptyArray();
    for (const Entry& e : mEntries)
    {
        LLSD row;
        row["name"] = e.name;
        row["material_id"] = e.material_id;
        row["mesh_id"] = e.mesh_id;
        sd.append(row);
    }
    return sd;
}

void FSCutoutMaterialStore::fromLLSD(const LLSD& sd)
{
    mEntries.clear();
    if (sd.isArray())
    {
        for (LLSD::array_const_iterator it = sd.beginArray(); it != sd.endArray(); ++it)
        {
            const LLSD& row = *it;
            if (!row.isMap() || row["name"].asString().empty())
            {
                continue;
            }
            // A present but non-UUID id means a damaged row: drop it rather than keep it half-set.
            const LLSD& mat = row["material_id"];
            const LLSD& mesh = row["mesh_id"];
            if ((mat.isDefined() && !mat.isUUID() && !LLUUID::validate(mat.asString())) ||
                (mesh.isDefined() && !mesh.isUUID() && !LLUUID::validate(mesh.asString())))
            {
                continue;
            }
            Entry e;
            e.name = row["name"].asString();
            e.material_id = mat.asUUID();
            e.mesh_id = mesh.asUUID();
            mEntries.push_back(e);
        }
    }
    trim();
    rebuildIndex();
}

std::vector<std::pair<S32, LLUUID>> cutoutApplyPlan(const LLUUID current[FSCutout::FACE_COUNT],
                                                    const LLUUID& material_id, const LLUUID& edge_material)
{
    std::vector<std::pair<S32, LLUUID>> plan;
    for (S32 face = 0; face < FSCutout::FACE_COUNT; ++face)
    {
        const LLUUID& id = (face == FSCutout::FACE_EDGE) ? edge_material : material_id;
        if (current[face].isNull() && id.notNull())
        {
            plan.emplace_back(face, id);
        }
    }
    return plan;
}
