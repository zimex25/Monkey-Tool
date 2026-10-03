// Model import: OBJ and FBX (binary or ASCII) read into a Model, and a Real
// Racing 3 .m3g rewritten around it - the car mod path.
//
// A Real Racing 3 car model is a flat JSR-184 file: a header section, then
// one section of vertex arrays (20), vertex buffers (21), index buffers (11)
// and meshes (14), plus Firemint's material name list (24). There is no scene
// graph - the game finds each mesh by its name (LOD_A_BODY_mm_ext) - so a mesh
// is replaced by rewriting its own arrays and index buffer in place, and a new
// one is added by appending arrays, a buffer, indices and a mesh cloned from
// one that shares its material. Every object still refers only to objects
// before it, the section lengths, the header's file size and the Adler-32
// checksums are recomputed, and the file loads the way the game wrote it.

#include "nfsnl.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <tuple>
#include <unordered_map>

namespace nfsnl {

namespace {

std::string lowerStr(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

void computeBounds(Mesh& m) {
    for (int k = 0; k < 3; ++k) { m.bboxMin[k] = 1e30f; m.bboxMax[k] = -1e30f; }
    for (size_t i = 0; i + 2 < m.positions.size(); i += 3)
        for (int k = 0; k < 3; ++k) {
            m.bboxMin[k] = std::min(m.bboxMin[k], m.positions[i + k]);
            m.bboxMax[k] = std::max(m.bboxMax[k], m.positions[i + k]);
        }
    if (m.positions.empty())
        for (int k = 0; k < 3; ++k) m.bboxMin[k] = m.bboxMax[k] = 0;
}

// face normals summed onto the corners, for a mesh that came without any
void fillNormals(Mesh& m) {
    size_t nv = m.positions.size() / 3;
    if (m.normals.size() == nv * 3) return;
    m.normals.assign(nv * 3, 0.0f);
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        const float* a = &m.positions[m.indices[i] * 3];
        const float* b = &m.positions[m.indices[i + 1] * 3];
        const float* c = &m.positions[m.indices[i + 2] * 3];
        float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
        float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
        float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        for (int k = 0; k < 3; ++k)
            for (int j = 0; j < 3; ++j) m.normals[m.indices[i + k] * 3 + j] += n[j];
    }
    for (size_t i = 0; i < nv; ++i) {
        float* n = &m.normals[i * 3];
        float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l > 1e-12f) { n[0] /= l; n[1] /= l; n[2] /= l; } else { n[1] = 1; }
    }
}

// One vertex per distinct (position, uv, normal) corner, the way every game
// format wants it; OBJ and FBX index each attribute on its own.
struct Welder {
    Mesh& mesh;
    std::map<std::tuple<long long, long long, long long, long long>, uint32_t> seen;
    explicit Welder(Mesh& m) : mesh(m) {}
    uint32_t add(long long pi, long long ti, long long ni, long long ci,
                 const float* p, const float* uv, const float* n, const float* col, const float* uv2) {
        auto key = std::make_tuple(pi, ti, ni, ci);
        auto it = seen.find(key);
        if (it != seen.end()) return it->second;
        uint32_t idx = (uint32_t)(mesh.positions.size() / 3);
        mesh.positions.insert(mesh.positions.end(), p, p + 3);
        if (uv) mesh.uvs.insert(mesh.uvs.end(), uv, uv + 2);
        if (n) mesh.normals.insert(mesh.normals.end(), n, n + 3);
        if (col) mesh.colors.insert(mesh.colors.end(), col, col + 4);
        if (uv2) mesh.uvs2.insert(mesh.uvs2.end(), uv2, uv2 + 2);
        seen[key] = idx;
        return idx;
    }
};

void finishMesh(Model& model, Mesh& m) {
    size_t nv = m.positions.size() / 3;
    if (m.uvs.size() != nv * 2) m.uvs.clear();
    if (m.uvs2.size() != nv * 2) m.uvs2.clear();
    if (m.colors.size() != nv * 4) m.colors.clear();
    if (m.normals.size() != nv * 3) m.normals.clear();
    if (m.indices.empty() || !nv) return;
    fillNormals(m);
    computeBounds(m);
    model.meshes.push_back(std::move(m));
}

// ------------------------------------------------------------------ OBJ

Model readObj(const uint8_t* data, size_t len) {
    Model model;
    std::vector<float> v, vt, vn, vc;
    Mesh cur;
    std::string curMaterial, groupName, curLod;
    bool named = false;
    std::unique_ptr<Welder> weld(new Welder(cur));
    auto flush = [&]() {
        if (!cur.indices.empty()) {
            if (cur.name.empty()) cur.name = groupName.empty() ? "mesh" : groupName;
            finishMesh(model, cur);
        }
        cur = Mesh();
        weld.reset(new Welder(cur));
        cur.material = curMaterial;
        cur.lod = curLod;
    };
    size_t p = 0;
    std::vector<long long> fp, ft, fn;
    while (p < len) {
        size_t e = p;
        while (e < len && data[e] != '\n' && data[e] != '\r') ++e;
        std::string line((const char*)data + p, e - p);
        p = e;
        while (p < len && (data[p] == '\n' || data[p] == '\r')) ++p;
        size_t s = 0;
        while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
        if (s >= line.size() || line[s] == '#') continue;
        size_t k = s;
        while (k < line.size() && line[k] != ' ' && line[k] != '\t') ++k;
        std::string key = line.substr(s, k - s);
        std::string rest = k < line.size() ? line.substr(k + 1) : std::string();
        while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t')) rest.pop_back();
        size_t rs = 0;
        while (rs < rest.size() && (rest[rs] == ' ' || rest[rs] == '\t')) ++rs;
        rest = rest.substr(rs);
        if (key == "v") {
            float x = 0, y = 0, z = 0, r = -1, g = -1, b = -1;
            int got = sscanf(rest.c_str(), "%f %f %f %f %f %f", &x, &y, &z, &r, &g, &b);
            v.push_back(x); v.push_back(y); v.push_back(z);
            if (got >= 6) { vc.push_back(r); vc.push_back(g); vc.push_back(b); vc.push_back(1); }
            else { vc.push_back(-1); vc.push_back(-1); vc.push_back(-1); vc.push_back(-1); }
        } else if (key == "vt") {
            float a = 0, b = 0;
            sscanf(rest.c_str(), "%f %f", &a, &b);
            vt.push_back(a); vt.push_back(b);
        } else if (key == "vn") {
            float a = 0, b = 0, c = 0;
            sscanf(rest.c_str(), "%f %f %f", &a, &b, &c);
            vn.push_back(a); vn.push_back(b); vn.push_back(c);
        } else if (key == "o") {
            flush();
            cur.name = rest;
            named = true;
        } else if (key == "g") {
            // the tool's own OBJ groups by "LOD00/part" and names meshes with
            // "o"; a file without "o" names its meshes by group
            groupName = rest;
            if (!named) { flush(); cur.name = rest; }
            // the tool's own OBJ: "g LOD01/part" says the detail level
            size_t sl = rest.find('/');
            curLod = sl != std::string::npos ? rest.substr(0, sl) : std::string();
            cur.lod = curLod;
        } else if (key == "usemtl") {
            if (!cur.indices.empty() && rest != cur.material) {
                std::string keep = cur.name;
                flush();
                cur.name = keep;
            }
            curMaterial = rest;
            cur.material = rest;
        } else if (key == "f") {
            fp.clear(); ft.clear(); fn.clear();
            size_t q = 0;
            while (q < rest.size()) {
                while (q < rest.size() && (rest[q] == ' ' || rest[q] == '\t')) ++q;
                if (q >= rest.size()) break;
                size_t w = q;
                while (w < rest.size() && rest[w] != ' ' && rest[w] != '\t') ++w;
                std::string tok = rest.substr(q, w - q);
                q = w;
                long long a[3] = { 0, 0, 0 };
                int slot = 0;
                size_t t = 0;
                while (t <= tok.size() && slot < 3) {
                    size_t sl = tok.find('/', t);
                    if (sl == std::string::npos) sl = tok.size();
                    if (sl > t) a[slot] = atoll(tok.substr(t, sl - t).c_str());
                    ++slot;
                    t = sl + 1;
                }
                auto fix = [](long long i, size_t n) -> long long {
                    if (i > 0) return i - 1;
                    if (i < 0) return (long long)n + i;
                    return -1;
                };
                fp.push_back(fix(a[0], v.size() / 3));
                ft.push_back(fix(a[1], vt.size() / 2));
                fn.push_back(fix(a[2], vn.size() / 3));
            }
            if (fp.size() < 3) continue;
            std::vector<uint32_t> corner;
            for (size_t c = 0; c < fp.size(); ++c) {
                if (fp[c] < 0 || (size_t)fp[c] * 3 + 2 >= v.size() + 0 || (size_t)fp[c] >= v.size() / 3) { corner.clear(); break; }
                const float* uv = (ft[c] >= 0 && (size_t)ft[c] < vt.size() / 2) ? &vt[ft[c] * 2] : nullptr;
                const float* n = (fn[c] >= 0 && (size_t)fn[c] < vn.size() / 3) ? &vn[fn[c] * 3] : nullptr;
                const float* col = vc[fp[c] * 4] >= 0 ? &vc[fp[c] * 4] : nullptr;
                corner.push_back(weld->add(fp[c], uv ? ft[c] : -1, n ? fn[c] : -1, -1,
                                           &v[fp[c] * 3], uv, n, col, nullptr));
            }
            for (size_t c = 1; c + 1 < corner.size(); ++c) {
                cur.indices.push_back(corner[0]);
                cur.indices.push_back(corner[c]);
                cur.indices.push_back(corner[c + 1]);
            }
        }
    }
    flush();
    model.valid = !model.meshes.empty();
    if (!model.valid) model.warnings.push_back("the OBJ file has no faces");
    return model;
}

// ------------------------------------------------------------------ FBX

struct FNode {
    std::string name;
    std::vector<double> nums;          // every numeric property and array, in order
    std::vector<std::string> strs;     // every string property, in order
    std::vector<long long> ids;        // integer properties (object ids), in order
    std::vector<FNode> kids;
    const FNode* child(const char* n) const {
        for (const FNode& k : kids) if (k.name == n) return &k;
        return nullptr;
    }
    // an array's values, which ASCII keeps in a child "a"
    const std::vector<double>& values() const {
        if (nums.empty()) { const FNode* a = child("a"); if (a) return a->nums; }
        return nums;
    }
    std::string str(size_t i) const { return i < strs.size() ? strs[i] : std::string(); }
};

bool fbxBinaryNode(const uint8_t* d, size_t len, size_t& p, bool wide, FNode& out, bool& end, int depth) {
    end = false;
    size_t hdr = wide ? 25 : 13;
    if (p + hdr > len || depth > 64) return false;
    uint64_t endOff, nProps, propLen;
    if (wide) { memcpy(&endOff, d + p, 8); memcpy(&nProps, d + p + 8, 8); memcpy(&propLen, d + p + 16, 8); }
    else {
        uint32_t a, b, c; memcpy(&a, d + p, 4); memcpy(&b, d + p + 4, 4); memcpy(&c, d + p + 8, 4);
        endOff = a; nProps = b; propLen = c;
    }
    uint8_t nameLen = d[p + hdr - 1];
    if (endOff == 0) { p += hdr; end = true; return true; }
    if (endOff > len || p + hdr + nameLen > len) return false;
    out.name.assign((const char*)d + p + hdr, nameLen);
    size_t q = p + hdr + nameLen;
    size_t propsEnd = q + (size_t)propLen;
    if (propsEnd > len) return false;
    for (uint64_t i = 0; i < nProps && q < propsEnd; ++i) {
        char t = (char)d[q++];
        auto rd = [&](void* dst, size_t n) { if (q + n > len) return false; memcpy(dst, d + q, n); q += n; return true; };
        switch (t) {
        case 'Y': { int16_t v; if (!rd(&v, 2)) return false; out.nums.push_back(v); out.ids.push_back(v); break; }
        case 'C': { uint8_t v; if (!rd(&v, 1)) return false; out.nums.push_back(v); break; }
        case 'I': { int32_t v; if (!rd(&v, 4)) return false; out.nums.push_back(v); out.ids.push_back(v); break; }
        case 'F': { float v; if (!rd(&v, 4)) return false; out.nums.push_back(v); break; }
        case 'D': { double v; if (!rd(&v, 8)) return false; out.nums.push_back(v); break; }
        case 'L': { int64_t v; if (!rd(&v, 8)) return false; out.nums.push_back((double)v); out.ids.push_back(v); break; }
        case 'S': case 'R': {
            uint32_t n; if (!rd(&n, 4) || q + n > len) return false;
            if (t == 'S') out.strs.emplace_back((const char*)d + q, n);
            q += n;
            break;
        }
        case 'f': case 'd': case 'l': case 'i': case 'b': {
            uint32_t count, enc, comp;
            if (!rd(&count, 4) || !rd(&enc, 4) || !rd(&comp, 4) || q + comp > len) return false;
            size_t esz = (t == 'd' || t == 'l') ? 8 : (t == 'b' ? 1 : 4);
            Bytes raw;
            const uint8_t* src = d + q;
            if (enc == 1) {
                if (!inflateZlib(d + q, comp, raw)) return false;
                src = raw.data();
                if (raw.size() < (size_t)count * esz) return false;
            } else if ((size_t)count * esz > comp) return false;
            q += comp;
            out.nums.reserve(out.nums.size() + count);
            for (uint32_t k = 0; k < count; ++k) {
                const uint8_t* e = src + (size_t)k * esz;
                double v = 0;
                if (t == 'f') { float f; memcpy(&f, e, 4); v = f; }
                else if (t == 'd') { memcpy(&v, e, 8); }
                else if (t == 'l') { int64_t x; memcpy(&x, e, 8); v = (double)x; }
                else if (t == 'i') { int32_t x; memcpy(&x, e, 4); v = x; }
                else v = *e;
                out.nums.push_back(v);
            }
            break;
        }
        default: return false;
        }
    }
    q = propsEnd;
    while (q < endOff) {
        FNode kid;
        bool kidEnd;
        if (!fbxBinaryNode(d, len, q, wide, kid, kidEnd, depth + 1)) return false;
        if (kidEnd) break;
        out.kids.push_back(std::move(kid));
    }
    p = (size_t)endOff;
    return true;
}

bool fbxBinary(const uint8_t* d, size_t len, FNode& root) {
    if (len < 27 || memcmp(d, "Kaydara FBX Binary", 18) != 0) return false;
    uint32_t version; memcpy(&version, d + 23, 4);
    bool wide = version >= 7500;
    size_t p = 27;
    while (p < len) {
        FNode n;
        bool end;
        if (!fbxBinaryNode(d, len, p, wide, n, end, 0)) break;
        if (end) break;
        root.kids.push_back(std::move(n));
    }
    return !root.kids.empty();
}

// ASCII FBX: "Name: props {children}" with ";" comments and "*N { a: ... }"
// arrays
struct FbxText {
    const char* s; size_t n, p = 0;
    void spaces(bool lines) {
        while (p < n) {
            char c = s[p];
            if (c == ';') { while (p < n && s[p] != '\n') ++p; continue; }
            if (c == ' ' || c == '\t' || c == '\r' || (lines && c == '\n')) { ++p; continue; }
            break;
        }
    }
    bool node(FNode& out, int depth) {
        if (depth > 64) return false;
        spaces(true);
        size_t b = p;
        while (p < n && s[p] != ':' && s[p] != '{' && s[p] != '}' && s[p] != '\n') ++p;
        if (p >= n || s[p] != ':') return false;
        out.name.assign(s + b, p - b);
        while (!out.name.empty() && (out.name.back() == ' ' || out.name.back() == '\t')) out.name.pop_back();
        ++p;
        for (;;) {
            spaces(false);
            if (p >= n || s[p] == '\n' || s[p] == '}') break;
            if (s[p] == '{') break;
            if (s[p] == '"') {
                size_t e = ++p;
                while (e < n && s[e] != '"') ++e;
                out.strs.emplace_back(s + p, e - p);
                p = e < n ? e + 1 : n;
            } else if (s[p] == '*') {
                ++p;
                while (p < n && s[p] >= '0' && s[p] <= '9') ++p;
            } else {
                size_t e = p;
                while (e < n && s[e] != ',' && s[e] != '\n' && s[e] != '{' && s[e] != '}' && s[e] != ' ' && s[e] != '\r') ++e;
                std::string tok(s + p, e - p);
                p = e;
                if (!tok.empty()) {
                    char* endp = nullptr;
                    double v = strtod(tok.c_str(), &endp);
                    if (endp && *endp == 0) {
                        out.nums.push_back(v);
                        if (tok.find_first_of(".eE") == std::string::npos) out.ids.push_back(atoll(tok.c_str()));
                    } else out.strs.push_back(tok);
                }
            }
            spaces(false);
            if (p < n && s[p] == ',') { ++p; spaces(true); continue; }
        }
        if (p < n && s[p] == '{') {
            ++p;
            for (;;) {
                spaces(true);
                if (p >= n) return false;
                if (s[p] == '}') { ++p; break; }
                FNode kid;
                size_t before = p;
                if (!node(kid, depth + 1)) {
                    // skip a line the parser does not follow
                    p = before;
                    while (p < n && s[p] != '\n') ++p;
                    continue;
                }
                out.kids.push_back(std::move(kid));
            }
        }
        return true;
    }
};

bool fbxAscii(const uint8_t* d, size_t len, FNode& root) {
    FbxText t{ (const char*)d, len };
    for (;;) {
        t.spaces(true);
        if (t.p >= t.n) break;
        FNode n;
        size_t before = t.p;
        if (!t.node(n, 0)) {
            t.p = before;
            while (t.p < t.n && t.s[t.p] != '\n') ++t.p;
            continue;
        }
        root.kids.push_back(std::move(n));
    }
    return !root.kids.empty();
}

std::string fbxObjectName(const std::string& raw) {
    // binary: "Name\0\1Model"; ASCII: "Model::Name"
    size_t z = raw.find('\0');
    if (z != std::string::npos) return raw.substr(0, z);
    size_t c = raw.find("::");
    if (c != std::string::npos) return raw.substr(c + 2);
    return raw;
}

struct Xform { double t[3] = {0, 0, 0}, r[3] = {0, 0, 0}, s[3] = {1, 1, 1}; };

void applyXform(const Xform& x, float* p, bool direction) {
    double v[3] = { p[0], p[1], p[2] };
    if (!direction) for (int k = 0; k < 3; ++k) v[k] *= x.s[k];
    else for (int k = 0; k < 3; ++k) v[k] *= (x.s[k] != 0 ? x.s[k] / std::fabs(x.s[k]) : 1) * 1.0;
    // Euler XYZ: X first, then Y, then Z
    const double d2r = 3.14159265358979323846 / 180.0;
    for (int axis = 0; axis < 3; ++axis) {
        double a = x.r[axis] * d2r, c = std::cos(a), s = std::sin(a);
        int i = (axis + 1) % 3, j = (axis + 2) % 3;
        double vi = v[i] * c - v[j] * s, vj = v[i] * s + v[j] * c;
        v[i] = vi; v[j] = vj;
    }
    if (!direction) for (int k = 0; k < 3; ++k) v[k] += x.t[k];
    else {
        double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l > 1e-12) for (int k = 0; k < 3; ++k) v[k] /= l;
    }
    for (int k = 0; k < 3; ++k) p[k] = (float)v[k];
}

