/**
 * @file fscutoutmaterials.h
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

#ifndef FS_CUTOUTMATERIALS_H
#define FS_CUTOUTMATERIALS_H

#include "fscutoutmesh.h"
#include "llsd.h"
#include "llsingleton.h"
#include "lluuid.h"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Pure store (llcommon only) so it can be unit tested. The caller saves asLLSD() to disk.
class FSCutoutMaterialStore
{
public:
    struct Entry
    {
        std::string name;
        LLUUID material_id;
        LLUUID mesh_id;
    };
    static const size_t MAX_ENTRIES = 200;

    void addPending(const std::string& name);                               // newest last; trims oldest past MAX
    bool setMaterial(const std::string& name, const LLUUID& material_id);   // false if no entry
    // Entry with this name, a material and no mesh yet: records mesh_id and returns it; else nullptr.
    const Entry* learnMesh(const std::string& name, const LLUUID& mesh_id);
    const Entry* findByMesh(const LLUUID& mesh_id) const;
    LLSD asLLSD() const;
    void fromLLSD(const LLSD& sd);                                          // replaces contents, skips malformed rows

private:
    void rebuildIndex();
    void trim();

    std::vector<Entry> mEntries;
    std::unordered_map<LLUUID, size_t> mByMesh;
};

// Faces to set: a face is set only when its current render material is null.
// current[i] = obj->getRenderMaterialID(i); edge_material may be null (then the edge is never set).
std::vector<std::pair<S32, LLUUID>> cutoutApplyPlan(const LLUUID current[FSCutout::FACE_COUNT],
                                                    const LLUUID& material_id, const LLUUID& edge_material);

class LLSnapshotLivePreview;
class LLViewerObject;

// Viewer glue (fscutoutviewer.cpp, not part of the unit test): the snapshot button, the upload and rez hooks.
class FSCutoutMaterials : public LLSingleton<FSCutoutMaterials>
{
    LLSINGLETON(FSCutoutMaterials);
public:
    void createFromSnapshot(LLSnapshotLivePreview* preview);   // the button
    void onMaterialUploaded(const std::string& name, const LLUUID& material_id);
    void onObjectProperties(LLViewerObject* obj, const std::string& name);
    // Static so non-cutouts return before the singleton is even looked up.
    static void onObjectCreated(LLViewerObject* obj);

private:
    bool load();    // lazily, once after login; false before
    void save() const;
    void apply(LLViewerObject* obj, const FSCutoutMaterialStore::Entry& e);

    FSCutoutMaterialStore mStore;   // LL_PATH_PER_SL_ACCOUNT/fs_cutout_materials.xml
    bool mLoaded = false;
};

#endif // FS_CUTOUTMATERIALS_H
