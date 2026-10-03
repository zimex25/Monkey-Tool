// nfsnl_model.cpp - .sb3d mesh decoding and FBX / OBJ writing
#include "nfsnl.h"
#include <functional>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <array>
#include <cstdarg>

namespace nfsnl {
static float halfToFloat(uint16_t h) {
    uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, man = h & 1023;
    float v;
    if (exp == 0) v = man / 1024.0f / 16384.0f;
    else if (exp == 31) v = man ? 0.0f : 65504.0f;
    else v = std::ldexp(1.0f + man / 1024.0f, (int)exp - 15);
    return sign ? -v : v;
}

namespace {

struct Reader {
    const uint8_t* d;
    size_t n;
    size_t p = 0;
    bool err = false;
    Reader(const uint8_t* dd, size_t nn) : d(dd), n(nn) {}

    void seek(size_t pos) { p = pos; if (p > n) { p = n; err = true; } }
    void skip(long delta) {
        long np = (long)p + delta;
        if (np < 0 || (size_t)np > n) { err = true; p = n; }
        else p = (size_t)np;
    }
    size_t tell() const { return p; }
    int32_t i32() {
        if (p + 4 > n) { err = true; return 0; }
        int32_t v; memcpy(&v, d + p, 4); p += 4; return v;
    }
    void vec4(float* out) {
        if (p + 16 > n) { err = true; return; }
        memcpy(out, d + p, 16); p += 16;
    }
    void mat4(float* out) {
        if (p + 64 > n) { err = true; return; }
        memcpy(out, d + p, 64); p += 64;
    }
    void readBytes(void* dst, size_t count) {
        if (p + count > n) { err = true; return; }
        memcpy(dst, d + p, count); p += count;
    }
};

// One entry of a mesh's vertex declaration.
//   semantic: 0 position, 4 normal, 5 tangent, 6 bitangent, 8 texcoord, 9 colour
//   type:     0 int8, 1 uint8, 3 uint16
struct VertexAttr {
    uint32_t semantic = 0, usageIndex = 0, type = 0, count = 0, offset = 0;
};

struct Shape {
    std::string name;
    std::vector<VertexAttr> attrs;
    // per-mesh dequantisation, read from the record that follows the
    // attribute table: value * scale + bias
    bool hasTransform = false;
    float posScale[4] = {0,0,0,0}, posBias[4] = {0,0,0,0};
    float uvScale[4] = {0,0,0,0}, uvBias[4] = {0,0,0,0};
    int boneIndex = 0;
    int submeshCount = 0;
    int vertexBufferIndex = 0;
    std::vector<int> faceBufferIndices;
    std::vector<int> faceCounts;
    std::vector<int> materials;     // per submesh, into the file's material table
    // per submesh: the range of the index buffer it draws, and the index size
    // (Real Racing Next shares one index buffer between many shapes)
    std::vector<int> faceFirst, faceSubCount, faceType;
    // Real Racing Next: a handful of vertex declarations, each with one big
    // vertex buffer, that shapes point into (vertexBufferIndex is then the
    // declaration); -1 when the shape has its own declaration and buffer
    int sharedBuffer = -1;
    int stride = 0;
    int vertexCount = 0;
    float bboxMin[4] = {0,0,0,0};
    float bboxMax[4] = {0,0,0,0};
};

// ------------------------------------------------------------- materials
//
// The .sb3d's own string table carries the material names the artist used
// (paint_shader_opaque, chrome_opaque, glass_taillight_..., interior_opaque)
// and the texture each one points at, as a path relative to the model:
//
//     ../../../textures/cars/nissan_skyline_2000gtr/..._interior.sba
//
// The binary link from a mesh to its material sits somewhere this reader does
// not decode yet, but it does not have to be guessed blindly either: a mesh
// is named mesh_<material tag>_<part>_<lod>, and that tag is the same word
// the material and its texture are named after. So the mesh names and the
// file's own material list are matched on that tag.

// The part a mesh belongs to: its name without the leading "mesh_", without
// the material word that starts it, and without the trailing _lodNN.
//   mesh_paint_bumper_rear_a_lod03  ->  bumper_rear_a
//   mesh_chassis_chassis_a_lod00    ->  chassis_a
std::string partOf(const std::string& meshName) {
    std::string s = meshName;
    if (s.rfind("mesh_", 0) == 0) s.erase(0, 5);
    size_t p = s.rfind("_lod");
    if (p != std::string::npos) {
        bool digits = p + 4 < s.size();
        for (size_t i = p + 4; i < s.size() && digits; ++i)
            if (s[i] < '0' || s[i] > '9') digits = false;
        if (digits) s.erase(p);
    }
    size_t u = s.find('_');
    if (u != std::string::npos && u + 1 < s.size()) s.erase(0, u + 1);
    return s.empty() ? meshName : s;
}

// The detail level a mesh belongs to.
//
// The game names a mesh mesh_<material>_<part>_lod<NN>, and that NN really is
// the level: mesh_interior_interior_a_lod00 is the interior at the car's
// highest detail, mesh_interior_interior_a_lod05 the same interior five steps
// down. Up to 0.7 this reader ignored the number and re-derived the level from
// vertex counts and bounding boxes, on the theory that the names counted
// across body kits. That theory cost more than it bought: the re-derivation
// split one ladder into several and restarted each at zero, so LOD00 ended up
// holding lod00, lod05 and lod07 of the very same mesh - which is what showed
// up in 3ds Max as LOD 2 parts sitting inside LOD 0.
//
// Using the stated number fixes that by construction: a number appears once
// per chain, so one chain can never put two meshes in the same level.
//
// A chain whose numbers start above zero simply has no mesh at LOD00, and that
// is the truth rather than a gap to paper over - mesh_chassis_interior_a runs
// lod02..lod04 because the chassis material only takes over the interior once
// the car is far enough away. Inventing an LOD00 for it, as the old code did,
// put low-detail geometry in the high-detail level.
void assignLodLevels(Model& model) {
    for (Mesh& mesh : model.meshes) {
        // the part is the name without the material word and the level, which
        // is what groups a car's meshes the way its art is grouped
        if (mesh.part.empty()) mesh.part = partOf(mesh.name);
        // Collision hulls and the decal placement shell (a grey copy of the
        // body the game projects liveries onto) are never drawn. Left loose
        // they cover the paint in Blender, so they get their own group.
        if (mesh.name.find("collider") != std::string::npos ||
            mesh.name.find("collision") != std::string::npos ||
            mesh.name.find("decal_placement") != std::string::npos) {
            mesh.lod = "HELPERS";
            continue;
        }
        // Real Racing Next names its parts the Real Racing 3 way:
        // LOD_A_BODY, LOD_B_BODY ..., with the damaged, bonnet-camera and
        // interior-camera versions of each as states of their own
        if (mesh.name.size() > 6 && mesh.name.compare(0, 4, "LOD_") == 0 && mesh.name[5] == '_' &&
            mesh.name[4] >= 'A' && mesh.name[4] <= 'Z') {
            const std::string& n = mesh.name;
            if (n.size() > 7 && n.compare(n.size() - 7, 7, "_SHADOW") == 0) mesh.lod = "HELPERS";
            else if (n.find("BONNETCAM") != std::string::npos) mesh.lod = "BONNETCAM";
            else if (n.find("INTCAM") != std::string::npos) mesh.lod = "INTCAM";
            else if (n.find("_DAMAGE") != std::string::npos || n.find("BROKEN") != std::string::npos ||
                     n.find("SHATTER") != std::string::npos || n.find("CRACKED") != std::string::npos)
                mesh.lod = "DAMAGE";
            else {
                char buf[16];
                snprintf(buf, sizeof(buf), "LOD%02d", n[4] - 'A');
                mesh.lod = buf;
            }
            if (mesh.part.empty() || mesh.part == partOf(mesh.name)) mesh.part = n.substr(6);
            continue;
        }
        if (mesh.name.compare(0, 10, "collision_") == 0) { mesh.lod = "HELPERS"; continue; }
        if (!mesh.lod.empty()) continue;      // the node tree already said
        size_t p = mesh.name.rfind("_lod");
        if (p == std::string::npos || p + 4 >= mesh.name.size()) continue;
        int level = 0;
        bool digits = true;
        for (size_t k = p + 4; k < mesh.name.size(); ++k) {
            char c = mesh.name[k];
            if (c < '0' || c > '9') { digits = false; break; }
            level = level * 10 + (c - '0');
        }
        if (!digits || level > 99) continue;
        char buf[16];
        snprintf(buf, sizeof(buf), "LOD%02d", level);
        mesh.lod = buf;
    }
}

// Kit letter and slot of each mesh, from its name once the material words
// and the level are taken off: paint_bumper_front_a -> bumper_front / a,
// hood_y_paint -> hood / y, overfender_large -> overfender_large / '+'.
void assignKits(Model& model) {
    for (Mesh& mesh : model.meshes) {
        if (!mesh.kitSlot.empty() && mesh.kit) continue;   // the node tree already said
        mesh.kit = 0;
        mesh.kitSlot.clear();
        if (mesh.lod == "HELPERS") continue;
        std::string n = mesh.name;
        if (n.rfind("mesh_", 0) == 0) n.erase(0, 5);
        size_t p = n.rfind("_lod");
        if (p != std::string::npos) n.erase(p);
        std::vector<std::string> t, mt;
        auto split = [](const std::string& x, std::vector<std::string>& out) {
            size_t a = 0;
            while (a <= x.size()) {
                size_t b = x.find('_', a);
                if (b == std::string::npos) b = x.size();
                if (b > a) out.push_back(x.substr(a, b - a));
                a = b + 1;
            }
        };
        split(n, t);
        split(mesh.material, mt);
        size_t i = 0;
        while (i < t.size() && i < mt.size() && t[i] == mt[i]) ++i;
        if (i == t.size()) continue;
        std::string slot;
        for (size_t j = i; j < t.size(); ++j) {
            const std::string& w = t[j];
            if (w.size() == 1 && w[0] >= 'a' && w[0] <= 'z' && j > i) {
                mesh.kit = w[0];
                break;
            }
            if (!slot.empty()) slot += '_';
            slot += w;
        }
        mesh.kitSlot = slot;
        if (!mesh.kit) {
            // the tuning add-ons the game fits to every car, which have no letter
            static const char* const addOns[] = {
                "overfender", "wheel_arch_large", "wheel_arch_small", "ducktail",
            };
            for (const char* a : addOns)
                if (slot.find(a) != std::string::npos) { mesh.kit = '+'; break; }
        }
    }
    // A body kit always has its stock version, 'a'. Without one the letters
    // are part of names (jaguar_f_typer: the "f" of F-Type), not kits, and
    // hiding every "kit f" part left No Limits VR's Jaguar interior empty.
    bool stock = false;
    for (const Mesh& m : model.meshes) if (m.kit == 'a') stock = true;
    if (!stock)
        for (Mesh& m : model.meshes)
            if (m.kit >= 'a' && m.kit <= 'z') { m.kit = 0; m.kitSlot.clear(); }
}

std::string materialTag(const std::string& meshName) {
    if (meshName.rfind("mesh_", 0) != 0) return "";
    size_t start = 5;
    size_t end = meshName.find('_', start);
    if (end == std::string::npos) end = meshName.size();
    return meshName.substr(start, end - start);
}

bool looksLikeMaterialName(const std::string& s) {
    if (s.size() < 4 || s.size() > 96) return false;
    if (s.find('/') != std::string::npos || s.find('.') != std::string::npos) return false;
    bool underscore = false;
    for (char c : s) {
        if (c == '_') { underscore = true; continue; }
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    }
    return underscore;
}

// One submesh's triangles inside its mesh's index list, and its material.
struct SubRange { size_t start = 0, count = 0; int material = -1; };

// Render states, spelled out in the material's own name.
void renderStatesFromName(Material& mat) {
    const std::string& m = mat.name;
    auto has = [&](const char* w) { return m.find(w) != std::string::npos; };
    mat.additive = has("alphaadd") || has("additive");
    mat.alphaBlend = mat.additive || has("_alpha") || has("alpha_") ||
                     (has("alpha") && !has("opaque"));
    mat.twoSided = has("twosided");
    mat.noDepthWrite = has("nozwrite");
    size_t lp = m.find("layer");
    if (lp != std::string::npos) {
        int v = 0; bool any = false;
        for (size_t i = lp + 5; i < m.size() && m[i] >= '0' && m[i] <= '9'; ++i) {
            v = v * 10 + (m[i] - '0'); any = true;
        }
        if (any) mat.layer = v;
    }
}

// ---- the .sb3d's own material table ----
//
// The SBIN object data (DATA + OHDR) holds, besides the mesh asset:
//   Assets          one record per asset: name, type (1 mesh, 2 material)
//                   and a data index
//   UserProperties  per material, by that data index, the properties the
//                   artist's scene exported: DiffuseColorTexture,
//                   TransparentColorTexture, DiffuseColor, material_id ...
// and every submesh in the scene description ends with an index into it.
// Read that way, nothing about a mesh's material is guessed. (0.7.6 and
// earlier matched mesh names against material names, which is how the
// Beetle's paint came out as black_gloss_paint and carbon went on parts that
// have none.)
//
// Object records are those of every SBIN v4 file: OHDR entry = offset << 3 |
// kind; kind 1 = an object of (name u16, type u16, value offset u16, value
// size u16) fields, kind 2 = an array (element type u32, count u32, items),
// kind 0 = a fixed struct from the STRU/FIEL schema.
std::vector<Material> readMaterialTable(const std::vector<SbinChunk>& chunks) {
    std::vector<Material> table;
    const SbinChunk* data = findChunk(chunks, "DATA");
    const SbinChunk* oh = findChunk(chunks, "OHDR");
    if (!data || !oh) return table;
    std::vector<std::string> names = sbinNames(chunks);
    const uint8_t* D = data->data;
    size_t n = data->size;
    size_t nObj = oh->size / 4;
    auto u16 = [&](size_t o) -> uint32_t { return o + 2 <= n ? (uint32_t)(D[o] | D[o + 1] << 8) : 0xffffffffu; };
    auto u32 = [&](size_t o) -> uint32_t {
        uint32_t v = 0xffffffffu;
        if (o + 4 <= n) memcpy(&v, D + o, 4);
        return v;
    };
    auto entry = [&](uint32_t idx) -> uint32_t {
        uint32_t v = 0;
        if (idx < nObj) memcpy(&v, oh->data + idx * 4, 4);
        return v;
    };
    auto str = [&](uint32_t i) { return i < names.size() ? names[i] : std::string(); };
    auto fixedSize = [](uint32_t t) -> size_t {
        switch (t) {
            case 0x01: case 0x02: case 0x09: return 1;
            case 0x03: case 0x04: case 0x15: case 0x17: return 2;
            case 0x05: case 0x06: case 0x0a: case 0x0f: case 0x12: case 0x16: return 4;
            case 0x07: case 0x08: case 0x0b: return 8;
            case 0x19: return 12;
            case 0x1a: return 16;
        }
        return 0;
    };
    struct Field { std::string name; uint32_t type; size_t at; };
    auto fields = [&](uint32_t idx) {
        std::vector<Field> out;
        if (idx >= nObj) return out;
        uint32_t e = entry(idx);
        if ((e & 7) != 1) return out;
        size_t off = e >> 3;
        uint32_t cnt = u16(off);
        size_t q = off + 4;
        for (uint32_t i = 0; i < cnt && i < 4096; ++i) {
            if (q + 8 > n) break;
            uint32_t k = u16(q), t = u16(q + 2), vo = u16(q + 4), vs = u16(q + 6);
            size_t va = off + vo;
            size_t sz = vs ? vs : fixedSize(t);
            if (!sz || va + sz > n) break;
            out.push_back({ str(k), t, va });
            q = (va + sz + 1) & ~(size_t)1;
            while (q + 1 < n && D[q] == 0xcd && D[q + 1] == 0xcd) q += 2;
        }
        return out;
    };
    auto array = [&](uint32_t idx) {
        std::vector<uint32_t> items;
        uint32_t e = entry(idx);
        if (idx >= nObj || (e & 7) != 2) return items;
        size_t off = e >> 3;
        if (u32(off) != 0x0f) return items;
        uint32_t cnt = u32(off + 4);
        for (uint32_t i = 0; i < cnt && off + 12 + 4 * (size_t)i <= n && i < 100000; ++i)
            items.push_back(u32(off + 8 + 4 * (size_t)i));
        return items;
    };

    std::vector<uint32_t> props, assets;
    for (const Field& f : fields(0)) {
        if (f.type != 0x0f) continue;
        if (f.name == "UserProperties") props = array(u32(f.at));
        else if (f.name == "Assets") assets = array(u32(f.at));
    }
    if (assets.empty()) return table;

    for (uint32_t a : assets) {
        uint32_t e = entry(a);
        if (a >= nObj || (e & 7) != 0) continue;
        size_t off = (e >> 3) + 2;               // after the struct's 2-byte id
        uint32_t type = u32(off + 4), index = u32(off + 8);
        if (type != 2 || index > 4096) continue;        // 2 = Material
        Material mat;
        mat.name = str(u16(off));
        if (index < props.size())
            for (const Field& f : fields(props[index])) {
                if (f.type == 0x15 && f.name == "DiffuseColorTexture") mat.diffuse = str(u16(f.at));
                else if (f.type == 0x15 && f.name == "TransparentColorTexture" && mat.diffuse.empty())
                    mat.diffuse = str(u16(f.at));
                else if (f.type == 0x1a && f.name == "DiffuseColor") {
                    memcpy(mat.color, D + f.at, 16);
                    mat.hasColor = true;
                }
            }
        renderStatesFromName(mat);
        if (table.size() <= index) table.resize(index + 1);
        table[index] = mat;
    }
    // the maps the game keeps beside a diffuse under the same stem
    std::vector<std::string> textures;
    for (const std::string& s : names)
        if (s.rfind("../", 0) == 0 || s.find("/textures/") != std::string::npos) textures.push_back(s);
    for (Material& mat : table) {
        if (mat.diffuse.empty()) continue;
        std::string stem = stripExtension(mat.diffuse);
        for (const std::string& t : textures) {
            std::string ts = stripExtension(t);
            if (ts == stem + "_normal") mat.normal = t;
            else if (ts == stem + "_reflection") mat.specular = t;
        }
    }
    for (const Material& mat : table) if (mat.name.empty()) return {};   // a hole: do not trust it
    return table;
}

void assignMaterials(Model& model, const std::vector<SbinChunk>& chunks) {
    std::vector<std::string> names = sbinNames(chunks);
    std::vector<std::string> materials, textures;
    for (const std::string& s : names) {
        if (s.rfind("../", 0) == 0 || s.find("/textures/") != std::string::npos) {
            if (std::find(textures.begin(), textures.end(), s) == textures.end())
                textures.push_back(s);
        } else if (looksLikeMaterialName(s)) {
            if (std::find(materials.begin(), materials.end(), s) == materials.end())
                materials.push_back(s);
        }
    }

    // the word a material is named after, e.g. plastic_dull_opaque -> "plastic"
    auto tagOfMaterial = [](const std::string& m) {
        size_t u = m.find('_');
        return u == std::string::npos ? m : m.substr(0, u);
    };

    // How much of `t` the name `m` accounts for, taken greedily in chunks of
    // at least three characters. Used both for material names against texture
    // filenames and for mesh tags against material names.
    auto chunkScore = [](const std::string& t, const std::string& m) {
        size_t total = 0, i = 0;
        while (i < t.size()) {
            size_t bestLen = 0;
            for (size_t len = t.size() - i; len >= 3; --len) {
                if (m.find(t.substr(i, len)) != std::string::npos) { bestLen = len; break; }
            }
            if (!bestLen) { ++i; continue; }
            total += bestLen;
            i += bestLen;
        }
        return total;
    };

    auto pickTexture = [&](const std::string& tag, const std::string& mat) -> std::string {
        const std::string* best = nullptr;
        size_t bestScore = 0;
        for (const std::string& t : textures) {
            std::string file = baseName(t);
            size_t score = chunkScore(mat, file);
            if (!tag.empty() && file.find(tag) != std::string::npos) score += tag.size();
            // on a tie the shorter filename is the plainer match, so
            // headlight_type_a_opaque lands on ..._headlight rather than
            // ..._headlight_additive
            if (score > bestScore ||
                (score == bestScore && score > 0 && best && file.size() < baseName(*best).size())) {
                bestScore = score;
                best = &t;
            }
        }
        return bestScore >= 4 && best ? *best : std::string();
    };

    // A material name spells out how the engine draws it. These are render
    // states, not shader code: any viewer can honour them, and they are what
    // makes glass look like glass and a taillight glow instead of sitting
    // there as a grey lump.
    auto has = [](const std::string& m, const char* w) {
        return m.find(w) != std::string::npos;
    };

    for (const std::string& m : materials) {
        Material mat;
        mat.name = m;
        mat.diffuse = pickTexture(tagOfMaterial(m), m);
        mat.additive = has(m, "alphaadd") || has(m, "additive");
        mat.alphaBlend = mat.additive || has(m, "_alpha") || has(m, "alpha_") ||
                         (has(m, "alpha") && !has(m, "opaque"));
        mat.twoSided = has(m, "twosided");
        mat.noDepthWrite = has(m, "nozwrite");
        size_t lp = m.find("layer");
        if (lp != std::string::npos) {
            int v = 0; bool any = false;
            for (size_t i = lp + 5; i < m.size() && m[i] >= '0' && m[i] <= '9'; ++i) {
                v = v * 10 + (m[i] - '0'); any = true;
            }
            if (any) mat.layer = v;
        }

        // The game keeps the extra maps beside the diffuse under the same
        // stem: ..._alpha.sba, ..._alpha_normal.sba, ..._alpha_reflection.sba.
        if (!mat.diffuse.empty()) {
            std::string stem = stripExtension(mat.diffuse);
            for (const std::string& t : textures) {
                std::string ts = stripExtension(t);
                if (ts == stem + "_normal") mat.normal = t;
                else if (ts == stem + "_reflection") mat.specular = t;
            }
        }
        model.materials.push_back(std::move(mat));
    }

    for (Mesh& mesh : model.meshes) {
        std::string tag = materialTag(mesh.name);
        if (tag.empty()) continue;
        // collision hulls are never drawn, so they get no material
        if (tag == "collider" || tag == "collision") continue;

        // Score each material by how much of the tag its name accounts for.
        // The tag is often a compound - "glasstail" is glass + tail - and a
        // plain prefix match would hand it to glass_headlight, so the tag is
        // consumed greedily in chunks and every chunk has to appear.
        auto& score = chunkScore;
        const std::string* best = nullptr;
        size_t bestLen = 0;
        for (const std::string& m : materials) {
            size_t sc = score(tag, m);
            // a tie goes to the shorter material name, which is the more
            // specific match rather than the one that merely has more words
            if (sc >= 3 && (sc > bestLen || (sc == bestLen && best && m.size() < best->size()))) {
                bestLen = sc;
                best = &m;
            }
        }
        if (best) {
            mesh.material = *best;
            for (const Material& mm : model.materials)
                if (mm.name == *best) { mesh.texture = mm.diffuse; break; }
        } else {
            mesh.material = tag;
            mesh.texture = pickTexture(tag, tag);
        }
    }
}

} // namespace

Model loadSb3d(const uint8_t* data, size_t len, bool flipV) {
    Model model;
    // No Limits' tracks are SBIN too, but a scene of instances, not a car
    if (isNlScene(data, len)) return loadNlScene(data, len);
    auto chunks = sbinChunks(data, len);
    const SbinChunk* bulk = findChunk(chunks, "BULK");
    const SbinChunk* barg = findChunk(chunks, "BARG");
    if (!bulk || !barg) {
        model.warnings.push_back("not a mesh .sb3d (no BULK/BARG)");
        return model;
    }

    // BARG entries: flag 0 = descriptor record, flag 2 = zstd-compressed buffer.
    // The last compressed buffer describes the scene; the rest are geometry.
    size_t count = bulk->size / 8;
    std::vector<Bytes> buffers;
    Bytes information;
    bool sawZstdFailure = false;

    for (size_t i = 0; i < count; ++i) {
        uint32_t off, zsize;
        memcpy(&off, bulk->data + i * 8, 4);
        memcpy(&zsize, bulk->data + i * 8 + 4, 4);
        if ((size_t)off + 4 > barg->size) continue;
        int32_t flag;
        memcpy(&flag, barg->data + off, 4);
        // Real Racing Next (SB3D format 21) stores the scene description
        // uncompressed (flag 0) as the last entry; No Limits compresses it
        bool storedInfo = flag == 0 && i == count - 1;
        if (flag != 2 && flag != 1 && !storedInfo) continue;
        if ((size_t)off + zsize > barg->size || zsize <= 16) continue;
        Bytes buf;
        if (storedInfo) {
            buf.assign(barg->data + off + 16, barg->data + off + zsize);
        } else if (flag == 1) {
            // LZ4 (Real Racing Next); the header's second word is the size
            uint32_t usize;
            memcpy(&usize, barg->data + off + 4, 4);
            if (!lz4DecompressBlock(barg->data + off + 16, zsize - 16, usize, buf)) continue;
        } else {
            if (!zstdAvailable()) { sawZstdFailure = true; break; }
            if (!zstdDecompress(barg->data + off + 16, zsize - 16, 0, buf)) {
                sawZstdFailure = true;
                continue;
            }
        }
        if (i == count - 1) information.swap(buf);
        else buffers.push_back(std::move(buf));
    }

    if (information.empty()) {
        model.warnings.push_back(sawZstdFailure || !zstdAvailable()
            ? "models need Zstandard - place libzstd.dll next to the program"
            : "no scene description found in .sb3d");
        return model;
    }

    if (getenv("MT_SB3D_DUMP")) {
        writeFile(std::string(getenv("MT_SB3D_DUMP")) + ".info", information);
        for (size_t b = 0; b < buffers.size(); ++b)
            writeFile(std::string(getenv("MT_SB3D_DUMP")) + "." + std::to_string(b), buffers[b]);
    }
    // ---- scene description ----
    Reader r(information.data(), information.size());
    int32_t headerSize = r.i32();
    r.i32();
    r.skip(8);
    float tmp[4];
    r.vec4(tmp);
    r.vec4(tmp);

    struct Sub { int32_t count; size_t offset; };
    std::vector<Sub> sub;
    for (int i = 0; i < 8; ++i) {
        int32_t c = r.i32();
        int32_t rel = r.i32();
        sub.push_back({c, (size_t)((long)rel + (long)r.tell() - 4)});
    }
    if (r.err || sub.size() < 8) {
        model.warnings.push_back("malformed scene header");
        return model;
    }
    r.seek((size_t)headerSize);
    // The node tree: sub[2] is each node's parent, sub[4] its record (a
    // 48-byte transform, then at +68 its name). A car's parts hang under
    // nodes named lod_00 .. lod_05 - body/standard/lod_02/mesh_... - and that
    // is the detail level, whatever number the mesh's own name ends in (the
    // 911 carries artist leftovers like _lod148 and _lod52, and a lod_01 node
    // full of meshes still called _lod00).
    std::vector<int> nodeLod;
    // and the body kit: the node under a slot (bumper_front, hood, spoiler,
    // children of the "mesh" node) names the version - standard_type_a,
    // type_b, kit_y, pulled_type_a, large, type_a_carbon ...
    std::vector<std::string> nodeSlot, nodeVariant, treeNames;
    std::vector<int> treeParent;
    if (sub[0].count > 0 && sub[2].count == sub[0].count && sub[4].count == sub[0].count &&
        sub[0].count < 200000) {
        int n = sub[0].count;
        std::vector<int> parent(n, -1);
        std::vector<int> ownLevel(n, -1);
        const uint8_t* info = information.data();
        size_t infoLen = information.size();
        bool ok = sub[2].offset + (size_t)n * 4 <= infoLen && sub[4].offset + (size_t)n * 4 <= infoLen;
        for (int i = 0; ok && i < n; ++i) {
            int32_t p;
            memcpy(&p, info + sub[2].offset + (size_t)i * 4, 4);
            parent[i] = (p >= 0 && p < n) ? p : -1;
            int32_t rel;
            memcpy(&rel, info + sub[4].offset + (size_t)i * 4, 4);
            size_t rec = sub[4].offset + (size_t)i * 4 + (size_t)(long)rel;
            if (rec + 68 + 6 > infoLen) continue;
            const char* nm = (const char*)info + rec + 68;
            if (strncmp(nm, "lod_", 4) == 0 && nm[4] >= '0' && nm[4] <= '9' && nm[5] >= '0' && nm[5] <= '9' &&
                (rec + 68 + 6 >= infoLen || nm[6] == 0))
                ownLevel[i] = (nm[4] - '0') * 10 + (nm[5] - '0');
        }
        std::vector<std::string> nodeName(n);
        for (int i = 0; ok && i < n; ++i) {
            int32_t rel;
            memcpy(&rel, info + sub[4].offset + (size_t)i * 4, 4);
            size_t rec = sub[4].offset + (size_t)i * 4 + (size_t)(long)rel;
            if (rec + 68 >= infoLen) continue;
            size_t e = rec + 68;
            while (e < infoLen && info[e] && e - rec < 400) ++e;
            nodeName[i].assign((const char*)info + rec + 68, e - rec - 68);
        }
        if (ok) { treeNames = nodeName; treeParent = parent; }
        if (ok) {
            nodeSlot.assign(n, std::string());
            nodeVariant.assign(n, std::string());
            for (int i = 0; i < n; ++i) {
                int at = i, below = -1, guard = 0;
                while (at >= 0 && guard++ < 64) {
                    int pa = parent[at];
                    if (pa >= 0 && nodeName[pa] == "mesh") {
                        nodeSlot[i] = nodeName[at];
                        if (below >= 0) nodeVariant[i] = nodeName[below];
                        break;
                    }
                    below = at;
                    at = pa;
                }
            }
        }
        if (ok) {
            nodeLod.assign(n, -1);
            for (int i = 0; i < n; ++i) {
                int at = i, guard = 0;
                while (at >= 0 && guard++ < 64) {
                    if (ownLevel[at] >= 0) { nodeLod[i] = ownLevel[at]; break; }
                    at = parent[at];
                }
            }
        }
    }

    // matrices
    std::vector<std::array<float,16>> matrices;
    r.seek(sub[0].offset);
    for (int i = 0; i < sub[0].count && !r.err; ++i) {
        std::array<float,16> m{};
        r.mat4(m.data());
        matrices.push_back(m);
    }

    // The locators (locator_visual_wheel_front_left and the rest) become
    // named points: they say where the game puts the wheels, lights and
    // exhausts, which a car without brake discs (the cop cars) needs.
    // Each node's matrix is already its place on the car, as the meshes use it.
    for (size_t i = 0; i < treeNames.size() && i < matrices.size(); ++i) {
        if (treeNames[i].rfind("locator_", 0) != 0 && treeNames[i].rfind("J_wheel_", 0) != 0) continue;
        Hardpoint h;
        h.name = treeNames[i];
        const float* m = matrices[i].data();
        h.pos[0] = m[12]; h.pos[1] = m[13]; h.pos[2] = m[14];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) h.basis[r * 3 + c] = m[c * 4 + r];
        model.points.push_back(h);
    }