Model readFbx(const uint8_t* data, size_t len) {
    Model model;
    FNode root;
    bool ok = fbxBinary(data, len, root) || fbxAscii(data, len, root);
    const FNode* objects = ok ? root.child("Objects") : nullptr;
    if (!objects) { model.warnings.push_back("not an FBX file this tool can read"); return model; }

    std::unordered_map<long long, const FNode*> byId;
    for (const FNode& o : objects->kids)
        if (!o.ids.empty()) byId[o.ids[0]] = &o;
    // child -> parent (object to object)
    std::unordered_map<long long, long long> parentOf;
    std::unordered_map<long long, std::vector<long long>> childrenOf;
    if (const FNode* conns = root.child("Connections"))
        for (const FNode& c : conns->kids) {
            if (c.ids.size() < 2) continue;
            if (!c.strs.empty() && c.strs[0] != "OO") continue;
            long long ch = c.ids[0], pa = c.ids[1];
            childrenOf[pa].push_back(ch);
            auto it = byId.find(ch);
            if (it != byId.end() && it->second->name == "Model") parentOf[ch] = pa;
        }
    auto xformOf = [&](const FNode& m) {
        Xform x;
        if (const FNode* p70 = m.child("Properties70"))
            for (const FNode& pr : p70->kids) {
                std::string n = pr.str(0);
                const std::vector<double>& v = pr.nums;
                if (v.size() < 3) continue;
                size_t b = v.size() - 3;
                if (n == "Lcl Translation") for (int k = 0; k < 3; ++k) x.t[k] = v[b + k];
                else if (n == "Lcl Rotation") for (int k = 0; k < 3; ++k) x.r[k] = v[b + k];
                else if (n == "Lcl Scaling") for (int k = 0; k < 3; ++k) x.s[k] = v[b + k];
            }
        return x;
    };

    for (const FNode& g : objects->kids) {
        if (g.name != "Geometry" || g.ids.empty()) continue;
        if (g.strs.size() > 1 && g.strs[1] != "Mesh") continue;
        const FNode* vNode = g.child("Vertices");
        const FNode* iNode = g.child("PolygonVertexIndex");
        if (!vNode || !iNode) continue;
        const std::vector<double>& V = vNode->values();
        const std::vector<double>& PI = iNode->values();
        // the model that uses it
        long long modelId = 0;
        const FNode* modelNode = nullptr;
        for (auto& kv : childrenOf)
            for (long long ch : kv.second)
                if (ch == g.ids[0]) {
                    auto it = byId.find(kv.first);
                    if (it != byId.end() && it->second->name == "Model") { modelId = kv.first; modelNode = it->second; }
                }
        Mesh mesh;
        mesh.name = fbxObjectName(modelNode ? modelNode->str(0) : g.str(0));
        if (modelNode)
            for (long long ch : childrenOf[modelId]) {
                auto it = byId.find(ch);
                if (it != byId.end() && it->second->name == "Material") {
                    mesh.material = fbxObjectName(it->second->str(0));
                    break;
                }
            }
        struct Layer { const std::vector<double>* data = nullptr; const std::vector<double>* index = nullptr; bool byPoly = true; int width = 0; };
        auto layer = [&](const char* elem, const char* dataName, const char* indexName, int width, int which) {
            Layer L;
            int seen = 0;
            for (const FNode& k : g.kids) {
                if (k.name != elem) continue;
                if (seen++ != which) continue;
                const FNode* dn = k.child(dataName);
                if (!dn) break;
                L.data = &dn->values();
                L.width = width;
                const FNode* mt = k.child("MappingInformationType");
                std::string map = mt ? mt->str(0) : "ByPolygonVertex";
                L.byPoly = map == "ByPolygonVertex";
                if (!L.byPoly && map != "ByVertice" && map != "ByVertex") { L.data = nullptr; break; }
                const FNode* rt = k.child("ReferenceInformationType");
                std::string ref = rt ? rt->str(0) : "Direct";
                if (ref == "IndexToDirect" || ref == "Index") {
                    const FNode* in = k.child(indexName);
                    if (in) L.index = &in->values();
                }
                break;
            }
            return L;
        };
        Layer nL = layer("LayerElementNormal", "Normals", "NormalsIndex", 3, 0);
        Layer uL = layer("LayerElementUV", "UV", "UVIndex", 2, 0);
        Layer u2L = layer("LayerElementUV", "UV", "UVIndex", 2, 1);
        Layer cL = layer("LayerElementColor", "Colors", "ColorIndex", 4, 0);
        auto pick = [&](const Layer& L, size_t corner, long long vert, float* out) -> long long {
            if (!L.data) return -1;
            long long i = L.byPoly ? (long long)corner : vert;
            if (L.index) { if ((size_t)i >= L.index->size()) return -1; i = (long long)(*L.index)[i]; }
            if (i < 0 || (size_t)(i + 1) * L.width > L.data->size()) return -1;
            for (int k = 0; k < L.width; ++k) out[k] = (float)(*L.data)[(size_t)i * L.width + k];
            return i;
        };
        Xform chain[16];
        int chainN = 0;
        for (long long id = modelId; id && chainN < 16;) {
            auto it = byId.find(id);
            if (it == byId.end() || it->second->name != "Model") break;
            chain[chainN++] = xformOf(*it->second);
            auto pp = parentOf.find(id);
            id = pp == parentOf.end() ? 0 : pp->second;
        }
        Welder weld(mesh);
        std::vector<uint32_t> poly;
        std::vector<long long> polyCorner;
        size_t corner = 0;
        for (size_t k = 0; k < PI.size(); ++k, ++corner) {
            long long raw = (long long)PI[k];
            bool last = raw < 0;
            long long vi = last ? ~raw : raw;
            if (vi < 0 || (size_t)(vi + 1) * 3 > V.size()) { poly.clear(); if (last) continue; else continue; }
            float p[3] = { (float)V[vi * 3], (float)V[vi * 3 + 1], (float)V[vi * 3 + 2] };
            for (int c = 0; c < chainN; ++c) applyXform(chain[c], p, false);
            float n[3], uv[2], uv2[2], col[4];
            long long ni = pick(nL, corner, vi, n);
            if (ni >= 0) for (int c = 0; c < chainN; ++c) applyXform(chain[c], n, true);
            long long ti = pick(uL, corner, vi, uv);
            long long t2 = pick(u2L, corner, vi, uv2);
            long long ci = pick(cL, corner, vi, col);
            // welding keys on the values' sources; a normal given per corner
            // is keyed by its value so smooth corners still share a vertex
            long long nkey = ni;
            if (ni >= 0 && nL.byPoly && !nL.index) {
                int32_t q[3] = { (int32_t)std::lround(n[0] * 1000), (int32_t)std::lround(n[1] * 1000), (int32_t)std::lround(n[2] * 1000) };
                nkey = ((long long)(q[0] & 0xFFFFF) << 40) ^ ((long long)(q[1] & 0xFFFFF) << 20) ^ (q[2] & 0xFFFFF);
            }
            long long tkey = ti;
            if (ti >= 0 && uL.byPoly && !uL.index) {
                int32_t q[2] = { (int32_t)std::lround(uv[0] * 100000), (int32_t)std::lround(uv[1] * 100000) };
                tkey = ((long long)(uint32_t)q[0] << 32) ^ (uint32_t)q[1];
            }
            poly.push_back(weld.add(vi, tkey, nkey, (ci >= 0 ? ci : -1) ^ (t2 >= 0 ? (t2 << 24) : 0),
                                    p, ti >= 0 ? uv : nullptr, ni >= 0 ? n : nullptr,
                                    ci >= 0 ? col : nullptr, t2 >= 0 ? uv2 : nullptr));
            if (last) {
                for (size_t c = 1; c + 1 < poly.size(); ++c) {
                    mesh.indices.push_back(poly[0]);
                    mesh.indices.push_back(poly[c]);
                    mesh.indices.push_back(poly[c + 1]);
                }
                poly.clear();
            }
        }
        finishMesh(model, mesh);
    }
    model.valid = !model.meshes.empty();
    if (!model.valid) model.warnings.push_back("the FBX file has no meshes");
    return model;
}

