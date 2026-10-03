// nfsnl_m3g.cpp - M3G (JSR-184 Mobile 3D Graphics) reader
//
// Older builds of the game shipped models as .m3g. Three container
// identifiers exist in the wild, all 12 bytes and all the same shape:
//
//     AB "JSR184" BB 0D 0A 1A 0A      stock JSR-184          (version 1)
//     AB "IM2M3G" BB 0D 0A 1A 0A      EA / Firemonkeys       (version 2)
//     AB "IM3M3G" BB 0D 0A 1A 0A      EA / Firemonkeys       (version 3)
//
// A file may additionally be wrapped in EA's little gzip header:
//     1F 8C  <uint32 uncompressedSize>  <gzip stream from offset 6>
//
// After the identifier comes a sequence of sections:
//     uint8   compressionScheme   0 = stored, 1 = zlib
//     uint32  totalSectionLength  counts these header fields and the checksum
//     uint32  uncompressedLength
//     bytes   objects             (zlib-compressed when scheme = 1)
//     uint32  checksum            (Adler32)
// and each object is:
//     uint8   objectType
//     uint32  length
//     bytes   payload
// Objects are numbered from 1 in file order and reference each other by
// that index.
//
// The object *payloads* differ between the three variants: every object
// starts with an Object3D header whose length depends on how many animation
// tracks and user parameters it carries, and the EA variants add fields of
// their own (the mesh name lives in a user parameter, for instance). Rather
// than hard-code one layout per variant, each payload is scanned for the
// header length at which the record parses and consumes the payload exactly.
// The records are rigid enough - component sizes, index encodings and
// cross-references to other objects all have to line up - that a wrong
// offset practically never validates.
#include "nfsnl.h"

#include <cctype>
#include <set>
#include <cstring>
#include <cmath>
#include <array>
#include <map>
#include <algorithm>

