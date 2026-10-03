// nfsnl_sbin.cpp - reading the objects inside an SBIN file, and No Limits'
// encrypted .sb data files
//
// SBIN is the Firemonkeys container for everything that is not a sound: data
// (.sb), textures (.sba) and models (.sb3d). Two versions are in use:
//
//   version 3  Need for Speed: Most Wanted (2012, mobile)
//   version 4  Need for Speed: No Limits, Real Racing 3
//
// Both are a run of chunks - tag, length, FNV-1 (32-bit) hash of the data,
// the data, padding to four bytes - and both describe their objects with the
// same tables: STRU (structures), FIEL (their fields), ENUM, OHDR (where each
// object starts or ends in DATA) and CHDR/CDAT (the string table).
//
// What version 3 means field by field comes from Hypercycle's
// NFSMW12MobileTools (github.com/hypernucle/NFSMW12MobileTools, GPL-3.0),
// which unpacks and repacks Most Wanted's files: the field type numbers, the
// three map kinds, the OHDR end offsets, the ENUM -> member-map link and the
// FNV-1 chunk hash. Version 4's object layout (typed key/value objects and
// arrays) is the one the save editor and the model reader already use.
//
// No Limits encrypts most of its data files. They are the save files' cipher
// (AES-256-CBC, the IV from the file name) around "ZBDS" + size + gzip.
#include "nfsnl.h"
#include "nfsnl_save.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <set>

namespace nfsnl {

uint32_t sbinFnv1(const uint8_t* d, size_t n) {
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < n; ++i) { h *= 16777619u; h ^= d[i]; }
    return h;
}

void sbinRehash(Bytes& sbin) {
    size_t pos = 8;
    while (pos + 12 <= sbin.size()) {
        uint32_t len;
        memcpy(&len, sbin.data() + pos + 4, 4);
        if (pos + 12 + len > sbin.size()) break;
        uint32_t h = sbinFnv1(sbin.data() + pos + 12, len);
        memcpy(sbin.data() + pos + 8, &h, 4);
        pos = (pos + 12 + len + 3) & ~(size_t)3;
    }
}

// ------------------------------------------------------------ .sb data files

bool nlSbDecode(const std::string& fileName, const uint8_t* d, size_t n, Bytes& sbin,
                std::string* how) {
    sbin.clear();
    if (n >= 8 && !memcmp(d, "SBIN", 4)) {
        sbin.assign(d, d + n);
        if (how) *how = "plain SBIN";
        return true;
    }
    Bytes plain;
    std::string err;
    Bytes c(d, d + n);
    if (!saves::nfsDecryptAny(baseName(fileName), c, plain, err)) {
        if (how) *how = err;
        return false;
    }
    if (plain.size() >= 8 && !memcmp(plain.data(), "SBIN", 4)) {
        sbin.swap(plain);
        if (how) *how = "encrypted SBIN";
        return true;
    }
    if (plain.size() >= 10 && !memcmp(plain.data(), "ZBDS", 4)) {
        uint32_t want;
        memcpy(&want, plain.data() + 4, 4);
        Bytes out;
        if (inflateGzip(plain.data() + 8, plain.size() - 8, out) &&
            out.size() >= 8 && !memcmp(out.data(), "SBIN", 4)) {
            if (how) *how = want == out.size() ? "encrypted, ZBDS + gzip"
                                               : "encrypted, ZBDS + gzip (size field differs)";
            sbin.swap(out);
            return true;
        }
        if (how) *how = "encrypted ZBDS, but the gzip inside did not unpack";
        return false;
    }
    if (how) *how = "decrypted, but what is inside is not SBIN";
    return false;
}

Bytes nlSbEncode(const std::string& fileName, const Bytes& sbin) {
    Bytes fixed = sbin;
    sbinRehash(fixed);
    Bytes z = gzipCompress(fixed.data(), fixed.size());
    Bytes plain = { 'Z', 'B', 'D', 'S' };
    uint32_t n = (uint32_t)fixed.size();
    for (int k = 0; k < 4; ++k) plain.push_back((uint8_t)(n >> (8 * k)));
    plain.insert(plain.end(), z.begin(), z.end());
    return saves::nfsEncryptAny(baseName(fileName), plain);
}

