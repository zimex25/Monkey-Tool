// nfsnl_image.cpp - .sba reading, PNG decode, and PNG/BMP/TGA/DDS/JPEG encode
#include "nfsnl.h"
#include <set>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace nfsnl {

// ================================================================ checksums

// built once; a function-local static is thread-safe (textures are decoded
// on several threads)
struct CrcTable { uint32_t v[256]; CrcTable() {
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        v[n] = c;
    }
} };
static const uint32_t* crcTableGet() { static const CrcTable t; return t.v; }
static uint32_t crc32buf(const uint8_t* buf, size_t len, uint32_t crc = 0) {
    const uint32_t* crcTable = crcTableGet();
    uint32_t c = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) c = crcTable[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void put32be(Bytes& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));  v.push_back((uint8_t)x);
}

// ================================================================ PNG decode

// decodePng lives in nfsnl_decode.cpp, beside the JPEG decoder.

// ================================================================ PNG encode
// Filtered scanlines, DEFLATE-compressed (deflateRaw in nfsnl_core.cpp).

Bytes encodePng(const Image& img) {
    Bytes out;
    if (!img.ok()) return out;
    int ch = img.channels;
    int colorType = ch == 1 ? 0 : (ch == 3 ? 2 : 6);

    static const uint8_t sig[8] = {0x89,'P','N','G','\r','\n',0x1a,'\n'};
    out.insert(out.end(), sig, sig + 8);

    auto chunk = [&](const char* type, const Bytes& body) {
        put32be(out, (uint32_t)body.size());
        size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), body.begin(), body.end());
        uint32_t c = crc32buf(out.data() + start, out.size() - start);
        put32be(out, c);
    };

    Bytes ihdr;
    put32be(ihdr, (uint32_t)img.width);
    put32be(ihdr, (uint32_t)img.height);
    ihdr.push_back(8);
    ihdr.push_back((uint8_t)colorType);
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk("IHDR", ihdr);

    // Each scanline gets the PNG filter that leaves the smallest residuals
    // (the usual minimum-sum-of-absolute-differences choice), then the lot
    // is DEFLATE-compressed.
    size_t stride = (size_t)img.width * ch;
    Bytes raw;
    raw.reserve((stride + 1) * img.height);
    std::vector<uint8_t> cand[5];
    for (auto& c : cand) c.resize(stride);
    const uint8_t* prevRow = nullptr;
    std::vector<uint8_t> zero(stride, 0);
    for (int y = 0; y < img.height; ++y) {
        const uint8_t* row = img.pixels.data() + stride * y;
        const uint8_t* up = prevRow ? prevRow : zero.data();
        for (size_t x = 0; x < stride; ++x) {
            int a = x >= (size_t)ch ? row[x - ch] : 0;
            int b = up[x];
            int c = x >= (size_t)ch ? up[x - ch] : 0;
            int pa = std::abs(b - c), pb = std::abs(a - c), pc = std::abs(a + b - 2 * c);
            int paeth = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
            cand[0][x] = row[x];
            cand[1][x] = (uint8_t)(row[x] - a);
            cand[2][x] = (uint8_t)(row[x] - b);
            cand[3][x] = (uint8_t)(row[x] - ((a + b) >> 1));
            cand[4][x] = (uint8_t)(row[x] - paeth);
        }
        int best = 0;
        uint64_t bestSum = ~0ull;
        for (int f = 0; f < 5; ++f) {
            uint64_t sum = 0;
            for (size_t x = 0; x < stride; ++x) sum += (uint64_t)std::abs((int)(int8_t)cand[f][x]);
            if (sum < bestSum) { bestSum = sum; best = f; }
        }
        raw.push_back((uint8_t)best);
        raw.insert(raw.end(), cand[best].begin(), cand[best].end());
        prevRow = row;
    }
    Bytes z = zlibCompress(raw.data(), raw.size());
    chunk("IDAT", z);
    chunk("IEND", Bytes());
    return out;
}

// ================================================================ BMP / TGA

Bytes encodeBmp(const Image& img) {
    Bytes out;
    if (!img.ok()) return out;
    int ch = img.channels >= 3 ? 3 : 1;     // 24-bit BGR (or expanded grey)
    int rowBytes = img.width * 3;
    int pad = (4 - (rowBytes % 4)) % 4;
    uint32_t pixelBytes = (uint32_t)((rowBytes + pad) * img.height);
    uint32_t fileSize = 14 + 40 + pixelBytes;

    auto put16 = [&](uint16_t v) { out.push_back((uint8_t)v); out.push_back((uint8_t)(v >> 8)); };
    auto put32 = [&](uint32_t v) {
        out.push_back((uint8_t)v); out.push_back((uint8_t)(v >> 8));
        out.push_back((uint8_t)(v >> 16)); out.push_back((uint8_t)(v >> 24));
    };
    out.push_back('B'); out.push_back('M');
    put32(fileSize); put16(0); put16(0); put32(14 + 40);
    put32(40); put32((uint32_t)img.width); put32((uint32_t)img.height);
    put16(1); put16(24); put32(0); put32(pixelBytes);
    put32(2835); put32(2835); put32(0); put32(0);

    // BMP rows are bottom-up
    for (int y = img.height - 1; y >= 0; --y) {
        const uint8_t* row = img.pixels.data() + (size_t)y * img.width * img.channels;
        for (int x = 0; x < img.width; ++x) {
            const uint8_t* px = row + (size_t)x * img.channels;
            uint8_t r, g, b;
            if (img.channels == 1) { r = g = b = px[0]; }
            else { r = px[0]; g = px[1]; b = px[2]; }
            out.push_back(b); out.push_back(g); out.push_back(r);
        }
        for (int i = 0; i < pad; ++i) out.push_back(0);
    }
    (void)ch;
    return out;
}

Bytes encodeTga(const Image& img) {
    Bytes out;
    if (!img.ok()) return out;
    bool alpha = img.channels == 4;
    uint8_t header[18] = {0};
    header[2] = 2;                       // uncompressed true-colour
    header[12] = (uint8_t)(img.width & 0xFF);
    header[13] = (uint8_t)((img.width >> 8) & 0xFF);
    header[14] = (uint8_t)(img.height & 0xFF);
    header[15] = (uint8_t)((img.height >> 8) & 0xFF);
    header[16] = alpha ? 32 : 24;
    header[17] = alpha ? 0x28 : 0x20;    // top-left origin
    out.insert(out.end(), header, header + 18);
    for (int y = 0; y < img.height; ++y) {
        const uint8_t* row = img.pixels.data() + (size_t)y * img.width * img.channels;
        for (int x = 0; x < img.width; ++x) {
            const uint8_t* px = row + (size_t)x * img.channels;
            uint8_t r, g, b, a = 255;
            if (img.channels == 1) { r = g = b = px[0]; }
            else { r = px[0]; g = px[1]; b = px[2]; if (alpha) a = px[3]; }
            out.push_back(b); out.push_back(g); out.push_back(r);
            if (alpha) out.push_back(a);
        }
    }
    return out;
}

Bytes makeDds(int w, int h, const char fourcc[4], const uint8_t* payload, size_t len) {
    Bytes out(128, 0);
    memcpy(out.data(), "DDS ", 4);
    auto put = [&](size_t off, uint32_t v) { memcpy(out.data() + off, &v, 4); };
    put(4, 124);
    put(8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000);
    put(12, (uint32_t)h);
    put(16, (uint32_t)w);
    int block = memcmp(fourcc, "DXT1", 4) == 0 ? 8 : 16;
    put(20, (uint32_t)(std::max(1, (w + 3) / 4) * std::max(1, (h + 3) / 4) * block));
    put(28, 1);
    put(76, 32);
    put(80, 0x4);
    memcpy(out.data() + 84, fourcc, 4);
    put(108, 0x1000);
    out.insert(out.end(), payload, payload + len);
    return out;
}

// ================================================================ JPEG encode
// Baseline sequential, 4:4:4, standard Annex-K tables.

