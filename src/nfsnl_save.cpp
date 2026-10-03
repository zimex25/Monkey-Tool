// nfsnl_save.cpp - the Save Editor's core (see nfsnl_save.h)
//
// Ported line for line from the Firemonkeys Save Editor's Go source (nfs.go,
// rr3.go). Every read is bounds-checked: the Go version relied on the runtime
// catching a slice out of range, which C++ does not do.
#include "nfsnl_save.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <set>

namespace fs = std::filesystem;

namespace nfsnl {
namespace saves {

// ============================================================== primitives

namespace {

const uint8_t kSbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

uint8_t kInv[256];
bool invReady = false;
void makeInv() {
    if (invReady) return;
    for (int i = 0; i < 256; ++i) kInv[kSbox[i]] = (uint8_t)i;
    invReady = true;
}

inline uint8_t xt(uint8_t a) { return (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1b : 0)); }
inline uint8_t mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; }
    return r;
}

// 15 round keys of 16 bytes
void expandKey(const uint8_t key[32], uint8_t rk[240]) {
    memcpy(rk, key, 32);
    uint8_t rcon = 1;
    for (int i = 8; i < 60; ++i) {
        uint8_t t[4];
        memcpy(t, rk + (i - 1) * 4, 4);
        if (i % 8 == 0) {
            uint8_t a = t[0];
            t[0] = (uint8_t)(kSbox[t[1]] ^ rcon); t[1] = kSbox[t[2]];
            t[2] = kSbox[t[3]]; t[3] = kSbox[a];
            rcon = xt(rcon);
        } else if (i % 8 == 4) {
            for (int k = 0; k < 4; ++k) t[k] = kSbox[t[k]];
        }
        for (int k = 0; k < 4; ++k) rk[i * 4 + k] = rk[(i - 8) * 4 + k] ^ t[k];
    }
}

void encBlock(const uint8_t rk[240], uint8_t s[16]) {
    for (int i = 0; i < 16; ++i) s[i] ^= rk[i];
    for (int r = 1; r <= 14; ++r) {
        uint8_t t[16];
        for (int i = 0; i < 16; ++i) t[i] = kSbox[s[i]];
        // shift rows (column-major state)
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row) s[c * 4 + row] = t[((c + row) % 4) * 4 + row];
        if (r != 14)
            for (int c = 0; c < 4; ++c) {
                uint8_t* p = s + c * 4;
                uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
                p[0] = (uint8_t)(xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3);
                p[1] = (uint8_t)(a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3);
                p[2] = (uint8_t)(a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3);
                p[3] = (uint8_t)(xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3));
            }
        for (int i = 0; i < 16; ++i) s[i] ^= rk[r * 16 + i];
    }
}

void decBlock(const uint8_t rk[240], uint8_t s[16]) {
    makeInv();
    for (int i = 0; i < 16; ++i) s[i] ^= rk[14 * 16 + i];
    for (int r = 13; r >= 0; --r) {
        uint8_t t[16];
        // inverse shift rows, inverse sub bytes
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row) t[((c + row) % 4) * 4 + row] = s[c * 4 + row];
        for (int i = 0; i < 16; ++i) s[i] = kInv[t[i]];
        for (int i = 0; i < 16; ++i) s[i] ^= rk[r * 16 + i];
        if (r != 0)
            for (int c = 0; c < 4; ++c) {
                uint8_t* p = s + c * 4;
                uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
                p[0] = (uint8_t)(mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9));
                p[1] = (uint8_t)(mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13));
                p[2] = (uint8_t)(mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11));
                p[3] = (uint8_t)(mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14));
            }
    }
}

uint16_t rd16le(const Bytes& b, size_t o) { return o + 2 <= b.size() ? (uint16_t)(b[o] | b[o + 1] << 8) : 0; }
uint32_t rd32le(const Bytes& b, size_t o) {
    return o + 4 <= b.size() ? (uint32_t)(b[o] | b[o + 1] << 8 | b[o + 2] << 16 | (uint32_t)b[o + 3] << 24) : 0;
}
uint32_t rd32be(const Bytes& b, size_t o) {
    return o + 4 <= b.size() ? ((uint32_t)b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]) : 0;
}
void wr32le(Bytes& b, size_t o, uint32_t v) { for (int k = 0; k < 4; ++k) b[o + k] = (uint8_t)(v >> (8 * k)); }
void wr32be(Bytes& b, size_t o, uint32_t v) { for (int k = 0; k < 4; ++k) b[o + k] = (uint8_t)(v >> (24 - 8 * k)); }

std::string lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) ++a;
    while (b > a && isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}
// drop the characters people type between digits: space, comma, underscore,
// no-break space (U+00A0, UTF-8 C2 A0), and optionally dots
std::string cleanNumber(const std::string& s, bool dropDots) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xC2 && i + 1 < s.size() && (unsigned char)s[i + 1] == 0xA0) { ++i; continue; }
        if (c == 0xA0 || c == ' ' || c == ',' || c == '_' || (dropDots && c == '.')) continue;
        o += (char)c;
    }
    return o;
}
bool parseI64(const std::string& s, int64_t lo, int64_t hi, int64_t& v) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    long long x = strtoll(s.c_str(), &end, 10);
    if (errno || *end || x < lo || x > hi) return false;
    v = x;
    return true;
}
bool parseU64(const std::string& s, uint64_t hi, uint64_t& v) {
    if (s.empty() || s[0] == '-' || s[0] == '+') return false;
    char* end = nullptr;
    errno = 0;
    unsigned long long x = strtoull(s.c_str(), &end, 10);
    if (errno || *end || x > hi) return false;
    v = x;
    return true;
}
bool parseF32(std::string s, float& f) {
    size_t comma = s.find(',');
    if (comma != std::string::npos) s[comma] = '.';
    s = trim(s);
    if (s.empty()) return false;
    char* end = nullptr;
    double d = strtod(s.c_str(), &end);
    if (*end || !std::isfinite(d)) return false;
    f = (float)d;
    return true;
}

} // namespace

void aes256CbcDecrypt(const uint8_t key[32], const uint8_t iv[16], uint8_t* buf, size_t len) {
    uint8_t rk[240], prev[16], cur[16];
    expandKey(key, rk);
    memcpy(prev, iv, 16);
    for (size_t o = 0; o + 16 <= len; o += 16) {
        memcpy(cur, buf + o, 16);
        decBlock(rk, buf + o);
        for (int i = 0; i < 16; ++i) buf[o + i] ^= prev[i];
        memcpy(prev, cur, 16);
    }
}

void aes256CbcEncrypt(const uint8_t key[32], const uint8_t iv[16], uint8_t* buf, size_t len) {
    uint8_t rk[240], prev[16];
    expandKey(key, rk);
    memcpy(prev, iv, 16);
    for (size_t o = 0; o + 16 <= len; o += 16) {
        for (int i = 0; i < 16; ++i) buf[o + i] ^= prev[i];
        encBlock(rk, buf + o);
        memcpy(prev, buf + o, 16);
    }
}