// ------------------------------------------------------------ M3G writer

uint32_t adler32(const uint8_t* d, size_t n) {
    uint32_t a = 1, b = 0;
    while (n) {
        size_t k = std::min<size_t>(n, 5552);
        n -= k;
        while (k--) { a += *d++; b += a; }
        a %= 65521; b %= 65521;
    }
    return (b << 16) | a;
}

void put32(Bytes& b, uint32_t v) { for (int k = 0; k < 4; ++k) b.push_back((uint8_t)(v >> (8 * k))); }
void put16(Bytes& b, uint16_t v) { b.push_back((uint8_t)v); b.push_back((uint8_t)(v >> 8)); }
uint32_t get32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
float getf(const uint8_t* p) { float v; memcpy(&v, p, 4); return v; }
void set32(Bytes& b, size_t at, uint32_t v) { memcpy(&b[at], &v, 4); }
void setf(Bytes& b, size_t at, float v) { memcpy(&b[at], &v, 4); }

struct Obj { uint8_t type; Bytes data; };

// a vertex array: 12 bytes of Object3D, then size, count, encoding, n
Bytes vertexArray(const Bytes& head12, int size, int comps, const std::vector<int32_t>& v, int n) {
    Bytes b(head12.begin(), head12.begin() + 12);
    b.push_back((uint8_t)size); b.push_back((uint8_t)comps); b.push_back(0);
    put16(b, (uint16_t)n);
    for (int32_t x : v) {
        if (size == 1) b.push_back((uint8_t)x);
        else if (size == 2) put16(b, (uint16_t)(int16_t)x);
        else put32(b, (uint32_t)x);
    }
    return b;
}

int32_t clamp16(double v) { return (int32_t)std::max(-32767.0, std::min(32767.0, std::floor(v + 0.5))); }

} // namespace