    // mesh info
    std::vector<Shape> shapes;
    r.seek(sub[6].offset);
    {
        std::vector<size_t> offsets;
        for (int i = 0; i < sub[6].count && !r.err; ++i) {
            size_t here = r.tell();
            int32_t rel = r.i32();
            offsets.push_back(here + (size_t)(long)rel);
        }
        for (size_t i = 0; i < offsets.size(); ++i) {
            r.seek(offsets[i]);
            if (r.err) break;
            Shape sh;
            int32_t size = r.i32();
            r.i32();
            sh.boneIndex = r.i32();
            for (int k = 0; k < 7; ++k) r.i32();
            int nameLen = std::max(0, size - 40);
            std::string nm(nameLen, '\0');
            if (nameLen) r.readBytes(&nm[0], nameLen);
            // the name ends at its first zero; a skinned shape (No Limits VR's
            // drivers) keeps more data in the same field after it
            {
                size_t b = 0;
                while (b < nm.size() && nm[b] == '\0') ++b;
                size_t e = nm.find('\0', b);
                nm = nm.substr(b, e == std::string::npos ? std::string::npos : e - b);
                size_t bad = 0;
                while (bad < nm.size() && (unsigned char)nm[bad] >= 0x20 && (unsigned char)nm[bad] < 0x7F) ++bad;
                nm.resize(bad);
            }
            sh.name = nm;
            r.vec4(sh.bboxMin);
            r.vec4(sh.bboxMax);
            sh.submeshCount = r.i32();
            size_t dbgAt = r.tell();
            int32_t size2 = r.i32();
            r.i32();
            r.i32();
            sh.vertexBufferIndex = r.i32();
            r.i32(); r.i32(); r.i32();
            if (getenv("MT_SB3D")) {
                size_t back = r.tell();
                r.seek(dbgAt);
                fprintf(stderr, "rec %-34s", nm.c_str());
                for (int q = 0; q < 12; ++q) fprintf(stderr, " %d", r.i32());
                fprintf(stderr, "\n");
                r.seek(back);
            }
            if (size2 > 28) r.skip(size2 - 28);
            int32_t skip = r.i32() - 4;
            r.skip(skip);
            // Each submesh is five ints - an offset to its face record, three
            // more, and the index count - and the face record (index count,
            // 3, face buffer) sits either elsewhere or straight after it, in
            // which case the offset is 20. After the last submesh come three
            // ints (4, ?, n) and then n material indices, one per submesh.
            //
            // Up to 0.7.6 every submesh was taken as 32 bytes. That is right
            // for the first one only, so the second of a two-part mesh -
            // an exhaust's tip, a badge's second material - came out of
            // garbage and was lost.
            for (int j = 0; j < sh.submeshCount && !r.err; ++j) {
                size_t save = r.tell();
                int32_t rel = r.i32();
                if (rel < 20) { r.err = true; break; }
                r.i32(); r.i32();
                sh.faceFirst.push_back(r.i32());
                sh.faceSubCount.push_back(r.i32());
                r.seek(save + (size_t)rel);
                sh.faceCounts.push_back(r.i32());
                sh.faceType.push_back(r.i32());
                sh.faceBufferIndices.push_back(r.i32());
                r.seek(save + (rel == 20 ? 32 : 20));
            }
            if (!r.err) {
                r.i32();
                r.i32();
                int32_t n = r.i32();
                for (int k = 0; k < n && k < 64 && !r.err; ++k) sh.materials.push_back(r.i32());
                if (r.err) { sh.materials.clear(); r.err = false; }
            }
            if (r.err) break;
            shapes.push_back(std::move(sh));
        }
    }

    // vertex counts, strides and the per-mesh vertex declaration
    //
    // Record layout: vertexCount, ?, stride, ?, ?, and at byte 18 the number
    // of attributes, followed by that many 28-byte descriptors:
    //   (semantic, usageIndex, type, componentCount, setIndex, byteOffset, 0)
    //
    // Reading this matters: the UV offset is NOT fixed. Measured over a whole
    // car, 1386 meshes carry UVs at byte 12 and 1009 at byte 20, and stride
    // alone does not tell them apart (stride 24 appears in both groups).
    // Assuming byte 20 leaves mesh_paint_* with collapsed UVs and
    // mesh_interior_* with none at all.
    r.err = false;
    r.seek(sub[7].offset);
    {
        std::vector<size_t> offsets;
        for (int i = 0; i < sub[7].count && !r.err; ++i) {
            size_t here = r.tell();
            int32_t rel = r.i32();
            offsets.push_back(here + (size_t)(long)rel);
        }
        const uint8_t* info = information.data();
        size_t infoLen = information.size();
        // Real Racing Next: fewer declarations than shapes; each shape names
        // its declaration, whose buffer it indexes into
        bool shared = !offsets.empty() && offsets.size() < shapes.size();
        std::vector<Shape> decls;
        if (shared) decls.resize(offsets.size());
        for (size_t i = 0; i < offsets.size() && (shared || i < shapes.size()); ++i) {
            Shape& target = shared ? decls[i] : shapes[i];
            size_t o = offsets[i];
            if (o + 20 > infoLen) break;
            uint32_t vc, stride, vbuf;
            memcpy(&vc, info + o, 4);
            memcpy(&vbuf, info + o + 4, 4);
            memcpy(&stride, info + o + 8, 4);
            target.vertexCount = (int)vc;
            target.stride = (int)stride;
            target.sharedBuffer = (int)vbuf;

            uint32_t nAttr = info[o + 18];
            size_t base = o + 20;
            for (uint32_t k = 0; k < nAttr; ++k) {
                if (base + (size_t)k * 28 + 28 > infoLen) break;
                uint32_t f[7];
                memcpy(f, info + base + (size_t)k * 28, 28);
                VertexAttr a;
                a.semantic = f[0];
                a.usageIndex = f[1];
                a.type = f[2];
                a.count = f[3];
                a.offset = f[5];
                if (a.offset < stride) target.attrs.push_back(a);
            }

            // Four vec4s follow the attribute table:
            //   position scale, position bias, texcoord scale, texcoord bias
            // The position pair repeats the bounding box, but the texcoord
            // pair appears nowhere else - without it every mesh's UVs get
            // stretched across the whole texture instead of sitting in the
            // patch the artist assigned them, and since each LOD carries its
            // own pair, the LODs disagree with each other.
            size_t tx = base + (size_t)nAttr * 28;
            if (tx + 64 <= infoLen) {
                memcpy(target.posScale, info + tx, 16);
                memcpy(target.posBias, info + tx + 16, 16);
                memcpy(target.uvScale, info + tx + 32, 16);
                memcpy(target.uvBias, info + tx + 48, 16);
                // sanity: the position pair has to agree with the bounding box
                float span = target.posScale[0] * 65535.0f;
                float bboxSpan = target.bboxMax[0] - target.bboxMin[0];
                target.hasTransform =
                    std::isfinite(span) && std::isfinite(target.uvScale[0]) &&
                    std::isfinite(target.uvBias[0]) &&
                    std::fabs(span - bboxSpan) <= 0.01f * (std::fabs(bboxSpan) + 1e-4f);
            }
            if (!shared) target.sharedBuffer = -1;
        }
        if (shared)
            for (Shape& sh : shapes) {
                int d = sh.vertexBufferIndex;
                if (d < 0 || d >= (int)decls.size()) { sh.stride = 0; continue; }
                const Shape& dc = decls[d];
                sh.vertexCount = dc.vertexCount;
                sh.stride = dc.stride;
                sh.attrs = dc.attrs;
                sh.sharedBuffer = dc.sharedBuffer;
                sh.hasTransform = false;       // float positions, S16 UVs
            }
    }

