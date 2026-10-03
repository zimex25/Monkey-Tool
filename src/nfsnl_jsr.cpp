// nfsnl_jsr.cpp - standard JSR-184 (M3G 1.0) scenes: Need for Speed
// Undercover, Shift and Shift 2 Unleashed on the iPhone
//
// These games write M3G by the book: every object starts with Object3D,
// nodes carry Transformable and Node fields, Groups list their children, a
// Mesh names a VertexBuffer and (TriangleStripArray, Appearance) pairs.
// Shift wraps each file in gzip. Real Racing's files are Firemint's own
// variant of the format and keep their reader (loadJsr184); this one only
// takes a file whose every object parses exactly as the spec lays it out.
//
// Textures: a track carries its atlases inside (Image2D, format 124/125 =
// PVRTC 4 bpp with its mip chain, 99/100 = RGB/RGBA bytes); a car carries
// none and the game picks texture_car_<name>.m3g by name.
#include "nfsnl.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>

namespace nfsnl {

namespace {

struct Rd {
    const uint8_t* d; size_t n; size_t p = 0; bool bad = false;
    Rd(const uint8_t* data, size_t len) : d(data), n(len) {}
    bool have(size_t k) { if (p + k > n) { bad = true; return false; } return true; }
    uint8_t u8() { if (!have(1)) return 0; return d[p++]; }
    uint16_t u16() { if (!have(2)) return 0; uint16_t v; memcpy(&v, d + p, 2); p += 2; return v; }
    uint32_t u32() { if (!have(4)) return 0; uint32_t v; memcpy(&v, d + p, 4); p += 4; return v; }
    float f32() { if (!have(4)) return 0; float v; memcpy(&v, d + p, 4); p += 4; return v; }
    void skip(size_t k) { if (have(k)) p += k; }
};

struct Obj { uint8_t type = 0; const uint8_t* data = nullptr; size_t len = 0; };

// Object3D: user id, animation tracks, user parameters
bool readObject3D(Rd& r, uint32_t* userId = nullptr) {
    uint32_t id = r.u32();
    if (userId) *userId = id;
    uint32_t tracks = r.u32();
    if (tracks > 4096) { r.bad = true; return false; }
    r.skip((size_t)tracks * 4);
    uint32_t params = r.u32();
    if (params > 4096) { r.bad = true; return false; }
    for (uint32_t k = 0; k < params && !r.bad; ++k) {
        r.u32();
        uint32_t sz = r.u32();
        r.skip(sz);
    }
    return !r.bad;
}

typedef std::array<float, 16> Mat4;     // column-major, as OpenGL

Mat4 identity() { Mat4 m{}; m[0] = m[5] = m[10] = m[15] = 1; return m; }
Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + rr] * b[c * 4 + k];
            r[c * 4 + rr] = s;
        }
    return r;
}

// Transformable: an optional T R S and an optional general 4x4 (row-major
// in the file), composed T * R * S * M
bool readTransformable(Rd& r, Mat4& out) {
    out = identity();
    if (r.u8()) {
        float t[3] = { r.f32(), r.f32(), r.f32() };
        float s[3] = { r.f32(), r.f32(), r.f32() };
        float ang = r.f32();
        float ax[3] = { r.f32(), r.f32(), r.f32() };
        Mat4 T = identity();
        T[12] = t[0]; T[13] = t[1]; T[14] = t[2];
        Mat4 R = identity();
        float l = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
        if (ang != 0 && l > 1e-9f) {
            float x = ax[0] / l, y = ax[1] / l, z = ax[2] / l;
            float a = ang * 3.14159265f / 180.0f, c = std::cos(a), sn = std::sin(a), k = 1 - c;
            R[0] = x * x * k + c;     R[4] = x * y * k - z * sn; R[8]  = x * z * k + y * sn;
            R[1] = y * x * k + z * sn; R[5] = y * y * k + c;     R[9]  = y * z * k - x * sn;
            R[2] = z * x * k - y * sn; R[6] = z * y * k + x * sn; R[10] = z * z * k + c;
        }
        Mat4 S = identity();
        S[0] = s[0]; S[5] = s[1]; S[10] = s[2];
        out = mul(mul(T, R), S);
    }
    if (r.u8()) {
        Mat4 g{};
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col) g[col * 4 + row] = r.f32();
        out = mul(out, g);
    }
    return !r.bad;
}

// Node: rendering and picking flags, alpha factor, scope, alignment
bool readNode(Rd& r, Mat4& local, bool* enabled = nullptr) {
    if (!readTransformable(r, local)) return false;
    bool en = r.u8() != 0;
    if (enabled) *enabled = en;
    r.u8();                          // picking
    r.u8();                          // alpha factor
    r.u32();                         // scope
    if (r.u8()) { r.u8(); r.u8(); r.u32(); r.u32(); }   // alignment
    return !r.bad;
}

struct VArr { int cs = 0, cc = 0, count = 0; std::vector<float> v; };

bool readVertexArray(const Obj& o, VArr& a) {
    Rd r(o.data, o.len);
    if (!readObject3D(r)) return false;
    a.cs = r.u8();
    a.cc = r.u8();
    int enc = r.u8();
    a.count = r.u16();
    if (r.bad || (a.cs != 1 && a.cs != 2 && a.cs != 4) || a.cc < 2 || a.cc > 4) return false;
    size_t n = (size_t)a.count * a.cc;
    if (!r.have(n * a.cs)) return false;
    a.v.resize(n);
    std::vector<int32_t> prev(a.cc, 0);
    for (size_t i = 0; i < n; ++i) {
        int c = (int)(i % a.cc);
        if (a.cs == 4) { a.v[i] = r.f32(); continue; }
        int32_t x = a.cs == 1 ? (int32_t)(int8_t)r.u8() : (int32_t)(int16_t)r.u16();
        if (enc) {                   // delta encoded, wrapping at the type's width
            x += prev[c];
            x = a.cs == 1 ? (int32_t)(int8_t)x : (int32_t)(int16_t)x;
            prev[c] = x;
        }
        a.v[i] = (float)x;
    }
    return !r.bad;
}

bool readStrips(const Obj& o, std::vector<uint32_t>& tris) {
    Rd r(o.data, o.len);
    if (!readObject3D(r)) return false;
    int enc = r.u8();
    std::vector<uint32_t> idx;
    uint32_t start = 0;
    bool implicit = enc < 128;
    if (enc == 0) start = r.u32();
    else if (enc == 1) start = r.u8();
    else if (enc == 2) start = r.u16();
    else if (enc >= 128 && enc <= 130) {
        uint32_t n = r.u32();
        if (n > (1u << 24)) return false;
        idx.resize(n);
        for (uint32_t k = 0; k < n && !r.bad; ++k)
            idx[k] = enc == 128 ? r.u32() : enc == 129 ? r.u8() : r.u16();
    } else return false;
    uint32_t nl = r.u32();
    if (r.bad || nl > (1u << 24)) return false;
    std::vector<uint32_t> lens(nl);
    size_t total = 0;
    for (uint32_t k = 0; k < nl && !r.bad; ++k) { lens[k] = r.u32(); total += lens[k]; }
    if (r.bad) return false;
    if (implicit) { idx.resize(total); for (size_t k = 0; k < total; ++k) idx[k] = start + (uint32_t)k; }
    if (idx.size() < total) return false;
    size_t p = 0;
    for (uint32_t L : lens) {
        for (uint32_t i = 0; i + 2 < L; ++i) {
            uint32_t a = idx[p + i], b = idx[p + i + 1], c = idx[p + i + 2];
            if (a == b || b == c || a == c) continue;
            if (i & 1) std::swap(a, b);
            tris.push_back(a); tris.push_back(b); tris.push_back(c);
        }
        p += L;
    }
    return true;
}

} // namespace