void md5(const uint8_t* data, size_t len, uint8_t out[16]) {
    static const uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
    static const int R[64] = { 7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
                               5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
                               4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
                               6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };
    uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    Bytes m(data, data + len);
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; ++i) m.push_back((uint8_t)(bits >> (8 * i)));
    for (size_t o = 0; o < m.size(); o += 64) {
        uint32_t w[16];
        for (int i = 0; i < 16; ++i) w[i] = rd32le(m, o + i * 4);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; ++i) {
            uint32_t f; int g;
            if (i < 16)      { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) % 16; }
            else             { f = c ^ (b | ~d);       g = (7 * i) % 16; }
            uint32_t t = d; d = c; c = b;
            uint32_t x = a + f + K[i] + w[g];
            b = b + ((x << R[i]) | (x >> (32 - R[i])));
            a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k) out[i * 4 + k] = (uint8_t)(h[i] >> (8 * k));
}

std::string base64(const uint8_t* d, size_t n) {
    static const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        o += A[v >> 18 & 63];
        o += A[v >> 12 & 63];
        o += i + 1 < n ? A[v >> 6 & 63] : '=';
        o += i + 2 < n ? A[v & 63] : '=';
    }
    return o;
}

uint32_t fnv1(const uint8_t* b, size_t n) {
    uint32_t h = 0x811c9dc5;
    for (size_t i = 0; i < n; ++i) h = (h * 0x01000193) ^ b[i];
    return h;
}

std::string shortestFloat(float f) {
    if (std::isnan(f)) return "NaN";
    if (std::isinf(f)) return f > 0 ? "+Inf" : "-Inf";
    char buf[64];
    for (int p = 1; p <= 9; ++p) {
        snprintf(buf, sizeof(buf), "%.*g", p, (double)f);
        if ((float)strtod(buf, nullptr) == f) break;
    }
    // Go writes the exponent as e+06, C as e+06 too, but Go drops nothing -
    // close enough for display and for parsing back
    return buf;
}

// ============================================================== No Limits

namespace {

const uint8_t kNfsKey[32] = {
    0xca, 0x3f, 0x71, 0x6e, 0x03, 0x58, 0x7a, 0x78, 0x63, 0x0e, 0x4b, 0xed, 0xdd, 0xf4, 0xee, 0xdc,
    0x92, 0xb8, 0x9d, 0x9c, 0xa6, 0x76, 0x44, 0x71, 0x4f, 0x7e, 0x32, 0x2b, 0x25, 0x55, 0x40, 0xb9,
};
const uint8_t kNfsIvMask[16] = { 0x03, 0x14, 0x25, 0x36, 0x47, 0x58, 0x69, 0x7a,
                                 0x8b, 0x9c, 0xad, 0xbe, 0xcf, 0xe0, 0xf1, 0x02 };

void nfsIv(const std::string& name, uint8_t iv[16]) {
    for (int i = 0; i < 16; ++i)
        iv[i] = (uint8_t)(name.empty() ? 0 : name[i % name.size()]) ^ kNfsIvMask[i];
}

bool nfsDecrypt(const std::string& name, const Bytes& c, Bytes& p, std::string& err) {
    if (c.empty() || c.size() % 16) { err = "size is not a multiple of 16"; return false; }
    uint8_t iv[16];
    nfsIv(name, iv);
    p = c;
    aes256CbcDecrypt(kNfsKey, iv, p.data(), p.size());
    size_t n = p.back();
    bool ok = n >= 1 && n <= 16;
    for (size_t k = 0; ok && k < n; ++k) if (p[p.size() - 1 - k] != n) ok = false;
    if (!ok) { err = "wrong key or damaged file (bad padding)"; return false; }
    p.resize(p.size() - n);
    if (p.size() < 4 || memcmp(p.data(), "SBIN", 4)) { err = "decrypted data is not SBIN"; return false; }
    return true;
}

Bytes nfsEncrypt(const std::string& name, const Bytes& p) {
    size_t n = 16 - p.size() % 16;
    Bytes b = p;
    b.insert(b.end(), n, (uint8_t)n);
    uint8_t iv[16];
    nfsIv(name, iv);
    aes256CbcEncrypt(kNfsKey, iv, b.data(), b.size());
    return b;
}

bool parseSbin(NfsFile& f, std::string& err) {
    const Bytes& p = f.P;
    f.chunks.clear(); f.strs.clear(); f.ohdr.clear();
    size_t pos = 8;
    while (pos + 12 <= p.size()) {
        std::string tag((const char*)p.data() + pos, 4);
        size_t ln = rd32le(p, pos + 4);
        if (pos + 12 + ln > p.size()) { err = "bad SBIN (chunk " + tag + " runs past the end)"; return false; }
        f.chunks[tag] = { pos, pos + 12, ln };
        pos = (pos + 12 + ln + 3) & ~(size_t)3;
    }
    for (const char* t : { "OHDR", "DATA", "CHDR", "CDAT" })
        if (!f.chunks.count(t)) { err = std::string("missing chunk ") + t; return false; }
    const auto& ch = f.chunks["CHDR"];
    const auto& cd = f.chunks["CDAT"];
    for (size_t i = 0; i + 8 <= ch.len; i += 8) {
        size_t a = rd32le(p, ch.data + i), l = rd32le(p, ch.data + i + 4);
        if (a + l > cd.len) { err = "bad SBIN (string outside CDAT)"; return false; }
        f.strs.emplace_back((const char*)p.data() + cd.data + a, l);
    }
    const auto& oh = f.chunks["OHDR"];
    for (size_t i = 0; i + 4 <= oh.len; i += 4) f.ohdr.push_back(rd32le(p, oh.data + i));
    return true;
}

void rehash(NfsFile& f, const char* tag) {
    auto it = f.chunks.find(tag);
    if (it == f.chunks.end()) return;
    wr32le(f.P, it->second.hdr + 8, fnv1(f.P.data() + it->second.data, it->second.len));
}

enum { sbInt32 = 0x05, sbUint32 = 0x06, sbInt64 = 0x08, sbBool = 0x09, sbFloat = 0x0a,
       sbObj = 0x0f, sbStr = 0x15 };

int sbSize(int t) {
    switch (t) {
        case 0x01: case 0x02: case 0x09: return 1;
        case 0x03: case 0x04: case 0x15: return 2;
        case 0x05: case 0x06: case 0x0a: case 0x0f: return 4;
        case 0x07: case 0x08: case 0x0b: return 8;
    }
    return 0;
}

// The object tree: kind 1 is an object of (key, type, value offset) fields,
// kind 2 an array. Everything is read relative to the DATA chunk.
void walkFile(const NfsFile& f, int fileIndex, const std::function<void(NfsLeaf)>& add) {
    const Bytes& P = f.P;
    size_t d = f.chunks.at("DATA").data;
    size_t dlen = f.chunks.at("DATA").len;
    auto in = [&](size_t o, size_t n) { return o + n <= dlen; };
    auto u16 = [&](size_t o) -> int { return in(o, 2) ? rd16le(P, d + o) : 0; };
    auto u32 = [&](size_t o) -> uint32_t { return in(o, 4) ? rd32le(P, d + o) : 0xffffffffu; };
    auto str = [&](int k) -> std::string { return k >= 0 && (size_t)k < f.strs.size() ? f.strs[k] : "?"; };
    std::set<uint32_t> seen;

    // an array element's label: its _Id / key / SeriesId / Name string
    auto label = [&](uint32_t idx) -> std::string {
        if (idx == 0xffffffffu || idx >= f.ohdr.size() || (f.ohdr[idx] & 7) != 1) return "";
        size_t off = f.ohdr[idx] >> 3;
        int cnt = u16(off);
        size_t q = off + 4;
        for (int i = 0; i < cnt && i < 6; ++i) {
            if (!in(q, 8)) return "";
            int k = u16(q), t = u16(q + 2);
            size_t vo = u32(q + 4);
            if (t == sbStr) {
                std::string n = str(k);
                if (n == "_Id" || n == "key" || n == "SeriesId" || n == "Name" ||
                    n == "ChapterId" || n == "_CardDescriptionId")
                    return str(u16(off + vo));
            }
            int sz = sbSize(t);
            if (!sz) return "";
            q = (off + vo + sz + 1) & ~(size_t)1;
            while (in(q, 2) && P[d + q] == 0xcd && P[d + q + 1] == 0xcd) q += 2;
        }
        return "";
    };

    std::function<void(uint32_t, const std::string&, int)> obj =
        [&](uint32_t idx, const std::string& path, int depth) {
        if (idx == 0xffffffffu || depth > 64 || idx >= f.ohdr.size() || seen.count(idx)) return;
        seen.insert(idx);
        uint32_t e = f.ohdr[idx];
        uint32_t kind = e & 7;
        size_t off = e >> 3;
        if (kind == 2) {
            int et = (int)u32(off);
            uint32_t n = u32(off + 4);
            int sz = sbSize(et);
            if (!sz || !in(off + 8, (size_t)sz * n)) return;
            for (uint32_t i = 0; i < n; ++i) {
                size_t va = off + 8 + (size_t)sz * i;
                if (et == sbObj) {
                    uint32_t c = u32(va);
                    std::string l = label(c);
                    if (l.empty()) l = std::to_string(i);
                    obj(c, path + "[" + l + "]", depth + 1);
                } else {
                    add({ fileIndex, path + "[" + std::to_string(i) + "]", et, d + va });
                }
            }
            return;
        }
        if (kind != 1) return;
        int cnt = u16(off);
        size_t q = off + 4;
        for (int i = 0; i < cnt; ++i) {
            if (!in(q, 8)) return;
            int k = u16(q), t = u16(q + 2);
            size_t vo = u32(q + 4);
            size_t va = off + vo;
            std::string name = path + "/" + str(k);
            int sz = sbSize(t);
            if (t == sbObj) {
                obj(u32(va), name, depth + 1);
            } else if (sz && in(va, sz)) {
                add({ fileIndex, name, t, d + va });
            }
            if (!sz) return;                     // unknown type: stop this object safely
            q = (va + sz + 1) & ~(size_t)1;
            while (in(q, 2) && P[d + q] == 0xcd && P[d + q + 1] == 0xcd) q += 2;
        }
    };
    if (!f.ohdr.empty()) obj(0, "", 0);
}

std::string sectionTitle(const NfsFile& f, const std::string& fallback) {
    for (size_t i = 0; i + 1 < f.strs.size(); ++i)
        if (f.strs[i] == "SectionId") return f.strs[i + 1];
    return fallback;
}

// "<uid>_<n>.sb"
bool sectionName(const std::string& n, std::string& uid, long& num) {
    if (n.size() < 6 || n.compare(n.size() - 3, 3, ".sb")) return false;
    std::string stem = n.substr(0, n.size() - 3);
    size_t u = stem.find('_');
    if (u == std::string::npos || u == 0 || u + 1 >= stem.size()) return false;
    for (size_t i = 0; i < stem.size(); ++i)
        if (i != u && !isdigit((unsigned char)stem[i])) return false;
    uid = stem.substr(0, u);
    num = atol(stem.c_str() + u + 1);
    return true;
}

bool loadNfsFile(const std::string& dir, const std::string& name, NfsFile& f, std::string& err) {
    f = NfsFile();
    f.name = name;
    f.path = (fs::u8path(dir) / fs::u8path(name)).u8string();
    Bytes c;
    if (!readFile(f.path, c)) { err = name + ": cannot read"; return false; }
    std::string e;
    if (!nfsDecrypt(name, c, f.P, e)) { err = name + ": " + e; return false; }
    if (!parseSbin(f, e)) { err = name + ": " + e; return false; }
    uint8_t sum[16];
    md5(f.P.data(), f.P.size(), sum);
    f.oldMd5 = base64(sum, 16);
    return true;
}

bool replaceString(NfsFile& f, const std::string& old, const std::string& nw, std::string& err) {
    if (old.size() != nw.size()) { err = "hash length mismatch"; return false; }
    const auto& ch = f.chunks["CHDR"];
    const auto& cd = f.chunks["CDAT"];
    bool found = false;
    for (size_t i = 0; i + 8 <= ch.len; i += 8) {
        size_t a = rd32le(f.P, ch.data + i), l = rd32le(f.P, ch.data + i + 4);
        if (l == old.size() && a + l <= cd.len &&
            !memcmp(f.P.data() + cd.data + a, old.data(), l)) {
            memcpy(f.P.data() + cd.data + a, nw.data(), l);
            f.strs[i / 8] = nw;
            found = true;
        }
    }
    if (!found) { err = "section hash not found (manifest out of date?)"; return false; }
    return true;
}

NfsMain currency(const char* id, const char* label) {
    std::string base = std::string("Currencies/ProgressionModel/_CurrencyInventory/_Currencies[") + id + "]";
    return { label, base + "/_BalanceEncrypted/Value", base + "/_BalanceEarned/Value" };
}

} // namespace

