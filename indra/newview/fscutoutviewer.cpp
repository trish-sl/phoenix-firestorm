/**
 * @file fscutoutviewer.cpp
 * @brief Paper cutout: the snapshot button, the material upload and the auto-apply on rez
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

#include "fscutoutgltf.h"
#include "fscutoutmaterials.h"
#include "fscutoutmesh.h"

#include "lldate.h"
#include "lldir.h"
#include "llfloatermodelpreview.h"
#include "llfloaterreg.h"
#include "llgltfmateriallist.h"
#include "llimagepng.h"
#include "llmaterialeditor.h"
#include "llnotificationsutil.h"
#include "llsdserialize.h"
#include "llsnapshotlivepreview.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"

namespace
{
    const U8  ALPHA_THRESHOLD = 128;
    const F32 CUTOUT_HEIGHT_M = 2.f;
    const S32 MAX_TEXTURE_SIDE = 2048;   // SL's texture limit; the outline is traced at this size too
    const char* const STORE_FILE = "fs_cutout_materials.xml";
    const std::string NAME_PREFIX = "Cutout_";

    LLUUID setting_uuid(const char* name)
    {
        std::string s = gSavedSettings.getString(name);
        LLStringUtil::trim(s);
        return LLUUID::validate(s) ? LLUUID(s) : LLUUID::null;
    }

    bool is_cutout_name(const std::string& name)
    {
        return name.compare(0, NAME_PREFIX.size(), NAME_PREFIX) == 0;
    }

    // LLImageRaw keeps the bottom row first; the cutout code and the PNG writer's input order want the top row first.
    void copy_rows_flipped(const U8* src, U8* dst, S32 width, S32 height, S32 components)
    {
        const size_t row = (size_t)width * components;
        for (S32 y = 0; y < height; ++y)
        {
            memcpy(dst + (size_t)y * row, src + (size_t)(height - 1 - y) * row, row);
        }
    }

    bool owned_cutout_shape(const LLViewerObject* obj)
    {
        return obj->permYouOwner() && obj->permModify() && obj->getNumTEs() == FSCutout::FACE_COUNT;
    }
}

FSCutoutMaterials::FSCutoutMaterials()
{
}

bool FSCutoutMaterials::load()
{
    if (mLoaded)
    {
        return true;
    }
    if (gDirUtilp->getLindenUserDir().empty())
    {
        return false;   // not logged in yet: the per-account folder is not known
    }
    mLoaded = true;
    const std::string filename = gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, STORE_FILE);
    llifstream file(filename.c_str());
    if (file.is_open())
    {
        LLSD data;
        LLSDSerialize::fromXMLDocument(data, file);
        mStore.fromLLSD(data);
    }
    return true;
}

void FSCutoutMaterials::save() const
{
    const std::string filename = gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, STORE_FILE);
    llofstream file(filename.c_str());
    if (!file.is_open())
    {
        LL_WARNS("Cutout") << "Unable to save " << filename << LL_ENDL;
        return;
    }
    LLSDSerialize::toPrettyXML(mStore.asLLSD(), file);
}

void FSCutoutMaterials::createFromSnapshot(LLSnapshotLivePreview* preview)
{
    if (!preview || !load())
    {
        return;
    }
    LLPointer<LLImageRaw> image = preview->getEncodedImage();
    if (image.isNull())
    {
        return;
    }

    std::vector<U8> top_first;   // the whole snapshot, RGBA, top row first
    S32 width = 0;
    S32 height = 0;
    {
        LLImageDataSharedLock lock(image);
        width = image->getWidth();
        height = image->getHeight();
        if (!image->getData() ||
            !FSCutout::hasUsableAlpha(image->getData(), width, height, image->getComponents(), ALPHA_THRESHOLD))
        {
            LLNotificationsUtil::add("FSCutoutNoAlpha");
            return;
        }
        top_first.resize((size_t)width * height * 4);
        copy_rows_flipped(image->getData(), top_first.data(), width, height, 4);
    }

    // Crop with twice the margin around the subject, so the dilation by the margin fits inside it.
    const F32 margin_px = gSavedSettings.getF32("FSCutoutMarginPercent") * 0.01f * (F32)height;
    const FSCutout::Rect r = FSCutout::alphaBounds(top_first.data(), width, height, ALPHA_THRESHOLD,
                                                   ll_round(margin_px * 2.f));
    if (r.w <= 0 || r.h <= 0)
    {
        LLNotificationsUtil::add("FSCutoutNoAlpha");
        return;
    }

    // The crop keeps the top row first: scaling does not care about the row order.
    LLPointer<LLImageRaw> crop = new LLImageRaw((U16)r.w, (U16)r.h, 4);
    if (crop->isBufferInvalid())
    {
        LL_WARNS("Snapshot") << "Cutout: no memory for a " << r.w << "x" << r.h << " crop" << LL_ENDL;
        LLNotificationsUtil::add("FSCutoutFailed");
        return;
    }
    for (S32 y = 0; y < r.h; ++y)
    {
        memcpy(crop->getData() + (size_t)y * r.w * 4,
               top_first.data() + ((size_t)(r.y + y) * width + r.x) * 4, (size_t)r.w * 4);
    }
    std::vector<U8>().swap(top_first);

    F32 scale = 1.f;
    const S32 long_side = llmax(r.w, r.h);
    if (long_side > MAX_TEXTURE_SIDE)
    {
        scale = (F32)MAX_TEXTURE_SIDE / (F32)long_side;
        // A full-size crop would freeze the viewer in the tracer for minutes: give up instead.
        if (!crop->scale(llmax(1, ll_round(r.w * scale)), llmax(1, ll_round(r.h * scale))) || crop->isBufferInvalid())
        {
            LL_WARNS("Snapshot") << "Cutout: could not scale the " << r.w << "x" << r.h << " crop" << LL_ENDL;
            LLNotificationsUtil::add("FSCutoutFailed");
            return;
        }
    }
    const S32 img_w = crop->getWidth();
    const S32 img_h = crop->getHeight();

    FSCutout::TraceParams params;
    params.alpha_threshold = ALPHA_THRESHOLD;
    params.margin_px = margin_px * scale;
    params.simplify_px = llmax(0.5f, gSavedSettings.getF32("FSCutoutSimplifyPx"));
    params.cut_holes = gSavedSettings.getBOOL("FSCutoutCutHoles");
    params.min_hole_frac = llmax(0.f, gSavedSettings.getF32("FSCutoutMinHolePercent")) * 0.01f;
    const std::vector<FSCutout::Outline> outlines = FSCutout::traceOutlines(crop->getData(), img_w, img_h, params);
    if (outlines.empty())
    {
        LLNotificationsUtil::add("FSCutoutNoAlpha");
        return;
    }
    const FSCutout::Mesh mesh = FSCutout::buildMesh(outlines, img_w, img_h, CUTOUT_HEIGHT_M,
                                                    llmax(0.001f, gSavedSettings.getF32("FSCutoutThickness")),
                                                    llmax(0.001f, gSavedSettings.getF32("FSCutoutEdgeTileM")));

    // Stretch the picture to power-of-two sides rounded up, so the uploader never shrinks a side
    // (a standing avatar ~290 px wide used to go up 256 wide). UVs run 0..1, so it still lines up.
    S32 tex_w = img_w, tex_h = img_h;
    FSCutout::uploadTextureSize(img_w, img_h, MAX_TEXTURE_SIDE, tex_w, tex_h);
    if ((tex_w != img_w || tex_h != img_h) && (!crop->scale(tex_w, tex_h) || crop->isBufferInvalid()))
    {
        LL_WARNS("Snapshot") << "Cutout: could not scale the picture to " << tex_w << "x" << tex_h << LL_ENDL;
        LLNotificationsUtil::add("FSCutoutFailed");
        return;
    }

    std::vector<U8> rgb;
    LLColor3 paper_color = gSavedSettings.getColor3("FSCutoutPaperColor");
    for (S32 c = 0; c < 3; ++c)
    {
        paper_color.mV[c] = llclamp(paper_color.mV[c], 0.f, 1.f);
    }
    FSCutout::flattenOnto(crop->getData(), tex_w, tex_h, paper_color, rgb);
    LLPointer<LLImageRaw> paper = new LLImageRaw((U16)tex_w, (U16)tex_h, 3);
    if (paper->isBufferInvalid())
    {
        LL_WARNS("Snapshot") << "Cutout: no memory for the " << tex_w << "x" << tex_h << " picture" << LL_ENDL;
        LLNotificationsUtil::add("FSCutoutFailed");
        return;
    }
    copy_rows_flipped(rgb.data(), paper->getData(), tex_w, tex_h, 3);

    // Unique and without spaces: it is the file name, the glTF image uri and the model and material name.
    const std::string name = NAME_PREFIX + LLDate::now().toLocalDateString("%Y-%m-%d_%H-%M-%S");
    const std::string png_path = gDirUtilp->getExpandedFilename(LL_PATH_TEMP, name + ".png");
    const std::string gltf_path = gDirUtilp->getExpandedFilename(LL_PATH_TEMP, name + ".gltf");
    LLPointer<LLImagePNG> png = new LLImagePNG;
    if (!png->encode(paper, 0.f) || !png->save(png_path) ||
        !fs_write_cutout_gltf(gltf_path, name + ".png", mesh, name))
    {
        LL_WARNS("Cutout") << "Unable to write " << gltf_path << LL_ENDL;
        LLNotificationsUtil::add("CannotUploadMaterial");
        return;
    }

    mStore.addPending(name);
    save();

    // The snapshot travels with the material, so the mesh goes up without textures; and with its own normals
    // (flat caps, creased edge), so "Generate normals" stays off.
    if (LLFloaterModelPreview* fmp = LLFloaterReg::getTypedInstance<LLFloaterModelPreview>("upload_model"))
    {
        for (const char* option : { "upload_textures", "gen_normals" })
        {
            LLUICtrl* ctrl = fmp->getChild<LLUICtrl>(option);
            if (ctrl->getValue().asBoolean())
            {
                ctrl->setValue(false);
                ctrl->onCommit();
            }
        }
    }
    LLFloaterModelPreview::showModelPreview(gltf_path);

    // Index 0 opens the editor under the default key. It drops texture ids from the glTF, so set them here.
    LLMaterialEditor::loadMaterialFromFile(gltf_path, 0);
    if (LLMaterialEditor* me = LLFloaterReg::findTypedInstance<LLMaterialEditor>("material_editor"))
    {
        const LLUUID orm = setting_uuid("FSCutoutPaperORM");
        const LLUUID normal = setting_uuid("FSCutoutPaperNormal");
        if (orm.notNull())
        {
            me->setMetallicRoughnessId(orm);
        }
        if (normal.notNull())
        {
            me->setNormalId(normal);
        }
        // The picture also glows at half strength (tint 7F7F7F) so it is not too dark. Emissive points at the
        // base colour's pending upload, so the snapshot is uploaded once; the editor swaps both to the new asset.
        me->setEmissiveId(me->getBaseColorId());
        me->setEmissiveColor(LLColor4(127.f / 255.f, 127.f / 255.f, 127.f / 255.f, 1.f));
        me->setMaterialName(name);   // the editor named it "<file> (<material>)"
    }
}

void FSCutoutMaterials::onMaterialUploaded(const std::string& name, const LLUUID& material_id)
{
    if (!is_cutout_name(name) || material_id.isNull() || !load())
    {
        return;
    }
    if (mStore.setMaterial(name, material_id))
    {
        save();
    }
}

void FSCutoutMaterials::onObjectProperties(LLViewerObject* obj, const std::string& name)
{
    if (!obj || !is_cutout_name(name))
    {
        return;
    }
    const LLSculptParams* sculpt = obj->getSculptParams();
    if (!sculpt || sculpt->getSculptType() != LL_SCULPT_TYPE_MESH || !owned_cutout_shape(obj) || !load())
    {
        return;
    }
    if (const FSCutoutMaterialStore::Entry* e = mStore.learnMesh(name, sculpt->getSculptTexture()))
    {
        const FSCutoutMaterialStore::Entry entry = *e;   // copied: the next store change invalidates e
        save();
        apply(obj, entry);
    }
}

// static
void FSCutoutMaterials::onObjectCreated(LLViewerObject* obj)
{
    // Runs for every object the viewer creates: anything but an owned, modifiable 3-face mesh leaves here.
    const LLSculptParams* sculpt = obj->getSculptParams();
    if (!sculpt || sculpt->getSculptType() != LL_SCULPT_TYPE_MESH || !owned_cutout_shape(obj))
    {
        return;
    }
    FSCutoutMaterials& self = instance();
    if (!self.load())
    {
        return;
    }
    if (const FSCutoutMaterialStore::Entry* e = self.mStore.findByMesh(sculpt->getSculptTexture()))
    {
        self.apply(obj, *e);
    }
}

void FSCutoutMaterials::apply(LLViewerObject* obj, const FSCutoutMaterialStore::Entry& e)
{
    LLUUID current[FSCutout::FACE_COUNT];
    for (S32 te = 0; te < FSCutout::FACE_COUNT; ++te)
    {
        current[te] = obj->getRenderMaterialID((U8)te);
    }
    const std::vector<std::pair<S32, LLUUID>> plan = cutoutApplyPlan(current, e.material_id,
                                                                     setting_uuid("FSCutoutEdgeMaterial"));
    if (plan.empty())
    {
        return;
    }
    for (const auto& face : plan)
    {
        LLGLTFMaterialList::queueApply(obj, face.first, face.second);
    }
    LLGLTFMaterialList::flushUpdates();
}