// An M3G Image2D's pixels: RGB / RGBA bytes, or EA's PVRTC 4 bpp codes 124
// (RGB) and 125 (RGBA), the first mip level of the chain that follows
bool decodeM3gPixels(int format, int w, int h, const uint8_t* px, size_t n, Image& out) {
    size_t count = (size_t)w * h;
    if (format == 124 || format == 125) {
        size_t need = std::max<size_t>(32, count / 2);
        if (n < need) return false;
        return decodePvrtcBlocks(px, need, w, h, false, out);
    }
    int ch = n >= count * 4 && (format == 100 || n == count * 4) ? 4
           : n >= count * 3 ? 3 : n >= count * 2 ? 2 : n >= count ? 1 : 0;
    if (!ch) return false;
    out = Image();
    out.width = w; out.height = h;
    if (ch == 2) {
        out.channels = 4;
        out.pixels.resize(count * 4);
        for (size_t i = 0; i < count; ++i) {
            out.pixels[i * 4] = out.pixels[i * 4 + 1] = out.pixels[i * 4 + 2] = px[i * 2];
            out.pixels[i * 4 + 3] = px[i * 2 + 1];
        }
    } else {
        out.channels = ch;
        out.pixels.assign(px, px + count * ch);
    }
    return true;
}

Model loadJsr184Scene(const uint8_t* data, size_t len) {
    Model model;
    Bytes gz;
    if (len > 18 && data[0] == 0x1F && data[1] == 0x8B && inflateGzip(data, len, gz)) {
        data = gz.data();
        len = gz.size();
    }
    static const uint8_t kId[12] = { 0xAB, 'J', 'S', 'R', '1', '8', '4', 0xBB, 0x0D, 0x0A, 0x1A, 0x0A };
    if (len < 12 || memcmp(data, kId, 12) != 0) return model;

    // ---- every object, from every section (zlib-packed ones unpacked) ----
    std::vector<Bytes> sections;
    std::vector<Obj> objs(1);                // index 0: the null reference
    size_t p = 12;
    while (p + 13 <= len) {
        uint8_t comp = data[p];
        uint32_t total, plain;
        memcpy(&total, data + p + 1, 4);
        memcpy(&plain, data + p + 5, 4);
        if (total < 13 || p + total > len) return model;
        const uint8_t* body = data + p + 9;
        size_t bodyLen = total - 13;
        if (comp) {
            sections.emplace_back();
            if (!inflateZlib(body, bodyLen, sections.back())) return model;
            body = sections.back().data();
            bodyLen = sections.back().size();
        }
        size_t q = 0;
        while (q + 5 <= bodyLen) {
            Obj o;
            o.type = body[q];
            uint32_t l;
            memcpy(&l, body + q + 1, 4);
            if (q + 5 + (size_t)l > bodyLen) return model;
            o.data = body + q + 5;
            o.len = l;
            objs.push_back(o);
            q += 5 + l;
        }
        p += total;
    }
    if (objs.size() < 3) return model;

    // ---- the scene graph: nodes, their local transforms and children ----
    struct NodeInfo { int type = 0; Mat4 local = identity(); bool enabled = true; uint32_t userId = 0;
                      std::vector<uint32_t> children; uint32_t vb = 0; int ordinal = -1;
                      std::vector<std::pair<uint32_t, uint32_t>> subs; };
    std::map<uint32_t, NodeInfo> nodes;
    std::map<uint32_t, uint32_t> parent;
    int meshes = 0, groups9 = 0;
    for (uint32_t i = 1; i < objs.size(); ++i) {
        const Obj& o = objs[i];
        if (o.type != 9 && o.type != 22 && o.type != 14 && o.type != 16) continue;
        Rd r(o.data, o.len);
        NodeInfo ni;
        ni.type = o.type;
        if (!readObject3D(r, &ni.userId) || !readNode(r, ni.local, &ni.enabled)) return model;
        if (o.type == 9) ni.ordinal = groups9++;  // the track layouts name pieces by this
        if (o.type == 9 || o.type == 22) {
            uint32_t n = r.u32();
            if (n > 100000) return model;
            for (uint32_t k = 0; k < n && !r.bad; ++k) {
                uint32_t c = r.u32();
                if (c > 0 && c < objs.size()) { ni.children.push_back(c); parent[c] = i; }
            }
            if (o.type == 22) { r.u32(); r.u32(); }   // active camera, background
            if (r.bad || r.p != o.len) return model;  // not the spec's layout
        } else {
            ni.vb = r.u32();
            uint32_t n = r.u32();
            if (n > 256) return model;
            for (uint32_t k = 0; k < n && !r.bad; ++k) {
                uint32_t ib = r.u32(), ap = r.u32();
                ni.subs.push_back({ ib, ap });
            }
            if (r.bad || ni.vb == 0 || ni.vb >= objs.size() || objs[ni.vb].type != 21) return model;
            if (o.type == 14 && r.p != o.len) return model;
            ++meshes;
        }
        nodes[i] = std::move(ni);
    }
    if (!meshes) return model;
    // EA's iPhone games hang every mesh from a group; Firemint's Real Racing
    // files have no groups at all and keep their own readers
    bool groups = false;
    for (auto& kv : nodes) if (kv.second.type == 9 || kv.second.type == 22) groups = true;
    if (!groups) return model;
    std::function<Mat4(uint32_t, int)> world = [&](uint32_t i, int depth) -> Mat4 {
        auto it = nodes.find(i);
        Mat4 m = it == nodes.end() ? identity() : it->second.local;
        auto pa = parent.find(i);
        if (pa != parent.end() && depth < 64) m = mul(world(pa->second, depth + 1), m);
        return m;
    };
    // the group a mesh hangs from names the part (Undercover's cars give each
    // part a user id: 20-37 the car, 126-153 its body kit)
    auto partOf = [&](uint32_t i) -> uint32_t {
        uint32_t id = nodes[i].userId;
        auto pa = parent.find(i);
        while (!id && pa != parent.end()) {
            auto it = nodes.find(pa->second);
            if (it == nodes.end()) break;
            id = it->second.userId;
            pa = parent.find(pa->second);
        }
        return id;
    };

    // ---- appearances: blending and their texture ----
    struct App { bool alpha = false, additive = false, cutout = false; int image = -1;
                 std::string external; float tcScale[2] = {1, 1}; };
    std::map<uint32_t, int> imageSlot;       // Image2D object -> model.images
    auto appearance = [&](uint32_t a) -> App {
        App ap;
        if (a == 0 || a >= objs.size() || objs[a].type != 3) return ap;
        Rd r(objs[a].data, objs[a].len);
        if (!readObject3D(r)) return ap;
        r.u8();                              // layer
        uint32_t cm = r.u32();
        r.u32();                             // fog
        r.u32();                             // polygon mode
        r.u32();                             // material
        uint32_t nt = r.u32();
        if (r.bad || nt > 8) return ap;
        if (cm && cm < objs.size() && objs[cm].type == 6) {
            Rd c(objs[cm].data, objs[cm].len);
            if (readObject3D(c)) {
                c.u8(); c.u8(); c.u8(); c.u8();          // depth test/write, colour/alpha write
                int blend = c.u8();
                int threshold = c.u8();
                if (blend == 64) ap.alpha = true;             // ALPHA
                else if (blend == 65) { ap.alpha = ap.additive = true; }   // ALPHA_ADD
                if (threshold > 0) ap.cutout = true;
            }
        }
        for (uint32_t k = 0; k < nt && !r.bad; ++k) {
            uint32_t t = r.u32();
            if (k > 0 || t == 0 || t >= objs.size() || objs[t].type != 17) continue;
            Rd tr(objs[t].data, objs[t].len);
            Mat4 tm;
            if (!readObject3D(tr) || !readTransformable(tr, tm)) continue;
            uint32_t img = tr.u32();
            if (tr.bad || img == 0 || img >= objs.size()) continue;
            if (objs[img].type == 255) {
                const uint8_t* u = objs[img].data;
                size_t n = 0;
                while (n < objs[img].len && u[n]) ++n;
                ap.external = std::string((const char*)u, n);
                continue;
            }
            if (objs[img].type != 10) continue;
            auto known = imageSlot.find(img);
            if (known != imageSlot.end()) { ap.image = known->second; continue; }
            Rd ir(objs[img].data, objs[img].len);
            if (!readObject3D(ir)) continue;
            int fmt = ir.u8();
            bool mut = ir.u8() != 0;
            uint32_t w = ir.u32(), h = ir.u32();
            if (ir.bad || mut || !w || !h || w > 8192 || h > 8192) continue;
            uint32_t pal = ir.u32();
            ir.skip(pal);
            uint32_t pix = ir.u32();
            if (ir.bad || !ir.have(pix)) continue;
            Image im;
            if (!decodeM3gPixels(fmt, (int)w, (int)h, objs[img].data + ir.p, pix, im)) continue;
            // the rows as every other texture in the tool: bottom row first,
            // so the viewer's V needs no turning over
            flipImageVertically(im);
            model.images.push_back(std::move(im));
            ap.image = (int)model.images.size() - 1;
            imageSlot[img] = ap.image;
        }
        return ap;
    };
    std::map<uint32_t, App> apps;
    std::map<uint32_t, int> appOrdinal;      // the appearance's place among the file's appearances
    for (uint32_t i = 1, k = 0; i < objs.size(); ++i)
        if (objs[i].type == 3) appOrdinal[i] = (int)k++;

    // ---- the meshes ----
    std::map<uint32_t, VArr> arrays;
    auto arrayOf = [&](uint32_t i) -> const VArr* {
        if (i == 0 || i >= objs.size() || objs[i].type != 20) return nullptr;
        auto it = arrays.find(i);
        if (it != arrays.end()) return it->second.count ? &it->second : nullptr;
        VArr a;
        if (!readVertexArray(objs[i], a)) a = VArr();
        return arrays.emplace(i, std::move(a)).first->second.count ? &arrays[i] : nullptr;
    };
    int skipped = 0;
    for (auto& kv : nodes) {
        uint32_t idx = kv.first;
        NodeInfo& ni = kv.second;
        if (ni.type != 14 && ni.type != 16) continue;
        Rd r(objs[ni.vb].data, objs[ni.vb].len);
        if (!readObject3D(r)) continue;
        r.u32();                             // default colour
        uint32_t posRef = r.u32();
        float bias[3] = { r.f32(), r.f32(), r.f32() };
        float scale = r.f32();
        uint32_t nrmRef = r.u32(), colRef = r.u32();
        uint32_t sets = r.u32();
        if (r.bad || sets > 8) continue;
        uint32_t tcRef = 0;
        float tcBias[3] = { 0, 0, 0 }, tcScale = 1;
        for (uint32_t s = 0; s < sets && !r.bad; ++s) {
            uint32_t ref = r.u32();
            float b[3] = { r.f32(), r.f32(), r.f32() };
            float sc = r.f32();
            if (s == 0) { tcRef = ref; memcpy(tcBias, b, sizeof(b)); tcScale = sc; }
        }
        const VArr* pa = arrayOf(posRef);
        if (!pa || pa->cc < 3) { ++skipped; continue; }
        Mat4 W = world(idx, 0);
        const VArr* na = arrayOf(nrmRef);
        const VArr* ca = arrayOf(colRef);
        const VArr* ta = arrayOf(tcRef);
        Mesh base;
        size_t nv = (size_t)pa->count;
        base.positions.resize(nv * 3);
        for (size_t v = 0; v < nv; ++v) {
            float x = pa->v[v * pa->cc] * scale + bias[0];
            float y = pa->v[v * pa->cc + 1] * scale + bias[1];
            float z = pa->v[v * pa->cc + 2] * scale + bias[2];
            base.positions[v * 3]     = W[0] * x + W[4] * y + W[8] * z + W[12];
            base.positions[v * 3 + 1] = W[1] * x + W[5] * y + W[9] * z + W[13];
            base.positions[v * 3 + 2] = W[2] * x + W[6] * y + W[10] * z + W[14];
        }
        if (na && (size_t)na->count == nv && na->cc >= 3) {
            base.normals.resize(nv * 3);
            for (size_t v = 0; v < nv; ++v) {
                float x = na->v[v * na->cc], y = na->v[v * na->cc + 1], z = na->v[v * na->cc + 2];
                float nx = W[0] * x + W[4] * y + W[8] * z, ny = W[1] * x + W[5] * y + W[9] * z,
                      nz = W[2] * x + W[6] * y + W[10] * z;
                float l = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (l < 1e-9f) { nx = 0; ny = 1; nz = 0; l = 1; }
                base.normals[v * 3] = nx / l; base.normals[v * 3 + 1] = ny / l; base.normals[v * 3 + 2] = nz / l;
            }
        }
        if (ta && (size_t)ta->count == nv) {
            base.uvs.resize(nv * 2);
            for (size_t v = 0; v < nv; ++v) {
                base.uvs[v * 2]     = ta->v[v * ta->cc] * tcScale + tcBias[0];
                base.uvs[v * 2 + 1] = ta->v[v * ta->cc + 1] * tcScale + tcBias[1];
            }
        }
        if (ca && (size_t)ca->count == nv && ca->cs == 1) {
            base.colors.resize(nv * 4);
            for (size_t v = 0; v < nv; ++v)
                for (int c = 0; c < 4; ++c)
                    base.colors[v * 4 + c] = c < ca->cc ? ((uint8_t)(int8_t)ca->v[v * ca->cc + c]) / 255.0f : 1.0f;
        }
        uint32_t part = partOf(idx);
        for (size_t s = 0; s < ni.subs.size(); ++s) {
            uint32_t ib = ni.subs[s].first;
            if (ib == 0 || ib >= objs.size() || objs[ib].type != 11) continue;
            Mesh m = base;
            if (!readStrips(objs[ib], m.indices)) continue;
            bool ok = true;
            for (uint32_t t : m.indices) if (t >= nv) { ok = false; break; }
            if (!ok || m.indices.empty()) continue;
            auto ai = apps.find(ni.subs[s].second);
            if (ai == apps.end()) ai = apps.emplace(ni.subs[s].second, appearance(ni.subs[s].second)).first;
            const App& ap = ai->second;
            char nm[64];
            auto ord = appOrdinal.find(ni.subs[s].second);
            // the mesh's number is its group's place among the file's groups,
            // which is how a track layout names its pieces
            int piece = -1;
            float origin[3] = { 0, 0, 0 };
            {
                auto pa = parent.find(idx);
                auto g = pa == parent.end() ? nodes.end() : nodes.find(pa->second);
                if (g != nodes.end()) {
                    piece = g->second.ordinal;
                    Mat4 G = world(pa->second, 0);
                    origin[0] = G[12]; origin[1] = G[13]; origin[2] = G[14];
                }
            }
            snprintf(nm, sizeof(nm), "part_%u_mesh_%d%s_app%d", part, piece,
                     ni.subs.size() > 1 ? ("_" + std::to_string(s)).c_str() : "",
                     ord == appOrdinal.end() ? -1 : ord->second);
            m.name = nm;
            memcpy(m.origin, origin, sizeof(origin));
            snprintf(nm, sizeof(nm), "part_%u", part);
            m.part = nm;
            // the blending names the material, which is how the viewer and
            // the exporters tell opaque, cut-out and see-through parts apart
            // (blending with an alpha threshold is drawn as a cut-out: people,
            // trees and fences)
            m.material = ap.additive ? "m3g_alphaadd" : ap.cutout ? "m3g_alpha_cutout"
                       : ap.alpha ? "m3g_alpha" : "m3g_opaque";
            if (ap.image >= 0) m.texture = "#img:" + std::to_string(ap.image);
            else if (!ap.external.empty()) m.texture = ap.external;
            // one detail level (the helpers aside), so the viewer's level
            // filter leaves the shadow cards and switched-off parts out
            m.lod = ni.enabled ? "LOD00" : "HELPERS";
            for (int k = 0; k < 3; ++k) { m.bboxMin[k] = 1e30f; m.bboxMax[k] = -1e30f; }
            for (size_t v = 0; v < nv; ++v)
                for (int k = 0; k < 3; ++k) {
                    m.bboxMin[k] = std::min(m.bboxMin[k], m.positions[v * 3 + k]);
                    m.bboxMax[k] = std::max(m.bboxMax[k], m.positions[v * 3 + k]);
                }
            model.meshes.push_back(std::move(m));
        }
    }
    // the empty groups with a place of their own are locators (a car's wheels)
    for (auto& kv : nodes) {
        const NodeInfo& ni = kv.second;
        if (ni.type != 9 || !ni.children.empty() || !ni.userId) continue;
        Mat4 W = world(kv.first, 0);
        Hardpoint h;
        h.name = "node_" + std::to_string(ni.userId);
        h.pos[0] = W[12]; h.pos[1] = W[13]; h.pos[2] = W[14];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) h.basis[r * 3 + c] = W[c * 4 + r];
        model.points.push_back(h);
    }
    for (const char* mn : { "m3g_opaque", "m3g_alpha", "m3g_alphaadd", "m3g_alpha_cutout" }) {
        Material mt;
        mt.name = mn;
        mt.alphaBlend = strstr(mn, "alpha") != nullptr && !strstr(mn, "cutout");
        mt.additive = strstr(mn, "add") != nullptr;
        mt.twoSided = true;
        model.materials.push_back(mt);
    }
    if (skipped) model.warnings.push_back(std::to_string(skipped) + " mesh(es) without positions skipped");
    model.valid = !model.meshes.empty();
    return model;
}

} // namespace nfsnl

