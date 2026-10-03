// nfsnl_decode.cpp - reading ordinary picture files: PNG and JPEG
//
// Used for the game's own PNG payloads and for pictures a modder imports to
// replace a texture. Both decoders are written here so the tool needs no
// image library; on Windows, decodeImageFile() also asks the system's image
// codecs (WIC), which read progressive JPEGs, GIF, BMP and TIFF as well.
#include "nfsnl.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincodec.h>
#endif

namespace nfsnl {

// ================================================================ PNG
//
// Every PNG form: grey, grey+alpha, RGB, RGBA, palette; 1, 2, 4, 8 and 16
// bits; Adam7 interlacing; tRNS transparency. Output is 8-bit grey, RGB or
// RGBA.

bool decodePng(const uint8_t* data, size_t len, Image& out) {
    static const uint8_t sig[8] = {0x89,'P','N','G','\r','\n',0x1a,'\n'};
    if (len < 8 || memcmp(data, sig, 8) != 0) return false;
    auto be32 = [&](size_t p) {
        return ((uint32_t)data[p] << 24) | ((uint32_t)data[p + 1] << 16) |
               ((uint32_t)data[p + 2] << 8) | data[p + 3];
    };
    size_t p = 8;
    uint32_t w = 0, h = 0;
    int depth = 0, type = 0, interlace = 0;
    Bytes idat, palette, trns;
    bool cgbi = false;            // Apple's iPhone PNG: raw deflate, BGRA, premultiplied
    while (p + 8 <= len) {
        uint32_t clen = be32(p);
        const char* t = (const char*)data + p + 4;
        size_t body = p + 8;
        if (body + clen > len) break;
        if (!memcmp(t, "CgBI", 4)) {
            cgbi = true;
        } else if (!memcmp(t, "IHDR", 4) && clen >= 13) {
            w = be32(body); h = be32(body + 4);
            depth = data[body + 8]; type = data[body + 9]; interlace = data[body + 12];
        } else if (!memcmp(t, "PLTE", 4)) {
            palette.assign(data + body, data + body + clen);
        } else if (!memcmp(t, "tRNS", 4)) {
            trns.assign(data + body, data + body + clen);
        } else if (!memcmp(t, "IDAT", 4)) {
            idat.insert(idat.end(), data + body, data + body + clen);
        } else if (!memcmp(t, "IEND", 4)) {
            break;
        }
        p = body + clen + 4;
    }
    if (!w || !h || w > 16384 || h > 16384 || idat.empty()) return false;
    int samples;
    switch (type) {
        case 0: samples = 1; break;
        case 2: samples = 3; break;
        case 3: samples = 1; break;
        case 4: samples = 2; break;
        case 6: samples = 4; break;
        default: return false;
    }
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) return false;
    if ((type == 2 || type == 4 || type == 6) && depth < 8) return false;
    if (type == 3 && (depth > 8 || palette.empty())) return false;

    Bytes raw;
    if (cgbi ? !inflateRaw(idat.data(), idat.size(), raw) && !inflateZlib(idat.data(), idat.size(), raw)
             : !inflateZlib(idat.data(), idat.size(), raw))
        return false;

    int bitsPerPixel = samples * depth;
    int filterBpp = std::max(1, bitsPerPixel / 8);
    // the 16-bit samples, one per channel, before conversion
    std::vector<uint16_t> px((size_t)w * h * samples, 0);

    auto unfilter = [&](size_t& at, uint32_t pw, uint32_t ph,
                        const std::function<void(uint32_t, uint32_t, const uint8_t*)>& row) -> bool {
        if (!pw || !ph) return true;
        size_t stride = ((size_t)pw * bitsPerPixel + 7) / 8;
        Bytes prev(stride, 0), cur(stride);
        for (uint32_t y = 0; y < ph; ++y) {
            if (at + 1 + stride > raw.size()) return false;
            uint8_t f = raw[at];
            const uint8_t* line = raw.data() + at + 1;
            for (size_t x = 0; x < stride; ++x) {
                int a = x >= (size_t)filterBpp ? cur[x - filterBpp] : 0;
                int b = prev[x];
                int c = x >= (size_t)filterBpp ? prev[x - filterBpp] : 0;
                int v = line[x];
                switch (f) {
                    case 0: break;
                    case 1: v += a; break;
                    case 2: v += b; break;
                    case 3: v += (a + b) >> 1; break;
                    case 4: {
                        int pp = a + b - c, pa = std::abs(pp - a), pb = std::abs(pp - b), pc = std::abs(pp - c);
                        v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                        break;
                    }
                    default: return false;
                }
                cur[x] = (uint8_t)v;
            }
            row(y, pw, cur.data());
            prev.swap(cur);
            at += 1 + stride;
        }
        return true;
    };
    // one sample of a packed row
    auto sampleAt = [&](const uint8_t* row, uint32_t x, int c) -> uint16_t {
        if (depth == 16) {
            size_t o = ((size_t)x * samples + c) * 2;
            return (uint16_t)((row[o] << 8) | row[o + 1]);
        }
        if (depth == 8) return row[(size_t)x * samples + c];
        size_t bit = (size_t)x * depth;          // depth < 8: one sample per pixel
        int shift = 8 - depth - (int)(bit & 7);
        return (uint16_t)((row[bit >> 3] >> shift) & ((1 << depth) - 1));
    };