// No Limits' data files (.sb under data/) use the save files' cipher: the
// same key, the IV from the file's own name. What is inside is not always
// SBIN directly - most are a "ZBDS" wrapper around gzip - so these two do the
// cipher alone and leave the contents to the caller.
bool nfsDecryptAny(const std::string& name, const Bytes& c, Bytes& p, std::string& err) {
    if (c.empty() || c.size() % 16) { err = "size is not a multiple of 16"; return false; }
    uint8_t iv[16];
    nfsIv(name, iv);
    p = c;
    aes256CbcDecrypt(kNfsKey, iv, p.data(), p.size());
    size_t n = p.back();
    bool ok = n >= 1 && n <= 16 && n <= p.size();
    for (size_t k = 0; ok && k < n; ++k) if (p[p.size() - 1 - k] != n) ok = false;
    if (!ok) { err = "not encrypted with the game's key (bad padding)"; return false; }
    p.resize(p.size() - n);
    return true;
}

Bytes nfsEncryptAny(const std::string& name, const Bytes& p) {
    return nfsEncrypt(name, p);
}

const std::vector<NfsMain>& nfsMainFields() {
    static const std::vector<NfsMain> v = {
        currency("Cash", "Cash"),
        currency("PC", "Gold"),
        { "Driver level", "Unspecified/ProgressionModel/_SPLevelEncrypted/Value", "" },
        { "Rep (XP)", "Unspecified/ProgressionModel/_RepEncrypted/Value", "" },
        { "Mechanic points", "Unspecified/ProgressionModel/_MechanicPoints/Value", "" },
        currency("Fuel", "Fuel"),
        currency("Scrap", "Scrap"),
        currency("RaceSkips", "Race skips"),
        currency("TuningTools", "Tuning tools"),
        currency("TunerTrialKeys", "Tuner trial keys"),
        currency("VP", "VP"),
        currency("TournamentCurrency", "Tournament currency"),
        currency("VIPP", "VIP points"),
        currency("LTSGrind", "LTS currency"),
    };
    return v;
}