namespace nfsnl {

namespace {
int uidOf(const Mesh& m) { return m.part.compare(0, 5, "part_") == 0 ? atoi(m.part.c_str() + 5) : -1; }
int appOf(const Mesh& m) {
    size_t at = m.name.rfind("_app");
    return at == std::string::npos ? -1 : atoi(m.name.c_str() + at + 4);
}
void boxOf(Mesh& m) {
    for (int k = 0; k < 3; ++k) { m.bboxMin[k] = 1e30f; m.bboxMax[k] = -1e30f; }
    for (size_t v = 0; v + 2 < m.positions.size(); v += 3)
        for (int k = 0; k < 3; ++k) {
            m.bboxMin[k] = std::min(m.bboxMin[k], m.positions[v + k]);
            m.bboxMax[k] = std::max(m.bboxMax[k], m.positions[v + k]);
        }
}
std::string firstFound(const std::function<std::string(const std::string&)>& find, std::initializer_list<std::string> names) {
    for (const std::string& n : names) { std::string p = find(n); if (!p.empty()) return p; }
    return std::string();
}
}

// NFS Undercover / Shift / Shift 2: a car_<name>.m3g, organised the way the
// game draws it. Each part hangs from a group whose user id says what it is,
// and the numbering is shared by all three games: below 100 the car as it
// ships, 100-137 and 160-199 the body kit's versions of the same parts,
// 138-159 the bonnet scoops and spoilers the shop sells, 1000 and up Shift
// 2's extra parts. A part with no height is the shadow under the car.
//
// The game binds a texture to each of the car's appearances by what the
// appearance is for, which its parts tell: the one with the wheel arches and
// sills (25, 126) draws from texture_car_<name>_bodykit.m3g, the one with the
// racing livery shells (131, 170, 172) from ..._livery.m3g, Undercover's
// shop parts on their own from texture_spoilers.m3g and its vinyl shell
// (128, 130) from texture_vinyls_01.m3g; everything else, the body kit's
// own body included, from texture_car_<name>.m3g. Traffic and police cars
// have no texture of their own and share texture_civilian_and_cop_mastertexture.
int eaPhoneOrganizeCar(Model& car, const std::string& carName,
                       const std::function<std::string(const std::string&)>& find) {
    std::string stock = firstFound(find, { "texture_car_" + carName + ".m3g", "texture_car_" + carName + "_01.m3g" });
    std::string kit = firstFound(find, { "texture_car_" + carName + "_bodykit.m3g", "texture_car_" + carName + "_bodykit_01.m3g" });
    std::string livery = find("texture_car_" + carName + "_livery.m3g");
    std::string spoilers = find("texture_spoilers.m3g");
    std::string vinyls = find("texture_vinyls_01.m3g");
    if (stock.empty()) stock = find("texture_civilian_and_cop_mastertexture.m3g");
    if (kit.empty()) kit = stock;

    // what each appearance is for
    std::map<int, std::set<int>> uidsOf;
    for (Mesh& m : car.meshes) {
        int uid = uidOf(m);
        if (uid < 0) continue;
        if (m.bboxMax[1] - m.bboxMin[1] < 1e-3f) { m.lod = "HELPERS"; continue; }   // the shadow cards
        uidsOf[appOf(m)].insert(uid);
    }
    enum Role { CAR, BODYKIT, LIVERY, SPOILERS, VINYL };
    std::map<int, Role> role;
    for (auto& kv : uidsOf) {
        const std::set<int>& u = kv.second;
        bool allShop = true, allVinyl = true;
        for (int x : u) {
            if (x < 138 || x > 159) allShop = false;
            if (x != 128 && x != 130) allVinyl = false;
        }
        Role r = CAR;
        if (!livery.empty() && (u.count(131) || u.count(170) || (u.count(172) && !u.count(22)))) r = LIVERY;
        else if (u.count(25) || u.count(126)) r = BODYKIT;
        else if (allShop && !spoilers.empty()) r = SPOILERS;
        else if (allVinyl && !vinyls.empty()) r = VINYL;
        role[kv.first] = r;
    }

    // the kit's parts: matched to the stock part they replace by their size
    std::vector<int> stockIdx, kitIdx;
    for (size_t i = 0; i < car.meshes.size(); ++i) {
        Mesh& m = car.meshes[i];
        int uid = uidOf(m);
        if (uid < 0 || m.lod == "HELPERS") continue;
        Role r = role[appOf(m)];
        if (r == VINYL) { m.kit = '+'; m.kitSlot = "vinyl"; continue; }
        if (r == LIVERY && uid != 20) { m.kit = '+'; m.kitSlot = "livery_" + std::to_string(uid); continue; }
        // the wide arches and sills (25, and 126 the second kit's) are body
        // kit parts whatever their number: the car as it ships has none
        if (r == BODYKIT && (uid == 25 || uid == 126)) { m.kit = uid == 126 ? 'c' : 'b'; m.kitSlot = "arches"; continue; }
        if (uid < 100) stockIdx.push_back((int)i);
        else if ((uid >= 100 && uid < 138) || (uid >= 160 && uid < 200)) kitIdx.push_back((int)i);
        else { m.kit = '+'; m.kitSlot = "option_" + std::to_string(uid); }
    }
    auto close = [](const Mesh& a, const Mesh& b) {
        float d = 0;
        for (int k = 0; k < 3; ++k)
            d = std::max(d, std::max(std::fabs(a.bboxMin[k] - b.bboxMin[k]), std::fabs(a.bboxMax[k] - b.bboxMax[k])));
        return d;
    };
    std::vector<int> stockSlot(car.meshes.size(), -1);
    for (int ki : kitIdx) {
        Mesh& k = car.meshes[ki];
        int best = -1;
        float bestD = 3.0f;
        for (int si : stockIdx) {
            float d = close(k, car.meshes[si]);
            if (d < bestD) { bestD = d; best = si; }
        }
        k.kit = uidOf(k) >= 160 ? 'c' : 'b';     // 100-137 the first kit, 160-199 the second
        k.kitSlot = best >= 0 ? "slot_" + std::to_string(uidOf(car.meshes[best])) : "kit_" + std::to_string(uidOf(k));
        if (best >= 0) stockSlot[best] = 1;
    }
    for (int si : stockIdx) {
        Mesh& m = car.meshes[si];
        if (stockSlot[si] > 0) { m.kit = 'a'; m.kitSlot = "slot_" + std::to_string(uidOf(m)); }
    }
    int textured = 0;
    for (Mesh& m : car.meshes) {
        int uid = uidOf(m);
        if (uid < 0) continue;
        Role r = role.count(appOf(m)) ? role[appOf(m)] : CAR;
        const std::string& t = r == BODYKIT ? kit : r == LIVERY ? livery : r == SPOILERS ? spoilers
                             : r == VINYL ? vinyls : stock;
        // the decals and badges (20), the livery and the vinyls are cut out
        // of their texture by its alpha; drawn solid the clear parts of the
        // picture show as a rainbow of noise around the letters
        if (r == LIVERY || uid == 20) m.material = "m3g_alpha_cutout";
        if (r == VINYL) m.material = "m3g_alpha";
        if (!t.empty()) { m.texture = t; ++textured; }
    }
    if (!stock.empty())
        car.warnings.push_back("textured with " + baseName(stock) +
                               (kit != stock ? " (body kit: " + baseName(kit) + ")" : std::string()));
    return textured;
}

// The car's wheels: wheels.m3g holds every rim the games offer, each the
// same size, and the car marks where they go (node_32 front, node_34 rear,
// on the car's left; the right side is their mirror image). Undercover's
// rims draw from texture_wheels_01 / _02, Shift's from _02 and
// texture_wheels_generic, Shift 2's newer ones from _03.
int eaPhoneAttachWheels(Model& car, const Model& wheels, const std::function<std::string(const std::string&)>& find) {
    const Hardpoint* front = nullptr;
    const Hardpoint* rear = nullptr;
    for (const Hardpoint& h : car.points) {
        if (h.name == "node_32") front = &h;
        if (h.name == "node_34") rear = &h;
    }
    if (!front || !rear) return 0;
    // the rim: Shift's generic one (81) when there is one, else the first
    std::map<int, std::vector<const Mesh*>> byUid;
    for (const Mesh& m : wheels.meshes) if (uidOf(m) >= 0) byUid[uidOf(m)].push_back(&m);
    if (byUid.empty()) return 0;
    int pick = byUid.count(81) ? 81 : byUid.begin()->first;
    auto texFor = [&](int uid) -> std::string {
        if (uid >= 1100) return firstFound(find, { "texture_wheels_03.m3g", "texture_wheels_genericnew01.m3g" });
        if (uid >= 81) return firstFound(find, { "texture_wheels_generic.m3g", "texture_wheels_02.m3g" });
        if (uid >= 61) return firstFound(find, { "texture_wheels_02.m3g", "texture_wheels_01.m3g" });
        return firstFound(find, { "texture_wheels_01.m3g", "texture_wheels_02.m3g" });
    };
    std::string tex = texFor(pick);
    int placed = 0;
    for (const Hardpoint* at : { front, rear }) {
        for (int side = 0; side < 2; ++side) {
            float sx = side ? -1.0f : 1.0f;          // the rims face -x, the car's left
            for (const Mesh* src : byUid[pick]) {
                Mesh m = *src;
                for (size_t v = 0; v + 2 < m.positions.size(); v += 3) {
                    m.positions[v] = m.positions[v] * sx + at->pos[0] * sx;
                    m.positions[v + 1] += at->pos[1];
                    m.positions[v + 2] += at->pos[2];
                }
                for (size_t v = 0; v + 2 < m.normals.size(); v += 3) m.normals[v] *= sx;
                if (side)
                    for (size_t k = 0; k + 2 < m.indices.size(); k += 3) std::swap(m.indices[k + 1], m.indices[k + 2]);
                m.name = std::string("wheel_") + (at == front ? "front" : "rear") + (side ? "_right" : "_left");
                m.part = m.name;
                m.kit = 0;
                m.kitSlot.clear();
                m.lod.clear();
                if (!tex.empty()) m.texture = tex;
                boxOf(m);
                car.meshes.push_back(std::move(m));
            }
            ++placed;
        }
    }
    return placed;
}

// Shift's cockpit view: cockpit_<car>.m3g is the dashboard painted on one
// card (250) and the steering wheel (251), bonnet_<car>.m3g the bonnet seen
// from the bonnet camera (252), Cockpit_HandL / R the driver's arms. Their
// textures go by the same name: texture_cockpit_<car>, texture_steeringwheel_
// <car>, texture_bonnet_<car> (or its first livery, _01) and
// texture_cockpit_arm_01. Shift 2's windscreen cracks (100-199) are left off
// as an option, drawn from texture_cockpit_cracks.
int eaPhoneOrganizeInterior(Model& m, const std::string& stem,
                            const std::function<std::string(const std::string&)>& find) {
    std::string lower = stem;
    for (char& c : lower) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    std::string car;
    std::string cockpit, wheel, bonnet, arms, cracks;
    if (lower.compare(0, 13, "cockpit_hand") == 0) {
        arms = firstFound(find, { "texture_cockpit_arm_01.m3g" });
    } else if (lower.compare(0, 8, "cockpit_") == 0) {
        car = stem.substr(8);
        cockpit = firstFound(find, { "texture_cockpit_" + car + ".m3g" });
        wheel = firstFound(find, { "texture_steeringwheel_" + car + ".m3g" });
        cracks = firstFound(find, { "texture_cockpit_cracks.m3g", "texture_cockpit_" + car + "_crack_01.m3g" });
    } else if (lower.compare(0, 7, "bonnet_") == 0) {
        car = stem.substr(7);
        bonnet = firstFound(find, { "texture_bonnet_" + car + ".m3g", "texture_bonnet_" + car + "_01.m3g",
                                    "texture_" + car + "_bonnet.m3g", "texture_" + car + "_bonnet_01.m3g" });
    } else {
        return 0;
    }
    int n = 0;
    for (Mesh& me : m.meshes) {
        int uid = uidOf(me);
        std::string t;
        if (!arms.empty()) t = arms;
        else if (!bonnet.empty()) t = bonnet;
        else if (uid == 251) t = wheel;
        else if (uid >= 100 && uid < 200) {
            me.kit = '+';
            me.kitSlot = "cracks";
            me.material = "m3g_alpha";
            t = cracks;
        } else if (me.bboxMax[0] - me.bboxMin[0] < 0.2f && me.bboxMax[1] - me.bboxMin[1] < 0.05f) {
            me.lod = "HELPERS";                       // the camera marks
        } else t = cockpit;
        if (!t.empty()) { me.texture = t; ++n; }
    }
    return n;
}

} // namespace nfsnl