// ------------------------------------------------------------ object text

namespace {

void addf(std::string& s, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s += buf;
}

std::string quoted(const std::string& v) {
    std::string o = "\"";
    for (char c : v) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 32) { char b[8]; snprintf(b, sizeof(b), "\\x%02X", (unsigned char)c); o += b; }
        else o += c;
    }
    return o + "\"";
}

struct Sbin {
    const uint8_t* d = nullptr;
    size_t len = 0;
    uint32_t version = 0;
    std::vector<SbinChunk> chunks;
    std::vector<std::string> names;
    const SbinChunk *stru = nullptr, *fiel = nullptr, *enm = nullptr,
                    *ohdr = nullptr, *data = nullptr;
    std::string name(size_t i) const { return i < names.size() ? names[i] : std::string("?"); }
    uint16_t u16(const SbinChunk* c, size_t o) const {
        if (!c || o + 2 > c->size) return 0;
        uint16_t v; memcpy(&v, c->data + o, 2); return v;
    }
    uint32_t u32(const SbinChunk* c, size_t o) const {
        if (!c || o + 4 > c->size) return 0;
        uint32_t v; memcpy(&v, c->data + o, 4); return v;
    }
    bool load(const uint8_t* p, size_t n) {
        d = p; len = n;
        if (n < 12 || memcmp(p, "SBIN", 4)) return false;
        memcpy(&version, p + 4, 4);
        chunks = sbinChunks(p, n);
        names = sbinNames(chunks);
        stru = findChunk(chunks, "STRU");
        fiel = findChunk(chunks, "FIEL");
        enm  = findChunk(chunks, "ENUM");
        ohdr = findChunk(chunks, "OHDR");
        data = findChunk(chunks, "DATA");
        return true;
    }
};

// ---- version 3 (Most Wanted 2012) ----
//
// Field types as the game binary numbers them.
const char* v3TypeName(int t) {
    switch (t) {
        case 0x1: return "int8";     case 0x2: return "uint8";
        case 0x3: return "int16";    case 0x4: return "uint16";
        case 0x5: return "int32";    case 0x6: return "uint32";
        case 0x7: return "int64";    case 0x8: return "uint64";
        case 0x9: return "bool";     case 0xA: return "float";
        case 0xB: return "double";   case 0xC: return "char";
        case 0xD: return "string";   case 0xE: return "pod";
        case 0xF: return "ref";      case 0x10: return "struct";
        case 0x11: return "array";   case 0x12: return "enum";
        case 0x13: return "bitfield"; case 0x14: return "symbol";
        case 0x16: return "bulk";
    }
    return nullptr;
}

struct V3Field { std::string name; int type = 0; int offset = 0; int spec = 0; int size = 0; };
struct V3Struct { std::string name; std::vector<V3Field> fields; };