bool NfsSave::load(const std::string& folder, std::string& err) {
    *this = NfsSave();
    dir = folder;
    std::vector<std::string> entries;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(fs::u8path(folder), ec))
        if (e.is_regular_file(ec)) entries.push_back(e.path().filename().u8string());
    if (ec && entries.empty()) { err = "cannot read the folder"; return false; }
    std::sort(entries.begin(), entries.end());
    for (const auto& n : entries)
        if (n.size() > 5 && n.compare(n.size() - 5, 5, "_m.sb") == 0) { uid = n.substr(0, n.size() - 5); break; }
    if (uid.empty())
        for (const auto& n : entries) {
            std::string u; long k;
            if (sectionName(n, u, k)) { uid = u; break; }
        }
    if (uid.empty()) { err = "no NFS No Limits save (*.sb) found in this folder"; return false; }

    std::vector<std::pair<long, std::string>> names;
    for (const auto& n : entries) {
        std::string u; long k;
        if (sectionName(n, u, k) && u == uid) names.push_back({ k, n });
    }
    std::sort(names.begin(), names.end());
    for (const auto& kn : names) {
        NfsFile f;
        if (!loadNfsFile(dir, kn.second, f, err)) return false;
        files.push_back(std::move(f));
        int fi = (int)files.size() - 1;
        std::string title = sectionTitle(files[fi], kn.second.substr(0, kn.second.size() - 3));
        walkFile(files[fi], fi, [&](NfsLeaf l) {
            l.path = title + l.path;
            leaves.push_back(l);
            if (!byPath.count(l.path)) byPath[l.path] = leaves.size() - 1;
        });
    }
    if (files.empty()) { err = "no section files found"; return false; }
    std::string man = uid + "_m.sb";
    if (fs::exists(fs::u8path(dir) / fs::u8path(man), ec)) {
        if (!loadNfsFile(dir, man, manifest, err)) return false;
        hasManifest = true;
    }
    return true;
}

const NfsLeaf* NfsSave::find(const std::string& path) const {
    auto it = byPath.find(path);
    return it == byPath.end() ? nullptr : &leaves[it->second];
}

std::string NfsSave::get(const NfsLeaf& l) const {
    const Bytes& P = files[l.file].P;
    if (l.off + std::max(1, sbSize(l.type)) > P.size()) return "?";
    switch (l.type) {
        case sbInt32:  return std::to_string((int32_t)rd32le(P, l.off));
        case sbUint32: return std::to_string(rd32le(P, l.off));
        case sbInt64: {
            uint64_t v = (uint64_t)rd32le(P, l.off) | (uint64_t)rd32le(P, l.off + 4) << 32;
            return std::to_string((int64_t)v);
        }
        case sbBool: return P[l.off] ? "true" : "false";
        case sbFloat: {
            uint32_t b = rd32le(P, l.off); float f;
            memcpy(&f, &b, 4);
            return shortestFloat(f);
        }
        case sbStr: {
            size_t i = rd16le(P, l.off);
            const auto& s = files[l.file].strs;
            return i < s.size() ? s[i] : "?";
        }
        case 0x01: case 0x02: return std::to_string(P[l.off]);
        case 0x03: case 0x04: return std::to_string(rd16le(P, l.off));
    }
    return "?";
}

bool NfsSave::editable(const NfsLeaf& l) const {
    switch (l.type) { case sbInt32: case sbUint32: case sbInt64: case sbBool: case sbFloat: return true; }
    return false;
}

bool NfsSave::set(const NfsLeaf& l, const std::string& text, std::string& err) {
    std::string s = trim(text);
    Bytes& P = files[l.file].P;
    std::string clean = cleanNumber(s, false);
    switch (l.type) {
        case sbInt32: {
            int64_t v;
            if (!parseI64(clean, INT32_MIN, INT32_MAX, v)) { err = "enter a whole number (max 2147483647)"; return false; }
            wr32le(P, l.off, (uint32_t)(int32_t)v);
            break;
        }
        case sbUint32: {
            uint64_t v;
            if (!parseU64(clean, UINT32_MAX, v)) { err = "enter a whole number"; return false; }
            wr32le(P, l.off, (uint32_t)v);
            break;
        }
        case sbInt64: {
            int64_t v;
            if (!parseI64(clean, INT64_MIN, INT64_MAX, v)) { err = "enter a whole number"; return false; }
            wr32le(P, l.off, (uint32_t)(uint64_t)v);
            wr32le(P, l.off + 4, (uint32_t)((uint64_t)v >> 32));
            break;
        }
        case sbBool: {
            std::string b = lower(s);
            if (b == "1" || b == "true" || b == "yes" || b == "on") P[l.off] = 1;
            else if (b == "0" || b == "false" || b == "no" || b == "off") P[l.off] = 0;
            else { err = "enter true or false"; return false; }
            break;
        }
        case sbFloat: {
            float f;
            if (!parseF32(s, f)) { err = "enter a number"; return false; }
            uint32_t b; memcpy(&b, &f, 4);
            wr32le(P, l.off, b);
            break;
        }
        default:
            err = "this field cannot be edited";
            return false;
    }
    files[l.file].dirty = true;
    return true;
}

bool NfsSave::setMain(const NfsMain& m, const std::string& text, std::string& err) {
    const NfsLeaf* l = find(m.path);
    if (!l) { err = std::string(m.label) + " not in this save"; return false; }
    int64_t old = atoll(get(*l).c_str());
    std::string e;
    if (!set(*l, text, e)) { err = std::string(m.label) + ": " + e; return false; }
    int64_t nv = atoll(get(*l).c_str());
    // the "earned" ledger takes the same increase, so the totals still agree
    if (!m.also.empty() && nv > old)
        if (const NfsLeaf* a = find(m.also)) {
            int64_t v = atoll(get(*a).c_str()) + (nv - old);
            if (v > INT32_MAX) v = INT32_MAX;
            std::string ignore;
            set(*a, std::to_string(v), ignore);
        }
    return true;
}

bool NfsSave::anyDirty() const {
    for (const auto& f : files) if (f.dirty) return true;
    return false;
}

bool NfsSave::save(const std::string& stamp, std::string& backupDir, std::string& err) {
    std::vector<size_t> changed;
    for (size_t i = 0; i < files.size(); ++i) if (files[i].dirty) changed.push_back(i);
    if (changed.empty()) { err = "nothing changed"; return false; }
    fs::path bdir = fs::u8path(dir) / fs::u8path("save_editor_backup_" + stamp);

    struct Out { NfsFile* f; Bytes enc; };
    std::vector<Out> outs;
    bool manifestDirty = false;
    for (size_t i : changed) {
        NfsFile& f = files[i];
        rehash(f, "DATA");
        uint8_t sum[16];
        md5(f.P.data(), f.P.size(), sum);
        std::string nm = base64(sum, 16);
        if (hasManifest && nm != f.oldMd5) {
            std::string e;
            if (!replaceString(manifest, f.oldMd5, nm, e)) { err = "manifest: " + e; return false; }
            manifestDirty = true;
        }
        f.oldMd5 = nm;
        outs.push_back({ &f, nfsEncrypt(f.name, f.P) });
    }
    if (manifestDirty) {
        rehash(manifest, "CDAT");
        outs.push_back({ &manifest, nfsEncrypt(manifest.name, manifest.P) });
    }
    // verify before touching disk
    for (const Out& o : outs) {
        Bytes p; std::string e;
        if (!nfsDecrypt(o.f->name, o.enc, p, e) || p != o.f->P) {
            err = "self-check failed, nothing written";
            return false;
        }
    }
    std::error_code ec;
    fs::create_directories(bdir, ec);
    if (ec) { err = "cannot make the backup folder: " + ec.message(); return false; }
    for (const Out& o : outs) {
        Bytes orig;
        if (!readFile(o.f->path, orig) ||
            !writeFile((bdir / fs::u8path(o.f->name)).u8string(), orig)) {
            err = "cannot back up " + o.f->name;
            return false;
        }
    }
    for (const Out& o : outs) {
        if (!writeFile(o.f->path, o.enc)) { err = "cannot write " + o.f->name; return false; }
        o.f->dirty = false;
    }
    backupDir = bdir.u8string();
    return true;
}