namespace nfsnl {

// Shift 2's tracks carry no textures: the game binds them by a table in its
// code. The atlases are texture_tracktex_<location>_01 / _alpha / _threshold,
// and a part's blending says which one it draws from - opaque the first,
// see-through the alpha one, cut-out the threshold one.
int eaPhoneTextureTrack(Model& track, const std::string& trackLeaf,
                        const std::function<std::string(const std::string&)>& find) {
    std::string loc = trackLeaf;
    while (!loc.empty() && loc.back() >= '0' && loc.back() <= '9') loc.pop_back();
    while (!loc.empty() && loc.back() == '_') loc.pop_back();
    bool night = false;
    size_t nt = loc.find("night");
    if (nt != std::string::npos) { night = true; loc.erase(nt, 5); }
    if (loc.empty()) return 0;
    auto pick = [&](const char* kind) -> std::string {
        std::vector<std::string> c;
        std::string base = "texture_tracktex_" + loc;
        if (night) {
            c.push_back(base + "night_" + kind + ".m3g");
            c.push_back(base + "_" + kind + "night.m3g");
            c.push_back(base + "_night_" + kind + ".m3g");
        }
        c.push_back(base + "_" + kind + ".m3g");
        for (const std::string& x : c) { std::string p = find(x); if (!p.empty()) return p; }
        return std::string();
    };
    std::string opaque = pick("01"), alpha = pick("alpha"), cut = pick("threshold");
    if (alpha.empty()) alpha = cut;
    if (cut.empty()) cut = alpha.empty() ? opaque : alpha;
    int n = 0;
    for (Mesh& m : track.meshes) {
        if (!m.texture.empty() && m.texture.compare(0, 5, "#img:") == 0) continue;
        const std::string& t = m.material == "m3g_alpha_cutout" ? cut
                             : (m.material == "m3g_alpha" || m.material == "m3g_alphaadd") ? alpha : opaque;
        if (!t.empty()) { m.texture = t; ++n; }
    }
    if (n) track.warnings.push_back(std::to_string(n) + " part(s) given the location's atlases (" +
                                    baseName(opaque) + " and its alpha / threshold versions)");
    return n;
}

} // namespace nfsnl

