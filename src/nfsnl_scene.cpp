// nfsnl_scene.cpp - No Limits' track geometry: prefabs/tracks/*.scene_static.sba
//
// The open world's roads, buildings and props are not .m3g files. Each region
// is one SBIN (version 4) file whose own structure table describes it:
//
//   Model           instances[] (Instance), lod_groups[], pvs_areas[]
//   Instance        transform (Matrix4: rows x, y, z, translation), primitive,
//                   material_instance, name ("Mesh/region03_env/road_00/
//                   MergeGroup-asphalt_road_wssuv-fx/MergeGroup_31/mesh"),
//                   lod_group
//   Primitive       type (Lines, Triangles), vertex_buffer, index_buffer,
//                   index_offset, index_count, bounds
//   VertexBuffer    count, vertex_declaration, scale_bias_entries[], data
//   VertexDeclaration  streams[] (VertexStream), stride
//   VertexStream    usage (Position, ..., Normal = 4, ..., TexCoord = 8,
//                   Color = 9), index, element_type (S8 U8 S16 U16 S32 F32
//                   U32), element_count, scale_bias_index, offset
//   IndexBuffer     vertex_offset, count, data
//
// "data" names an entry of BULK, which points into BARG: 16 bytes of header
// (flag 0 stored, 2 zstd-compressed) and the buffer. Quantised components are
// value * scale + bias with the ScaleBiasEntry the stream names.
//
// Every field is found by its name in the file's own tables, not by a fixed
// offset, so a region built by a later version of the tools still reads.
#include "nfsnl.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <functional>

namespace nfsnl {

namespace {

struct Scene {
    const SbinChunk* stru = nullptr;
    const SbinChunk* fiel = nullptr;
    const SbinChunk* ohdr = nullptr;
    const SbinChunk* data = nullptr;
    const SbinChunk* bulk = nullptr;
    const SbinChunk* barg = nullptr;
    std::vector<std::string> names;