void v3Objects(const Sbin& S, std::string& s, size_t limit) {
    // structures and their fields; a field's size is the gap to the next one
    std::vector<V3Struct> structs;
    size_t nStru = S.stru ? S.stru->size / 6 : 0, nFiel = S.fiel ? S.fiel->size / 8 : 0;
    for (size_t i = 0; i < nStru; ++i) {
        V3Struct st;
        st.name = S.name(S.u16(S.stru, i * 6));
        size_t first = S.u16(S.stru, i * 6 + 2), count = S.u16(S.stru, i * 6 + 4);
        if (!count) count = 1;
        for (size_t f = first; f < first + count && f < nFiel; ++f) {
            V3Field fl;
            fl.name = S.name(S.u16(S.fiel, f * 8));
            fl.type = S.u16(S.fiel, f * 8 + 2);
            fl.offset = S.u16(S.fiel, f * 8 + 4);
            fl.spec = S.u16(S.fiel, f * 8 + 6);
            st.fields.push_back(fl);
        }
        for (size_t k = 0; k < st.fields.size(); ++k)
            st.fields[k].size = k + 1 < st.fields.size()
                                    ? st.fields[k + 1].offset - st.fields[k].offset : -1;
        structs.push_back(std::move(st));
    }

    // objects: OHDR holds each one's end in DATA, times eight; the first
    // entry is always 1 and is not an object
    std::vector<std::pair<size_t, size_t>> spans;
    size_t nO = S.ohdr ? S.ohdr->size / 4 : 0;
    size_t prev = 0;
    for (size_t i = 1; i <= nO; ++i) {
        size_t end = i < nO ? S.u32(S.ohdr, i * 4) / 8 : S.data ? S.data->size : 0;
        if (end < prev || !S.data || end > S.data->size) break;
        spans.push_back({ prev, end });
        prev = end;
    }
    const uint8_t* D = S.data ? S.data->data : nullptr;

    // enums: each names the DATA object that lists its members (a map of
    // string ids)
    std::vector<std::vector<std::string>> enumMembers;
    size_t nEnum = S.enm ? S.enm->size / 8 : 0;
    for (size_t e = 0; e < nEnum; ++e) {
        std::vector<std::string> members;
        size_t ref = S.u16(S.enm, e * 8 + 4);
        if (ref < spans.size() && D) {
            size_t a = spans[ref].first, b = spans[ref].second;
            if (b - a >= 8) {
                uint32_t cnt; memcpy(&cnt, D + a + 4, 4);
                for (uint32_t k = 0; k < cnt && a + 8 + k * 2 + 2 <= b && k < 4096; ++k) {
                    uint16_t id; memcpy(&id, D + a + 8 + k * 2, 2);
                    members.push_back(S.name(id));
                }
            }
        }
        enumMembers.push_back(std::move(members));
    }

    auto fieldValue = [&](const V3Field& f, const uint8_t* p, size_t avail) -> std::string {
        int sz = f.size > 0 ? f.size : (int)avail;
        if (sz <= 0 || (size_t)sz > avail) return "(outside the object)";
        auto i32 = [&]() { int32_t v; memcpy(&v, p, 4); return v; };
        auto u16v = [&]() { uint16_t v; memcpy(&v, p, 2); return v; };
        char b[64];
        switch (f.type) {
            case 0x5: case 0x6: case 0x16:
                if (sz >= 4) { snprintf(b, sizeof(b), "%d", i32()); return b; } break;
            case 0x3: case 0x4:
                if (sz >= 2) { snprintf(b, sizeof(b), "%d", f.type == 3 ? (int)(int16_t)u16v() : (int)u16v()); return b; } break;
            case 0xA:
                if (sz >= 4) { float v; memcpy(&v, p, 4); snprintf(b, sizeof(b), "%g", v); return b; } break;
            case 0xB:
                if (sz >= 8) { double v; memcpy(&v, p, 8); snprintf(b, sizeof(b), "%g", v); return b; } break;
            case 0x9: {
                int v = sz == 1 ? p[0] : u16v();
                if (v < 2) return v ? "true" : "false";
                break;
            }
            case 0xC: return quoted(std::string(1, (char)p[0]));
            case 0xD: case 0x14:
                if (sz >= 2) return quoted(S.name(u16v()));
                break;
            case 0xF: case 0x11:
                if (sz >= 2) { snprintf(b, sizeof(b), "#%u", (unsigned)u16v()); return b; }
                break;
            case 0x12:
                if (sz >= 4) {
                    int v = i32();
                    if ((size_t)f.spec < enumMembers.size() && v >= 0 &&
                        (size_t)v < enumMembers[f.spec].size())
                        return enumMembers[f.spec][v];
                    snprintf(b, sizeof(b), "%d", v);
                    return b;
                }
                break;
        }
        std::string h;
        for (int k = 0; k < sz && k < 32; ++k) { snprintf(b, sizeof(b), "%02X", p[k]); h += b; }
        return "0x" + h;
    };

    std::function<void(const V3Struct&, const uint8_t*, size_t, int)> dumpStruct =
        [&](const V3Struct& st, const uint8_t* body, size_t bodyLen, int indent) {
        for (const V3Field& f : st.fields) {
            if (s.size() > limit) return;
            if ((size_t)f.offset > bodyLen) break;
            std::string pad(indent, ' ');
            const char* tn = v3TypeName(f.type);
            if (f.type == 0x10 && (size_t)f.spec < structs.size() && indent < 24) {
                addf(s, "%s%s: %s {\r\n", pad.c_str(), f.name.c_str(), structs[f.spec].name.c_str());
                size_t sub = f.size > 0 ? (size_t)f.size : bodyLen - f.offset;
                dumpStruct(structs[f.spec], body + f.offset, std::min(sub, bodyLen - f.offset), indent + 4);
                addf(s, "%s}\r\n", pad.c_str());
                continue;
            }
            addf(s, "%s%s = %s", pad.c_str(), f.name.c_str(),
                 fieldValue(f, body + f.offset, bodyLen - f.offset).c_str());
            if (!tn) addf(s, "   (type 0x%X)", f.type);
            s += "\r\n";
        }
    };

    addf(s, "\r\nobjects (%u), as Most Wanted's own structures describe them\r\n",
         (unsigned)spans.size());
    for (size_t i = 0; i < spans.size() && D; ++i) {
        if (s.size() > limit) { s += "... (the rest is left out - the file is large)\r\n"; break; }
        const uint8_t* e = D + spans[i].first;
        size_t n = spans[i].second - spans[i].first;
        if (n < 2) { addf(s, "  #%u  (empty)\r\n", (unsigned)i); continue; }
        uint16_t id; memcpy(&id, e, 2);
        uint16_t second = n >= 4 ? (uint16_t)(e[2] | (e[3] << 8)) : 0xFFFF;
        uint32_t count = 0;
        if (n >= 8) memcpy(&count, e + 4, 4);
        // the three map kinds; a structure can share their numbers, which
        // is why the header has to agree as well
        if (id == 0xD && second == 0 && n >= 8 && count <= 4096 && 8 + count * 2 <= n && nEnum) {
            addf(s, "  #%u  string list (%u):", (unsigned)i, count);
            for (uint32_t k = 0; k < count; ++k) {
                uint16_t v; memcpy(&v, e + 8 + k * 2, 2);
                addf(s, "%s %s", k ? "," : "", S.name(v).c_str());
            }
            s += "\r\n";
            continue;
        }
        if (id == 0xF && second == 0 && n >= 8 && count <= 0x1000 && 8 + count * 4 <= n) {
            addf(s, "  #%u  references (%u):", (unsigned)i, count);
            for (uint32_t k = 0; k < count && k < 256; ++k) {
                uint16_t v; memcpy(&v, e + 8 + k * 4, 2);
                addf(s, " #%u", v);
            }
            s += "\r\n";
            continue;
        }
        if (id == 0x10 && n >= 8 && second < structs.size() && count && count <= 0x1000 &&
            (n - 8) % count == 0) {
            size_t each = (n - 8) / count;
            addf(s, "  #%u  array of %u %s\r\n", (unsigned)i, count, structs[second].name.c_str());
            for (uint32_t k = 0; k < count && s.size() <= limit; ++k) {
                addf(s, "      [%u]\r\n", k);
                dumpStruct(structs[second], e + 8 + k * each, each, 10);
            }
            continue;
        }
        if (id < structs.size()) {
            addf(s, "  #%u  %s\r\n", (unsigned)i, structs[id].name.c_str());
            dumpStruct(structs[id], e + 2, n - 2, 6);
            continue;
        }
        addf(s, "  #%u  %u bytes, no structure\r\n", (unsigned)i, (unsigned)n);
    }
}

