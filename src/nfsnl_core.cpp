// nfsnl_core.cpp - utility, DEFLATE, Zstandard loading, SBIN, PACK parsing
#include "nfsnl.h"

#include <cstring>
#include <cstdio>
#include <algorithm>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <direct.h>
#else
#  include <dlfcn.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#endif

namespace nfsnl {

// ================================================================ utility

// Paths are UTF-8 throughout the tool. On Windows, fopen reads a narrow path
// in the ANSI code page instead, so a folder with a Polish, German or any
// other non-ASCII letter in its name could not be opened; the wide call takes
// the path as it really is.
FILE* openFile(const std::string& path, const char* mode) {
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), &w[0], n);
    std::wstring m;
    for (const char* c = mode; *c; ++c) m += (wchar_t)*c;
    FILE* f = n > 0 ? _wfopen(w.c_str(), m.c_str()) : nullptr;
    return f ? f : fopen(path.c_str(), mode);      // a path that is ANSI after all
#else
    return fopen(path.c_str(), mode);
#endif
}

bool readFile(const std::string& path, Bytes& out) {
    FILE* f = openFile(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return false; }
    out.resize((size_t)n);
    size_t rd = n ? fread(out.data(), 1, (size_t)n, f) : 0;
    fclose(f);
    out.resize(rd);
    return true;
}

size_t fileSizeOf(const std::string& path) {
    FILE* f = openFile(path, "rb");
    if (!f) return 0;
#ifdef _WIN32
    _fseeki64(f, 0, SEEK_END);
    long long n = (long long)_ftelli64(f);
#else
    fseeko(f, 0, SEEK_END);
    long long n = (long long)ftello(f);
#endif
    fclose(f);
    return n > 0 ? (size_t)n : 0;
}

static void makeOneDir(const std::string& d) {
    if (d.empty()) return;
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, d.c_str(), (int)d.size(), nullptr, 0);
    if (n > 0) {
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, d.c_str(), (int)d.size(), &w[0], n);
        if (_wmkdir(w.c_str()) == 0) return;
    }
    _mkdir(d.c_str());
#else
    mkdir(d.c_str(), 0775);
#endif
}

void makeDirsFor(const std::string& filePath) {
    std::string cur;
    for (size_t i = 0; i < filePath.size(); ++i) {
        char c = filePath[i];
        if (c == '/' || c == '\\') {
            if (!cur.empty() && cur != "." && !(cur.size() == 2 && cur[1] == ':'))
                makeOneDir(cur);
        }
        cur.push_back(c);
    }
}

bool writeFile(const std::string& path, const uint8_t* data, size_t len) {
    makeDirsFor(path);
    FILE* f = openFile(path, "wb");
    if (!f) return false;
    if (len) fwrite(data, 1, len, f);
    fclose(f);
    return true;
}
bool writeFile(const std::string& path, const Bytes& d) {
    return writeFile(path, d.data(), d.size());
}

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    char last = a[a.size() - 1];
    if (last == '/' || last == '\\') return a + b;
#ifdef _WIN32
    return a + "\\" + b;
#else
    return a + "/" + b;
#endif
}

std::string extensionOf(const std::string& p) {
    size_t dot = p.find_last_of('.');
    size_t slash = p.find_last_of("/\\");
    if (dot == std::string::npos) return "";
    if (slash != std::string::npos && dot < slash) return "";
    std::string e = p.substr(dot + 1);
    for (auto& c : e) c = (char)tolower((unsigned char)c);
    return e;
}

std::string stripExtension(const std::string& p) {
    size_t dot = p.find_last_of('.');
    size_t slash = p.find_last_of("/\\");
    if (dot == std::string::npos) return p;
    if (slash != std::string::npos && dot < slash) return p;
    return p.substr(0, dot);
}

std::string baseName(const std::string& p) {
    size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

// ================================================================ LZ4
// Block format (no frame): Real Racing Next's SBIN buffers, BARG flag 1.
bool lz4DecompressBlock(const uint8_t* src, size_t n, size_t expected, Bytes& out) {
    out.clear();
    out.reserve(expected);
    size_t i = 0;
    while (i < n) {
        uint8_t t = src[i++];
        size_t lit = t >> 4;
        if (lit == 15) {
            uint8_t b;
            do { if (i >= n) return false; b = src[i++]; lit += b; } while (b == 255);
        }
        if (i + lit > n) return false;
        out.insert(out.end(), src + i, src + i + lit);
        i += lit;
        if (i >= n) break;                      // the last sequence has no match
        if (i + 2 > n) return false;
        size_t off = src[i] | (src[i + 1] << 8);
        i += 2;
        if (off == 0 || off > out.size()) return false;
        size_t ml = t & 15;
        if (ml == 15) {
            uint8_t b;
            do { if (i >= n) return false; b = src[i++]; ml += b; } while (b == 255);
        }
        ml += 4;
        if (out.size() + ml > expected + 64 && expected) return false;
        size_t from = out.size() - off;
        for (size_t k = 0; k < ml; ++k) out.push_back(out[from + k]);
    }
    return !expected || out.size() == expected;
}

// ================================================================ inflate
// Straightforward RFC1951 implementation.

namespace {

struct BitReader {
    const uint8_t* src;
    size_t len;
    size_t pos = 0;
    uint32_t bitBuf = 0;
    int bitCount = 0;
    bool error = false;

    BitReader(const uint8_t* s, size_t l) : src(s), len(l) {}

    int getBit() {
        if (bitCount == 0) {
            if (pos >= len) { error = true; return 0; }
            bitBuf = src[pos++];
            bitCount = 8;
        }
        int b = bitBuf & 1;
        bitBuf >>= 1;
        bitCount--;
        return b;
    }
    uint32_t getBits(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v |= (uint32_t)getBit() << i;
        return v;
    }
    void alignByte() { bitCount = 0; bitBuf = 0; }
};

// Canonical Huffman decoder
struct Huffman {
    // counts[i] = number of codes of length i; symbols sorted by code
    std::vector<uint16_t> counts;
    std::vector<uint16_t> symbols;

    void build(const uint8_t* lengths, int n) {
        counts.assign(16, 0);
        for (int i = 0; i < n; ++i) counts[lengths[i]]++;
        counts[0] = 0;
        std::vector<uint16_t> offs(16, 0);
        for (int i = 1; i < 16; ++i) offs[i] = offs[i - 1] + counts[i - 1];
        symbols.assign(n, 0);
        for (int i = 0; i < n; ++i)
            if (lengths[i]) symbols[offs[lengths[i]]++] = (uint16_t)i;
    }

    int decode(BitReader& br) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= br.getBit();
            if (br.error) return -1;
            int count = counts[len];
            if (code - first < count) return symbols[index + (code - first)];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        return -1;
    }
};

const uint16_t kLenBase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,
                               67,83,99,115,131,163,195,227,258};