namespace {

const int kZigZag[64] = {
     0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
};
const uint8_t kLumaQ[64] = {
    16,11,10,16,24,40,51,61, 12,12,14,19,26,58,60,55,
    14,13,16,24,40,57,69,56, 14,17,22,29,51,87,80,62,
    18,22,37,56,68,109,103,77, 24,35,55,64,81,104,113,92,
    49,64,78,87,103,121,120,101, 72,92,95,98,112,100,103,99
};
const uint8_t kChromaQ[64] = {
    17,18,24,47,99,99,99,99, 18,21,26,66,99,99,99,99,
    24,26,56,99,99,99,99,99, 47,66,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99
};
const uint8_t kDcLumBits[17]   = {0,0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
const uint8_t kDcLumVals[12]   = {0,1,2,3,4,5,6,7,8,9,10,11};
const uint8_t kDcChrBits[17]   = {0,0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
const uint8_t kDcChrVals[12]   = {0,1,2,3,4,5,6,7,8,9,10,11};
const uint8_t kAcLumBits[17]   = {0,0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d};
const uint8_t kAcLumVals[162]  = {
 0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
 0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
 0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
 0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
 0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
 0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
 0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
 0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
 0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
 0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
 0xf9,0xfa
};
const uint8_t kAcChrBits[17]   = {0,0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77};
const uint8_t kAcChrVals[162]  = {
 0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
 0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,
 0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
 0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,
 0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,
 0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
 0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,
 0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
 0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
 0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
 0xf9,0xfa
};

struct HuffEnc {
    uint16_t code[256] = {0};
    uint8_t  size[256] = {0};
    void build(const uint8_t* bits, const uint8_t* vals) {
        int k = 0, codeVal = 0;
        for (int l = 1; l <= 16; ++l) {
            for (int i = 0; i < bits[l]; ++i) {
                code[vals[k]] = (uint16_t)codeVal;
                size[vals[k]] = (uint8_t)l;
                codeVal++; k++;
            }
            codeVal <<= 1;
        }
    }
};

struct JpegWriter {
    Bytes& out;
    uint32_t bitBuf = 0;
    int bitCnt = 0;
    explicit JpegWriter(Bytes& o) : out(o) {}

    void byteOut(uint8_t b) {
        out.push_back(b);
        if (b == 0xFF) out.push_back(0x00);   // byte stuffing
    }
    void writeBits(int code, int size) {
        for (int i = size - 1; i >= 0; --i) {
            bitBuf = (bitBuf << 1) | ((code >> i) & 1);
            if (++bitCnt == 8) { byteOut((uint8_t)bitBuf); bitBuf = 0; bitCnt = 0; }
        }
    }
    void flushBits() {
        while (bitCnt) {
            bitBuf = (bitBuf << 1) | 1;
            if (++bitCnt == 8) { byteOut((uint8_t)bitBuf); bitBuf = 0; bitCnt = 0; }
        }
    }
};

void fdct(float* b) {
    // separable float DCT-II, straightforward form
    float tmp[64];
    for (int u = 0; u < 8; ++u)
        for (int x = 0; x < 8; ++x)
            tmp[u * 8 + x] = cosf((2.0f * x + 1.0f) * u * 3.14159265358979f / 16.0f) *
                             (u == 0 ? 0.353553390593f : 0.5f);
    float t[64];
    for (int y = 0; y < 8; ++y)
        for (int u = 0; u < 8; ++u) {
            float s = 0;
            for (int x = 0; x < 8; ++x) s += b[y * 8 + x] * tmp[u * 8 + x];
            t[y * 8 + u] = s;
        }
    for (int u = 0; u < 8; ++u)
        for (int v = 0; v < 8; ++v) {
            float s = 0;
            for (int y = 0; y < 8; ++y) s += t[y * 8 + v] * tmp[u * 8 + y];
            b[u * 8 + v] = s;
        }
}

int encodeBlock(JpegWriter& w, float* block, const uint8_t* q,
                const HuffEnc& dcH, const HuffEnc& acH, int prevDC) {
    fdct(block);
    int coef[64];
    for (int i = 0; i < 64; ++i) {
        float v = block[kZigZag[i]] / (float)q[kZigZag[i]];
        coef[i] = (int)lrintf(v);
    }
    auto categoryOf = [](int v) {
        int a = v < 0 ? -v : v, n = 0;
        while (a) { a >>= 1; n++; }
        return n;
    };
    auto bitsOf = [](int v, int n) { return v < 0 ? (v - 1) & ((1 << n) - 1) : v; };

    int diff = coef[0] - prevDC;
    int n = categoryOf(diff);
    w.writeBits(dcH.code[n], dcH.size[n]);
    if (n) w.writeBits(bitsOf(diff, n), n);

    int run = 0;
    for (int i = 1; i < 64; ++i) {
        if (coef[i] == 0) { run++; continue; }
        while (run > 15) { w.writeBits(acH.code[0xF0], acH.size[0xF0]); run -= 16; }
        int cat = categoryOf(coef[i]);
        int sym = (run << 4) | cat;
        w.writeBits(acH.code[sym], acH.size[sym]);
        w.writeBits(bitsOf(coef[i], cat), cat);
        run = 0;
    }
    if (run) w.writeBits(acH.code[0x00], acH.size[0x00]);
    return coef[0];
}

} // namespace

Bytes encodeJpeg(const Image& img, int quality) {
    Bytes out;
    if (!img.ok()) return out;
    quality = std::max(1, std::min(100, quality));
    int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;

    uint8_t lq[64], cq[64];
    for (int i = 0; i < 64; ++i) {
        int v = (kLumaQ[i] * scale + 50) / 100;
        lq[i] = (uint8_t)std::max(1, std::min(255, v));
        v = (kChromaQ[i] * scale + 50) / 100;
        cq[i] = (uint8_t)std::max(1, std::min(255, v));
    }

    auto put16 = [&](uint16_t v) { out.push_back((uint8_t)(v >> 8)); out.push_back((uint8_t)v); };
    auto marker = [&](uint8_t m) { out.push_back(0xFF); out.push_back(m); };

    marker(0xD8);                                  // SOI
    marker(0xE0); put16(16);                       // APP0/JFIF
    out.insert(out.end(), {'J','F','I','F',0});
    out.insert(out.end(), {1,1,0, 0,1, 0,1, 0,0});

    marker(0xDB); put16(67); out.push_back(0);     // DQT luma
    for (int i = 0; i < 64; ++i) out.push_back(lq[kZigZag[i]]);
    marker(0xDB); put16(67); out.push_back(1);     // DQT chroma
    for (int i = 0; i < 64; ++i) out.push_back(cq[kZigZag[i]]);

    marker(0xC0); put16(17); out.push_back(8);     // SOF0
    put16((uint16_t)img.height); put16((uint16_t)img.width);
    out.push_back(3);
    out.insert(out.end(), {1, 0x11, 0});           // Y  (no subsampling)
    out.insert(out.end(), {2, 0x11, 1});
    out.insert(out.end(), {3, 0x11, 1});

    auto dht = [&](uint8_t id, const uint8_t* bits, const uint8_t* vals, int nvals) {
        marker(0xC4); put16((uint16_t)(3 + 16 + nvals));
        out.push_back(id);
        for (int i = 1; i <= 16; ++i) out.push_back(bits[i]);
        out.insert(out.end(), vals, vals + nvals);
    };
    dht(0x00, kDcLumBits, kDcLumVals, 12);
    dht(0x10, kAcLumBits, kAcLumVals, 162);
    dht(0x01, kDcChrBits, kDcChrVals, 12);
    dht(0x11, kAcChrBits, kAcChrVals, 162);

    marker(0xDA); put16(12); out.push_back(3);     // SOS
    out.insert(out.end(), {1, 0x00, 2, 0x11, 3, 0x11});
    out.insert(out.end(), {0, 63, 0});

    HuffEnc dcL, acL, dcC, acC;
    dcL.build(kDcLumBits, kDcLumVals);
    acL.build(kAcLumBits, kAcLumVals);
    dcC.build(kDcChrBits, kDcChrVals);
    acC.build(kAcChrBits, kAcChrVals);

    JpegWriter w(out);
    int pdcY = 0, pdcCb = 0, pdcCr = 0;
    int ch = img.channels;

    for (int by = 0; by < img.height; by += 8) {
        for (int bx = 0; bx < img.width; bx += 8) {
            float Y[64], Cb[64], Cr[64];
            for (int y = 0; y < 8; ++y) {
                int sy = std::min(by + y, img.height - 1);
                for (int x = 0; x < 8; ++x) {
                    int sx = std::min(bx + x, img.width - 1);
                    const uint8_t* px = img.pixels.data() + ((size_t)sy * img.width + sx) * ch;
                    float r, g, b;
                    if (ch == 1) { r = g = b = px[0]; }
                    else { r = px[0]; g = px[1]; b = px[2]; }
                    int i = y * 8 + x;
                    Y[i]  =  0.299f * r + 0.587f * g + 0.114f * b - 128.0f;
                    Cb[i] = -0.168736f * r - 0.331264f * g + 0.5f * b;
                    Cr[i] =  0.5f * r - 0.418688f * g - 0.081312f * b;
                }
            }
            pdcY  = encodeBlock(w, Y,  lq, dcL, acL, pdcY);
            pdcCb = encodeBlock(w, Cb, cq, dcC, acC, pdcCb);
            pdcCr = encodeBlock(w, Cr, cq, dcC, acC, pdcCr);
        }
    }
    w.flushBits();
    marker(0xD9);                                  // EOI
    return out;
}

// ================================================================ .sba

static const uint8_t kBargSentinel[16] = {
    0x00,0x00,0x00,0x00, 0xff,0xff,0xff,0xff,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00
};

// Payload slots inside BARG are aligned, so an encoded image is usually
// followed by a few bytes of padding (16 is the common case). Trimming to the
// real end of the stream makes a written .png / .jpg byte-exact instead of
// carrying junk past IEND / EOI.
static size_t imagePayloadEnd(const uint8_t* b, size_t n, const std::string& fmt) {
    if (fmt == "png") {
        size_t i = 8;
        while (i + 12 <= n) {
            uint32_t len = ((uint32_t)b[i] << 24) | ((uint32_t)b[i + 1] << 16) |
                           ((uint32_t)b[i + 2] << 8) | (uint32_t)b[i + 3];
            if (len > n || i + 12 + (size_t)len > n) break;
            bool iend = !memcmp(b + i + 4, "IEND", 4);
            i += 12 + (size_t)len;
            if (iend) return i;
        }
        return n;
    }
    if (fmt == "jpg") {
        for (size_t i = n; i >= 2; --i)
            if (b[i - 2] == 0xFF && b[i - 1] == 0xD9) return i;
        return n;
    }
    return n;
}

static std::string identifyImage(const uint8_t* b, size_t n) {
    if (n >= 8 && !memcmp(b, "\x89PNG\r\n\x1a\n", 8)) return "png";
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return "jpg";
    if (n >= 4 && (!memcmp(b, "PVR\x03", 4) || !memcmp(b, "PVR!", 4))) return "pvr";
    if (n >= 4 && !memcmp(b, "\xabKTX", 4)) return "ktx";
    if (n >= 4 && !memcmp(b, "\x13\xab\xa1\x5c", 4)) return "astc";
    if (n >= 4 && !memcmp(b, "DDS ", 4)) return "dds";
    return "raw";
}

// Image records in DATA look like (format, kind, dataIndex, width, height),
// where kind is 13 for an encoded payload and 0 for raw pixels.
//
// Some .sba files are UI atlases whose DATA also holds Box / Rectangle
// records, and those produce plausible-looking but wrong (w,h) pairs. Each
// candidate is therefore checked against the real blob size: it is only
// accepted when width*height*bytesPerPixel matches the payload for some
// sensible pixel size.
// How many bytes one image of this codec occupies, or 0 when the payload is a
// self-describing stream (PNG / JPEG) or a codec with no fixed rate.
static size_t codecSize(int codec, int w, int h) {
    size_t px = (size_t)w * h;
    if (codec > SBA_ASTC_BASE) {
        int bw = (codec - SBA_ASTC_BASE) / 100, bh = (codec - SBA_ASTC_BASE) % 100;
        if (bw <= 0 || bh <= 0) return 0;
        return (size_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * 16;
    }
    switch (codec) {
        case SBA_RGB:  return px * 3;
        case SBA_RGBA: return px * 4;
        case SBA_PVRTC_2BPP_RGB:
        case SBA_PVRTC_2BPP_RGBA: return std::max<size_t>(32, px / 4);
        case SBA_PVRTC_4BPP_RGBA:
        case SBA_PVRTC_4BPP_RGB:  return std::max<size_t>(32, px / 2);
        case SBA_DXT1:
        case SBA_ETC_RGB:
        case SBA_ETC2_RGB:        return std::max<size_t>(8, px / 2);
        case SBA_ETC2_RGBA:       return std::max<size_t>(16, px);
        case SBA_RGB565:          return px * 2;
        case SBA_DXT3:
        case SBA_DXT5:            return std::max<size_t>(16, px);
        case SBA_ATC_RGB:         return std::max<size_t>(8, px / 2);
        case SBA_ATC_RGBA_EXPLICIT:
        case SBA_ATC_RGBA_INTERPOLATED: return std::max<size_t>(16, px);
        default: return 0;
    }
}

const char* sbaCodecName(int codec) {
    switch (codec) {
        case SBA_DEFAULT: return "default";
        case SBA_RGB: return "RGB";
        case SBA_RGBA: return "RGBA";
        case SBA_PVRTC_2BPP_RGB: return "PVRTC_2BPP_RGB";
        case SBA_PVRTC_2BPP_RGBA: return "PVRTC_2BPP_RGBA";
        case SBA_PVRTC_4BPP_RGBA: return "PVRTC_4BPP_RGBA";
        case SBA_PVRTC_4BPP_RGB: return "PVRTC_4BPP_RGB";
        case SBA_DXT1: return "DXT1";
        case SBA_DXT3: return "DXT3";
        case SBA_DXT5: return "DXT5";
        case SBA_ATC_RGB: return "ATC_RGB";
        case SBA_ATC_RGBA_EXPLICIT: return "ATC_RGBA_Explicit";
        case SBA_ATC_RGBA_INTERPOLATED: return "ATC_RGBA_Interpolated";
        case SBA_ETC_RGB: return "ETC_RGB";
        case SBA_ETC2_RGB: return "ETC2_RGB";
        case SBA_ETC2_RGBA: return "ETC2_RGBA";
        case SBA_RGB565: return "RGB565";
        case SBA_ASTC_BASE + 404: return "ASTC_LDR_4x4";
        case SBA_ASTC_BASE + 505: return "ASTC_LDR_5x5";
        case SBA_ASTC_BASE + 606: return "ASTC_LDR_6x6";
        case SBA_ASTC_BASE + 808: return "ASTC_LDR_8x8";
        case SBA_PNG: return "PNG";
        case SBA_JPEG: return "JPEG";
        default: return "unknown";
    }
}

// The Image records in DATA. Each is five words:
//
//     0, ImageFormatType, dataIndex, width, height
//
// and one record exists per mip level, largest first. The format word is an
// index into the enum the file's own string table spells out (PVRTC_4BPP_RGBA,
// ETC_RGB, DXT1, PNG ...), so the codec never has to be guessed - which is
// what earlier versions did, and why PVRTC packs came out looking like mush.
struct SbaRecord { int codec, blob, w, h; int alt = SBA_UNKNOWN; };

// Name -> codec. The names are the file's own; only the ones that can be
// decoded (or identified) are listed.
static int codecFromName(const std::string& n) {
    if (n == "default") return SBA_DEFAULT;
    if (n == "RGB") return SBA_RGB;
    if (n == "RGBA") return SBA_RGBA;
    if (n == "PVRTC_2BPP_RGB") return SBA_PVRTC_2BPP_RGB;
    if (n == "PVRTC_2BPP_RGBA") return SBA_PVRTC_2BPP_RGBA;
    if (n == "PVRTC_4BPP_RGBA") return SBA_PVRTC_4BPP_RGBA;
    if (n == "PVRTC_4BPP_RGB") return SBA_PVRTC_4BPP_RGB;
    if (n == "DXT1") return SBA_DXT1;
    if (n == "DXT3") return SBA_DXT3;
    if (n == "DXT5") return SBA_DXT5;
    if (n == "ATC_RGB") return SBA_ATC_RGB;
    if (n == "ATC_RGBA_Explicit") return SBA_ATC_RGBA_EXPLICIT;
    if (n == "ATC_RGBA_Interpolated") return SBA_ATC_RGBA_INTERPOLATED;
    if (n == "ETC_RGB") return SBA_ETC_RGB;
    if (n == "ETC2_RGB") return SBA_ETC2_RGB;
    if (n == "ETC2_RGBA") return SBA_ETC2_RGBA;
    if (n == "PNG") return SBA_PNG;
    if (n == "JPEG") return SBA_JPEG;
    if (n == "RGB565") return SBA_RGB565;
    if (n.compare(0, 9, "ASTC_LDR_") == 0) {
        int bw = 0, bh = 0;
        if (sscanf(n.c_str() + 9, "%dx%d", &bw, &bh) == 2 && bw >= 4 && bw <= 12 && bh >= 4 && bh <= 12)
            return SBA_ASTC_BASE + bw * 100 + bh;
    }
    return SBA_UNKNOWN;
}

// Every .sba spells out its own ImageFormatType. The lists differ between
// builds - the newer one adds ASTC, ETC2 and EAC entries and even swaps RGB
// and RGBA - so the enum cannot be hardcoded, and what a stored value means
// depends on the file holding it.
static std::vector<std::string> sbaStringTable(const std::vector<SbinChunk>& chunks) {
    std::vector<std::string> names;
    const SbinChunk* c = findChunk(chunks, "CDAT");
    if (!c) return names;
    size_t i = 0;
    while (i < c->size) {
        size_t j = i;
        while (j < c->size && c->data[j]) ++j;
        if (j > i) names.emplace_back((const char*)c->data + i, j - i);
        i = j + 1;
    }
    return names;
}

static std::vector<SbaRecord> sbaRecords(const std::vector<SbinChunk>& chunks,
                                         const std::map<int, size_t>& blobSizes) {
    std::vector<SbaRecord> out;
    const SbinChunk* d = findChunk(chunks, "DATA");
    if (!d) return out;

    // Where the value list starts is not stated anywhere, so both plausible
    // anchors are tried and the payload's byte rate decides between them:
    // a codec whose size does not match the blob is not the codec.
    std::vector<std::string> names = sbaStringTable(chunks);
    size_t anchor = 0;
    bool haveAnchor = false;
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == "default") { anchor = i; haveAnchor = true; break; }

    int lastAlt = SBA_UNKNOWN;
    auto resolve = [&](uint32_t value, int w, int h, size_t have) -> int {
        lastAlt = SBA_UNKNOWN;
        if (!haveAnchor) return (int)value;      // no table: trust the number
        int best = SBA_UNKNOWN, first = SBA_UNKNOWN;
        for (int shift = 0; shift <= 1; ++shift) {
            size_t idx = anchor + shift + value;
            if (idx >= names.size()) continue;
            int codec = codecFromName(names[idx]);
            if (codec == SBA_UNKNOWN) continue;
            size_t need = codecSize(codec, w, h);
            if (!need) {                          // self-describing or unrated
                if (best == SBA_UNKNOWN) best = codec;
                continue;
            }
            if (have >= need && have - need <= 64) {
                if (first == SBA_UNKNOWN) first = codec;
                else lastAlt = codec;             // both fit: the content decides
            }
        }
        return first != SBA_UNKNOWN ? first : best;
    };
    size_t words = d->size / 4;
    auto W = [&](size_t i) { uint32_t v; memcpy(&v, d->data + i * 4, 4); return v; };
    for (size_t i = 0; i + 5 <= words; ++i) {
        if (W(i) != 0) continue;
        uint32_t value = W(i + 1), idx = W(i + 2), w = W(i + 3), h = W(i + 4);
        if (value > 64) continue;
        if (w == 0 || h == 0 || w > 8192 || h > 8192) continue;
        auto it = blobSizes.find((int)idx);
        if (it == blobSizes.end()) continue;
        size_t have = it->second;

        int codec = resolve(value, (int)w, (int)h, have);
        if (codec == SBA_UNKNOWN) continue;
        size_t need = codecSize(codec, (int)w, (int)h);
        if (need) {
            // a payload may carry a little alignment padding, never less data
            if (have < need || have - need > 64) continue;
        } else if (codec == SBA_DEFAULT) {
            // "default" names no codec, so the byte count has to place it:
            // some whole number of bytes per pixel, or a 4bpp block payload
            size_t px = (size_t)w * h;
            bool fits = false;
            for (size_t bpp = 1; bpp <= 4 && !fits; ++bpp)
                if (have >= px * bpp && have - px * bpp <= 16) fits = true;
            if (!fits && have >= px / 2 && have - px / 2 <= 16) fits = true;
            if (!fits) continue;
        }
        SbaRecord rec{codec, (int)idx, (int)w, (int)h};
        rec.alt = lastAlt;
        out.push_back(rec);
    }
    return out;
}

static bool isSvgBox(const TexturePackBox& b) {
    return b.name.size() > 4 && (b.name.compare(b.name.size() - 4, 4, ".svg") == 0 ||
                                 b.name.compare(b.name.size() - 4, 4, ".SVG") == 0);
}

std::vector<TexturePackBox> readTexturePack(const uint8_t* data, size_t len) {
    std::vector<TexturePackBox> out;
    // only a file whose string table names the structure is parsed
    {
        auto chunks = sbinChunks(data, len);
        const SbinChunk* cd = findChunk(chunks, "CDAT");
        if (!cd) return out;
        static const char kWant[] = "TexturePack";
        bool found = false;
        for (size_t i = 0; i + sizeof(kWant) <= cd->size && !found; ++i)
            if (!memcmp(cd->data + i, kWant, sizeof(kWant))) found = true;   // with its NUL
        if (!found) return out;
    }
    std::string t = sbinObjectsTextExpanded(data, len);
    TexturePackBox cur;
    bool inBox = false;
    std::string sub;          // the field whose object the lines are in
    size_t p = 0;
    auto flush = [&]() { if (inBox && cur.blob >= 0) out.push_back(cur); };
    while (p < t.size()) {
        size_t e = t.find('\n', p);
        if (e == std::string::npos) e = t.size();
        std::string line = t.substr(p, e - p);
        p = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t a = line.find_first_not_of(' ');
        if (a == std::string::npos) continue;
        line.erase(0, a);
        if (line.size() > 4 && line.find("] Box {") != std::string::npos) {
            flush();
            cur = TexturePackBox();
            inBox = true;
            sub.clear();
            continue;
        }
        if (!inBox) continue;
        auto eq = line.find(" = ");
        if (eq == std::string::npos) { if (line == "}") sub.clear(); continue; }
        std::string key = line.substr(0, eq), val = line.substr(eq + 3);
        if (val.size() > 2 && val.back() == '{') { sub = key; continue; }
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
        int iv = atoi(val.c_str());
        if (sub.empty()) {
            if (key == "name") cur.name = val;
        } else if (sub == "source_rect") {
            if (key == "x") cur.x = iv; else if (key == "y") cur.y = iv;
            else if (key == "width") cur.w = iv; else if (key == "height") cur.h = iv;
        } else if (sub == "image") {
            if (key == "format") cur.format = iv; else if (key == "data") cur.blob = iv;
            else if (key == "width") cur.imageW = iv; else if (key == "height") cur.imageH = iv;
        }
    }
    flush();
    return out;
}

std::vector<NamedImage> texturePackPictures(const uint8_t* data, size_t len) {
    std::vector<NamedImage> out;
    std::vector<TexturePackBox> boxes = readTexturePack(data, len);
    if (boxes.empty()) return out;
    std::vector<SbaEntry> entries = readSba(data, len);
    std::map<int, Image> pages;
    for (const TexturePackBox& b : boxes) {
        if (isSvgBox(b)) continue;
        auto pg = pages.find(b.blob);
        if (pg == pages.end()) {
            Image img;
            for (const SbaEntry& en : entries)
                if (en.index == b.blob) { decodeImageAuto(en, img); break; }
            pg = pages.emplace(b.blob, toRgba(img)).first;
        }
        const Image& page = pg->second;
        if (!page.ok() || b.w <= 0 || b.h <= 0) continue;
        int x0 = std::max(0, b.x), y0 = std::max(0, b.y);
        int x1 = std::min(page.width, b.x + b.w), y1 = std::min(page.height, b.y + b.h);
        if (x1 <= x0 || y1 <= y0) continue;
        NamedImage ni;
        ni.name = b.name;
        ni.image.width = x1 - x0;
        ni.image.height = y1 - y0;
        ni.image.channels = 4;
        ni.image.pixels.resize((size_t)ni.image.width * ni.image.height * 4);
        for (int y = y0; y < y1; ++y)
            memcpy(&ni.image.pixels[(size_t)(y - y0) * ni.image.width * 4],
                   &page.pixels[((size_t)y * page.width + x0) * 4], (size_t)ni.image.width * 4);
        out.push_back(std::move(ni));
    }
    return out;
}

std::vector<SbaEntry> readSba(const uint8_t* data, size_t len) {
    std::vector<SbaEntry> out;
    auto chunks = sbinChunks(data, len);
    const SbinChunk* bulk = findChunk(chunks, "BULK");
    const SbinChunk* barg = findChunk(chunks, "BARG");
    if (!bulk || !barg) return out;

    // some BARG chunks carry a 16-byte sentinel before the payload
    const uint8_t* body = barg->data;
    size_t bodyLen = barg->size;
    if (bodyLen >= 16 && memcmp(body, kBargSentinel, 16) == 0) {
        body += 16;
        bodyLen -= 16;
    }

    size_t n = bulk->size / 8;
    // Real Racing Next wraps each payload: a 16-byte header (u32 flag - 0
    // stored, 1 LZ4, 2 zstd - u32 unpacked size, 8 zero bytes). Unwrapped
    // here, so everything below sees the image data itself.
    std::map<int, Bytes> unwrapped;
    for (size_t i = 0; i < n; ++i) {
        uint32_t off, size;
        memcpy(&off, bulk->data + i * 8, 4);
        memcpy(&size, bulk->data + i * 8 + 4, 4);
        if (off >= bodyLen || size <= 16 || off + (size_t)size > bodyLen) continue;
        const uint8_t* p = body + off;
        uint32_t flag, usize;
        memcpy(&flag, p, 4);
        memcpy(&usize, p + 4, 4);
        bool zeros = true;
        for (int k = 8; k < 16; ++k) if (p[k]) zeros = false;
        if (!zeros || flag > 2) continue;
        Bytes out;
        if (flag == 2 && size >= 20 && p[16] == 0x28 && p[17] == 0xB5 && p[18] == 0x2F && p[19] == 0xFD) {
            if (zstdAvailable() && zstdDecompress(p + 16, size - 16, 0, out)) unwrapped[(int)i] = std::move(out);
        } else if (flag == 1 && usize > 0 && usize < (1u << 28)) {
            if (lz4DecompressBlock(p + 16, size - 16, usize, out)) unwrapped[(int)i] = std::move(out);
        } else if (flag == 0 && usize == 0xFFFFFFFFu) {
            unwrapped[(int)i] = Bytes(p + 16, p + size);
        }
    }
    std::map<int, size_t> blobSizes;
    for (size_t i = 0; i < n; ++i) {
        uint32_t off, size;
        memcpy(&off, bulk->data + i * 8, 4);
        memcpy(&size, bulk->data + i * 8 + 4, 4);
        auto u = unwrapped.find((int)i);
        if (u != unwrapped.end()) { blobSizes[(int)i] = u->second.size(); continue; }
        if (off >= bodyLen) continue;
        blobSizes[(int)i] = std::min<size_t>(size, bodyLen - off);
    }
    // The declared records: keep the largest one per blob, which is the record
    // for that mip level.
    std::map<int, SbaRecord> byBlob;
    std::vector<SbaRecord> records;
    std::set<int> svgBlobs;
    uint32_t version = 0;
    memcpy(&version, data + 4, 4);
    if (version == 3) {
        // Most Wanted 2012: the Image objects say it outright
        for (const SbinImageRecord& r : sbinVersion3Images(data, len))
            records.push_back({ codecFromName(r.format), r.blob, r.width, r.height });
    } else {
        records = sbaRecords(chunks, blobSizes);
        // a UI texture pack: its Image records sit inside Box objects, where
        // the scan above does not look; the boxes say each page's size
        std::vector<TexturePackBox> boxes = readTexturePack(data, len);
        if (!boxes.empty()) records.clear();      // the boxes are the truth
        for (const TexturePackBox& b : boxes) {
            if (b.blob < 0 || b.imageW <= 0 || b.imageH <= 0) continue;
            // vector icons (.svg, stored as a packed SVG tree) are not pictures
            if (isSvgBox(b)) { svgBlobs.insert(b.blob); continue; }
            SbaRecord r{ SBA_UNKNOWN, b.blob, b.imageW, b.imageH };
            r.codec = SBA_DEFAULT;      // the payload itself says what it is
            records.push_back(r);
        }
    }
    for (const SbaRecord& r : records) {
        auto it = byBlob.find(r.blob);
        if (it == byBlob.end() || (size_t)r.w * r.h > (size_t)it->second.w * it->second.h)
            byBlob[r.blob] = r;
    }

    for (size_t i = 0; i < n; ++i) {
        uint32_t off, size;
        memcpy(&off, bulk->data + i * 8, 4);
        memcpy(&size, bulk->data + i * 8 + 4, 4);
        if (off >= bodyLen) continue;
        size_t avail = bodyLen - off;
        size_t use = std::min<size_t>(size, avail);
        if (!use || svgBlobs.count((int)i)) continue;
        SbaEntry e;
        e.index = (int)i;
        auto u = unwrapped.find((int)i);
        if (u != unwrapped.end()) e.data = u->second;
        else e.data.assign(body + off, body + off + use);
        e.format = identifyImage(e.data.data(), e.data.size());
        size_t end = imagePayloadEnd(e.data.data(), e.data.size(), e.format);
        if (end > 0 && end < e.data.size()) e.data.resize(end);
        auto it = byBlob.find((int)i);
        if (it != byBlob.end()) {
            e.width = it->second.w;
            e.height = it->second.h;
            e.codec = it->second.codec;
            e.codecAlt = it->second.alt;
        }
        out.push_back(std::move(e));
    }
    // mip 0 first. The mip chain is stored largest-first, so the biggest
    // payload is the full-size image; the declared record supplies its codec
    // and dimensions, never its rank.
    std::sort(out.begin(), out.end(), [](const SbaEntry& a, const SbaEntry& b) {
        if (a.data.size() != b.data.size()) return a.data.size() > b.data.size();
        return (size_t)a.width * a.height > (size_t)b.width * b.height;
    });
    return out;
}

// block codecs, defined further down
namespace {
bool decodePvrtc(const uint8_t* data, size_t len, int w, int h, bool twoBpp, Image& out);
bool decodeEtc(const uint8_t* data, size_t len, int w, int h, bool withAlpha, Image& out);
bool decodeDxt(const uint8_t* data, size_t len, int w, int h, int variant, Image& out);
bool decodeAtc(const uint8_t* data, size_t len, int w, int h, int variant, Image& out);
}

// raw pixel payload -> Image, when the byte count matches a known layout
static bool decodeRawPixels(const SbaEntry& e, Image& out) {
    if (e.width <= 0 || e.height <= 0) return false;
    size_t n = (size_t)e.width * e.height;
    if (!n) return false;
    const Bytes& p = e.data;
    if (p.size() >= n * 4) {
        out.width = e.width; out.height = e.height; out.channels = 4;
        out.pixels.assign(p.begin(), p.begin() + n * 4);
        return true;
    }
    if (p.size() >= n * 3) {
        out.width = e.width; out.height = e.height; out.channels = 3;
        out.pixels.assign(p.begin(), p.begin() + n * 3);
        return true;
    }
    if (p.size() >= n * 2) {           // RGB565
        out.width = e.width; out.height = e.height; out.channels = 3;
        out.pixels.resize(n * 3);
        for (size_t i = 0; i < n; ++i) {
            uint16_t v = (uint16_t)(p[i * 2] | (p[i * 2 + 1] << 8));
            int r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
            out.pixels[i * 3 + 0] = (uint8_t)((r << 3) | (r >> 2));
            out.pixels[i * 3 + 1] = (uint8_t)((g << 2) | (g >> 4));
            out.pixels[i * 3 + 2] = (uint8_t)((b << 3) | (b >> 2));
        }
        return true;
    }
    if (p.size() >= n) {
        out.width = e.width; out.height = e.height; out.channels = 1;
        out.pixels.assign(p.begin(), p.begin() + n);
        return true;
    }
    return false;
}

void flipImageVertically(Image& img) {
    if (!img.ok() || img.height < 2) return;
    size_t stride = (size_t)img.width * img.channels;
    std::vector<uint8_t> row(stride);
    for (int y = 0; y < img.height / 2; ++y) {
        uint8_t* a = img.pixels.data() + (size_t)y * stride;
        uint8_t* b = img.pixels.data() + (size_t)(img.height - 1 - y) * stride;
        memcpy(row.data(), a, stride);
        memcpy(a, b, stride);
        memcpy(b, row.data(), stride);
    }
}

// Everything the game stores is bottom-up, so one flip here puts previews,
// saved images and the textures the 3D viewer samples all the same way up.
// The model readers are told not to flip V, which keeps the pair consistent.
static bool decodeImageRaw(const SbaEntry& e, Image& out, const std::string& hint);

bool decodeImageAuto(const SbaEntry& e, Image& out, const std::string& hint) {
    if (!decodeImageRaw(e, out, hint)) return false;
    // Both games write their textures bottom-up, the way OpenGL wants them,
    // whatever the container says. Real Racing 3's DDS files are bottom-up
    // too - 0.7 assumed the format's own top-down convention and showed the
    // Koenigsegg badge sheet upside down - so everything is turned over here.
    flipImageVertically(out);
    return true;
}

// No Limits' UI texture packs (texturepacks/ui/*.sba): a picture of up to
// 256 colours, LZ4-packed - one byte with the palette size (0 = 256), the
// palette as RGBA, then one index byte per pixel. The file's own enum names
// no codec for it, so it is recognised by the unpacked size matching.
static bool decodePal8Lz4(const SbaEntry& e, Image& out) {
    if (e.width <= 0 || e.height <= 0 || e.data.size() < 8) return false;
    size_t px = (size_t)e.width * e.height;
    if (e.data.size() > px + 1025 + 4096) return false;   // not packed at all
    // LZ4 block, read until the picture is complete (the payload carries
    // alignment padding after the stream)
    const uint8_t* src = e.data.data();
    size_t sn = e.data.size(), i = 0, want = 0;
    Bytes un;
    un.reserve(px + 1025);
    while (i < sn) {
        uint8_t t = src[i++];
        size_t lit = t >> 4;
        if (lit == 15) { uint8_t b; do { if (i >= sn) return false; b = src[i++]; lit += b; } while (b == 255); }
        if (i + lit > sn) return false;
        un.insert(un.end(), src + i, src + i + lit);
        i += lit;
        if (!want && !un.empty()) want = 1 + (size_t)(un[0] ? un[0] : 256) * 4 + px;
        if (want && un.size() >= want) break;
        if (i + 2 > sn) return false;
        size_t off = src[i] | (src[i + 1] << 8);
        i += 2;
        if (off == 0 || off > un.size()) return false;
        size_t ml = t & 15;
        if (ml == 15) { uint8_t b; do { if (i >= sn) return false; b = src[i++]; ml += b; } while (b == 255); }
        ml += 4;
        if (un.size() + ml > px + 1025 + 64) return false;
        size_t from = un.size() - off;
        for (size_t k = 0; k < ml; ++k) un.push_back(un[from + k]);
        if (want && un.size() >= want) break;
    }
    if (un.empty()) return false;
    size_t n = un[0] ? un[0] : 256;
    if (un.size() != 1 + n * 4 + px) return false;
    const uint8_t* pal = un.data() + 1;
    const uint8_t* idx = pal + n * 4;
    out.width = e.width;
    out.height = e.height;
    out.channels = 4;
    out.pixels.resize(px * 4);
    for (size_t j = 0; j < px; ++j) {
        size_t k = idx[j] < n ? idx[j] : 0;
        memcpy(&out.pixels[j * 4], pal + k * 4, 4);
    }
    return true;
}

static bool decodeImageRaw(const SbaEntry& e, Image& out, const std::string& hint) {
    if (e.format == "raw" && decodePal8Lz4(e, out)) return true;
    if (e.format == "png") return decodePng(e.data.data(), e.data.size(), out);
    if (e.format == "jpg") return decodeJpeg(e.data.data(), e.data.size(), out);
    if (e.format == "pvr") return decodePvrContainer(e.data.data(), e.data.size(), out);
    if (e.format == "dds") return decodeDdsContainer(e.data.data(), e.data.size(), out);
    if (e.format == "astc" && e.data.size() > 16) {
        // .astc file: magic, block x/y/z, then 24-bit width, height, depth
        const uint8_t* h = e.data.data();
        int w = h[7] | (h[8] << 8) | (h[9] << 16), ht = h[10] | (h[11] << 8) | (h[12] << 16);
        return decodeAstc(h + 16, e.data.size() - 16, w, ht, h[4], h[5], out);
    }
    if (e.codec > SBA_ASTC_BASE && e.width > 0 && e.height > 0) {
        int bw = (e.codec - SBA_ASTC_BASE) / 100, bh = (e.codec - SBA_ASTC_BASE) % 100;
        return decodeAstc(e.data.data(), e.data.size(), e.width, e.height, bw, bh, out);
    }

    // Two codecs fit the stored number and the byte count: decode both and
    // keep the one that looks like a picture - neighbouring pixels close to
    // each other - rather than block noise.
    if (e.codecAlt > SBA_DEFAULT && e.codec > SBA_DEFAULT && e.width > 0 && e.height > 0) {
        SbaEntry a = e, b = e;
        a.codecAlt = b.codecAlt = SBA_UNKNOWN;
        b.codec = e.codecAlt;
        Image ia, ib;
        bool okA = decodeImageRaw(a, ia, hint), okB = decodeImageRaw(b, ib, hint);
        auto roughness = [](const Image& im) {
            if (!im.ok() || im.channels < 3) return 1e30;
            double sum = 0;
            size_t n = 0;
            int ch = im.channels;
            for (int y = 0; y + 1 < im.height; y += 2)
                for (int x = 0; x + 1 < im.width; x += 2) {
                    const uint8_t* p = &im.pixels[((size_t)y * im.width + x) * ch];
                    const uint8_t* r = p + ch;
                    const uint8_t* d = p + (size_t)im.width * ch;
                    for (int c = 0; c < ch; ++c) sum += std::abs(p[c] - r[c]) + std::abs(p[c] - d[c]);
                    ++n;
                }
            return n ? sum / n : 1e30;
        };
        if (okA && okB) { out = roughness(ib) < roughness(ia) ? std::move(ib) : std::move(ia); return true; }
        if (okA) { out = std::move(ia); return true; }
        if (okB) { out = std::move(ib); return true; }
    }
    // When the .sba names its codec, use it.
    if (e.codec > SBA_DEFAULT && e.width > 0 && e.height > 0) {
        const uint8_t* d = e.data.data();
        size_t n = e.data.size();
        int w = e.width, h = e.height;
        switch (e.codec) {
            case SBA_PVRTC_2BPP_RGB:
            case SBA_PVRTC_2BPP_RGBA: return decodePvrtc(d, n, w, h, true, out);
            case SBA_PVRTC_4BPP_RGBA:
            case SBA_PVRTC_4BPP_RGB:  return decodePvrtc(d, n, w, h, false, out);
            case SBA_ETC_RGB:
            case SBA_ETC2_RGB:        return decodeEtc(d, n, w, h, false, out);
            case SBA_ETC2_RGBA:       return decodeEtc(d, n, w, h, true, out);
            case SBA_DXT1:            return decodeDxt(d, n, w, h, 1, out);
            case SBA_DXT3:            return decodeDxt(d, n, w, h, 3, out);
            case SBA_DXT5:            return decodeDxt(d, n, w, h, 5, out);
            case SBA_PNG:             return decodePng(d, n, out);
            case SBA_JPEG:            return decodeJpeg(d, n, out);
            case SBA_ATC_RGB:         return decodeAtc(d, n, w, h, 1, out);
            case SBA_ATC_RGBA_EXPLICIT:     return decodeAtc(d, n, w, h, 3, out);
            case SBA_ATC_RGBA_INTERPOLATED: return decodeAtc(d, n, w, h, 5, out);
            case SBA_RGB: case SBA_RGBA: case SBA_RGB565: {
                SbaEntry tmp = e;
                return decodeRawPixels(tmp, out);
            }
            default: break;   // ATC and friends fall through to the guesses
        }
    }

    if (e.format != "raw") return false;
    if (decodeRawPixels(e, out)) return true;
    if (e.width > 0 && e.height > 0 &&
        decodeBlockCompressed(e.data.data(), e.data.size(), e.width, e.height, hint, out))
        return true;
    return false;
}

bool decodeTextureFile(const uint8_t* data, size_t len, Image& out) {
    // NFS Shift: every .m3g, textures included, is a gzip file
    if (len > 18 && data[0] == 0x1F && data[1] == 0x8B) {
        Bytes un;
        if (inflateGzip(data, len, un) && un.size() > 12 && un[0] == 0xAB)
            return decodeTextureFile(un.data(), un.size(), out);
    }
    // Hot Pursuit keeps its textures as M3G files holding one Image2D,
    // stored bottom-up like everything else these games upload
    if (len >= 12 && data[0] == 0xAB && data[7] == 0xBB && decodeM3gImage(data, len, out)) {
        flipImageVertically(out);
        return true;
    }
    if (len >= 4 && (!memcmp(data, "PVR\x03", 4) ||
                     (len >= 48 && !memcmp(data + 44, "PVR!", 4))))
        return decodePvrContainer(data, len, out);
    if (len >= 128 && !memcmp(data, "DDS ", 4)) {
        if (!decodeDdsContainer(data, len, out)) return false;
        flipImageVertically(out);
        return true;
    }
    auto entries = readSba(data, len);
    return !entries.empty() && decodeImageAuto(entries[0], out);
}

bool convertSba(const uint8_t* data, size_t len, const std::string& format,
                Bytes& out, std::string* usedExtension, std::string* error,
                const std::string& hint) {
    // A bare .pvr / container file (such as one SBAbrute wrote) converts too,
    // not just a .sba wrapper.
    std::vector<SbaEntry> entries;
    if (len >= 4 && (!memcmp(data, "PVR\x03", 4) ||
                     (len >= 48 && !memcmp(data + 44, "PVR!", 4)))) {
        SbaEntry e;
        e.format = "pvr";
        e.data.assign(data, data + len);
        entries.push_back(std::move(e));
    } else if ((len >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N') ||
               (len >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)) {
        // a plain picture (a loose .png / .jpg): converted, not unwrapped
        Image img;
        if (!decodeImageFile(data, len, img)) {
            if (error) *error = "the picture did not decode";
            return false;
        }
        if (format == "jpg" || format == "jpeg") { out = encodeJpeg(img, 92); if (usedExtension) *usedExtension = "jpg"; }
        else if (format == "bmp") { out = encodeBmp(img); if (usedExtension) *usedExtension = "bmp"; }
        else if (format == "tga") { out = encodeTga(img); if (usedExtension) *usedExtension = "tga"; }
        else if (format == "raw") { out.assign(data, data + len); if (usedExtension) *usedExtension = data[0] == 0x89 ? "png" : "jpg"; }
        else { out = encodePng(img); if (usedExtension) *usedExtension = "png"; }
        return !out.empty();
    } else if (len >= 128 && !memcmp(data, "DDS ", 4)) {
        SbaEntry e;
        e.format = "dds";
        e.data.assign(data, data + len);
        entries.push_back(std::move(e));
    } else {
        entries = readSba(data, len);
    }
    if (entries.empty()) {
        if (error) *error = "no image payload found in .sba";
        return false;
    }
    const SbaEntry& best = entries[0];

    // "raw" writes the payload exactly as stored, upside down and all - that
    // is what raw means. Every other format is decoded and re-encoded even
    // when the payload is already in it, because the stored copy is bottom-up
    // and a saved image that does not match its preview is worse than useless.
    if (format == "raw") {
        out = best.data;
        if (usedExtension) *usedExtension = best.format == "raw" ? "bin" : best.format;
        return true;
    }

    Image img;
    if (!decodeImageAuto(best, img, hint)) {
        // can't decode: fall back to a DDS wrapper for block data, else raw
        if (best.format == "raw" && best.width > 0 && best.height > 0) {
            size_t blocks = (size_t)std::max(1, (best.width + 3) / 4) *
                            std::max(1, (best.height + 3) / 4);
            if (best.data.size() >= blocks * 8) {
                const char* fc = best.data.size() >= blocks * 16 ? "DXT5" : "DXT1";
                out = makeDds(best.width, best.height, fc, best.data.data(), best.data.size());
                if (usedExtension) *usedExtension = "dds";
                return true;
            }
        }
        out = best.data;
        if (usedExtension) *usedExtension = best.format == "raw" ? "bin" : best.format;
        if (error) *error = "payload is " + best.format + "; written without conversion";
        return true;
    }

    if (format == "png")      { out = encodePng(img);  if (usedExtension) *usedExtension = "png"; }
    else if (format == "bmp") { out = encodeBmp(img);  if (usedExtension) *usedExtension = "bmp"; }
    else if (format == "tga") { out = encodeTga(img);  if (usedExtension) *usedExtension = "tga"; }
    else if (format == "jpg" || format == "jpeg") {
        out = encodeJpeg(img, 92);
        if (usedExtension) *usedExtension = "jpg";
    } else if (format == "dds") {
        // DXT5 when the picture has any transparency, DXT1 when it has none:
        // what every DDS reader (Photoshop, GIMP, paint.net) opens
        Image rgba = toRgba(img);
        bool alpha = false;
        for (size_t i = 3; i < rgba.pixels.size(); i += 4) if (rgba.pixels[i] < 250) { alpha = true; break; }
        Bytes blocks = encodeDxt(rgba, alpha ? 5 : 1);
        out = makeDds(rgba.width, rgba.height, alpha ? "DXT5" : "DXT1", blocks.data(), blocks.size());
        if (usedExtension) *usedExtension = "dds";
    } else {
        out = encodePng(img);
        if (usedExtension) *usedExtension = "png";
    }
    return !out.empty();
}


// ================================================================ block codecs
//
// Mobile builds of the game ship the same texture in several encodings; the
// pack's sku says which ("texture_etc" -> ETC, otherwise DXT). Both are 4bpp
// for RGB and 8bpp with alpha, so the byte count alone cannot tell them
// apart - the caller passes a hint, and when there is none both are decoded
// and the less noisy result wins.

namespace {

// ETC1 intensity modifiers. The order is NOT ascending - a selector picks
//   0 = +small, 1 = +large, 2 = -small, 3 = -large
// Sorting these rows (which is the obvious-looking thing to do) swaps the two
// bright selectors for the two dark ones: the image keeps its structure and
// comes out far too dark, which looks like a bad texture rather than a bug.
const int kEtcModifier[8][4] = {
    {2, 8, -2, -8},     {5, 17, -5, -17},    {9, 29, -9, -29},    {13, 42, -13, -42},
    {18, 60, -18, -60}, {24, 80, -24, -80},  {33, 106, -33, -106},{47, 183, -47, -183}
};
const int kEtcDistance[8] = {3, 6, 11, 16, 23, 32, 41, 64};

inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }
inline int extend5(int v)  { return (v << 3) | (v >> 2); }
inline int extend4(int v)  { return v * 17; }
inline int extend6(int v)  { return (v << 2) | (v >> 4); }
inline int extend7(int v)  { return (v << 1) | (v >> 6); }
inline int signed3(int v)  { return v > 3 ? v - 8 : v; }

struct RGB { int r, g, b; };

void etcWritePixel(Image& img, int x, int y, int r, int g, int b, int a) {
    if (x < 0 || y < 0 || x >= img.width || y >= img.height) return;
    uint8_t* p = img.pixels.data() + ((size_t)y * img.width + x) * img.channels;
    p[0] = clamp8(r); p[1] = clamp8(g); p[2] = clamp8(b);
    if (img.channels == 4) p[3] = (uint8_t)a;
}

// one 8-byte ETC1/ETC2 RGB block at (bx,by)
void decodeEtcBlock(const uint8_t* b, Image& img, int bx, int by, int alpha) {
    int flip = b[3] & 1;
    int diff = (b[3] >> 1) & 1;

    RGB c0{}, c1{};
    bool planar = false, th = false;
    RGB paint[4];

    if (!diff) {
        c0 = { extend4(b[0] >> 4), extend4(b[1] >> 4), extend4(b[2] >> 4) };
        c1 = { extend4(b[0] & 15), extend4(b[1] & 15), extend4(b[2] & 15) };
    } else {
        int r = b[0] >> 3, dr = signed3(b[0] & 7);
        int g = b[1] >> 3, dg = signed3(b[1] & 7);
        int bl = b[2] >> 3, db = signed3(b[2] & 7);
        int r1 = r + dr, g1 = g + dg, b1 = bl + db;
        if (r1 < 0 || r1 > 31) {            // ETC2 'T' mode
            th = true;
            int r0 = ((b[0] >> 3) & 3) | ((b[0] >> 1) & 0xC);
            RGB a{ extend4(r0), extend4(b[1] >> 4), extend4(b[1] & 15) };
            RGB bb{ extend4(b[2] >> 4), extend4(b[2] & 15), extend4(b[3] >> 4) };
            int d = kEtcDistance[((b[3] >> 1) & 3) | ((b[3] << 1) & 4)];
            paint[0] = a;
            paint[1] = { bb.r + d, bb.g + d, bb.b + d };
            paint[2] = bb;
            paint[3] = { bb.r - d, bb.g - d, bb.b - d };
        } else if (g1 < 0 || g1 > 31) {     // ETC2 'H' mode
            th = true;
            int ra = (b[0] >> 3) & 0xF;
            int ga = ((b[0] & 7) << 1) | ((b[1] >> 4) & 1);
            int ba = (b[1] & 8) | ((b[1] & 3) << 1) | ((b[2] >> 7) & 1);
            int rb = (b[2] >> 3) & 0xF;
            int gb = ((b[2] & 7) << 1) | (b[3] >> 7);
            int bb2 = (b[3] >> 3) & 0xF;
            RGB a{ extend4(ra), extend4(ga), extend4(ba) };
            RGB bq{ extend4(rb), extend4(gb), extend4(bb2) };
            int di = ((b[3] & 4) | ((b[3] & 1) << 1));
            if (a.r * 65536 + a.g * 256 + a.b >= bq.r * 65536 + bq.g * 256 + bq.b) di |= 1;
            int d = kEtcDistance[di & 7];
            paint[0] = { a.r + d, a.g + d, a.b + d };
            paint[1] = { a.r - d, a.g - d, a.b - d };
            paint[2] = { bq.r + d, bq.g + d, bq.b + d };
            paint[3] = { bq.r - d, bq.g - d, bq.b - d };
        } else if (b1 < 0 || b1 > 31) {     // ETC2 planar mode
            planar = true;
            int RO = extend6(((b[0] & 0x7E) >> 1));
            int GO = extend7((((b[0] & 1) << 6) | ((b[1] & 0x7E) >> 1)));
            int BO = extend6((((b[1] & 1) << 5) | ((b[2] & 0x18) >> 1) | (b[2] & 3) << 1 |
                              ((b[3] & 0x80) >> 7)));
            int RH = extend6((((b[3] & 0x7C) >> 1) | (b[3] & 1)));
            int GH = extend7(b[4] >> 1);
            int BH = extend6((((b[4] & 1) << 5) | (b[5] >> 3)));
            int RV = extend6((((b[5] & 7) << 3) | (b[6] >> 5)));
            int GV = extend7((((b[6] & 0x1F) << 2) | (b[7] >> 6)));
            int BV = extend6(b[7] & 0x3F);
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) {
                    int r2 = (x * (RH - RO) + y * (RV - RO) + 4 * RO + 2) >> 2;
                    int g2 = (x * (GH - GO) + y * (GV - GO) + 4 * GO + 2) >> 2;
                    int b2 = (x * (BH - BO) + y * (BV - BO) + 4 * BO + 2) >> 2;
                    etcWritePixel(img, bx * 4 + x, by * 4 + y, r2, g2, b2, alpha);
                }
            return;
        } else {
            c0 = { extend5(r), extend5(g), extend5(bl) };
            c1 = { extend5(r1), extend5(g1), extend5(b1) };
        }
    }
    (void)planar;

    int t0 = (b[3] >> 5) & 7, t1 = (b[3] >> 2) & 7;
    uint32_t idx = ((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) |
                   ((uint32_t)b[6] << 8) | b[7];
    for (int i = 0; i < 16; ++i) {
        int x = i >> 2, y = i & 3;
        int lsb = (idx >> i) & 1;
        int msb = (idx >> (16 + i)) & 1;
        int m = (msb << 1) | lsb;
        if (th) {
            const RGB& p = paint[m];
            etcWritePixel(img, bx * 4 + x, by * 4 + y, p.r, p.g, p.b, alpha);
        } else {
            int sub = (flip == 0) ? (x < 2 ? 0 : 1) : (y < 2 ? 0 : 1);
            const RGB& base = sub == 0 ? c0 : c1;
            int d = kEtcModifier[sub == 0 ? t0 : t1][m];
            etcWritePixel(img, bx * 4 + x, by * 4 + y,
                          base.r + d, base.g + d, base.b + d, alpha);
        }
    }
}

// ETC2 alpha (EAC) block, 8 bytes, preceding the colour block in RGBA8
const int kEacTable[16][8] = {
 {-3,-6,-9,-15,2,5,8,14},{-3,-7,-10,-13,2,6,9,12},{-2,-5,-8,-13,1,4,7,12},{-2,-4,-6,-13,1,3,5,12},
 {-3,-6,-8,-12,2,5,7,11},{-3,-7,-9,-11,2,6,8,10},{-4,-7,-8,-11,3,6,7,10},{-3,-5,-8,-11,2,4,7,10},
 {-2,-6,-8,-10,1,5,7,9}, {-2,-5,-8,-10,1,4,7,9}, {-2,-4,-8,-10,1,3,7,9}, {-2,-5,-7,-10,1,4,6,9},
 {-3,-4,-7,-10,2,3,6,9}, {-1,-2,-3,-10,0,1,2,9}, {-4,-6,-8,-9,3,5,7,8},  {-3,-5,-7,-9,2,4,6,8}
};

void decodeEacAlphaBlock(const uint8_t* b, Image& img, int bx, int by) {
    int base = b[0];
    int mul = (b[1] >> 4) & 15;
    int tbl = b[1] & 15;
    uint64_t bits = 0;
    for (int i = 2; i < 8; ++i) bits = (bits << 8) | b[i];
    for (int i = 0; i < 16; ++i) {
        int x = i >> 2, y = i & 3;
        int shift = 45 - 3 * i;
        int v = (int)((bits >> shift) & 7);
        int a = base + kEacTable[tbl][v] * (mul == 0 ? 1 : mul);
        int px = bx * 4 + x, py = by * 4 + y;
        if (px < img.width && py < img.height && img.channels == 4)
            img.pixels[((size_t)py * img.width + px) * 4 + 3] = clamp8(a);
    }
}

bool decodeEtc(const uint8_t* data, size_t len, int w, int h, bool withAlpha, Image& out) {
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    size_t blockBytes = withAlpha ? 16 : 8;
    if (len < (size_t)bw * bh * blockBytes) return false;
    out.width = w; out.height = h; out.channels = withAlpha ? 4 : 3;
    out.pixels.assign((size_t)w * h * out.channels, 255);
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            const uint8_t* blk = data + ((size_t)by * bw + bx) * blockBytes;
            if (withAlpha) {
                decodeEacAlphaBlock(blk, out, bx, by);
                decodeEtcBlock(blk + 8, out, bx, by, 255);
            } else {
                decodeEtcBlock(blk, out, bx, by, 255);
            }
        }
    return true;
}

// --- DXT / BC ---
void dxtColors(const uint8_t* b, RGB* c, bool dxt1) {
    int c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
    c[0] = { extend5((c0 >> 11) & 31), extend6((c0 >> 5) & 63), extend5(c0 & 31) };
    c[1] = { extend5((c1 >> 11) & 31), extend6((c1 >> 5) & 63), extend5(c1 & 31) };
    if (!dxt1 || c0 > c1) {
        c[2] = { (2 * c[0].r + c[1].r) / 3, (2 * c[0].g + c[1].g) / 3, (2 * c[0].b + c[1].b) / 3 };
        c[3] = { (c[0].r + 2 * c[1].r) / 3, (c[0].g + 2 * c[1].g) / 3, (c[0].b + 2 * c[1].b) / 3 };
    } else {
        c[2] = { (c[0].r + c[1].r) / 2, (c[0].g + c[1].g) / 2, (c[0].b + c[1].b) / 2 };
        c[3] = { 0, 0, 0 };
    }
}

// ATI/Qualcomm ATC, which the Android builds of Real Racing 2 use. A block is
// laid out like DXT1 (two colours, sixteen 2-bit indices), but the first
// colour is RGB555 with its top bit choosing how the four colours are made,
// and the in-between colours sit at 3/8 and 5/8 rather than 1/3 and 2/3.
// The RGBA forms put a DXT3-style (explicit) or DXT5-style (interpolated)
// alpha block in front, exactly as DXT does.
static void atcColors(const uint8_t* b, RGB c[4]) {
    uint16_t c0 = (uint16_t)(b[0] | (b[1] << 8));
    uint16_t c1 = (uint16_t)(b[2] | (b[3] << 8));
    int r0 = (c0 >> 10) & 31, g0 = (c0 >> 5) & 31, b0 = c0 & 31;
    RGB a = { (r0 << 3) | (r0 >> 2), (g0 << 3) | (g0 >> 2), (b0 << 3) | (b0 >> 2) };
    int r1 = (c1 >> 11) & 31, g1 = (c1 >> 5) & 63, b1 = c1 & 31;
    RGB d = { (r1 << 3) | (r1 >> 2), (g1 << 2) | (g1 >> 4), (b1 << 3) | (b1 >> 2) };
    if (c0 & 0x8000) {
        c[0] = { 0, 0, 0 };
        c[1] = { std::max(0, a.r - d.r / 4), std::max(0, a.g - d.g / 4), std::max(0, a.b - d.b / 4) };
        c[2] = a;
        c[3] = d;
    } else {
        c[0] = a;
        c[1] = { (5 * a.r + 3 * d.r) / 8, (5 * a.g + 3 * d.g) / 8, (5 * a.b + 3 * d.b) / 8 };
        c[2] = { (3 * a.r + 5 * d.r) / 8, (3 * a.g + 5 * d.g) / 8, (3 * a.b + 5 * d.b) / 8 };
        c[3] = d;
    }
}

// variant: 1 = ATC RGB, 3 = ATC RGBA explicit alpha, 5 = interpolated alpha
bool decodeAtc(const uint8_t* data, size_t len, int w, int h, int variant, Image& out) {
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    size_t blockBytes = variant == 1 ? 8 : 16;
    if (w <= 0 || h <= 0 || len < (size_t)bw * bh * blockBytes) return false;
    bool alpha = variant != 1;
    out.width = w; out.height = h; out.channels = alpha ? 4 : 3;
    out.pixels.assign((size_t)w * h * out.channels, 255);
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            const uint8_t* blk = data + ((size_t)by * bw + bx) * blockBytes;
            const uint8_t* cb = alpha ? blk + 8 : blk;
            RGB c[4];
            atcColors(cb, c);
            uint32_t bits;
            memcpy(&bits, cb + 4, 4);
            uint8_t a0 = blk[0], a1 = blk[1];
            uint64_t abits = 0;
            if (alpha) {
                int first = variant == 3 ? 0 : 2;
                for (int i = 7; i >= first; --i) abits = (abits << 8) | blk[i];
            }
            for (int i = 0; i < 16; ++i) {
                int px = bx * 4 + (i & 3), py = by * 4 + (i >> 2);
                if (px >= w || py >= h) continue;
                const RGB& col = c[(bits >> (2 * i)) & 3];
                uint8_t* p = out.pixels.data() + ((size_t)py * w + px) * out.channels;
                p[0] = clamp8(col.r); p[1] = clamp8(col.g); p[2] = clamp8(col.b);
                if (alpha && variant == 3) {
                    p[3] = clamp8((int)((abits >> (4 * i)) & 0xf) * 17);
                } else if (alpha) {
                    int code = (int)((abits >> (3 * i)) & 7), a;
                    if (code == 0) a = a0;
                    else if (code == 1) a = a1;
                    else if (a0 > a1) a = ((8 - code) * a0 + (code - 1) * a1) / 7;
                    else if (code == 6) a = 0;
                    else if (code == 7) a = 255;
                    else a = ((6 - code) * a0 + (code - 1) * a1) / 5;
                    p[3] = clamp8(a);
                }
            }
        }
    return true;
}

bool decodeDxt(const uint8_t* data, size_t len, int w, int h, int variant, Image& out) {
    // variant: 1 = DXT1 (8 bytes/block), 3 = DXT3 (explicit 4-bit alpha),
    //          5 = DXT5 (interpolated alpha); 3 and 5 are 16 bytes/block
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    size_t blockBytes = variant == 1 ? 8 : 16;
    if (len < (size_t)bw * bh * blockBytes) return false;
    bool alpha = variant != 1;
    out.width = w; out.height = h; out.channels = alpha ? 4 : 3;
    out.pixels.assign((size_t)w * h * out.channels, 255);
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            const uint8_t* blk = data + ((size_t)by * bw + bx) * blockBytes;
            const uint8_t* cb = alpha ? blk + 8 : blk;
            RGB c[4];
            dxtColors(cb, c, variant == 1);
            uint32_t bits;
            memcpy(&bits, cb + 4, 4);
            uint8_t a0 = 0, a1 = 0;
            uint64_t abits = 0;
            if (alpha) {
                a0 = blk[0]; a1 = blk[1];
                if (variant == 3) {
                    // DXT3: sixteen 4-bit alpha values, no interpolation
                    for (int i = 7; i >= 0; --i) abits = (abits << 8) | blk[i];
                } else {
                    for (int i = 7; i >= 2; --i) abits = (abits << 8) | blk[i];
                }
            }
            for (int i = 0; i < 16; ++i) {
                int x = i & 3, y = i >> 2;
                int px = bx * 4 + x, py = by * 4 + y;
                if (px >= w || py >= h) continue;
                const RGB& col = c[(bits >> (2 * i)) & 3];
                uint8_t* p = out.pixels.data() + ((size_t)py * w + px) * out.channels;
                p[0] = clamp8(col.r); p[1] = clamp8(col.g); p[2] = clamp8(col.b);
                if (alpha && variant == 3) {
                    int a = (int)((abits >> (4 * i)) & 0xf);
                    p[3] = clamp8(a * 17);
                } else if (alpha) {
                    int code = (int)((abits >> (3 * i)) & 7);
                    int a;
                    if (code == 0) a = a0;
                    else if (code == 1) a = a1;
                    else if (a0 > a1) a = ((8 - code) * a0 + (code - 1) * a1) / 7;
                    else if (code == 6) a = 0;
                    else if (code == 7) a = 255;
                    else a = ((6 - code) * a0 + (code - 1) * a1) / 5;
                    p[3] = clamp8(a);
                }
            }
        }
    return true;
}

