// nfsnl_im2.cpp - the Firemonkeys M3G layouts: IM2M3G, IM3M3G and IM4M3G
//
// Three games, one container, three spellings:
//
//   JSR184   Real Racing 3        sections of objects, each with its own
//                                 header and checksum; Firemint's own mesh
//                                 record and a material-name list (type 24).
//                                 Read by loadJsr184.
//   IM2M3G   NFS Most Wanted 2012 one run of objects from byte 21 to four
//                                 bytes before the end (no per-section
//                                 framing); scene graph of groups with
//                                 transforms, meshes split into submeshes,
//                                 appearances naming Image2D textures, and
//                                 cars skinned to wheel/suspension joints.
//   IM4M3G   NFS No Limits        exactly IM2 with one more 32-bit field in
//                                 every object's header (after its first
//                                 word), which moves every field after it by
//                                 four. Inside the game's "DA BD" LZHAM wrapper.
//
// The object layouts follow Hypercycle's NFSMW12MobileTools (M3GTools.java),
// which reads and rebuilds Most Wanted's files byte for byte; every object in
// the Most Wanted and No Limits files at hand parses to its exact length with
// them. Object numbers are 1-based positions in the file, 0 is "none".
//
//    3 appearance   [u32 +4?] u32 tracks, refs, params (the material name),
//                   u8 layer, u32 compositing, fog, polygon, material,
//                   u32 texture count, texture references
//    9 group        the same start, then u8 has-transform (T xyz, S xyz,
//                   angle, axis xyz), u8 has-matrix (16 floats), 7 bytes,
//                   u8, u32 child count, children
//   10 image2D      8 (+4) bytes, params (the texture's path), u8, u8 format,
//                   u32 width, height, ...
//   14 mesh         8 (+4) bytes, 14 bytes, u32 vertex buffer, u32 count,
//                   submeshes
//   16 cloned group a group, then u32 vertex buffer, u32 count, submeshes
//                   (one per LOD), u32 skeleton, u32 bone indices, u32 bone
//                   weights, u32 count, joints - a skinned car
//   17 texture ref  12 (+4) bytes, 2 bytes, u32 image2D
//   20 vertex array 12 (+4) bytes, u8 component size, u8 components,
//                   u8 encoding, u16 vertex count, data
//   21 vertex buf.  12 (+4) bytes, u32 colour, u32 positions, bias xyz, scale,
//                   u32 normals, u32 colours, i32 texture sets (-1 = 1), per
//                   set (u32 array, bias xyz, scale), u32 tangents, binormals
//  100 submesh      8 (+4) bytes, params (the part's name), u32 index buffer,
//                   u32 appearance
//  101 index buffer 12 (+4) bytes, u8 encoding (0/1/2 implicit strips,
//                   0x80/81/82 explicit u32/u8/u16), data
#include "nfsnl.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>

namespace nfsnl {

namespace {

struct Obj { uint8_t type = 0; size_t off = 0, len = 0; };

struct Rd {
    const uint8_t* d;
    size_t n, p;
    bool bad = false;
    Rd(const uint8_t* data, size_t len, size_t at) : d(data), n(len), p(at) {}
    bool have(size_t k) const { return p + k <= n; }
    void skip(size_t k) { if (!have(k)) { bad = true; p = n; } else p += k; }
    uint8_t u8() { if (!have(1)) { bad = true; return 0; } return d[p++]; }
    uint16_t u16() { if (!have(2)) { bad = true; return 0; } uint16_t v; memcpy(&v, d + p, 2); p += 2; return v; }
    uint32_t u32() { if (!have(4)) { bad = true; return 0; } uint32_t v; memcpy(&v, d + p, 4); p += 4; return v; }
    int32_t i32() { return (int32_t)u32(); }
    float f32() { if (!have(4)) { bad = true; return 0; } float v; memcpy(&v, d + p, 4); p += 4; return v; }
};

struct M4 { float m[16]; };            // column-major, like OpenGL
M4 identity() { M4 r{}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1; return r; }
M4 mul(const M4& a, const M4& b) {
    M4 r{};
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
            r.m[c * 4 + rr] = s;
        }
    return r;
}
void apply(const M4& a, const float in[3], float out[3], bool point) {
    for (int r = 0; r < 3; ++r)
        out[r] = a.m[r] * in[0] + a.m[4 + r] * in[1] + a.m[8 + r] * in[2] + (point ? a.m[12 + r] : 0);
}
bool isIdentity(const M4& a) {
    M4 i = identity();
    for (int k = 0; k < 16; ++k) if (std::fabs(a.m[k] - i.m[k]) > 1e-6f) return false;
    return true;
}

struct VArr {
    int size = 0, count = 0, verts = 0;
    size_t data = 0;
    bool ok = false;
    // JSR-184 delta encoding (encoding 1, Hot Pursuit): each vertex stored as
    // the difference from the one before, per component, wrapping in the
    // component's own width. Decoded once, here.
    std::vector<float> decoded;
};
struct VBuf {
    uint32_t positions = 0, normals = 0, colours = 0, tex = 0;
    float bias[3] = {0, 0, 0}, scale = 1, tbias[3] = {0, 0, 0}, tscale = 1;
    bool ok = false;
};
struct Node {
    std::string name;
    M4 local = identity();
    // the parts of `local`, for an animated node: T * R * S * G
    bool hasTrs = false;
    float tr[3] = {0, 0, 0};
    M4 S = identity(), G = identity();
    int anim = -1;                      // its node in the model's PartAnim
    std::vector<uint32_t> children;
    bool cloned = false;
    uint32_t vb = 0, skeleton = 0;
    std::vector<uint32_t> lods, joints;
};

} // namespace