namespace nfsnl {

// NFS Undercover / Shift: a location's .m3g is a kit of road pieces, every one
// at the origin, and an event's layout file (<location>_<event>.bin) lays
// them out on a grid: per tile a position (x, z, in tenths of the model's
// units), a turn about the vertical in degrees, a mirror flag and the pieces
// (by their place among the file's meshes) that make up the tile.
//
//   Undercover   u8 tiles; per tile f32 x, f32 z (big-endian), u16 turn,
//                u16 mirror, u8 0, u8 kind, u8 n, n x (u16 id, u16 piece)
//   Shift        00 ff 00 ff 00 ff, u8 tiles; per tile the same head, then
//                n x (u16 id, u16 piece, u32), u8 m, m x 28 bytes, u8,
//                6 x u16 neighbours, 3 x u16, u8 q, q x 8 bytes
namespace {
float beF32(const uint8_t* p) { uint32_t v = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
                                float f; memcpy(&f, &v, 4); return f; }
int beU16(const uint8_t* p) { return p[0] << 8 | p[1]; }
bool tileHeadOk(const uint8_t* p) {
    float x = beF32(p), z = beF32(p + 4);
    int rot = beU16(p + 8), mir = beU16(p + 10);
    return std::isfinite(x) && std::isfinite(z) && std::fabs(x) < 5000 && std::fabs(z) < 5000 &&
           rot < 360 && rot % 90 == 0 && mir < 4;
}
}

namespace {
// one try at the layout: `head` extra bytes after the tile's piece count's
// place (Shift 2 has two), `ent` bytes per piece
bool readLayoutAs(const uint8_t* d, size_t n, bool shift, int head, size_t ent, std::vector<EaTile>& tiles) {
    tiles.clear();
    size_t p = shift ? 7 : 1;
    int count = n > p ? d[p - 1] : 0;
    if (!count) return false;
    for (int t = 0; t < count; ++t) {
        if (p + 15 + head > n || !tileHeadOk(d + p)) return false;
        EaTile tile;
        tile.x = beF32(d + p);
        tile.z = beF32(d + p + 4);
        tile.rot = beU16(d + p + 8);
        tile.mirror = beU16(d + p + 10);
        tile.kind = d[p + 13];
        int k = d[p + 14 + head];
        p += 15 + head;
        if (!k || p + ent * k > n) return false;
        for (int i = 0; i < k; ++i) tile.pieces.push_back(beU16(d + p + ent * i + 2));
        p += ent * k;
        if (shift) {
            if (p + 1 > n) return false;
            p += 1 + 28 * (size_t)d[p];
            if (p + 20 > n || d[p] != 1) return false;     // the links block
            p += 19;
            p += 1 + 8 * (size_t)d[p];
            if (p > n) return false;
        }
        tiles.push_back(std::move(tile));
    }
    return true;
}
}

bool eaPhoneReadLayout(const uint8_t* d, size_t n, std::vector<EaTile>& tiles) {
    static const uint8_t kShift[6] = { 0, 0xFF, 0, 0xFF, 0, 0xFF };
    if (n > 7 && memcmp(d, kShift, 6) == 0)
        return readLayoutAs(d, n, true, 0, 8, tiles) || readLayoutAs(d, n, true, 2, 9, tiles);
    return readLayoutAs(d, n, false, 0, 4, tiles);
}

int eaPhoneLayoutTrack(Model& m, const std::vector<EaTile>& tiles, float unit) {
    std::map<int, std::vector<size_t>> byPiece;
    for (size_t i = 0; i < m.meshes.size(); ++i) {
        size_t at = m.meshes[i].name.find("_mesh_");
        if (at == std::string::npos) continue;
        byPiece[atoi(m.meshes[i].name.c_str() + at + 6)].push_back(i);
    }
    std::vector<Mesh> out;
    int placed = 0;
    for (size_t t = 0; t < tiles.size(); ++t) {
        const EaTile& tl = tiles[t];
        int rotDeg = tl.rot;
        float a = rotDeg * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a);
        float sx = (tl.mirror & 1) ? -1.0f : 1.0f, sz = (tl.mirror & 2) ? -1.0f : 1.0f;
        for (int piece : tl.pieces) {
            auto it = byPiece.find(piece);
            if (it == byPiece.end()) continue;
            for (size_t mi : it->second) {
                Mesh me = m.meshes[mi];
                // a piece the file keeps far from the origin (Shift's Tokyo
                // keeps some 6 km out) is brought home first: the tile places it
                float far = std::sqrt(me.origin[0] * me.origin[0] + me.origin[2] * me.origin[2]);
                if (far > 1000.0f)
                    for (size_t k = 0; k + 2 < me.positions.size(); k += 3) {
                        me.positions[k] -= me.origin[0];
                        me.positions[k + 1] -= me.origin[1];
                        me.positions[k + 2] -= me.origin[2];
                    }
                // a stray marker still kilometres out once home (Tokyo has
                // one) belongs to no tile
                {
                    float cx = 0, cz = 0;
                    size_t nv = me.positions.size() / 3;
                    for (size_t k = 0; k < nv; ++k) { cx += me.positions[k * 3]; cz += me.positions[k * 3 + 2]; }
                    if (nv && std::sqrt(cx * cx + cz * cz) / nv > 1500.0f) continue;
                }
                // turned about the vertical (x towards -z for a positive
                // angle), then mirrored across the world's axes; measured by
                // how well the road pieces of every layout meet
                auto turn = [&](float* v, bool point) {
                    float x = v[0], z = v[2];
                    float X = (c * x - s * z) * sx, Z = (s * x + c * z) * sz;
                    v[0] = X + (point ? tl.x * unit : 0);
                    v[2] = Z + (point ? tl.z * unit : 0);
                };
                for (size_t k = 0; k + 2 < me.positions.size(); k += 3) turn(&me.positions[k], true);
                for (size_t k = 0; k + 2 < me.normals.size(); k += 3) turn(&me.normals[k], false);
                if ((tl.mirror & 1) != (tl.mirror >> 1 & 1))      // a mirrored piece turns its triangles inside out
                    for (size_t k = 0; k + 2 < me.indices.size(); k += 3) std::swap(me.indices[k + 1], me.indices[k + 2]);
                me.name += "_tile" + std::to_string(t);
                for (int k = 0; k < 3; ++k) { me.bboxMin[k] = 1e30f; me.bboxMax[k] = -1e30f; }
                for (size_t k = 0; k + 2 < me.positions.size(); k += 3)
                    for (int q = 0; q < 3; ++q) {
                        me.bboxMin[q] = std::min(me.bboxMin[q], me.positions[k + q]);
                        me.bboxMax[q] = std::max(me.bboxMax[q], me.positions[k + q]);
                    }
                out.push_back(std::move(me));
            }
            ++placed;
        }
    }
    if (!placed) return 0;
    m.meshes.swap(out);
    return placed;
}

} // namespace nfsnl