    if (getenv("MT_SB3D")) {
        for (int q = 0; q < 8; ++q) fprintf(stderr, "sub[%d] count %d\n", q, sub[q].count);
    }
    if (getenv("MT_SB3D"))
        for (const Shape& sh : shapes) {
            fprintf(stderr, "shape %-40s vc %d stride %d vb %d bone %d sub %d bbox %.2f..%.2f attrs:", sh.name.c_str(),
                    sh.vertexCount, sh.stride, sh.vertexBufferIndex, sh.boneIndex, sh.submeshCount,
                    sh.bboxMin[0], sh.bboxMax[0]);
            for (const VertexAttr& a : sh.attrs)
                fprintf(stderr, " [s%u u%u t%u c%u @%u]", a.semantic, a.usageIndex, a.type, a.count, a.offset);
            fprintf(stderr, "\n");
        }
    if (shapes.empty()) {
        model.warnings.push_back("no shapes in .sb3d");
        return model;
    }

    // ---- split the buffer list into face buffers and vertex buffers ----
    bool separate = true;
    std::vector<Bytes> faceBuffers, vertexBuffers;
    {
        int firstFace = shapes[0].faceBufferIndices.empty() ? 0
                        : shapes[0].faceBufferIndices[0];
        const Shape& last = shapes.back();
        int nVertex = last.vertexBufferIndex;
        int nFace = last.faceBufferIndices.empty() ? 0 : last.faceBufferIndices.back();
        if (firstFace == 0) {
            for (int i = 0; i <= nFace && i < (int)buffers.size(); ++i)
                faceBuffers.push_back(buffers[i]);
            for (int i = nFace + 1; i <= nFace + 1 + nVertex && i < (int)buffers.size(); ++i)
                vertexBuffers.push_back(buffers[i]);
        } else {
            separate = false;
        }
    }
    const std::vector<Bytes>& vbList = separate ? vertexBuffers : buffers;
    const std::vector<Bytes>& fbList = separate ? faceBuffers : buffers;

    // ---- decode geometry ----
    //
    // Vertex layout (verified against the geometry itself):
    //    0..5   position   3x uint16, normalised into the shape bounding box
    //    6..7   padding
    //    8..10  normal     3x int8      <- 0.994 mean dot with face normals
    //   11      padding
    //   12..14  tangent    3x int8
    //   15      padding
    //   16..18  bitangent  3x int8
    //   19      padding
    //   20..23  UV         2x uint16 over [0,1]
    //
    // The commonly circulated Blender importer reads UVs at offset 12, which
    // is the tangent - that is why its UVs come out scrambled.
    //
    std::vector<std::vector<SubRange>> meshRanges;     // parallel to model.meshes
    for (const Shape& sh : shapes) {
        bool sharedVb = sh.sharedBuffer >= 0;
        if (sharedVb ? sh.sharedBuffer >= (int)buffers.size()
                     : (sh.vertexBufferIndex < 0 || sh.vertexBufferIndex >= (int)vbList.size()))
            continue;
        if (sh.faceBufferIndices.empty()) continue;
        const Bytes& vb = sharedVb ? buffers[sh.sharedBuffer] : vbList[sh.vertexBufferIndex];
        if (sh.stride <= 0 || sh.vertexCount <= 0) continue;
        // A shared buffer holds many shapes: only the vertices this shape's
        // index ranges use are taken, renumbered from 0.
        std::vector<int> used;
        std::vector<int32_t> remap;
        auto sharedIndex = [&](size_t si, size_t k, uint32_t& v) {
            int fbIdx = sh.faceBufferIndices[si];
            if (fbIdx < 0 || fbIdx >= (int)buffers.size()) return false;
            const Bytes& fb = buffers[fbIdx];
            int wide = si < sh.faceType.size() && sh.faceType[si] == 6 ? 4 : 2;
            size_t at = ((size_t)sh.faceFirst[si] + k) * wide;
            if (at + wide > fb.size()) return false;
            if (wide == 4) memcpy(&v, fb.data() + at, 4);
            else { uint16_t w; memcpy(&w, fb.data() + at, 2); v = w; }
            return v < (uint32_t)sh.vertexCount;
        };
        if (sharedVb) {
            remap.assign((size_t)sh.vertexCount, -1);
            for (size_t si = 0; si < sh.faceBufferIndices.size() && si < sh.faceFirst.size(); ++si)
                for (int k = 0; k < sh.faceSubCount[si]; ++k) {
                    uint32_t v;
                    if (sharedIndex(si, (size_t)k, v) && remap[v] < 0) {
                        remap[v] = (int32_t)used.size();
                        used.push_back((int)v);
                    }
                }
            if (used.empty()) continue;
        }
        int loopCount = sharedVb ? (int)used.size() : sh.vertexCount;

        Mesh mesh;
        mesh.name = sh.name.empty() ? ("mesh_" + std::to_string(model.meshes.size())) : sh.name;
        if (sh.boneIndex >= 0 && (size_t)sh.boneIndex < nodeLod.size() && nodeLod[sh.boneIndex] >= 0) {
            char lb[16];
            snprintf(lb, sizeof(lb), "LOD%02d", nodeLod[sh.boneIndex]);
            mesh.lod = lb;
        }
        if (sh.boneIndex >= 0 && (size_t)sh.boneIndex < nodeSlot.size() && !nodeSlot[sh.boneIndex].empty() &&
            !nodeVariant[sh.boneIndex].empty()) {
            // stock is standard_type_a / type_a / standard; kit_y -> y; the
            // wider arches (pulled, large) and carbon versions are extras
            const std::string& v = nodeVariant[sh.boneIndex];
            char letter = 0;
            if (v.find("carbon") != std::string::npos || v.compare(0, 6, "pulled") == 0 ||
                v.compare(0, 5, "large") == 0)
                letter = '+';
            else if (v.compare(0, 4, "kit_") == 0 && v.size() >= 5)
                letter = v[4];
            else if (v == "standard")
                letter = 'a';
            else {
                size_t t = v.find("type_");
                if (t != std::string::npos && t + 5 < v.size()) letter = v[t + 5];
            }
            if (letter) {
                mesh.kit = letter;
                mesh.kitSlot = nodeSlot[sh.boneIndex];
            }
        }
        for (int k = 0; k < 3; ++k) {
            mesh.bboxMin[k] = sh.bboxMin[k];
            mesh.bboxMax[k] = sh.bboxMax[k];
        }

        float minx = sh.bboxMin[0], miny = sh.bboxMin[1], minz = sh.bboxMin[2];
        float sx = sh.bboxMax[0] - minx, sy = sh.bboxMax[1] - miny, sz = sh.bboxMax[2] - minz;

        // Resolve attribute offsets from the mesh's own vertex declaration
        // rather than assuming fixed positions.
        auto findAttr = [&](uint32_t sem) -> const VertexAttr* {
            const VertexAttr* best = nullptr;
            for (const VertexAttr& a : sh.attrs) {
                if (a.semantic != sem) continue;
                if (!best || a.usageIndex < best->usageIndex) best = &a;
            }
            return best;
        };
        const VertexAttr* aPos = findAttr(0);
        const VertexAttr* aNrm = findAttr(4);
        const VertexAttr* aUv  = findAttr(8);
        const VertexAttr* aCol = findAttr(9);
        // A second texture-coordinate set: on the paint meshes it is the
        // livery / vinyl layout, spread over the whole body in one map. It
        // has no dequantisation of its own (the record's second scale/bias
        // pair reads 1 and 0), so it is taken over [0,1] as stored.
        const VertexAttr* aUv2 = nullptr;
        for (const VertexAttr& a : sh.attrs)
            if (a.semantic == 8 && aUv && a.usageIndex > aUv->usageIndex &&
                (!aUv2 || a.usageIndex < aUv2->usageIndex)) aUv2 = &a;
        bool hasUv2 = aUv2 && aUv2->type == 3 && (int)aUv2->offset + 4 <= sh.stride;
        if (hasUv2) mesh.uvs2.reserve((size_t)sh.vertexCount * 2);
        // colour: uint8 x3/x4 (the usual), int8, or uint16, normalised
        int colBytes = !aCol ? 0 : aCol->type == 3 ? 2 : 1;
        int colN = aCol ? (int)std::min<uint32_t>(aCol->count ? aCol->count : 4, 4) : 0;
        bool hasCol = aCol && colN >= 3 && (aCol->type == 0 || aCol->type == 1 || aCol->type == 3) &&
                      (int)aCol->offset + colN * colBytes <= sh.stride;
        if (hasCol) mesh.colors.reserve((size_t)sh.vertexCount * 4);

        // fall back to the old fixed layout only when the table is absent
        int posOff = aPos ? (int)aPos->offset : 0;
        int nrmOff = aNrm ? (int)aNrm->offset : 8;
        int uvOff  = aUv  ? (int)aUv->offset  : (sh.stride >= 24 ? 20 : -1);
        bool hasUv = uvOff >= 0 && uvOff + 4 <= sh.stride;

        mesh.positions.reserve((size_t)sh.vertexCount * 3);
        mesh.normals.reserve((size_t)sh.vertexCount * 3);
        if (hasUv) mesh.uvs.reserve((size_t)sh.vertexCount * 2);

        const std::array<float,16>* M =
            (sh.boneIndex >= 0 && sh.boneIndex < (int)matrices.size())
            ? &matrices[sh.boneIndex] : nullptr;

        for (int li = 0; li < loopCount; ++li) {
            int i = sharedVb ? used[li] : li;
            size_t base = (size_t)i * sh.stride;
            if (base + sh.stride > vb.size()) break;
            uint16_t px, py, pz;
            memcpy(&px, vb.data() + base + posOff, 2);
            memcpy(&py, vb.data() + base + posOff + 2, 2);
            memcpy(&pz, vb.data() + base + posOff + 4, 2);
            float x = (px / 65535.0f) * sx + minx;
            float y = (py / 65535.0f) * sy + miny;
            float z = (pz / 65535.0f) * sz + minz;
            // Real Racing Next (format 21): plain floats
            bool floatPos = aPos && aPos->type == 5 && posOff + 12 <= sh.stride;
            if (floatPos) {
                memcpy(&x, vb.data() + base + posOff, 4);
                memcpy(&y, vb.data() + base + posOff + 4, 4);
                memcpy(&z, vb.data() + base + posOff + 8, 4);
            }
            // The mesh's own dequantisation, not its bounding box: the two
            // differ by about 0.03% of the span, which is enough to open
            // 0.2 mm seams between neighbouring parts and to put a symmetric
            // bumper 0.3 mm off the centre line. Measured on the Audi TT RS
            // and the Cayenne: the median gap between parts that meet drops
            // from 0.22 mm to 0.02 mm (one quantisation step), and symmetric
            // paint panels centre on x = 0 to within 0.004 mm.
            if (sh.hasTransform && !floatPos) {
                x = px * sh.posScale[0] + sh.posBias[0];
                y = py * sh.posScale[1] + sh.posBias[1];
                z = pz * sh.posScale[2] + sh.posBias[2];
            }

            int8_t nx = (int8_t)vb[base + nrmOff];
            int8_t ny = (int8_t)vb[base + nrmOff + 1];
            int8_t nz = (int8_t)vb[base + nrmOff + 2];
            float fnx = nx, fny = ny, fnz = nz;
            float nl = std::sqrt(fnx*fnx + fny*fny + fnz*fnz);
            if (nl > 0) { fnx /= nl; fny /= nl; fnz /= nl; } else { fnx = 0; fny = 0; fnz = 1; }

            if (M) {
                const float* m = M->data();
                float tx = m[0]*x + m[4]*y + m[8]*z  + m[12];
                float ty = m[1]*x + m[5]*y + m[9]*z  + m[13];
                float tz = m[2]*x + m[6]*y + m[10]*z + m[14];
                x = tx; y = ty; z = tz;
                float vx = m[0]*fnx + m[4]*fny + m[8]*fnz;
                float vy = m[1]*fnx + m[5]*fny + m[9]*fnz;
                float vz = m[2]*fnx + m[6]*fny + m[10]*fnz;
                float l2 = std::sqrt(vx*vx + vy*vy + vz*vz);
                if (l2 > 0) { fnx = vx/l2; fny = vy/l2; fnz = vz/l2; }
            }

            // The engine already uses Y-up / Z-forward / X-right, matching the
            // FBX and OBJ convention, so no axis conversion is applied.
            mesh.positions.push_back(x);
            mesh.positions.push_back(y);
            mesh.positions.push_back(z);
            mesh.normals.push_back(fnx);
            mesh.normals.push_back(fny);
            mesh.normals.push_back(fnz);

            if (hasUv) {
                uint16_t u, v;
                memcpy(&u, vb.data() + base + uvOff, 2);
                memcpy(&v, vb.data() + base + uvOff + 2, 2);
                float fu, fv;
                if (aUv && aUv->type == 2 && getenv("MT_UVHALF")) {
                    fu = halfToFloat(u); fv = halfToFloat(v);
                } else if (aUv && aUv->type == 2) {
                    // signed 16-bit, 2048 to the texture (Real Racing Next)
                    // The rows run top down (DirectX): the headlight lens at
                    // v -0.99..-0.41 is the clear ovals in the top half of
                    // <car>_lights_glass, not the red bars below them. Turned
                    // to the bottom-up convention everything else here uses.
                    fu = (int16_t)u / 2048.0f;
                    fv = 1.0f - (int16_t)v / 2048.0f;
                } else if (aUv && aUv->type == 5 && uvOff + 8 <= sh.stride) {
                    memcpy(&fu, vb.data() + base + uvOff, 4);
                    memcpy(&fv, vb.data() + base + uvOff + 4, 4);
                } else if (sh.hasTransform) {
                    fu = u * sh.uvScale[0] + sh.uvBias[0];
                    fv = v * sh.uvScale[1] + sh.uvBias[1];
                } else {
                    fu = u / 65535.0f;
                    fv = v / 65535.0f;
                }
                mesh.uvs.push_back(fu);
                mesh.uvs.push_back(flipV ? (1.0f - fv) : fv);
            }
            if (hasUv2) {
                uint16_t u, v;
                memcpy(&u, vb.data() + base + aUv2->offset, 2);
                memcpy(&v, vb.data() + base + aUv2->offset + 2, 2);
                float fv = v / 65535.0f;
                mesh.uvs2.push_back(u / 65535.0f);
                mesh.uvs2.push_back(flipV ? 1.0f - fv : fv);
            }
            if (hasCol) {
                const uint8_t* cp = vb.data() + base + aCol->offset;
                float c[4] = {1, 1, 1, 1};
                for (int k = 0; k < colN; ++k) {
                    if (aCol->type == 3) { uint16_t w; memcpy(&w, cp + k * 2, 2); c[k] = w / 65535.0f; }
                    else if (aCol->type == 1) c[k] = cp[k] / 255.0f;
                    else c[k] = ((int8_t)cp[k] + 128) / 255.0f;
                }
                mesh.colors.insert(mesh.colors.end(), c, c + 4);
            }
        }

        std::vector<SubRange> ranges;
        for (size_t si = 0; si < sh.faceBufferIndices.size(); ++si) {
            SubRange sr;
            sr.start = mesh.indices.size();
            sr.material = si < sh.materials.size() ? sh.materials[si]
                        : (sh.materials.empty() ? -1 : sh.materials.back());
            struct Close { std::vector<SubRange>& v; SubRange& s; Mesh& m;
                ~Close() { s.count = m.indices.size() - s.start; if (s.count) v.push_back(s); } }
                close{ranges, sr, mesh};
            if (sharedVb) {
                if (si >= sh.faceFirst.size()) continue;
                for (int k = 0; k + 2 < sh.faceSubCount[si]; k += 3) {
                    uint32_t a, b, c;
                    if (!sharedIndex(si, k, a) || !sharedIndex(si, k + 1, b) || !sharedIndex(si, k + 2, c))
                        continue;
                    mesh.indices.push_back((uint32_t)remap[a]);
                    mesh.indices.push_back((uint32_t)remap[b]);
                    mesh.indices.push_back((uint32_t)remap[c]);
                }
                continue;
            }
            int fbIdx = sh.faceBufferIndices[si];
            if (fbIdx < 0 || fbIdx >= (int)fbList.size()) continue;
            const Bytes& fb = fbList[fbIdx];
            int fc = si < sh.faceCounts.size() ? sh.faceCounts[si] : 0;
            for (int t = 0; t < fc / 3; ++t) {
                size_t o = (size_t)t * 6;
                if (o + 6 > fb.size()) break;
                uint16_t a, b, c;
                memcpy(&a, fb.data() + o, 2);
                memcpy(&b, fb.data() + o + 2, 2);
                memcpy(&c, fb.data() + o + 4, 2);
                mesh.indices.push_back(a);
                mesh.indices.push_back(b);
                mesh.indices.push_back(c);
            }
        }

        if (mesh.positions.empty() || mesh.indices.empty()) continue;
        model.meshes.push_back(std::move(mesh));
        meshRanges.push_back(std::move(ranges));
    }

    // The file's own material table and each submesh's index into it, when
    // both read; the name-matching guess only when they do not.
    std::vector<Material> table = readMaterialTable(chunks);
    bool linked = !table.empty() && meshRanges.size() == model.meshes.size();
    if (linked) {
        for (const auto& rs : meshRanges)
            for (const SubRange& sr : rs)
                if (sr.material < 0 || sr.material >= (int)table.size()) linked = false;
    }
    if (linked) {
        assignLodLevels(model);
        std::vector<Mesh> out;
        out.reserve(model.meshes.size());
        for (size_t i = 0; i < model.meshes.size(); ++i) {
            Mesh& mesh = model.meshes[i];
            // one mesh per material: a two-material mesh becomes two meshes
            std::vector<int> mats;
            for (const SubRange& sr : meshRanges[i])
                if (std::find(mats.begin(), mats.end(), sr.material) == mats.end())
                    mats.push_back(sr.material);
            for (size_t k = 0; k < mats.size(); ++k) {
                Mesh part;
                if (mats.size() == 1) {
                    part = std::move(mesh);
                } else {
                    part = mesh;
                    part.indices.clear();
                    for (const SubRange& sr : meshRanges[i])
                        if (sr.material == mats[k])
                            part.indices.insert(part.indices.end(),
                                                mesh.indices.begin() + sr.start,
                                                mesh.indices.begin() + sr.start + sr.count);
                    if (k) part.name += "_" + table[mats[k]].name;
                }
                const Material& mt = table[mats[k]];
                part.material = mt.name;
                part.texture = mt.diffuse;
                memcpy(part.color, mt.color, sizeof(part.color));
                // Carbon parts: some cars (the Evo X) leave the carbon-fibre
                // pieces on the plastic material with carbon's tiled UVs, and
                // the game swaps the carbon texture in at run time. Do the
                // same, or they show as stretched plastic.
                // A paint panel the file hands to another material: the
                // Beetle's front wings and bonnet (mesh_paint_wheel_arch) are
                // on plastic_dull in the table, yet carry the livery UVs and
                // are body colour in the game. The name and the livery UVs
                // win, or the front of the car comes out black.
                if (part.name.rfind("mesh_paint_", 0) == 0 && !part.uvs2.empty() &&
                    mt.name.find("paint") == std::string::npos) {
                    const Material* paint = nullptr;
                    for (const Material& c : table)
                        if (c.name.find("paint_shader") != std::string::npos && !c.diffuse.empty()) { paint = &c; break; }
                    if (!paint)
                        for (const Material& c : table)
                            if (c.name.rfind("paint", 0) == 0 && !c.diffuse.empty()) { paint = &c; break; }
                    if (paint) {
                        part.material = paint->name;
                        part.texture = paint->diffuse;
                        memcpy(part.color, paint->color, sizeof(part.color));
                    }
                }
                bool carbonPart = part.name.rfind("mesh_carbon_", 0) == 0 ||
                                  part.name.find("_carbon_lod") != std::string::npos;
                if (carbonPart && mt.name.find("carbon") == std::string::npos &&
                    part.texture.find("/common/") != std::string::npos) {
                    size_t sl = part.texture.find_last_of('/');
                    part.texture = part.texture.substr(0, sl + 1) + "texture_carbon.sba";
                    part.material = "carbon_opaque";
                }
                out.push_back(std::move(part));
            }
        }
        model.meshes.swap(out);
        model.materials = table;
    } else {
        assignMaterials(model, chunks);
        assignLodLevels(model);
        model.warnings.push_back("the material table did not read - materials matched by name");
    }