// ---- version 4 (No Limits, Real Racing 3) ----

int v4Size(int t) {
    switch (t) {
        case 0x01: case 0x02: case 0x09: return 1;
        case 0x03: case 0x04: case 0x0d: case 0x15: case 0x17: return 2;
        case 0x05: case 0x06: case 0x0a: case 0x0f: case 0x12: case 0x16: return 4;
        case 0x07: case 0x08: case 0x0b: return 8;
        case 0x19: return 12;
        case 0x1a: return 16;
    }
    return 0;
}

// shared objects written out again wherever they are referenced (for the
// readers that parse this text), rather than "shown above"
static thread_local bool g_expandShared = false;

void v4Objects(const Sbin& S, std::string& s, size_t limit) {
    if (!S.ohdr || !S.data) return;
    const uint8_t* D = S.data->data;
    size_t dl = S.data->size;
    size_t nO = S.ohdr->size / 4;
    auto in = [&](size_t o, size_t n) { return o + n <= dl; };
    auto u16 = [&](size_t o) -> uint32_t { if (!in(o, 2)) return 0; uint16_t v; memcpy(&v, D + o, 2); return v; };
    auto u32 = [&](size_t o) -> uint32_t { if (!in(o, 4)) return 0xFFFFFFFFu; uint32_t v; memcpy(&v, D + o, 4); return v; };

    auto value = [&](int t, size_t o) -> std::string {
        char b[96];
        if (!in(o, (size_t)std::max(1, v4Size(t)))) return "?";
        switch (t) {
            case 0x01: snprintf(b, sizeof(b), "%d", (int)(int8_t)D[o]); return b;
            case 0x02: snprintf(b, sizeof(b), "%u", D[o]); return b;
            case 0x03: snprintf(b, sizeof(b), "%d", (int)(int16_t)u16(o)); return b;
            case 0x04: case 0x17: snprintf(b, sizeof(b), "%u", u16(o)); return b;
            case 0x05: snprintf(b, sizeof(b), "%d", (int32_t)u32(o)); return b;
            case 0x06: case 0x12: case 0x16: snprintf(b, sizeof(b), "%u", u32(o)); return b;
            case 0x07: { int64_t v; memcpy(&v, D + o, 8); snprintf(b, sizeof(b), "%lld", (long long)v); return b; }
            case 0x08: { uint64_t v; memcpy(&v, D + o, 8); snprintf(b, sizeof(b), "%llu", (unsigned long long)v); return b; }
            case 0x09: return D[o] ? "true" : "false";
            case 0x0a: { float v; memcpy(&v, D + o, 4); snprintf(b, sizeof(b), "%g", v); return b; }
            case 0x0b: { double v; memcpy(&v, D + o, 8); snprintf(b, sizeof(b), "%g", v); return b; }
            // 0x0D is an enum's value spelled as its member's name
            case 0x0d: return S.name(u16(o));
            case 0x15: return quoted(S.name(u16(o)));
            case 0x19: case 0x1a: {
                float v[4] = {0, 0, 0, 0};
                memcpy(v, D + o, t == 0x19 ? 12 : 16);
                if (t == 0x19) snprintf(b, sizeof(b), "(%g, %g, %g)", v[0], v[1], v[2]);
                else snprintf(b, sizeof(b), "(%g, %g, %g, %g)", v[0], v[1], v[2], v[3]);
                return b;
            }
        }
        snprintf(b, sizeof(b), "(type 0x%X)", t);
        return b;
    };

    std::set<uint32_t> seen;
    std::function<void(uint32_t, int)> obj;
    // a structure's fields at `base` (a typed object's data, or a value
    // field held inline in another structure)
    std::function<void(size_t, size_t, int)> fields = [&](size_t sid, size_t base, int indent) {
        std::string pad(indent, ' ');
        size_t nStru = S.stru->size / 6;
        if (sid >= nStru) { s += "?\r\n"; return; }
        size_t first = S.u16(S.stru, sid * 6 + 2), count = S.u16(S.stru, sid * 6 + 4);
        addf(s, "%s {\r\n", S.name(S.u16(S.stru, sid * 6)).c_str());
        for (size_t f = first; f < first + count && f < S.fiel->size / 8; ++f) {
            std::string fn = S.name(S.u16(S.fiel, f * 8));
            int ft = S.u16(S.fiel, f * 8 + 2);
            size_t fo = S.u16(S.fiel, f * 8 + 4);
            size_t va = base + fo;
            addf(s, "%s  %s = ", pad.c_str(), fn.c_str());
            // the structure's own numbering: 5 int, 9 bool, 10 float,
            // 13 name (32-bit), 15 reference, 16 value (a structure held
            // inline), 17 array, 18 enum, 20 name, 23 string
            if (ft == 15 || ft == 17) obj(u32(va), indent + 2);
            else if (ft == 16 && indent < 60) fields(S.u16(S.fiel, f * 8 + 6), va, indent + 2);
            else if (ft == 13) { s += quoted(S.name(u32(va))); s += "\r\n"; }
            else if (ft == 20 || ft == 23) { s += quoted(S.name(u16(va))); s += "\r\n"; }
            else if (ft == 9) { s += value(0x09, va); s += "\r\n"; }
            else if (ft == 10) { s += value(0x0a, va); s += "\r\n"; }
            else { s += value(0x05, va); s += "\r\n"; }
        }
        addf(s, "%s}\r\n", pad.c_str());
    };
    obj = [&](uint32_t idx, int indent) {
        std::string pad(indent, ' ');
        if (s.size() > limit) return;
        if (idx >= nO) { s += "(no object)\r\n"; return; }
        if (seen.count(idx) && (!g_expandShared || indent > 160)) { addf(s, "(object #%u, shown above)\r\n", idx); return; }
        seen.insert(idx);
        uint32_t e = S.u32(S.ohdr, idx * 4);
        uint32_t kind = e & 7;
        size_t off = e >> 3;
        if (kind == 2) {
            uint32_t et = u32(off), n = u32(off + 4);
            int sz = v4Size((int)et);
            if (!sz || n > 100000 || !in(off + 8, (size_t)sz * n)) { s += "[ ? ]\r\n"; return; }
            if (et != 0x0f) {
                s += "[";
                for (uint32_t k = 0; k < n && k < 64; ++k)
                    addf(s, "%s%s", k ? ", " : " ", value((int)et, off + 8 + (size_t)k * sz).c_str());
                if (n > 64) addf(s, ", ... %u more", n - 64);
                s += " ]\r\n";
                return;
            }
            addf(s, "[%u]\r\n", n);
            for (uint32_t k = 0; k < n && s.size() <= limit; ++k) {
                addf(s, "%s  [%u] ", pad.c_str(), k);
                obj(u32(off + 8 + (size_t)k * 4), indent + 4);
            }
            return;
        }
        if (kind == 1) {
            uint32_t cnt = u16(off);
            s += "{\r\n";
            size_t q = off + 4;
            for (uint32_t i = 0; i < cnt && s.size() <= limit; ++i) {
                if (!in(q, 8)) break;
                uint32_t k = u16(q), t = u16(q + 2), vo = u16(q + 4), vs = u16(q + 6);
                size_t va = off + vo;
                addf(s, "%s  %s = ", pad.c_str(), S.name(k).c_str());
                if (t == 0x0f) obj(u32(va), indent + 2);
                else { s += value((int)t, va); s += "\r\n"; }
                int sz = vs ? (int)vs : v4Size((int)t);
                if (!sz) { addf(s, "%s  (unknown type 0x%X - stopped here)\r\n", pad.c_str(), t); break; }
                q = (va + sz + 1) & ~(size_t)1;
                while (in(q, 2) && D[q] == 0xcd && D[q + 1] == 0xcd) q += 2;
            }
            addf(s, "%s}\r\n", pad.c_str());
            return;
        }
        if (kind == 0 && S.stru && S.fiel) {
            uint32_t sid = u16(off);
            size_t nStru = S.stru->size / 6;
            if (sid >= nStru) { s += "(typed object, unknown structure)\r\n"; return; }
            fields(sid, off + 2, indent);
            return;
        }
        addf(s, "(object kind %u)\r\n", kind);
    };
    s += "\r\nobjects, from the root\r\n";
    obj(0, 0);
    if (s.size() > limit) s += "\r\n... (the rest is left out - the file is large)\r\n";
}

} // namespace