Model loadImM3g(const uint8_t* data, size_t len, int version, bool flipV) {
    Model model;
    // ---- the objects: one run from byte 21 to the four-byte checksum ----
    std::vector<Obj> objs(1);
    {
        size_t p = 21;
        while (p + 5 <= len - 4 && objs.size() < 400000) {
            Obj o;
            o.type = data[p];
            uint32_t l;
            memcpy(&l, data + p + 1, 4);
            if (p + 5 + (size_t)l > len) break;
            o.off = p + 5;
            o.len = l;
            objs.push_back(o);
            p += 5 + (size_t)l;
        }
    }
    if (objs.size() <= 1) { model.warnings.push_back("no objects in this M3G"); return model; }
    auto typeOf = [&](uint32_t i) -> int { return i && i < objs.size() ? objs[i].type : -1; };

    // IM4 has four more bytes in every header. Rather than trust the name
    // alone, the first vertex array decides: its size has to add up.
    int E = version == 4 ? 4 : 0;
    for (size_t i = 1; i < objs.size(); ++i) {
        if (objs[i].type != 20) continue;
        for (int e : { E, 4 - E }) {
            Rd r(data, len, objs[i].off + 12 + e);
            int cs = r.u8(), cc = r.u8(), enc = r.u8(), vc = r.u16();
            if (!r.bad && (cs == 1 || cs == 2 || cs == 4) && cc >= 1 && cc <= 4 && enc <= 1 &&
                12 + e + 5 + (size_t)cs * cc * vc == objs[i].len) { E = e; break; }
        }
        break;
    }

    auto readParams = [&](Rd& r, std::vector<std::string>* texts) {
        uint32_t n = r.u32();
        if (r.bad || n > 256) { r.bad = true; return; }
        for (uint32_t k = 0; k < n && !r.bad; ++k) {
            uint32_t ty = r.u32(), sz = r.u32();
            if (r.bad || !r.have(sz)) { r.bad = true; return; }
            if (texts && (ty == 0 || ty == 0x384) && sz) {
                std::string t((const char*)data + r.p, sz);
                while (!t.empty() && t.back() == 0) t.pop_back();
                if (!t.empty()) texts->push_back(t);
            }
            r.skip(sz);
        }
    };
    // the start every node-like object shares: u32, (+4), tracks, params
    auto nodeStart = [&](Rd& r, std::vector<std::string>* names) {
        r.u32();
        r.skip((size_t)E);
        uint32_t tracks = r.u32();
        if (tracks > 1024) { r.bad = true; return; }
        r.skip((size_t)tracks * 4);
        readParams(r, names);
    };

    // ---- vertex arrays and buffers ----
    std::vector<VArr> arrays(objs.size());
    std::vector<VBuf> bufs(objs.size());
    for (size_t i = 1; i < objs.size(); ++i) {
        if (objs[i].type == 20) {
            Rd r(data, len, objs[i].off + 12 + E);
            VArr a;
            a.size = r.u8(); a.count = r.u8();
            int enc = r.u8();
            a.verts = r.u16();
            a.data = r.p;
            a.ok = !r.bad && (a.size == 1 || a.size == 2 || a.size == 4) && a.count >= 1 &&
                   a.count <= 4 && a.data + (size_t)a.size * a.count * a.verts <= objs[i].off + objs[i].len;
            if (a.ok && enc == 1 && a.size != 4) {
                a.decoded.resize((size_t)a.verts * a.count);
                int32_t acc[4] = {0, 0, 0, 0};
                for (int v = 0; v < a.verts; ++v)
                    for (int c = 0; c < a.count; ++c) {
                        size_t at = a.data + ((size_t)v * a.count + c) * a.size;
                        if (a.size == 1) acc[c] = (int8_t)(uint8_t)(acc[c] + (int8_t)data[at]);
                        else { int16_t d16; memcpy(&d16, data + at, 2); acc[c] = (int16_t)(uint16_t)(acc[c] + d16); }
                        a.decoded[(size_t)v * a.count + c] = (float)acc[c];
                    }
            }
            arrays[i] = a;
        } else if (objs[i].type == 21) {
            Rd r(data, len, objs[i].off + 12 + E);
            VBuf b;
            r.u32();
            b.positions = r.u32();
            b.bias[0] = r.f32(); b.bias[1] = r.f32(); b.bias[2] = r.f32();
            b.scale = r.f32();
            b.normals = r.u32();
            b.colours = r.u32();
            int32_t sets = r.i32();
            sets = sets < 0 ? -sets : sets;
            for (int32_t s = 0; s < sets && s < 8 && !r.bad; ++s) {
                uint32_t t = r.u32();
                float tb[3] = { r.f32(), r.f32(), r.f32() };
                float ts = r.f32();
                if (s == 0) { b.tex = t; memcpy(b.tbias, tb, sizeof(tb)); b.tscale = ts; }
            }
            b.ok = !r.bad && b.positions < objs.size() && arrays[b.positions].ok;
            bufs[i] = b;
        }
    }

    // one component of one vertex, as a float (normalised for normals)
    auto comp = [&](const VArr& a, int v, int c) -> float {
        if (!a.decoded.empty()) return a.decoded[(size_t)v * a.count + c];
        size_t at = a.data + ((size_t)v * a.count + c) * a.size;
        if (a.size == 1) return (float)(int8_t)data[at];
        if (a.size == 2) { int16_t s; memcpy(&s, data + at, 2); return (float)s; }
        float f; memcpy(&f, data + at, 4); return f;
    };

    // ---- index buffers ----
    auto readIndices = [&](uint32_t ib, std::vector<uint32_t>& out) -> bool {
        if (typeOf(ib) != 101 && typeOf(ib) != 11) return false;
        Rd r(data, len, objs[ib].off + 12 + E);
        uint8_t enc = r.u8();
        std::vector<uint32_t> raw;
        bool strip = false;
        if (enc >= 0x80 && enc <= 0x82) {
            uint32_t n = r.u32();
            if (n > 8000000 || !r.have((size_t)n * (enc == 0x80 ? 4 : enc == 0x81 ? 1 : 2))) return false;
            raw.reserve(n);
            for (uint32_t k = 0; k < n; ++k)
                raw.push_back(enc == 0x80 ? r.u32() : enc == 0x81 ? r.u8() : r.u16());
        } else if (enc <= 2) {
            uint32_t first = enc == 0 ? r.u32() : enc == 1 ? r.u8() : r.u16();
            uint32_t runs = r.u32();
            if (runs > 1000000) return false;
            strip = true;
            for (uint32_t k = 0; k < runs && !r.bad; ++k) {
                uint32_t n = r.u32();
                if (n > 1000000) return false;
                for (uint32_t j = 0; j < n; ++j) {
                    // a run is one strip: its triangles, with the winding
                    // alternating as strips do
                    if (j >= 2) {
                        uint32_t a = first + j - 2, b = first + j - 1, c = first + j;
                        if (j & 1) { out.push_back(a); out.push_back(c); out.push_back(b); }
                        else { out.push_back(a); out.push_back(b); out.push_back(c); }
                    }
                }
                first += n;
            }
        } else {
            return false;
        }
        if (r.bad) return false;
        // A JSR-184 triangle strip array (type 11, Hot Pursuit) follows its
        // explicit indices with the strip lengths; Most Wanted's index
        // buffer (type 101) is a plain triangle list.
        if (!strip && typeOf(ib) == 11) {
            uint32_t runs = r.u32();
            if (r.bad || runs > 1000000) return false;
            size_t at = 0;
            for (uint32_t k = 0; k < runs && !r.bad; ++k) {
                uint32_t n = r.u32();
                if (at + n > raw.size()) return false;
                for (uint32_t j = 2; j < n; ++j) {
                    uint32_t a = raw[at + j - 2], b = raw[at + j - 1], c = raw[at + j];
                    if (j & 1) { out.push_back(a); out.push_back(c); out.push_back(b); }
                    else { out.push_back(a); out.push_back(b); out.push_back(c); }
                }
                at += n;
            }
            return !out.empty();
        }
        if (!strip) out.insert(out.end(), raw.begin(), raw.end());
        return !out.empty();
    };

    // ---- appearances: material name and texture ----
    struct Look { std::string material, texture; };
    std::map<uint32_t, Look> looks;
    auto lookOf = [&](uint32_t app) -> Look {
        auto it = looks.find(app);
        if (it != looks.end()) return it->second;
        Look lk;
        if (typeOf(app) == 3) {
            Rd r(data, len, objs[app].off);
            std::vector<std::string> names;
            nodeStart(r, &names);
            if (!names.empty()) lk.material = names[0];
            r.u8();
            r.skip(16);
            uint32_t texCount = r.u32();
            for (uint32_t k = 0; k < texCount && k < 8 && !r.bad && lk.texture.empty(); ++k) {
                uint32_t ref = r.u32();
                if (typeOf(ref) != 17) continue;
                Rd t(data, len, objs[ref].off + 12 + E + 2);
                uint32_t img = t.u32();
                if (t.bad || typeOf(img) != 10) continue;
                Rd im(data, len, objs[img].off + 8 + E);
                std::vector<std::string> paths;
                readParams(im, &paths);
                for (const std::string& pth : paths)
                    if (pth.find('/') != std::string::npos || pth.find('.') != std::string::npos) {
                        lk.texture = pth;
                        break;
                    }
            }
            // Most Wanted's files name their textures .m3g and the game ships
            // them as .sba; Hot Pursuit's really are .m3g files
            if (version != 5 && lk.texture.size() > 4 && lk.texture.compare(lk.texture.size() - 4, 4, ".m3g") == 0)
                lk.texture.replace(lk.texture.size() - 4, 4, ".sba");
        }
        looks[app] = lk;
        return lk;
    };

    // ---- scene graph ----
    std::map<uint32_t, Node> nodes;
    std::map<uint32_t, uint32_t> parent;
    for (size_t i = 1; i < objs.size(); ++i) {
        if (objs[i].type != 9 && objs[i].type != 16) continue;
        Rd r(data, len, objs[i].off);
        Node nd;
        std::vector<std::string> names;
        nodeStart(r, &names);
        if (!names.empty()) nd.name = names[0];
        if (r.u8()) {
            float t[3] = { r.f32(), r.f32(), r.f32() };
            float s[3] = { r.f32(), r.f32(), r.f32() };
            float ang = r.f32();
            float ax[3] = { r.f32(), r.f32(), r.f32() };
            // T * R * S, the order JSR-184 composes a node's transform in
            M4 T = identity(), R = identity(), S = identity();
            T.m[12] = t[0]; T.m[13] = t[1]; T.m[14] = t[2];
            S.m[0] = s[0]; S.m[5] = s[1]; S.m[10] = s[2];
            float l = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
            if (l > 1e-6f && std::fabs(ang) > 1e-6f) {
                float x = ax[0] / l, y = ax[1] / l, z = ax[2] / l;
                float a = ang * 3.14159265358979f / 180.0f, c = std::cos(a), sn = std::sin(a), k = 1 - c;
                R.m[0] = c + x * x * k;     R.m[4] = x * y * k - z * sn; R.m[8] = x * z * k + y * sn;
                R.m[1] = y * x * k + z * sn; R.m[5] = c + y * y * k;     R.m[9] = y * z * k - x * sn;
                R.m[2] = z * x * k - y * sn; R.m[6] = z * y * k + x * sn; R.m[10] = c + z * z * k;
            }
            nd.local = mul(T, mul(R, S));
            nd.hasTrs = true;
            memcpy(nd.tr, t, sizeof(t));
            nd.S = S;
        }
        if (r.u8()) {
            // a general 4x4, row-major in the file
            M4 G{};
            for (int rr = 0; rr < 4; ++rr)
                for (int c = 0; c < 4; ++c) G.m[c * 4 + rr] = r.f32();
            nd.local = mul(nd.local, G);
            nd.G = G;
        }
        r.skip(7);
        r.u8();
        if (objs[i].type == 9) {
            uint32_t n = r.u32();
            for (uint32_t k = 0; k < n && k < 100000 && !r.bad; ++k) nd.children.push_back(r.u32());
        } else {
            nd.cloned = true;
            nd.vb = r.u32();
            uint32_t n = r.u32();
            for (uint32_t k = 0; k < n && k < 4096 && !r.bad; ++k) nd.lods.push_back(r.u32());
            nd.skeleton = r.u32();
            r.u32(); r.u32();                        // bone indices, bone weights
            uint32_t m = r.u32();
            for (uint32_t k = 0; k < m && k < 4096 && !r.bad; ++k) nd.joints.push_back(r.u32());
            if (nd.skeleton) nd.children.push_back(nd.skeleton);
        }
        if (r.bad) continue;
        for (uint32_t c : nd.children) if (c < objs.size() && !parent.count(c)) parent[c] = (uint32_t)i;
        nodes[(uint32_t)i] = std::move(nd);
    }
    // ---- animation (Most Wanted's opening spoilers) ----
    // A node's Object3D header lists animation tracks (type 2): a keyframe
    // sequence (19), a controller (1) and the property it drives - 275
    // translation, 268 orientation (a quaternion). The sequences here carry
    // their times first, then the values (encoding 0x80). The node's own
    // translation and rotation are what the tracks replace; its scale and
    // matrix stay.
    struct Keys { int comps = 0; std::vector<float> t, v; };
    auto readKeys = [&](uint32_t ks, Keys& k) -> bool {
        if (typeOf(ks) != 19) return false;
        Rd r(data, len, objs[ks].off);
        nodeStart(r, nullptr);
        r.u8(); r.u8();
        uint8_t enc = r.u8();
        r.u32(); r.u32(); r.u32();
        uint32_t comps = r.u32(), n = r.u32();
        if (r.bad || comps < 1 || comps > 16 || n < 1 || n > 100000) return false;
        k.comps = (int)comps;
        if (enc & 0x80) {
            for (uint32_t i = 0; i < n; ++i) k.t.push_back((float)r.i32());
            for (uint32_t i = 0; i < n * comps; ++i) k.v.push_back(r.f32());
        } else if (enc == 0) {
            for (uint32_t i = 0; i < n; ++i) {
                k.t.push_back((float)r.i32());
                for (uint32_t c = 0; c < comps; ++c) k.v.push_back(r.f32());
            }
        } else {
            return false;
        }
        return !r.bad;
    };
    auto sampleKeys = [](const Keys& k, float t, float* out) {
        size_t n = k.t.size();
        size_t i = 0;
        while (i + 1 < n && k.t[i + 1] <= t) ++i;
        float a = 0;
        if (i + 1 < n && k.t[i + 1] > k.t[i]) a = std::min(1.0f, std::max(0.0f, (t - k.t[i]) / (k.t[i + 1] - k.t[i])));
        size_t j = std::min(i + 1, n - 1);
        for (int c = 0; c < k.comps; ++c)
            out[c] = k.v[i * k.comps + c] + (k.v[j * k.comps + c] - k.v[i * k.comps + c]) * a;
    };
    struct AnimNode { uint32_t node; Keys tr, rot; bool hasT = false, hasR = false; };
    std::vector<AnimNode> animNodes;
    for (auto& kv : nodes) {
        Rd r(data, len, objs[kv.first].off);
        r.u32();
        r.skip((size_t)E);
        uint32_t tracks = r.u32();
        if (r.bad || !tracks || tracks > 64) continue;
        AnimNode an;
        an.node = kv.first;
        for (uint32_t k = 0; k < tracks; ++k) {
            uint32_t tr = r.u32();
            if (typeOf(tr) != 2) continue;
            Rd q(data, len, objs[tr].off);
            nodeStart(q, nullptr);
            uint32_t ks = q.u32();
            q.u32();
            uint32_t prop = q.u32();
            if (q.bad) continue;
            if (prop == 275 && readKeys(ks, an.tr) && an.tr.comps == 3) an.hasT = true;
            if (prop == 268 && readKeys(ks, an.rot) && an.rot.comps == 4) an.hasR = true;
        }
        if (an.hasT || an.hasR) animNodes.push_back(std::move(an));
    }
    // a node's own transform at time t
    auto animLocal = [&](const AnimNode& an, float t) -> M4 {
        const Node& nd = nodes[an.node];
        M4 T = identity(), R = identity();
        float tv[3] = { nd.tr[0], nd.tr[1], nd.tr[2] };
        if (an.hasT) sampleKeys(an.tr, t, tv);
        T.m[12] = tv[0]; T.m[13] = tv[1]; T.m[14] = tv[2];
        if (an.hasR) {
            float q[4];
            sampleKeys(an.rot, t, q);
            float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            if (l > 1e-6f) for (float& c : q) c /= l;
            float x = q[0], y = q[1], z = q[2], w = q[3];
            R.m[0] = 1 - 2 * (y * y + z * z); R.m[4] = 2 * (x * y - z * w);     R.m[8] = 2 * (x * z + y * w);
            R.m[1] = 2 * (x * y + z * w);     R.m[5] = 1 - 2 * (x * x + z * z); R.m[9] = 2 * (y * z - x * w);
            R.m[2] = 2 * (x * z - y * w);     R.m[6] = 2 * (y * z + x * w);     R.m[10] = 1 - 2 * (x * x + y * y);
        } else if (nd.hasTrs) {
            // the static rotation: local = T R S G, so R = T^-1 local G^-1 S^-1 -
            // simpler to keep the static rotation part of local itself
            R = nd.local;
            R.m[12] = R.m[13] = R.m[14] = 0;
            return mul(T, R);
        }
        return mul(T, mul(R, mul(nd.S, nd.G)));
    };
    // the first frame is the rest pose the meshes are built in
    for (size_t a = 0; a < animNodes.size(); ++a) {
        const AnimNode& an = animNodes[a];
        float t0 = 1e30f;
        if (an.hasT && !an.tr.t.empty()) t0 = std::min(t0, an.tr.t.front());
        if (an.hasR && !an.rot.t.empty()) t0 = std::min(t0, an.rot.t.front());
        if (t0 > 1e29f) t0 = 0;
        nodes[an.node].local = animLocal(an, t0);
        nodes[an.node].anim = (int)a;
    }

    std::map<uint32_t, M4> worldCache;
    std::function<M4(uint32_t, int)> world = [&](uint32_t i, int depth) -> M4 {
        auto c = worldCache.find(i);
        if (c != worldCache.end()) return c->second;
        M4 w = identity();
        auto pi = parent.find(i);
        if (pi != parent.end() && depth < 256) w = world(pi->second, depth + 1);
        auto ni = nodes.find(i);
        if (ni != nodes.end()) w = mul(w, ni->second.local);
        worldCache[i] = w;
        return w;
    };
    // the animation as the viewer plays it: per animated node, its world
    // transform over time as translation + rotation channels (the Real Racing
    // 3 .banim units: 1/65536 m, 1/65536 turn; axes lateral, fore, up)
    if (!animNodes.empty()) {
        PartAnim pa;
        pa.name = "parts";
        const float kTurn = 6.28318530718f / 65536.0f;
        for (const AnimNode& an : animNodes) {
            std::vector<float> times;
            if (an.hasT) times.insert(times.end(), an.tr.t.begin(), an.tr.t.end());
            if (an.hasR) times.insert(times.end(), an.rot.t.begin(), an.rot.t.end());
            std::sort(times.begin(), times.end());
            times.erase(std::unique(times.begin(), times.end()), times.end());
            auto pi = parent.find(an.node);
            M4 pw = pi != parent.end() ? world(pi->second, 1) : identity();
            PartAnim::Node node;
            float prev[3] = { 0, 0, 0 };
            bool first = true;
            for (float t : times) {
                M4 w = mul(pw, animLocal(an, t));
                // strip any scale from the rotation part
                float c0 = std::sqrt(w.m[0] * w.m[0] + w.m[1] * w.m[1] + w.m[2] * w.m[2]);
                float c1 = std::sqrt(w.m[4] * w.m[4] + w.m[5] * w.m[5] + w.m[6] * w.m[6]);
                float c2 = std::sqrt(w.m[8] * w.m[8] + w.m[9] * w.m[9] + w.m[10] * w.m[10]);
                if (c0 < 1e-6f || c1 < 1e-6f || c2 < 1e-6f) continue;
                float r00 = w.m[0] / c0, r10 = w.m[1] / c0, r20 = w.m[2] / c0;
                float r21 = w.m[6] / c1, r22 = w.m[10] / c2;
                float ay = std::asin(std::max(-1.0f, std::min(1.0f, -r20)));
                float ax = std::atan2(r21, r22);
                float az = std::atan2(r10, r00);
                float e[3] = { ax, ay, az };
                if (!first)
                    for (int k = 0; k < 3; ++k) {
                        while (e[k] - prev[k] > 3.14159265f) e[k] -= 6.28318531f;
                        while (e[k] - prev[k] < -3.14159265f) e[k] += 6.28318531f;
                    }
                memcpy(prev, e, sizeof(e));
                first = false;
                node.ch[0].push_back({ t, w.m[12] * 65536.0f });
                node.ch[1].push_back({ t, -w.m[14] * 65536.0f });
                node.ch[2].push_back({ t, w.m[13] * 65536.0f });
                node.ch[3].push_back({ t, e[0] / kTurn });
                node.ch[4].push_back({ t, -e[2] / kTurn });
                node.ch[5].push_back({ t, e[1] / kTurn });
                pa.duration = std::max(pa.duration, t);
            }
            pa.nodes.push_back(std::move(node));
        }
        model.anims.push_back(std::move(pa));
    }
    // the animated node a mesh hangs under, if any
    auto animatedAncestor = [&](uint32_t i) -> int {
        int guard = 0;
        for (uint32_t at = i; guard++ < 128;) {
            auto ni = nodes.find(at);
            if (ni != nodes.end() && ni->second.anim >= 0) return ni->second.anim;
            auto pi = parent.find(at);
            if (pi == parent.end()) break;
            at = pi->second;
        }
        return -1;
    };
    auto parentName = [&](uint32_t i) -> std::string {
        auto pi = parent.find(i);
        if (pi == parent.end()) return "";
        auto ni = nodes.find(pi->second);
        return ni == nodes.end() ? "" : ni->second.name;
    };

    // A detail-level node (lod_01, lod_02 ...) above a mesh says its level
    // whatever the mesh's own name claims: NFS Edge's wheels keep their fine
    // rim in a group called ..._rim_notint_lod02 under lod_01.
    auto ancestorLod = [&](uint32_t i) -> int {
        int guard = 0;
        for (auto pi = parent.find(i); pi != parent.end() && guard++ < 64; pi = parent.find(pi->second)) {
            auto ni = nodes.find(pi->second);
            if (ni == nodes.end()) continue;
            const std::string& n = ni->second.name;
            if (n.size() == 6 && (n.compare(0, 4, "lod_") == 0 || n.compare(0, 4, "LOD_") == 0) &&
                n[4] >= '0' && n[4] <= '9' && n[5] >= '0' && n[5] <= '9')
                return (n[4] - '0') * 10 + (n[5] - '0');
        }
        return -1;
    };
    int curAncestorLod = -1;

    // ---- one output mesh per submesh ----
    auto build = [&](uint32_t vbIdx, uint32_t sub, const M4& xf, const std::string& fallbackName) {
        if (vbIdx >= objs.size() || !bufs[vbIdx].ok || typeOf(sub) != 100) return;
        const VBuf& vb = bufs[vbIdx];
        Rd r(data, len, objs[sub].off + 8 + E);
        std::vector<std::string> names;
        readParams(r, &names);
        uint32_t ib = r.u32(), app = r.u32();
        if (r.bad) return;
        std::vector<uint32_t> idx;
        if (!readIndices(ib, idx)) return;
        const VArr& pa = arrays[vb.positions];
        Mesh m;
        m.name = !names.empty() ? names[0] : fallbackName;
        Look lk = lookOf(app);
        m.material = lk.material;
        m.texture = lk.texture;
        bool xfOn = !isIdentity(xf);
        m.positions.reserve((size_t)pa.verts * 3);
        for (int v = 0; v < pa.verts; ++v) {
            float p[3];
            for (int c = 0; c < 3; ++c)
                p[c] = (c < pa.count ? comp(pa, v, c) : 0.0f) * vb.scale + vb.bias[c];
            if (xfOn) { float q[3]; apply(xf, p, q, true); memcpy(p, q, sizeof(p)); }
            m.positions.insert(m.positions.end(), p, p + 3);
        }
        if (vb.normals && vb.normals < objs.size() && arrays[vb.normals].ok &&
            arrays[vb.normals].verts == pa.verts && arrays[vb.normals].count >= 3) {
            const VArr& na = arrays[vb.normals];
            float div = na.size == 1 ? 127.0f : na.size == 2 ? 32767.0f : 1.0f;
            for (int v = 0; v < pa.verts; ++v) {
                float n[3] = { comp(na, v, 0) / div, comp(na, v, 1) / div, comp(na, v, 2) / div };
                if (xfOn) { float q[3]; apply(xf, n, q, false); memcpy(n, q, sizeof(n)); }
                float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (l > 1e-6f) for (float& c : n) c /= l;
                m.normals.insert(m.normals.end(), n, n + 3);
            }
        }
        if (vb.tex && vb.tex < objs.size() && arrays[vb.tex].ok && arrays[vb.tex].verts == pa.verts &&
            arrays[vb.tex].count >= 2) {
            const VArr& ta = arrays[vb.tex];
            for (int v = 0; v < pa.verts; ++v) {
                float u = comp(ta, v, 0) * vb.tscale + vb.tbias[0];
                float w = comp(ta, v, 1) * vb.tscale + vb.tbias[1];
                // The games keep textures bottom-up (OpenGL order) and the
                // decoder turns them upright, so t is already measured from
                // the bottom of the picture: the usual V. NFSMW12MobileTools
                // flips its PNGs the same way on extraction.
                m.uvs.push_back(u);
                m.uvs.push_back(flipV ? 1.0f - w : w);
            }
        }
        if (vb.colours && vb.colours < objs.size() && arrays[vb.colours].ok &&
            arrays[vb.colours].verts == pa.verts && arrays[vb.colours].size == 1 &&
            arrays[vb.colours].count >= 3) {
            const VArr& ca = arrays[vb.colours];
            for (int v = 0; v < pa.verts; ++v) {
                float c4[4] = {1, 1, 1, 1};
                for (int c = 0; c < ca.count && c < 4; ++c)
                    c4[c] = (ca.decoded.empty()
                                 ? data[ca.data + ((size_t)v * ca.count + c)]
                                 : (uint8_t)(int)ca.decoded[(size_t)v * ca.count + c]) / 255.0f;
                m.colors.insert(m.colors.end(), c4, c4 + 4);
            }
        }
        uint32_t vcount = (uint32_t)pa.verts;
        for (size_t k = 0; k + 2 < idx.size(); k += 3)
            if (idx[k] < vcount && idx[k + 1] < vcount && idx[k + 2] < vcount &&
                idx[k] != idx[k + 1] && idx[k + 1] != idx[k + 2] && idx[k] != idx[k + 2]) {
                m.indices.push_back(idx[k]);
                m.indices.push_back(idx[k + 1]);
                m.indices.push_back(idx[k + 2]);
            }
        if (m.indices.empty()) return;
        // A skinned car's detail levels all index one shared buffer of every
        // vertex; keep only the ones this part uses.
        {
            std::vector<int32_t> remap(vcount, -1);
            uint32_t used = 0;
            for (uint32_t ix : m.indices) if (remap[ix] < 0) remap[ix] = (int32_t)used++;
            if (used < vcount) {
                auto squeeze = [&](std::vector<float>& a, size_t w) {
                    if (a.size() != (size_t)vcount * w) { a.clear(); return; }
                    std::vector<float> b((size_t)used * w);
                    for (uint32_t v = 0; v < vcount; ++v)
                        if (remap[v] >= 0)
                            for (size_t k = 0; k < w; ++k) b[(size_t)remap[v] * w + k] = a[(size_t)v * w + k];
                    a.swap(b);
                };
                squeeze(m.positions, 3);
                if (!m.normals.empty()) squeeze(m.normals, 3);
                if (!m.uvs.empty()) squeeze(m.uvs, 2);
                if (!m.colors.empty()) squeeze(m.colors, 4);
                for (uint32_t& ix : m.indices) ix = (uint32_t)remap[ix];
            }
        }
        // the detail level from the name: _lod_00 (Most Wanted) or _lod00
        std::string low = m.name;
        for (char& ch : low) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        // (NFS Edge also has mesh_chassis_lod02_a_lod: the last _lod that is
        // followed by a number wins)
        for (size_t lp = low.rfind("_lod"); lp != std::string::npos;
             lp = lp ? low.rfind("_lod", lp - 1) : std::string::npos) {
            size_t q = lp + 4;
            if (q < low.size() && low[q] == '_') ++q;
            int level = 0, digits = 0;
            while (q < low.size() && low[q] >= '0' && low[q] <= '9' && digits < 3) {
                level = level * 10 + (low[q] - '0');
                ++q; ++digits;
            }
            if (digits) {
                char buf[8];
                snprintf(buf, sizeof(buf), "LOD%02d", level);
                m.lod = buf;
                break;
            }
            if (lp == 0) break;
        }
        if (curAncestorLod >= 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "LOD%02d", curAncestorLod);
            m.lod = buf;
        }
        if (low.find("collider") != std::string::npos || low.find("collision") != std::string::npos ||
            low.find("shadow_plane") != std::string::npos)
            m.lod = "HELPERS";
        m.part = fallbackName.empty() ? m.name : fallbackName;
        for (size_t k = 0; k + 2 < m.positions.size(); k += 3)
            for (int c = 0; c < 3; ++c) {
                float v = m.positions[k + c];
                if (k == 0) m.bboxMin[c] = m.bboxMax[c] = v;
                else { m.bboxMin[c] = std::min(m.bboxMin[c], v); m.bboxMax[c] = std::max(m.bboxMax[c], v); }
            }
        model.meshes.push_back(std::move(m));
    };

    for (size_t i = 1; i < objs.size(); ++i) {
        if (objs[i].type != 14) continue;
        Rd r(data, len, objs[i].off + 8 + E + 14);
        uint32_t vb = r.u32(), n = r.u32();
        if (r.bad || n > 4096) continue;
        M4 xf = world((uint32_t)i, 0);
        std::string pname = parentName((uint32_t)i);
        curAncestorLod = ancestorLod((uint32_t)i);
        size_t before = model.meshes.size();
        for (uint32_t k = 0; k < n && !r.bad; ++k) build(vb, r.u32(), xf, pname);
        curAncestorLod = -1;
        int an = animatedAncestor((uint32_t)i);
        if (an >= 0 && !model.anims.empty())
            for (size_t k = before; k < model.meshes.size(); ++k) {
                model.meshes[k].animIndex = 0;
                model.meshes[k].animNode = an;
            }
    }
    // skinned parts (a car's wheels and suspension): the vertices are stored
    // in their bind pose, which is where they belong on the car
    for (auto& kv : nodes) {
        if (!kv.second.cloned) continue;
        M4 xf = world(kv.first, 0);
        for (uint32_t sub : kv.second.lods) build(kv.second.vb, sub, xf, kv.second.name);
    }

    // Only some detail levels of a moving part carry the tracks (Most
    // Wanted's Aventador animates its spoiler's lod_01 and lod_02 only); the
    // others are the same part and move with it.
    if (!model.anims.empty()) {
        auto stem = [](std::string n) {
            for (char& c : n) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            size_t p = n.rfind("_lod");
            return p == std::string::npos ? n : n.substr(0, p);
        };
        std::map<std::string, int> byStem;
        for (const Mesh& m : model.meshes)
            if (m.animIndex >= 0) byStem.emplace(stem(m.name), m.animNode);
        for (Mesh& m : model.meshes) {
            if (m.animIndex >= 0) continue;
            auto it = byStem.find(stem(m.name));
            if (it == byStem.end()) continue;
            m.animIndex = 0;
            m.animNode = it->second;
        }
    }

    // Parts with no level in their name (a cop's light bars, a scene's props)
    // belong with the most detailed level the file has. That is not always
    // LOD00: Most Wanted's cop cars carry only LOD03-05.
    {
        std::string best;
        for (const Mesh& m : model.meshes)
            if (m.lod.compare(0, 3, "LOD") == 0 && (best.empty() || m.lod < best)) best = m.lod;
        if (best.empty()) best = "LOD00";
        for (Mesh& m : model.meshes) if (m.lod.empty()) m.lod = best;
    }
    // Hot Pursuit: MESH_metal_lod is the far version of MESH_metal, and
    // MESH_cop the police parts laid over the same body
    if (version == 5)
        for (Mesh& m : model.meshes) {
            std::string low = m.name;
            for (char& ch : low) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
            if (low.size() > 4 && low.compare(low.size() - 4, 4, "_lod") == 0) m.lod = "LOD01";
            if (low == "mesh_cop") m.lod = "COP";
        }
    // Hot Pursuit's cars carry the right-hand wheels only; the game mirrors
    // them for the left. Do the same, so the export has four.
    if (version == 5) {
        size_t n = model.meshes.size();
        for (size_t i = 0; i < n; ++i) {
            const Mesh& src = model.meshes[i];
            std::string low = src.name;
            for (char& ch : low) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
            size_t at = low.rfind("_right");
            if (low.find("wheel") == std::string::npos || at == std::string::npos) continue;
            std::string leftName = src.name.substr(0, at) + "_left" + src.name.substr(at + 6);
            bool have = false;
            for (const Mesh& o : model.meshes) if (o.name == leftName) have = true;
            if (have) continue;
            Mesh m = src;
            m.name = leftName;
            m.part = leftName;
            for (size_t k = 0; k < m.positions.size(); k += 3) m.positions[k] = -m.positions[k];
            for (size_t k = 0; k < m.normals.size(); k += 3) m.normals[k] = -m.normals[k];
            for (size_t k = 0; k + 2 < m.indices.size(); k += 3) std::swap(m.indices[k + 1], m.indices[k + 2]);
            float lo = -m.bboxMax[0], hi = -m.bboxMin[0];
            m.bboxMin[0] = lo; m.bboxMax[0] = hi;
            model.meshes.push_back(std::move(m));
        }
    }

    // named points: locators, pivots and joints become empties in the export.
    // A file with no geometry at all (an overlay, a searchlight's path) is
    // nothing but its groups, so then every named group is kept.
    bool anyGeometry = !model.meshes.empty();
    for (auto& kv : nodes) {
        const std::string& n = kv.second.name;
        if (n.empty()) continue;
        if (anyGeometry && n.rfind("locator_", 0) && n.rfind("pivot_", 0) && n.rfind("J_", 0) &&
            n.rfind("POINT_", 0) && n.rfind("point_", 0))
            continue;
        M4 w = world(kv.first, 0);
        Hardpoint h;
        h.name = n;
        h.pos[0] = w.m[12]; h.pos[1] = w.m[13]; h.pos[2] = w.m[14];
        model.points.push_back(h);
    }

    // every material that is named, for the export's material list
    for (auto& kv : looks) {
        if (kv.second.material.empty()) continue;
        bool seen = false;
        for (const Material& mt : model.materials) if (mt.name == kv.second.material) seen = true;
        if (seen) continue;
        Material mt;
        mt.name = kv.second.material;
        mt.diffuse = kv.second.texture;
        std::string low = mt.name;
        for (char& ch : low) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        mt.alphaBlend = low.find("alpha") != std::string::npos || low.find("window") != std::string::npos;
        mt.additive = low.find("additive") != std::string::npos || low.find("falloff") != std::string::npos;
        model.materials.push_back(mt);
    }
    model.valid = !model.meshes.empty();
    if (!model.valid) {
        bool anyMesh = false;
        for (size_t i = 1; i < objs.size(); ++i) if (objs[i].type == 14 || objs[i].type == 16) anyMesh = true;
        model.warnings.push_back(anyMesh ? "the M3G parsed but held no drawable meshes"
                                         : "this M3G holds no geometry - only groups and "
                                           "animation (a transform or camera path). It "
                                           "exports to FBX as empties: the groups' names "
                                           "and positions.");
    }
    return model;
}

} // namespace nfsnl