    assignKits(model);
    model.valid = !model.meshes.empty();
    if (!model.valid) model.warnings.push_back("no usable meshes decoded");
    return model;
}

std::string modelKits(const Model& m) {
    std::string k;
    for (const Mesh& mesh : m.meshes)
        if (mesh.kit >= 'b' && mesh.kit <= 'z' && k.find(mesh.kit) == std::string::npos)
            k += mesh.kit;
    std::sort(k.begin(), k.end());
    return k;
}

std::vector<uint8_t> kitMask(const Model& m, const std::string& kit) {
    std::vector<uint8_t> show(m.meshes.size(), 1);
    if (kit == "all" || kit.empty()) return show;
    char want = kit == "stock" ? 'a' : kit[0];
    // which slots the chosen kit has its own version of
    std::vector<std::string> own;
    for (const Mesh& mesh : m.meshes)
        if (mesh.kit == want) own.push_back(mesh.kitSlot);
    std::sort(own.begin(), own.end());
    own.erase(std::unique(own.begin(), own.end()), own.end());
    // A stock part goes when the kit has its own version of that slot. The
    // slot names carry material words where the name had them - the stock
    // hood's chrome trim is chrome_hood, its diffuser plastic_diffuser - so
    // "hood" also takes chrome_hood and "diffuser" plastic_diffuser.
    auto replaced = [&](const std::string& slot) {
        for (const std::string& o : own) {
            if (o.empty()) continue;
            if (slot == o) return true;
            if (slot.size() > o.size() + 1 &&
                ((slot.compare(slot.size() - o.size(), o.size(), o) == 0 &&
                  slot[slot.size() - o.size() - 1] == '_') ||
                 (slot.compare(0, o.size(), o) == 0 && slot[o.size()] == '_')))
                return true;
        }
        return false;
    };
    for (size_t i = 0; i < m.meshes.size(); ++i) {
        const Mesh& mesh = m.meshes[i];
        char k = mesh.kit;
        if (!k || k == want) continue;
        if (k == 'a' && !replaced(mesh.kitSlot)) continue;
        show[i] = 0;
    }
    return show;
}

// ================================================================ writers

namespace {
void appendf(std::string& s, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) s.append(buf, (size_t)std::min<int>(n, (int)sizeof(buf) - 1));
}
}

std::string writeObjString(const Model& m) {
    std::string s;
    s.reserve(1 << 20);
    s += "# exported by Monkey Tool 1.2.2\n";
    size_t baseV = 1;
    for (const Mesh& mesh : m.meshes) {
        // group by LOD so an OBJ viewer shows the same tree the FBX does
        appendf(s, "g %s/%s\n", (mesh.lod.empty() ? std::string("MESHES") : mesh.lod).c_str(),
                (mesh.part.empty() ? std::string("parts") : mesh.part).c_str());
        appendf(s, "o %s\n", mesh.name.c_str());
        if (!mesh.material.empty()) appendf(s, "usemtl %s\n", mesh.material.c_str());
        size_t nv = mesh.positions.size() / 3;
        // Vertex colour rides on the "v" line as three extra numbers - the
        // extension Blender, MeshLab, ZBrush and 3ds Max all read. OBJ has no
        // alpha, so only RGB goes here; the FBX keeps all four channels.
        bool hasCol = mesh.colors.size() / 4 == nv && nv;
        for (size_t i = 0; i < nv; ++i) {
            if (hasCol)
                appendf(s, "v %.6f %.6f %.6f %.4f %.4f %.4f\n", mesh.positions[i*3],
                        mesh.positions[i*3+1], mesh.positions[i*3+2],
                        mesh.colors[i*4], mesh.colors[i*4+1], mesh.colors[i*4+2]);
            else
                appendf(s, "v %.6f %.6f %.6f\n", mesh.positions[i*3],
                        mesh.positions[i*3+1], mesh.positions[i*3+2]);
        }
        bool hasUv = mesh.uvs.size() / 2 == nv;
        if (hasUv)
            for (size_t i = 0; i < nv; ++i)
                appendf(s, "vt %.6f %.6f\n", mesh.uvs[i*2], mesh.uvs[i*2+1]);
        for (size_t i = 0; i < nv; ++i)
            appendf(s, "vn %.6f %.6f %.6f\n", mesh.normals[i*3],
                    mesh.normals[i*3+1], mesh.normals[i*3+2]);
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            size_t a = baseV + mesh.indices[i];
            size_t b = baseV + mesh.indices[i+1];
            size_t c = baseV + mesh.indices[i+2];
            if (hasUv)
                appendf(s, "f %zu/%zu/%zu %zu/%zu/%zu %zu/%zu/%zu\n",
                        a,a,a, b,b,b, c,c,c);
            else
                appendf(s, "f %zu//%zu %zu//%zu %zu//%zu\n", a,a, b,b, c,c);
        }
        baseV += nv;
    }

    // Hardpoints, as a group of single loose vertices. OBJ has no concept of
    // an empty, but a point element survives every importer and keeps the
    // name, so a wheel can still be snapped onto POINT_WHEEL_FL by eye.
    if (!m.points.empty()) {
        s += "# hardpoints from the model's .points file\n";
        for (const Hardpoint& h : m.points) {
            appendf(s, "g HARDPOINTS\no %s\n", h.name.c_str());
            appendf(s, "v %.6f %.6f %.6f\n", h.pos[0], h.pos[1], h.pos[2]);
            appendf(s, "p %zu\n", baseV);
            baseV += 1;
        }
    }
    return s;
}

// meshes whose name carries no _lodNN still need a parent
static const char* const kNoLod = "MESHES";

std::string writeFbxString(const Model& m) {
    std::string s;
    s.reserve(1 << 22);
    s += "; FBX 7.3.0 project file\n"
         "; Exported by Monkey Tool 1.2.2\n\n"
         "FBXHeaderExtension:  {\n"
         "    FBXHeaderVersion: 1003\n"
         "    FBXVersion: 7300\n"
         "    Creator: \"Monkey Tool 1.2.2 by GM25\"\n"
         "}\n"
         "GlobalSettings:  {\n"
         "    Version: 1000\n"
         "    Properties70:  {\n"
         "        P: \"UpAxis\", \"int\", \"Integer\", \"\",1\n"
         "        P: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
         "        P: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n"
         "        P: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n"
         "        P: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n"
         "        P: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n"
         "        P: \"UnitScaleFactor\", \"double\", \"Number\", \"\",1\n"
         "    }\n"
         "}\n\n";

    // body kits other than stock come in hidden, so the car opens as it ships
    std::vector<uint8_t> stock = kitMask(m, "stock");

    // Meshes are grouped under one null per LOD, so a viewer shows
    //   LOD00 -> its meshes,  LOD01 -> its meshes, ...
    std::vector<std::string> lodNames;
    for (const Mesh& mesh : m.meshes) {
        const std::string& l = mesh.lod.empty() ? kNoLod : mesh.lod;
        if (std::find(lodNames.begin(), lodNames.end(), l) == lodNames.end())
            lodNames.push_back(l);
    }
    std::sort(lodNames.begin(), lodNames.end());

    // Under each LOD sits one null per part, so the scene tree reads
    //   LOD00 -> bumper_rear_a -> its meshes
    // which is how the parts are grouped in the game's own art.
    std::vector<std::string> groupKeys;     // "LOD00\x1fbumper_rear_a"
    auto groupKey = [](const Mesh& mesh) {
        std::string l = mesh.lod.empty() ? std::string(kNoLod) : mesh.lod;
        std::string part = mesh.part.empty() ? std::string("parts") : mesh.part;
        return l + "\x1f" + part;
    };
    for (const Mesh& mesh : m.meshes) {
        std::string k = groupKey(mesh);
        if (std::find(groupKeys.begin(), groupKeys.end(), k) == groupKeys.end())
            groupKeys.push_back(k);
    }
    std::sort(groupKeys.begin(), groupKeys.end());

    // one material per distinct name actually used, with every map it has
    std::vector<Material> mats;
    for (const Mesh& mesh : m.meshes) {
        if (mesh.material.empty()) continue;
        bool seen = false;
        for (auto& x : mats) if (x.name == mesh.material) { seen = true; break; }
        if (seen) continue;
        const Material* src = nullptr;
        for (const Material& mm : m.materials)
            if (mm.name == mesh.material) { src = &mm; break; }
        if (src) mats.push_back(*src);
        else { Material mm; mm.name = mesh.material; mm.diffuse = mesh.texture; mats.push_back(mm); }
    }
    auto mapsOf = [](const Material& x) {
        size_t n = 0;
        if (!x.diffuse.empty()) ++n;
        if (!x.normal.empty()) ++n;
        if (!x.specular.empty()) ++n;
        return n;
    };
    size_t nTextures = 0;
    for (auto& x : mats) nTextures += mapsOf(x);

    appendf(s, "Definitions:  {\n    Version: 100\n    Count: %zu\n",
            m.meshes.size() * 2 + lodNames.size() + mats.size() + nTextures + 1);
    s += "    ObjectType: \"GlobalSettings\" {\n        Count: 1\n    }\n";
    appendf(s, "    ObjectType: \"Model\" {\n        Count: %zu\n    }\n",
            m.meshes.size() + lodNames.size() + groupKeys.size());
    appendf(s, "    ObjectType: \"Geometry\" {\n        Count: %zu\n    }\n", m.meshes.size());
    if (!mats.empty())
        appendf(s, "    ObjectType: \"Material\" {\n        Count: %zu\n    }\n", mats.size());
    if (nTextures)
        appendf(s, "    ObjectType: \"Texture\" {\n        Count: %zu\n    }\n", nTextures);
    s += "}\n\nObjects:  {\n";

    long long id = 1000000;
    std::vector<std::pair<long long,long long>> ids;
    for (size_t k = 0; k < m.meshes.size(); ++k) {
        ids.push_back({id, id + 1});
        id += 2;
    }
    std::map<std::string, long long> lodId;
    for (const std::string& l : lodNames) lodId[l] = id++;
    std::map<std::string, long long> groupId;
    for (const std::string& g : groupKeys) groupId[g] = id++;
    std::map<std::string, long long> matId;
    std::map<std::string, long long> texDiffuse, texNormal, texSpecular;
    for (auto& x : mats) {
        matId[x.name] = id++;
        if (!x.diffuse.empty()) texDiffuse[x.name] = id++;
        if (!x.normal.empty()) texNormal[x.name] = id++;
        if (!x.specular.empty()) texSpecular[x.name] = id++;
    }

    for (size_t k = 0; k < m.meshes.size(); ++k) {
        const Mesh& mesh = m.meshes[k];
        std::string name = mesh.name;
        for (auto& c : name) if (c == '"') c = '\'';
        size_t nv = mesh.positions.size() / 3;

        appendf(s, "    Geometry: %lld, \"Geometry::%s\", \"Mesh\" {\n", ids[k].first, name.c_str());
        appendf(s, "        Vertices: *%zu {\n            a: ", nv * 3);
        for (size_t i = 0; i < nv * 3; ++i) {
            appendf(s, "%.6f", mesh.positions[i]);
            if (i + 1 < nv * 3) s += ',';
        }
        s += "\n        }\n";

        appendf(s, "        PolygonVertexIndex: *%zu {\n            a: ", mesh.indices.size());
        for (size_t i = 0; i + 2 < mesh.indices.size() + 1 && i < mesh.indices.size(); i += 3) {
            appendf(s, "%u,%u,%d", mesh.indices[i], mesh.indices[i+1],
                    ~(int)mesh.indices[i+2]);
            if (i + 3 < mesh.indices.size()) s += ',';
        }
        s += "\n        }\n        GeometryVersion: 124\n";

        if (mesh.normals.size() == nv * 3) {
            s += "        LayerElementNormal: 0 {\n            Version: 101\n"
                 "            Name: \"\"\n"
                 "            MappingInformationType: \"ByVertice\"\n"
                 "            ReferenceInformationType: \"Direct\"\n";
            appendf(s, "            Normals: *%zu {\n                a: ", nv * 3);
            for (size_t i = 0; i < nv * 3; ++i) {
                appendf(s, "%.6f", mesh.normals[i]);
                if (i + 1 < nv * 3) s += ',';
            }
            s += "\n            }\n        }\n";
        }
        if (mesh.uvs.size() == nv * 2) {
            s += "        LayerElementUV: 0 {\n            Version: 101\n"
                 "            Name: \"UVMap\"\n"
                 "            MappingInformationType: \"ByVertice\"\n"
                 "            ReferenceInformationType: \"Direct\"\n";
            appendf(s, "            UV: *%zu {\n                a: ", nv * 2);
            for (size_t i = 0; i < nv * 2; ++i) {
                appendf(s, "%.6f", mesh.uvs[i]);
                if (i + 1 < nv * 2) s += ',';
            }
            s += "\n            }\n        }\n";
        }

        bool hasUv2 = nv && mesh.uvs2.size() == nv * 2;
        if (hasUv2) {
            s += "        LayerElementUV: 1 {\n            Version: 101\n"
                 "            Name: \"LiveryUV\"\n"
                 "            MappingInformationType: \"ByVertice\"\n"
                 "            ReferenceInformationType: \"Direct\"\n";
            appendf(s, "            UV: *%zu {\n                a: ", nv * 2);
            for (size_t i = 0; i < nv * 2; ++i) {
                appendf(s, "%.6f", mesh.uvs2[i]);
                if (i + 1 < nv * 2) s += ',';
            }
            s += "\n            }\n        }\n";
        }
        bool hasCol = nv && mesh.colors.size() == nv * 4;
        if (hasCol) {
            s += "        LayerElementColor: 0 {\n            Version: 101\n"
                 "            Name: \"Col\"\n"
                 "            MappingInformationType: \"ByVertice\"\n"
                 "            ReferenceInformationType: \"Direct\"\n";
            appendf(s, "            Colors: *%zu {\n                a: ", nv * 4);
            for (size_t i = 0; i < nv * 4; ++i) {
                appendf(s, "%.4f", mesh.colors[i]);
                if (i + 1 < nv * 4) s += ',';
            }
            s += "\n            }\n        }\n";
        }

        s += "        Layer: 0 {\n            Version: 100\n";
        if (mesh.normals.size() == nv * 3)
            s += "            LayerElement:  {\n"
                 "                Type: \"LayerElementNormal\"\n"
                 "                TypedIndex: 0\n            }\n";
        if (mesh.uvs.size() == nv * 2)
            s += "            LayerElement:  {\n"
                 "                Type: \"LayerElementUV\"\n"
                 "                TypedIndex: 0\n            }\n";
        if (hasCol)
            s += "            LayerElement:  {\n"
                 "                Type: \"LayerElementColor\"\n"
                 "                TypedIndex: 0\n            }\n";
        s += "        }\n";
        // a second layer carries the second UV set, the way FBX expects it
        if (hasUv2)
            s += "        Layer: 1 {\n            Version: 100\n"
                 "            LayerElement:  {\n"
                 "                Type: \"LayerElementUV\"\n"
                 "                TypedIndex: 1\n            }\n        }\n";
        s += "    }\n";

        appendf(s, "    Model: %lld, \"Model::%s\", \"Mesh\" {\n", ids[k].second, name.c_str());
        s += "        Version: 232\n        Properties70:  {\n"
             "            P: \"DefaultAttributeIndex\", \"int\", \"Integer\", \"\",0\n";
        // collision hulls and the decal shell come in hidden
        if (mesh.lod == "HELPERS" || mesh.lod == "DAMAGE" || mesh.lod == "BONNETCAM" || mesh.lod == "INTCAM" || mesh.lod == "COP" || !stock[k])
            s += "            P: \"Visibility\", \"Visibility\", \"\", \"A\",0\n";
        s += "        }\n    }\n";
    }

    for (const std::string& l : lodNames) {
        appendf(s, "    Model: %lld, \"Model::%s\", \"Null\" {\n", lodId[l], l.c_str());
        s += "        Version: 232\n        Properties70:  {\n"
             "            P: \"DefaultAttributeIndex\", \"int\", \"Integer\", \"\",0\n"
             "        }\n    }\n";
    }
    for (const std::string& g : groupKeys) {
        std::string part = g.substr(g.find('\x1f') + 1);
        appendf(s, "    Model: %lld, \"Model::%s\", \"Null\" {\n", groupId[g], part.c_str());
        s += "        Version: 232\n        Properties70:  {\n"
             "            P: \"DefaultAttributeIndex\", \"int\", \"Integer\", \"\",0\n"
             "        }\n    }\n";
    }

    // Hardpoints become empties under one "HARDPOINTS" null, positioned where
    // the game puts them. A wheel, a mirror or a driver dropped onto one of
    // these lands exactly where the game would put it, which is the whole
    // point of shipping them rather than describing them in a text file.
    std::vector<long long> pointIds;
    long long pointsRoot = 0;
    if (!m.points.empty()) {
        pointsRoot = id++;
        for (size_t k = 0; k < m.points.size(); ++k) pointIds.push_back(id++);
        appendf(s, "    Model: %lld, \"Model::HARDPOINTS\", \"Null\" {\n", pointsRoot);
        s += "        Version: 232\n        Properties70:  {\n"
             "            P: \"DefaultAttributeIndex\", \"int\", \"Integer\", \"\",0\n"
             "        }\n    }\n";
        for (size_t k = 0; k < m.points.size(); ++k) {
            const Hardpoint& h = m.points[k];
            std::string name = h.name;
            for (auto& c : name) if (c == '"') c = '\'';
            appendf(s, "    Model: %lld, \"Model::%s\", \"Null\" {\n", pointIds[k], name.c_str());
            s += "        Version: 232\n        Properties70:  {\n"
                 "            P: \"DefaultAttributeIndex\", \"int\", \"Integer\", \"\",0\n";
            appendf(s, "            P: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",%.6f,%.6f,%.6f\n",
                    h.pos[0], h.pos[1], h.pos[2]);
            if (h.hinge) {
                // the axis the part swings about, kept as data rather than
                // baked into a rotation that an importer might reinterpret
                appendf(s, "            P: \"MT_HingeX\", \"Vector3D\", \"Vector\", \"\",%.6f,%.6f,%.6f\n",
                        h.basis[0], h.basis[1], h.basis[2]);
                appendf(s, "            P: \"MT_HingeY\", \"Vector3D\", \"Vector\", \"\",%.6f,%.6f,%.6f\n",
                        h.basis[3], h.basis[4], h.basis[5]);
                appendf(s, "            P: \"MT_HingeZ\", \"Vector3D\", \"Vector\", \"\",%.6f,%.6f,%.6f\n",
                        h.basis[6], h.basis[7], h.basis[8]);
            }
            s += "        }\n    }\n";
        }
    }

    // A texture object, named the way the model refers to the file, with the
    // extension this tool actually writes.
    auto writeTexture = [&](long long tid, const std::string& rel, const char* use) {
        // a Real Racing 3 ".z" wrapper is not part of the texture's name
        std::string bare = extensionOf(rel) == "z" ? stripExtension(rel) : rel;
        std::string png = stripExtension(bare) + ".png";
        std::string file = baseName(png);
        appendf(s, "    Texture: %lld, \"Texture::%s\", \"\" {\n", tid, file.c_str());
        s += "        Type: \"TextureVideoClip\"\n        Version: 202\n";
        appendf(s, "        TextureName: \"Texture::%s\"\n", file.c_str());
        appendf(s, "        Properties70:  {\n"
                   "            P: \"UseMaterial\", \"bool\", \"\", \"\",1\n"
                   "        }\n");
        appendf(s, "        FileName: \"%s\"\n", png.c_str());
        appendf(s, "        RelativeFilename: \"%s\"\n", png.c_str());
        s += "        ModelUVTranslation: 0,0\n        ModelUVScaling: 1,1\n";
        appendf(s, "        Texture_Alpha_Source: \"%s\"\n",
                strcmp(use, "DiffuseColor") == 0 ? "Alpha_Black" : "None");
        s += "        Cropping: 0,0,0,0\n    }\n";
    };

    for (const Material& x : mats) {
        appendf(s, "    Material: %lld, \"Material::%s\", \"\" {\n", matId[x.name],
                x.name.c_str());
        // phong when the game gives it a reflection map, lambert otherwise
        appendf(s, "        Version: 102\n        ShadingModel: \"%s\"\n",
                x.specular.empty() ? "lambert" : "phong");
        s += "        MultiLayer: 0\n        Properties70:  {\n";
        // untextured materials (bronze chrome, gloss black) carry their colour
        if (x.hasColor && x.diffuse.empty())
            appendf(s, "            P: \"DiffuseColor\", \"Color\", \"\", \"A\",%.4f,%.4f,%.4f\n",
                    x.color[0], x.color[1], x.color[2]);
        else
            s += "            P: \"DiffuseColor\", \"Color\", \"\", \"A\",1,1,1\n";
        s += "            P: \"DiffuseFactor\", \"Number\", \"\", \"A\",1\n";
        if (!x.specular.empty())
            s += "            P: \"SpecularColor\", \"Color\", \"\", \"A\",1,1,1\n"
                 "            P: \"ShininessExponent\", \"Number\", \"\", \"A\",40\n";
        if (x.additive) {
            // the engine adds this one to the frame; nothing occludes it
            s += "            P: \"EmissiveColor\", \"Color\", \"\", \"A\",1,1,1\n"
                 "            P: \"EmissiveFactor\", \"Number\", \"\", \"A\",1\n";
        }
        if (x.alphaBlend)
            s += "            P: \"TransparencyFactor\", \"Number\", \"\", \"A\",0.5\n"
                 "            P: \"TransparentColor\", \"Color\", \"\", \"A\",1,1,1\n";
        // the render states the name spells out, kept verbatim so an importer
        // (or a person) can act on them
        appendf(s, "            P: \"NFS_BlendMode\", \"KString\", \"\", \"\", \"%s\"\n",
                x.additive ? "additive" : (x.alphaBlend ? "alpha" : "opaque"));
        appendf(s, "            P: \"NFS_TwoSided\", \"bool\", \"\", \"\",%d\n", x.twoSided ? 1 : 0);
        appendf(s, "            P: \"NFS_DepthWrite\", \"bool\", \"\", \"\",%d\n", x.noDepthWrite ? 0 : 1);
        appendf(s, "            P: \"NFS_Layer\", \"int\", \"Integer\", \"\",%d\n", x.layer);
        s += "        }\n    }\n";

        if (!x.diffuse.empty())  writeTexture(texDiffuse[x.name],  x.diffuse,  "DiffuseColor");
        if (!x.normal.empty())   writeTexture(texNormal[x.name],   x.normal,   "NormalMap");
        if (!x.specular.empty()) writeTexture(texSpecular[x.name], x.specular, "SpecularColor");
    }

    s += "}\n\nConnections:  {\n";
    for (size_t k = 0; k < ids.size(); ++k) {
        const Mesh& mesh = m.meshes[k];
        appendf(s, "    C: \"OO\",%lld,%lld\n", ids[k].first, ids[k].second);
        appendf(s, "    C: \"OO\",%lld,%lld\n", ids[k].second, groupId[groupKey(mesh)]);
        if (!mesh.material.empty() && matId.count(mesh.material))
            appendf(s, "    C: \"OO\",%lld,%lld\n", matId[mesh.material], ids[k].second);
    }
    for (const std::string& g : groupKeys) {
        std::string l = g.substr(0, g.find('\x1f'));
        appendf(s, "    C: \"OO\",%lld,%lld\n", groupId[g], lodId[l]);
    }
    for (const std::string& l : lodNames)
        appendf(s, "    C: \"OO\",%lld,0\n", lodId[l]);
    if (pointsRoot) {
        for (size_t k = 0; k < pointIds.size(); ++k)
            appendf(s, "    C: \"OO\",%lld,%lld\n", pointIds[k], pointsRoot);
        appendf(s, "    C: \"OO\",%lld,0\n", pointsRoot);
    }
    for (const Material& x : mats) {
        if (texDiffuse.count(x.name))
            appendf(s, "    C: \"OP\",%lld,%lld, \"DiffuseColor\"\n",
                    texDiffuse[x.name], matId[x.name]);
        if (texNormal.count(x.name))
            appendf(s, "    C: \"OP\",%lld,%lld, \"NormalMap\"\n",
                    texNormal[x.name], matId[x.name]);
        if (texSpecular.count(x.name))
            appendf(s, "    C: \"OP\",%lld,%lld, \"SpecularColor\"\n",
                    texSpecular[x.name], matId[x.name]);
    }
    s += "}\n";
    return s;
}

