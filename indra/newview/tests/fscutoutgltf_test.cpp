/**
 * @file fscutoutgltf_test.cpp
 * @brief Tests for the paper cutout glTF writer (fscutoutgltf.cpp).
 */

#include "../llviewerprecompiledheaders.h"

#include "../test/lltut.h"

// Same tinygltf setup as indra/llprimitive/tests/llgltfmaterial_test.cpp
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_USE_CPP14
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
// The png is never created here; skip reading external images.
#define TINYGLTF_NO_EXTERNAL_IMAGE 1
#include "tinygltf/tiny_gltf.h"

#include "../fscutoutgltf.h"

#include <algorithm>
#include <filesystem>

// Not "using namespace FSCutout": its Polygon clashes with the Windows GDI Polygon().
using FSCutout::Mesh;
using FSCutout::buildMesh;
using FSCutout::FACE_FRONT;
using FSCutout::FACE_COUNT;
typedef FSCutout::Polygon Poly;

namespace tut
{
    struct fscutoutgltf_data
    {
        std::filesystem::path dir;
        fscutoutgltf_data()
        {
            dir = std::filesystem::temp_directory_path() / "fscutoutgltf_test";
            std::filesystem::create_directories(dir);
        }
        ~fscutoutgltf_data()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    };
    typedef test_group<fscutoutgltf_data> fscutoutgltf_group;
    typedef fscutoutgltf_group::object fscutoutgltf_object;
    tut::fscutoutgltf_group fscutoutgltf_testgroup("fscutoutgltf");

    static Mesh squareMesh()
    {
        Poly sq = { LLVector2(10, 90), LLVector2(90, 90), LLVector2(90, 10), LLVector2(10, 10) };
        return buildMesh({ { sq, {} } }, 100, 100, 2.f, 0.03f, 0.1f);
    }