namespace nfsnl {

// NFS Most Wanted's downloadable cars (the Jaguar C-X16 ...) carry 8x8
// stand-in pictures where the other cars name their textures; the game
// finds the real ones by the car's name, as textures/cars/<car>/
// texture_<car>_diffuse_00 (glass: _alpha). The parts left without a
// texture get those names, written as the other cars write theirs.
int mwFillCarTextures(Model& m, const std::string& car) {
    bool mw = false;
    for (const Mesh& me : m.meshes)
        if (me.material == "damage_opaque_paint_scratch" || me.material == "windows_shatter") mw = true;
    if (!mw || car.empty()) return 0;
    std::string dir = "../../../textures/cars/" + car + "/texture_" + car;
    int n = 0;
    for (Mesh& me : m.meshes) {
        if (!me.texture.empty() && me.texture.find('/') != std::string::npos) continue;
        const std::string& mat = me.material;
        std::string t;
        if (mat == "windows_shatter" || mat.find("glass") != std::string::npos || mat.find("window") != std::string::npos)
            t = dir + "_alpha.sba";
        else if (mat == "falloff_lights") t = "../../../textures/cars/texture_light_falloff.sba";
        else if (mat == "numberplate") t = "../../../textures/cars/numberplates/texture_numberplate_need4speed.sba";
        else if (!mat.empty()) t = dir + "_diffuse_00.sba";
        if (!t.empty()) { me.texture = t; ++n; }
    }
    return n;
}

} // namespace nfsnl