static std::string lowerAscii(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

void rr2AssignTextures(Model& m, const std::string& modelPath) {
    std::string stem = stripExtension(baseName(modelPath));
    std::string low = lowerAscii(stem);
    if (low.compare(0, 4, "car_") != 0) return;
    bool interior = low.size() > 4 && low.compare(low.size() - 4, 4, "_int") == 0;
    std::string car = interior ? stem.substr(0, stem.size() - 4) : stem;
    // an outside model with a LOD or damage suffix still shares the car's maps
    for (const char* suf : {"_lod0", "_lod1", "_lod2", "_lod3", "_low", "_high", "_ext"}) {
        size_t n = strlen(suf);
        if (!interior && car.size() > n && lowerAscii(car).compare(car.size() - n, n, suf) == 0) {
            car.erase(car.size() - n);
            break;
        }
    }
    for (Mesh& mesh : m.meshes) {
        if (!mesh.texture.empty()) continue;
        std::string n = lowerAscii(mesh.name);
        auto has = [&](const char* w) { return n.find(w) != std::string::npos; };
        std::string t;
        if (interior) {
            if (has("steering")) t = "_sw";
            else if (has("hood") || has("mirror") || has("bonnet")) t = "_ext_01";
            else t = "_int";
        } else {
            if (has("shadow")) t = "_sha";
            else if (has("blur")) t = "_wheel_blur";
            else if (has("wheel") || has("tyre") || has("tire") || has("rim")) t = "_wheel";
            else if (has("interior") || has("cab") || has("cockpit") || has("seat") ||
                     has("driver")) t = "_cab";
            else t = "_ext_01";
        }
        mesh.texture = car + t + ".pvr";
        if (mesh.material.empty()) mesh.material = lowerAscii(car) + t;
    }
}

std::string rr3PointsNameFor(const std::string& modelPath) {
    std::string base = stripExtension(modelPath);
    // detail variants are named _a, _b, _c and share one .points file
    if (base.size() > 2 && base[base.size() - 2] == '_') {
        char c = base[base.size() - 1];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
            base.erase(base.size() - 2);
    }
    return base + ".points";
}

bool writeModel(const Model& m, const std::string& format, Bytes& out) {
    // a file of groups alone still exports: its groups become empties
    if ((!m.valid || m.meshes.empty()) && m.points.empty()) return false;
    std::string s = (format == "obj") ? writeObjString(m) : writeFbxString(m);
    out.assign(s.begin(), s.end());
    return !out.empty();
}

bool convertSb3d(const uint8_t* data, size_t len, const std::string& format,
                 Bytes& out, std::string* error,
                 const uint8_t* pointsData, size_t pointsLen) {
    // loadModel picks the reader from the file's own magic, so an .sb3d that
    // is really an M3G blob still converts.
    Model m = loadModel(data, len);
    if (!m.valid && m.points.empty()) {
        if (error) *error = m.warnings.empty() ? "no meshes" : m.warnings[0];
        return false;
    }
    if (pointsData && pointsLen) {
        std::vector<Hardpoint> pts = rr3ReadPoints(pointsData, pointsLen);
        if (!pts.empty()) rr3PlaceParts(m, pts);
    }
    std::string s = (format == "obj") ? writeObjString(m) : writeFbxString(m);
    out.assign(s.begin(), s.end());
    return !out.empty();
}

// ================================================================ top level


// Real Racing Next: the texture files a material of <car> samples, best first.
// The atlases' own materials (vehicles/common_materials/atlas/*.sbma) say
// which of the car's textures in vehicles/<car>/car_textures/ each one uses;
// tiled materials (leather, carbon) come from vehicles/common_textures.
std::vector<std::string> rrNextTextureCandidates(const std::string& car, const std::string& mat) {
    std::vector<std::string> cand;
    // the paint is the game's paint shader (a colour ramp from the car's
    // skin) over black in <car>_ext: sampling _ext would paint the car black
    if (mat == "paint" || mat == "paint_interior") return cand;
    if (mat == "shadow" || mat == "shadow_interior") { cand.push_back("car_ambient_shadow.sba"); return cand; }
    // light lenses: the car's own see-through lens texture before the atlas
    if (mat == "lights_glass" || mat == "lights_glass_damage") cand.push_back(car + "_lights_glass.sba");
    // the atlases' own materials (common_materials/atlas/*.sbma)
    // say which of the car's textures each one samples
    static const struct { const char* mat; const char* tex; } kAtlas[] = {
        { "caliper", "_ext" }, { "chasis", "_ext" }, { "chassis", "_ext" },
        { "rotor", "_ext" }, { "undercarriage", "_ext" }, { "paint_interior", "_ext" },
        { "lights_glass", "_lights" }, { "lights_glass_damage", "_lights" },
        { "misc_interior", "_int" }, { "misc_interior_alpha", "_int_alpha" },
        { "wheel_damage", "_wheel" }, { "wheel_metalness", "_wheel" },
        { "caliper_02", "_brake_caliper_02" }, { "sw_alpha", "_sw_alpha" },
        { "glass", "=car_windows.sba" }, { "glass_interior", "=car_windows.sba" },
        { "windows", "=car_windows.sba" }, { "windows_02", "=car_windows.sba" },
        { "windows_cracked", "=car_windows.sba" }, { "windows_shattered", "=car_windows.sba" },
        { "windows_interior", "=car_windows_interior.sba" },
        { "windows_heating_01", "=car_windows_heating.sba" },
        { "mirror", "=car_glass_mirror.sba" }, { "mirror_interior", "=car_glass_mirror.sba" },
        { "tire", "=tyre_tread_01.sba" }, { "tire_2", "=tyre_racing_classic.sba" },
        { "tire_racing", "=tire_classic_racing.sba" },
    };
    for (const auto& a : kAtlas)
        if (mat == a.mat) cand.push_back(a.tex[0] == '=' ? std::string(a.tex + 1) : car + a.tex + ".sba");
    if (mat.compare(0, 5, "tire_") == 0 || mat.compare(0, 5, "tyre_") == 0) cand.push_back("tyre_tread_01.sba");
    if (mat == "paint" || mat == "combined" || mat.compare(0, 5, "paint") == 0)
        cand.push_back(car + "_ext.sba");
    cand.push_back(car + "_" + mat + ".sba");
    std::string trimmed = mat;
    for (const char* suf : { "_interior_alpha", "_interior", "_damage", "_alpha", "_02", "_01" }) {
        size_t n = strlen(suf);
        if (trimmed.size() > n && trimmed.compare(trimmed.size() - n, n, suf) == 0)
            trimmed.erase(trimmed.size() - n);
    }
    if (trimmed != mat) cand.push_back(car + "_" + trimmed + ".sba");
    cand.push_back(mat + ".sba");
    cand.push_back(mat + "_01.sba");
    if (trimmed != mat) cand.push_back(trimmed + ".sba");
    return cand;
}

// Real Racing Next: the wheel is modelled once at the origin (LOD_A_WHEEL_
// FRONT_RIM, ..._FRONTLEFT_TIRE, ..._ROTOR, ..._CALIPER), its face at x = 0
// and the brake inboard towards +x. The car's <car>_carpoints.sb places the
// four corners by their wheel arches (POINT_WHEEL_ARCH_FL ...): the hub sits
// under the arch's centre, one tyre radius off the ground, the wheel's face
// a little outside the arch.
int rrNextAttachWheels(Model& car, const uint8_t* carpoints, size_t len) {
    std::string t = sbinObjectsText(carpoints, len);
    std::map<std::string, std::array<float, 3>> pts;
    std::map<std::string, std::array<float, 4>> rots;
    size_t p = 0;
    while ((p = t.find("CarPointID = ", p)) != std::string::npos) {
        size_t e = t.find('\n', p);
        std::string id = t.substr(p + 13, e - p - 13);
        while (!id.empty() && (id.back() == '\r' || id.back() == ' ' || id.back() == '"')) id.pop_back();
        if (!id.empty() && id[0] == '"') id.erase(0, 1);
        size_t tr = t.find("Translation = [", e);
        size_t next = t.find("CarPointID = ", e);
        if (tr != std::string::npos && (next == std::string::npos || tr < next)) {
            std::array<float, 3> v{};
            if (sscanf(t.c_str() + tr + 15, "%f, %f, %f", &v[0], &v[1], &v[2]) == 3) pts[id] = v;
        }
        size_t ro = t.find("Rotation = [", e);
        if (ro != std::string::npos && (next == std::string::npos || ro < next)) {
            std::array<float, 4> q{};
            if (sscanf(t.c_str() + ro + 12, "%f, %f, %f, %f", &q[0], &q[1], &q[2], &q[3]) == 4) rots[id] = q;
        }
        p = e;
    }
    static const char* kArch[4] = { "POINT_WHEEL_ARCH_FL", "POINT_WHEEL_ARCH_FR", "POINT_WHEEL_ARCH_RL", "POINT_WHEEL_ARCH_RR" };
    static const char* kTag[4] = { "FL", "FR", "RL", "RR" };
    // POINT_WHEEL_FL ... are the wheels themselves: the centre of the wheel's
    // face, and its turn (the right-hand ones half a turn about y). The low
    // LOD's LOD_F_MERGED_WHEELS, all four wheels in one mesh, sits exactly
    // there - the tyres reach 17 cm below the body's floor.
    static const char* kWheelPt[4] = { "POINT_WHEEL_FL", "POINT_WHEEL_FR", "POINT_WHEEL_RL", "POINT_WHEEL_RR" };
    bool wheelPts = true;
    for (const char* a : kWheelPt) if (!pts.count(a)) wheelPts = false;
    if (!wheelPts) for (const char* a : kArch) if (!pts.count(a)) return 0;
    auto isWheel = [](const Mesh& m) {
        const std::string& n = m.name;
        if (n.find("_WHEEL_") == std::string::npos || n.find("STEERING") != std::string::npos ||
            n.find("INTCAM") != std::string::npos) return false;
        for (int c = 0; c < 3; ++c)
            if (std::fabs((m.bboxMin[c] + m.bboxMax[c]) * 0.5f) > 0.45f) return false;
        return true;
    };
    bool haveRear = false;
    float radius = 0, ground = 1e30f;
    for (const Mesh& m : car.meshes) {
        if (isWheel(m)) {
            if (m.name.find("REAR") != std::string::npos && m.name.find("FRONT") == std::string::npos &&
                m.name.find("RIM") != std::string::npos) haveRear = true;
            if (m.name.find("TIRE") != std::string::npos || m.name.find("TYRE") != std::string::npos)
                radius = std::max(radius, std::max(m.bboxMax[1], -m.bboxMin[1]));
        } else if (m.lod == "LOD00") {
            ground = std::min(ground, m.bboxMin[1]);
        }
    }
    if (radius <= 0) return 0;
    if (ground > 1e29f) ground = 0;
    std::vector<Mesh> added;
    std::vector<bool> used(car.meshes.size(), false);
    for (int w = 0; w < 4; ++w) {
        const std::array<float, 3> arch = wheelPts ? pts[kWheelPt[w]] : pts[kArch[w]];
        std::array<float, 4> rq = { 0, 0, 0, 1 };
        if (wheelPts && rots.count(kWheelPt[w])) rq = rots[kWheelPt[w]];
        bool right = arch[0] > 0, rear = w >= 2;
        auto rot = [&](float* v) {        // v' = q v q*
            float qx = rq[0], qy = rq[1], qz = rq[2], qw = rq[3];
            float tx = 2 * (qy * v[2] - qz * v[1]), ty = 2 * (qz * v[0] - qx * v[2]), tz = 2 * (qx * v[1] - qy * v[0]);
            float r0 = v[0] + qw * tx + (qy * tz - qz * ty);
            float r1 = v[1] + qw * ty + (qz * tx - qx * tz);
            float r2 = v[2] + qw * tz + (qx * ty - qy * tx);
            v[0] = r0; v[1] = r1; v[2] = r2;
        };
        float outer = std::fabs(arch[0]) + 0.04f;
        float hubY = ground + radius;
        for (size_t i = 0; i < car.meshes.size(); ++i) {
            const Mesh& m = car.meshes[i];
            if (!isWheel(m)) continue;
            bool hasRear = m.name.find("REAR") != std::string::npos;
            bool hasFront = m.name.find("FRONT") != std::string::npos;
            // a rear-only wheel (..._REAR_RIM) goes on the rear corners and
            // then the front-only ones stay at the front; FRONT_REAR is both
            if (haveRear) {
                if (hasRear && !hasFront && !rear) continue;
                if (hasFront && !hasRear && rear) continue;
            }
            used[i] = true;
            Mesh c = m;
            c.name = m.name + "_" + kTag[w];
            c.part = std::string("wheel_") + kTag[w];
            if (wheelPts) {
                for (size_t k = 0; k + 2 < c.positions.size(); k += 3) {
                    rot(&c.positions[k]);
                    for (int q = 0; q < 3; ++q) c.positions[k + q] += arch[q];
                }
                for (size_t k = 0; k + 2 < c.normals.size(); k += 3) rot(&c.normals[k]);
            } else
            for (size_t k = 0; k + 2 < c.positions.size(); k += 3) {
                float x = c.positions[k];
                c.positions[k] = right ? outer - x : x - outer;
                c.positions[k + 1] += hubY;
                c.positions[k + 2] += arch[2];
            }
            if (right && !wheelPts) {
                for (size_t k = 0; k + 2 < c.indices.size(); k += 3) std::swap(c.indices[k + 1], c.indices[k + 2]);
                for (size_t k = 0; k + 2 < c.normals.size(); k += 3) c.normals[k] = -c.normals[k];
            }
            for (int q = 0; q < 3; ++q) { c.bboxMin[q] = 1e30f; c.bboxMax[q] = -1e30f; }
            for (size_t k = 0; k + 2 < c.positions.size(); k += 3)
                for (int q = 0; q < 3; ++q) {
                    c.bboxMin[q] = std::min(c.bboxMin[q], c.positions[k + q]);
                    c.bboxMax[q] = std::max(c.bboxMax[q], c.positions[k + q]);
                }
            added.push_back(std::move(c));
        }
    }
    if (added.empty()) return 0;
    std::vector<Mesh> kept;
    for (size_t i = 0; i < car.meshes.size(); ++i) if (!used[i]) kept.push_back(std::move(car.meshes[i]));
    car.meshes = std::move(kept);
    for (Mesh& m : added) car.meshes.push_back(std::move(m));
    car.warnings.push_back(wheelPts ? "wheels placed on the car's POINT_WHEEL_* carpoints"
                                    : "wheels placed at the four wheel arches of the car's carpoints");
    return 4;
}

std::vector<std::string> formatsFor(const std::string& assetPath) {
    std::string e = extensionOf(assetPath);
    // No Limits' own .m3g still needs LZHAM, so text and raw are offered too -
    // an export that always produces something beats a dialog that only says
    // no. Real Racing 3's .m3g converts outright.
    if (e == "m3g") return {"fbx", "obj", "png", "txt", "raw"};
    if (e == "sb3d") return {"fbx", "obj", "txt", "raw"};
    if (e == "points") return {"txt", "raw"};
    // the decoded .bin is what a hex editor wants; the same XOR puts it back
    if (e == "nct") return {"txt", "bin", "raw"};
    // a .gui is XML under the same pad
    if (e == "gui") return {"xml", "txt", "raw"};
    // No Limits' tracks: geometry in an .sba
    if (e == "sba" && assetPath.find(".scene_static.sba") != std::string::npos &&
        assetPath.find("lightprobes") == std::string::npos &&
        assetPath.find("exclusions") == std::string::npos &&
        assetPath.find("lightmaps") == std::string::npos)
        return {"fbx", "obj", "txt", "raw"};
    if (e == "sba" || e == "pvr" || e == "ktx" || e == "dds")
        return {"png", "jpg", "bmp", "tga", "dds", "raw"};
    if (e == "wem") return {"wav", "raw"};
    if (e == "gnsu") return {"wav", "txt", "raw"};
    if (e == "sps") return {"wav", "raw"};
    if (e == "ogg" || e == "mp3" || e == "flac" || e == "m4a" || e == "aac" || e == "opus" ||
        e == "wma" || e == "fsb" || e == "xma" || e == "adx" || e == "at3" || e == "wav")
        return {"wav", "raw"};
    if (e == "bnk") return {"wem", "wav", "txt", "raw"};
    // No Limits' .sb data files: the decrypted SBIN is what an editor wants
    if (e == "sb") return {"txt", "sbin", "raw"};
    if (e == "png" || e == "jpg" || e == "jpeg") return {"png", "jpg", "bmp", "tga", "dds", "raw"};
    // text already: saved as it is, or as .txt for an editor
    if (e == "xml" || e == "gui" || e == "json" || e == "lua" || e == "txt" || e == "ini" || e == "config")
        return {"raw", "txt"};
    if (e == "sbfx" || e == "bin") return {"txt", "raw"};
    return {"txt", "raw"};
}

bool convertAsset(const std::string& assetPath, const uint8_t* data, size_t len,
                  const std::string& format, Bytes& out, std::string* error,
                  std::string* usedExtension,
                  const uint8_t* sidecar, size_t sidecarLen) {
    std::string e = extensionOf(assetPath);

    // Text first: every asset can be written out as text, whatever else the
    // tool can or cannot do with it.
    if (format == "txt") {
        // Real Racing 3's data files as the text the Import editor reads back
        if (e == "sounddef" || e == "evt") {
            std::string text, why;
            if (dataEditableText(assetPath, data, len, text, why)) {
                out.assign(text.begin(), text.end());
                if (usedExtension) *usedExtension = "txt";
                return true;
            }
        }
        std::string text = assetText(assetPath, data, len);
        out.assign(text.begin(), text.end());
        if (usedExtension) *usedExtension = "txt";
        return true;
    }
    if (format == "sbin") {
        std::string how;
        if (!nlSbDecode(assetPath, data, len, out, &how)) {
            if (error) *error = "not an SBIN file, and it did not decrypt: " + how;
            return false;
        }
        if (usedExtension) *usedExtension = "sbin";
        return true;
    }
    // a texture shipped as an .m3g (Hot Pursuit): its Image2D as a picture
    if (e == "m3g" && (format == "png" || format == "jpg" || format == "bmp" || format == "tga")) {
        Image img;
        if (!decodeTextureFile(data, len, img)) {
            if (error) *error = "this .m3g holds no picture (it is a model - save it as FBX or OBJ)";
            return false;
        }
        out = format == "jpg" ? encodeJpeg(img, 92) : format == "bmp" ? encodeBmp(img)
            : format == "tga" ? encodeTga(img) : encodePng(img);
        if (usedExtension) *usedExtension = format;
        return !out.empty();
    }
    // Real Racing 3's .m3g is a real JSR-184 file: it converts here and never
    // reaches the LZHAM branches below, which are about No Limits' unrelated
    // container of the same name.
    if (e == "m3g" && isJsr184(data, len) && format != "raw") {
        bool ok = convertSb3d(data, len, format, out, error, sidecar, sidecarLen);
        if (usedExtension) *usedExtension = (format == "obj") ? "obj" : "fbx";
        return ok;
    }
    // No Limits' own .m3g, once LZHAM has unpacked it, is IM4M3G - the Most
    // Wanted 2012 layout with one more header word - and converts the same.
    // "raw" then gives the unpacked M3G itself.
    if (e == "m3g" && m3gWrapperInfo(data, len, nullptr)) {
        Bytes plain;
        std::string why;
        if (m3gUnwrap(data, len, plain, &why)) {
            if (format == "raw") {
                out.swap(plain);
                if (usedExtension) *usedExtension = "m3g";
                return true;
            }
            if (format != "txt") {
                bool ok = convertSb3d(data, len, format, out, error, sidecar, sidecarLen);
                if (usedExtension) *usedExtension = (format == "obj") ? "obj" : "fbx";
                return ok;
            }
        }
    }
    if (e == "m3g" && identifierVersionOf(data, len) >= 2 && format != "raw" && format != "txt") {
        bool ok = convertSb3d(data, len, format, out, error, sidecar, sidecarLen);
        if (usedExtension) *usedExtension = (format == "obj") ? "obj" : "fbx";
        return ok;
    }
    if (e == "m3g" && format == "raw") {
        // the compressed payload on its own, for anyone who wants to work on
        // the codec
        size_t inside = 0;
        if (m3gWrapperInfo(data, len, &inside) && len > 10) {
            out.assign(data + 10, data + len);
            if (usedExtension) *usedExtension = "bin";
            return true;
        }
    }
    if (e == "m3g" && format != "txt") {
        size_t inside = 0;
        if (m3gWrapperInfo(data, len, &inside)) {
            char msg[320];
            if (lzhamAvailable())
                snprintf(msg, sizeof(msg),
                         "this .m3g is LZHAM-compressed (%zu bytes inside) and an LZHAM "
                         "library is loaded, but the stream did not decode.", inside);
            else
                snprintf(msg, sizeof(msg),
                         "this .m3g is LZHAM-compressed (the game's DA BD wrapper, "
                         "%zu bytes inside) and the LZHAM decoder did not start.", inside);
            if (error) *error = msg;
            return false;
        }
    }
    if (e == "m3g" && !isM3g(data, len) &&
        !(len >= 4 && memcmp(data, "SBIN", 4) == 0)) {
        // Not JSR-184 and not an SBIN blob either. Say what it actually is
        // rather than blaming the .sb3d reader for a file that is not one.
        if (error) {
            char head[64];
            snprintf(head, sizeof(head), "%02X %02X %02X %02X %02X %02X %02X %02X",
                     len > 0 ? data[0] : 0, len > 1 ? data[1] : 0,
                     len > 2 ? data[2] : 0, len > 3 ? data[3] : 0,
                     len > 4 ? data[4] : 0, len > 5 ? data[5] : 0,
                     len > 6 ? data[6] : 0, len > 7 ? data[7] : 0);
            *error = std::string("this .m3g is not JSR-184 M3G - it starts with ") + head +
                     ". Send the file and the format can be added.";
        }
        return false;
    }
    if (e == "gui" && format == "xml") {
        size_t covered = len;
        if (len >= 5 && !memcmp(data, "<?xml", 5)) out.assign(data, data + len);
        else nctTransform(data, len, out, &covered);
        if (usedExtension) *usedExtension = "xml";
        if (covered < len && error) {
            char msg[192];
            snprintf(msg, sizeof(msg),
                     "only the first %zu of %zu bytes are covered by the known pad; "
                     "the rest are written unchanged", covered, len);
            *error = msg;
        }
        return true;
    }
    if (e == "nct" && format == "bin") {
        size_t covered = 0;
        nctTransform(data, len, out, &covered);
        if (usedExtension) *usedExtension = "bin";
        if (covered < len && error) {
            char msg[192];
            snprintf(msg, sizeof(msg),
                     "only the first %zu of %zu bytes are covered by the known pad; "
                     "the rest are written unchanged", covered, len);
            *error = msg;
        }
        return true;
    }
    // "raw" is the file exactly as the game has it (it used to fall through
    // to the model converter here and come out as an FBX)
    if (format == "raw" && (e == "sb3d" || e == "m3g")) {
        out.assign(data, data + len);
        if (usedExtension) *usedExtension = e;
        return true;
    }
    if (e == "sb3d" || e == "m3g") {
        bool ok = convertSb3d(data, len, format, out, error, sidecar, sidecarLen);
        if (usedExtension) *usedExtension = (format == "obj") ? "obj" : "fbx";
        return ok;
    }
    if (e == "sba" && (format == "fbx" || format == "obj") && isNlScene(data, len)) {
        bool ok = convertSb3d(data, len, format, out, error, sidecar, sidecarLen);
        if (usedExtension) *usedExtension = format;
        return ok;
    }
    if (e == "sba" || e == "pvr" || e == "ktx" || e == "dds" || e == "png" || e == "jpg" || e == "jpeg") {
        std::string used;
        // the sku prefix in the asset path (e.g. "texture_etc/...") says which
        // block codec the pack was built with
        bool ok = convertSba(data, len, format, out, &used, error, assetPath);
        if (usedExtension) *usedExtension = used;
        return ok;
    }
    if (e == "sps" && format == "wav") {
        if (!spsToWav(data, len, out, error)) return false;
        if (usedExtension) *usedExtension = "wav";
        return true;
    }
    if (e == "gnsu" && format == "wav") {
        if (!gnsuToWav(data, len, out, error)) return false;
        if (usedExtension) *usedExtension = "wav";
        return true;
    }
    if (e == "wem" && format == "wav") {
        if (wemToWav(data, len, out, error)) {
            if (usedExtension) *usedExtension = "wav";
            return true;
        }
        // keep the sound rather than losing it: the raw .wem still plays in
        // vgmstream, and the error says why it was not converted here
        out.assign(data, data + len);
        if (usedExtension) *usedExtension = "wem";
        return true;
    }
    out.assign(data, data + len);
    if (usedExtension) *usedExtension = e.empty() ? "bin" : e;
    return true;
}

// ============================================================ NL wheels
//
// A No Limits car ships without wheels. The car model carries the brake parts
// - mesh_rotor_front_left_lod00 and so on, one disc per corner, already in
// place - and the wheel is its own model, models/cars/wheels/wheel_<car>.sb3d,
// that the game hangs on each corner at run time.
//
// Measured on the Jaguar XE SV and its wheel:
//  * the wheel is modelled centred on its own axle, spokes towards -x
//    (the carbon spoke face sits at x -0.122..-0.094 of a -0.125..0.126 rim),
//    so unchanged it is the LEFT side's wheel; the right side is the same
//    wheel turned half a turn about the vertical axis, not mirrored, so its
//    lettering still reads correctly;
//  * the axle is the disc's centre: the front-left rotor spans y -0.566..
//    -0.163 and z -1.622..-1.218, so the hub is at y -0.364, z -1.420, and a
//    0.360 tyre then reaches y -0.724 - the ground under that car;
//  * across the axle, the wheel's hub face (the innermost point of its
//    centre section) bolts against the outer face of the disc.
// No scale is applied: the wheel is modelled at the car's size.
namespace {

struct Corner { const char* tag; bool right; };
const Corner kNlCorners[4] = { { "FL", false }, { "FR", true }, { "RL", false }, { "RR", true } };

int lodNumber(const Mesh& m) {
    if (m.lod.size() == 5 && m.lod.compare(0, 3, "LOD") == 0) return atoi(m.lod.c_str() + 3);
    return 99;
}

bool isBlurOrShadow(const std::string& n) {
    return n.find("blur") != std::string::npos || n.find("shadow") != std::string::npos;
}

// A mesh's own material says best: NFS Edge hangs the solid rim and its
// motion-blur card under one group called ..._rim_notint_blur_alpha_lod01,
// and only the materials tell them apart.
bool isBlurOrShadowMesh(const Mesh& m) {
    if (!m.material.empty()) return isBlurOrShadow(m.material);
    return isBlurOrShadow(m.name);
}

} // namespace

std::string nlWheelNameFor(const std::string& carPath) {
    std::string stem = stripExtension(baseName(carPath));
    return "wheel_" + stem + ".sb3d";
}

bool nlVisualPartsMatches(const std::string& file, const std::string& carId) {
    auto words = [](const std::string& s) {
        std::vector<std::string> w;
        std::string cur;
        for (char c : s) {
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) cur += c;
            else if (!cur.empty()) { w.push_back(cur); cur.clear(); }
        }
        if (!cur.empty()) w.push_back(cur);
        return w;
    };
    std::vector<std::string> f = words(stripExtension(baseName(file))), c = words(carId);
    // C_ ... _1: the card prefix and its number are not part of the car
    if (!f.empty() && f.front() == "c") f.erase(f.begin());
    if (!f.empty() && f.back().size() <= 2 && f.back().find_first_not_of("0123456789") == std::string::npos)
        f.pop_back();
    // gt3_rs against gt3rs: the words run together
    std::string fj, cj;
    for (const std::string& w : f) fj += w;
    for (const std::string& w : c) cj += w;
    if (!fj.empty() && fj == cj) return true;
    std::sort(f.begin(), f.end());
    std::sort(c.begin(), c.end());
    return !f.empty() && f == c;
}

