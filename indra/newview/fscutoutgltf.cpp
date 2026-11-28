/**
 * @file fscutoutgltf.cpp
 * @brief Paper cutout: writes the cutout mesh as a glTF file beside its texture
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

#include "linden_common.h"

#include "fscutoutgltf.h"

// Header only here; the implementation is defined in gltf/llgltfloader.cpp.
#define TINYGLTF_USE_CPP14
#include "tinygltf/tiny_gltf.h"

#include <cstring>
#include <filesystem>

namespace
{
    // Appends raw data to the single buffer (4 byte aligned: floats and U32 only) and
    // returns the new bufferView index.
    int addView(tinygltf::Model& model, const void* data, size_t bytes, int target)
    {
        std::vector<unsigned char>& buf = model.buffers[0].data;
        tinygltf::BufferView view;
        view.buffer = 0;
        view.byteOffset = buf.size();
        view.byteLength = bytes;
        view.target = target;
        buf.resize(buf.size() + bytes);
        std::memcpy(buf.data() + view.byteOffset, data, bytes);
        model.bufferViews.push_back(view);
        return (int)model.bufferViews.size() - 1;
    }

    int addAccessor(tinygltf::Model& model, int view, int component_type, int type, size_t count)
    {
        tinygltf::Accessor acc;
        acc.bufferView = view;
        acc.componentType = component_type;
        acc.type = type;
        acc.count = count;
        model.accessors.push_back(acc);
        return (int)model.accessors.size() - 1;
    }
}

bool fs_write_cutout_gltf(const std::string& gltf_path, const std::string& png_filename,
                          const FSCutout::Mesh& mesh, const std::string& name)
{
    using FSCutout::FACE_COUNT;
    for (int f = 0; f < FACE_COUNT; ++f)
    {
        if (mesh.positions[f].empty() || mesh.indices[f].empty()
            || mesh.normals[f].size() != mesh.positions[f].size()
            || mesh.uvs[f].size() != mesh.positions[f].size())
        {
            return false;
        }
    }

    tinygltf::Model model;
    model.asset.version = "2.0";
    model.asset.generator = "Firestorm paper cutout";
    model.buffers.emplace_back();
    // External .bin beside the .gltf: the viewer's mesh loader (LL::GLTF::Buffer::prep) rejects data URIs.
    model.buffers[0].uri = std::filesystem::path(gltf_path).stem().string() + ".bin";

    tinygltf::Mesh gmesh;
    gmesh.name = name;
    for (int f = 0; f < FACE_COUNT; ++f)
    {
        const size_t n = mesh.positions[f].size();
        tinygltf::Primitive prim;
        prim.mode = TINYGLTF_MODE_TRIANGLES;
        prim.material = (f == FACE_COUNT - 1) ? 1 : 0;   // front/back textured, edge plain

        // LLVector3/LLVector2 are plain F32 arrays, so they can be copied as they are.
        int pos = addAccessor(model,
            addView(model, mesh.positions[f].data(), n * sizeof(LLVector3), TINYGLTF_TARGET_ARRAY_BUFFER),
            TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, n);
        tinygltf::Accessor& pa = model.accessors[pos];
        pa.minValues = { 1e30, 1e30, 1e30 };
        pa.maxValues = { -1e30, -1e30, -1e30 };
        for (const LLVector3& v : mesh.positions[f])
        {
            for (int c = 0; c < 3; ++c)
            {
                pa.minValues[c] = std::min(pa.minValues[c], (double)v.mV[c]);
                pa.maxValues[c] = std::max(pa.maxValues[c], (double)v.mV[c]);
            }
        }
        prim.attributes["POSITION"] = pos;
        prim.attributes["NORMAL"] = addAccessor(model,
            addView(model, mesh.normals[f].data(), n * sizeof(LLVector3), TINYGLTF_TARGET_ARRAY_BUFFER),
            TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, n);
        prim.attributes["TEXCOORD_0"] = addAccessor(model,
            addView(model, mesh.uvs[f].data(), n * sizeof(LLVector2), TINYGLTF_TARGET_ARRAY_BUFFER),
            TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC2, n);
        prim.indices = addAccessor(model,
            addView(model, mesh.indices[f].data(), mesh.indices[f].size() * sizeof(U32),
                    TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER),
            TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT, TINYGLTF_TYPE_SCALAR, mesh.indices[f].size());
        gmesh.primitives.push_back(prim);
    }
    model.meshes.push_back(gmesh);

    tinygltf::Image image;
    image.uri = png_filename;
    image.mimeType = "image/png";
    model.images.push_back(image);
    tinygltf::Texture tex;
    tex.source = 0;
    model.textures.push_back(tex);

    tinygltf::Material paper;
    paper.name = name;
    paper.pbrMetallicRoughness.baseColorFactor = { 1.0, 1.0, 1.0, 1.0 };
    paper.pbrMetallicRoughness.baseColorTexture.index = 0;
    paper.pbrMetallicRoughness.metallicFactor = 0.0;
    paper.pbrMetallicRoughness.roughnessFactor = 0.9;
    paper.alphaMode = "OPAQUE";
    paper.doubleSided = false;
    model.materials.push_back(paper);

    tinygltf::Material edge;
    // The uploader sorts faces by material name, ignoring case: "Cutout_..." < "edge" keeps the edge face last (TE 2).
    edge.name = "edge";
    edge.pbrMetallicRoughness.baseColorFactor = { 1.0, 1.0, 1.0, 1.0 };
    edge.pbrMetallicRoughness.metallicFactor = 0.0;
    edge.pbrMetallicRoughness.roughnessFactor = 1.0;
    edge.alphaMode = "OPAQUE";
    edge.doubleSided = false;
    model.materials.push_back(edge);

    tinygltf::Node node;
    node.name = name;
    node.mesh = 0;
    model.nodes.push_back(node);
    tinygltf::Scene scene;
    scene.nodes.push_back(0);
    model.scenes.push_back(scene);
    model.defaultScene = 0;

    tinygltf::TinyGLTF writer;
    return writer.WriteGltfSceneToFile(&model, gltf_path, /*embedImages*/ false, /*embedBuffers*/ false,
                                       /*prettyPrint*/ true, /*writeBinary*/ false);
}