    size_t at = 0;
    if (!interlace) {
        if (!unfilter(at, w, h, [&](uint32_t y, uint32_t pw, const uint8_t* row) {
                for (uint32_t x = 0; x < pw; ++x)
                    for (int c = 0; c < samples; ++c)
                        px[((size_t)y * w + x) * samples + c] = sampleAt(row, x, c);
            }))
            return false;
    } else {
        static const int sx[7] = {0, 4, 0, 2, 0, 1, 0}, sy[7] = {0, 0, 4, 0, 2, 0, 1};
        static const int dx[7] = {8, 8, 4, 4, 2, 2, 1}, dy[7] = {8, 8, 8, 4, 4, 2, 2};
        for (int pass = 0; pass < 7; ++pass) {
            uint32_t pw = (w > (uint32_t)sx[pass]) ? (w - sx[pass] + dx[pass] - 1) / dx[pass] : 0;
            uint32_t ph = (h > (uint32_t)sy[pass]) ? (h - sy[pass] + dy[pass] - 1) / dy[pass] : 0;
            if (!unfilter(at, pw, ph, [&](uint32_t y, uint32_t pwid, const uint8_t* row) {
                    uint32_t yy = sy[pass] + y * dy[pass];
                    for (uint32_t x = 0; x < pwid; ++x) {
                        uint32_t xx = sx[pass] + x * dx[pass];
                        for (int c = 0; c < samples; ++c)
                            px[((size_t)yy * w + xx) * samples + c] = sampleAt(row, x, c);
                    }
                }))
                return false;
        }
    }

    // to 8 bits: 16-bit keeps its high byte, low depths are scaled up
    auto to8 = [&](uint16_t v) -> uint8_t {
        if (depth == 16) return (uint8_t)(v >> 8);
        if (depth == 8) return (uint8_t)v;
        return (uint8_t)(v * 255 / ((1 << depth) - 1));
    };
    size_t n = (size_t)w * h;
    out.width = (int)w;
    out.height = (int)h;
    if (type == 3) {
        out.channels = trns.empty() ? 3 : 4;
        out.pixels.assign(n * out.channels, 255);
        for (size_t i = 0; i < n; ++i) {
            size_t idx = px[i];
            uint8_t* o = &out.pixels[i * out.channels];
            if (idx * 3 + 2 < palette.size()) {
                o[0] = palette[idx * 3]; o[1] = palette[idx * 3 + 1]; o[2] = palette[idx * 3 + 2];
            } else {
                o[0] = o[1] = o[2] = 0;
            }
            if (out.channels == 4) o[3] = idx < trns.size() ? trns[idx] : 255;
        }
        return true;
    }
    // a tRNS on grey or RGB names one colour as fully transparent
    bool keyed = (type == 0 && trns.size() >= 2) || (type == 2 && trns.size() >= 6);
    uint16_t key[3] = {0, 0, 0};
    if (keyed)
        for (int c = 0; c < (type == 0 ? 1 : 3); ++c)
            key[c] = (uint16_t)((trns[c * 2] << 8) | trns[c * 2 + 1]);
    int outCh = type == 0 ? (keyed ? 4 : 1) : type == 4 ? 4 : type == 2 ? (keyed ? 4 : 3) : 4;
    out.channels = outCh;
    out.pixels.assign(n * outCh, 255);
    for (size_t i = 0; i < n; ++i) {
        const uint16_t* s = &px[i * samples];
        uint8_t* o = &out.pixels[i * outCh];
        switch (type) {
            case 0:
                if (outCh == 1) o[0] = to8(s[0]);
                else { o[0] = o[1] = o[2] = to8(s[0]); o[3] = s[0] == key[0] ? 0 : 255; }
                break;
            case 4: o[0] = o[1] = o[2] = to8(s[0]); o[3] = to8(s[1]); break;
            case 2:
                o[0] = to8(s[0]); o[1] = to8(s[1]); o[2] = to8(s[2]);
                if (outCh == 4) o[3] = (s[0] == key[0] && s[1] == key[1] && s[2] == key[2]) ? 0 : 255;
                break;
            case 6: for (int c = 0; c < 4; ++c) o[c] = to8(s[c]); break;
        }
    }
    // CgBI: blue and red swapped, colour multiplied by alpha - undone
    if (cgbi && out.channels >= 3) {
        for (size_t i = 0; i < n; ++i) {
            uint8_t* o = &out.pixels[i * out.channels];
            std::swap(o[0], o[2]);
            if (out.channels == 4 && o[3] > 0 && o[3] < 255)
                for (int c = 0; c < 3; ++c) o[c] = (uint8_t)std::min(255, o[c] * 255 / o[3]);
        }
    }
    return true;
}