bool nlVisualPartsWheelTweak(const uint8_t* d, size_t n, NlWheelTweak& out) {
    out = NlWheelTweak();
    std::string t = sbinObjectsText(d, n);
    size_t p = t.find("FrontTireWidthOffset");
    if (p == std::string::npos) p = t.find("RearTireWidthOffset");
    if (p == std::string::npos) return false;
    // the block that holds it: the stock arch comes first
    size_t a = t.rfind("VisualPart = {", p);
    if (a == std::string::npos) a = p > 2000 ? p - 2000 : 0;
    size_t b = t.find('}', p);
    if (b == std::string::npos) b = t.size();
    std::string blk = t.substr(a, b - a);
    auto num = [&](const char* key, float& v) {
        std::string k = std::string(key) + " = ";
        size_t q = blk.find(k);
        if (q == std::string::npos) return;
        q += k.size();
        if (blk[q] == '[') {
            // one value per stance; the middle one is the standard stance
            std::vector<float> vals;
            const char* s = blk.c_str() + q + 1;
            while (*s && *s != ']') {
                char* e;
                float f = strtof(s, &e);
                if (e == s) { ++s; continue; }
                vals.push_back(f);
                s = e;
            }
            if (!vals.empty()) v = vals[vals.size() / 2];
        } else {
            char* e;
            float f = strtof(blk.c_str() + q, &e);
            if (e != blk.c_str() + q) v = f;
        }
    };
    num("FrontTireWidthOffset", out.widthOffset[0]);
    num("RearTireWidthOffset", out.widthOffset[1]);
    num("FrontWheelRadiusScale", out.radiusScale[0]);
    num("RearWheelRadiusScale", out.radiusScale[1]);
    num("FrontTireProfileOffset", out.profileOffset[0]);
    num("RearTireProfileOffset", out.profileOffset[1]);
    num("FrontWheelOffset", out.wheelOffset[0]);
    num("RearWheelOffset", out.wheelOffset[1]);
    for (int k = 0; k < 2; ++k) {
        if (!(out.widthOffset[k] > -0.2f && out.widthOffset[k] < 0.4f)) out.widthOffset[k] = 0;
        if (!(out.radiusScale[k] > 0.5f && out.radiusScale[k] < 2.0f)) out.radiusScale[k] = 1;
        if (!(out.profileOffset[k] > -0.1f && out.profileOffset[k] < 0.2f)) out.profileOffset[k] = 0;
        if (!(out.wheelOffset[k] > -0.2f && out.wheelOffset[k] < 0.2f)) out.wheelOffset[k] = 0;
    }
    out.valid = true;
    return true;
}