// Mean absolute difference between horizontally adjacent pixels. A texture
// decoded with the wrong codec turns into block noise and scores far higher
// than the same data decoded correctly.
double noiseScore(const Image& img) {
    if (!img.ok()) return 1e18;
    double sum = 0;
    size_t count = 0;
    int ch = img.channels;
    for (int y = 0; y < img.height; ++y)
        for (int x = 1; x < img.width; ++x) {
            const uint8_t* a = img.pixels.data() + ((size_t)y * img.width + x - 1) * ch;
            const uint8_t* b = img.pixels.data() + ((size_t)y * img.width + x) * ch;
            for (int c = 0; c < 3 && c < ch; ++c) { sum += std::abs((int)a[c] - (int)b[c]); count++; }
        }
    return count ? sum / count : 1e18;
}

// ================================================================ PVRTC
//
// PowerVR texture compression, which is what this game actually ships its
// mobile textures in - not ETC. A 4bpp block covers 4x4 pixels in 8 bytes:
// two endpoint colours plus a 2-bit modulation value per pixel. Unlike ETC
// or DXT the endpoint colours are *shared between neighbouring blocks* and
// bilinearly interpolated across them, which is why decoding PVRTC data as
// ETC produces the blocky, dark, dithered mess rather than an obviously
// wrong image - the block size is identical, only the meaning differs.
//
// Blocks are stored in Morton (Z-curve) order, not raster order.