// ============================================================== Real Racing 3

namespace {

const uint8_t kRr3Key[64] = {
    0x64, 0x95, 0xe4, 0x50, 0xcf, 0xd2, 0x0c, 0x32, 0x54, 0x82, 0xfc, 0x43, 0x80, 0x13, 0xa1, 0x5e,
    0x0b, 0x46, 0x5c, 0xd7, 0xc6, 0x87, 0xa8, 0xa9, 0x55, 0xa7, 0x94, 0x10, 0x11, 0x33, 0xe7, 0x67,
    0xb0, 0xc9, 0xef, 0xda, 0x21, 0x11, 0x3d, 0x91, 0x8d, 0x88, 0x4c, 0xc9, 0xfa, 0x81, 0x5d, 0x9f,
    0x17, 0x1c, 0xda, 0x97, 0x4b, 0x9c, 0xac, 0x92, 0x60, 0x7f, 0xff, 0x3c, 0x40, 0xc6, 0x50, 0x00,
};
const uint32_t kRr3Magic = 0xABCFFCBA;
enum { tBool = 0, tUint = 1, tFloat = 2, tString = 3, tRef = 4, tBlob = 5 };

Bytes rr3Crypt(const Bytes& b) {
    Bytes o(b.size());
    for (size_t i = 0; i < b.size(); ++i) o[i] = b[i] ^ kRr3Key[i % 64];
    return o;
}

bool uvarint(const Bytes& d, size_t& p, uint64_t& v) {
    v = 0;
    unsigned s = 0;
    for (;;) {
        if (p >= d.size()) return false;
        uint8_t b = d[p++];
        v |= (uint64_t)(b & 0x7f) << s;
        s += 7;
        if (b < 0x80) return true;
        if (s > 63) return false;
    }
}

Bytes putUvarint(uint64_t v) {
    Bytes o;
    for (;;) {
        uint8_t b = (uint8_t)(v & 0x7f);
        v >>= 7;
        if (v) o.push_back(b | 0x80);
        else { o.push_back(b); return o; }
    }
}

Bytes encUint(uint8_t origTag, uint64_t v) {
    if (v < 16 && origTag != 0x81) return Bytes{ (uint8_t)((v << 3) | tUint) };
    Bytes o{ 0x81 };
    Bytes vv = putUvarint(v & 0xffffffffu);
    o.insert(o.end(), vv.begin(), vv.end());
    return o;
}

bool replaceInTempSave(const Bytes& tb, const std::string& name, const Bytes& data, Bytes& out) {
    if (tb.size() < 8) return false;
    uint32_t cnt = rd32le(tb, 4);
    out.assign(tb.begin(), tb.begin() + 8);
    size_t p = 8;
    bool found = false;
    for (uint32_t i = 0; i < cnt; ++i) {
        if (p + 4 > tb.size()) return false;
        size_t nl = rd32le(tb, p);
        if (p + 4 + nl + 4 > tb.size()) return false;
        std::string nm((const char*)tb.data() + p + 4, nl);
        size_t sz = rd32le(tb, p + 4 + nl);
        size_t ds = p + 8 + nl;
        if (ds + sz > tb.size()) return false;
        out.insert(out.end(), tb.begin() + p, tb.begin() + p + 4 + nl);
        const uint8_t* payload = tb.data() + ds;
        size_t plen = sz;
        if (nm == name) { payload = data.data(); plen = data.size(); found = true; }
        uint8_t l[4];
        for (int k = 0; k < 4; ++k) l[k] = (uint8_t)(plen >> (8 * k));
        out.insert(out.end(), l, l + 4);
        out.insert(out.end(), payload, payload + plen);
        p = ds + sz;
    }
    return found && p == tb.size();
}

} // namespace

const std::vector<Rr3Hidden>& rr3HiddenFields() {
    static const std::vector<Rr3Hidden> v = {
        { "R$", "m_CurrencyWallet.m_RDollars.m_Balance[EHV].nValue", "rdollars", true },
        { "Gold", "m_CurrencyWallet.m_Gold.m_Balance[EHV].nValue", "gold", true },
        { "M$", "m_CurrencyWallet.m_MDollars.m_Balance[EHV].nValue", "mdollars", true },
        { "Driver level", "m_xp.m_currentDriverLevel[EHV].nValue", "level", false },
    };
    return v;
}

const std::vector<Rr3Plain>& rr3PlainFields() {
    static const std::vector<Rr3Plain> v = {
        { "XP total earned", "m_xp.m_totalEarned" },
        { "Level progress (0-1)", "m_xp.m_currentDriverLevelProgress" },
        { "Races won in a row", "m_raceStats.m_iWonRacesInARow" },
    };
    return v;
}

bool rr3ParseNum(const std::string& text, uint64_t& v, std::string& err) {
    std::string s = cleanNumber(text, true);
    if (!s.empty() && s[0] == '-') {
        int64_t x;
        if (!parseI64(s, INT32_MIN, INT32_MAX, x)) { err = "enter a whole number"; return false; }
        v = (uint32_t)(int32_t)x;
        return true;
    }
    if (!parseU64(s, UINT32_MAX, v)) { err = "enter a whole number between 0 and 4294967295"; return false; }
    return true;
}

std::string Rr3Rec::valueString() const {
    switch (type()) {
        case tBool: return u ? "true" : "false";
        case tUint: return std::to_string(u);
        case tFloat: return shortestFloat(f);
        case tString: return s;
        case tRef: return "<object #" + std::to_string(u) + ">";
        case tBlob:
            if (blob.size() == 4) return std::to_string(rd32le(blob, 0));
            if (blob.size() == 8) return "<hidden 64-bit>";
            return "<" + std::to_string(blob.size()) + " bytes>";
    }
    return "?";
}

bool Rr3Rec::editable() const {
    switch (type()) {
        case tBool: case tUint: case tFloat: case tString: return true;
        case tBlob: return blob.size() == 4;
    }
    return false;
}

void Rr3Rec::setUint(uint64_t v) {
    u = v & 0xffffffffu;
    raw = encUint(tag, u);
    tag = raw[0];
}

void Rr3Rec::setBlob(const Bytes& b) {
    blob = b;
    raw = { tBlob, 0, 0, 0, 0 };
    wr32be(raw, 1, (uint32_t)b.size());
    raw.insert(raw.end(), b.begin(), b.end());
}