std::vector<NlRimColour> nlRimColours(const uint8_t* d, size_t n) {
    std::vector<NlRimColour> out;
    std::string t = sbinObjectsText(d, n);
    std::string group;
    bool rimGroup = false, inColour = false;
    NlRimColour cur;
    bool have = false;
    auto flush = [&]() {
        if (have && rimGroup) out.push_back(cur);
        have = false;
    };
    size_t p = 0;
    while (p < t.size()) {
        size_t e = t.find('\n', p);
        if (e == std::string::npos) e = t.size();
        std::string line = t.substr(p, e - p);
        p = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t a = line.find_first_not_of(' ');
        if (a == std::string::npos) continue;
        line.erase(0, a);
        auto val = [&](const char* key) -> std::string {
            size_t k = strlen(key);
            if (line.compare(0, k, key) != 0) return std::string("\x01");
            std::string v = line.substr(k);
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
            return v;
        };
        std::string v;
        if ((v = val("GroupId = ")) != "\x01") {
            flush();
            group = v;
            // CG_COMMON_RIM, CG_FERRARI_RIM (not CG_RIMAC_BODY)
            rimGroup = (group.size() >= 4 && group.compare(group.size() - 4, 4, "_RIM") == 0) ||
                       group.find("_RIM_") != std::string::npos;
            continue;
        }
        if (!rimGroup) continue;
        if ((v = val("Id = ")) != "\x01") {
            flush();
            cur = NlRimColour();
            cur.id = v;
            cur.group = group;
            // V_RC_Flat_Grey_Gloss -> Flat Grey Gloss
            std::string nm = v;
            if (nm.size() > 5 && nm[0] == 'V' && nm[1] == '_' && nm[4] == '_') nm.erase(0, 5);
            for (char& c : nm) if (c == '_') c = ' ';
            cur.name = nm;
            have = true;
            inColour = false;
            continue;
        }
        if (!have) continue;
        if (line == "Colour = {") { inColour = true; continue; }
        if (line.compare(0, 1, "}") == 0) { inColour = false; continue; }
        if (inColour) {
            if ((v = val("Red = ")) != "\x01") cur.rgb[0] = (float)atof(v.c_str());
            else if ((v = val("Green = ")) != "\x01") cur.rgb[1] = (float)atof(v.c_str());
            else if ((v = val("Blue = ")) != "\x01") cur.rgb[2] = (float)atof(v.c_str());
        }
    }
    flush();
    return out;
}

std::vector<NlPaintColour> nlPaintColours(const uint8_t* d, size_t n) {
    std::vector<NlPaintColour> out;
    std::string t = sbinObjectsText(d, n, 64u << 20);
    std::string group;
    int kind = -1;
    NlPaintColour cur;
    bool have = false;
    int block = 0;        // 1 inside Colour = { }, 2 inside another { } of the colour
    auto flush = [&]() {
        if (have && kind >= 0) out.push_back(cur);
        have = false;
    };
    size_t p = 0;
    while (p < t.size()) {
        size_t e = t.find('\n', p);
        if (e == std::string::npos) e = t.size();
        std::string line = t.substr(p, e - p);
        p = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t a = line.find_first_not_of(' ');
        if (a == std::string::npos) continue;
        line.erase(0, a);
        auto val = [&](const char* key, std::string& v) {
            size_t k = strlen(key);
            if (line.compare(0, k, key) != 0) return false;
            v = line.substr(k);
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
            return true;
        };
        std::string v;
        if (val("GroupId = ", v)) {
            flush();
            group = v;
            kind = -1;
            if (v.find("_BODY") != std::string::npos) kind = NL_BODY;
            else if (v.find("_RIM") != std::string::npos) kind = NL_RIM;
            else if (v.find("_BRAKE") != std::string::npos) kind = NL_BRAKE;
            else if (v.find("_WINDOW") != std::string::npos) kind = NL_WINDOW;
            continue;
        }
        if (kind < 0) continue;
        if (block == 0 && val("Id = ", v)) {
            flush();
            cur = NlPaintColour();
            cur.id = v;
            cur.group = group;
            cur.kind = kind;
            // V_BC_GT3_Orange_Gloss -> GT3 Orange Gloss
            std::string nm = v;
            if (nm.compare(0, 2, "V_") == 0) {
                size_t u = nm.find('_', 2);
                if (u != std::string::npos) nm.erase(0, u + 1);
            }
            for (char& c : nm) if (c == '_') c = ' ';
            cur.name = nm;
            have = true;
            continue;
        }
        if (!have) continue;
        if (line == "}") { block = 0; continue; }
        if (line.size() > 4 && line.compare(line.size() - 3, 3, "= {") == 0) {
            block = line == "Colour = {" ? 1 : 2;
            continue;
        }
        if (block == 1) {
            if (val("Red = ", v)) cur.rgb[0] = (float)atof(v.c_str());
            else if (val("Green = ", v)) cur.rgb[1] = (float)atof(v.c_str());
            else if (val("Blue = ", v)) cur.rgb[2] = (float)atof(v.c_str());
            continue;
        }
        if (block) continue;
        if (val("HashedID = ", v)) cur.hash = (uint32_t)strtoul(v.c_str(), nullptr, 10);
        else if (val("FinishName = ", v)) cur.finish = v;
        else if (val("Matte = ", v)) cur.matte = v == "true";
    }
    flush();
    return out;
}

NlCarPaint nlCarSetupPaint(const uint8_t* d, size_t n, const std::vector<NlPaintColour>& colours) {
    NlCarPaint out;
    std::map<uint32_t, const NlPaintColour*> byHash;
    for (const NlPaintColour& c : colours) if (c.hash) byHash[c.hash] = &c;
    std::string t = sbinObjectsText(d, n);
    size_t p = 0;
    while ((p = t.find("Value = ", p)) != std::string::npos) {
        p += 8;
        uint32_t h = (uint32_t)strtoul(t.c_str() + p, nullptr, 10);
        auto it = byHash.find(h);
        if (it == byHash.end()) continue;
        const NlPaintColour& c = *it->second;
        if (c.kind < 0 || c.kind >= NL_PAINT_KINDS || out.have[c.kind]) continue;
        out.have[c.kind] = true;
        memcpy(out.rgb[c.kind], c.rgb, sizeof(c.rgb));
        out.name[c.kind] = c.name;
    }
    return out;
}

std::string nlPaintRef(int mode, const float rgb[3], const std::string& base) {
    char b[32];
    auto to8 = [](float f) { return (unsigned)std::min(255.0f, std::max(0.0f, f * 255.0f + 0.5f)); };
    snprintf(b, sizeof(b), "#paint:%d:%02x%02x%02x:", mode, to8(rgb[0]), to8(rgb[1]), to8(rgb[2]));
    return b + nlUnpaintRef(base);
}

bool nlParsePaintRef(const std::string& ref, int& mode, float rgb[3], std::string& base) {
    if (ref.compare(0, 7, "#paint:") != 0 || ref.size() < 16 || ref[8] != ':' || ref[15] != ':') return false;
    mode = ref[7] - '0';
    unsigned v = (unsigned)strtoul(ref.substr(9, 6).c_str(), nullptr, 16);
    rgb[0] = ((v >> 16) & 255) / 255.0f;
    rgb[1] = ((v >> 8) & 255) / 255.0f;
    rgb[2] = (v & 255) / 255.0f;
    base = ref.substr(16);
    return mode == 1 || mode == 2;
}

std::string nlUnpaintRef(const std::string& ref) {
    std::string r = ref;
    while (r.compare(0, 7, "#paint:") == 0 && r.size() >= 16) r.erase(0, 16);
    return r;
}

void nlPaintImage(Image& img, int mode, const float rgb[3]) {
    if (mode == 2) {
        // the paint itself: a small solid picture (the game shades it)
        img = Image();
        img.width = img.height = 8;
        img.channels = 4;
        img.pixels.resize(8 * 8 * 4);
        for (size_t i = 0; i < 64; ++i) {
            for (int c = 0; c < 3; ++c)
                img.pixels[i * 4 + c] = (uint8_t)std::min(255.0f, std::max(0.0f, rgb[c] * 255.0f + 0.5f));
            img.pixels[i * 4 + 3] = 255;
        }
        return;
    }
    img = toRgba(img);
    nlTintImage(img, std::vector<uint8_t>(), rgb);
}

namespace {
std::string lowerOf(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}
}

int nlApplyCarPaint(Model& m, const NlCarPaint& paint) {
    int n = 0;
    for (Mesh& me : m.meshes) {
        std::string base = nlUnpaintRef(me.texture);
        me.texture = base;
        std::string leaf = lowerOf(baseName(base));
        std::string nm = lowerOf(me.name + " " + me.material);
        int kind = -1, mode = 1;
        if (leaf.compare(0, 13, "texture_paint") == 0 ||
            (base.empty() && lowerOf(me.material).compare(0, 5, "paint") == 0)) {
            kind = NL_BODY;
            mode = 2;
        } else if (leaf.compare(0, 21, "texture_brake_caliper") == 0 && leaf.find("alpha") == std::string::npos) {
            kind = NL_BRAKE;
        } else if (leaf.compare(0, 13, "texture_wheel") == 0 && leaf.find("tire") == std::string::npos &&
                   nm.find("notint") == std::string::npos && nm.find("tint") != std::string::npos) {
            kind = NL_RIM;
        }
        if (kind < 0 || !paint.have[kind]) continue;
        me.texture = nlPaintRef(mode, paint.rgb[kind], base);
        if (mode == 2) {
            // what an untextured view draws it in
            memcpy(me.color, paint.rgb[kind], sizeof(paint.rgb[kind]));
            me.color[3] = 1;
        }
        ++n;
    }
    return n;
}

void nlRemoveCarPaint(Model& m) {
    for (Mesh& me : m.meshes) {
        if (me.texture.compare(0, 7, "#paint:") != 0) continue;
        int mode = 0;
        float rgb[3];
        std::string base;
        nlParsePaintRef(me.texture, mode, rgb, base);
        me.texture = base;
        if (mode == 2) { me.color[0] = me.color[1] = me.color[2] = me.color[3] = 1; }
    }
}

bool nlRimTintMask(const Model& wheel, const std::string& textureLeaf, int w, int h,
                   std::vector<uint8_t>& mask) {
    mask.assign((size_t)w * h, 0);
    if (w <= 0 || h <= 0) return false;
    auto lower = [](std::string x) {
        for (char& c : x) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return x;
    };
    std::string want = lower(stripExtension(baseName(textureLeaf)));
    // tinted first, then the untinted parts cut back out of it
    int painted = 0;
    for (int pass = 0; pass < 2; ++pass) {
        for (const Mesh& m : wheel.meshes) {
            std::string nm = lower(m.name);
            bool notint = nm.find("notint") != std::string::npos;
            bool tint = !notint && nm.find("tint") != std::string::npos;
            if (pass == 0 ? !tint : !notint) continue;
            if (lower(stripExtension(baseName(m.texture))) != want) continue;
            size_t nv = m.positions.size() / 3;
            if (m.uvs.size() != nv * 2) continue;
            uint8_t value = pass == 0 ? 255 : 0;
            for (size_t k = 0; k + 2 < m.indices.size(); k += 3) {
                float px[3], py[3];
                bool ok = true;
                for (int c = 0; c < 3; ++c) {
                    uint32_t vi = m.indices[k + c];
                    if (vi >= nv) { ok = false; break; }
                    float u = m.uvs[vi * 2], vv = m.uvs[vi * 2 + 1];
                    u -= std::floor(u);
                    vv -= std::floor(vv);
                    // the renderer's convention: row = (1 - v) * height
                    px[c] = u * w;
                    py[c] = (1.0f - vv) * h;
                }
                if (!ok) continue;
                int x0 = std::max(0, (int)std::floor(std::min({px[0], px[1], px[2]})) - 1);
                int x1 = std::min(w - 1, (int)std::ceil(std::max({px[0], px[1], px[2]})) + 1);
                int y0 = std::max(0, (int)std::floor(std::min({py[0], py[1], py[2]})) - 1);
                int y1 = std::min(h - 1, (int)std::ceil(std::max({py[0], py[1], py[2]})) + 1);
                if ((x1 - x0) * (long long)(y1 - y0) > (long long)w * h) continue;
                float area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
                if (std::fabs(area) < 1e-6f) continue;
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x) {
                        float cx = x + 0.5f, cy = y + 0.5f;
                        float w0 = ((px[1] - cx) * (py[2] - cy) - (px[2] - cx) * (py[1] - cy)) / area;
                        float w1 = ((px[2] - cx) * (py[0] - cy) - (px[0] - cx) * (py[2] - cy)) / area;
                        float w2 = 1 - w0 - w1;
                        // a little over the edge, so no seam is left unpainted
                        const float tol = pass == 0 ? 0.02f : 0.0f;
                        if (w0 >= -tol && w1 >= -tol && w2 >= -tol) {
                            mask[(size_t)y * w + x] = value;
                            if (pass == 0) ++painted;
                        }
                    }
            }
        }
    }
    return painted > 0;
}

void nlTintImage(Image& img, const std::vector<uint8_t>& mask, const float rgb[3]) {
    if (!img.ok() || img.channels < 3) return;
    size_t n = (size_t)img.width * img.height;
    bool useMask = mask.size() == n;
    for (size_t i = 0; i < n; ++i) {
        uint8_t* p = &img.pixels[i * img.channels];
        if (useMask) {
            if (!mask[i]) continue;
        } else {
            int mx = std::max({p[0], p[1], p[2]}), mn = std::min({p[0], p[1], p[2]});
            if (mx - mn > 40) continue;           // a coloured logo: left alone
        }
        for (int c = 0; c < 3; ++c) p[c] = (uint8_t)std::min(255.0f, p[c] * rgb[c] + 0.5f);
    }
}