static uint32_t pvrTwiddle(uint32_t xs, uint32_t ys, uint32_t x, uint32_t y) {
    uint32_t minDim = xs, maxVal = y;
    if (ys < xs) { minDim = ys; maxVal = x; }
    uint32_t t = 0, src = 1, dst = 1;
    int shift = 0;
    while (src < minDim) {
        if (y & src) t |= dst;
        if (x & src) t |= dst << 1;
        src <<= 1; dst <<= 2; ++shift;
    }
    maxVal >>= shift;
    t |= maxVal << (2 * shift);
    return t;
}

struct PvrColour { int r, g, b, a; };

static void pvrColours(uint32_t cd, PvrColour& A, PvrColour& B) {
    if (cd & (1u << 15)) {                    // opaque, RGB 554
        A.r = (cd & 0x7c00) >> 10;
        A.g = (cd & 0x03e0) >> 5;
        // 4 bits held in positions 1..4, widened to 5 by repeating the top bit
        A.b = (cd & 0x001e) | ((cd & 0x001e) >> 4);
        A.a = 15;
    } else {                                  // translucent, ARGB 3443
        A.r = ((cd & 0x0f00) >> 7) | ((cd & 0x0f00) >> 11);
        A.g = ((cd & 0x00f0) >> 3) | ((cd & 0x00f0) >> 7);
        A.b = ((cd & 0x000e) << 1) | ((cd & 0x000e) >> 2);
        A.a = (cd & 0x7000) >> 11;
    }
    if (cd & (1u << 31)) {                    // opaque, RGB 555
        B.r = (cd & 0x7c000000) >> 26;
        B.g = (cd & 0x03e00000) >> 21;
        B.b = (cd & 0x001f0000) >> 16;
        B.a = 15;
    } else {                                  // translucent, ARGB 4443
        B.r = ((cd & 0x0f000000) >> 23) | ((cd & 0x0f000000) >> 27);
        B.g = ((cd & 0x00f00000) >> 19) | ((cd & 0x00f00000) >> 23);
        B.b = ((cd & 0x000f0000) >> 15) | ((cd & 0x000f0000) >> 19);
        B.a = (cd & 0x70000000) >> 27;
    }
    // 5/4 bit channels to 8 bit
    A.r = A.r * 255 / 31; A.g = A.g * 255 / 31; A.b = A.b * 255 / 31; A.a = A.a * 255 / 15;
    B.r = B.r * 255 / 31; B.g = B.g * 255 / 31; B.b = B.b * 255 / 31; B.a = B.a * 255 / 15;
}