const uint8_t  kLenExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
const uint16_t kDistBase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
                                1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
const uint8_t  kDistExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

bool inflateBlockData(BitReader& br, const Huffman& lit, const Huffman& dist, Bytes& out) {
    for (;;) {
        int sym = lit.decode(br);
        if (sym < 0) return false;
        if (sym < 256) {
            out.push_back((uint8_t)sym);
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            int length = kLenBase[sym] + (int)br.getBits(kLenExtra[sym]);
            int dsym = dist.decode(br);
            if (dsym < 0 || dsym >= 30) return false;
            int d = kDistBase[dsym] + (int)br.getBits(kDistExtra[dsym]);
            if ((size_t)d > out.size()) return false;
            size_t start = out.size() - d;
            for (int i = 0; i < length; ++i) out.push_back(out[start + i]);
        }
        if (br.error) return false;
    }
}

void buildFixedTrees(Huffman& lit, Huffman& dist) {
    uint8_t l[288];
    for (int i = 0; i < 144; ++i) l[i] = 8;
    for (int i = 144; i < 256; ++i) l[i] = 9;
    for (int i = 256; i < 280; ++i) l[i] = 7;
    for (int i = 280; i < 288; ++i) l[i] = 8;
    lit.build(l, 288);
    uint8_t d[30];
    for (int i = 0; i < 30; ++i) d[i] = 5;
    dist.build(d, 30);
}

bool readDynamicTrees(BitReader& br, Huffman& lit, Huffman& dist) {
    int hlit = (int)br.getBits(5) + 257;
    int hdist = (int)br.getBits(5) + 1;
    int hclen = (int)br.getBits(4) + 4;
    static const int order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    uint8_t clen[19] = {0};
    for (int i = 0; i < hclen; ++i) clen[order[i]] = (uint8_t)br.getBits(3);
    Huffman cl;
    cl.build(clen, 19);

    std::vector<uint8_t> lengths(hlit + hdist, 0);
    int i = 0;
    while (i < hlit + hdist) {
        int sym = cl.decode(br);
        if (sym < 0) return false;
        if (sym < 16) {
            lengths[i++] = (uint8_t)sym;
        } else if (sym == 16) {
            if (i == 0) return false;
            uint8_t prev = lengths[i - 1];
            int rep = 3 + (int)br.getBits(2);
            while (rep-- && i < hlit + hdist) lengths[i++] = prev;
        } else if (sym == 17) {
            int rep = 3 + (int)br.getBits(3);
            while (rep-- && i < hlit + hdist) lengths[i++] = 0;
        } else {
            int rep = 11 + (int)br.getBits(7);
            while (rep-- && i < hlit + hdist) lengths[i++] = 0;
        }
        if (br.error) return false;
    }
    lit.build(lengths.data(), hlit);
    dist.build(lengths.data() + hlit, hdist);
    return true;
}

} // namespace

bool inflateRaw(const uint8_t* src, size_t srcLen, Bytes& out, size_t* consumed) {
    BitReader br(src, srcLen);
    for (;;) {
        int final = br.getBit();
        int type = (int)br.getBits(2);
        if (br.error) return false;
        if (type == 0) {
            br.alignByte();
            if (br.pos + 4 > br.len) return false;
            uint16_t len = (uint16_t)(src[br.pos] | (src[br.pos + 1] << 8));
            br.pos += 4;
            if (br.pos + len > br.len) return false;
            out.insert(out.end(), src + br.pos, src + br.pos + len);
            br.pos += len;
        } else if (type == 1) {
            Huffman lit, dist;
            buildFixedTrees(lit, dist);
            if (!inflateBlockData(br, lit, dist, out)) return false;
        } else if (type == 2) {
            Huffman lit, dist;
            if (!readDynamicTrees(br, lit, dist)) return false;
            if (!inflateBlockData(br, lit, dist, out)) return false;
        } else {
            return false;
        }
        if (final) break;
    }
    if (consumed) {
        // round up to the next whole byte
        *consumed = br.pos;
    }
    return true;
}

bool inflateZlib(const uint8_t* src, size_t srcLen, Bytes& out, size_t* consumed) {
    if (srcLen < 2) return false;
    size_t used = 0;
    if (!inflateRaw(src + 2, srcLen - 2, out, &used)) return false;
    if (consumed) *consumed = used + 2 + 4;  // header + adler32
    return true;
}

bool inflateGzip(const uint8_t* src, size_t srcLen, Bytes& out, size_t* consumed) {
    if (srcLen < 18) return false;
    if (src[0] != 0x1f || src[1] != 0x8b || src[2] != 8) return false;
    uint8_t flg = src[3];
    size_t p = 10;
    if (flg & 4) {                       // FEXTRA
        if (p + 2 > srcLen) return false;
        uint16_t xlen = (uint16_t)(src[p] | (src[p + 1] << 8));
        p += 2 + xlen;
    }
    if (flg & 8)  { while (p < srcLen && src[p]) p++; p++; }   // FNAME
    if (flg & 16) { while (p < srcLen && src[p]) p++; p++; }   // FCOMMENT
    if (flg & 2)  { p += 2; }                                  // FHCRC
    if (p >= srcLen) return false;
    size_t used = 0;
    if (!inflateRaw(src + p, srcLen - p, out, &used)) return false;
    if (consumed) *consumed = p + used + 8;  // + crc32 + isize
    return true;
}

// ================================================================ zstd

namespace {

typedef size_t (*ZSTD_decompress_fn)(void*, size_t, const void*, size_t);
typedef unsigned long long (*ZSTD_getFrameContentSize_fn)(const void*, size_t);
typedef unsigned (*ZSTD_isError_fn)(size_t);

struct ZstdLib {
    void* handle = nullptr;
    ZSTD_decompress_fn decompress = nullptr;
    ZSTD_getFrameContentSize_fn frameSize = nullptr;
    ZSTD_isError_fn isError = nullptr;
    std::string name;
    bool tried = false;

    void* sym(const char* s) {
#ifdef _WIN32
        return (void*)GetProcAddress((HMODULE)handle, s);
#else
        return dlsym(handle, s);
#endif
    }