namespace nfsnl {

namespace {
// every appearance's blending, in file order: 0 opaque, 1 see-through,
// 2 additive, 3 cut-out (see-through with an alpha threshold)
std::vector<int> appearanceKinds(const uint8_t* data, size_t len) {
    std::vector<int> out;
    Bytes gz;
    if (len > 18 && data[0] == 0x1F && data[1] == 0x8B && inflateGzip(data, len, gz)) { data = gz.data(); len = gz.size(); }
    if (len < 12 || data[0] != 0xAB) return out;
    std::vector<Bytes> sections;
    std::vector<Obj> objs(1);
    size_t p = 12;
    while (p + 13 <= len) {
        uint32_t total;
        memcpy(&total, data + p + 1, 4);
        if (total < 13 || p + total > len) break;
        const uint8_t* body = data + p + 9;
        size_t bodyLen = total - 13;
        if (data[p]) {
            sections.emplace_back();
            if (!inflateZlib(body, bodyLen, sections.back())) break;
            body = sections.back().data();
            bodyLen = sections.back().size();
        }
        for (size_t q = 0; q + 5 <= bodyLen;) {
            Obj o;
            o.type = body[q];
            uint32_t l;
            memcpy(&l, body + q + 1, 4);
            if (q + 5 + (size_t)l > bodyLen) break;
            o.data = body + q + 5;
            o.len = l;
            objs.push_back(o);
            q += 5 + l;
        }
        p += total;
    }
    for (size_t i = 1; i < objs.size(); ++i) {
        if (objs[i].type != 3) continue;
        int kind = 0;
        Rd r(objs[i].data, objs[i].len);
        if (readObject3D(r)) {
            r.u8();
            uint32_t cm = r.u32();
            if (cm && cm < objs.size() && objs[cm].type == 6) {
                Rd c(objs[cm].data, objs[cm].len);
                if (readObject3D(c)) {
                    c.u8(); c.u8(); c.u8(); c.u8();
                    int blend = c.u8(), threshold = c.u8();
                    kind = blend == 65 ? 2 : (blend == 64 && threshold > 0) || (threshold > 0) ? 3 : blend == 64 ? 1 : 0;
                }
            }
        }
        out.push_back(kind);
    }
    return out;
}

// texturelist_<track>.bin: u8 count, then per appearance u8 kind, u8,
// u16 (big-endian) index into a table the game builds, u8, u8
std::vector<int> readTextureList(const Bytes& b) {
    std::vector<int> out;
    if (b.empty() || b.size() < 1 + 6 * (size_t)b[0]) return out;
    for (int i = 0; i < b[0]; ++i) out.push_back(beU16(b.data() + 1 + 6 * i + 2));
    return out;
}

std::string trackBase(const std::string& leaf) {
    std::string loc = leaf;
    while (!loc.empty() && loc.back() >= '0' && loc.back() <= '9') loc.pop_back();
    while (!loc.empty() && loc.back() == '_') loc.pop_back();
    size_t nt = loc.find("night");
    if (nt != std::string::npos) loc.erase(nt, 5);
    return loc;
}
}

// Shift 2's tracks name their textures by number: texturelist_<track>.bin
// gives each appearance an index into a table the game fills as it reads
// every track's list in turn (texturelist_*.bin in name order, lightmap lists
// included), a new number for each texture it has not seen. Which texture a
// new number is follows from the appearance that first used it: the first
// appearance of a track is its road (the location's _alpha atlas), the
// cut-out one the people and trees (_threshold), a plain see-through one the
// shared tyre marks and headlight pools, and the rest the numbered atlases
// (_01, _02 ..., the night versions once the day ones are taken).
int eaPhoneTextureTrackLists(Model& track, const std::string& trackLeaf, const std::vector<std::string>& leaves,
                             const std::function<std::string(const std::string&)>& find,
                             const std::function<bool(const std::string&, Bytes&)>& read) {
    std::vector<std::string> lists;
    for (const std::string& l : leaves)
        if (l.compare(0, 12, "texturelist_") == 0 && l.size() > 16 && l.compare(l.size() - 4, 4, ".bin") == 0)
            lists.push_back(l);
    std::sort(lists.begin(), lists.end());
    lists.erase(std::unique(lists.begin(), lists.end()), lists.end());
    std::string mine = "texturelist_" + trackLeaf + ".bin";
    if (!std::binary_search(lists.begin(), lists.end(), mine)) return 0;

    // the location's atlases, sorted into the kinds the rule hands out
    std::map<std::string, std::map<std::string, std::vector<std::string>>> pools;   // base -> kind -> names
    auto poolOf = [&](const std::string& base) -> std::map<std::string, std::vector<std::string>>& {
        auto it = pools.find(base);
        if (it != pools.end()) return it->second;
        auto& pool = pools[base];
        std::string pre = "texture_tracktex_" + base;
        std::vector<std::string> day, night;
        for (const std::string& l : leaves) {
            if (l.compare(0, pre.size(), pre) != 0 || l.size() < 4 || l.compare(l.size() - 4, 4, ".m3g") != 0) continue;
            std::string rest = l.substr(pre.size(), l.size() - 4 - pre.size());   // _01, _alphanight, night_02 ...
            bool isNight = rest.find("night") != std::string::npos;
            std::string kind = rest.find("threshold") != std::string::npos ? "threshold"
                             : rest.find("alpha") != std::string::npos && rest.find("overlay") == std::string::npos ? "road"
                             : rest.find_first_of("0123456789") != std::string::npos ? "numbered" : "extra";
            pool[kind + (isNight ? "1" : "0")].push_back(l);
        }
        for (auto& kv : pool) std::sort(kv.second.begin(), kv.second.end());
        for (const char* k : { "road", "threshold", "numbered", "extra" }) {
            auto& all = pool[k];
            for (const char* d : { "0", "1" }) {
                auto& v = pool[std::string(k) + d];
                all.insert(all.end(), v.begin(), v.end());
            }
        }
        return pool;
    };
    std::map<std::string, size_t> taken;            // base + kind -> how many handed out
    std::vector<std::string> shared = { "texture_TyreMarks.m3g", "texture_headlights_road.m3g" };
    size_t sharedTaken = 0;
    std::map<int, std::string> names;
    std::set<int> seen;
    std::vector<int> want;
    for (const std::string& l : lists) {
        Bytes b;
        std::string path = find(l);
        if (path.empty() || !read(path, b)) continue;
        std::vector<int> idx = readTextureList(b);
        bool lightmaps = l.find("lightMap") != std::string::npos;
        std::string leaf = l.substr(12, l.size() - 16);
        if (l == mine) want = idx;
        bool fresh = false;
        for (int v : idx) if (!seen.count(v)) fresh = true;
        if (!fresh) continue;
        std::vector<int> kinds;
        if (!lightmaps) {
            Bytes mb;
            std::string mp = find(leaf + ".m3g");
            if (!mp.empty() && read(mp, mb)) kinds = appearanceKinds(mb.data(), mb.size());
        }
        std::string base = trackBase(leaf);
        for (size_t k = 0; k < idx.size(); ++k) {
            int v = idx[k];
            if (seen.count(v)) continue;
            seen.insert(v);
            if (lightmaps) continue;
            int kind = k < kinds.size() ? kinds[k] : 0;
            auto hand = [&](const std::string& what) -> std::string {
                auto& pool = poolOf(base)[what];
                size_t& n = taken[base + "|" + what];
                return n < pool.size() ? pool[n++] : std::string();
            };
            std::string name;
            if (k == 0) name = hand("road");
            else if (kind == 3) name = hand("threshold");
            else if (kind == 1 && sharedTaken < shared.size()) name = shared[sharedTaken++];
            if (name.empty()) name = hand("numbered");
            if (name.empty()) name = hand("extra");
            if (!name.empty()) names[v] = name;
        }
    }
    if (want.empty()) return 0;
    int n = 0;
    for (Mesh& m : track.meshes) {
        size_t at = m.name.rfind("_app");
        if (at == std::string::npos) continue;
        int app = atoi(m.name.c_str() + at + 4);
        if (app < 0 || app >= (int)want.size()) continue;
        auto it = names.find(want[app]);
        if (it == names.end()) continue;
        std::string p = find(it->second);
        if (p.empty()) continue;
        m.texture = p;
        ++n;
    }
    if (n) track.warnings.push_back(std::to_string(n) + " part(s) textured as texturelist_" + trackLeaf + ".bin says");
    return n;
}

} // namespace nfsnl