// ================================================================ JPEG
//
// Baseline and extended sequential JPEG (SOF0 / SOF1), Huffman-coded, 8-bit,
// one to four components with any sampling factors, restart markers. The
// output is RGB (or grey). Progressive files are left to WIC on Windows.

namespace {

struct JHuff {
    uint8_t lengths[17] = {0};
    uint8_t values[256] = {0};
    int maxcode[18], valptr[17], mincode[17];
    bool ready = false;
    void build() {
        int code = 0, k = 0;
        for (int l = 1; l <= 16; ++l) {
            valptr[l] = k;
            mincode[l] = code;
            code += lengths[l];
            k += lengths[l];
            maxcode[l] = lengths[l] ? code - 1 : -1;
            code <<= 1;
        }
        maxcode[17] = 0x7fffffff;
        ready = true;
    }
};

struct JReader {
    const uint8_t* d;
    size_t n, p;
    uint32_t acc = 0;
    int bits = 0;
    bool hitMarker = false;
    JReader(const uint8_t* data, size_t len, size_t pos) : d(data), n(len), p(pos) {}
    int bit() {
        if (bits == 0) {
            uint8_t b = 0;
            if (!hitMarker && p < n) {
                b = d[p];
                if (b == 0xFF) {
                    uint8_t nx = p + 1 < n ? d[p + 1] : 0;
                    if (nx == 0x00) { p += 2; }
                    else { hitMarker = true; b = 0; }
                } else {
                    ++p;
                }
            }
            acc = b;
            bits = 8;
        }
        --bits;
        return (acc >> bits) & 1;
    }
    int receive(int s) { int v = 0; for (int i = 0; i < s; ++i) v = (v << 1) | bit(); return v; }
    int decode(const JHuff& h) {
        int code = bit();
        for (int l = 1; l <= 16; ++l) {
            if (h.maxcode[l] >= 0 && code <= h.maxcode[l]) {
                int idx = h.valptr[l] + code - h.mincode[l];
                return idx >= 0 && idx < 256 ? h.values[idx] : 0;
            }
            code = (code << 1) | bit();
        }
        return -1;
    }
    // skip to the restart marker and past it
    void restart() {
        bits = 0;
        hitMarker = false;
        while (p + 1 < n && !(d[p] == 0xFF && d[p + 1] >= 0xD0 && d[p + 1] <= 0xD7)) ++p;
        if (p + 1 < n) p += 2;
    }
};

inline int extend(int v, int s) { return (s && v < (1 << (s - 1))) ? v - (1 << s) + 1 : v; }

const uint8_t kZigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };

void idct8x8(const float in[64], uint8_t* out, int stride) {
    // built once; a function-local static is thread-safe, and the viewer
    // decodes textures on several threads
    struct Table { float v[8][8]; Table() {
        for (int x = 0; x < 8; ++x)
            for (int u = 0; u < 8; ++u)
                v[x][u] = (float)((u == 0 ? std::sqrt(0.125) : 0.5) *
                                  std::cos((2 * x + 1) * u * 3.14159265358979 / 16));
    } };
    static const Table table;
    const auto& c = table.v;
    float tmp[64];
    for (int y = 0; y < 8; ++y)            // rows: frequency u -> x
        for (int x = 0; x < 8; ++x) {
            float s = 0;
            for (int u = 0; u < 8; ++u) s += c[x][u] * in[y * 8 + u];
            tmp[y * 8 + x] = s;
        }
    for (int x = 0; x < 8; ++x)            // columns: frequency v -> y
        for (int y = 0; y < 8; ++y) {
            float s = 0;
            for (int v = 0; v < 8; ++v) s += c[y][v] * tmp[v * 8 + x];
            int val = (int)std::lround(s + 128.0f);
            out[y * stride + x] = (uint8_t)std::min(255, std::max(0, val));
        }
}

} // namespace

bool decodeJpeg(const uint8_t* data, size_t len, Image& out) {
    if (len < 4 || data[0] != 0xFF || data[1] != 0xD8) return false;
    uint16_t qt[4][64];
    memset(qt, 0, sizeof(qt));
    JHuff dcT[4], acT[4];
    struct Comp { int id, h, v, tq, td = 0, ta = 0; int bw = 0, bh = 0; Bytes plane; int pred = 0; };
    std::vector<Comp> comps;
    int W = 0, H = 0, hmax = 1, vmax = 1, restartInterval = 0;
    bool frame = false, adobe = false;
    int adobeTransform = -1;
    size_t p = 2;
    while (p + 4 <= len) {
        if (data[p] != 0xFF) { ++p; continue; }
        uint8_t m = data[p + 1];
        if (m == 0xFF) { ++p; continue; }
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { p += 2; continue; }
        if (m == 0xD9) break;
        size_t seg = ((size_t)data[p + 2] << 8) | data[p + 3];
        size_t body = p + 4, end = p + 2 + seg;
        if (seg < 2 || end > len) return false;
        if (m == 0xDB) {                                  // quantisation tables
            size_t q = body;
            while (q < end) {
                int pq = data[q] >> 4, tq = data[q] & 15;
                ++q;
                if (tq > 3) return false;
                for (int k = 0; k < 64; ++k) {
                    if (pq) { if (q + 2 > end) return false; qt[tq][kZigzag[k]] = (uint16_t)((data[q] << 8) | data[q + 1]); q += 2; }
                    else { if (q >= end) return false; qt[tq][kZigzag[k]] = data[q++]; }
                }
            }
        } else if (m == 0xC4) {                           // Huffman tables
            size_t q = body;
            while (q + 17 <= end) {
                int tc = data[q] >> 4, th = data[q] & 15;
                if (th > 3 || tc > 1) return false;
                JHuff& t = tc ? acT[th] : dcT[th];
                int total = 0;
                for (int l = 1; l <= 16; ++l) { t.lengths[l] = data[q + l]; total += t.lengths[l]; }
                q += 17;
                if (total > 256 || q + total > end) return false;
                memcpy(t.values, data + q, total);
                q += total;
                t.build();
            }
        } else if (m == 0xC0 || m == 0xC1) {              // baseline / extended frame
            if (seg < 8 || data[body] != 8) return false;
            H = (data[body + 1] << 8) | data[body + 2];
            W = (data[body + 3] << 8) | data[body + 4];
            int nc = data[body + 5];
            if (W <= 0 || H <= 0 || W > 16384 || H > 16384 || nc < 1 || nc > 4) return false;
            if (seg < (size_t)(8 + nc * 3)) return false;
            for (int i = 0; i < nc; ++i) {
                Comp c;
                c.id = data[body + 6 + i * 3];
                c.h = data[body + 7 + i * 3] >> 4;
                c.v = data[body + 7 + i * 3] & 15;
                c.tq = data[body + 8 + i * 3] & 3;
                if (c.h < 1 || c.h > 4 || c.v < 1 || c.v > 4) return false;
                hmax = std::max(hmax, c.h);
                vmax = std::max(vmax, c.v);
                comps.push_back(std::move(c));
            }
            int mcux = (W + 8 * hmax - 1) / (8 * hmax), mcuy = (H + 8 * vmax - 1) / (8 * vmax);
            for (Comp& c : comps) {
                c.bw = mcux * c.h;
                c.bh = mcuy * c.v;
                c.plane.assign((size_t)c.bw * 8 * c.bh * 8, 0);
            }
            frame = true;
        } else if (m >= 0xC2 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            return false;                                 // progressive, lossless, arithmetic
        } else if (m == 0xDD) {
            if (seg >= 4) restartInterval = (data[body] << 8) | data[body + 1];
        } else if (m == 0xEE) {
            if (seg >= 14 && !memcmp(data + body, "Adobe", 5)) { adobe = true; adobeTransform = data[body + 11]; }
        } else if (m == 0xDA) {                           // a scan
            if (!frame) return false;
            int ns = data[body];
            if (ns < 1 || ns > 4 || seg < (size_t)(6 + ns * 2)) return false;
            std::vector<Comp*> sc;
            for (int i = 0; i < ns; ++i) {
                int cid = data[body + 1 + i * 2];
                Comp* found = nullptr;
                for (Comp& c : comps) if (c.id == cid) found = &c;
                if (!found) return false;
                found->td = data[body + 2 + i * 2] >> 4;
                found->ta = data[body + 2 + i * 2] & 15;
                if (found->td > 3 || found->ta > 3 || !dcT[found->td].ready || !acT[found->ta].ready) return false;
                found->pred = 0;
                sc.push_back(found);
            }
            JReader r(data, len, end);
            int mcux, mcuy;
            if (ns == 1) {                                // non-interleaved: one block per MCU
                Comp& c = *sc[0];
                mcux = (W * c.h / hmax + 7) / 8;
                mcuy = (H * c.v / vmax + 7) / 8;
            } else {
                mcux = (W + 8 * hmax - 1) / (8 * hmax);
                mcuy = (H + 8 * vmax - 1) / (8 * vmax);
            }
            int todo = restartInterval;
            float blk[64];
            auto decodeBlock = [&](Comp& c, int bx, int by) -> bool {
                int coef[64] = {0};
                int t = r.decode(dcT[c.td]);
                if (t < 0 || t > 16) return false;
                int diff = t ? extend(r.receive(t), t) : 0;
                c.pred += diff;
                coef[0] = c.pred;
                for (int k = 1; k < 64;) {
                    int rs = r.decode(acT[c.ta]);
                    if (rs < 0) return false;
                    int run = rs >> 4, s = rs & 15;
                    if (!s) { if (run == 15) { k += 16; continue; } break; }
                    k += run;
                    if (k > 63) return false;
                    coef[kZigzag[k]] = extend(r.receive(s), s);
                    ++k;
                }
                for (int k = 0; k < 64; ++k) blk[k] = (float)(coef[k] * qt[c.tq][k]);
                if (bx >= c.bw || by >= c.bh) return true;
                idct8x8(blk, c.plane.data() + ((size_t)by * 8) * (c.bw * 8) + bx * 8, c.bw * 8);
                return true;
            };
            for (int my = 0; my < mcuy; ++my) {
                for (int mx = 0; mx < mcux; ++mx) {
                    if (restartInterval) {
                        if (todo == 0) {
                            r.restart();
                            for (Comp* c : sc) c->pred = 0;
                            todo = restartInterval;
                        }
                        --todo;
                    }
                    if (ns == 1) {
                        if (!decodeBlock(*sc[0], mx, my)) return false;
                    } else {
                        for (Comp* c : sc)
                            for (int v = 0; v < c->v; ++v)
                                for (int h = 0; h < c->h; ++h)
                                    if (!decodeBlock(*c, mx * c->h + h, my * c->v + v)) return false;
                    }
                }
            }
            // carry on after the entropy-coded data
            size_t q = r.p;
            while (q + 1 < len && !(data[q] == 0xFF && data[q + 1] != 0 &&
                                     !(data[q + 1] >= 0xD0 && data[q + 1] <= 0xD7)))
                ++q;
            p = q;
            continue;
        }
        p = end;
    }
    if (!frame || comps.empty()) return false;

    // Upsample the subsampled planes with a triangle filter, sample centres
    // lined up the way libjpeg's "fancy" upsampling lines them up; a full
    // resolution plane is read directly.
    auto sampleOf = [&](const Comp& c, int x, int y) -> int {
        int pw = c.bw * 8, ph = c.bh * 8;
        const uint8_t* pl = c.plane.data();
        if (c.h == hmax && c.v == vmax) return pl[(size_t)y * pw + x];
        float fx = (x + 0.5f) * c.h / hmax - 0.5f, fy = (y + 0.5f) * c.v / vmax - 0.5f;
        fx = std::max(0.0f, fx); fy = std::max(0.0f, fy);
        int x0 = (int)fx, y0 = (int)fy;
        int x1 = std::min(x0 + 1, pw - 1), y1 = std::min(y0 + 1, ph - 1);
        float ax = fx - x0, ay = fy - y0;
        float top = pl[(size_t)y0 * pw + x0] * (1 - ax) + pl[(size_t)y0 * pw + x1] * ax;
        float bot = pl[(size_t)y1 * pw + x0] * (1 - ax) + pl[(size_t)y1 * pw + x1] * ax;
        return (int)(top * (1 - ay) + bot * ay + 0.5f);
    };
    out.width = W;
    out.height = H;
    if (comps.size() == 1) {
        out.channels = 1;
        out.pixels.resize((size_t)W * H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) out.pixels[(size_t)y * W + x] = (uint8_t)sampleOf(comps[0], x, y);
        return true;
    }
    if (comps.size() == 2) return false;
    out.channels = 3;
    out.pixels.resize((size_t)W * H * 3);
    bool ycc = comps.size() == 3 ? !(adobe && adobeTransform == 0) : (adobe && adobeTransform == 2);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float a = (float)sampleOf(comps[0], x, y), b = (float)sampleOf(comps[1], x, y),
                  c = (float)sampleOf(comps[2], x, y);
            float R, G, B;
            if (ycc) {
                R = a + 1.402f * (c - 128);
                G = a - 0.344136f * (b - 128) - 0.714136f * (c - 128);
                B = a + 1.772f * (b - 128);
            } else {
                R = a; G = b; B = c;
            }
            if (comps.size() == 4) {                     // CMYK (Adobe stores it inverted)
                float k = (float)sampleOf(comps[3], x, y);
                R = R * k / 255.0f; G = G * k / 255.0f; B = B * k / 255.0f;
            }
            uint8_t* o = &out.pixels[((size_t)y * W + x) * 3];
            o[0] = (uint8_t)std::min(255.0f, std::max(0.0f, R + 0.5f));
            o[1] = (uint8_t)std::min(255.0f, std::max(0.0f, G + 0.5f));
            o[2] = (uint8_t)std::min(255.0f, std::max(0.0f, B + 0.5f));
        }
    return true;
}