    void load() {
        if (tried) return;
        tried = true;
        static const char* candidates[] = {
#ifdef _WIN32
            "libzstd.dll", "zstd.dll",
#else
            "libzstd.so.1", "libzstd.so", "libzstd.dylib",
#endif
            nullptr
        };
        for (int i = 0; candidates[i]; ++i) {
#ifdef _WIN32
            handle = (void*)LoadLibraryA(candidates[i]);
#else
            handle = dlopen(candidates[i], RTLD_NOW);
#endif
            if (!handle) continue;
            decompress = (ZSTD_decompress_fn)sym("ZSTD_decompress");
            frameSize = (ZSTD_getFrameContentSize_fn)sym("ZSTD_getFrameContentSize");
            isError = (ZSTD_isError_fn)sym("ZSTD_isError");
            if (decompress) { name = candidates[i]; return; }
            handle = nullptr;
        }
    }
};

ZstdLib& zstdLib() {
    static ZstdLib lib;
    lib.load();
    return lib;
}

} // namespace

bool zstdAvailable() { return zstdLib().decompress != nullptr; }
std::string zstdBackend() {
    ZstdLib& l = zstdLib();
    return l.decompress ? l.name : std::string("unavailable");
}

bool zstdDecompress(const uint8_t* src, size_t srcLen, size_t expectedSize, Bytes& out) {
    ZstdLib& lib = zstdLib();
    if (!lib.decompress) return false;
    size_t want = expectedSize;
    if (!want && lib.frameSize) {
        unsigned long long fs = lib.frameSize(src, srcLen);
        // ZSTD_CONTENTSIZE_UNKNOWN / _ERROR
        if (fs != (unsigned long long)-1 && fs != (unsigned long long)-2 && fs > 0)
            want = (size_t)fs;
    }
    if (!want) want = srcLen * 40 + (1u << 20);
    out.resize(want);
    size_t got = lib.decompress(out.data(), out.size(), src, srcLen);
    if (lib.isError && lib.isError(got)) { out.clear(); return false; }
    if (got > out.size()) { out.clear(); return false; }
    out.resize(got);
    return true;
}

// ================================================================ deflate

namespace {

struct BitWriter {
    Bytes& out;
    uint32_t acc = 0;
    int n = 0;
    explicit BitWriter(Bytes& o) : out(o) {}
    void bits(uint32_t v, int count) {          // LSB first
        acc |= v << n;
        n += count;
        while (n >= 8) { out.push_back((uint8_t)acc); acc >>= 8; n -= 8; }
    }
    void huff(uint32_t code, int len) {         // Huffman codes go MSB first
        uint32_t r = 0;
        for (int i = 0; i < len; ++i) r |= ((code >> i) & 1) << (len - 1 - i);
        bits(r, len);
    }
    void flush() { if (n > 0) { out.push_back((uint8_t)acc); acc = 0; n = 0; } }
};

void fixedLiteral(BitWriter& w, int sym) {
    if (sym < 144)      w.huff(0x30 + sym, 8);
    else if (sym < 256) w.huff(0x190 + sym - 144, 9);
    else if (sym < 280) w.huff(sym - 256, 7);
    else                w.huff(0xC0 + sym - 280, 8);
}

const uint16_t kDfLenBase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,
                               67,83,99,115,131,163,195,227,258};
const uint8_t kDfLenExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
const uint16_t kDfDistBase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
                                1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
const uint8_t kDfDistExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

void writeMatch(BitWriter& w, int len, int dist) {
    int li = 28;
    while (li > 0 && kDfLenBase[li] > len) --li;
    fixedLiteral(w, 257 + li);
    if (kDfLenExtra[li]) w.bits((uint32_t)(len - kDfLenBase[li]), kDfLenExtra[li]);
    int di = 29;
    while (di > 0 && kDfDistBase[di] > dist) --di;
    w.huff((uint32_t)di, 5);
    if (kDfDistExtra[di]) w.bits((uint32_t)(dist - kDfDistBase[di]), kDfDistExtra[di]);
}

} // namespace

Bytes deflateRaw(const uint8_t* src, size_t len) {
    Bytes out;
    out.reserve(len / 2 + 64);
    BitWriter w(out);
    const size_t kWindow = 32768, kHashSize = 1 << 15;
    const int kMaxChain = 48;
    std::vector<int32_t> head(kHashSize, -1), prev(len ? len : 1, -1);
    auto hash3 = [&](size_t i) {
        return ((uint32_t)src[i] * 506832829u ^ (uint32_t)src[i + 1] * 2654435761u ^
                (uint32_t)src[i + 2] * 40503u) & (kHashSize - 1);
    };
    // one final block with the fixed codes; a block may be any length
    w.bits(1, 1);
    w.bits(1, 2);
    size_t i = 0;
    auto insert = [&](size_t at) {
        if (at + 2 >= len) return;
        uint32_t h = hash3(at);
        prev[at] = head[h];
        head[h] = (int32_t)at;
    };
    auto longest = [&](size_t at, int& bestDist) {
        int best = 0;
        if (at + 2 >= len) return 0;
        int32_t c = head[hash3(at)];
        int chain = kMaxChain;
        size_t maxLen = std::min<size_t>(258, len - at);
        while (c >= 0 && chain-- > 0 && at - (size_t)c <= kWindow) {
            const uint8_t* a = src + c;
            const uint8_t* b = src + at;
            if (a[best] == b[best]) {
                size_t l = 0;
                while (l < maxLen && a[l] == b[l]) ++l;
                if ((int)l > best) {
                    best = (int)l;
                    bestDist = (int)(at - (size_t)c);
                    if (l == maxLen) break;
                }
            }
            c = prev[c];
        }
        return best >= 3 ? best : 0;
    };
    while (i < len) {
        int dist = 0;
        int l = longest(i, dist);
        if (l) {
            // one step of lazy matching: a longer match one byte on wins
            int dist2 = 0;
            insert(i);
            int l2 = (i + 1 < len) ? longest(i + 1, dist2) : 0;
            if (l2 > l + 1) {
                fixedLiteral(w, src[i]);
                ++i;
                continue;
            }
            writeMatch(w, l, dist);
            for (size_t k = 1; k < (size_t)l; ++k) insert(i + k);
            i += (size_t)l;
        } else {
            fixedLiteral(w, src[i]);
            insert(i);
            ++i;
        }
    }
    fixedLiteral(w, 256);
    w.flush();
    return out;
}