const char* sbinVersion3TypeName(int t) { return v3TypeName(t); }

// Most Wanted's textures: every mip level is an "Image" object - width,
// height, format (an enum whose member names are the codec names) and data
// (the BULK entry holding its pixels). Read by structure, not by pattern.
std::vector<SbinImageRecord> sbinVersion3Images(const uint8_t* d, size_t len) {
    std::vector<SbinImageRecord> out;
    Sbin S;
    if (!S.load(d, len) || S.version != 3 || !S.stru || !S.fiel || !S.ohdr || !S.data) return out;
    size_t nStru = S.stru->size / 6, nFiel = S.fiel->size / 8;
    int imageId = -1;
    int offW = -1, offH = -1, offF = -1, offD = -1, fmtEnum = -1;
    for (size_t i = 0; i < nStru && imageId < 0; ++i) {
        if (S.name(S.u16(S.stru, i * 6)) != "Image") continue;
        imageId = (int)i;
        size_t first = S.u16(S.stru, i * 6 + 2), count = S.u16(S.stru, i * 6 + 4);
        for (size_t f = first; f < first + count && f < nFiel; ++f) {
            std::string fn = S.name(S.u16(S.fiel, f * 8));
            int off = S.u16(S.fiel, f * 8 + 4);
            if (fn == "width") offW = off;
            else if (fn == "height") offH = off;
            else if (fn == "format") { offF = off; fmtEnum = S.u16(S.fiel, f * 8 + 6); }
            else if (fn == "data") offD = off;
        }
    }
    if (imageId < 0 || offW < 0 || offH < 0 || offD < 0) return out;

    std::vector<std::pair<size_t, size_t>> spans;
    size_t nO = S.ohdr->size / 4, prev = 0;
    for (size_t i = 1; i <= nO; ++i) {
        size_t end = i < nO ? S.u32(S.ohdr, i * 4) / 8 : S.data->size;
        if (end < prev || end > S.data->size) break;
        spans.push_back({ prev, end });
        prev = end;
    }
    const uint8_t* D = S.data->data;
    std::vector<std::string> members;
    if (fmtEnum >= 0 && S.enm && (size_t)fmtEnum < S.enm->size / 8) {
        size_t ref = S.u16(S.enm, (size_t)fmtEnum * 8 + 4);
        if (ref < spans.size() && spans[ref].second - spans[ref].first >= 8) {
            size_t a = spans[ref].first;
            uint32_t cnt; memcpy(&cnt, D + a + 4, 4);
            for (uint32_t k = 0; k < cnt && k < 256 && a + 8 + k * 2 + 2 <= spans[ref].second; ++k) {
                uint16_t id; memcpy(&id, D + a + 8 + k * 2, 2);
                members.push_back(S.name(id));
            }
        }
    }
    auto rd = [&](size_t a, size_t b, int off) -> int32_t {
        size_t at = a + 2 + (size_t)off;
        if (off < 0 || at + 4 > b) return -1;
        int32_t v; memcpy(&v, D + at, 4);
        return v;
    };
    for (const auto& sp : spans) {
        if (sp.second - sp.first < 2) continue;
        uint16_t id; memcpy(&id, D + sp.first, 2);
        if (id != imageId) continue;
        SbinImageRecord r;
        r.width = rd(sp.first, sp.second, offW);
        r.height = rd(sp.first, sp.second, offH);
        r.blob = rd(sp.first, sp.second, offD);
        int32_t f = rd(sp.first, sp.second, offF);
        if (f >= 0 && (size_t)f < members.size()) r.format = members[f];
        if (r.width > 0 && r.height > 0 && r.width <= 16384 && r.height <= 16384 && r.blob >= 0)
            out.push_back(r);
    }
    return out;
}