namespace nfsnl {

namespace {

enum ObjType {
    OBJ_TRISTRIP = 11,
    OBJ_MESH = 14,
    OBJ_VERTEXARRAY = 20,
    OBJ_VERTEXBUFFER = 21,
};

struct Reader {
    const uint8_t* d;
    size_t n, p;
    bool err = false;
    Reader(const uint8_t* dd, size_t nn, size_t start = 0) : d(dd), n(nn), p(start) {}
    bool need(size_t k) const { return p + k <= n; }
    uint8_t  u8()  { if (!need(1)) { err = true; return 0; } return d[p++]; }
    int8_t   i8()  { return (int8_t)u8(); }
    uint16_t u16() { if (!need(2)) { err = true; return 0; } uint16_t v; memcpy(&v, d + p, 2); p += 2; return v; }
    int16_t  i16() { if (!need(2)) { err = true; return 0; } int16_t v; memcpy(&v, d + p, 2); p += 2; return v; }
    uint32_t u32() { if (!need(4)) { err = true; return 0; } uint32_t v; memcpy(&v, d + p, 4); p += 4; return v; }
    float    f32() { if (!need(4)) { err = true; return 0; } float v; memcpy(&v, d + p, 4); p += 4; return v; }
    void skip(size_t k) { if (!need(k)) { err = true; p = n; } else p += k; }
};

// ------------------------------------------------------------- vertex array
struct VertexArray {
    int componentSize = 0;    // 1, 2 or 4 bytes
    int componentCount = 0;   // 2, 3 or 4
    int vertexCount = 0;
    std::vector<float> values;   // raw stored values, no bias/scale applied
    bool ok = false;
};

// Parses a VertexArray record assuming it starts at `at`. Returns false
// unless every field is sane and the record ends at the end of the payload.
bool tryVertexArray(const uint8_t* d, size_t n, size_t at, VertexArray& out) {
    Reader r(d, n, at);
    int compSize = r.u8();
    int compCount = r.u8();
    int encoding = r.u8();
    int vcount = r.u16();
    if (r.err) return false;
    if (compSize != 1 && compSize != 2 && compSize != 4) return false;
    if (compCount < 2 || compCount > 4) return false;
    if (encoding != 0 && encoding != 1) return false;
    if (vcount <= 0) return false;
    if (encoding == 1 && compSize == 4) return false;   // not defined

    size_t total = (size_t)vcount * compCount;
    size_t bytes = total * compSize;
    if (r.p + bytes > n) return false;
    // the record has to account for the payload; a few bytes of trailing
    // padding are tolerated, anything more means this is the wrong offset
    if (n - (r.p + bytes) > 8) return false;

    out = VertexArray();
    out.componentSize = compSize;
    out.componentCount = compCount;
    out.vertexCount = vcount;
    out.values.reserve(total);

    std::vector<double> prev(compCount, 0.0);
    for (size_t i = 0; i < total; ++i) {
        int c = (int)(i % (size_t)compCount);
        double v;
        if (compSize == 1)      v = (double)r.i8();
        else if (compSize == 2) v = (double)r.i16();
        else                    v = (double)r.f32();
        if (encoding == 1) { prev[c] += v; v = prev[c]; }
        out.values.push_back((float)v);
    }
    out.ok = !r.err;
    return out.ok;
}

// ------------------------------------------------------------ vertex buffer
struct VertexBuffer {
    uint32_t positions = 0, normals = 0, colors = 0;
    float posBias[3] = {0, 0, 0};
    float posScale = 1.0f;
    std::vector<uint32_t> texcoords;
    std::vector<std::array<float, 3>> texBias;
    std::vector<float> texScale;
    bool ok = false;
};

bool tryVertexBuffer(const uint8_t* d, size_t n, size_t at,
                     const std::map<uint32_t, VertexArray>& arrays,
                     VertexBuffer& out) {
    Reader r(d, n, at);
    r.skip(4);                       // default colour RGBA
    VertexBuffer vb;
    vb.positions = r.u32();
    vb.posBias[0] = r.f32();
    vb.posBias[1] = r.f32();
    vb.posBias[2] = r.f32();
    vb.posScale = r.f32();
    vb.normals = r.u32();
    vb.colors = r.u32();
    uint32_t texCount = r.u32();
    if (r.err) return false;
    if (texCount > 8) return false;

    // positions must name a vertex array with at least x/y/z
    auto it = arrays.find(vb.positions);
    if (it == arrays.end() || it->second.componentCount < 3) return false;
    if (!std::isfinite(vb.posScale) || vb.posScale == 0.0f) return false;
    for (float b : vb.posBias) if (!std::isfinite(b)) return false;

    for (uint32_t i = 0; i < texCount; ++i) {
        uint32_t ref = r.u32();
        std::array<float, 3> bias{r.f32(), r.f32(), r.f32()};
        float scale = r.f32();
        if (r.err) return false;
        if (ref != 0 && arrays.find(ref) == arrays.end()) return false;
        if (!std::isfinite(scale)) return false;
        vb.texcoords.push_back(ref);
        vb.texBias.push_back(bias);
        vb.texScale.push_back(scale);
    }
    if (n - r.p > 8) return false;   // must consume the payload

    vb.ok = true;
    out = vb;
    return true;
}

// -------------------------------------------------------- triangle strips
struct StripArray {
    std::vector<uint32_t> indices;   // expanded to a triangle list
    bool ok = false;
};

bool tryStripArray(const uint8_t* d, size_t n, size_t at, StripArray& out) {
    Reader r(d, n, at);
    int encoding = r.u8();
    if (r.err) return false;
    if (encoding != 0 && encoding != 1 && encoding != 2 &&
        encoding != 128 && encoding != 129 && encoding != 130) return false;

    uint32_t firstIndex = 0;
    std::vector<uint32_t> idx;
    if (encoding == 0)      firstIndex = r.u32();
    else if (encoding == 1) firstIndex = r.u8();
    else if (encoding == 2) firstIndex = r.u16();
    else {
        uint32_t count = r.u32();
        if (r.err || count == 0 || count > 4000000u) return false;
        size_t unit = (encoding == 128) ? 4 : (encoding == 129 ? 1 : 2);
        if (!r.need(count * unit)) return false;
        idx.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            if (encoding == 128)      idx.push_back(r.u32());
            else if (encoding == 129) idx.push_back(r.u8());
            else                      idx.push_back(r.u16());
        }
    }
    if (r.err) return false;