static uint32_t adler32Of(const uint8_t* d, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) { a = (a + d[i]) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}

uint32_t crc32Of(const uint8_t* d, size_t n) {
    // built once; a function-local static is thread-safe
    struct Table { uint32_t v[256]; Table() {
        for (uint32_t k = 0; k < 256; ++k) {
            uint32_t c = k;
            for (int j = 0; j < 8; ++j) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            v[k] = c;
        }
    } };
    static const Table t;
    const uint32_t* table = t.v;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

Bytes zlibCompress(const uint8_t* src, size_t len) {
    Bytes out = { 0x78, 0x9C };
    Bytes body = deflateRaw(src, len);
    out.insert(out.end(), body.begin(), body.end());
    uint32_t a = adler32Of(src, len);
    for (int k = 3; k >= 0; --k) out.push_back((uint8_t)(a >> (8 * k)));
    return out;
}

Bytes gzipCompress(const uint8_t* src, size_t len) {
    Bytes out = { 0x1F, 0x8B, 8, 0, 0, 0, 0, 0, 0, 0x0B };
    Bytes body = deflateRaw(src, len);
    out.insert(out.end(), body.begin(), body.end());
    uint32_t c = crc32Of(src, len);
    for (int k = 0; k < 4; ++k) out.push_back((uint8_t)(c >> (8 * k)));
    uint32_t n = (uint32_t)len;
    for (int k = 0; k < 4; ++k) out.push_back((uint8_t)(n >> (8 * k)));
    return out;
}

// ================================================================ SBIN

static inline size_t align4(size_t p) { return (p + 3) & ~(size_t)3; }

std::vector<SbinChunk> sbinChunks(const uint8_t* data, size_t len) {
    std::vector<SbinChunk> out;
    if (len < 12) return out;
    size_t pos = 8;  // 'SBIN' + version
    while (pos + 12 <= len) {
        size_t p = align4(pos);
        if (p + 12 > len) break;
        uint32_t size;
        memcpy(&size, data + p + 4, 4);
        size_t start = p + 12;
        if (start + size > len) break;
        SbinChunk c;
        memcpy(c.tag, data + p, 4);
        c.tag[4] = 0;
        c.data = data + start;
        c.size = size;
        out.push_back(c);
        pos = start + size;
    }
    return out;
}

const SbinChunk* findChunk(const std::vector<SbinChunk>& chunks, const char* tag) {
    for (const auto& c : chunks)
        if (memcmp(c.tag, tag, 4) == 0) return &c;
    return nullptr;
}

std::vector<std::string> sbinNames(const std::vector<SbinChunk>& chunks) {
    std::vector<std::string> names;
    const SbinChunk* chdr = findChunk(chunks, "CHDR");
    const SbinChunk* cdat = findChunk(chunks, "CDAT");
    // SBIN version 2 (Hot Pursuit) keeps its names in STRS instead: a count,
    // then each name as a 32-bit length and its bytes; 0 means no name
    const SbinChunk* strs = findChunk(chunks, "STRS");
    if ((!chdr || !cdat) && strs && strs->size >= 4) {
        uint32_t n;
        memcpy(&n, strs->data, 4);
        size_t p = 4;
        names.push_back("");          // the references count from 1
        for (uint32_t i = 0; i < n && i < 1000000 && p + 4 <= strs->size; ++i) {
            uint32_t sz;
            memcpy(&sz, strs->data + p, 4);
            p += 4;
            if (sz > strs->size - p) break;
            std::string s((const char*)strs->data + p, sz);
            while (!s.empty() && s.back() == '\0') s.pop_back();
            names.push_back(s);
            p += sz;
        }
        return names;
    }
    if (!chdr || !cdat) return names;
    size_t n = chdr->size / 8;
    names.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        uint32_t off, sz;
        memcpy(&off, chdr->data + i * 8, 4);
        memcpy(&sz, chdr->data + i * 8 + 4, 4);
        if (off + sz <= cdat->size) {
            std::string s((const char*)cdat->data + off, sz);
            while (!s.empty() && s.back() == '\0') s.pop_back();
            names.push_back(s);
        } else {
            names.push_back("");
        }
    }
    return names;
}

// ================================================================ PACK

std::map<uint32_t, std::string> PackManifest::resolvePaths() const {
    std::map<uint32_t, std::string> out;
    std::vector<bool> seen(folders.size(), false);

    std::function<void(int, const std::string&)> walk =
        [&](int idx, const std::string& prefix) {
            if (idx < 0 || idx >= (int)folders.size() || seen[idx]) return;
            seen[idx] = true;
            const PackFolder& f = folders[idx];
            std::string nm = f.nameIndex < names.size() ? names[f.nameIndex] : std::string();
            // folders[0] is the publish root and is not part of the path
            std::string path = (idx == 0) ? prefix
                               : (prefix.empty() ? nm : prefix + "/" + nm);
            // clamp to the tables: a damaged count would otherwise spin
            // through four billion indices that all miss
            uint64_t fEnd = std::min<uint64_t>((uint64_t)f.filesIndex + f.filesCount, files.size());
            for (uint64_t k = f.filesIndex; k < fEnd; ++k) {
                {
                    uint32_t ni = files[k].nameIndex;
                    std::string fn = ni < names.size() ? names[ni] : std::string();
                    out[k] = path.empty() ? fn : path + "/" + fn;
                }
            }
            // child folder indices are 1-based
            uint64_t cEnd = std::min<uint64_t>((uint64_t)f.foldersIndex + f.foldersCount,
                                               (uint64_t)folders.size() + 1);
            for (uint64_t c = f.foldersIndex; c < cEnd; ++c)
                walk((int)c - 1, path);
        };
    walk(0, "");
    return out;
}