std::string sbinObjectsText(const uint8_t* d, size_t len, size_t limit) {
    Sbin S;
    std::string s;
    if (!S.load(d, len)) return s;
    // every chunk carries the FNV-1 hash of its data: a damaged or hand-edited
    // file shows up here before the game refuses it
    s += "\r\nchunk hashes (FNV-1 of each chunk's data)\r\n";
    size_t pos = 8;
    while (pos + 12 <= len) {
        uint32_t cl, h;
        memcpy(&cl, d + pos + 4, 4);
        memcpy(&h, d + pos + 8, 4);
        if (pos + 12 + cl > len) break;
        uint32_t want = sbinFnv1(d + pos + 12, cl);
        addf(s, "  %.4s  %08X  %s\r\n", (const char*)d + pos, h,
             h == want ? "ok" : "WRONG - the game would reject this chunk");
        pos = (pos + 12 + cl + 3) & ~(size_t)3;
    }
    if (S.version == 3) v3Objects(S, s, limit);
    else v4Objects(S, s, limit);
    return s;
}

std::string sbinObjectsTextExpanded(const uint8_t* d, size_t len, size_t limit) {
    g_expandShared = true;
    std::string s = sbinObjectsText(d, len, limit);
    g_expandShared = false;
    return s;
}

} // namespace nfsnl