bool Rr3Rec::setFromString(const std::string& text, std::string& err) {
    std::string t = trim(text);
    switch (type()) {
        case tBool: {
            std::string b = lower(t);
            bool on;
            if (b == "1" || b == "true" || b == "yes" || b == "on") on = true;
            else if (b == "0" || b == "false" || b == "no" || b == "off") on = false;
            else { err = "enter true or false"; return false; }
            u = on ? 1 : 0;
            raw = { (uint8_t)(on ? 0x08 : 0x00) };
            tag = raw[0];
            return true;
        }
        case tUint: {
            uint64_t v;
            if (!rr3ParseNum(t, v, err)) return false;
            setUint(v);
            return true;
        }
        case tFloat: {
            float x;
            if (!parseF32(t, x)) { err = "enter a number"; return false; }
            f = x;
            raw.assign(5, 0);
            raw[0] = tFloat;
            uint32_t b; memcpy(&b, &x, 4);
            wr32le(raw, 1, b);
            return true;
        }
        case tString:
            if (t.find('\0') != std::string::npos) { err = "invalid text"; return false; }
            s = t;
            raw.assign(1, tString);
            raw.insert(raw.end(), t.begin(), t.end());
            raw.push_back(0);
            return true;
        case tBlob: {
            if (blob.size() != 4) { err = "this field cannot be edited"; return false; }
            uint64_t v;
            if (!rr3ParseNum(t, v, err)) return false;
            Bytes b(4);
            wr32le(b, 0, (uint32_t)v);
            setBlob(b);
            return true;
        }
    }
    err = "this field cannot be edited";
    return false;
}

bool Rr3Save::load(const std::string& file, std::string& err) {
    Bytes raw;
    if (!readFile(file, raw)) { err = "cannot read " + file; return false; }
    return parse(file, raw, err);
}

bool Rr3Save::parse(const std::string& file, const Bytes& raw, std::string& err) {
    *this = Rr3Save();
    path = file;
    Bytes d = rr3Crypt(raw);
    if (d.size() < 25 || rd32be(d, 0) != kRr3Magic) {
        err = "not a Real Racing 3 save (bad header after decryption)";
        return false;
    }
    if (rd32be(d, 8) != d.size()) { err = "size field does not match file size"; return false; }
    uint8_t x = 0;
    for (size_t i = 0; i + 1 < d.size(); ++i) x ^= d[i];
    if (x != d.back()) { err = "checksum mismatch - file is damaged"; return false; }
    const std::string bad = "corrupt save (unexpected end of data)";

    uint32_t nNames = rd32be(d, 12);
    size_t p = 20;
    for (uint32_t i = 0; i < nNames; ++i) {
        size_t e = p;
        while (e < d.size() && d[e] >= 0x20) ++e;
        if (e >= d.size()) { err = bad; return false; }
        std::string name((const char*)d.data() + p, e - p);
        int t = d[e];
        p = e + 1;
        uint64_t v;
        if (t == 0) {
            if (!uvarint(d, p, v)) { err = bad; return false; }
            names[(uint32_t)v] = name;
            continue;
        }
        uint64_t n;
        if (!uvarint(d, p, n)) { err = bad; return false; }
        for (uint64_t j = 0; j < n; ++j) {
            std::string nm = name;
            for (int k = 0; k < t; ++k) {
                uint64_t id;
                if (!uvarint(d, p, id)) { err = bad; return false; }
                size_t at = nm.find("[id]");
                if (at != std::string::npos) nm.replace(at, 4, "[" + std::to_string(id) + "]");
            }
            if (!uvarint(d, p, v)) { err = bad; return false; }
            names[(uint32_t)v] = nm;
        }
    }
    if (p + 4 > d.size()) { err = bad; return false; }
    uint32_t cnt = rd32be(d, p);
    p += 4;
    rootFrom = p;
    for (uint32_t i = 0; i < cnt; ++i) {
        Rr3Rec r;
        size_t ks = p;
        if (p + 2 > d.size()) { err = bad; return false; }
        uint32_t k = (uint32_t)d[p] << 8 | d[p + 1];
        if (k & 0x8000) {
            if (p + 4 > d.size()) { err = bad; return false; }
            k = (k & 0x7fff) | ((uint32_t)d[p + 2] << 8 | d[p + 3]) << 15;
            p += 4;
        } else {
            p += 2;
        }
        r.key = k;
        r.keyBytes.assign(d.begin() + ks, d.begin() + p);
        auto nit = names.find(k);
        r.name = nit == names.end() ? std::string() : nit->second;
        size_t vs = p;
        if (p >= d.size()) { err = bad; return false; }
        uint8_t t = d[p++];
        r.tag = t;
        char hex[64];
        if (t & 0x80) {
            if ((t & 7) != tUint) {
                snprintf(hex, sizeof(hex), "unknown value tag %02x at %zx", t, vs);
                err = hex; return false;
            }
            if (!uvarint(d, p, r.u)) { err = bad; return false; }
        } else if ((t & 7) == tBool || (t & 7) == tUint) {
            r.u = t >> 3;
        } else if (t == tFloat) {
            if (p + 4 > d.size()) { err = bad; return false; }
            uint32_t b = rd32le(d, p);
            memcpy(&r.f, &b, 4);
            p += 4;
        } else if (t == tString) {
            size_t e = p;
            while (e < d.size() && d[e]) ++e;
            if (e >= d.size()) { err = bad; return false; }
            r.s.assign((const char*)d.data() + p, e - p);
            p = e + 1;
        } else if (t == tRef) {
            if (!uvarint(d, p, r.u)) { err = bad; return false; }
        } else if (t == tBlob) {
            if (p + 4 > d.size()) { err = bad; return false; }
            size_t l = rd32be(d, p);
            p += 4;
            if (p + l > d.size()) { err = bad; return false; }
            r.blob.assign(d.begin() + p, d.begin() + p + l);
            p += l;
        } else {
            snprintf(hex, sizeof(hex), "unknown value tag %02x at %zx", t, vs);
            err = hex; return false;
        }
        r.raw.assign(d.begin() + vs, d.begin() + p);
        root.push_back(std::move(r));
        if (!byName.count(root.back().name)) byName[root.back().name] = root.size() - 1;
    }
    rootTo = p;
    if (rootTo >= d.size()) { err = bad; return false; }
    plain = std::move(d);
    return true;
}

Bytes Rr3Save::build() const {
    Bytes out(plain.begin(), plain.begin() + rootFrom);
    for (const Rr3Rec& r : root) {
        out.insert(out.end(), r.keyBytes.begin(), r.keyBytes.end());
        out.insert(out.end(), r.raw.begin(), r.raw.end());
    }
    out.insert(out.end(), plain.begin() + rootTo, plain.end() - 1);
    out.push_back(0);
    wr32be(out, 8, (uint32_t)out.size());
    uint8_t x = 0;
    for (size_t i = 0; i + 1 < out.size(); ++i) x ^= out[i];
    out.back() = x;
    return rr3Crypt(out);
}