PackManifest parsePackManifest(const Bytes& meta, size_t packSize, size_t dataBase) {
    PackManifest man;
    auto chunks = sbinChunks(meta.data(), meta.size());
    man.names = sbinNames(chunks);
    const SbinChunk* dataChunk = findChunk(chunks, "DATA");
    if (!dataChunk || man.names.empty()) return man;

    const uint8_t* D = dataChunk->data;
    size_t n = dataChunk->size;
    auto u32 = [&](size_t off) -> uint32_t {
        uint32_t v; memcpy(&v, D + off, 4); return v;
    };
    size_t expectedSpan = packSize - dataBase;

    // --- the tables, read where they actually are ---------------------------
    //
    // The DATA chunk has a fixed layout, and this is how Bigchillghost's
    // QuickBMS unpacker reads it: 0x24 bytes in, three counted tables in a row
    //
    //     u32 tag, u32 count, count x 20   folders
    //     u32 tag, u32 count, count x 20   files
    //     u32 tag, u32 count, count x 16   cabinets
    //
    // Up to 0.7.2 the cabinet table was found by scanning for a run of records
    // whose packed data chained end to end up to the last byte of the .pack.
    // That is true only when every cabinet is inside the .pack. A cabinet
    // flagged 0x40 lives in a separate file, <pack name>\<n>.cab, so any pack
    // with even one of those failed the check and lost its whole cabinet
    // table - and every asset in it. Car packs split that way are why the
    // AE86, the S2000 and the Falcon came out with their scripts and nothing
    // else. Reading the table positionally has no such dependency.
    bool structured = false;
    if (n >= 0x24 + 24) {
        size_t q = 0x24;
        auto take = [&](size_t recSize, uint32_t& count, size_t& at) -> bool {
            if (q + 8 > n) return false;
            count = u32(q + 4);
            at = q + 8;
            if (count > 2000000 || at + (size_t)count * recSize > n) return false;
            q = at + (size_t)count * recSize;
            return true;
        };
        uint32_t nFold = 0, nFile = 0, nCab = 0;
        size_t atFold = 0, atFile = 0, atCab = 0;
        if (take(20, nFold, atFold) && take(20, nFile, atFile) && take(16, nCab, atCab) &&
            nFile > 0 && nCab > 0) {
            std::vector<Cabinet> cabs(nCab);
            bool ok = true;
            for (uint32_t i = 0; i < nCab && ok; ++i) {
                Cabinet& c = cabs[i];
                c.flags        = u32(atCab + i * 16);
                c.length       = u32(atCab + i * 16 + 4);
                c.packedOffset = u32(atCab + i * 16 + 8);
                c.packedLength = u32(atCab + i * 16 + 12);
                if (c.flags > 0xFFFF) ok = false;
                // an internal cabinet must fit inside the .pack it is in
                if (!(c.flags & CAB_EXTERNAL) &&
                    (uint64_t)c.packedOffset + c.packedLength > expectedSpan + 16)
                    ok = false;
            }
            std::vector<PackFile> files(nFile);
            for (uint32_t i = 0; i < nFile && ok; ++i) {
                PackFile& f = files[i];
                f.nameIndex    = u32(atFile + i * 20);
                f.cabinetIndex = u32(atFile + i * 20 + 4);
                f.offset       = u32(atFile + i * 20 + 8);
                f.length       = u32(atFile + i * 20 + 12);
                if (f.nameIndex >= man.names.size() || f.cabinetIndex >= nCab) ok = false;
            }
            std::vector<PackFolder> folders;
            for (uint32_t i = 0; i < nFold && ok; ++i) {
                PackFolder f;
                f.nameIndex    = u32(atFold + i * 20);
                f.filesIndex   = u32(atFold + i * 20 + 4);
                f.filesCount   = u32(atFold + i * 20 + 8);
                f.foldersIndex = u32(atFold + i * 20 + 12);
                f.foldersCount = u32(atFold + i * 20 + 16);
                if (f.nameIndex >= man.names.size()) { ok = false; break; }
                folders.push_back(f);
            }
            // Record 0 is the unnamed root. Child indices count it as 0, and
            // resolvePaths (written for the older layout scan, which started
            // one record later) treats "published" as folders[0] and child
            // indices as 1-based - so drop the root to keep the two in step.
            if (ok && folders.size() > 1 && man.names[folders[0].nameIndex].empty())
                folders.erase(folders.begin());
            if (ok && !folders.empty()) {
                man.cabinets = std::move(cabs);
                man.files = std::move(files);
                man.folders = std::move(folders);
                structured = true;
            }
        }
    }

    // --- cabinets: (recordSize=16, count) header, then a contiguous chain of
    // packed offsets that ends exactly at the end of the file. Kept as the
    // fallback for any layout the positional read above does not recognise.
    if (!structured && n >= 8) {
        for (size_t hp = 0; hp + 8 <= n; hp += 4) {
            uint32_t recSize = u32(hp), count = u32(hp + 4);
            if (recSize != 16 || count == 0 || count > 200000) continue;
            size_t start = hp + 8;
            if (start + (size_t)count * 16 > n) continue;
            std::vector<Cabinet> recs;
            recs.reserve(count);
            uint64_t prevEnd = 0;
            bool ok = true;
            for (uint32_t i = 0; i < count; ++i) {
                Cabinet c;
                c.flags = u32(start + i * 16);
                c.length = u32(start + i * 16 + 4);
                c.packedOffset = u32(start + i * 16 + 8);
                c.packedLength = u32(start + i * 16 + 12);
                if (c.packedOffset < prevEnd || c.packedOffset - prevEnd >= 16 ||
                    c.packedLength == 0 || c.length == 0 || c.flags > 0xFFFF) {
                    ok = false; break;
                }
                recs.push_back(c);
                prevEnd = (uint64_t)c.packedOffset + c.packedLength;
            }
            if (ok && prevEnd == expectedSpan) { man.cabinets = recs; break; }
        }
    }

    // --- files: (0x10010, count) header, then count 20-byte records
    size_t filesHeaderPos = (size_t)-1;
    for (size_t hp = 0; !structured && hp + 8 <= n; hp += 4) {
        uint32_t marker = u32(hp), count = u32(hp + 4);
        if (marker != 0x10010u || count == 0 || count > 500000) continue;
        size_t start = hp + 8;
        if (start + (size_t)count * 20 > n) continue;
        std::vector<PackFile> recs;
        recs.reserve(count);
        bool ok = true;
        for (uint32_t i = 0; i < count; ++i) {
            PackFile f;
            f.nameIndex = u32(start + i * 20);
            f.cabinetIndex = u32(start + i * 20 + 4);
            f.offset = u32(start + i * 20 + 8);
            f.length = u32(start + i * 20 + 12);
            if (f.nameIndex >= man.names.size() ||
                (!man.cabinets.empty() && f.cabinetIndex >= man.cabinets.size())) {
                ok = false; break;
            }
            recs.push_back(f);
        }
        if (ok) { man.files = recs; filesHeaderPos = hp; break; }
    }

    // --- folders: 20-byte records between the fixed header and the files header
    if (filesHeaderPos != (size_t)-1 && filesHeaderPos > 64) {
        size_t fstart = 64;
        size_t count = (filesHeaderPos - fstart) / 20;
        for (size_t i = 0; i < count; ++i) {
            PackFolder f;
            f.nameIndex = u32(fstart + i * 20);
            if (f.nameIndex >= man.names.size()) break;
            f.filesIndex = u32(fstart + i * 20 + 4);
            f.filesCount = u32(fstart + i * 20 + 8);
            f.foldersIndex = u32(fstart + i * 20 + 12);
            f.foldersCount = u32(fstart + i * 20 + 16);
            man.folders.push_back(f);
        }
    }

    // --- sku/variant: names between "cabinets" and the publish root
    {
        size_t iCab = (size_t)-1, iPub = (size_t)-1;
        for (size_t i = 0; i < man.names.size(); ++i) {
            if (iCab == (size_t)-1 && man.names[i] == "cabinets") iCab = i;
            else if (iCab != (size_t)-1 && man.names[i] == "published") { iPub = i; break; }
        }
        if (iCab != (size_t)-1 && iPub != (size_t)-1) {
            for (size_t i = iCab + 1; i < iPub; ++i)
                if (!man.names[i].empty() && man.names[i] != "default") {
                    man.variant = man.names[i];
                    break;
                }
        }
    }

    man.dataBase = dataBase;
    man.valid = !man.files.empty() && !man.folders.empty();
    return man;
}