    uint32_t stripCount = r.u32();
    if (r.err || stripCount == 0 || stripCount > 200000u) return false;
    if (!r.need((size_t)stripCount * 4)) return false;
    std::vector<uint32_t> lengths;
    lengths.reserve(stripCount);
    uint64_t sum = 0;
    for (uint32_t i = 0; i < stripCount; ++i) {
        uint32_t L = r.u32();
        if (L < 3 || L > 4000000u) return false;
        sum += L;
        lengths.push_back(L);
    }
    if (!idx.empty() && sum != idx.size()) return false;
    if (n - r.p > 8) return false;   // must consume the payload

    StripArray s;
    size_t cursor = 0;
    for (uint32_t L : lengths) {
        for (uint32_t i = 0; i + 2 < L; ++i) {
            uint32_t a, b, c;
            if (idx.empty()) {
                a = firstIndex + (uint32_t)cursor + i;
                b = a + 1;
                c = a + 2;
            } else {
                if (cursor + i + 2 >= idx.size()) break;
                a = idx[cursor + i];
                b = idx[cursor + i + 1];
                c = idx[cursor + i + 2];
            }
            if (a == b || b == c || a == c) continue;   // degenerate joiner
            if (i & 1) std::swap(b, c);
            s.indices.push_back(a);
            s.indices.push_back(b);
            s.indices.push_back(c);
        }
        cursor += L;
    }
    s.ok = !s.indices.empty();
    if (!s.ok) return false;
    out = s;
    return true;
}

// ------------------------------------------------------------------- mesh
struct M3gMesh {
    uint32_t vertexBuffer = 0;
    std::vector<uint32_t> submeshes;
    std::string name;
};

// The Mesh payload ends with `vertexBuffer, submeshCount, (submesh,
// appearance) * submeshCount`. Everything before it is variant-specific, so
// the tail is located by looking for the first offset where those references
// all point at objects of the right type.
bool tryMesh(const uint8_t* d, size_t n, const std::map<uint32_t, int>& types,
             M3gMesh& out) {
    for (size_t at = 0; at + 8 <= n; ++at) {
        Reader r(d, n, at);
        uint32_t vb = r.u32();
        uint32_t count = r.u32();
        if (count == 0 || count > 256) continue;
        auto tv = types.find(vb);
        if (tv == types.end() || tv->second != OBJ_VERTEXBUFFER) continue;
        if (!r.need((size_t)count * 8)) continue;

        std::vector<uint32_t> subs;
        bool good = true;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t sub = r.u32();
            r.u32();                       // appearance, unused here
            auto ts = types.find(sub);
            if (ts == types.end() || ts->second != OBJ_TRISTRIP) { good = false; break; }
            subs.push_back(sub);
        }
        if (!good) continue;

        out = M3gMesh();
        out.vertexBuffer = vb;
        out.submeshes = subs;

        // the EA variants keep the mesh name in a user parameter ahead of
        // this tail; take the longest printable run that looks like a name
        size_t bestStart = 0, bestLen = 0, runStart = 0, runLen = 0;
        for (size_t i = 0; i < at; ++i) {
            uint8_t ch = d[i];
            bool nameChar = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
                            (ch >= 'a' && ch <= 'z') || ch == '_' || ch == '-' || ch == '.';
            if (nameChar) {
                if (runLen == 0) runStart = i;
                ++runLen;
                if (runLen > bestLen) { bestLen = runLen; bestStart = runStart; }
            } else {
                runLen = 0;
            }
        }
        if (bestLen >= 3)
            out.name.assign((const char*)d + bestStart, bestLen);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- container
struct RawObject { int type = 0; Bytes payload; };

int identifierVersion(const uint8_t* d, size_t len) {
    if (len < 12) return 0;
    if (d[0] != 0xAB || d[7] != 0xBB ||
        d[8] != 0x0D || d[9] != 0x0A || d[10] != 0x1A || d[11] != 0x0A)
        return 0;
    if (!memcmp(d + 1, "JSR184", 6)) return 1;
    if (!memcmp(d + 1, "IM2M3G", 6)) return 2;
    if (!memcmp(d + 1, "IM3M3G", 6)) return 3;
    if (!memcmp(d + 1, "IM4M3G", 6)) return 4;
    // Need for Speed: Hot Pursuit (2010, mobile) spells it with a hyphen;
    // the layout is Most Wanted's
    if (!memcmp(d + 1, "IM-M3G", 6)) return 5;
    return 0;
}

bool isEaGzipWrapper(const uint8_t* d, size_t len) {
    return len > 18 && d[0] == 0x1F && d[1] == 0x8C && d[6] == 0x1F && d[7] == 0x8B;
}

// No Limits wraps its .m3g files in a ten-byte header of its own:
//
//     DA BD  <uint32 uncompressed size>  <uint32 compressed size>
//
// and the payload follows. Verified on every sample seen: file size is
// always exactly 10 + compressed size.
bool readNflWrapper(const uint8_t* d, size_t len, uint32_t& uncompressed,
                    const uint8_t*& payload, size_t& payloadLen) {
    if (len < 11 || d[0] != 0xDA || d[1] != 0xBD) return false;
    uint32_t unc, comp;
    memcpy(&unc, d + 2, 4);
    memcpy(&comp, d + 6, 4);
    if (comp == 0 || (size_t)comp + 10 != len) return false;
    if (unc < comp || unc > (1u << 30)) return false;
    uncompressed = unc;
    payload = d + 10;
    payloadLen = comp;
    return true;
}

// Hook for the wrapper's payload codec. Nothing standard decodes it, so this
// is where a decoder plugs in once the codec is known - the same way the
// Zstandard library is picked up at runtime for models.
bool decompressM3gPayload(const uint8_t* src, size_t srcLen, size_t expected, Bytes& out) {
    // LZHAM first: it is what the game actually uses. The engine vendors it
    // (lzham_symbol_codec.cpp is named inside libapp.so), the asset format
    // enumerates CompressedLZHAM, and the game's own LZHAM cabinets carry the
    // same stream signature as these payloads.
    if (lzhamAvailable() && lzhamDecompress(src, srcLen, expected, out) &&
        out.size() == expected)
        return true;
    out.clear();
    // zstd is already loaded dynamically for .sb3d, so try it: costs nothing
    // and covers the case where a build switches codec.
    if (zstdAvailable() && zstdDecompress(src, srcLen, expected, out) && out.size() == expected)
        return true;
    out.clear();
    if (inflateRaw(src, srcLen, out) && out.size() == expected) return true;
    out.clear();
    if (inflateZlib(src, srcLen, out) && out.size() == expected) return true;
    out.clear();
    return false;
}

} // namespace

bool isM3g(const uint8_t* data, size_t len) {
    if (identifierVersion(data, len)) return true;
    if (isEaGzipWrapper(data, len)) return true;
    uint32_t unc; const uint8_t* p; size_t n;
    return readNflWrapper(data, len, unc, p, n);
}

int identifierVersionOf(const uint8_t* data, size_t len) {
    return identifierVersion(data, len);
}

bool m3gWrapperInfo(const uint8_t* data, size_t len, size_t* uncompressedSize) {
    uint32_t unc; const uint8_t* p; size_t n;
    if (!readNflWrapper(data, len, unc, p, n)) return false;
    if (uncompressedSize) *uncompressedSize = unc;
    return true;
}

bool m3gUnwrap(const uint8_t* data, size_t len, Bytes& out, std::string* error) {
    out.clear();
    uint32_t unc; const uint8_t* payload; size_t payloadLen;
    if (readNflWrapper(data, len, unc, payload, payloadLen)) {
        if (!decompressM3gPayload(payload, payloadLen, unc, out) || out.empty()) {
            out.clear();
            if (error)
                *error = lzhamAvailable()
                    ? "this .m3g is LZHAM-compressed and an LZHAM library is loaded, "
                      "but the stream did not decode - please send this file"
                    : "this .m3g is LZHAM-compressed (the game's DA BD wrapper, " +
                      std::to_string(unc) + " bytes inside). Put lzham_x64.dll next "
                      "to MonkeyTool.exe (BUILD_LZHAM.bat builds it) and it will convert.";
            return false;
        }
        return true;
    }
    if (isEaGzipWrapper(data, len)) {
        if (!inflateGzip(data + 6, len - 6, out) || out.empty()) {
            out.clear();
            if (error) *error = "the gzip wrapper failed to decompress";
            return false;
        }
        return true;
    }
    return false;
}

namespace {
// Real Racing 1 and GTI: some files (the skies, the wheels) keep their one
// texture's name where the appearance readers do not look; when a single
// picture is named anywhere in the file, it is the one
void nameLoneTexture(Model& model, const uint8_t* data, size_t len) {
    bool bare = false;
    for (const Mesh& m : model.meshes) if (m.texture.empty()) bare = true;
    if (!bare) return;
    std::set<std::string> found;
    for (size_t i = 1; i + 4 <= len; ++i) {
        if (data[i] != '.') continue;
        std::string ext((const char*)data + i + 1, 3);
        for (char& c : ext) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (ext != "png" && ext != "pvr" && ext != "tga" && ext != "bmp") continue;
        size_t b = i;
        while (b > 0 && (isalnum(data[b - 1]) || data[b - 1] == '_' || data[b - 1] == '-' || data[b - 1] == '/')) --b;
        if (i - b >= 2) found.insert(std::string((const char*)data + b, i + 4 - b));
    }
    if (found.size() == 1)
        for (Mesh& m : model.meshes) if (m.texture.empty()) m.texture = *found.begin();
}
}

Model loadM3g(const uint8_t* data, size_t len, bool flipV) {
    Model model;

    // ---- unwrap No Limits' own container if present ----
    Bytes unwrapped;
    {
        uint32_t unc; const uint8_t* payload; size_t payloadLen;
        if (readNflWrapper(data, len, unc, payload, payloadLen)) {
            // The payload is entropy coded - 7.995 bits per byte, no literal
            // runs, and it matches none of deflate, zlib, gzip, bzip2, xz,
            // lzma, lzma2, zstd (including magicless), lz4 or brotli. The
            // pack format this game uses lists LZHAM among its cabinet
            // codecs, which is the most likely answer, but nothing here can
            // confirm it. Decompress it elsewhere and this reader takes the
            // result; until then, say so plainly rather than failing oddly.
            Bytes plain;
            if (decompressM3gPayload(payload, payloadLen, unc, plain) && !plain.empty()) {
                unwrapped.swap(plain);
                data = unwrapped.data();
                len = unwrapped.size();
            } else {
                model.warnings.push_back(
                    lzhamAvailable()
                    ? "this .m3g is LZHAM-compressed and lzham_x64.dll is present, "
                      "but the stream did not decode - please send this file"
                    : "this .m3g is LZHAM-compressed (the game's DA BD wrapper, " +
                      std::to_string(unc) + " bytes inside). Put lzham_x64.dll next "
                      "to MonkeyTool.exe and it will convert - see the README.");
                return model;
            }
        }
    }
    if (isEaGzipWrapper(data, len)) {
        if (!inflateGzip(data + 6, len - 6, unwrapped) || unwrapped.empty()) {
            model.warnings.push_back("M3G: the gzip wrapper failed to decompress");
            return model;
        }
        data = unwrapped.data();
        len = unwrapped.size();
    }

    int version = identifierVersion(data, len);
    if (!version) {
        model.warnings.push_back("not an M3G file");
        return model;
    }

    // ---- collect every object across all sections ----
    std::vector<RawObject> objects;
    size_t p = 12;
    while (p + 9 <= len) {
        uint8_t scheme = data[p];
        uint32_t totalLen, uncompLen;
        memcpy(&totalLen, data + p + 1, 4);
        memcpy(&uncompLen, data + p + 5, 4);
        (void)uncompLen;
        if (totalLen < 13 || p + totalLen > len) break;

        const uint8_t* body = data + p + 9;
        size_t bodyLen = (size_t)totalLen - 13;   // minus the 9-byte header and the checksum
        Bytes inflated;
        const uint8_t* objs = body;
        size_t objsLen = bodyLen;
        if (scheme == 1) {
            if (!inflateZlib(body, bodyLen, inflated)) {
                model.warnings.push_back("an M3G section failed to decompress");
                break;
            }
            objs = inflated.data();
            objsLen = inflated.size();
        }

        size_t q = 0;
        while (q + 5 <= objsLen) {
            uint8_t type = objs[q];
            uint32_t olen;
            memcpy(&olen, objs + q + 1, 4);
            if (olen > objsLen || q + 5 + olen > objsLen) break;
            RawObject o;
            o.type = type;
            o.payload.assign(objs + q + 5, objs + q + 5 + olen);
            objects.push_back(std::move(o));
            q += 5 + (size_t)olen;
        }
        p += totalLen;
    }

    if (objects.empty()) {
        model.warnings.push_back("no objects found in the M3G file");
        return model;
    }

    // Objects are referenced by their 1-based position in file order.
    std::map<uint32_t, int> types;
    for (size_t i = 0; i < objects.size(); ++i) types[(uint32_t)i + 1] = objects[i].type;

    // The Object3D header length varies, so each record is located by trying
    // successive start offsets and keeping the one that validates.
    const size_t kMaxHeader = 128;

    std::map<uint32_t, VertexArray> arrays;
    for (size_t i = 0; i < objects.size(); ++i) {
        if (objects[i].type != OBJ_VERTEXARRAY) continue;
        const Bytes& pl = objects[i].payload;
        VertexArray va;
        for (size_t at = 0; at <= kMaxHeader && at + 5 <= pl.size(); ++at) {
            if (tryVertexArray(pl.data(), pl.size(), at, va)) {
                arrays[(uint32_t)i + 1] = std::move(va);
                break;
            }
        }
    }

    std::map<uint32_t, VertexBuffer> buffers;
    for (size_t i = 0; i < objects.size(); ++i) {
        if (objects[i].type != OBJ_VERTEXBUFFER) continue;
        const Bytes& pl = objects[i].payload;
        VertexBuffer vb;
        for (size_t at = 0; at <= kMaxHeader && at + 40 <= pl.size(); ++at) {
            if (tryVertexBuffer(pl.data(), pl.size(), at, arrays, vb)) {
                buffers[(uint32_t)i + 1] = std::move(vb);
                break;
            }
        }
    }

    std::map<uint32_t, StripArray> strips;
    for (size_t i = 0; i < objects.size(); ++i) {
        if (objects[i].type != OBJ_TRISTRIP) continue;
        const Bytes& pl = objects[i].payload;
        StripArray sa;
        for (size_t at = 0; at <= kMaxHeader && at + 5 <= pl.size(); ++at) {
            if (tryStripArray(pl.data(), pl.size(), at, sa)) {
                strips[(uint32_t)i + 1] = std::move(sa);
                break;
            }
        }
    }

    std::vector<M3gMesh> meshes;
    for (size_t i = 0; i < objects.size(); ++i) {
        if (objects[i].type != OBJ_MESH) continue;
        M3gMesh m;
        if (tryMesh(objects[i].payload.data(), objects[i].payload.size(), types, m)) {
            if (m.name.empty()) m.name = "mesh_" + std::to_string(i + 1);
            meshes.push_back(std::move(m));
        }
    }

    // Fallback for files whose Mesh records did not validate: attach to each
    // vertex buffer the strip arrays that sit between it and the next one.
    if (meshes.empty() && !buffers.empty()) {
        for (auto it = buffers.begin(); it != buffers.end(); ++it) {
            uint32_t nextVb = UINT32_MAX;
            auto nx = it; ++nx;
            if (nx != buffers.end()) nextVb = nx->first;
            M3gMesh m;
            m.vertexBuffer = it->first;
            m.name = "mesh_" + std::to_string(it->first);
            for (auto& sp : strips)
                if (sp.first > it->first && sp.first < nextVb) m.submeshes.push_back(sp.first);
            if (!m.submeshes.empty()) meshes.push_back(std::move(m));
        }
        if (!meshes.empty())
            model.warnings.push_back(
                "M3G: mesh records did not validate, geometry was paired by object order");
    }

    // Real Racing 1 and GTI state the UV scale as it is (raw coordinates
    // times it span 0..1); Real Racing 2 and 3 state four times the real one.
    // The file's largest coordinate says which.
    bool statedUv = false;
    if (version == 1) {
        double span = 0;
        for (const auto& b : buffers) {
            const VertexBuffer& vb = b.second;
            if (vb.texcoords.empty() || !vb.texcoords[0] || vb.texScale.empty()) continue;
            auto ti = arrays.find(vb.texcoords[0]);
            if (ti == arrays.end()) continue;
            for (float x : ti->second.values) span = std::max(span, std::fabs((double)x * vb.texScale[0]));
        }
        statedUv = span > 0 && span <= 1.1;
    }

    // ---- build the output ----
    for (const M3gMesh& src : meshes) {
        auto bi = buffers.find(src.vertexBuffer);
        if (bi == buffers.end()) continue;
        const VertexBuffer& vb = bi->second;
        auto pi = arrays.find(vb.positions);
        if (pi == arrays.end()) continue;
        const VertexArray& pos = pi->second;
        int vcount = pos.vertexCount;
        if (vcount <= 0) continue;

        Mesh mesh;
        mesh.name = src.name;
        mesh.positions.reserve((size_t)vcount * 3);
        for (int v = 0; v < vcount; ++v) {
            const float* s = &pos.values[(size_t)v * pos.componentCount];
            mesh.positions.push_back(s[0] * vb.posScale + vb.posBias[0]);
            mesh.positions.push_back(s[1] * vb.posScale + vb.posBias[1]);
            mesh.positions.push_back(s[2] * vb.posScale + vb.posBias[2]);
        }

        auto ni = arrays.find(vb.normals);
        if (vb.normals && ni != arrays.end() && ni->second.vertexCount == vcount &&
            ni->second.componentCount >= 3) {
            const VertexArray& nr = ni->second;
            for (int v = 0; v < vcount; ++v) {
                const float* s = &nr.values[(size_t)v * nr.componentCount];
                float x = s[0], y = s[1], z = s[2];
                float l = std::sqrt(x * x + y * y + z * z);
                if (l > 0) { x /= l; y /= l; z /= l; } else { x = 0; y = 0; z = 1; }
                mesh.normals.push_back(x);
                mesh.normals.push_back(y);
                mesh.normals.push_back(z);
            }
        }

        // per-vertex colour: bytes, read back as signed above, so undo that
        auto ci = arrays.find(vb.colors);
        if (vb.colors && ci != arrays.end() && ci->second.vertexCount == vcount &&
            ci->second.componentSize == 1 && ci->second.componentCount >= 3) {
            const VertexArray& ca = ci->second;
            mesh.colors.reserve((size_t)vcount * 4);
            for (int v = 0; v < vcount; ++v) {
                const float* s = &ca.values[(size_t)v * ca.componentCount];
                for (int k = 0; k < 4; ++k) {
                    float x = k < ca.componentCount ? s[k] : 255.0f;
                    if (x < 0) x += 256.0f;
                    mesh.colors.push_back(x / 255.0f);
                }
            }
        }

        if (!vb.texcoords.empty() && vb.texcoords[0]) {
            auto ti = arrays.find(vb.texcoords[0]);
            if (ti != arrays.end() && ti->second.vertexCount == vcount &&
                ti->second.componentCount >= 2) {
                const VertexArray& tc = ti->second;
                float scale = vb.texScale[0];
                // stock JSR-184 files from this engine store texture
                // coordinates at four times the scale the field states; the
                // EA-identified variants do not. M3G2FBX applies the same
                // correction and is the reference for these files.
                if (version == 1 && !statedUv) scale /= 4.0f;
                float bu = vb.texBias[0][0], bv = vb.texBias[0][1];
                mesh.uvs.reserve((size_t)vcount * 2);
                for (int v = 0; v < vcount; ++v) {
                    const float* s = &tc.values[(size_t)v * tc.componentCount];
                    float u = s[0] * scale + bu;
                    float w = s[1] * scale + bv;
                    mesh.uvs.push_back(u);
                    mesh.uvs.push_back(flipV ? (1.0f - w) : w);
                }
            }
        }

        for (uint32_t subId : src.submeshes) {
            auto si = strips.find(subId);
            if (si == strips.end()) continue;
            for (uint32_t i : si->second.indices)
                if ((int)i < vcount) mesh.indices.push_back(i);
        }

        if (mesh.indices.empty() || mesh.positions.empty()) continue;
        if (mesh.normals.size() != mesh.positions.size()) mesh.normals.clear();
        if (mesh.uvs.size() / 2 != mesh.positions.size() / 3) mesh.uvs.clear();
        model.meshes.push_back(std::move(mesh));
    }

    model.valid = !model.meshes.empty();
    if (!model.valid)
        model.warnings.push_back("the M3G file parsed but produced no usable meshes");
    return model;
}

Model loadModel(const uint8_t* data, size_t len, bool flipV) {
    // Real Racing 3's .m3g is a genuine JSR-184 file and has an exact reader
    // of its own, checked against the geometry it produces. The reader below
    // is the general one, which finds its offsets by trial; it stays as the
    // fallback for the EA variants and for anything the exact reader declines.
    //
    // Most Wanted's IM2M3G and No Limits' IM4M3G (inside the DA BD LZHAM
    // wrapper) are a different layout again and have their own reader.
    Bytes plain;
    std::string unwrapError;
    bool wrapped = m3gUnwrap(data, len, plain, &unwrapError);
    if (!wrapped && !unwrapError.empty()) {
        Model m;
        m.warnings.push_back(unwrapError);
        return m;
    }
    const uint8_t* d = wrapped ? plain.data() : data;
    size_t n = wrapped ? plain.size() : len;
    int version = identifierVersion(d, n);
    if (version >= 2) {
        Model im = loadImM3g(d, n, version, flipV);
        if (im.valid || version >= 4) return im;
    }
    if (isJsr184(d, n)) {
        Model exact = loadJsr184(d, n, flipV);
        if (exact.valid) { nameLoneTexture(exact, d, n); return exact; }
        // by the book (NFS Undercover, Shift): the scene graph's transforms
        // and the textures inside
        Model scene = loadJsr184Scene(d, n);
        if (scene.valid) return scene;
    }
    if (isM3g(data, len)) {
        Model m = loadM3g(data, len, flipV);
        nameLoneTexture(m, d, n);
        return m;
    }
    return loadSb3d(data, len, flipV);
}

} // namespace nfsnl