bool Rr3Save::save(const std::string& stamp, std::string& backupDir, std::string& err) {
    Bytes enc = build();
    Rr3Save check;
    std::string e;
    if (!check.parse(path, enc, e)) { err = "self-check failed, nothing written: " + e; return false; }
    Bytes orig;
    if (!readFile(path, orig)) { err = "cannot read " + path; return false; }
    fs::path file = fs::u8path(path);
    fs::path bdir = file.parent_path() / fs::u8path("save_editor_backup_" + stamp);
    std::error_code ec;
    fs::create_directories(bdir, ec);
    if (ec) { err = "cannot make the backup folder: " + ec.message(); return false; }
    if (!writeFile((bdir / file.filename()).u8string(), orig)) { err = "cannot back up the save"; return false; }
    // the game keeps a second copy inside TempSaveGame.dat; keep it in step
    fs::path tmp = file.parent_path() / "TempSaveGame.dat";
    Bytes tb;
    if (readFile(tmp.u8string(), tb) && !tb.empty()) {
        writeFile((bdir / "TempSaveGame.dat").u8string(), tb);
        Bytes nb;
        if (replaceInTempSave(tb, file.filename().u8string(), enc, nb))
            if (!writeFile(tmp.u8string(), nb)) { err = "cannot write TempSaveGame.dat"; return false; }
    }
    if (!writeFile(path, enc)) { err = "cannot write " + path; return false; }
    Rr3Save again;
    if (!again.load(path, e)) {
        err = "written file failed verification: " + e + " (backup in " + bdir.u8string() + ")";
        return false;
    }
    *this = std::move(again);
    backupDir = bdir.u8string();
    return true;
}

Rr3Rec* Rr3Save::find(const std::string& name) {
    auto it = byName.find(name);
    return it == byName.end() ? nullptr : &root[it->second];
}

bool Rr3Save::hiddenStored(const Rr3Hidden& h, uint64_t& v) const {
    auto it = byName.find(h.field);
    if (it == byName.end()) return false;
    const Rr3Rec& r = root[it->second];
    if (h.wide) {
        if (r.blob.size() != 8) return false;
        v = (uint64_t)rd32le(r.blob, 0) | (uint64_t)rd32le(r.blob, 4) << 32;
        return true;
    }
    if (r.type() != tUint) return false;
    v = r.u;
    return true;
}

bool Rr3Save::hiddenGet(const Rr3Hidden& h, const std::map<std::string, uint64_t>& keys,
                        uint64_t& value) const {
    uint64_t st;
    auto k = keys.find(h.keyName);
    if (!hiddenStored(h, st) || k == keys.end()) return false;
    value = st ^ k->second;
    return true;
}

bool Rr3Save::hiddenSet(const Rr3Hidden& h, const std::map<std::string, uint64_t>& keys,
                        uint64_t v, std::string& err) {
    Rr3Rec* r = find(h.field);
    auto k = keys.find(h.keyName);
    if (!r || k == keys.end()) { err = "not calibrated"; return false; }
    if (h.wide) {
        Bytes b(8);
        uint64_t x = v ^ k->second;
        wr32le(b, 0, (uint32_t)x);
        wr32le(b, 4, (uint32_t)(x >> 32));
        r->setBlob(b);
    } else {
        r->u = (v ^ k->second) & 0xffffffffu;
        r->raw = { 0x81 };
        Bytes vv = putUvarint(r->u);
        r->raw.insert(r->raw.end(), vv.begin(), vv.end());
        r->tag = 0x81;
    }
    return true;
}

bool Rr3Save::calibrate(const Rr3Hidden& h, std::map<std::string, uint64_t>& keys,
                        uint64_t shown, std::string& err) {
    uint64_t st;
    if (!hiddenStored(h, st)) { err = std::string(h.label) + " field not found in this save"; return false; }
    keys[h.keyName] = st ^ shown;
    return true;
}

std::vector<size_t> Rr3Save::sortedRoot() const {
    std::vector<size_t> idx(root.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::vector<std::string> keys(root.size());
    for (size_t i = 0; i < root.size(); ++i) keys[i] = lower(root[i].name);
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return keys[a] < keys[b]; });
    return idx;
}

// ============================================================== keys, finding