std::map<std::string, MeshPlacement> meshPlacements(const Model& raw, const Model& placed) {
    std::map<std::string, MeshPlacement> out;
    std::map<std::string, const Mesh*> after;
    for (const Mesh& m : placed.meshes) after.emplace(m.name, &m);
    for (const Mesh& m : raw.meshes) {
        if (out.count(m.name)) continue;
        Mesh a = m;
        computeBounds(a);
        MeshPlacement mp;
        for (int k = 0; k < 3; ++k) {
            mp.centre[k] = (a.bboxMin[k] + a.bboxMax[k]) * 0.5f;
            mp.size = std::max(mp.size, a.bboxMax[k] - a.bboxMin[k]);
        }
        auto it = after.find(m.name);
        if (it != after.end()) {
            Mesh b = *it->second;
            computeBounds(b);
            for (int k = 0; k < 3; ++k) mp.offset[k] = (b.bboxMin[k] + b.bboxMax[k]) * 0.5f - mp.centre[k];
        }
        out[m.name] = mp;
    }
    return out;
}

Model readImportModel(const uint8_t* data, size_t len, const std::string& name) {
    std::string ext = lowerStr(extensionOf(name));
    if (ext == "fbx" || (len > 18 && memcmp(data, "Kaydara FBX Binary", 18) == 0)) return readFbx(data, len);
    return readObj(data, len);
}

namespace {
// where a mesh object (type 14) keeps its name and its references: the vertex
// buffer and each submesh's index buffer and appearance
bool meshLayout(const Bytes& b, size_t& nameAt, size_t& nameEnd, size_t& matAt) {
    if (b.size() < 12) return false;
    uint32_t kind = get32(b.data() + 8);
    size_t q = 12;
    matAt = 0;
    if (kind == 5) q += 48;
    else if (kind == 6) { q += 56; matAt = q; q += 4; }
    else if (kind == 7) { q += 81; matAt = q; q += 4; }
    else return false;
    if (q >= b.size()) return false;
    size_t e = q;
    while (e < b.size() && b[e]) ++e;
    if (e + 1 + 50 + 8 > b.size()) return false;
    nameAt = q;
    nameEnd = e + 1;
    return true;
}
}

static bool replaceOrBuild(const Bytes& original, const Model& importedIn,
                           const std::map<std::string, MeshPlacement>& placement,
                           Bytes& out, std::string& report, bool build);

bool rr3ReplaceMeshes(const Bytes& original, const Model& imported,
                      const std::map<std::string, MeshPlacement>& placement,
                      Bytes& out, std::string& report) {
    return replaceOrBuild(original, imported, placement, out, report, false);
}