bool decodePvrtc(const uint8_t* data, size_t len, int w, int h,
                 bool twoBpp, Image& out) {
    if (w <= 0 || h <= 0) return false;
    int blockW = twoBpp ? 8 : 4;
    int blockH = 4;
    int bw = std::max(2, (w + blockW - 1) / blockW);
    int bh = std::max(2, (h + blockH - 1) / blockH);
    if (len < (size_t)bw * bh * 8) return false;

    std::vector<PvrColour> ca((size_t)bw * bh), cb((size_t)bw * bh);
    std::vector<uint32_t> mod((size_t)bw * bh);
    std::vector<uint8_t> mode((size_t)bw * bh);
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            size_t off = (size_t)pvrTwiddle(bw, bh, bx, by) * 8;
            if (off + 8 > len) return false;
            uint32_t m, cd;
            memcpy(&m, data + off, 4);
            memcpy(&cd, data + off + 4, 4);
            size_t i = (size_t)by * bw + bx;
            pvrColours(cd, ca[i], cb[i]);
            mod[i] = m;
            mode[i] = (uint8_t)(cd & 1);
        }
    }

    out.width = w; out.height = h; out.channels = 4;
    out.pixels.assign((size_t)w * h * 4, 0);

    auto at = [&](const std::vector<PvrColour>& v, int bx, int by) -> const PvrColour& {
        return v[(size_t)by * bw + bx];
    };

    for (int y = 0; y < h; ++y) {
        int by0 = (((y - 2) >> 2) % bh + bh) % bh, by1 = (by0 + 1) % bh;
        int fy = (y - 2) & 3;
        for (int x = 0; x < w; ++x) {
            int bx0, fx;
            if (twoBpp) {
                bx0 = (((x - 4) >> 3) % bw + bw) % bw;
                fx = (x - 4) & 7;
            } else {
                bx0 = (((x - 2) >> 2) % bw + bw) % bw;
                fx = (x - 2) & 3;
            }
            int bx1 = (bx0 + 1) % bw;
            int span = twoBpp ? 8 : 4;

            int P[4], Q[4];
            const PvrColour* a00 = &at(ca, bx0, by0); const PvrColour* a01 = &at(ca, bx1, by0);
            const PvrColour* a10 = &at(ca, bx0, by1); const PvrColour* a11 = &at(ca, bx1, by1);
            const PvrColour* b00 = &at(cb, bx0, by0); const PvrColour* b01 = &at(cb, bx1, by0);
            const PvrColour* b10 = &at(cb, bx0, by1); const PvrColour* b11 = &at(cb, bx1, by1);
            const int ai[4][4] = {{a00->r,a01->r,a10->r,a11->r}, {a00->g,a01->g,a10->g,a11->g},
                                  {a00->b,a01->b,a10->b,a11->b}, {a00->a,a01->a,a10->a,a11->a}};
            const int bi[4][4] = {{b00->r,b01->r,b10->r,b11->r}, {b00->g,b01->g,b10->g,b11->g},
                                  {b00->b,b01->b,b10->b,b11->b}, {b00->a,b01->a,b10->a,b11->a}};
            for (int c = 0; c < 4; ++c) {
                int top = ai[c][0] * (span - fx) + ai[c][1] * fx;
                int bot = ai[c][2] * (span - fx) + ai[c][3] * fx;
                P[c] = (top * (4 - fy) + bot * fy) / (span * 4);
                top = bi[c][0] * (span - fx) + bi[c][1] * fx;
                bot = bi[c][2] * (span - fx) + bi[c][3] * fx;
                Q[c] = (top * (4 - fy) + bot * fy) / (span * 4);
            }

            size_t mi = (size_t)(y / blockH) * bw + (x / blockW);
            if (mi >= mod.size()) continue;
            int weight;
            bool punch = false;
            if (twoBpp) {
                // one bit per pixel; the "mode" bit selects the interpolated
                // variant, which this game does not use for 2bpp content
                int bit = (y & 3) * 8 + (x & 7);
                weight = ((mod[mi] >> bit) & 1) ? 8 : 0;
            } else {
                int bit = ((y & 3) * 4 + (x & 3)) * 2;
                int m = (mod[mi] >> bit) & 3;
                if (mode[mi]) {
                    static const int wm[4] = {0, 4, 4, 8};
                    weight = wm[m];
                    if (m == 2) punch = true;
                } else {
                    static const int wm[4] = {0, 3, 5, 8};
                    weight = wm[m];
                }
            }

            uint8_t* o = &out.pixels[((size_t)y * w + x) * 4];
            for (int c = 0; c < 4; ++c) {
                int v = (P[c] * (8 - weight) + Q[c] * weight) / 8;
                o[c] = (uint8_t)std::min(255, std::max(0, v));
            }
            if (punch) o[3] = 0;
        }
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------- containers
//
// A .sba payload is sometimes a complete PVR file rather than bare block
// data. Version 3 starts with "PVR\3"; the older version 2 puts "PVR!" at
// offset 44 and its header first.
bool decodePvrContainer(const uint8_t* data, size_t len, Image& out) {
    if (len < 52) return false;

    if (!memcmp(data, "PVR\x03", 4)) {
        uint64_t format;
        uint32_t height, width, mipCount, metaSize;
        memcpy(&format, data + 8, 8);
        memcpy(&height, data + 24, 4);
        memcpy(&width, data + 28, 4);
        memcpy(&mipCount, data + 44, 4);
        memcpy(&metaSize, data + 48, 4);
        (void)mipCount;
        size_t off = 52 + (size_t)metaSize;
        if (off >= len || width == 0 || height == 0 ||
            width > 16384 || height > 16384) return false;
        const uint8_t* body = data + off;
        size_t bodyLen = len - off;
        int w = (int)width, h = (int)height;

        // a channel-named format (the high half holds 'r','g','b','a') is
        // uncompressed; everything else is one of the enumerated codecs
        if ((format >> 32) != 0) {
            SbaEntry tmp;
            tmp.width = w; tmp.height = h;
            tmp.data.assign(body, body + bodyLen);
            return decodeRawPixels(tmp, out);
        }
        switch ((uint32_t)format) {
            case 0: case 1: return decodePvrtc(body, bodyLen, w, h, true, out);
            case 2: case 3: return decodePvrtc(body, bodyLen, w, h, false, out);
            case 6:  return decodeEtc(body, bodyLen, w, h, false, out);
            case 7:  return decodeDxt(body, bodyLen, w, h, 1, out);
            case 9: case 11: return decodeDxt(body, bodyLen, w, h, 5, out);
            case 22: return decodeEtc(body, bodyLen, w, h, false, out);
            case 23: return decodeEtc(body, bodyLen, w, h, true, out);
            default: return false;
        }
    }

    if (len >= 48 && !memcmp(data + 44, "PVR!", 4)) {
        uint32_t headerLen, height, width, flags;
        memcpy(&headerLen, data + 0, 4);
        memcpy(&height, data + 4, 4);
        memcpy(&width, data + 8, 4);
        memcpy(&flags, data + 16, 4);
        if (headerLen < 44 || headerLen > len || width == 0 || height == 0) return false;
        const uint8_t* body = data + headerLen;
        size_t bodyLen = len - headerLen;
        uint32_t kind = flags & 0xff;
        int w = (int)width, h = (int)height;
        if (width > 16384 || height > 16384) return false;
        bool ok = false;
        switch (kind) {
            // PVRTC, in both the MGL and the OGL numbering
            case 0x0C: case 0x18: ok = decodePvrtc(body, bodyLen, w, h, true, out); break;
            case 0x0D: case 0x19: ok = decodePvrtc(body, bodyLen, w, h, false, out); break;
            case 0x20: ok = decodeDxt(body, bodyLen, w, h, 1, out); break;
            case 0x22: ok = decodeDxt(body, bodyLen, w, h, 3, out); break;
            case 0x24: ok = decodeDxt(body, bodyLen, w, h, 5, out); break;
            case 0x36: ok = decodeEtc(body, bodyLen, w, h, false, out); break;
            // ATC, as Real Racing 2's Android build writes it: 0x78 RGB,
            // 0x79 RGBA with interpolated alpha
            case 0x78: ok = decodeAtc(body, bodyLen, w, h, 1, out); break;
            case 0x79: ok = decodeAtc(body, bodyLen, w, h, 5, out); break;
            case 0x7A: ok = decodeAtc(body, bodyLen, w, h, 3, out); break;
            // uncompressed OGL formats, 16 or 32 bits a pixel
            case 0x10: case 0x11: case 0x12: case 0x13: case 0x15: case 0x1A: case 0x00: case 0x01: case 0x02: {
                int bpp = (kind == 0x12 || kind == 0x1A) ? 4 : kind == 0x15 ? 3 : 2;
                size_t need = (size_t)w * h * bpp;
                if (bodyLen < need) return false;
                out.width = w; out.height = h; out.channels = 4;
                out.pixels.assign((size_t)w * h * 4, 255);
                for (size_t i = 0; i < (size_t)w * h; ++i) {
                    const uint8_t* q = body + i * bpp;
                    uint8_t* o = &out.pixels[i * 4];
                    uint16_t v = (uint16_t)(q[0] | (bpp >= 2 ? q[1] << 8 : 0));
                    auto x5 = [](int c) { return (uint8_t)((c << 3) | (c >> 2)); };
                    auto x4 = [](int c) { return (uint8_t)(c * 17); };
                    switch (kind) {
                        case 0x10: o[0] = x4(v >> 12); o[1] = x4((v >> 8) & 15); o[2] = x4((v >> 4) & 15); o[3] = x4(v & 15); break;
                        case 0x11: o[0] = x5(v >> 11); o[1] = x5((v >> 6) & 31); o[2] = x5((v >> 1) & 31); o[3] = (v & 1) ? 255 : 0; break;
                        case 0x13: case 0x02: o[0] = x5(v >> 11); o[1] = (uint8_t)((((v >> 5) & 63) << 2) | (((v >> 5) & 63) >> 4)); o[2] = x5(v & 31); break;
                        case 0x00: o[3] = x4(v >> 12); o[0] = x4((v >> 8) & 15); o[1] = x4((v >> 4) & 15); o[2] = x4(v & 15); break;
                        case 0x01: o[3] = (v & 0x8000) ? 255 : 0; o[0] = x5((v >> 10) & 31); o[1] = x5((v >> 5) & 31); o[2] = x5(v & 31); break;
                        case 0x12: o[0] = q[0]; o[1] = q[1]; o[2] = q[2]; o[3] = q[3]; break;
                        case 0x1A: o[0] = q[2]; o[1] = q[1]; o[2] = q[0]; o[3] = q[3]; break;
                        case 0x15: o[0] = q[0]; o[1] = q[1]; o[2] = q[2]; break;
                    }
                }
                ok = true;
                break;
            }
            default: return false;
        }
        // flag 0x10000 marks an image stored bottom-up
        if (ok && (flags & 0x10000)) flipImageVertically(out);
        return ok;
    }
    return false;
}

// A plain DDS file. Real Racing 3 ships every texture this way and calls them
// ".etc.dds": a standard 128-byte header, then either ETC1 blocks (FourCC
// "ETC ") or uncompressed RGBA4444 (no FourCC at all, four bits per channel
// with red in the low nibble). ATITC is declared by some builds and cannot be
// decoded - it is reported rather than turned into noise. DXT FourCCs are
// handled too, so an ordinary DDS from anywhere else opens as well.
bool decodeDdsContainer(const uint8_t* data, size_t len, Image& out) {
    if (len < 132 || memcmp(data, "DDS ", 4) != 0) return false;
    auto u32 = [&](size_t off) {
        return (uint32_t)data[off] | ((uint32_t)data[off + 1] << 8) |
               ((uint32_t)data[off + 2] << 16) | ((uint32_t)data[off + 3] << 24);
    };
    if (u32(4) != 124) return false;
    uint32_t height = u32(12), width = u32(16);
    if (!width || !height || width > 16384 || height > 16384) return false;

    char fourcc[5] = {0, 0, 0, 0, 0};
    memcpy(fourcc, data + 84, 4);
    const uint8_t* body = data + 128;
    size_t bodyLen = len - 128;
    int w = (int)width, h = (int)height;

    if (!memcmp(fourcc, "ETC ", 4) || !memcmp(fourcc, "ETC1", 4))
        return decodeEtc(body, bodyLen, w, h, false, out);
    if (!memcmp(fourcc, "DXT1", 4)) return decodeDxt(body, bodyLen, w, h, 1, out);
    if (!memcmp(fourcc, "DXT3", 4)) return decodeDxt(body, bodyLen, w, h, 3, out);
    if (!memcmp(fourcc, "DXT5", 4)) return decodeDxt(body, bodyLen, w, h, 5, out);
    if (!memcmp(fourcc, "ATC ", 4) || !memcmp(fourcc, "ATCI", 4) ||
        !memcmp(fourcc, "ATCA", 4))
        return false;                      // ATITC: no decoder, say so upstream

    if (fourcc[0] == 0 && fourcc[1] == 0 && fourcc[2] == 0 && fourcc[3] == 0) {
        // Uncompressed. The header states a bit count and one mask per
        // channel, so the layout is read rather than assumed - 0.7 guessed
        // red in the low nibble and got the Koenigsegg badge sheet in magenta
        // and cyan, because Real Racing 3 puts red in the high nibble
        // (R 0xF000, G 0x0F00, B 0x00F0, A 0x000F).
        uint32_t bits = u32(88);
        uint32_t mask[4] = { u32(92), u32(96), u32(100), u32(104) };
        if (bits == 0) bits = 16;
        size_t pixels = (size_t)w * h;
        size_t bytesPer = bits / 8;
        if (bytesPer < 2 || bytesPer > 4) return false;
        if (bodyLen < pixels * bytesPer) return false;
        if (!mask[0] && !mask[1] && !mask[2]) {         // nothing stated
            mask[0] = 0xF000; mask[1] = 0x0F00; mask[2] = 0x00F0; mask[3] = 0x000F;
        }

        // one shift and one scale per channel, worked out from its mask
        int shift[4] = {0, 0, 0, 0};
        uint32_t span[4] = {0, 0, 0, 0};
        for (int c = 0; c < 4; ++c) {
            if (!mask[c]) continue;
            while (!((mask[c] >> shift[c]) & 1)) ++shift[c];
            span[c] = mask[c] >> shift[c];
        }

        out.width = w; out.height = h; out.channels = 4;
        out.pixels.resize(pixels * 4);
        for (size_t i = 0; i < pixels; ++i) {
            uint32_t v = 0;
            for (size_t k = 0; k < bytesPer; ++k)
                v |= (uint32_t)body[i * bytesPer + k] << (8 * k);
            for (int c = 0; c < 4; ++c) {
                if (!span[c]) { out.pixels[i * 4 + c] = (c == 3) ? 255 : 0; continue; }
                uint32_t raw = (v & mask[c]) >> shift[c];
                out.pixels[i * 4 + c] = (uint8_t)((raw * 255 + span[c] / 2) / span[c]);
            }
        }
        return true;
    }
    return false;
}

bool decodeBlockCompressed(const uint8_t* data, size_t len, int w, int h,
                           const std::string& hint, Image& out) {
    size_t pixels = (size_t)w * h;
    bool fourBpp = len >= pixels / 2 && len < pixels * 3 / 4;
    bool eightBpp = len >= pixels && len < pixels * 2;
    bool preferEtc = hint.find("etc") != std::string::npos;
    bool preferPvr = hint.find("pvr") != std::string::npos;

    std::vector<Image> tries;
    if (fourBpp) {
        // PVRTC first: it is what this game ships, and at 4bpp its block data
        // is the same size as ETC1, so only the decode tells them apart
        Image p, a, b;
        if (decodePvrtc(data, len, w, h, false, p)) tries.push_back(std::move(p));
        if (preferPvr && !tries.empty()) { out = tries[0]; return true; }
        size_t etcIndex = tries.size();
        if (decodeEtc(data, len, w, h, false, a)) tries.push_back(std::move(a));
        if (decodeDxt(data, len, w, h, 1, b)) tries.push_back(std::move(b));
        if (preferEtc && etcIndex < tries.size()) { out = tries[etcIndex]; return true; }
    } else if (eightBpp) {
        Image a, b;
        if (decodeEtc(data, len, w, h, true, a)) tries.push_back(std::move(a));
        if (decodeDxt(data, len, w, h, 5, b)) tries.push_back(std::move(b));
        if (preferEtc && !tries.empty()) { out = tries[0]; return true; }
    }
    if (tries.empty()) return false;

    // no hint: keep whichever decode looks least like noise
    size_t best = 0;
    double bestScore = noiseScore(tries[0]);
    for (size_t i = 1; i < tries.size(); ++i) {
        double s = noiseScore(tries[i]);
        if (s < bestScore) { bestScore = s; best = i; }
    }
    out = std::move(tries[best]);
    return true;
}

std::vector<std::string> textureRefCandidates(const std::string& ref) {
    std::string p = ref;
    for (char& c : p) if (c == '\\') c = '/';
    while (p.compare(0, 3, "../") == 0) p.erase(0, 3);
    while (p.compare(0, 2, "./") == 0) p.erase(0, 2);
    std::vector<std::string> out;
    if (p.empty()) return out;
    out.push_back(p);
    std::string ext = extensionOf(p);
    if (ext == "m3g" || ext == "png" || ext == "pvr" || ext == "tga") {
        std::string stem = stripExtension(p);
        for (const char* e : { "sba", "pvr", "png", "dds", "ktx" })
            if (ext != e) out.push_back(stem + "." + e);
    }
    // Real Racing 1's cut-scene cars name a texture of their own that the
    // game does not ship: car_sedan_cutscene_ext_01 is car_sedan_ext_01
    for (const char* cut : { "_cutscene_closeup", "_cutscene" }) {
        size_t at = p.find(cut);
        if (at == std::string::npos) continue;
        std::string q = p.substr(0, at) + p.substr(at + strlen(cut));
        for (const std::string& c : textureRefCandidates(q)) out.push_back(c);
        break;
    }
    return out;
}

std::string etcAlphaCompanion(const std::string& path) {
    std::string ext = extensionOf(path);
    std::string stem = stripExtension(path);
    if (stem.size() > 9 && stem.compare(stem.size() - 9, 9, "_ETCAlpha") == 0) return std::string();
    return stem + "_ETCAlpha" + (ext.empty() ? std::string() : "." + ext);
}

bool decodePvrtcBlocks(const uint8_t* data, size_t len, int w, int h, bool twoBpp, Image& out) {
    return decodePvrtc(data, len, w, h, twoBpp, out);
}

std::vector<std::string> alphaCompanions(const std::string& path) {
    std::vector<std::string> out;
    std::string nfsEdge = etcAlphaCompanion(path);
    if (!nfsEdge.empty()) out.push_back(nfsEdge);
    // Real Racing 3: org_brandshatch_tree_line_1.etc.dds.z carries its
    // see-through in org_brandshatch_tree_line_1_alpha.etc.dds.z (trees,
    // the sky ring, fences) - the colour file itself has no alpha
    std::string leaf = baseName(path);
    std::string dir = path.substr(0, path.size() - leaf.size());
    size_t dot = leaf.find('.');
    if (dot != std::string::npos && dot > 0) {
        std::string stem = leaf.substr(0, dot), ext = leaf.substr(dot);
        if (stem.size() <= 6 || stem.compare(stem.size() - 6, 6, "_alpha") != 0)
            out.push_back(dir + stem + "_alpha" + ext);
    }
    return out;
}

bool applyEtcAlpha(Image& color, const Image& alpha) {
    if (color.width <= 0 || alpha.width <= 0 || alpha.pixels.empty()) return false;
    Image rgba = toRgba(color);
    Image a = toRgba(alpha);
    if (a.width != rgba.width || a.height != rgba.height) a = resizeRgba(a, rgba.width, rgba.height);
    if (a.pixels.size() != rgba.pixels.size()) return false;
    for (size_t i = 0; i + 3 < rgba.pixels.size(); i += 4) rgba.pixels[i + 3] = a.pixels[i];
    color = std::move(rgba);
    return true;
}

} // namespace nfsnl