int nlAttachWheels(Model& car, const Model& wheel, const float* axleRadius, const NlWheelTweak* tweak) {
    if (!wheel.valid || wheel.meshes.empty()) return 0;

    // the wheel: its finest LOD, blur cards left out
    int bestLod = 99;
    for (const Mesh& m : wheel.meshes)
        if (!isBlurOrShadowMesh(m)) bestLod = std::min(bestLod, lodNumber(m));
    // every LOD goes on (each shows with the car's own LOD of the same
    // number); the finest one is what the measurements below are taken from
    std::vector<const Mesh*> parts, finest;
    for (const Mesh& m : wheel.meshes)
        if (!isBlurOrShadowMesh(m)) parts.push_back(&m);
    float radius = 0, xLo = 1e30f, xHi = -1e30f;
    for (const Mesh& m : wheel.meshes) {
        if (isBlurOrShadowMesh(m) || lodNumber(m) != bestLod) continue;
        finest.push_back(&m);
        for (size_t i = 0; i + 2 < m.positions.size(); i += 3) {
            radius = std::max(radius, std::sqrt(m.positions[i + 1] * m.positions[i + 1] +
                                                m.positions[i + 2] * m.positions[i + 2]));
            xLo = std::min(xLo, m.positions[i]);
            xHi = std::max(xHi, m.positions[i]);
        }
    }
    if (parts.empty() || radius <= 1e-4f) return 0;
    // the hub face: the innermost x among the vertices near the axle (within
    // a quarter of the radius), which is the back of the centre section
    float mountX = -1e30f;
    for (const Mesh* m : finest)
        for (size_t i = 0; i + 2 < m->positions.size(); i += 3) {
            float r = std::sqrt(m->positions[i + 1] * m->positions[i + 1] +
                                m->positions[i + 2] * m->positions[i + 2]);
            if (r < radius * 0.25f) mountX = std::max(mountX, m->positions[i]);
        }
    if (mountX < -1e29f) mountX = (xLo + xHi) * 0.5f;
    // the tyre (material tire / tyre) across and the rim's radius: a wider
    // tyre is the model pulled apart at the middle of the tread, both sides
    // moving out by half the extra width (the rim's barrel stretches with it)
    auto isTyre = [](const Mesh& m) {
        std::string s = m.material + " " + m.name;
        for (char& ch : s) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        return s.find("tire") != std::string::npos || s.find("tyre") != std::string::npos;
    };
    float tLo = 1e30f, tHi = -1e30f, rimR = 0;
    for (const Mesh* m : finest)
        for (size_t i = 0; i + 2 < m->positions.size(); i += 3) {
            if (isTyre(*m)) {
                tLo = std::min(tLo, m->positions[i]);
                tHi = std::max(tHi, m->positions[i]);
            } else {
                rimR = std::max(rimR, std::sqrt(m->positions[i + 1] * m->positions[i + 1] +
                                                m->positions[i + 2] * m->positions[i + 2]));
            }
        }
    float tyreMid = tLo < tHi ? (tLo + tHi) * 0.5f : (xLo + xHi) * 0.5f;
    if (rimR <= 0 || rimR >= radius * 0.98f) rimR = radius * 0.7f;

    // ---- the four discs, found by where they are rather than what they are
    // called. The names cannot be trusted: the Nissan Z calls all four of its
    // discs mesh_rotor_front_left_a, the Cayenne's "front_left" disc sits on
    // the right, and the Evora's names matched one corner and put its wheel
    // in the middle of the door. So every vertex of every rotor mesh (finest
    // LOD) is sorted into a corner by its own side and axle.
    auto isDisc = [](const std::string& n) {
        return (n.find("rotor") != std::string::npos || n.find("brake_disc") != std::string::npos) &&
               n.find("logo") == std::string::npos && !isBlurOrShadow(n);
    };
    int discLod = 99;
    for (const Mesh& m : car.meshes) if (isDisc(m.name)) discLod = std::min(discLod, lodNumber(m));
    float zLo = 1e30f, zHi = -1e30f;
    for (const Mesh& m : car.meshes) {
        if (!isDisc(m.name) || lodNumber(m) != discLod) continue;
        for (size_t i = 2; i < m.positions.size(); i += 3) {
            zLo = std::min(zLo, m.positions[i]);
            zHi = std::max(zHi, m.positions[i]);
        }
    }
    // two axles when the discs spread along the car further than a disc is
    // wide; otherwise only one axle has discs and it is all one row
    bool twoAxles = zHi - zLo > 1.0f;
    float zSplit = (zLo + zHi) * 0.5f;
    struct Box { float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}; size_t n = 0; };
    Box box[4];                     // 0 FL, 1 FR, 2 RL, 3 RR (front is -z)
    for (const Mesh& m : car.meshes) {
        if (!isDisc(m.name) || lodNumber(m) != discLod) continue;
        for (size_t i = 0; i + 2 < m.positions.size(); i += 3) {
            const float* v = &m.positions[i];
            int q = (v[0] > 0 ? 1 : 0) + ((twoAxles ? v[2] > zSplit : zLo > 0) ? 2 : 0);
            for (int k = 0; k < 3; ++k) {
                box[q].lo[k] = std::min(box[q].lo[k], v[k]);
                box[q].hi[k] = std::max(box[q].hi[k], v[k]);
            }
            ++box[q].n;
        }
    }

    // the body as the car ships (stock kit, finest LOD): used below to check
    // the wheel against its arch
    std::vector<uint8_t> stock = kitMask(car, "stock");
    int bodyLod = 99;
    for (const Mesh& m : car.meshes) bodyLod = std::min(bodyLod, lodNumber(m));
    auto isBodySide = [](const std::string& n) {
        static const char* skip[] = {"rotor", "caliper", "brake", "blur", "shadow", "mirror",
                                     "wheel_rim", "tire", "tyre", "interior", "driver"};
        for (const char* k : skip) if (n.find(k) != std::string::npos) return false;
        return true;
    };

    // The rig's wheel joints (J_wheel_front_left ...) are where the game
    // itself hangs the wheels: when the car has them they win over anything
    // measured. Cop cars have no brake discs at all, and on some cars (the
    // One-77) the discs sit well inside the wheel.
    static const char* const kJoint[4] = { "J_wheel_front_left", "J_wheel_front_right",
                                           "J_wheel_rear_left", "J_wheel_rear_right" };
    const Hardpoint* joint[4] = { nullptr, nullptr, nullptr, nullptr };
    int joints = 0;
    for (int q = 0; q < 4; ++q)
        for (const Hardpoint& h : car.points)
            if (h.name == kJoint[q]) { joint[q] = &h; ++joints; break; }
    if (joints < 4) for (auto& j : joint) j = nullptr;

    int placed = 0, pushedOut = 0, onJoints = 0;
    float moved[4][2] = { { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 } };   // hub moved off its joint (y, z)
    std::vector<Mesh> added;
    for (int q = 0; q < 4; ++q) {
        const Corner& c = kNlCorners[q];
        const Box& b = box[q];
        float hubY, hubZ, offX;
        if (joint[q]) {
            // the wheel model's origin on the joint, the right-hand ones
            // turned half a turn as below
            hubY = joint[q]->pos[1];
            hubZ = joint[q]->pos[2];
            offX = joint[q]->pos[0];
            ++onJoints;
            // The joint is where the game hangs the wheel: with the prefab's
            // tyre radius every tyre touches the same ground (the joint sits
            // one radius above it on every car measured). The visual locators
            // (locator_visual_wheel_*) do not: taking them lifted one axle and
            // left the car standing on two wheels.
            // Across the car the joint is the hub, but the one wheel model
            // serves both axles: a car with much wider rear tyres (the 911
            // GT3 RS, 335 against 275) has its rear wheel sunk inside the
            // wing. The tyre's face is brought to about 1.5 cm short of the
            // body side over the wheel, by at most 8 cm, never inwards.
            {
                int ax = q < 2 ? 0 : 1;
                float sc = 1.0f;
                if (axleRadius && radius > 1e-3f && axleRadius[ax] > 0.1f && axleRadius[ax] < 1.5f) sc = axleRadius[ax] / radius;
                float extra = 0;
                if (tweak && tweak->valid) { sc *= tweak->radiusScale[ax]; extra = tweak->widthOffset[ax] + tweak->wheelOffset[ax]; }
                float tyreOut = std::fabs(offX) - xLo * sc + extra;
                float body = 0;
                for (size_t mi = 0; mi < car.meshes.size(); ++mi) {
                    const Mesh& m = car.meshes[mi];
                    if (lodNumber(m) != bodyLod || (mi < stock.size() && !stock[mi]) || !isBodySide(m.name)) continue;
                    for (size_t i = 0; i + 2 < m.positions.size(); i += 3) {
                        const float* v = &m.positions[i];
                        if ((v[0] > 0) != c.right) continue;
                        if (v[1] < hubY || v[1] > hubY + radius * sc * 1.1f) continue;
                        if (std::fabs(v[2] - hubZ) > radius * sc * 0.6f) continue;
                        body = std::max(body, std::fabs(v[0]));
                    }
                }
                float gap = body - tyreOut;
                if (getenv("MT_WHEELDBG")) fprintf(stderr, "corner %d tyre %.3f body %.3f\n", q, tyreOut, body);
                if (body > 0 && gap > 0.03f && gap < 0.15f) {
                    float shift = std::min(gap - 0.015f, 0.08f);
                    offX += c.right ? shift : -shift;
                    ++pushedOut;
                }
            }
        } else {
        if (b.n < 8) continue;
        hubY = (b.lo[1] + b.hi[1]) * 0.5f; hubZ = (b.lo[2] + b.hi[2]) * 0.5f;
        float outer = c.right ? b.hi[0] : b.lo[0];   // the disc's outer face
        // left: local x maps as is, the hub face (mountX) onto the disc face
        // right: turned half a turn, x -> -x and z -> -z
        offX = c.right ? outer + mountX : outer - mountX;

        // The disc gives the hub, but on some cars (Bugatti EB110) the discs
        // sit deep inside and the wheel ends up well inside its arch. The
        // tyre's outer face should come about level with the body side over
        // the wheel: the widest point of the stock body in the upper half of
        // the wheel, over its middle. When the tyre is more than 3 cm short
        // of it, it is moved out to 1.5 cm short; never moved in.
        {
            float tyreOut = c.right ? offX - xLo : -(xLo + offX);   // |x| of the outer face
            if (tweak && tweak->valid) tyreOut += tweak->widthOffset[q < 2 ? 0 : 1] + tweak->wheelOffset[q < 2 ? 0 : 1];
            float body = 0;
            for (size_t mi = 0; mi < car.meshes.size(); ++mi) {
                const Mesh& m = car.meshes[mi];
                if (lodNumber(m) != bodyLod || (mi < stock.size() && !stock[mi]) ||
                    !isBodySide(m.name))
                    continue;
                for (size_t i = 0; i + 2 < m.positions.size(); i += 3) {
                    const float* v = &m.positions[i];
                    if ((v[0] > 0) != c.right) continue;
                    if (v[1] < hubY || v[1] > hubY + radius * 1.1f) continue;
                    if (std::fabs(v[2] - hubZ) > radius * 0.6f) continue;
                    body = std::max(body, std::fabs(v[0]));
                }
            }
            float gap = body - tyreOut;
            if (body > 0 && gap > 0.03f && gap < 0.35f) {
                float shift = gap - 0.015f;
                offX += c.right ? shift : -shift;
                ++pushedOut;
            }
        }
        }

        for (const Mesh* src : parts) {
            Mesh m = *src;
            m.name = std::string("wheel_") + c.tag + "_" + src->name;
            m.part = std::string("wheel_") + c.tag;
            if (m.lod.empty()) m.lod = "LOD00";
            // the car's prefab gives each axle's tyre radius (the Elise runs
            // 0.30 m front, 0.32 m rear on a wheel model 0.36 m across): the
            // game scales the one wheel model to it
            float sc = 1.0f;
            if (axleRadius && joint[q] && radius > 1e-3f) {
                float want = axleRadius[q < 2 ? 0 : 1];
                if (want > 0.1f && want < 1.5f) sc = want / radius;
            }
            int axle = q < 2 ? 0 : 1;
            // TireWidthOffset: each side of the tyre moves out by it (the
            // F132's rear tyre gains 2 x 11 cm, as wide as in the game), the
            // rim stays as it is and sits deeper in the tyre - the deep dish
            // the game shows
            float half = 0, prof = 0, track = 0;
            if (tweak && tweak->valid) {
                sc *= tweak->radiusScale[axle];
                half = tweak->widthOffset[axle];
                prof = tweak->profileOffset[axle];
                track = tweak->wheelOffset[axle];
            }
            bool tyre = isTyre(*src);
            for (size_t i = 0; i + 2 < m.positions.size(); i += 3) {
                float lx = m.positions[i], ly = m.positions[i + 1], lz = m.positions[i + 2];
                if (prof != 0 && tyre) {
                    float r = std::sqrt(ly * ly + lz * lz);
                    if (r > rimR && radius > rimR) {
                        float k = (r + prof / sc * (r - rimR) / (radius - rimR)) / r;
                        ly *= k; lz *= k;
                    }
                }
                float x = lx * sc, z = lz * sc;
                // the extra tyre width, in metres, split about the tread's middle
                if (half != 0 && tyre) x += (lx < tyreMid ? -half : half);
                x -= track;                 // local -x is outward
                if (c.right) { x = -x; z = -z; }
                m.positions[i] = x + offX;
                m.positions[i + 1] = ly * sc + hubY;
                m.positions[i + 2] = z + hubZ;
            }
            if (c.right)
                for (size_t i = 0; i + 2 < m.normals.size(); i += 3) {
                    m.normals[i] = -m.normals[i];
                    m.normals[i + 2] = -m.normals[i + 2];
                }
            for (int k = 0; k < 3; ++k) { m.bboxMin[k] = 1e30f; m.bboxMax[k] = -1e30f; }
            for (size_t i = 0; i + 2 < m.positions.size(); i += 3)
                for (int k = 0; k < 3; ++k) {
                    m.bboxMin[k] = std::min(m.bboxMin[k], m.positions[i + k]);
                    m.bboxMax[k] = std::max(m.bboxMax[k], m.positions[i + k]);
                }
            added.push_back(std::move(m));
        }
        ++placed;
    }
    // the brakes go where their wheel went
    for (Mesh& m : car.meshes) {
        std::string n = m.name;
        for (char& ch : n) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        if (!(isDisc(n) || n.find("caliper") != std::string::npos) || m.positions.empty()) continue;
        float cx = 0, cz = 0;
        size_t nv = m.positions.size() / 3;
        for (size_t v = 0; v < nv; ++v) { cx += m.positions[v * 3]; cz += m.positions[v * 3 + 2]; }
        cx /= nv; cz /= nv;
        int q = (cx > 0 ? 1 : 0) + ((twoAxles ? cz > zSplit : zLo > 0) ? 2 : 0);
        if (moved[q][0] == 0 && moved[q][1] == 0) continue;
        for (size_t v = 0; v < nv; ++v) { m.positions[v * 3 + 1] += moved[q][0]; m.positions[v * 3 + 2] += moved[q][1]; }
        for (int k = 0; k < 2; ++k) { m.bboxMin[k + 1] += moved[q][k]; m.bboxMax[k + 1] += moved[q][k]; }
    }
    for (Mesh& m : added) car.meshes.push_back(std::move(m));
    // the materials the wheel meshes name come with them
    for (const Material& mat : wheel.materials) {
        bool have = false;
        for (const Material& x : car.materials) if (x.name == mat.name) { have = true; break; }
        if (!have) car.materials.push_back(mat);
    }
    if (placed) {
        char buf[128];
        if (onJoints == placed)
            snprintf(buf, sizeof(buf), "%d wheel(s) hung on the car's wheel joints (J_wheel_*)", placed);
        else
            snprintf(buf, sizeof(buf), "%d wheel(s) put on the brake discs from the car's wheel model",
                     placed);
        car.warnings.push_back(buf);
        if (pushedOut) {
            snprintf(buf, sizeof(buf), "%d wheel(s) moved out to line up with the body side", pushedOut);
            car.warnings.push_back(buf);
        }
    } else {
        car.warnings.push_back("wheel model found, but the car has neither wheel joints nor "
                               "mesh_rotor_* discs to put it on");
    }
    return placed;
}

// The tyre radius of each axle, from a No Limits car's prefab
// (prefabs/cars/<car>.prefabs.sb, decoded): RaycastAxle {Index, WheelRadius,
// ChassisClearance, WheelBaseRatio, Wheels}. radius[0] front, [1] rear.
bool nlPrefabWheelRadii(const uint8_t* d, size_t n, float radius[2]) {
    radius[0] = radius[1] = 0;
    auto chunks = sbinChunks(d, n);
    const SbinChunk* stru = findChunk(chunks, "STRU");
    const SbinChunk* fiel = findChunk(chunks, "FIEL");
    const SbinChunk* ohdr = findChunk(chunks, "OHDR");
    const SbinChunk* data = findChunk(chunks, "DATA");
    if (!stru || !fiel || !ohdr || !data) return false;
    std::vector<std::string> names = sbinNames(chunks);
    int sid = -1, offIndex = -1, offRadius = -1;
    for (size_t k = 0; (k + 1) * 6 <= stru->size; ++k) {
        uint16_t nm, first, count;
        memcpy(&nm, stru->data + k * 6, 2);
        memcpy(&first, stru->data + k * 6 + 2, 2);
        memcpy(&count, stru->data + k * 6 + 4, 2);
        if (nm >= names.size() || names[nm] != "RaycastAxle") continue;
        sid = (int)k;
        for (size_t f = first; f < (size_t)first + count && (f + 1) * 8 <= fiel->size; ++f) {
            uint16_t fn, fo;
            memcpy(&fn, fiel->data + f * 8, 2);
            memcpy(&fo, fiel->data + f * 8 + 4, 2);
            if (fn < names.size() && names[fn] == "Index") offIndex = fo;
            if (fn < names.size() && names[fn] == "WheelRadius") offRadius = fo;
        }
    }
    if (sid < 0 || offRadius < 0) return false;
    int got = 0;
    for (size_t i = 0; (i + 1) * 4 <= ohdr->size; ++i) {
        uint32_t e;
        memcpy(&e, ohdr->data + i * 4, 4);
        if ((e & 7) != 0) continue;
        size_t off = e >> 3;
        if (off + 2 + 16 > data->size) continue;
        uint16_t s16;
        memcpy(&s16, data->data + off, 2);
        if (s16 != sid) continue;
        int32_t idx = 0;
        float r = 0;
        if (offIndex >= 0) memcpy(&idx, data->data + off + 2 + offIndex, 4);
        memcpy(&r, data->data + off + 2 + offRadius, 4);
        if (idx >= 0 && idx < 2 && std::isfinite(r) && r > 0) { radius[idx] = r; ++got; }
    }
    if (radius[0] <= 0) radius[0] = radius[1];
    if (radius[1] <= 0) radius[1] = radius[0];
    return got > 0;
}

// ---- models a No Limits prefab hangs on the car ----
//
// A car's prefab (prefabs/cars/<car>.prefabs.sb) is a tree of actors: Asset
// {children, data -> actor {name, components}}. A TransformComponent places
// an actor inside its parent (LocalTranslation, a Vector3D held inline); an
// NFSModel component names a model file (AnimatedM3GFile {Filepath}). The
// cop cars' light bars are such a model: /published/models/fx/cop_lights/
// cop_lights_01.m3g, 1.5 m up the chassis joint.
std::vector<PrefabModel> nlPrefabModels(const uint8_t* d, size_t n) {
    std::vector<PrefabModel> out;
    auto chunks = sbinChunks(d, n);
    const SbinChunk* stru = findChunk(chunks, "STRU");
    const SbinChunk* fiel = findChunk(chunks, "FIEL");
    const SbinChunk* ohdr = findChunk(chunks, "OHDR");
    const SbinChunk* data = findChunk(chunks, "DATA");
    if (!stru || !fiel || !ohdr || !data) return out;
    std::vector<std::string> names = sbinNames(chunks);
    const uint8_t* D = data->data;
    size_t DL = data->size;
    auto nm = [&](uint32_t i) { return i < names.size() ? names[i] : std::string(); };
    auto u16 = [&](size_t o) -> uint32_t { if (o + 2 > DL) return 0; uint16_t v; memcpy(&v, D + o, 2); return v; };
    auto u32 = [&](size_t o) -> uint32_t { if (o + 4 > DL) return 0xFFFFFFFFu; uint32_t v; memcpy(&v, D + o, 4); return v; };
    auto f32 = [&](size_t o) -> float { if (o + 4 > DL) return 0; float v; memcpy(&v, D + o, 4); return v; };
    size_t nObj = ohdr->size / 4, nStru = stru->size / 6, nFiel = fiel->size / 8;
    auto object = [&](uint32_t i, uint32_t& kind, size_t& off) {
        if (i >= nObj) return false;
        uint32_t e; memcpy(&e, ohdr->data + (size_t)i * 4, 4);
        kind = e & 7; off = e >> 3;
        return off < DL;
    };
    auto structName = [&](size_t sid) {
        if (sid >= nStru) return std::string();
        uint16_t v; memcpy(&v, stru->data + sid * 6, 2);
        return nm(v);
    };
    // a structure's field by name: type, offset, sub-structure
    struct F { int type = -1, off = 0, sub = 0; };
    auto field = [&](size_t sid, const char* want) {
        F f;
        if (sid >= nStru) return f;
        uint16_t first, count;
        memcpy(&first, stru->data + sid * 6 + 2, 2);
        memcpy(&count, stru->data + sid * 6 + 4, 2);
        for (size_t k = first; k < (size_t)first + count && k < nFiel; ++k) {
            uint16_t fn, ft, fo, fs;
            memcpy(&fn, fiel->data + k * 8, 2);
            memcpy(&ft, fiel->data + k * 8 + 2, 2);
            memcpy(&fo, fiel->data + k * 8 + 4, 2);
            memcpy(&fs, fiel->data + k * 8 + 6, 2);
            if (nm(fn) == want) { f.type = ft; f.off = fo; f.sub = fs; return f; }
        }
        return f;
    };
    // a typed object: its structure and where its fields start
    auto typed = [&](uint32_t i, size_t& sid, size_t& base) {
        uint32_t k; size_t off;
        if (!object(i, k, off) || k != 0) return false;
        sid = u16(off);
        base = off + 2;
        return sid < nStru;
    };
    // a map object's value for a key: type and address
    auto mapGet = [&](uint32_t i, const char* key, int& type, size_t& at) {
        uint32_t k; size_t off;
        if (!object(i, k, off) || k != 1) return false;
        uint32_t cnt = u16(off);
        for (uint32_t e = 0; e < cnt && e < 4096; ++e) {
            size_t q = off + 4 + (size_t)e * 8;
            if (nm(u16(q)) == key) { type = (int)u16(q + 2); at = off + u16(q + 4); return true; }
        }
        return false;
    };
    // an array of object references
    auto refs = [&](uint32_t i) {
        std::vector<uint32_t> r;
        uint32_t k; size_t off;
        if (!object(i, k, off) || k != 2) return r;
        uint32_t et = u32(off), cnt = u32(off + 4);
        if (et != 0x0f || cnt > 100000) return r;
        for (uint32_t e = 0; e < cnt; ++e) r.push_back(u32(off + 8 + (size_t)e * 4));
        return r;
    };
    std::function<void(uint32_t, const float*, int)> visit = [&](uint32_t asset, const float* parent, int depth) {
        if (depth > 64) return;
        size_t sid, base;
        if (!typed(asset, sid, base)) return;
        float pos[3] = { parent[0], parent[1], parent[2] };
        F fData = field(sid, "data");
        std::string file;
        if (fData.type == 15) {
            uint32_t actor = u32(base + fData.off);
            size_t asid, abase;
            if (typed(actor, asid, abase)) {
                F fc = field(asid, "components");
                if (fc.type == 17)
                    for (uint32_t c : refs(u32(abase + fc.off))) {
                        size_t csid, cbase;
                        if (!typed(c, csid, cbase)) continue;
                        std::string cn = structName(csid);
                        if (cn == "TransformComponent") {
                            F t = field(csid, "LocalTranslation");
                            if (t.type == 16) {
                                const char* ax[3] = { "x", "y", "z" };
                                for (int a = 0; a < 3; ++a) {
                                    F fa = field((size_t)t.sub, ax[a]);
                                    if (fa.type == 10) pos[a] += f32(cbase + t.off + fa.off);
                                }
                            }
                        } else if (cn == "NFSModel") {
                            F fm = field(csid, "AnimatedM3GFile");
                            if (fm.type == 15) {
                                int ty; size_t at;
                                if (mapGet(u32(cbase + fm.off), "Filepath", ty, at))
                                    file = nm(ty == 0x15 || ty == 0x0d ? u16(at) : u32(at));
                            }
                        }
                    }
            }
        }
        if (!file.empty()) {
            PrefabModel pm;
            pm.file = file;
            for (int a = 0; a < 3; ++a) pm.pos[a] = pos[a];
            out.push_back(pm);
        }
        F fChildren = field(sid, "children");
        if (fChildren.type == 17)
            for (uint32_t c : refs(u32(base + fChildren.off))) visit(c, pos, depth + 1);
    };
    int ty; size_t at;
    if (!mapGet(0, "asset", ty, at) || ty != 0x0f) return out;
    float zero[3] = { 0, 0, 0 };
    visit(u32(at), zero, 0);
    return out;
}

} // namespace nfsnl