static bool replaceOrBuild(const Bytes& original, const Model& importedIn,
                           const std::map<std::string, MeshPlacement>& placement,
                           Bytes& out, std::string& report, bool build) {
    // objects of one name (3ds Max and Blender split a mesh per material, or
    // export copies) are one mesh here: their geometry goes together
    Model imported;
    {
        std::map<std::string, size_t> at;
        for (const Mesh& m : importedIn.meshes) {
            if (m.indices.empty()) continue;
            std::string key = m.name;
            size_t dot = key.rfind('.');
            if (dot != std::string::npos && dot + 1 < key.size() &&
                key.find_first_not_of("0123456789", dot + 1) == std::string::npos) key.erase(dot);
            auto it = at.find(key);
            if (it == at.end()) {
                at[key] = imported.meshes.size();
                imported.meshes.push_back(m);
                imported.meshes.back().name = key;
                continue;
            }
            Mesh& d = imported.meshes[it->second];
            size_t nv = d.positions.size() / 3, add = m.positions.size() / 3;
            bool uv = d.uvs.size() == nv * 2 && m.uvs.size() == add * 2;
            bool uv2 = d.uvs2.size() == nv * 2 && m.uvs2.size() == add * 2;
            bool col = d.colors.size() == nv * 4 && m.colors.size() == add * 4;
            d.positions.insert(d.positions.end(), m.positions.begin(), m.positions.end());
            d.normals.insert(d.normals.end(), m.normals.begin(), m.normals.end());
            if (uv) d.uvs.insert(d.uvs.end(), m.uvs.begin(), m.uvs.end()); else d.uvs.clear();
            if (uv2) d.uvs2.insert(d.uvs2.end(), m.uvs2.begin(), m.uvs2.end()); else d.uvs2.clear();
            if (col) d.colors.insert(d.colors.end(), m.colors.begin(), m.colors.end()); else d.colors.clear();
            for (uint32_t i : m.indices) d.indices.push_back(i + (uint32_t)nv);
        }
        for (Mesh& m : imported.meshes) { fillNormals(m); computeBounds(m); }
    }
    report.clear();
    const uint8_t* d = original.data();
    size_t len = original.size();
    static const uint8_t kId[12] = { 0xAB, 0x4A, 0x53, 0x52, 0x31, 0x38, 0x34, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A };
    if (len < 12 || memcmp(d, kId, 12) != 0) {
        report = "this .m3g is not a plain JSR-184 file (Real Racing 3 car models are)";
        return false;
    }
    // ---- the objects, section by section ----
    std::vector<Obj> objs(1);
    std::vector<size_t> sectionOf(1, 0);
    int sections = 0;
    size_t p = 12;
    while (p + 9 <= len) {
        uint8_t comp = d[p];
        uint32_t plain = get32(d + p + 5);
        if (comp != 0) { report = "a compressed section - not a Real Racing 3 car model"; return false; }
        size_t q = p + 9, end = q + plain;
        if (end + 4 > len) { report = "the file is cut short"; return false; }
        while (q + 5 <= end) {
            Obj o;
            o.type = d[q];
            uint32_t l = get32(d + q + 1);
            if (q + 5 + l > end) { report = "an object runs past its section"; return false; }
            o.data.assign(d + q + 5, d + q + 5 + l);
            objs.push_back(std::move(o));
            sectionOf.push_back((size_t)sections);
            q += 5 + l;
        }
        p = end + 4;
        ++sections;
    }
    if (objs.size() < 3) { report = "no objects"; return false; }

    // ---- materials and meshes ----
    std::vector<std::string> materials;
    size_t materialsObj = 0;
    bool materialsDirty = false;
    for (size_t i = 1; i < objs.size(); ++i)
        if (objs[i].type == 24) {
            materialsObj = i;
            const Bytes& b = objs[i].data;
            if (b.size() < 4) break;
            uint32_t n = get32(b.data());
            size_t q = 4;
            for (uint32_t k = 0; k < n && q < b.size(); ++k) {
                size_t e = q;
                while (e < b.size() && b[e]) ++e;
                materials.emplace_back((const char*)b.data() + q, e - q);
                q = e + 1;
            }
            break;
        }
    struct MeshRef {
        size_t obj; std::string name; size_t nameAt, nameEnd, matAt;  // matAt 0 when none
        uint32_t vb, ib; int material;
    };
    std::vector<MeshRef> meshes;
    for (size_t i = 1; i < objs.size(); ++i) {
        if (objs[i].type != 14) continue;
        const Bytes& b = objs[i].data;
        if (b.size() < 12) continue;
        uint32_t kind = get32(b.data() + 8);
        size_t q = 12, matAt = 0;
        if (kind == 5) q += 48;
        else if (kind == 6) { q += 56; matAt = q; q += 4; }
        else if (kind == 7) { q += 81; matAt = q; q += 4; }
        else continue;
        if (q >= b.size()) continue;
        size_t e = q;
        while (e < b.size() && b[e]) ++e;
        if (e + 1 + 50 + 12 > b.size()) continue;
        MeshRef m;
        m.obj = i; m.nameAt = q; m.nameEnd = e + 1; m.matAt = matAt;
        m.name.assign((const char*)b.data() + q, e - q);
        size_t r = e + 1 + 50;
        m.vb = get32(b.data() + r);
        uint32_t subs = get32(b.data() + r + 4);
        m.ib = subs ? get32(b.data() + r + 8) : 0;
        m.material = matAt ? (int)get32(b.data() + matAt) : -1;
        if (m.vb && m.vb < objs.size() && objs[m.vb].type == 21 && m.ib && m.ib < objs.size() && objs[m.ib].type == 11)
            meshes.push_back(m);
    }
    if (meshes.empty()) { report = "no meshes in this file"; return false; }

    // how often each array is used: one shared by several buffers is left alone
    std::map<uint32_t, int> arrayUse;
    // A vertex buffer's arrays: the ones it names, and - Real Racing 3 writes
    // five arrays in front of every buffer and may name only four - the
    // unnamed ones in that run too (a second UV set the game reads by
    // position). They all hold one entry per vertex and go with the buffer.
    struct VbInfo { uint32_t pos = 0, nrm = 0, col = 0; std::vector<uint32_t> sets; std::vector<size_t> setAt;
                    std::vector<uint32_t> run, extras; size_t scaleAt = 0, biasAt = 0; };
    auto vbInfo = [&](uint32_t vb) {
        VbInfo v;
        const Bytes& b = objs[vb].data;
        if (b.size() < 48) return v;
        v.pos = get32(&b[16]); v.biasAt = 20; v.scaleAt = 32;
        v.nrm = get32(&b[36]); v.col = get32(&b[40]);
        uint32_t sets = get32(&b[44]);
        for (uint32_t s = 0; s < sets && 48 + (s + 1) * 20 <= b.size(); ++s) {
            v.sets.push_back(get32(&b[48 + s * 20]));
            v.setAt.push_back(48 + s * 20);
        }
        std::vector<uint32_t> named = { v.pos, v.nrm, v.col };
        named.insert(named.end(), v.sets.begin(), v.sets.end());
        uint32_t lo = vb;
        while (lo > 1 && objs[lo - 1].type == 20) --lo;
        for (uint32_t a = lo; a < vb; ++a) {
            v.run.push_back(a);
            if (std::find(named.begin(), named.end(), a) == named.end()) v.extras.push_back(a);
        }
        // named arrays outside the run (not how Real Racing 3 writes them)
        for (uint32_t a : named)
            if (a && std::find(v.run.begin(), v.run.end(), a) == v.run.end()) v.run.push_back(a);
        return v;
    };
    for (size_t i = 1; i < objs.size(); ++i)
        if (objs[i].type == 21) {
            VbInfo v = vbInfo((uint32_t)i);
            for (uint32_t a : { v.pos, v.nrm, v.col }) if (a) ++arrayUse[a];
            for (uint32_t a : v.sets) if (a) ++arrayUse[a];
        }
    bool rr3Naming = !materials.empty();

    auto arrayHead = [&](uint32_t a, int& size, int& comps, int& n) -> bool {
        if (!a || a >= objs.size() || objs[a].type != 20 || objs[a].data.size() < 17) return false;
        size = objs[a].data[12]; comps = objs[a].data[13]; n = objs[a].data[15] | (objs[a].data[16] << 8);
        return true;
    };
    // a normal's length in the array's integers (Real Racing 3 uses 4096)
    auto normalUnit = [&](uint32_t a) -> double {
        int size, comps, n;
        if (!arrayHead(a, size, comps, n) || size != 2 || n == 0) return 32767.0;
        const uint8_t* q = objs[a].data.data() + 17;
        int16_t x, y, z; memcpy(&x, q, 2); memcpy(&y, q + 2, 2); memcpy(&z, q + 4, 2);
        double l = std::sqrt((double)x * x + (double)y * y + (double)z * z);
        return l > 1000 && l < 40000 ? (l > 20000 ? 32767.0 : 4096.0) : 32767.0;
    };

    // Writes `src` into the mesh `m` (a vertex buffer and index buffer of the
    // file's own); arrays not listed in `own` are appended as new objects.
    auto fill = [&](const Mesh& src, uint32_t vbObj, uint32_t ibObj, std::string& why) -> bool {
        VbInfo v = vbInfo(vbObj);
        int psize, pcomps, pn;
        if (!arrayHead(v.pos, psize, pcomps, pn)) { why = "no position array"; return false; }
        size_t nv = src.positions.size() / 3;
        if (nv == 0 || nv > 65535) { why = "has " + std::to_string(nv) + " vertices (65535 at most)"; return false; }
        Bytes& vb = objs[vbObj].data;
        // what the file had: arrays the model brings no data for (a second
        // UV set, Real Racing 3's baked colours) are kept when the vertex
        // count is unchanged
        size_t before = (size_t)pn;
        bool sameCount = before == nv;
        // positions
        float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
        for (size_t i = 0; i < nv; ++i)
            for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], src.positions[i * 3 + k]); hi[k] = std::max(hi[k], src.positions[i * 3 + k]); }
        std::vector<int32_t> vals;
        vals.reserve(nv * 3);
        if (psize == 4) {
            float scale = getf(&vb[v.scaleAt]);
            float bias[3] = { getf(&vb[v.biasAt]), getf(&vb[v.biasAt + 4]), getf(&vb[v.biasAt + 8]) };
            if (!(scale != 0) || !std::isfinite(scale)) { scale = 1; setf(vb, v.scaleAt, 1); }
            for (size_t i = 0; i < nv; ++i)
                for (int k = 0; k < 3; ++k) {
                    float f = (src.positions[i * 3 + k] - bias[k]) / scale;
                    uint32_t u; memcpy(&u, &f, 4); vals.push_back((int32_t)u);
                }
        } else {
            float ext = 0, c[3];
            for (int k = 0; k < 3; ++k) { c[k] = (lo[k] + hi[k]) * 0.5f; ext = std::max(ext, (hi[k] - lo[k]) * 0.5f); }
            float scale = ext > 0 ? ext / 32000.0f : 1.0f;
            if (psize == 1) scale = ext > 0 ? ext / 120.0f : 1.0f;
            setf(vb, v.scaleAt, scale);
            for (int k = 0; k < 3; ++k) setf(vb, v.biasAt + k * 4, c[k]);
            for (size_t i = 0; i < nv; ++i)
                for (int k = 0; k < 3; ++k) vals.push_back(clamp16((src.positions[i * 3 + k] - c[k]) / scale));
        }
        objs[v.pos].data = vertexArray(objs[v.pos].data, psize, 3, vals, (int)nv);
        // normals
        int s2, c2, n2;
        if (arrayHead(v.nrm, s2, c2, n2)) {
            double unit = s2 == 1 ? 127.0 : normalUnit(v.nrm);
            vals.clear();
            for (size_t i = 0; i < nv; ++i)
                for (int k = 0; k < 3; ++k) {
                    const float* nn = &src.normals[i * 3];
                    double l = std::sqrt((double)nn[0] * nn[0] + (double)nn[1] * nn[1] + (double)nn[2] * nn[2]);
                    double x = l > 1e-9 ? nn[k] / l : (k == 1 ? 1.0 : 0.0);
                    if (s2 == 4) { float f = (float)x; uint32_t u; memcpy(&u, &f, 4); vals.push_back((int32_t)u); }
                    else vals.push_back(clamp16(x * unit));
                }
            objs[v.nrm].data = vertexArray(objs[v.nrm].data, s2, 3, vals, (int)nv);
        }
        // colours: bytes (255 white) or Real Racing 3's 12-bit fixed (4096)
        if (arrayHead(v.col, s2, c2, n2) && !(sameCount && src.colors.size() != nv * 4)) {
            double unit = s2 == 1 ? 255.0 : 4096.0;
            bool have = src.colors.size() == nv * 4;
            vals.clear();
            for (size_t i = 0; i < nv; ++i)
                for (int k = 0; k < c2; ++k) {
                    double x = have ? src.colors[i * 4 + std::min(k, 3)] : 1.0;
                    if (s2 == 4) { float f = (float)x; uint32_t u; memcpy(&u, &f, 4); vals.push_back((int32_t)u); }
                    else vals.push_back(s2 == 1 ? (int32_t)std::max(0.0, std::min(255.0, x * unit + 0.5)) : clamp16(x * unit));
                }
            objs[v.col].data = vertexArray(objs[v.col].data, s2, c2, vals, (int)nv);
        }
        // texture coordinates: the same file convention the reader undoes
        bool up = rr3Naming;          // the viewer reads RR3 with V as -v
        for (size_t s = 0; s < v.sets.size(); ++s) {
            if (!arrayHead(v.sets[s], s2, c2, n2)) continue;
            if (s >= 1 && sameCount && src.uvs2.size() != nv * 2) continue;
            // a second set is the model's own second UV set, or zeros - never
            // a copy of the first: every car the game ships has it all zero,
            // and a copy makes the game sample its second texture (the lights'
            // glow) at the wrong places
            static const std::vector<float> none;
            const std::vector<float>& uv = s == 0 ? src.uvs : (src.uvs2.size() == nv * 2 ? src.uvs2 : none);
            bool have = uv.size() == nv * 2;
            size_t at = v.setAt[s];
            float bias[2] = { getf(&vb[at + 4]), getf(&vb[at + 8]) };
            float fileScale = getf(&vb[at + 16]);
            if (!(fileScale != 0)) fileScale = 1.0f / 512;
            float realScale = fileScale * 0.25f;
            // the raw range must fit 16 bits: widen the scale when it does not
            double maxRaw = 0;
            std::vector<double> raw(nv * 2, 0.0);
            for (size_t i = 0; i < nv && have; ++i) {
                double u = uv[i * 2], vv = uv[i * 2 + 1];
                double fv = up ? -vv : vv - 1.0;
                raw[i * 2] = (u - bias[0]) / realScale;
                raw[i * 2 + 1] = (fv - bias[1]) / realScale;
                maxRaw = std::max(maxRaw, std::max(std::fabs(raw[i * 2]), std::fabs(raw[i * 2 + 1])));
            }
            if (s2 == 2 && maxRaw > 32000) {
                double k = maxRaw / 32000;
                fileScale = (float)(fileScale * k);
                setf(vb, at + 16, fileScale);
                for (double& r : raw) r /= k;
            }
            vals.clear();
            for (size_t i = 0; i < nv; ++i)
                for (int k = 0; k < c2; ++k) {
                    double x = k < 2 ? raw[i * 2 + k] : 0.0;
                    if (s2 == 4) { float f = (float)x; uint32_t u2; memcpy(&u2, &f, 4); vals.push_back((int32_t)u2); }
                    else if (s2 == 1) vals.push_back((int32_t)std::max(-128.0, std::min(127.0, std::floor(x + 0.5))));
                    else vals.push_back(clamp16(x));
                }
            objs[v.sets[s]].data = vertexArray(objs[v.sets[s]].data, s2, c2, vals, (int)nv);
        }
        // the unnamed arrays of the run: a second UV set is written like the
        // first; anything else is given the normals (or zeros) so every
        // array still holds one entry per vertex
        for (uint32_t ex : v.extras) {
            if (!arrayHead(ex, s2, c2, n2)) continue;
            if (sameCount && src.uvs2.size() != nv * 2) continue;
            vals.clear();
            if (c2 == 2 && s2 == 2 && !v.sets.empty()) {
                // zeros unless the model brings a second UV set (see above)
                const std::vector<float>& uv = src.uvs2;
                size_t at = v.setAt[0];
                float bias[2] = { getf(&vb[at + 4]), getf(&vb[at + 8]) };
                float realScale = getf(&vb[at + 16]) * 0.25f;
                if (!(realScale != 0)) realScale = 1.0f / 2048;
                for (size_t i = 0; i < nv; ++i) {
                    if (uv.size() != nv * 2) { vals.push_back(0); vals.push_back(0); continue; }
                    double u = uv[i * 2], vv = uv[i * 2 + 1];
                    double fv = up ? -vv : vv - 1.0;
                    vals.push_back(clamp16((u - bias[0]) / realScale));
                    vals.push_back(clamp16((fv - bias[1]) / realScale));
                }
            } else {
                double unit = s2 == 1 ? 127.0 : 4096.0;
                for (size_t i = 0; i < nv; ++i)
                    for (int k = 0; k < c2; ++k) {
                        double x = k < 3 ? src.normals[i * 3 + k] : 1.0;
                        if (s2 == 4) { float f = (float)x; uint32_t u2; memcpy(&u2, &f, 4); vals.push_back((int32_t)u2); }
                        else vals.push_back(s2 == 1 ? (int32_t)std::floor(x * unit + 0.5) : clamp16(x * unit));
                    }
            }
            objs[ex].data = vertexArray(objs[ex].data, s2, c2, vals, (int)nv);
        }
        // indices: a triangle list in the strip container, one "strip"
        Bytes& ib = objs[ibObj].data;
        if (ib.size() < 22) { why = "an index buffer too short"; return false; }
        Bytes nb(ib.begin(), ib.begin() + 21);
        int enc = nv <= 256 ? 129 : (nv <= 65536 ? 130 : 128);
        nb.push_back((uint8_t)enc);
        put32(nb, (uint32_t)src.indices.size());
        for (uint32_t x : src.indices) {
            if (enc == 129) nb.push_back((uint8_t)x);
            else if (enc == 130) put16(nb, (uint16_t)x);
            else put32(nb, x);
        }
        put32(nb, 1);
        put32(nb, (uint32_t)src.indices.size());
        ib = std::move(nb);
        return true;
    };

    // ---- match the imported meshes to the file's ----
    auto stripSuffix = [](std::string n) {
        // Blender's ".001" and 3ds Max's "001" copies; the tool's own wheel
        // copies (WHEEL_FL_...) are not parts of the file
        size_t dot = n.rfind('.');
        if (dot != std::string::npos && dot + 1 < n.size() &&
            n.find_first_not_of("0123456789", dot + 1) == std::string::npos) n.erase(dot);
        return n;
    };
    std::map<std::string, size_t> byName;
    for (size_t k = 0; k < meshes.size(); ++k) byName.emplace(meshes[k].name, k);
    std::vector<bool> used(meshes.size(), false);
    int replaced = 0, added = 0, skipped = 0;
    std::string lines;
    uint32_t nextIndex = (uint32_t)objs.size();
    // A new car keeps every part name the game looks for. Parts the model
    // does not have (the damaged panels, broken lenses, the bonnet-cam hood,
    // the steering wheel) are written as an empty speck at the origin: the
    // game finds the name and draws nothing.
    // A new car keeps every part the game may look for. A damaged panel or a
    // broken lens the model does not have is a copy of the model's own intact
    // part (the game swaps one for the other); any other part of the
    // original car the model does not replace is left as it was. An empty
    // stand-in is never written: an empty part is what crashed the game.
    int damageCopies = 0, keptOriginal = 0;
    if (build) {
        std::map<std::string, size_t> have;
        for (size_t k = 0; k < imported.meshes.size(); ++k) have[imported.meshes[k].name] = k;
        auto intactOf = [](std::string n) {
            for (const char* cut : { "_BROKEN_A", "_BROKEN_B", "_BROKEN", "_DAMAGE", "BONNETCAM_" }) {
                size_t at;
                while ((at = n.find(cut)) != std::string::npos) n.erase(at, strlen(cut));
            }
            return n;
        };
        for (const MeshRef& r : meshes) {
            if (have.count(r.name)) continue;
            std::string intact = intactOf(r.name);
            auto it = intact != r.name ? have.find(intact) : have.end();
            if (it == have.end()) {
                // a part the new car has no counterpart for: a 1 cm triangle
                // hidden inside the body, so the name is there and nothing of
                // the old car shows (a zero-size part is what crashed)
                Mesh p;
                p.name = r.name;
                if (r.material >= 0 && r.material < (int)materials.size()) p.material = materials[r.material];
                p.positions = { 0.0f, 0.45f, 0.0f, 0.01f, 0.45f, 0.0f, 0.0f, 0.46f, 0.0f };
                p.normals = { 0, 0, 1, 0, 0, 1, 0, 0, 1 };
                p.uvs = { 0.5f, 0.5f, 0.51f, 0.5f, 0.5f, 0.51f };
                p.indices = { 0, 1, 2 };
                have[r.name] = imported.meshes.size();
                imported.meshes.push_back(p);
                ++keptOriginal;
                continue;
            }
            Mesh copy = imported.meshes[it->second];
            copy.name = r.name;
            have[r.name] = imported.meshes.size();
            imported.meshes.push_back(copy);
            ++damageCopies;
        }
    }
    // The game's own order: a new car's parts are written in the order the
    // original file has them (it is sorted by name, and the game may pair
    // parts across the car's _a ... _h files by position).
    if (build) {
        std::map<std::string, size_t> order;
        for (size_t k = 0; k < meshes.size(); ++k) order.emplace(meshes[k].name, k);
        std::stable_sort(imported.meshes.begin(), imported.meshes.end(), [&](const Mesh& a, const Mesh& b) {
            auto ia = order.find(a.name), ib = order.find(b.name);
            size_t ka = ia == order.end() ? (size_t)-1 : ia->second;
            size_t kb = ib == order.end() ? (size_t)-1 : ib->second;
            return ka < kb;
        });
    }
    // A model that comes back 100 times too big (or small) went through an
    // application working in centimetres: the matched meshes' sizes say so.
    double unitScale = 1.0;
    {
        std::vector<double> ratios;
        for (const Mesh& in : imported.meshes) {
            auto pl = placement.find(in.name);
            if (pl == placement.end()) pl = placement.find(stripSuffix(in.name));
            if (pl == placement.end() || pl->second.size <= 1e-4f) continue;
            float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
            for (size_t i = 0; i + 2 < in.positions.size(); i += 3)
                for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], in.positions[i + k]); hi[k] = std::max(hi[k], in.positions[i + k]); }
            double sz = std::max(hi[0] - lo[0], std::max(hi[1] - lo[1], hi[2] - lo[2]));
            if (sz > 0) ratios.push_back(sz / pl->second.size);
        }
        if (!ratios.empty()) {
            std::sort(ratios.begin(), ratios.end());
            double r = ratios[ratios.size() / 2];
            if (r > 30 && r < 300) unitScale = 0.01;
            else if (r > 1.0 / 300 && r < 1.0 / 30) unitScale = 100.0;
        }
    }
    for (const Mesh& in : imported.meshes) {
        std::string name = in.name;
        auto it = byName.find(name);
        if (it == byName.end()) { name = stripSuffix(name); it = byName.find(name); }
        if (build) it = byName.end();
        if (name.compare(0, 6, "WHEEL_") == 0 && name.size() > 9 && name[8] == '_' && byName.count(name.substr(9))) {
            ++skipped;
            continue;
        }
        if (name.rfind("POINT_", 0) == 0 || in.indices.empty()) continue;
        Mesh src = in;
        if (it != byName.end()) {
            // undo the viewer's placement when the mesh was exported placed
            auto pl = placement.find(name);
            const MeshRef& mr = meshes[it->second];
            if (pl != placement.end()) {
                const MeshPlacement& mp = pl->second;
                computeBounds(src);
                double dFile = 0, dPlaced = 0;
                for (int k = 0; k < 3; ++k) {
                    double c = (src.bboxMin[k] + src.bboxMax[k]) * 0.5 * unitScale;
                    dFile += (c - mp.centre[k]) * (c - mp.centre[k]);
                    dPlaced += (c - mp.centre[k] - mp.offset[k]) * (c - mp.centre[k] - mp.offset[k]);
                }
                for (float& x : src.positions) x *= unitScale;
                if (dPlaced < dFile)
                    for (size_t i = 0; i + 2 < src.positions.size(); i += 3)
                        for (int k = 0; k < 3; ++k) src.positions[i + k] -= mp.offset[k];
            } else {
                for (float& x : src.positions) x *= unitScale;
            }
            std::string why;
            if (used[it->second]) { lines += "  " + name + ": given twice, the first kept\n"; continue; }
            if (arrayUse[vbInfo(mr.vb).pos] > 1) { lines += "  " + name + ": its vertices are shared with another mesh - left as it was\n"; continue; }
            if (!fill(src, mr.vb, mr.ib, why)) { lines += "  " + name + ": " + why + "\n"; continue; }
            used[it->second] = true;
            ++replaced;
            continue;
        }
        // ---- a new mesh: a template of the same material, its objects cloned ----
        // The car's own material, by name. 3ds Max and Blender turn the
        // space in "Vehicle Exterior_mm_ext" into "_" (and may add ".001"),
        // so names are compared without case, spaces, "_" or "-"; failing
        // that, by the _mm_<atlas> ending. A material the game does not know
        // makes it drop the mesh, so an unknown name is never added: the mesh
        // takes the material of the car part it is cloned from.
        auto norm = [](std::string x) {
            size_t dot = x.rfind('.');
            if (dot != std::string::npos && dot + 1 < x.size() &&
                x.find_first_not_of("0123456789", dot + 1) == std::string::npos) x.erase(dot);
            std::string o;
            for (char c : x) {
                if (c == ' ' || c == '_' || c == '-') continue;
                o += (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
            }
            return o;
        };
        int want = -1;
        std::string lm = norm(src.material);
        for (size_t k = 0; k < materials.size() && want < 0; ++k) if (!lm.empty() && norm(materials[k]) == lm) want = (int)k;
        if (want < 0) {
            std::string m = lowerStr(src.material.empty() ? name : src.material);
            size_t mm = m.rfind("mm_");
            if (mm != std::string::npos) {
                std::string atlas = norm(m.substr(mm + 3));
                for (size_t k = 0; k < materials.size() && want < 0; ++k) {
                    std::string mk = lowerStr(materials[k]);
                    size_t km = mk.rfind("mm_");
                    if (km != std::string::npos && norm(mk.substr(km + 3)) == atlas) want = (int)k;
                }
            }
        }
        if (want < 0 && !src.material.empty())
            lines += "  " + name + ": the car has no material \"" + src.material + "\" - it keeps its template's\n";
        (void)materialsDirty;
        for (float& x : src.positions) x *= (float)unitScale;
        const MeshRef* tpl = nullptr;
        // a part the car has: cloned from that very part, so its node data
        // (pivot, flags, the material it had) stays the game's own
        if (build)
            for (const MeshRef& m : meshes) if (m.name == name) { tpl = &m; break; }
        if (tpl) want = tpl->material;   // the part name says its atlas (_mm_lights ...)
        if (!tpl)
            for (const MeshRef& m : meshes) if (want >= 0 && m.material == want) { tpl = &m; break; }
        if (!tpl && build)
            for (const MeshRef& m : meshes) if (m.name.find("BODY") != std::string::npos) { tpl = &m; break; }
        if (!tpl) {
            // same detail level and a body-ish part, else the first mesh
            std::string pre = name.size() > 6 && name.compare(0, 4, "LOD_") == 0 ? name.substr(0, 6) : "";
            for (const MeshRef& m : meshes) if (!pre.empty() && m.name.compare(0, pre.size(), pre) == 0) { tpl = &m; break; }
            if (!tpl) tpl = &meshes[0];
        }
        VbInfo tv = vbInfo(tpl->vb);
        // clone: arrays, buffer, index buffer, mesh - new ones at the end
        std::map<uint32_t, uint32_t> remap;
        auto cloneObj = [&](uint32_t from) {
            if (!from) return 0u;
            auto r = remap.find(from);
            if (r != remap.end()) return r->second;
            Obj o = objs[from];
            objs.push_back(o);
            sectionOf.push_back(sectionOf[from]);
            uint32_t idx = nextIndex++;
            remap[from] = idx;
            return idx;
        };
        for (uint32_t a : tv.run) cloneObj(a);           // in file order
        uint32_t nvb = cloneObj(tpl->vb);
        {
            Bytes& b = objs[nvb].data;
            auto re = [&](size_t at) { uint32_t a = get32(&b[at]); if (a && remap.count(a)) set32(b, at, remap[a]); };
            re(16); re(36); re(40);
            for (size_t s = 0; s < tv.sets.size(); ++s) re(48 + s * 20);
        }
        uint32_t nib = cloneObj(tpl->ib);
        std::string why;
        if (!fill(src, nvb, nib, why)) {
            // take the clones back off
            objs.resize(objs.size() - remap.size());
            sectionOf.resize(objs.size());
            nextIndex = (uint32_t)objs.size();
            lines += "  " + name + " (new): " + why + "\n";
            continue;
        }
        const Bytes& tb = objs[tpl->obj].data;
        Bytes mb(tb.begin(), tb.begin() + tpl->nameAt);
        if (tpl->matAt && want >= 0) set32(mb, tpl->matAt, (uint32_t)want);
        mb.insert(mb.end(), name.begin(), name.end());
        mb.push_back(0);
        mb.insert(mb.end(), tb.begin() + tpl->nameEnd, tb.begin() + tpl->nameEnd + 50);
        put32(mb, nvb);
        put32(mb, 1);
        put32(mb, nib);
        size_t subAt = tpl->nameEnd + 50 + 8;
        put32(mb, subAt + 8 <= tb.size() ? get32(&tb[subAt + 4]) : 0);   // appearance
        // anything after the submesh list (morph/skin data) is the template's
        uint32_t subs = get32(&tb[tpl->nameEnd + 50 + 4]);
        size_t after = tpl->nameEnd + 50 + 8 + (size_t)subs * 8;
        if (after < tb.size()) mb.insert(mb.end(), tb.begin() + after, tb.end());
        Obj mo; mo.type = 14; mo.data = std::move(mb);
        objs.push_back(std::move(mo));
        sectionOf.push_back(sectionOf[tpl->obj]);
        ++nextIndex;
        ++added;
        lines += "  " + name + " (new, from " + tpl->name + (want >= 0 ? ", material " + materials[want] : "") + ")\n";
    }
    if (materialsDirty) {
        Bytes b;
        put32(b, (uint32_t)materials.size());
        for (const std::string& m : materials) { b.insert(b.end(), m.begin(), m.end()); b.push_back(0); }
        objs[materialsObj].data = std::move(b);
    }
    if (build) {
        if (!added) {
            report = "No mesh of the model could be written.";
            if (!lines.empty()) report += "\n\n" + lines;
            return false;
        }
        // the template's own meshes go, with their buffers and arrays; every
        // object after them moves down, and the new ones' references with it
        std::vector<bool> drop(objs.size(), false);
        for (const MeshRef& m : meshes) {
            drop[m.obj] = true;
            VbInfo v = vbInfo(m.vb);
            drop[m.vb] = true;
            drop[m.ib] = true;
            for (uint32_t a : v.run) if (a && a < drop.size()) drop[a] = true;
        }
        std::vector<uint32_t> newIndex(objs.size(), 0);
        std::vector<Obj> kept(1);
        std::vector<size_t> keptSec(1, 0);
        // the material list stays where Real Racing 3 writes it: last
        for (int pass = 0; pass < 2; ++pass)
            for (size_t i = 1; i < objs.size(); ++i) {
                if (drop[i] || (objs[i].type == 24) != (pass == 1)) continue;
                newIndex[i] = (uint32_t)kept.size();
                kept.push_back(std::move(objs[i]));
                keptSec.push_back(sectionOf[i]);
            }
        auto re = [&](Bytes& b, size_t at) {
            if (at + 4 > b.size()) return;
            uint32_t a = get32(&b[at]);
            if (a && a < newIndex.size()) set32(b, at, newIndex[a]);
        };
        for (Obj& o : kept) {
            if (o.type == 21 && o.data.size() >= 48) {
                re(o.data, 16); re(o.data, 36); re(o.data, 40);
                uint32_t sets = get32(&o.data[44]);
                for (uint32_t s2 = 0; s2 < sets && s2 < 16; ++s2) re(o.data, 48 + s2 * 20);
            } else if (o.type == 14) {
                size_t na, ne, ma;
                if (!meshLayout(o.data, na, ne, ma)) continue;
                size_t r = ne + 50;
                re(o.data, r);
                uint32_t subs = get32(&o.data[r + 4]);
                for (uint32_t s2 = 0; s2 < subs && s2 < 64; ++s2) { re(o.data, r + 8 + s2 * 8); re(o.data, r + 12 + s2 * 8); }
            }
        }
        objs = std::move(kept);
        sectionOf = std::move(keptSec);
    }
    if (!replaced && !added) {
        report = "None of the model's meshes matched this file's mesh names.\n\n"
                 "Export the model from Monkey Tool first (OBJ or FBX), edit it, and keep "
                 "each object's name (LOD_A_BODY_mm_ext...).";
        if (!lines.empty()) report += "\n\n" + lines;
        return false;
    }

    // ---- write it back: header section, then the rest ----
    // new objects all went into the last section, which is where they stand
    std::vector<Bytes> secData((size_t)sections);
    for (size_t i = 1; i < objs.size(); ++i) {
        size_t s = std::min(sectionOf[i], (size_t)sections - 1);
        if (i >= sectionOf.size()) s = (size_t)sections - 1;
        Bytes& b = secData[s];
        b.push_back(objs[i].type);
        put32(b, (uint32_t)objs[i].data.size());
        b.insert(b.end(), objs[i].data.begin(), objs[i].data.end());
    }
    // (new objects take their template's section - Real Racing 3 keeps
    // everything but the header in the last one, so they land after all
    // they refer to)
    size_t total = 12;
    for (const Bytes& b : secData) total += 9 + b.size() + 4;
    // the header object (type 0) states the file's size twice
    for (size_t i = 1; i < objs.size(); ++i)
        if (objs[i].type == 0 && objs[i].data.size() >= 11) {
            size_t s = sectionOf[i];
            Bytes& b = secData[s];
            // find it again inside its section
            size_t q = 0;
            for (size_t j = 1; j < i; ++j) if (sectionOf[j] == s) q += 5 + objs[j].data.size();
            set32(b, q + 5 + 3, (uint32_t)total);
            set32(b, q + 5 + 7, (uint32_t)total);
            break;
        }
    out.assign(d, d + 12);
    for (const Bytes& b : secData) {
        size_t start = out.size();
        out.push_back(0);
        put32(out, (uint32_t)(b.size() + 13));
        put32(out, (uint32_t)b.size());
        out.insert(out.end(), b.begin(), b.end());
        put32(out, adler32(out.data() + start, out.size() - start));
    }
    char buf[400];
    if (build) snprintf(buf, sizeof(buf), "%d mesh(es) written (%d damaged or broken parts copied from their "
                        "intact ones, %d tiny stand-ins inside the body for parts of the original car the model has no counterpart for)",
                        added, damageCopies, keptOriginal);
    else snprintf(buf, sizeof(buf), "%d mesh(es) replaced, %d added", replaced, added);
    report = buf;
    if (skipped) report += ", " + std::to_string(skipped) + " wheel cop(ies) the viewer made left out";
    report += ".";
    if (unitScale != 1.0) report += unitScale < 1 ? " The model was in centimetres and was scaled to metres." : " The model was scaled up 100 times to metres.";
    int untouched = 0;
    if (!build) for (bool u : used) if (!u) ++untouched;
    if (untouched) report += " " + std::to_string(untouched) + " mesh(es) of the file were not in the model and are unchanged.";
    if (!lines.empty()) report += "\n\n" + lines;
    return true;
}