// ================================================================ any picture

#ifdef _WIN32
namespace {
// The three identifiers WIC needs, spelled out so no import library has to
// provide them.
const GUID kClsidWicFactory = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
const GUID kIidWicFactory = {0xec5ec8a9, 0xc395, 0x4314, {0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70}};
const GUID kWicRgba32 = {0xf5c7ad2d, 0x6a8d, 0x43dd, {0xa7, 0xa8, 0xa2, 0x99, 0x35, 0x26, 0x1a, 0xe9}};

// Windows Imaging Component: every format Windows itself can open.
bool decodeWithWic(const uint8_t* data, size_t len, Image& out) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    IWICImagingFactory* factory = nullptr;
    if (SUCCEEDED(CoCreateInstance(kClsidWicFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   kIidWicFactory, (void**)&factory)) && factory) {
        IWICStream* stream = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frameDec = nullptr;
        IWICFormatConverter* conv = nullptr;
        if (SUCCEEDED(factory->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromMemory((BYTE*)data, (DWORD)len)) &&
            SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr,
                                                       WICDecodeMetadataCacheOnDemand, &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frameDec)) &&
            SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(frameDec, kWicRgba32,
                                       WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeCustom))) {
            UINT w = 0, h = 0;
            conv->GetSize(&w, &h);
            if (w && h && w <= 16384 && h <= 16384) {
                out.width = (int)w;
                out.height = (int)h;
                out.channels = 4;
                out.pixels.resize((size_t)w * h * 4);
                ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)out.pixels.size(),
                                                out.pixels.data()));
            }
        }
        if (conv) conv->Release();
        if (frameDec) frameDec->Release();
        if (decoder) decoder->Release();
        if (stream) stream->Release();
        factory->Release();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    if (!ok) out = Image();
    return ok;
}
} // namespace
#endif

bool decodeImageFile(const uint8_t* data, size_t len, Image& out) {
    out = Image();
    if (decodePng(data, len, out)) return true;
    if (decodeJpeg(data, len, out)) return true;
#ifdef _WIN32
    if (decodeWithWic(data, len, out)) return true;
#endif
    // the tool's own formats as well, so an exported .dds or .pvr imports too
    if (decodeTextureFile(data, len, out)) return true;
    return false;
}

} // namespace nfsnl