    uint32_t u32(size_t o) const {
        if (!data || o + 4 > data->size) return 0xFFFFFFFFu;
        uint32_t v; memcpy(&v, data->data + o, 4); return v;
    }
    uint16_t u16(size_t o) const {
        if (!data || o + 2 > data->size) return 0;
        uint16_t v; memcpy(&v, data->data + o, 2); return v;
    }
    float f32(size_t o) const {
        if (!data || o + 4 > data->size) return 0;
        float v; memcpy(&v, data->data + o, 4); return v;
    }
    std::string name(uint32_t i) const { return i < names.size() ? names[i] : std::string(); }
    // a map object's value for a key name (an object reference), or ~0
    uint32_t mapValue(uint32_t i, const char* key) const {
        uint32_t k; size_t off;
        if (!object(i, k, off) || k != 1) return 0xFFFFFFFFu;
        uint16_t n = u16(off);
        for (uint16_t e = 0; e < n && e < 4096; ++e) {
            size_t at = off + 4 + (size_t)e * 12;
            if (name(u16(at)) == key) return u32(off + u16(at + 4));
        }
        return 0xFFFFFFFFu;
    }
    size_t objects() const { return ohdr ? ohdr->size / 4 : 0; }
    // an object's kind (0 typed, 1 map, 2 array) and where it starts in DATA
    bool object(uint32_t i, uint32_t& kind, size_t& off) const {
        if (i >= objects()) return false;
        uint32_t e; memcpy(&e, ohdr->data + (size_t)i * 4, 4);
        kind = e & 7;
        off = e >> 3;
        return data && off < data->size;
    }
    // a typed object's structure name and the start of its fields
    bool typed(uint32_t i, std::string& structName, size_t& fields, uint16_t& sid) const {
        uint32_t k; size_t off;
        if (!object(i, k, off) || k != 0 || !stru) return false;
        sid = u16(off);
        if ((size_t)sid * 6 + 6 > stru->size) return false;
        uint16_t n; memcpy(&n, stru->data + (size_t)sid * 6, 2);
        structName = name(n);
        fields = off + 2;
        return true;
    }
    // the offset of a named field in structure sid, or -1
    int field(uint16_t sid, const char* want) const {
        if (!stru || !fiel || (size_t)sid * 6 + 6 > stru->size) return -1;
        uint16_t first, count;
        memcpy(&first, stru->data + (size_t)sid * 6 + 2, 2);
        memcpy(&count, stru->data + (size_t)sid * 6 + 4, 2);
        for (size_t f = first; f < (size_t)first + count && (f + 1) * 8 <= fiel->size; ++f) {
            uint16_t fn, fo;
            memcpy(&fn, fiel->data + f * 8, 2);
            memcpy(&fo, fiel->data + f * 8 + 4, 2);
            if (name(fn) == want) return fo;
        }
        return -1;
    }
    // an array object's element references
    std::vector<uint32_t> refs(uint32_t i) const {
        std::vector<uint32_t> out;
        uint32_t k; size_t off;
        if (!object(i, k, off) || k != 2) return out;
        uint32_t et = u32(off), n = u32(off + 4);
        if (et != 0x0f || n > 10000000 || off + 8 + (size_t)n * 4 > data->size) return out;
        out.reserve(n);
        for (uint32_t j = 0; j < n; ++j) out.push_back(u32(off + 8 + (size_t)j * 4));
        return out;
    }
    // a BULK entry's bytes, unpacked
    bool bulkData(uint32_t index, Bytes& out) const {
        out.clear();
        if (!bulk || !barg || (size_t)index * 8 + 8 > bulk->size) return false;
        uint32_t off, sz;
        memcpy(&off, bulk->data + (size_t)index * 8, 4);
        memcpy(&sz, bulk->data + (size_t)index * 8 + 4, 4);
        if ((size_t)off + sz > barg->size || sz < 16) return false;
        int32_t flag;
        memcpy(&flag, barg->data + off, 4);
        const uint8_t* p = barg->data + off + 16;
        size_t n = sz - 16;
        if (flag == 2) return zstdAvailable() && zstdDecompress(p, n, 0, out) && !out.empty();
        if (flag == 1) {
            uint32_t usize;
            memcpy(&usize, barg->data + off + 4, 4);
            return lz4DecompressBlock(p, n, usize, out) && !out.empty();
        }
        out.assign(p, p + n);
        return true;
    }
};

struct Stream { uint32_t usage = 0, index = 0, type = 0, count = 0, offset = 0; int32_t sb = -1; };
struct DecodedVB {
    bool ok = false;
    std::vector<float> pos, nrm, uv;   // 3, 3, 2 per vertex
    size_t count = 0;
};

float component(const uint8_t* p, uint32_t type) {
    switch (type) {
        case 0: return (float)(int8_t)p[0];
        case 1: return (float)p[0];
        case 2: { int16_t v; memcpy(&v, p, 2); return (float)v; }
        case 3: { uint16_t v; memcpy(&v, p, 2); return (float)v; }
        case 4: { int32_t v; memcpy(&v, p, 4); return (float)v; }
        case 5: { float v; memcpy(&v, p, 4); return v; }
        case 6: { uint32_t v; memcpy(&v, p, 4); return (float)v; }
    }
    return 0;
}
size_t componentSize(uint32_t type) {
    switch (type) { case 0: case 1: return 1; case 2: case 3: return 2; }
    return 4;
}

} // namespace

bool isNlScene(const uint8_t* data, size_t len) {
    if (len < 16 || memcmp(data, "SBIN", 4) != 0) return false;
    auto chunks = sbinChunks(data, len);
    if (!findChunk(chunks, "BULK") || !findChunk(chunks, "STRU")) return false;
    auto names = sbinNames(chunks);
    bool inst = false, vb = false;
    for (const std::string& n : names) {
        if (n == "Instance") inst = true;
        if (n == "VertexDeclaration") vb = true;
    }
    return inst && vb;
}

Model loadNlScene(const uint8_t* bytes, size_t len) {
    Model model;
    auto chunks = sbinChunks(bytes, len);
    Scene S;
    S.stru = findChunk(chunks, "STRU");
    S.fiel = findChunk(chunks, "FIEL");
    S.ohdr = findChunk(chunks, "OHDR");
    S.data = findChunk(chunks, "DATA");
    S.bulk = findChunk(chunks, "BULK");
    S.barg = findChunk(chunks, "BARG");
    S.names = sbinNames(chunks);
    if (!S.stru || !S.fiel || !S.ohdr || !S.data || !S.bulk || !S.barg) {
        model.warnings.push_back("not a No Limits scene (missing SBIN chunks)");
        return model;
    }

    // the root map's first value is the Model
    uint32_t kind; size_t off;
    if (!S.object(0, kind, off) || kind != 1) {
        model.warnings.push_back("scene: no root object");
        return model;
    }
    uint16_t vo = S.u16(off + 8);
    uint32_t modelObj = S.u32(off + vo);
    std::string sn; size_t mf; uint16_t msid;
    if (!S.typed(modelObj, sn, mf, msid) || sn != "Model") {
        model.warnings.push_back("scene: the root is not a Model");
        return model;
    }
    int fInstances = S.field(msid, "instances");
    if (fInstances < 0) { model.warnings.push_back("scene: Model has no instances"); return model; }
    std::vector<uint32_t> instances = S.refs(S.u32(mf + fInstances));

    std::map<uint32_t, DecodedVB> vbCache;
    auto decodeVB = [&](uint32_t vbObj) -> const DecodedVB& {
        auto it = vbCache.find(vbObj);
        if (it != vbCache.end()) return it->second;
        DecodedVB& d = vbCache[vbObj];
        std::string n; size_t f; uint16_t sid;
        if (!S.typed(vbObj, n, f, sid) || n != "VertexBuffer") return d;
        int fc = S.field(sid, "count"), fd = S.field(sid, "vertex_declaration"),
            fs = S.field(sid, "scale_bias_entries"), fdata = S.field(sid, "data");
        if (fc < 0 || fd < 0 || fdata < 0) return d;
        uint32_t count = S.u32(f + fc);
        // scale / bias pairs
        std::vector<std::array<float, 8>> sbs;
        if (fs >= 0)
            for (uint32_t e : S.refs(S.u32(f + fs))) {
                std::string en; size_t ef; uint16_t esid;
                std::array<float, 8> v{1, 1, 1, 1, 0, 0, 0, 0};
                if (S.typed(e, en, ef, esid)) {
                    int fsc = S.field(esid, "scale"), fb = S.field(esid, "bias");
                    for (int w = 0; w < 2; ++w) {
                        int fo = w == 0 ? fsc : fb;
                        if (fo < 0) continue;
                        std::string vn; size_t vf; uint16_t vsid;
                        if (!S.typed(S.u32(ef + fo), vn, vf, vsid)) continue;
                        const char* comps[4] = { "x", "y", "z", "w" };
                        for (int c = 0; c < 4; ++c) {
                            int cf = S.field(vsid, comps[c]);
                            if (cf >= 0) v[w * 4 + c] = S.f32(vf + cf);
                        }
                    }
                }
                sbs.push_back(v);
            }
        // the declaration
        std::string dn; size_t df; uint16_t dsid;
        if (!S.typed(S.u32(f + fd), dn, df, dsid)) return d;
        int fStreams = S.field(dsid, "streams"), fStride = S.field(dsid, "stride");
        if (fStreams < 0 || fStride < 0) return d;
        uint32_t stride = S.u32(df + fStride);
        std::vector<Stream> streams;
        for (uint32_t so : S.refs(S.u32(df + fStreams))) {
            std::string xn; size_t xf; uint16_t xsid;
            if (!S.typed(so, xn, xf, xsid)) continue;
            Stream st;
            int a = S.field(xsid, "usage"), b = S.field(xsid, "index"), c = S.field(xsid, "element_type"),
                e = S.field(xsid, "element_count"), g = S.field(xsid, "scale_bias_index"),
                h = S.field(xsid, "offset");
            if (a < 0 || c < 0 || e < 0 || h < 0) continue;
            st.usage = S.u32(xf + a);
            st.index = b >= 0 ? S.u32(xf + b) : 0;
            st.type = S.u32(xf + c);
            st.count = S.u32(xf + e);
            st.sb = g >= 0 ? (int32_t)S.u32(xf + g) : -1;
            st.offset = S.u32(xf + h);
            streams.push_back(st);
        }
        Bytes raw;
        if (!S.bulkData(S.u32(f + fdata), raw) || stride == 0 || count == 0 ||
            (size_t)count * stride > raw.size() || count > 20000000)
            return d;
        d.count = count;
        d.pos.assign((size_t)count * 3, 0.0f);
        bool haveN = false, haveUV = false;
        for (const Stream& st : streams) {
            size_t cs = componentSize(st.type);
            if (st.offset + cs * st.count > stride || st.count == 0) continue;
            std::vector<float>* dst = nullptr;
            int want = 0;
            if (st.usage == 0) { dst = &d.pos; want = 3; }
            else if (st.usage == 4) { dst = &d.nrm; want = 3; haveN = true; }
            else if (st.usage == 8 && st.index == 0) { dst = &d.uv; want = 2; haveUV = true; }
            if (!dst) continue;
            dst->assign((size_t)count * want, 0.0f);
            const std::array<float, 8>* sb =
                st.sb >= 0 && (size_t)st.sb < sbs.size() ? &sbs[st.sb] : nullptr;
            for (uint32_t v = 0; v < count; ++v) {
                const uint8_t* p = raw.data() + (size_t)v * stride + st.offset;
                for (int c = 0; c < want && c < (int)st.count; ++c) {
                    float x = component(p + c * cs, st.type);
                    if (sb) x = x * (*sb)[c] + (*sb)[4 + c];
                    else if (st.usage == 4 && st.type == 0) x /= 127.0f;
                    else if (st.usage == 4 && st.type == 1) x = x / 127.5f - 1.0f;
                    (*dst)[(size_t)v * want + c] = x;
                }
            }
        }
        if (!haveN) d.nrm.clear();
        if (!haveUV) d.uv.clear();
        d.ok = true;
        return d;
    };

    std::map<uint32_t, std::pair<Bytes, uint32_t>> ibCache;   // raw indices, vertex offset
    size_t skipped = 0;
    for (uint32_t io : instances) {
        std::string in; size_t inf; uint16_t isid;
        if (!S.typed(io, in, inf, isid) || in != "Instance") { ++skipped; continue; }
        int fT = S.field(isid, "transform"), fP = S.field(isid, "primitive"),
            fN = S.field(isid, "name"), fL = S.field(isid, "lod_group"),
            fMI = S.field(isid, "material_instance");
        if (fP < 0) { ++skipped; continue; }
        // Matrix4 inline: four Vector4 rows (x, y, z, translation)
        float M[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        if (fT >= 0) for (int k = 0; k < 16; ++k) M[k] = S.f32(inf + fT + (size_t)k * 4);
        std::string pn; size_t pf; uint16_t psid;
        if (!S.typed(S.u32(inf + fP), pn, pf, psid) || pn != "Primitive") { ++skipped; continue; }
        int fType = S.field(psid, "type"), fVB = S.field(psid, "vertex_buffer"),
            fIB = S.field(psid, "index_buffer"), fIO = S.field(psid, "index_offset"),
            fIC = S.field(psid, "index_count");
        if (fVB < 0 || fIB < 0 || fIO < 0 || fIC < 0) { ++skipped; continue; }
        if (fType >= 0 && S.u32(pf + fType) != 1) continue;          // lines: not a surface
        const DecodedVB& vb = decodeVB(S.u32(pf + fVB));
        if (!vb.ok) { ++skipped; continue; }
        uint32_t ibObj = S.u32(pf + fIB);
        auto ic = ibCache.find(ibObj);
        if (ic == ibCache.end()) {
            std::pair<Bytes, uint32_t> e;
            std::string bn; size_t bf; uint16_t bsid;
            if (S.typed(ibObj, bn, bf, bsid)) {
                int fvo = S.field(bsid, "vertex_offset"), fcnt = S.field(bsid, "count"),
                    fdat = S.field(bsid, "data");
                if (fdat >= 0) S.bulkData(S.u32(bf + fdat), e.first);
                e.second = fvo >= 0 ? S.u32(bf + fvo) : 0;
                (void)fcnt;
            }
            ic = ibCache.emplace(ibObj, std::move(e)).first;
        }
        const Bytes& ib = ic->second.first;
        uint32_t vOff = ic->second.second;
        uint32_t first = S.u32(pf + fIO), n = S.u32(pf + fIC);
        // 16-bit indices when the buffer holds fewer than 65536 vertices
        size_t isz = vb.count <= 65536 ? 2 : 4;
        if (((size_t)first + n) * isz > ib.size() || n < 3) {
            isz = 4;
            if (((size_t)first + n) * isz > ib.size()) { ++skipped; continue; }
        }
        Mesh mesh;
        std::string full = fN >= 0 ? S.name(S.u16(inf + fN)) : std::string();
        // "Mesh/region03_env/road_00/MergeGroup-asphalt_road_wssuv-fx/MergeGroup_31/mesh"
        std::string nm = full;
        if (nm.compare(0, 5, "Mesh/") == 0) nm.erase(0, 5);
        if (nm.size() > 5 && nm.compare(nm.size() - 5, 5, "/mesh") == 0) nm.erase(nm.size() - 5);
        mesh.name = nm;
        size_t mg = nm.find("MergeGroup-");
        if (mg != std::string::npos) {
            size_t e = nm.find("-fx", mg);
            mesh.material = nm.substr(mg + 11, (e == std::string::npos ? nm.size() : e) - mg - 11);
        }
        {
            size_t a = nm.find('/');
            size_t b = a == std::string::npos ? std::string::npos : nm.find('/', a + 1);
            mesh.part = a == std::string::npos ? nm : nm.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
        }
        std::string lodName;
        if (fL >= 0) {
            std::string ln; size_t lf; uint16_t lsid;
            if (S.typed(S.u32(inf + fL), ln, lf, lsid)) {
                int fln = S.field(lsid, "name");
                if (fln >= 0) lodName = S.name(S.u16(lf + fln));
            }
        }
        // The material instance: the shader's name, and its variables - an
        // ExternalAsset "#materialvars#N", entry N of the region's
        // .lightmaps.sba, which names the diffuse texture (see
        // nlReadMaterialVars). Kept on the mesh as "#materialvars#N" until
        // that file is read.
        if (fMI >= 0) {
            std::string mn; size_t mfo; uint16_t msd;
            if (S.typed(S.u32(inf + fMI), mn, mfo, msd) && mn == "MaterialInstance") {
                int fMat = S.field(msd, "material"), fVar = S.field(msd, "variables");
                if (fMat >= 0) {
                    // "beast" or "beast_overlay" (decals laid over the road):
                    // the MergeGroup name says more, the overlay is kept
                    std::string shader = S.name(S.u16(mfo + fMat));
                    if (mesh.material.empty()) mesh.material = shader;
                    else if (shader.find("overlay") != std::string::npos) mesh.material += "_overlay_alpha";
                }
                if (fVar >= 0) {
                    std::string en; size_t ef; uint16_t esd;
                    if (S.typed(S.u32(mfo + fVar), en, ef, esd) && en == "ExternalAsset") {
                        int fPath = S.field(esd, "path");
                        std::string path = fPath >= 0 ? S.name(S.u16(ef + fPath)) : std::string();
                        size_t at = path.find("#materialvars#");
                        if (at != std::string::npos) mesh.texture = path.substr(at);
                    }
                }
            }
        }
        // the far versions of the backdrop are a lower detail level
        mesh.lod = (nm.find("downres") != std::string::npos || lodName.find("downres") != std::string::npos ||
                    lodName.find("_lod") != std::string::npos) ? "LOD01" : "LOD00";

        std::map<uint32_t, uint32_t> remap;
        for (uint32_t k = 0; k + 2 < n; k += 3) {
            uint32_t tri[3];
            bool okTri = true;
            for (int c = 0; c < 3; ++c) {
                size_t at = ((size_t)first + k + c) * isz;
                uint32_t v = isz == 2 ? (uint32_t)(ib[at] | (ib[at + 1] << 8))
                                      : (uint32_t)(ib[at] | (ib[at + 1] << 8) | (ib[at + 2] << 16) |
                                                   ((uint32_t)ib[at + 3] << 24));
                v += vOff;
                if (v >= vb.count) { okTri = false; break; }
                auto r = remap.find(v);
                if (r == remap.end()) {
                    uint32_t ni = (uint32_t)(mesh.positions.size() / 3);
                    remap[v] = ni;
                    const float* p = &vb.pos[(size_t)v * 3];
                    float x = p[0], y = p[1], z = p[2];
                    // row vectors: p' = x*X + y*Y + z*Z + T
                    mesh.positions.push_back(x * M[0] + y * M[4] + z * M[8] + M[12]);
                    mesh.positions.push_back(x * M[1] + y * M[5] + z * M[9] + M[13]);
                    mesh.positions.push_back(x * M[2] + y * M[6] + z * M[10] + M[14]);
                    if (!vb.nrm.empty()) {
                        const float* q = &vb.nrm[(size_t)v * 3];
                        float nx = q[0] * M[0] + q[1] * M[4] + q[2] * M[8];
                        float ny = q[0] * M[1] + q[1] * M[5] + q[2] * M[9];
                        float nz = q[0] * M[2] + q[1] * M[6] + q[2] * M[10];
                        float l = std::sqrt(nx * nx + ny * ny + nz * nz);
                        if (l > 1e-6f) { nx /= l; ny /= l; nz /= l; }
                        mesh.normals.push_back(nx); mesh.normals.push_back(ny); mesh.normals.push_back(nz);
                    }
                    if (!vb.uv.empty()) {
                        mesh.uvs.push_back(vb.uv[(size_t)v * 2]);
                        mesh.uvs.push_back(vb.uv[(size_t)v * 2 + 1]);
                    }
                    tri[c] = ni;
                } else {
                    tri[c] = r->second;
                }
            }
            if (!okTri) continue;
            mesh.indices.insert(mesh.indices.end(), tri, tri + 3);
        }
        if (mesh.indices.empty()) { ++skipped; continue; }
        for (size_t k = 0; k + 2 < mesh.positions.size(); k += 3)
            for (int c = 0; c < 3; ++c) {
                float v = mesh.positions[k + c];
                if (k == 0) mesh.bboxMin[c] = mesh.bboxMax[c] = v;
                else { mesh.bboxMin[c] = std::min(mesh.bboxMin[c], v); mesh.bboxMax[c] = std::max(mesh.bboxMax[c], v); }
            }
        model.meshes.push_back(std::move(mesh));
    }
    // one material entry per MergeGroup material
    std::map<std::string, bool> seen;
    for (const Mesh& m : model.meshes) {
        if (m.material.empty() || seen[m.material]) continue;
        seen[m.material] = true;
        Material mt;
        mt.name = m.material;
        mt.alphaBlend = m.material.find("alpha") != std::string::npos ||
                        m.material.find("glass") != std::string::npos;
        mt.additive = m.material.find("glow") != std::string::npos;
        model.materials.push_back(mt);
    }
    if (skipped) {
        char b[96];
        snprintf(b, sizeof(b), "%u instance(s) of %u could not be read", (unsigned)skipped,
                 (unsigned)instances.size());
        model.warnings.push_back(b);
    }
    model.valid = !model.meshes.empty();
    if (!model.valid && model.warnings.empty())
        model.warnings.push_back(zstdAvailable() ? "scene: no drawable instances"
                                                 : "scene: needs Zstandard (libzstd.dll)");
    return model;
}

// The scene names no texture files: a part is "MergeGroup-<material>-fx",
// and the material's picture is one of the game's environment textures,
// named after it (texture_thesquare_sidewalk_diffuse, texture_region07_
// road_lines ...). The best match by name is taken - the whole material
// name first, then shorter forms of it (render-state words like _wssuv or
// _nodynshadows dropped), with textures of the same region preferred.
int nlAssignSceneTextures(Model& m, const std::string& scenePath,
                          const std::vector<std::pair<std::string, std::string>>& textures) {
    auto lower = [](std::string s) {
        for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return s;
    };
    // final_region03_thesquare.scene_static.sba -> region03, thesquare
    std::vector<std::string> regionWords;
    {
        std::string b = lower(baseName(scenePath));
        b = b.substr(0, b.find('.'));
        size_t a = 0;
        while (a < b.size()) {
            size_t e = b.find('_', a);
            if (e == std::string::npos) e = b.size();
            std::string w = b.substr(a, e - a);
            if (w.size() > 3 && w != "final") regionWords.push_back(w);
            a = e + 1;
        }
    }
    static const char* kStateWords[] = { "wssuv", "nodynshadows", "non", "shadow", "casting",
        "alpha", "cull", "alphathresh128", "nozwrite", "solid", "fx", "offset1", "layer2" };
    std::map<std::string, std::string> chosen;
    int assigned = 0;
    for (Mesh& mesh : m.meshes) {
        if (mesh.material.empty() || !mesh.texture.empty()) continue;
        std::string mat = lower(mesh.material);
        auto cached = chosen.find(mat);
        if (cached == chosen.end()) {
            std::vector<std::string> words;
            size_t a = 0;
            while (a < mat.size()) {
                size_t e = mat.find('_', a);
                if (e == std::string::npos) e = mat.size();
                std::string w = mat.substr(a, e - a);
                bool state = false;
                for (const char* k : kStateWords) if (w == k) state = true;
                if (!w.empty() && !state) words.push_back(w);
                a = e + 1;
            }
            std::string best;
            int bestScore = 0;
            for (size_t keep = words.size(); keep >= 1 && bestScore < 40; --keep) {
                std::string core;
                for (size_t k = 0; k < keep; ++k) core += (k ? "_" : "") + words[k];
                if (core.size() < 4) break;
                for (const auto& t : textures) {
                    const std::string& st = t.first;
                    size_t at = st.find(core);
                    if (at == std::string::npos) continue;
                    int score = st == "texture_" + core ? 30 : (at + core.size() == st.size() ? 20 : 10);
                    if (st.find("_normal") != std::string::npos || st.find("_spec") != std::string::npos ||
                        st.find("_refl") != std::string::npos || st.find("_mask") != std::string::npos)
                        score -= 8;
                    for (const std::string& w : regionWords)
                        if (st.find(w) != std::string::npos || t.second.find(w) != std::string::npos) score += 6;
                    score += (int)keep * 2;
                    if (score > bestScore) { bestScore = score; best = t.second; }
                }
            }
            cached = chosen.emplace(mat, best).first;
        }
        if (!cached->second.empty()) { mesh.texture = cached->second; ++assigned; }
    }
    for (Material& mt : m.materials) {
        auto it = chosen.find(lower(mt.name));
        if (it != chosen.end() && mt.diffuse.empty()) mt.diffuse = it->second;
    }
    return assigned;
}


// A region's .lightmaps.sba (one per lighting: day, night, ...) holds, beside
// the baked lightmaps, the material variables every part of the region's
// scene points at: root map "materialvars" -> array of
// BeastMaterialVariables { diffuse -> ExternalAsset path, normal_map,
// glow_map, light_map, material_name, ... }. Entry N is the scene's
// "#materialvars#N". Returned: entry N's diffuse path
// ("/published/textures/environments/road_new/texture_ashphalt_diffuse.sba"),
// empty where it has none.
std::vector<std::string> nlReadMaterialVars(const uint8_t* bytes, size_t len) {
    std::vector<std::string> out;
    if (len < 16 || memcmp(bytes, "SBIN", 4) != 0) return out;
    auto chunks = sbinChunks(bytes, len);
    Scene S;
    S.stru = findChunk(chunks, "STRU");
    S.fiel = findChunk(chunks, "FIEL");
    S.ohdr = findChunk(chunks, "OHDR");
    S.data = findChunk(chunks, "DATA");
    S.names = sbinNames(chunks);
    if (!S.stru || !S.fiel || !S.ohdr || !S.data) return out;
    uint32_t arr = S.mapValue(0, "materialvars");
    if (arr == 0xFFFFFFFFu) return out;
    for (uint32_t o : S.refs(arr)) {
        std::string n; size_t f; uint16_t sid;
        std::string path;
        if (S.typed(o, n, f, sid) && n == "BeastMaterialVariables") {
            int fd = S.field(sid, "diffuse");
            std::string en; size_t ef; uint16_t esd;
            if (fd >= 0 && S.typed(S.u32(f + fd), en, ef, esd) && en == "ExternalAsset") {
                int fp = S.field(esd, "path");
                if (fp >= 0) path = S.name(S.u16(ef + fp));
            }
        }
        out.push_back(path);
    }
    return out;
}

// Put the textures the material variables name on a scene's parts.
// `find` maps a texture's leaf name (texture_ashphalt_diffuse.sba) to a path
// in the library, or "" when it is not there.
int nlApplyMaterialVars(Model& m, const std::vector<std::string>& vars,
                        const std::function<std::string(const std::string&)>& find) {
    int assigned = 0;
    std::map<std::string, std::string> cache;
    for (Mesh& mesh : m.meshes) {
        if (mesh.texture.compare(0, 14, "#materialvars#") != 0) continue;
        size_t idx = (size_t)atoi(mesh.texture.c_str() + 14);
        std::string want = idx < vars.size() ? vars[idx] : std::string();
        mesh.texture.clear();
        if (want.empty()) continue;
        std::string leaf = want.substr(want.find_last_of('/') + 1);
        for (char& c : leaf) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        auto it = cache.find(leaf);
        if (it == cache.end()) it = cache.emplace(leaf, find(leaf)).first;
        if (!it->second.empty()) { mesh.texture = it->second; ++assigned; }
        else mesh.texture = want;              // not in the library: keep the name
    }
    return assigned;
}


// ---------------------------------------------------------------- limited-time layers
namespace {
struct TNode {
    int indent = 0;
    std::string key, value;
    std::vector<TNode*> kids;
};

void buildTree(const std::string& t, std::vector<TNode>& pool, TNode*& root) {
    // count lines first so the pool never reallocates under the pointers
    size_t lines = 1;
    for (char c : t) if (c == '\n') ++lines;
    pool.clear();
    pool.reserve(lines + 1);
    pool.emplace_back();
    root = &pool[0];
    root->indent = -1;
    std::vector<TNode*> stack{ root };
    size_t p = 0;
    while (p < t.size()) {
        size_t e = t.find('\n', p);
        if (e == std::string::npos) e = t.size();
        size_t a = t.find_first_not_of(' ', p);
        size_t end = e;
        while (end > p && (t[end - 1] == '\r' || t[end - 1] == ' ')) --end;
        if (a == std::string::npos || a >= end) { p = e + 1; continue; }
        std::string line = t.substr(a, end - a);
        p = e + 1;
        if (line == "}") continue;
        int indent = (int)(a - (t.rfind('\n', a) == std::string::npos ? 0 : t.rfind('\n', a) + 1));
        pool.emplace_back();
        TNode* n = &pool.back();
        n->indent = indent;
        size_t eq = line.find(" = ");
        if (line[0] == '[') {                       // "[3] actor {"
            size_t rb = line.find("] ");
            n->key = line.substr(0, rb == std::string::npos ? line.size() : rb + 1);
            n->value = rb == std::string::npos ? std::string() : line.substr(rb + 2);
        } else if (eq != std::string::npos) {
            n->key = line.substr(0, eq);
            n->value = line.substr(eq + 3);
        } else {
            n->key = line;
        }
        if (n->value.size() >= 2 && n->value.front() == '"' && n->value.back() == '"')
            n->value = n->value.substr(1, n->value.size() - 2);
        while (stack.size() > 1 && stack.back()->indent >= indent) stack.pop_back();
        stack.back()->kids.push_back(n);
        stack.push_back(n);
    }
}

const TNode* kid(const TNode* n, const char* key) {
    if (!n) return nullptr;
    for (const TNode* k : n->kids) if (k->key == key) return k;
    return nullptr;
}
float num(const TNode* n, const char* key, float def) {
    const TNode* k = kid(n, key);
    return k ? (float)atof(k->value.c_str()) : def;
}
bool startsWith(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }

void matMul(const float a[16], const float b[16], float out[16]) {
    float r[16];
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr) {
            float v = 0;
            for (int k = 0; k < 4; ++k) v += a[k * 4 + rr] * b[c * 4 + k];
            r[c * 4 + rr] = v;
        }
    memcpy(out, r, sizeof(r));
}

// TransformComponent: translation, Euler rotation (degrees: yaw about y,
// pitch about x, roll about z) and scale, as T * Ry * Rx * Rz * S
void localMatrix(const TNode* tc, float out[16]) {
    const TNode* t = kid(tc, "LocalTranslation");
    const TNode* r = kid(tc, "LocalRotation");
    const TNode* s = kid(tc, "LocalScale");
    const float d = 3.14159265358979f / 180.0f;
    float yw = num(r, "yaw", 0) * d, pt = num(r, "pitch", 0) * d, rl = num(r, "roll", 0) * d;
    float cy = std::cos(yw), sy = std::sin(yw), cp = std::cos(pt), sp = std::sin(pt), cr = std::cos(rl), sr = std::sin(rl);
    float Ry[16] = { cy, 0, -sy, 0,  0, 1, 0, 0,  sy, 0, cy, 0,  0, 0, 0, 1 };
    float Rx[16] = { 1, 0, 0, 0,  0, cp, sp, 0,  0, -sp, cp, 0,  0, 0, 0, 1 };
    float Rz[16] = { cr, sr, 0, 0,  -sr, cr, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
    float S[16] = { num(s, "x", 1), 0, 0, 0,  0, num(s, "y", 1), 0, 0,  0, 0, num(s, "z", 1), 0,  0, 0, 0, 1 };
    float m[16];
    matMul(Ry, Rx, m);
    matMul(m, Rz, m);
    matMul(m, S, m);
    m[12] = num(t, "x", 0);
    m[13] = num(t, "y", 0);
    m[14] = num(t, "z", 0);
    memcpy(out, m, sizeof(m));
}

void walkActor(const TNode* actor, const float parent[16], NlLtsGroup& g) {
    float world[16];
    memcpy(world, parent, sizeof(world));
    const TNode* comps = kid(actor, "components");
    if (comps)
        for (const TNode* c : comps->kids)
            if (startsWith(c->value, "TransformComponent")) {
                float local[16];
                localMatrix(c, local);
                matMul(parent, local, world);
            }
    if (comps)
        for (const TNode* c : comps->kids) {
            if (startsWith(c->value, "LayerScene")) {
                const TNode* f = kid(c, "FileName");
                if (f && !f->value.empty() && f->value != "?") g.layerFiles.push_back(f->value);
                const TNode* ln = kid(c, "LayerNames");
                if (ln) {
                    std::string v = ln->value;
                    for (char& ch : v) if (ch == '[' || ch == ']' || ch == ',') ch = ' ';
                    size_t q = 0;
                    while (q < v.size()) {
                        size_t a = v.find_first_not_of(' ', q);
                        if (a == std::string::npos) break;
                        size_t b = v.find(' ', a);
                        if (b == std::string::npos) b = v.size();
                        g.layerNames.push_back(v.substr(a, b - a));
                        q = b;
                    }
                }
            } else if (startsWith(c->value, "Layer")) {
                const TNode* ln = kid(c, "LayerName");
                if (ln && !ln->value.empty() && ln->value != "?") g.layerNames.push_back(ln->value);
            } else if (startsWith(c->value, "NFSModel") || startsWith(c->value, "AnimatedModelComponent")) {
                std::string file;
                for (const char* holder : { "AnimatedM3GFile", "Model" }) {
                    const TNode* h = kid(c, holder);
                    const TNode* fp = kid(h, "Filepath");
                    if (fp && !fp->value.empty() && fp->value != "?") { file = fp->value; break; }
                }
                if (!file.empty()) {
                    NlPlacedModel pm;
                    pm.file = file;
                    memcpy(pm.matrix, world, sizeof(world));
                    g.models.push_back(pm);
                }
            }
        }
    const TNode* ch = kid(actor, "children");
    if (ch) for (const TNode* a : ch->kids) walkActor(a, world, g);
}

bool hasLayerScene(const TNode* n) {
    if (startsWith(n->value, "LayerScene")) return true;
    for (const TNode* k : n->kids) if (hasLayerScene(k)) return true;
    return false;
}
} // namespace

std::vector<NlLtsGroup> nlSceneLtsGroups(const uint8_t* d, size_t n) {
    std::vector<NlLtsGroup> out;
    std::string t = sbinObjectsText(d, n, 256u << 20);
    std::vector<TNode> pool;
    TNode* root = nullptr;
    buildTree(t, pool, root);
    // the scene's actors: the first "actors" list in the tree
    const TNode* actors = nullptr;
    std::vector<const TNode*> todo{ root };
    while (!todo.empty() && !actors) {
        const TNode* x = todo.back();
        todo.pop_back();
        for (const TNode* k : x->kids) {
            if (k->key == "actors") { actors = k; break; }
            todo.push_back(k);
        }
    }
    if (!actors) return out;
    const float I[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
    for (const TNode* a : actors->kids) {
        const TNode* nm = kid(a, "name");
        std::string name = nm ? nm->value : std::string();
        std::string low = name;
        for (char& c : low) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (!hasLayerScene(a) && low.find("lts") == std::string::npos) continue;
        NlLtsGroup g;
        g.name = name == "?" ? std::string() : name;
        walkActor(a, I, g);
        auto dedupe = [](std::vector<std::string>& v) {
            std::vector<std::string> o;
            for (const std::string& x : v) if (std::find(o.begin(), o.end(), x) == o.end()) o.push_back(x);
            v.swap(o);
        };
        dedupe(g.layerFiles);
        dedupe(g.layerNames);
        // a name that says nothing (layerscene) gives way to the layer's file
        bool digit = false;
        for (char c : g.name) if (c >= '0' && c <= '9') digit = true;
        if ((g.name.empty() || (!digit && low.find("lts") == std::string::npos)) && !g.layerFiles.empty())
            g.name = g.layerFiles[0];
        if (g.name.empty()) g.name = "limited-time layer";
        if (g.layerFiles.empty() && g.models.empty()) continue;
        out.push_back(std::move(g));
    }
    return out;
}

std::string nlLtsLabel(const NlLtsGroup& g) {
    std::string s = g.name;
    std::string low = s;
    for (char& c : low) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (low.compare(0, 4, "lts_") == 0) s.erase(0, 4);
    if (low.size() > 4 && low.compare(low.size() - 4, 4, "_lts") == 0) s.erase(s.size() - 4);
    for (char& c : s) if (c == '_') c = ' ';
    return s;
}

void transformModel(Model& m, const float M[16]) {
    for (Mesh& me : m.meshes) {
        for (size_t k = 0; k + 2 < me.positions.size(); k += 3) {
            float x = me.positions[k], y = me.positions[k + 1], z = me.positions[k + 2];
            me.positions[k]     = M[0] * x + M[4] * y + M[8] * z + M[12];
            me.positions[k + 1] = M[1] * x + M[5] * y + M[9] * z + M[13];
            me.positions[k + 2] = M[2] * x + M[6] * y + M[10] * z + M[14];
        }
        for (size_t k = 0; k + 2 < me.normals.size(); k += 3) {
            float x = me.normals[k], y = me.normals[k + 1], z = me.normals[k + 2];
            float nx = M[0] * x + M[4] * y + M[8] * z, ny = M[1] * x + M[5] * y + M[9] * z,
                  nz = M[2] * x + M[6] * y + M[10] * z;
            float l = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (l > 1e-8f) { nx /= l; ny /= l; nz /= l; }
            me.normals[k] = nx; me.normals[k + 1] = ny; me.normals[k + 2] = nz;
        }
        // the bounds again, from the moved points
        if (me.positions.size() >= 3) {
            for (int a = 0; a < 3; ++a) { me.bboxMin[a] = 1e30f; me.bboxMax[a] = -1e30f; }
            for (size_t k = 0; k + 2 < me.positions.size(); k += 3)
                for (int a = 0; a < 3; ++a) {
                    me.bboxMin[a] = std::min(me.bboxMin[a], me.positions[k + a]);
                    me.bboxMax[a] = std::max(me.bboxMax[a], me.positions[k + a]);
                }
        }
    }
}

} // namespace nfsnl