namespace nfsnl {

// Layout files for a location: <loc>_<NN>.bin for a numbered track
// (Shift 2's locationc01 -> locationc_01.bin), else every <loc>_*.bin
std::vector<std::string> eaPhoneLayoutCandidates(const std::string& stem, const std::vector<std::string>& leaves) {
    std::vector<std::string> out;
    size_t d = stem.size();
    while (d > 0 && stem[d - 1] >= '0' && stem[d - 1] <= '9') --d;
    if (d < stem.size() && d > 0) {
        std::string want = stem.substr(0, d) + "_" + stem.substr(d) + ".bin";
        for (const std::string& l : leaves) if (l == want) out.push_back(l);
        return out;
    }
    std::string pre = stem + "_";
    for (const std::string& l : leaves)
        if (l.size() > pre.size() + 4 && l.compare(0, pre.size(), pre) == 0 && l.compare(l.size() - 4, 4, ".bin") == 0 &&
            l.find("lightMap") == std::string::npos && l.find("lightSamples") == std::string::npos)
            out.push_back(l);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> eaPhoneDress(Model& m, const std::string& stem, const EaPhoneFiles& f,
                                      const std::string& layout) {
    std::vector<std::string> said;
    std::string lower = stem;
    for (char& c : lower) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (lower.compare(0, 4, "car_") == 0) {
        int n = eaPhoneOrganizeCar(m, stem.substr(4), f.find);
        said.push_back(std::to_string(n) + " part(s) given the car's textures");
        std::string wp = f.find("wheels.m3g");
        Bytes wb;
        if (!wp.empty() && f.read(wp, wb)) {
            Model w = loadJsr184Scene(wb.data(), wb.size());
            int k = eaPhoneAttachWheels(m, w, f.find);
            if (k) said.push_back(std::to_string(k) + " wheel(s) from wheels.m3g");
        }
        return said;
    }
    if (lower.compare(0, 8, "cockpit_") == 0 || lower.compare(0, 7, "bonnet_") == 0) {
        said.push_back(std::to_string(eaPhoneOrganizeInterior(m, stem, f.find)) + " part(s) given the cockpit's textures");
        return said;
    }
    // the props by the road (cones, signs, barriers) share the traffic's texture
    if (lower == "objects") {
        std::string t = f.find("texture_civilian_and_cop_mastertexture.m3g");
        int n = 0;
        if (!t.empty()) for (Mesh& me : m.meshes) if (me.texture.empty()) { me.texture = t; ++n; }
        said.push_back(std::to_string(n) + " prop(s) given texture_civilian_and_cop_mastertexture.m3g");
        return said;
    }
    // the sky: skydome_<location>.m3g draws texture_skyline_<location>;
    // Undercover's skydomes.m3g holds every location's dome, one appearance
    // each, in the order of their names
    if (lower.compare(0, 7, "skydome") == 0) {
        std::vector<std::string> skies;
        if (lower == "skydomes") {
            for (const std::string& l : f.leaves)
                if (l.compare(0, 16, "texture_skyline_") == 0 && l.find("lightning") == std::string::npos) skies.push_back(l);
            std::sort(skies.begin(), skies.end());
        } else {
            std::string loc = stem.substr(stem.find('_') + 1);
            if (loc.compare(0, 3, "loc") == 0 && loc.compare(0, 8, "location") != 0) loc = "location" + loc.substr(3);
            skies.push_back("texture_skyline_" + loc + ".m3g");
        }
        int n = 0;
        for (Mesh& me : m.meshes) {
            int a = appOf(me);
            std::string p = a >= 0 && a < (int)skies.size() ? f.find(skies[a]) : std::string();
            if (lower != "skydomes" && !skies.empty()) p = f.find(skies[0]);
            if (!p.empty()) { me.texture = p; ++n; }
        }
        said.push_back(std::to_string(n) + " part(s) given the sky's texture");
        return said;
    }
    // a location: its road pieces laid out, then textured
    std::vector<std::string> cand;
    if (!layout.empty()) cand.push_back(layout);
    else cand = eaPhoneLayoutCandidates(stem, f.leaves);
    std::vector<EaTile> best;
    std::string bestName;
    for (const std::string& c : cand) {
        std::string p = f.find(c);
        Bytes b;
        std::vector<EaTile> t;
        if (p.empty() || !f.read(p, b) || !eaPhoneReadLayout(b.data(), b.size(), t)) continue;
        // the most complete layout; a test layout only when there is no other
        bool test = c.find("_test") != std::string::npos, bestTest = bestName.find("_test") != std::string::npos;
        if (best.empty() || (bestTest && !test) || (test == bestTest && t.size() > best.size())) { best.swap(t); bestName = c; }
    }
    if (!best.empty()) {
        int n = eaPhoneLayoutTrack(m, best);
        if (n) said.push_back(std::to_string(n) + " road piece(s) laid out as " + bestName + " has them");
    }
    if (m.images.empty()) {
        int n = eaPhoneTextureTrackLists(m, stem, f.leaves, f.find, f.read);
        if (!n) n = eaPhoneTextureTrack(m, stem, f.find);
        if (n) said.push_back(std::to_string(n) + " part(s) given the location's textures");
    }
    for (const std::string& x : said) m.warnings.push_back(x);
    return said;
}

} // namespace nfsnl