// A new car: every object of the model becomes a mesh of its own, written on
// the frame of an existing Real Racing 3 car (its header, material list and
// mesh layout); the template's own meshes are left out.
bool rr3BuildCar(const Bytes& templateM3g, const Model& importedIn, Bytes& out, std::string& report) {
    Model m = importedIn;
    std::vector<std::string> notes;
    // size and axes: a car is 3-6 m long, lying along z (the game's fore
    // axis is -z), with y up
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (const Mesh& me : m.meshes)
        for (size_t i = 0; i + 2 < me.positions.size(); i += 3)
            for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], me.positions[i + k]); hi[k] = std::max(hi[k], me.positions[i + k]); }
    float ext[3] = { hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2] };
    float longest = std::max(ext[0], std::max(ext[1], ext[2]));
    if (!(longest > 0)) { report = "the model is empty"; return false; }
    double scale = 1.0;
    if (longest > 100) { scale = 0.01; notes.push_back("centimetres scaled to metres"); }
    else if (longest > 20) { scale = 0.0254; notes.push_back("inches scaled to metres"); }
    else if (longest < 0.2f) { scale = 100.0; notes.push_back("scaled up 100 times"); }
    // Z up (3ds Max): the car is long along y or x and its height is along z
    bool zUp = ext[2] < ext[1] && ext[2] < std::max(ext[0], ext[1]) * 0.6f && ext[1] > ext[0];
    bool alongX = !zUp && ext[0] > ext[2] * 1.3f && ext[0] > ext[1];
    if (zUp) notes.push_back("Z-up axes turned to Y-up");
    if (alongX) notes.push_back("turned to lie along the game's fore axis");
    int renamed = 0;
    std::map<std::string, int> seen;
    m.meshes.erase(std::remove_if(m.meshes.begin(), m.meshes.end(), [](const Mesh& me) {
        return me.lod == "HARDPOINTS" || me.indices.empty();
    }), m.meshes.end());
    for (Mesh& me : m.meshes) {
        for (size_t i = 0; i + 2 < me.positions.size(); i += 3) {
            float x = me.positions[i], y = me.positions[i + 1], z = me.positions[i + 2];
            if (zUp) { float ny = z, nz = -y; y = ny; z = nz; }
            if (alongX) { float nz = -x, nx = z; x = nx; z = nz; }
            me.positions[i] = (float)(x * scale); me.positions[i + 1] = (float)(y * scale); me.positions[i + 2] = (float)(z * scale);
        }
        for (size_t i = 0; i + 2 < me.normals.size(); i += 3) {
            float x = me.normals[i], y = me.normals[i + 1], z = me.normals[i + 2];
            if (zUp) { float ny = z, nz = -y; y = ny; z = nz; }
            if (alongX) { float nz = -x, nx = z; x = nx; z = nz; }
            me.normals[i] = x; me.normals[i + 1] = y; me.normals[i + 2] = z;
        }
        // the game finds a car's parts by name: LOD_A_ is the detailed one
        std::string n = me.name;
        for (char& c : n) if (c == ' ' || c == '-' || c == '.') c = '_';
        if (n.compare(0, 4, "LOD_") != 0) {
            // a detail level the model states (LOD00 ... from this tool's
            // own export) picks the letter; everything else is the detailed one
            char letter = 'A';
            if (me.lod.size() == 5 && me.lod.compare(0, 3, "LOD") == 0 && isdigit((unsigned char)me.lod[3]))
                letter = (char)('A' + std::min(25, atoi(me.lod.c_str() + 3)));
            n = std::string("LOD_") + letter + "_" + n;
            ++renamed;
        }
        int k = seen[n]++;
        if (k) n += "_" + std::to_string(k + 1);
        me.name = n;
    }
    if (renamed) notes.push_back(std::to_string(renamed) + " object name(s) given the LOD_A_ prefix the game looks for");
    // the car sits on the ground: lowest point at y = 0 as Real Racing 3's do
    float minY = 1e30f;
    for (const Mesh& me : m.meshes)
        for (size_t i = 1; i < me.positions.size(); i += 3) minY = std::min(minY, me.positions[i]);
    if (minY < 1e29f && std::fabs(minY) > 0.02f) {
        for (Mesh& me : m.meshes) for (size_t i = 1; i < me.positions.size(); i += 3) me.positions[i] -= minY;
        notes.push_back("moved to stand on the ground");
    }
    std::map<std::string, MeshPlacement> none;
    if (!replaceOrBuild(templateM3g, m, none, out, report, true)) return false;
    if (!notes.empty()) {
        std::string n;
        for (const std::string& x : notes) n += (n.empty() ? "" : "; ") + x;
        report += "\nModel: " + n + ".";
    }
    return true;
}