    template<> template<> void fscutoutgltf_object::test<1>()
    {
        set_test_name("writes_loadable_gltf_with_expected_structure");
        Mesh mesh = squareMesh();
        std::string path = (dir / "Cut.gltf").string();
        ensure("write ok", fs_write_cutout_gltf(path, "Cut.png", mesh, "Cut"));

        tinygltf::Model model;
        tinygltf::TinyGLTF loader;
        std::string err, warn;
        ensure("load ok", loader.LoadASCIIFromFile(&model, &err, &warn, path));
        ensure_equals("no error", err, std::string());

        // The viewer's own glTF mesh loader (LL::GLTF::Buffer::prep) rejects data URIs,
        // so the geometry must live in an external .bin beside the .gltf.
        ensure_equals("1 buffer", (int)model.buffers.size(), 1);
        ensure_equals("buffer uri", model.buffers[0].uri, std::string("Cut.bin"));
        ensure("bin written", std::filesystem::exists(dir / "Cut.bin"));
        ensure_equals("bin size", (size_t)std::filesystem::file_size(dir / "Cut.bin"), model.buffers[0].data.size());

        ensure_equals("1 mesh", (int)model.meshes.size(), 1);
        const tinygltf::Mesh& m = model.meshes[0];
        ensure_equals("3 primitives", (int)m.primitives.size(), 3);
        ensure_equals("2 materials", (int)model.materials.size(), 2);
        const int expect_mat[3] = { 0, 0, 1 };
        for (int i = 0; i < FACE_COUNT; ++i)
        {
            const tinygltf::Primitive& p = m.primitives[i];
            ensure_equals("material index", p.material, expect_mat[i]);
            ensure_equals("mode triangles", p.mode, TINYGLTF_MODE_TRIANGLES);
            ensure("has normal", p.attributes.count("NORMAL") == 1);
            ensure("has uv", p.attributes.count("TEXCOORD_0") == 1);
            const tinygltf::Accessor& pos = model.accessors[p.attributes.at("POSITION")];
            ensure_equals("vertex count", (int)pos.count, (int)mesh.positions[i].size());
            ensure_equals("min size", (int)pos.minValues.size(), 3);
            ensure_equals("max size", (int)pos.maxValues.size(), 3);
            ensure_equals("normal count", (int)model.accessors[p.attributes.at("NORMAL")].count,
                          (int)mesh.normals[i].size());
            ensure_equals("uv count", (int)model.accessors[p.attributes.at("TEXCOORD_0")].count,
                          (int)mesh.uvs[i].size());
            ensure_equals("index count", (int)model.accessors[p.indices].count, (int)mesh.indices[i].size());
        }

        // front positions: min/max match the data
        const tinygltf::Accessor& fpos = model.accessors[m.primitives[FACE_FRONT].attributes.at("POSITION")];
        F32 mn = 1e30f, mx = -1e30f;
        for (const LLVector3& v : mesh.positions[FACE_FRONT])
        {
            mn = std::min(mn, v.mV[0]);
            mx = std::max(mx, v.mV[0]);
        }
        ensure("min x", (F32)fpos.minValues[0] == mn);
        ensure("max x", (F32)fpos.maxValues[0] == mx);

        // front normals read back exactly (0,0,1)
        const tinygltf::Accessor& nacc = model.accessors[m.primitives[FACE_FRONT].attributes.at("NORMAL")];
        const tinygltf::BufferView& bv = model.bufferViews[nacc.bufferView];
        const float* n = reinterpret_cast<const float*>(&model.buffers[bv.buffer].data[bv.byteOffset + nacc.byteOffset]);
        ensure_equals("normal type", nacc.type, TINYGLTF_TYPE_VEC3);
        ensure("front has vertices", nacc.count > 0);
        for (size_t i = 0; i < nacc.count; ++i)
        {
            ensure("nx", n[i * 3 + 0] == 0.f);
            ensure("ny", n[i * 3 + 1] == 0.f);
            ensure("nz", n[i * 3 + 2] == 1.f);
        }

        const tinygltf::Material& m0 = model.materials[0];
        ensure_equals("name", m0.name, std::string("Cut"));
        ensure("metallic", m0.pbrMetallicRoughness.metallicFactor == 0.0);
        ensure("roughness", m0.pbrMetallicRoughness.roughnessFactor == 0.9);
        ensure_equals("alpha mode", m0.alphaMode, std::string("OPAQUE"));
        ensure("single sided", !m0.doubleSided);
        int tex = m0.pbrMetallicRoughness.baseColorTexture.index;
        ensure("base texture", tex >= 0 && tex < (int)model.textures.size());
        int img = model.textures[tex].source;
        ensure("image", img >= 0 && img < (int)model.images.size());
        ensure_equals("uri", model.images[img].uri, std::string("Cut.png"));

        const tinygltf::Material& m1 = model.materials[1];
        ensure_equals("edge name", m1.name, std::string("edge"));
        ensure("edge no texture", m1.pbrMetallicRoughness.baseColorTexture.index < 0);
        ensure("edge roughness", m1.pbrMetallicRoughness.roughnessFactor == 1.0);
        ensure("edge metallic", m1.pbrMetallicRoughness.metallicFactor == 0.0);
        for (int c = 0; c < 4; ++c)
        {
            ensure("edge white", m1.pbrMetallicRoughness.baseColorFactor[c] == 1.0);
        }
    }

    template<> template<> void fscutoutgltf_object::test<2>()
    {
        set_test_name("missing_directory_fails");
        Mesh mesh = squareMesh();
        std::string path = (dir / "nope" / "Cut.gltf").string();
        ensure("write fails", !fs_write_cutout_gltf(path, "Cut.png", mesh, "Cut"));
    }

    template<> template<> void fscutoutgltf_object::test<3>()
    {
        set_test_name("empty_mesh_fails");
        Mesh mesh;
        ensure("write fails", !fs_write_cutout_gltf((dir / "E.gltf").string(), "E.png", mesh, "E"));
    }
}