std::string classifyByName(const std::string& name) {
    std::string e = extensionOf(name);
    // Real Racing 3 wraps almost everything in a .z, and a few textures in a
    // .z.bin on top of that, so what the file really is lives one or two
    // extensions further in.
    if (e == "bin" || e == "z") {
        std::string inner = stripExtension(name);
        if (extensionOf(inner) == "z") inner = stripExtension(inner);
        if (inner != name) return classifyByName(inner);
    }
    if (e == "sb3d" || e == "m3g") return "model";
    if (e == "points") return "model";
    if (e == "sba" || e == "png" || e == "jpg" || e == "jpeg" ||
        e == "pvr" || e == "ktx" || e == "dds") return "texture";
    if (e == "wem" || e == "wav" || e == "ogg" || e == "mp3" || e == "bnk" || e == "gnsu" || e == "sps" ||
        e == "flac" || e == "m4a" || e == "aac" || e == "opus" || e == "wma" ||
        e == "bank" || e == "fsb" || e == "fev" || e == "sounddef")
        return "sound";
    return "other";
}

// Decompress one cabinet from its packed bytes alone, so a caller that has
// read only that slice off disk can use it as well as one holding the whole
// archive in memory.
bool decompressCabinetBlock(const uint8_t* raw, size_t rawLen, const Cabinet& cab,
                            Bytes& out, std::string& err) {

    if (cab.flags & CAB_ENCRYPTED) { err = "cabinet is encrypted"; return false; }
    if (cab.flags & CAB_ZSTD) {
        if (!zstdAvailable()) {
            err = "Zstandard cabinet - libzstd not found";
            return false;
        }
        if (!zstdDecompress(raw, rawLen, cab.length, out)) {
            err = "zstd decode failed";
            return false;
        }
        return true;
    }
    if (cab.flags & CAB_DEFLATE) {
        if (inflateZlib(raw, rawLen, out)) return true;
        out.clear();
        if (inflateRaw(raw, rawLen, out)) return true;
        err = "deflate decode failed";
        return false;
    }
    if (cab.flags & CAB_LZHAM) {
        if (!lzhamAvailable()) {
            err = "LZHAM cabinet - the LZHAM decoder did not start";
            return false;
        }
        if (!lzhamDecompress(raw, rawLen, cab.length, out)) {
            err = "LZHAM decode failed";
            return false;
        }
        return true;
    }
    out.assign(raw, raw + rawLen);   // stored
    return true;
}

static bool decompressCabinet(const Bytes& file, size_t base, const Cabinet& cab,
                              Bytes& out, std::string& err) {
    if (base + cab.packedOffset + cab.packedLength > file.size()) {
        err = "cabinet range outside file";
        return false;
    }
    return decompressCabinetBlock(file.data() + base + cab.packedOffset,
                                  cab.packedLength, cab, out, err);
}


// ================================================================ LZHAM
//
// The game's own compressor. Proven, not guessed: libapp.so vendors LZHAM
// (core\vendor\lzham\lzham_symbol_codec.cpp is named in the binary), its asset
// format enumerates CompressedDeflate / CompressedLZHAM / CompressedLZMA /
// CompressedZstd, and the LZHAM cabinets inside the game's own packs
// (flags & 2) start with the same stream signature as every compressed .m3g:
//
//   cabinet : 16 xx xx xx  xx xx xx xx  40 00 00 00 ...
//   .m3g    : 16 00 00 00  vv vv vv vv  40 03 38 e9 ...
//
// LZHAM is loaded at runtime the same way Zstandard is, so the program runs
// without it and gains .m3g models the moment the DLL is beside it.
namespace {

// lzham_decompress_params, as lzham.h lays it out. The library checks
// m_struct_size, so the exact size matters - and it differs between the
// releases, which is why both are tried.
struct LzhamParams {
    uint32_t struct_size;
    uint32_t dict_size_log2;
    uint32_t table_update_rate;
    uint32_t decompress_flags;
    uint32_t num_seed_bytes;
    uint32_t pad;
    const void* seed_bytes;
    uint32_t table_max_update_interval;
    uint32_t table_update_interval_slow_rate;
};

typedef int (*lzham_decompress_memory_fn)(const LzhamParams*, uint8_t*, size_t*,
                                          const uint8_t*, size_t, uint32_t*);
typedef uint32_t (*lzham_get_version_fn)(void);

// set by lzhamSetPath: a decoder the user pointed at by hand
std::string g_lzhamPath;

struct LzhamLib {
    void* handle = nullptr;
    lzham_decompress_memory_fn decompressMemory = nullptr;
    uint32_t version = 0;
    std::string name;
    bool tried = false;

    void* sym(const char* s) {
#ifdef _WIN32
        return (void*)GetProcAddress((HMODULE)handle, s);
#else
        return dlsym(handle, s);
#endif
    }