// ---- names the car does not know ----
namespace {
std::string normName(std::string x) {
    size_t dot = x.rfind('.');
    if (dot != std::string::npos && dot + 1 < x.size() &&
        x.find_first_not_of("0123456789", dot + 1) == std::string::npos) x.erase(dot);
    std::string o;
    for (char c : x) {
        if (c == ' ' || c == '_' || c == '-') continue;
        o += (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    }
    return o;
}
size_t editDistance(const std::string& a, const std::string& b) {
    std::vector<size_t> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        size_t prev = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            size_t cur = row[j];
            row[j] = std::min(std::min(row[j] + 1, row[j - 1] + 1), prev + (a[i - 1] == b[j - 1] ? 0 : 1));
            prev = cur;
        }
    }
    return row[b.size()];
}
// the atlas a name or material ends in: ..._mm_ext -> "ext"
std::string atlasOf(const std::string& n) {
    std::string l = lowerStr(n);
    size_t at = l.rfind("mm_");
    return at == std::string::npos ? std::string() : normName(l.substr(at + 3));
}
bool templateNames(const Bytes& file, std::vector<std::string>& parts, std::vector<std::string>& materials) {
    const uint8_t* d = file.data();
    size_t len = file.size(), p = 12;
    if (len < 12) return false;
    while (p + 9 <= len) {
        uint32_t plain = get32(d + p + 5);
        size_t q = p + 9, end = q + plain;
        if (d[p] != 0 || end + 4 > len) return false;
        while (q + 5 <= end) {
            uint8_t t = d[q];
            uint32_t l = get32(d + q + 1);
            if (q + 5 + l > end) return false;
            Bytes b(d + q + 5, d + q + 5 + l);
            if (t == 24 && b.size() >= 4) {
                uint32_t n = get32(b.data());
                size_t r = 4;
                for (uint32_t k = 0; k < n && r < b.size(); ++k) {
                    size_t e = r;
                    while (e < b.size() && b[e]) ++e;
                    materials.emplace_back((const char*)b.data() + r, e - r);
                    r = e + 1;
                }
            } else if (t == 14) {
                size_t na, ne, ma;
                if (meshLayout(b, na, ne, ma)) parts.emplace_back((const char*)b.data() + na, ne - 1 - na);
            }
            q += 5 + l;
        }
        p = end + 4;
    }
    return !parts.empty();
}
}