std::map<std::string, uint64_t> loadKeys(const std::string& file) {
    std::map<std::string, uint64_t> k;
    Bytes b;
    if (!readFile(file, b)) return k;
    std::string text(b.begin(), b.end());
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        std::string line = trim(text.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
        size_t eq = line.find('=');
        if (eq != std::string::npos && line[0] != '#') {
            std::string name = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
            char* end = nullptr;
            unsigned long long v = strtoull(val.c_str(), &end, 16);
            if (!name.empty() && !val.empty() && end && !*end) k[name] = v;
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return k;
}

bool saveKeys(const std::string& file, const std::map<std::string, uint64_t>& keys) {
    std::string s = "# Real Racing 3 hidden-value keys (made by Calibrate)\n";
    char buf[96];
    for (const auto& kv : keys) {
        snprintf(buf, sizeof(buf), "=%016llx\n", (unsigned long long)kv.second);
        s += kv.first + buf;
    }
    std::error_code ec;
    fs::create_directories(fs::u8path(file).parent_path(), ec);
    return writeFile(file, (const uint8_t*)s.data(), s.size());
}

bool findSave(const std::string& root, std::string& game, std::string& dir) {
    std::vector<std::pair<fs::path, int>> q{ { fs::u8path(root), 0 } };
    for (size_t head = 0; head < q.size() && q.size() < 20000; ++head) {
        fs::path p = q[head].first;
        int depth = q[head].second;
        std::error_code ec;
        std::vector<fs::path> subdirs;
        for (auto& e : fs::directory_iterator(p, ec)) {
            std::error_code ec2;
            std::string n = e.path().filename().u8string();
            if (e.is_directory(ec2)) {
                if (n.compare(0, 19, "save_editor_backup_")) subdirs.push_back(e.path());
                continue;
            }
            std::string u; long k;
            if (lower(n) == "character.2.dat") { game = "rr3"; dir = p.u8string(); return true; }
            if ((n.size() > 5 && n.compare(n.size() - 5, 5, "_m.sb") == 0) || sectionName(n, u, k)) {
                game = "nfs"; dir = p.u8string(); return true;
            }
        }
        if (depth < 4) {
            std::sort(subdirs.begin(), subdirs.end());
            for (auto& s : subdirs) q.push_back({ s, depth + 1 });
        }
    }
    return false;
}

// ================================================================ profile picture

namespace {
bool looksLikePicture(const Bytes& b, std::string* fmt) {
    if (b.size() > 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') {
        if (fmt) *fmt = "png";
        return true;
    }
    if (b.size() > 4 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) {
        if (fmt) *fmt = "jpg";
        return true;
    }
    return false;
}
bool avatarWord(std::string n) {
    for (char& c : n) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    for (const char* k : { "avatar", "portrait", "profilepic", "profile_pic", "profileimage",
                           "profile_image", "picture", "photo", "headshot" })
        if (n.find(k) != std::string::npos) return true;
    return false;
}
} // namespace

Image fitPicture(const Image& pic, int w, int h) {
    Image src = toRgba(pic);
    if (!src.ok() || w <= 0 || h <= 0) return src;
    // crop the source to the target's shape, centred, then scale
    double want = (double)w / h, have = (double)src.width / src.height;
    int cw = src.width, ch = src.height, cx = 0, cy = 0;
    if (have > want) { cw = std::max(1, (int)(src.height * want + 0.5)); cx = (src.width - cw) / 2; }
    else if (have < want) { ch = std::max(1, (int)(src.width / want + 0.5)); cy = (src.height - ch) / 2; }
    Image crop;
    crop.width = cw; crop.height = ch; crop.channels = 4;
    crop.pixels.resize((size_t)cw * ch * 4);
    for (int y = 0; y < ch; ++y)
        memcpy(&crop.pixels[(size_t)y * cw * 4], &src.pixels[((size_t)(y + cy) * src.width + cx) * 4],
               (size_t)cw * 4);
    return resizeRgba(crop, w, h);
}

long rr3FindPicture(const Rr3Save& s, std::string* format) {
    long best = -1;
    bool bestNamed = false;
    for (size_t i = 0; i < s.root.size(); ++i) {
        const Rr3Rec& r = s.root[i];
        std::string f;
        if (r.type() != tBlob || !looksLikePicture(r.blob, &f)) continue;
        bool named = avatarWord(r.name);
        if (best < 0 || (named && !bestNamed)) {
            best = (long)i;
            bestNamed = named;
            if (format) *format = f;
        }
    }
    return best;
}

bool rr3SetPicture(Rr3Save& s, size_t index, const Image& pic, std::string& report) {
    if (index >= s.root.size()) { report = "no such field"; return false; }
    Rr3Rec& r = s.root[index];
    std::string fmt;
    if (!looksLikePicture(r.blob, &fmt)) { report = "the field holds no picture"; return false; }
    Image old;
    int w = 256, h = 256;
    if (decodeImageFile(r.blob.data(), r.blob.size(), old) && old.width > 0) { w = old.width; h = old.height; }
    Image fitted = fitPicture(pic, w, h);
    Bytes enc = fmt == "png" ? encodePng(fitted) : encodeJpeg(fitted, 92);
    if (enc.empty()) { report = "the picture could not be encoded"; return false; }
    r.setBlob(enc);
    char b[200];
    snprintf(b, sizeof(b), "%s: %d x %d %s, %u bytes", r.name.c_str(), w, h,
             fmt == "png" ? "PNG" : "JPEG", (unsigned)enc.size());
    report = b;
    return true;
}

// ---- the player's name ----
namespace {
bool nameField(const std::string& path) {
    size_t cut = path.find_last_of("/.");
    std::string leaf = lower(cut == std::string::npos ? path : path.substr(cut + 1));
    if (leaf.compare(0, 2, "m_") == 0) leaf = leaf.substr(2);
    std::string k;
    for (char c : leaf) if (c != '_') k += c;
    static const char* kNames[] = { "name", "playername", "displayname", "nickname", "username",
                                    "drivername", "profilename", "gamertag", "racername",
                                    "playerdisplayname", "personaname", "persona", "screenname" };
    for (const char* n : kNames) if (k == n) return true;
    return false;
}
}

long nfsPlayerNameLeaf(const NfsSave& s) {
    long best = -1;
    int bestScore = -1;
    for (size_t i = 0; i < s.leaves.size(); ++i) {
        const NfsLeaf& l = s.leaves[i];
        if (l.type != sbStr || !nameField(l.path)) continue;
        std::string lp = lower(l.path);
        // a car's or an event's name sits in an array; the player's does not
        int score = lp.find('[') == std::string::npos ? 2 : 0;
        if (lp.find("player") != std::string::npos || lp.find("profile") != std::string::npos ||
            lp.find("user") != std::string::npos || lp.find("social") != std::string::npos) score += 2;
        if (score > bestScore) { bestScore = score; best = (long)i; }
    }
    return bestScore >= 2 ? best : -1;
}

Rr3Rec* rr3PlayerName(Rr3Save& s) {
    for (Rr3Rec& r : s.root)
        if (r.type() == tString && nameField(r.name)) return &r;
    return nullptr;
}

// A string field takes a new entry in the file's string table (CHDR offsets
// and lengths, CDAT the characters, unterminated); the leaf points at it and
// the file is laid out again, chunk by chunk.
bool NfsSave::setString(const NfsLeaf& l, const std::string& text, std::string& err) {
    if (l.type != sbStr) { err = "not a text field"; return false; }
    if ((size_t)l.file >= files.size()) { err = "no such file"; return false; }
    NfsFile& f = files[l.file];
    std::string t = trim(text);
    if (t.empty()) { err = "the name cannot be empty"; return false; }
    if (t.size() > 64) { err = "64 characters at most"; return false; }
    size_t idx = f.strs.size();
    for (size_t i = 0; i < f.strs.size(); ++i) if (f.strs[i] == t) { idx = i; break; }
    if (idx == f.strs.size()) {
        if (idx > 0xFFFF) { err = "the save's string table is full"; return false; }
        // lay the file out again with the string added
        std::vector<std::pair<size_t, std::string>> order;
        for (const auto& c : f.chunks) order.push_back({ c.second.hdr, c.first });
        std::sort(order.begin(), order.end());
        Bytes out(f.P.begin(), f.P.begin() + 8);
        size_t oldData = f.chunks["DATA"].data, newData = 0;
        uint32_t cdatLen = (uint32_t)f.chunks["CDAT"].len;
        for (const auto& o : order) {
            const NfsFile::Chunk& c = f.chunks[o.second];
            Bytes body(f.P.begin() + c.data, f.P.begin() + c.data + c.len);
            if (o.second == "CHDR") {
                uint32_t v[2] = { cdatLen, (uint32_t)t.size() };
                const uint8_t* b = (const uint8_t*)v;
                body.insert(body.end(), b, b + 8);
            } else if (o.second == "CDAT") {
                body.insert(body.end(), t.begin(), t.end());
            }
            size_t hdr = out.size();
            out.insert(out.end(), f.P.begin() + c.hdr, f.P.begin() + c.hdr + 4);
            out.resize(out.size() + 8, 0);
            wr32le(out, hdr + 4, (uint32_t)body.size());
            wr32le(out, hdr + 8, fnv1(body.data(), body.size()));
            if (o.second == "DATA") newData = out.size();
            out.insert(out.end(), body.begin(), body.end());
            while (out.size() % 4) out.push_back(0);
        }
        f.P = std::move(out);
        std::string e;
        if (!parseSbin(f, e)) { err = "rebuilding the section failed: " + e; return false; }
        // the leaves point into DATA: move them with it
        long delta = (long)newData - (long)oldData;
        if (delta)
            for (NfsLeaf& x : leaves) if (x.file == l.file) x.off = (size_t)((long)x.off + delta);
    }
    // the leaf itself (found again: `l` may be one of `leaves`)
    for (NfsLeaf& x : leaves)
        if (x.file == l.file && x.path == l.path) { f.P[x.off] = (uint8_t)idx; f.P[x.off + 1] = (uint8_t)(idx >> 8); break; }
    rehash(f, "DATA");
    f.dirty = true;
    return true;
}

std::string nfsAvatarName(const NfsSave& s, std::string* field) {
    for (const NfsLeaf& l : s.leaves) {
        if (!avatarWord(l.path)) continue;
        std::string v = s.get(l);
        if (v.empty() || v == "?") continue;
        bool numeric = true;
        for (char c : v) if (!(c >= '0' && c <= '9') && c != '-') numeric = false;
        if (numeric) continue;
        if (field) *field = l.path;
        return v;
    }
    return std::string();
}

std::string rr3AvatarName(const Rr3Save& s, std::string* field) {
    for (const Rr3Rec& r : s.root) {
        if (r.type() != tString || r.s.empty() || !avatarWord(r.name)) continue;
        if (field) *field = r.name;
        return r.s;
    }
    return std::string();
}

} // namespace saves
} // namespace nfsnl