    // Try one file. Anything that loads but has no decompress function is
    // not the library we want, so it is let go of again.
    bool tryOne(const char* path) {
#ifdef _WIN32
        handle = (void*)LoadLibraryA(path);
#else
        handle = dlopen(path, RTLD_NOW);
#endif
        if (!handle) return false;
        decompressMemory = (lzham_decompress_memory_fn)sym("lzham_decompress_memory");
        if (!decompressMemory)
            decompressMemory =
                (lzham_decompress_memory_fn)sym("lzham_lib_decompress_memory");
        lzham_get_version_fn ver = (lzham_get_version_fn)sym("lzham_get_version");
        if (!ver) ver = (lzham_get_version_fn)sym("lzham_lib_get_version");
        if (ver) version = ver();
        if (decompressMemory) { name = path; return true; }
        handle = nullptr;
        return false;
    }

    void load() {
        if (tried) return;
        tried = true;
        // a file the user pointed at wins over anything found lying around
        if (!g_lzhamPath.empty() && tryOne(g_lzhamPath.c_str())) return;
        // the built-in decoder serves when no DLL was picked by hand; one
        // lying beside the program is no longer looked for, since a wrong
        // build there is exactly what broke No Limits VR for people
        if (g_lzhamPath.empty()) return;
        static const char* candidates[] = {
#ifdef _WIN32
            "lzham_x64.dll", "lzham.dll", "lzhamdll_x64.dll", "lzhamdll.dll",
            "lzham_x86.dll", "dist\\lzham_x64.dll", "lzham\\lzham_x64.dll",
            "tools\\lzham_x64.dll",
#else
            "liblzham.so", "liblzham.so.1", "liblzham.dylib", "./liblzham.so",
#endif
            nullptr
        };
        for (int i = 0; candidates[i]; ++i)
            if (tryOne(candidates[i])) return;
    }
};

// the decoder compiled into the program (nfsnl_lzham.cpp), behind the same
// call shape the DLL has
int builtinDecompressMemory(const LzhamParams* p, uint8_t* dst, size_t* dstLen,
                            const uint8_t* src, size_t srcLen, uint32_t* adler) {
    return lzhamBuiltinDecompress(p->dict_size_log2, p->table_update_rate, p->decompress_flags,
                                  dst, dstLen, src, srcLen, adler);
}

LzhamLib& lzhamLib() {
    static LzhamLib lib;
    lib.load();
    if (!lib.decompressMemory) {
        lib.decompressMemory = builtinDecompressMemory;
        lib.name = "built in";
        lib.version = 0x1010;
    }
    return lib;
}

} // namespace

// Point the program at a decoder by hand, and try it at once. Used by the
// File menu, so a DLL built anywhere - by BUILD_LZHAM.bat, by the GitHub
// build, by someone else - can be adopted without moving files around.
bool lzhamSetPath(const std::string& path) {
    g_lzhamPath = path;
    LzhamLib& lib = lzhamLib();
    lib.tried = false;
    lib.handle = nullptr;
    lib.decompressMemory = nullptr;
    lib.version = 0;
    lib.name.clear();
    lib.load();
    return lib.decompressMemory != nullptr;
}

bool lzhamAvailable() { return lzhamLib().decompressMemory != nullptr; }

std::string lzhamBackend() {
    LzhamLib& l = lzhamLib();
    if (!l.decompressMemory) return "unavailable";
    if (!l.version) return l.name;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s (version %x)", l.name.c_str(), l.version);
    return buf;
}

// The eight bytes in front of the stream, read off the game's own data.
//
// Six LZHAM cabinets out of the packs inside the apk carry three distinct
// payloads between them - and two packs hold the same payload twice:
//
//   740fef9c...  16 6f cf 9c | d8 9b db bf | 40 00 00 00 ...  bf db 9b d8
//   05eec1c3...  16 16 36 cf | d8 9b db bf | 40 00 00 00 ...  bf db 9b d8
//
// Identical contents, identical length, and everything identical except bytes
// 1 to 3. So those three bytes are not part of the compressed data - they are
// a per-pack stamp. Byte 0 is always 0x16, which is 22: a legal LZHAM
// dictionary size log2, and the one the 4 MB presets use. Bytes 4 to 7 come
// back reversed as the last four bytes of every payload, cabinet and .m3g
// alike - that is an Adler-32 written at both ends, the tail one laid down
// backwards by LZHAM's bidirectional coder.
//
// Which puts the stream itself at offset 8, where every payload starts 0x40.
//
// None of that is certain enough to hard-code, so the reader tries offset 8
// first and then 0 and 4, and sweeps the settings a stream does not carry -
// dictionary size, table update rate, the unbuffered flag, and the struct size
// the library checks. Wrong settings do not give wrong data: the decoder
// refuses the stream or stops short, and the size check catches it. A decode
// whose Adler-32 matches the one in the header is right beyond doubt and is
// taken at once.
bool lzhamDecompress(const uint8_t* src, size_t srcLen, size_t expectedSize, Bytes& out) {
    LzhamLib& lib = lzhamLib();
    if (!lib.decompressMemory || !expectedSize || !srcLen) return false;

    // where the stream starts
    std::vector<size_t> starts;
    // a dictionary-size byte first (0x16 in No Limits' packs; No Limits VR's
    // FMOBB files use other sizes too) means the 8-byte framing
    bool framed = srcLen > 8 && src[0] >= 15 && src[0] <= 26;
    if (framed) starts.push_back(8);
    starts.push_back(0);
    if (srcLen > 4) starts.push_back(4);
    if (srcLen > 8 && !framed) starts.push_back(8);

    // the Adler-32 the header claims, if this is the framing above
    uint32_t headerAdler = 0;
    if (srcLen > 8)
        headerAdler = ((uint32_t)src[4] << 24) | ((uint32_t)src[5] << 16) |
                      ((uint32_t)src[6] << 8) | (uint32_t)src[7];
    // it is stored little-endian (checked against the library's own sum)
    uint32_t headerAdlerLE = 0;
    if (srcLen > 8)
        headerAdlerLE = (uint32_t)src[4] | ((uint32_t)src[5] << 8) |
                        ((uint32_t)src[6] << 16) | ((uint32_t)src[7] << 24);

    // dictionary sizes: the header-looking first byte, then 22, then the rest
    std::vector<uint32_t> dicts;
    auto addDict = [&](uint32_t v) {
        if (v < 15 || v > 29) return;
        for (uint32_t d : dicts) if (d == v) return;
        dicts.push_back(v);
    };
    addDict(src[0]);
    addDict(22);
    for (int log2 = 26; log2 >= 15; --log2) addDict((uint32_t)log2);

    // table update rate: 0 asks the library for its default, 8 is that default
    // spelled out, then every legal rate
    std::vector<uint32_t> rates;
    auto addRate = [&](uint32_t v) {
        for (uint32_t r : rates) if (r == v) return;
        rates.push_back(v);
    };
    addRate(0);
    addRate(8);
    addRate(20);
    for (uint32_t r = 1; r <= 20; ++r) addRate(r);

    // LZHAM_DECOMP_FLAG_OUTPUT_UNBUFFERED is what the whole-buffer call wants;
    // bit 1 asks it to compute the Adler-32 as it goes
    const uint32_t flagSets[] = { 3, 1, 2, 0 };
    const uint32_t structSizes[] = { 40, 32, 48, 24 };

    Bytes buf(expectedSize, 0);
    bool haveFallback = false;
    Bytes fallback;

    for (size_t start : starts) {
        if (start >= srcLen) continue;
        const uint8_t* s = src + start;
        size_t n = srcLen - start;
        for (uint32_t structSize : structSizes) {
            for (uint32_t flags : flagSets) {
                for (uint32_t rate : rates) {
                    for (uint32_t log2 : dicts) {
                        LzhamParams p{};
                        p.struct_size = structSize;
                        p.dict_size_log2 = log2;
                        p.table_update_rate = rate;
                        p.decompress_flags = flags;
                        size_t dstLen = buf.size();
                        uint32_t adler = 0;
                        int status = lib.decompressMemory(&p, buf.data(), &dstLen,
                                                          s, n, &adler);
                        // LZHAM_DECOMP_STATUS_SUCCESS is 3 (0-2 are "not
                        // finished" states, 4 and up are failures). Up to 0.7.9
                        // this took 0 for success and so threw every good
                        // decode away - no .m3g ever unpacked.
                        if (status != 3 || dstLen != expectedSize) continue;
                        if (adler && headerAdler && (adler == headerAdler || adler == headerAdlerLE)) {
                            out.swap(buf);
                            return true;
                        }
                        if (!haveFallback) { fallback = buf; haveFallback = true; }
                    }
                }
            }
        }
    }
    if (haveFallback) { out.swap(fallback); return true; }
    return false;
}