std::vector<NameFix> rr3SuggestNames(const Bytes& m3g, const Model& model, bool newCar) {
    std::vector<NameFix> out;
    std::vector<std::string> parts, materials;
    if (!templateNames(m3g, parts, materials)) return out;
    std::set<std::string> partSet(parts.begin(), parts.end()), matSet(materials.begin(), materials.end());
    std::set<std::string> seen;
    for (const Mesh& me : model.meshes) {
        if (me.indices.empty() || me.lod == "HARDPOINTS") continue;
        // the viewer's own wheel copies and hardpoints are not parts of the file
        if (me.name.compare(0, 6, "WHEEL_") == 0 || me.name.compare(0, 6, "POINT_") == 0) continue;
        // materials
        if (!me.material.empty() && !matSet.count(me.material) && !seen.count("m:" + me.material)) {
            seen.insert("m:" + me.material);
            std::string best;
            for (const std::string& m : materials) if (normName(m) == normName(me.material)) { best = m; break; }
            if (best.empty()) {
                std::string a = atlasOf(me.material);
                for (const std::string& m : materials) if (!a.empty() && atlasOf(m) == a) { best = m; break; }
            }
            if (best.empty()) {
                size_t bd = (size_t)-1;
                for (const std::string& m : materials) {
                    size_t dd = editDistance(normName(m), normName(me.material));
                    if (dd < bd) { bd = dd; best = m; }
                }
            }
            if (!best.empty()) out.push_back({ true, me.material, best });
        }
        // parts
        std::string name = me.name;
        size_t dot = name.rfind('.');
        if (dot != std::string::npos && name.find_first_not_of("0123456789", dot + 1) == std::string::npos) name.erase(dot);
        if (newCar && name.compare(0, 4, "LOD_") != 0) name = "LOD_A_" + name;
        if (partSet.count(name) || seen.count("p:" + me.name)) continue;
        seen.insert("p:" + me.name);
        // same part spelled differently (spaces, case) first
        std::string best;
        for (const std::string& p : parts) if (normName(p) == normName(name)) { best = p; break; }
        if (best.empty()) {
            // the same atlas (the texture it samples), the nearest part name
            std::string a = atlasOf(name);
            if (a.empty()) a = atlasOf(me.material);
            std::string base = lowerStr(name.substr(0, name.rfind("_mm_") == std::string::npos ? name.size() : name.rfind("_mm_")));
            size_t bd = (size_t)-1;
            for (const std::string& p : parts) {
                if (!a.empty() && atlasOf(p) != a) continue;
                std::string pb = lowerStr(p.substr(0, p.rfind("_mm_") == std::string::npos ? p.size() : p.rfind("_mm_")));
                size_t dd = editDistance(pb, base);
                if (dd < bd) { bd = dd; best = p; }
            }
        }
        if (!best.empty()) out.push_back({ false, me.name, best });
    }
    return out;
}

void rr3ApplyNameFixes(Model& model, const std::vector<NameFix>& fixes) {
    for (Mesh& me : model.meshes)
        for (const NameFix& f : fixes) {
            if (f.material && me.material == f.from) me.material = f.to;
            if (!f.material && me.name == f.from) me.name = f.to;
        }
}

} // namespace nfsnl