void extractArchive(const std::string& path, ExtractResult& result) {
    Bytes file;
    if (!readFile(path, file)) {
        result.warnings.push_back(baseName(path) + ": cannot read file");
        return;
    }
    std::string label = stripExtension(baseName(path));

    if (file.size() >= 20 && memcmp(file.data(), "PACK", 4) == 0) {
        if (memcmp(file.data() + 12, "ZBDS", 4) != 0) {
            result.warnings.push_back(label + ": PACK header missing ZBDS");
            return;
        }
        Bytes meta;
        size_t consumed = 0;
        if (!inflateGzip(file.data() + 20, file.size() - 20, meta, &consumed)) {
            result.warnings.push_back(label + ": manifest gzip decode failed");
            return;
        }
        size_t bulkStart = 20 + consumed;
        size_t dataBase = (bulkStart + 15) & ~(size_t)15;
        if (meta.size() < 4 || memcmp(meta.data(), "SBIN", 4) != 0) {
            result.warnings.push_back(label + ": manifest is not SBIN");
            return;
        }

        PackManifest man = parsePackManifest(meta, file.size(), dataBase);
        if (!man.valid) {
            result.warnings.push_back(label + ": no usable manifest records");
            return;
        }
        auto paths = man.resolvePaths();
        if (paths.empty()) {
            result.warnings.push_back(label + ": could not resolve file paths");
            return;
        }

        // group files by cabinet so each is decompressed once
        std::map<uint32_t, std::vector<uint32_t>> byCab;
        for (auto& kv : paths) byCab[man.files[kv.first].cabinetIndex].push_back(kv.first);

        std::string prefix = man.variant.empty() ? "" : man.variant + "/";

        for (auto& kv : byCab) {
            uint32_t ci = kv.first;
            if (ci >= man.cabinets.size()) {
                result.warnings.push_back(
                    label + ": cabinet " + std::to_string(ci) +
                    " not present (external/DLC cabinet)");
                result.blockedByCodec += (int)kv.second.size();
                continue;
            }
            Bytes blob;
            std::string err;
            const Cabinet& cab = man.cabinets[ci];
            if (cab.flags & CAB_EXTERNAL) {
                // <folder of the pack>/<pack name>/<n>.cab, as Library::read
                size_t slash = path.find_last_of("/\\");
                std::string dir = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
                std::string cabPath = joinPath(dir + label, std::to_string(ci) + ".cab");
                Bytes packed;
                if (cab.length > (512u << 20) || !readFile(cabPath, packed) || packed.empty()) {
                    result.warnings.push_back(label + ": cabinet " + std::to_string(ci) +
                                              " is external and not on disk (" + cabPath + ")");
                    result.blockedByCodec += (int)kv.second.size();
                    continue;
                }
                if ((cab.flags & CAB_STORED) || !(cab.flags & CAB_ZSTD)) blob.swap(packed);
                else if (!zstdAvailable() ||
                         !zstdDecompress(packed.data(), packed.size(), cab.length, blob)) {
                    result.warnings.push_back(label + ": cabinet " + std::to_string(ci) +
                                              " - zstd decode failed (external)");
                    result.blockedByCodec += (int)kv.second.size();
                    continue;
                }
            } else if (!decompressCabinet(file, dataBase, cab, blob, err)) {
                result.warnings.push_back(label + ": cabinet " + std::to_string(ci) +
                                          " - " + err);
                result.blockedByCodec += (int)kv.second.size();
                continue;
            }
            for (uint32_t fi : kv.second) {
                const PackFile& pf = man.files[fi];
                if ((size_t)pf.offset + pf.length > blob.size()) {
                    result.warnings.push_back(label + ": " + paths[fi] +
                                              " outside cabinet range");
                    continue;
                }
                Asset a;
                a.path = prefix + paths[fi];
                a.data.assign(blob.begin() + pf.offset,
                              blob.begin() + pf.offset + pf.length);
                a.kind = classifyByName(a.path);
                result.assets.push_back(std::move(a));
            }
        }
        return;
    }

    // bare SBIN (.cab) - no names available inside the format
    if (file.size() >= 4 && memcmp(file.data(), "SBIN", 4) == 0) {
        Asset a;
        a.path = "_unnamed/" + label + ".sbin";
        a.data = file;
        a.kind = "other";
        result.assets.push_back(std::move(a));
        return;
    }

    result.warnings.push_back(label + ": unrecognised container");
}

} // namespace nfsnl
