// nfsnl_encode.cpp - putting a picture back into a game texture
//
// "Import PNG/JPG": the picture is resized to the texture's own size, turned
// the game's way up, encoded in the codec the texture already uses (ETC1,
// DXT, ATC, raw RGB/RGBA, PNG, JPEG) for every mip level it has, and written
// back into the same container - .sba (No Limits' SBIN 4, Most Wanted's SBIN
// 3), .pvr (version 2 and 3), .dds. Nothing about the texture's description
// changes, so the game reads it exactly as it read the original.
//
// The one codec that cannot be written is PVRTC. A .pvr or .dds holding it
// is switched to uncompressed RGBA, which the header can say; inside an .sba
// the codec is fixed by the file's own description, so it is reported.
#include "nfsnl.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace nfsnl {

// ================================================================ pixels

Image toRgba(const Image& src) {
    Image o;
    if (!src.ok()) return o;
    o.width = src.width; o.height = src.height; o.channels = 4;
    size_t n = (size_t)src.width * src.height;
    o.pixels.resize(n * 4);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* s = &src.pixels[i * src.channels];
        uint8_t* d = &o.pixels[i * 4];
        switch (src.channels) {
            case 1: d[0] = d[1] = d[2] = s[0]; d[3] = 255; break;
            case 2: d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; break;
            case 3: d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; break;
            default: memcpy(d, s, 4); break;
        }
    }
    return o;
}

namespace {

// one axis of a resize: area averaging when shrinking, linear when growing
void resampleAxis(const float* src, int n, int stride, float* dst, int m, int dstride, int ch) {
    if (m == n) {
        for (int i = 0; i < n; ++i)
            for (int c = 0; c < ch; ++c) dst[i * dstride + c] = src[i * stride + c];
        return;
    }
    if (m < n) {
        double scale = (double)n / m;
        for (int i = 0; i < m; ++i) {
            double a = i * scale, b = a + scale;
            for (int c = 0; c < ch; ++c) {
                double sum = 0;
                for (int k = (int)a; k < (int)std::ceil(b) && k < n; ++k) {
                    double lo = std::max<double>(a, k), hi = std::min<double>(b, k + 1);
                    if (hi > lo) sum += src[k * stride + c] * (hi - lo);
                }
                dst[i * dstride + c] = (float)(sum / scale);
            }
        }
        return;
    }
    double scale = (double)n / m;
    for (int i = 0; i < m; ++i) {
        double f = (i + 0.5) * scale - 0.5;
        if (f < 0) f = 0;
        int k0 = (int)f, k1 = std::min(k0 + 1, n - 1);
        double t = f - k0;
        for (int c = 0; c < ch; ++c)
            dst[i * dstride + c] = (float)(src[k0 * stride + c] * (1 - t) + src[k1 * stride + c] * t);
    }
}

} // namespace

// the largest texture anything here will build: 8192 x 8192
static const size_t kMaxPixels = (size_t)8192 * 8192;

Image resizeRgba(const Image& in, int w, int h) {
    if (!in.ok() || w <= 0 || h <= 0 || w > 16384 || h > 16384 || (size_t)w * h > kMaxPixels)
        return Image();
    Image src = in.channels == 4 ? in : toRgba(in);
    if (src.width == w && src.height == h) return src;
    // colour is averaged weighted by alpha, so transparent pixels do not
    // bleed their (usually black) colour into the edges
    int sw = src.width, sh = src.height;
    std::vector<float> a((size_t)sw * sh * 4);
    for (size_t i = 0; i < (size_t)sw * sh; ++i) {
        float al = src.pixels[i * 4 + 3] / 255.0f;
        for (int c = 0; c < 3; ++c) a[i * 4 + c] = src.pixels[i * 4 + c] * al;
        a[i * 4 + 3] = al;
    }
    std::vector<float> mid((size_t)w * sh * 4), out((size_t)w * h * 4);
    for (int y = 0; y < sh; ++y)
        resampleAxis(&a[(size_t)y * sw * 4], sw, 4, &mid[(size_t)y * w * 4], w, 4, 4);
    for (int x = 0; x < w; ++x)
        resampleAxis(&mid[(size_t)x * 4], sh, w * 4, &out[(size_t)x * 4], h, w * 4, 4);
    Image o;
    o.width = w; o.height = h; o.channels = 4;
    o.pixels.resize((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        float al = out[i * 4 + 3];
        for (int c = 0; c < 3; ++c) {
            float v = al > 1e-6f ? out[i * 4 + c] / al : 0.0f;
            o.pixels[i * 4 + c] = (uint8_t)std::min(255.0f, std::max(0.0f, v + 0.5f));
        }
        o.pixels[i * 4 + 3] = (uint8_t)std::min(255.0f, std::max(0.0f, al * 255.0f + 0.5f));
    }
    return o;
}

namespace {

// the pixel at (x, y), repeating the edge for the part of a 4x4 block that
// hangs past it
inline const uint8_t* texel(const Image& img, int x, int y) {
    x = std::min(x, img.width - 1);
    y = std::min(y, img.height - 1);
    return &img.pixels[((size_t)y * img.width + x) * 4];
}

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ================================================================ ETC1
//
// Each 4x4 block is two halves (side by side, or one above the other), each
// with a base colour and one of eight intensity tables. Every split, both
// base colour encodings (4:4:4 each, or 5:5:5 plus a 3-bit difference) and
// every table is tried; the least squared error wins.

const int kMod[8][4] = {
    {2, 8, -2, -8},     {5, 17, -5, -17},    {9, 29, -9, -29},    {13, 42, -13, -42},
    {18, 60, -18, -60}, {24, 80, -24, -80},  {33, 106, -33, -106},{47, 183, -47, -183} };

// best table and selectors for 8 pixels around base colour c
int etcHalf(const int px[8][3], const int c[3], int& table, int sel[8]) {
    int best = 0x7fffffff;
    for (int t = 0; t < 8; ++t) {
        int err = 0, s[8];
        for (int i = 0; i < 8 && err < best; ++i) {
            int be = 0x7fffffff;
            for (int m = 0; m < 4; ++m) {
                int e = 0;
                for (int k = 0; k < 3; ++k) {
                    int d = clampi(c[k] + kMod[t][m], 0, 255) - px[i][k];
                    e += d * d;
                }
                if (e < be) { be = e; s[i] = m; }
            }
            err += be;
        }
        if (err < best) { best = err; table = t; memcpy(sel, s, sizeof(s)); }
    }
    return best;
}

void encodeEtc1Block(const Image& img, int bx, int by, uint8_t out[8]) {
    int px[16][3];
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            const uint8_t* p = texel(img, bx * 4 + x, by * 4 + y);
            for (int k = 0; k < 3; ++k) px[y * 4 + x][k] = p[k];
        }
    int bestErr = 0x7fffffff;
    uint8_t best[8] = {0};
    for (int flip = 0; flip < 2; ++flip) {
        // the two halves' pixels, and where each came from
        int half[2][8][3], where[2][8];
        int cnt[2] = {0, 0};
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                int h = flip ? (y >= 2) : (x >= 2);
                memcpy(half[h][cnt[h]], px[y * 4 + x], sizeof(int) * 3);
                where[h][cnt[h]++] = x * 4 + y;          // the block's index order
            }
        float avg[2][3];
        for (int h = 0; h < 2; ++h)
            for (int k = 0; k < 3; ++k) {
                int s = 0;
                for (int i = 0; i < 8; ++i) s += half[h][i][k];
                avg[h][k] = s / 8.0f;
            }
        for (int diff = 0; diff < 2; ++diff) {
            int q[2][3], base[2][3];
            bool ok = true;
            for (int h = 0; h < 2; ++h)
                for (int k = 0; k < 3; ++k) {
                    if (diff) {
                        q[h][k] = clampi((int)std::lround(avg[h][k] * 31 / 255), 0, 31);
                        base[h][k] = (q[h][k] << 3) | (q[h][k] >> 2);
                    } else {
                        q[h][k] = clampi((int)std::lround(avg[h][k] * 15 / 255), 0, 15);
                        base[h][k] = q[h][k] * 17;
                    }
                }
            if (diff) {
                for (int k = 0; k < 3; ++k) {
                    int d = q[1][k] - q[0][k];
                    if (d < -4 || d > 3) ok = false;
                }
            }
            if (!ok) continue;
            int tables[2], sel[2][8];
            int err = etcHalf(half[0], base[0], tables[0], sel[0]);
            if (err >= bestErr) continue;
            err += etcHalf(half[1], base[1], tables[1], sel[1]);
            if (err >= bestErr) continue;
            bestErr = err;
            uint8_t b[8];
            if (diff) {
                for (int k = 0; k < 3; ++k)
                    b[k] = (uint8_t)((q[0][k] << 3) | ((q[1][k] - q[0][k]) & 7));
            } else {
                for (int k = 0; k < 3; ++k)
                    b[k] = (uint8_t)((q[0][k] << 4) | q[1][k]);
            }
            b[3] = (uint8_t)((tables[0] << 5) | (tables[1] << 2) | (diff << 1) | flip);
            uint32_t bits = 0;
            for (int h = 0; h < 2; ++h)
                for (int i = 0; i < 8; ++i) {
                    int idx = where[h][i], m = sel[h][i];
                    if (m & 1) bits |= 1u << idx;
                    if (m & 2) bits |= 1u << (16 + idx);
                }
            b[4] = (uint8_t)(bits >> 24); b[5] = (uint8_t)(bits >> 16);
            b[6] = (uint8_t)(bits >> 8);  b[7] = (uint8_t)bits;
            memcpy(best, b, 8);
        }
    }
    memcpy(out, best, 8);
}

// ================================================================ DXT / ATC

struct Col { int r, g, b; };

// the two end points of a block's colours: the extremes along its main axis
void endPoints(const int px[16][3], float lo[3], float hi[3]) {
    float mean[3] = {0, 0, 0};
    for (int i = 0; i < 16; ++i) for (int k = 0; k < 3; ++k) mean[k] += px[i][k] / 16.0f;
    float cov[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 16; ++i) {
        float d[3] = { px[i][0] - mean[0], px[i][1] - mean[1], px[i][2] - mean[2] };
        cov[0] += d[0] * d[0]; cov[1] += d[0] * d[1]; cov[2] += d[0] * d[2];
        cov[3] += d[1] * d[1]; cov[4] += d[1] * d[2]; cov[5] += d[2] * d[2];
    }
    float axis[3] = {1, 1, 1};
    for (int it = 0; it < 8; ++it) {
        float n[3] = { cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2],
                       cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2],
                       cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2] };
        float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l < 1e-6f) break;
        for (int k = 0; k < 3; ++k) axis[k] = n[k] / l;
    }
    float tmin = 1e9f, tmax = -1e9f;
    for (int i = 0; i < 16; ++i) {
        float t = (px[i][0] - mean[0]) * axis[0] + (px[i][1] - mean[1]) * axis[1] +
                  (px[i][2] - mean[2]) * axis[2];
        tmin = std::min(tmin, t); tmax = std::max(tmax, t);
    }
    for (int k = 0; k < 3; ++k) {
        lo[k] = std::min(255.0f, std::max(0.0f, mean[k] + axis[k] * tmin));
        hi[k] = std::min(255.0f, std::max(0.0f, mean[k] + axis[k] * tmax));
    }
}

inline uint16_t to565(const float c[3]) {
    int r = clampi((int)std::lround(c[0] * 31 / 255), 0, 31);
    int g = clampi((int)std::lround(c[1] * 63 / 255), 0, 63);
    int b = clampi((int)std::lround(c[2] * 31 / 255), 0, 31);
    return (uint16_t)((r << 11) | (g << 5) | b);
}
inline Col from565(uint16_t v) {
    int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return { (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2) };
}
inline uint16_t to555(const float c[3]) {
    int r = clampi((int)std::lround(c[0] * 31 / 255), 0, 31);
    int g = clampi((int)std::lround(c[1] * 31 / 255), 0, 31);
    int b = clampi((int)std::lround(c[2] * 31 / 255), 0, 31);
    return (uint16_t)((r << 10) | (g << 5) | b);
}
inline Col from555(uint16_t v) {
    int r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
    return { (r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2) };
}

uint32_t pickIndices(const int px[16][3], const Col pal[4]) {
    uint32_t bits = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 0, be = 0x7fffffff;
        for (int m = 0; m < 4; ++m) {
            int dr = pal[m].r - px[i][0], dg = pal[m].g - px[i][1], db = pal[m].b - px[i][2];
            int e = dr * dr + dg * dg + db * db;
            if (e < be) { be = e; best = m; }
        }
        bits |= (uint32_t)best << (2 * i);
    }
    return bits;
}

void colourBlock(const int px[16][3], bool atc, uint8_t out[8]) {
    float lo[3], hi[3];
    endPoints(px, lo, hi);
    Col pal[4];
    uint16_t c0, c1;
    if (atc) {
        // method 0: colour 0 is RGB555, colour 1 RGB565, in between 3/8 and 5/8
        c0 = to555(lo);
        c1 = to565(hi);
        Col a = from555(c0), d = from565(c1);
        pal[0] = a;
        pal[1] = { (5 * a.r + 3 * d.r) / 8, (5 * a.g + 3 * d.g) / 8, (5 * a.b + 3 * d.b) / 8 };
        pal[2] = { (3 * a.r + 5 * d.r) / 8, (3 * a.g + 5 * d.g) / 8, (3 * a.b + 5 * d.b) / 8 };
        pal[3] = d;
    } else {
        // four-colour mode needs colour 0 above colour 1
        c0 = to565(hi);
        c1 = to565(lo);
        if (c0 < c1) std::swap(c0, c1);
        if (c0 == c1) {
            out[0] = (uint8_t)c0; out[1] = (uint8_t)(c0 >> 8);
            out[2] = (uint8_t)c1; out[3] = (uint8_t)(c1 >> 8);
            out[4] = out[5] = out[6] = out[7] = 0;
            return;
        }
        Col a = from565(c0), d = from565(c1);
        pal[0] = a;
        pal[1] = d;
        pal[2] = { (2 * a.r + d.r) / 3, (2 * a.g + d.g) / 3, (2 * a.b + d.b) / 3 };
        pal[3] = { (a.r + 2 * d.r) / 3, (a.g + 2 * d.g) / 3, (a.b + 2 * d.b) / 3 };
    }
    uint32_t bits = pickIndices(px, pal);
    out[0] = (uint8_t)c0; out[1] = (uint8_t)(c0 >> 8);
    out[2] = (uint8_t)c1; out[3] = (uint8_t)(c1 >> 8);
    for (int k = 0; k < 4; ++k) out[4 + k] = (uint8_t)(bits >> (8 * k));
}

// 8 bytes of alpha: explicit (4 bits each) or interpolated (DXT5 style)
void alphaBlock(const int alpha[16], bool explicitAlpha, uint8_t out[8]) {
    if (explicitAlpha) {
        uint64_t bits = 0;
        for (int i = 0; i < 16; ++i)
            bits |= (uint64_t)clampi((alpha[i] + 8) / 17, 0, 15) << (4 * i);
        for (int k = 0; k < 8; ++k) out[k] = (uint8_t)(bits >> (8 * k));
        return;
    }
    int a0 = 0, a1 = 255;
    for (int i = 0; i < 16; ++i) { a0 = std::max(a0, alpha[i]); a1 = std::min(a1, alpha[i]); }
    uint64_t bits = 0;
    if (a0 == a1) {
        out[0] = (uint8_t)a0; out[1] = (uint8_t)a1;
    } else {
        int val[8];
        val[0] = a0; val[1] = a1;
        for (int c = 2; c < 8; ++c) val[c] = ((8 - c) * a0 + (c - 1) * a1) / 7;
        for (int i = 0; i < 16; ++i) {
            int best = 0, be = 1 << 30;
            for (int c = 0; c < 8; ++c) {
                int e = std::abs(val[c] - alpha[i]);
                if (e < be) { be = e; best = c; }
            }
            bits |= (uint64_t)best << (3 * i);
        }
        out[0] = (uint8_t)a0; out[1] = (uint8_t)a1;
    }
    for (int k = 0; k < 6; ++k) out[2 + k] = (uint8_t)(bits >> (8 * k));
}

} // namespace

// variant: 1 = DXT1, 3 = DXT3, 5 = DXT5; atc switches to the ATC colour rule
static Bytes encodeBlocks(const Image& img, int variant, bool atc) {
    if (!img.ok() || img.channels != 4) return Bytes();
    int bw = (img.width + 3) / 4, bh = (img.height + 3) / 4;
    size_t bb = variant == 1 ? 8 : 16;
    Bytes out((size_t)bw * bh * bb);
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            int px[16][3], al[16];
            for (int i = 0; i < 16; ++i) {
                const uint8_t* p = texel(img, bx * 4 + (i & 3), by * 4 + (i >> 2));
                px[i][0] = p[0]; px[i][1] = p[1]; px[i][2] = p[2];
                al[i] = p[3];
            }
            uint8_t* o = &out[((size_t)by * bw + bx) * bb];
            if (variant != 1) {
                alphaBlock(al, variant == 3, o);
                colourBlock(px, atc, o + 8);
            } else {
                colourBlock(px, atc, o);
            }
        }
    return out;
}

Bytes encodeEtc1(const Image& rgba) {
    if (!rgba.ok() || rgba.channels != 4) return Bytes();
    int bw = (rgba.width + 3) / 4, bh = (rgba.height + 3) / 4;
    Bytes out((size_t)bw * bh * 8);
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx)
            encodeEtc1Block(rgba, bx, by, &out[((size_t)by * bw + bx) * 8]);
    return out;
}
Bytes encodeDxt(const Image& rgba, int variant) { return encodeBlocks(rgba, variant, false); }
Bytes encodeAtc(const Image& rgba, int variant) { return encodeBlocks(rgba, variant, true); }

// Uncompressed, described by channel masks the way DDS and PVR describe it.
Bytes encodeMasked(const Image& rgba, int bytesPer, const uint32_t mask[4]) {
    if (!rgba.ok() || rgba.channels != 4 || bytesPer < 1 || bytesPer > 4) return Bytes();
    size_t n = (size_t)rgba.width * rgba.height;
    Bytes out(n * bytesPer, 0);
    int shift[4] = {0, 0, 0, 0};
    uint32_t span[4] = {0, 0, 0, 0};
    for (int c = 0; c < 4; ++c) {
        if (!mask[c]) continue;
        while (!((mask[c] >> shift[c]) & 1)) ++shift[c];
        span[c] = mask[c] >> shift[c];
    }
    for (size_t i = 0; i < n; ++i) {
        uint32_t v = 0;
        for (int c = 0; c < 4; ++c) {
            if (!span[c]) continue;
            uint32_t q = (rgba.pixels[i * 4 + c] * span[c] + 127) / 255;
            v |= (q << shift[c]) & mask[c];
        }
        for (int k = 0; k < bytesPer; ++k) out[i * bytesPer + k] = (uint8_t)(v >> (8 * k));
    }
    return out;
}

static Bytes encodeRgb(const Image& rgba, bool alpha) {
    if (!rgba.ok() || rgba.channels != 4) return Bytes();
    size_t n = (size_t)rgba.width * rgba.height;
    Bytes out(n * (alpha ? 4 : 3));
    for (size_t i = 0; i < n; ++i)
        memcpy(&out[i * (alpha ? 4 : 3)], &rgba.pixels[i * 4], alpha ? 4 : 3);
    return out;
}

static Image dropAlpha(const Image& rgba) {
    Image o;
    if (!rgba.ok() || rgba.channels != 4) return o;
    o.width = rgba.width; o.height = rgba.height; o.channels = 3;
    size_t n = (size_t)o.width * o.height;
    o.pixels.resize(n * 3);
    for (size_t i = 0; i < n; ++i) memcpy(&o.pixels[i * 3], &rgba.pixels[i * 4], 3);
    return o;
}

// one level in one of the .sba codecs; false when the codec cannot be written
bool encodeSbaCodec(int codec, const Image& rgba, Bytes& out, bool pngAlpha) {
    if (!rgba.ok()) return false;
    switch (codec) {
        case SBA_ETC_RGB: case SBA_ETC2_RGB: out = encodeEtc1(rgba); return true;
        case SBA_DXT1: out = encodeDxt(rgba, 1); return true;
        case SBA_DXT3: out = encodeDxt(rgba, 3); return true;
        case SBA_DXT5: out = encodeDxt(rgba, 5); return true;
        case SBA_ATC_RGB: out = encodeAtc(rgba, 1); return true;
        case SBA_ATC_RGBA_EXPLICIT: out = encodeAtc(rgba, 3); return true;
        case SBA_ATC_RGBA_INTERPOLATED: out = encodeAtc(rgba, 5); return true;
        case SBA_RGB: out = encodeRgb(rgba, false); return true;
        case SBA_RGBA: out = encodeRgb(rgba, true); return true;
        case SBA_RGB565: { const uint32_t m[4] = {0xF800, 0x07E0, 0x001F, 0}; out = encodeMasked(rgba, 2, m); return true; }
        case SBA_PNG: out = encodePng(pngAlpha ? rgba : dropAlpha(rgba)); return true;
        case SBA_JPEG: out = encodeJpeg(dropAlpha(rgba), 92); return true;
        default: return false;
    }
}

// ================================================================ containers

namespace {

uint32_t le32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
void put32(Bytes& b, size_t at, uint32_t v) { memcpy(&b[at], &v, 4); }

int levelDim(int d, int level) { return std::max(1, d >> level); }

// SBIN with some chunks' data replaced; sizes, padding and hashes redone
Bytes sbinRebuild(const uint8_t* d, size_t len, const std::map<std::string, Bytes>& repl) {
    Bytes out(d, d + 8);
    size_t pos = 8;
    while (pos + 12 <= len) {
        size_t p = (pos + 3) & ~(size_t)3;
        if (p + 12 > len) break;
        uint32_t size = le32(d + p + 4);
        if (p + 12 + size > len) break;
        std::string tag((const char*)d + p, 4);
        while (out.size() % 4) out.push_back(0);
        auto it = repl.find(tag);
        const uint8_t* body = d + p + 12;
        size_t bodyLen = size;
        if (it != repl.end()) { body = it->second.data(); bodyLen = it->second.size(); }
        size_t at = out.size();
        out.insert(out.end(), d + p, d + p + 4);
        out.resize(at + 12);
        put32(out, at + 4, (uint32_t)bodyLen);
        put32(out, at + 8, sbinFnv1(body, bodyLen));
        out.insert(out.end(), body, body + bodyLen);
        pos = p + 12 + size;
    }
    return out;
}

const uint8_t kSentinel[16] = { 0,0,0,0, 0xff,0xff,0xff,0xff, 0,0,0,0, 0,0,0,0 };

} // namespace

// Replace every image of an .sba. `stored` is the picture as the game keeps
// it (bottom-up); each level is resized from it.
static bool sbaReplace(const uint8_t* d, size_t len, const Image& stored, Bytes& out, std::string& rep) {
    auto chunks = sbinChunks(d, len);
    const SbinChunk* bulk = findChunk(chunks, "BULK");
    const SbinChunk* barg = findChunk(chunks, "BARG");
    if (!bulk || !barg) { rep = "this .sba has no image data (BULK/BARG)"; return false; }
    bool vectorFile = false;
    for (const std::string& x : sbinNames(chunks)) if (x == "VectorGraphic") vectorFile = true;
    size_t pre = (barg->size >= 16 && !memcmp(barg->data, kSentinel, 16)) ? 16 : 0;
    const uint8_t* body = barg->data + pre;
    size_t bodyLen = barg->size - pre;
    size_t n = bulk->size / 8;
    struct Blob { uint32_t off, size; Bytes data; bool replaced = false; };
    std::vector<Blob> blobs(n);
    for (size_t i = 0; i < n; ++i) {
        blobs[i].off = le32(bulk->data + i * 8);
        blobs[i].size = le32(bulk->data + i * 8 + 4);
        // the last slot's stated size can run past the data; what is there
        // is what counts, as it did when the texture was read
        if (blobs[i].off < bodyLen) {
            size_t have = std::min<size_t>(blobs[i].size, bodyLen - blobs[i].off);
            blobs[i].data.assign(body + blobs[i].off, body + blobs[i].off + have);
            blobs[i].size = (uint32_t)have;
        } else {
            blobs[i].size = 0;
        }
    }
    std::vector<SbaEntry> entries = readSba(d, len);
    int done = 0, kept = 0;
    std::string why;
    for (const SbaEntry& e : entries) {
        if (e.index < 0 || (size_t)e.index >= n) continue;
        Blob& b = blobs[e.index];
        if (b.off >= bodyLen || !b.size) { ++kept; continue; }
        Bytes enc;
        bool ok = false;
        if (e.format == "png" || e.format == "jpg") {
            // a PNG or JPEG payload: the size comes from the payload itself
            Image orig;
            bool had = e.format == "png" ? decodePng(e.data.data(), e.data.size(), orig)
                                         : decodeJpeg(e.data.data(), e.data.size(), orig);
            int w = had ? orig.width : (e.width ? e.width : stored.width);
            int h = had ? orig.height : (e.height ? e.height : stored.height);
            Image lvl = resizeRgba(stored, w, h);
            if (lvl.ok()) {
                if (e.format == "png") enc = encodePng(had && orig.channels == 3 ? dropAlpha(lvl) : lvl);
                else enc = encodeJpeg(dropAlpha(lvl), 92);
                ok = !enc.empty();
            }
        } else if (e.format == "pvr" || e.format == "dds") {
            Bytes inner(e.data.begin(), e.data.end());
            std::string r2;
            Image flippedBack = stored;               // containers inside .sba are bottom-up too
            ok = importIntoContainer(inner, flippedBack, enc, r2, false);
            if (!ok && why.empty()) why = r2;
        } else if (e.width > 0 && e.height > 0) {
            Image lvl = resizeRgba(stored, e.width, e.height);
            int codec = e.codec;
            if (codec == SBA_DEFAULT || codec == SBA_UNKNOWN) {
                // "default" says nothing: the byte count decides, as it did
                // when the texture was read
                size_t px = (size_t)e.width * e.height;
                if (b.size >= px * 4) codec = SBA_RGBA;
                else if (b.size >= px * 3) codec = SBA_RGB;
                else if (b.size >= px * 2) codec = SBA_RGB565;
            }
            ok = encodeSbaCodec(codec, lvl, enc, true);
            if (!ok && why.empty())
                why = std::string("the ") + sbaCodecName(e.codec) + " codec cannot be written";
            // a block codec's payload keeps its padding, if it had any
            if (ok && enc.size() < b.size) enc.resize(b.size, 0);
        }
        if (ok) { b.data = std::move(enc); b.replaced = true; ++done; }
        else ++kept;
    }
    if (!done) {
        rep = !why.empty() ? why
            : vectorFile ? "this .sba is a vector (SVG) drawing, not a picture - pick a raster texture instead"
                         : "no image in this .sba could be replaced";
        return false;
    }

    // lay the payloads out again: in place when every size stayed the same,
    // otherwise one after another at the alignment the file already used
    bool same = true;
    for (const Blob& b : blobs) if (b.data.size() != b.size) same = false;
    Bytes newBody;
    if (same) {
        newBody.assign(body, body + bodyLen);
        for (const Blob& b : blobs)
            if (b.replaced && (size_t)b.off + b.data.size() <= newBody.size())
                memcpy(&newBody[b.off], b.data.data(), b.data.size());
    } else {
        uint32_t align = 16;
        for (const Blob& b : blobs)
            while (align > 1 && b.off % align) align >>= 1;
        std::vector<size_t> order(n);
        for (size_t i = 0; i < n; ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(),
                         [&](size_t a, size_t c) { return blobs[a].off < blobs[c].off; });
        for (size_t k : order) {
            while (newBody.size() % align) newBody.push_back(0);
            blobs[k].off = (uint32_t)newBody.size();
            blobs[k].size = (uint32_t)blobs[k].data.size();
            newBody.insert(newBody.end(), blobs[k].data.begin(), blobs[k].data.end());
        }
    }
    Bytes newBulk(bulk->data, bulk->data + bulk->size);
    for (size_t i = 0; i < n; ++i) {
        put32(newBulk, i * 8, blobs[i].off);
        put32(newBulk, i * 8 + 4, same ? le32(bulk->data + i * 8 + 4) : blobs[i].size);
    }
    Bytes newBarg(barg->data, barg->data + pre);
    newBarg.insert(newBarg.end(), newBody.begin(), newBody.end());
    out = sbinRebuild(d, len, { { "BULK", newBulk }, { "BARG", newBarg } });
    char buf[200];
    snprintf(buf, sizeof(buf), "%d image(s) replaced%s", done,
             kept ? " - some were left as they were" : "");
    rep = buf;
    if (kept && !why.empty()) rep += " (" + why + ")";
    return true;
}

// .pvr (v2 / v3) and .dds; `img` in the container's own orientation
bool importIntoContainer(const Bytes& file, const Image& img, Bytes& out, std::string& rep,
                         bool /*unused*/) {
    const uint8_t* d = file.data();
    size_t len = file.size();
    Image rgba = img.channels == 4 ? img : toRgba(img);

    // ---- PVR version 2 ----
    if (len >= 52 && !memcmp(d + 44, "PVR!", 4)) {
        uint32_t headerLen = le32(d), height = le32(d + 4), width = le32(d + 8),
                 mips = le32(d + 12), flags = le32(d + 16);
        if (headerLen < 52 || headerLen > len || !width || !height || width > 16384 || height > 16384 ||
            (size_t)width * height > kMaxPixels) { rep = "damaged PVR header"; return false; }
        uint32_t kind = flags & 0xff;
        int levels = (flags & 0x100) ? (int)mips + 1 : 1;
        levels = std::max(1, std::min(levels, 16));
        bool flipped = (flags & 0x10000) != 0;
        // what each level becomes
        uint32_t newKind = kind;
        std::string note;
        auto encodeLevel = [&](const Image& lv, Bytes& o) -> bool {
            switch (newKind) {
                case 0x20: o = encodeDxt(lv, 1); return true;
                case 0x22: o = encodeDxt(lv, 3); return true;
                case 0x24: o = encodeDxt(lv, 5); return true;
                case 0x36: o = encodeEtc1(lv); return true;
                case 0x78: o = encodeAtc(lv, 1); return true;
                case 0x79: o = encodeAtc(lv, 5); return true;
                case 0x7A: o = encodeAtc(lv, 3); return true;
                case 0x10: { const uint32_t m[4] = {0xF000, 0x0F00, 0x00F0, 0x000F}; o = encodeMasked(lv, 2, m); return true; }
                case 0x11: { const uint32_t m[4] = {0xF800, 0x07C0, 0x003E, 0x0001}; o = encodeMasked(lv, 2, m); return true; }
                case 0x13: case 0x02: { const uint32_t m[4] = {0xF800, 0x07E0, 0x001F, 0}; o = encodeMasked(lv, 2, m); return true; }
                case 0x00: { const uint32_t m[4] = {0x0F00, 0x00F0, 0x000F, 0xF000}; o = encodeMasked(lv, 2, m); return true; }
                case 0x01: { const uint32_t m[4] = {0x7C00, 0x03E0, 0x001F, 0x8000}; o = encodeMasked(lv, 2, m); return true; }
                case 0x12: o = encodeRgb(lv, true); return true;
                case 0x15: o = encodeRgb(lv, false); return true;
                case 0x1A: {
                    o = encodeRgb(lv, true);
                    for (size_t i = 0; i + 3 < o.size(); i += 4) std::swap(o[i], o[i + 2]);
                    return true;
                }
            }
            return false;
        };
        if (kind == 0x0C || kind == 0x0D || kind == 0x18 || kind == 0x19) {
            newKind = 0x12;                                // PVRTC -> RGBA8888
            note = " (PVRTC cannot be written; the texture is now uncompressed RGBA)";
        }
        Bytes probe;
        if (!encodeLevel(Image{1, 1, 4, Bytes(4, 0)}, probe)) {
            rep = "this PVR's pixel format cannot be written";
            return false;
        }
        Bytes body;
        for (int l = 0; l < levels; ++l) {
            Image lv = resizeRgba(rgba, levelDim((int)width, l), levelDim((int)height, l));
            if (flipped) flipImageVertically(lv);
            Bytes o;
            encodeLevel(lv, o);
            body.insert(body.end(), o.begin(), o.end());
        }
        out.assign(d, d + headerLen);
        uint32_t newFlags = (flags & ~0xffu) | newKind;
        put32(out, 16, newFlags);
        put32(out, 20, (uint32_t)body.size());
        if (newKind != kind) {
            put32(out, 24, 32);
            put32(out, 28, 0x000000FF); put32(out, 32, 0x0000FF00);
            put32(out, 36, 0x00FF0000); put32(out, 40, 0xFF000000);
        }
        out.insert(out.end(), body.begin(), body.end());
        char buf[160];
        snprintf(buf, sizeof(buf), "PVR %ux%u, %d mip level(s) written", width, height, levels);
        rep = buf + note;
        return true;
    }

    // ---- PVR version 3 ----
    if (len >= 52 && !memcmp(d, "PVR\x03", 4)) {
        uint64_t format;
        memcpy(&format, d + 8, 8);
        uint32_t height = le32(d + 24), width = le32(d + 28), mips = le32(d + 44), meta = le32(d + 48);
        size_t headerLen = 52 + (size_t)meta;
        if (headerLen > len || !width || !height || width > 16384 || height > 16384 ||
            (size_t)width * height > kMaxPixels) { rep = "damaged PVR header"; return false; }
        int levels = std::max(1, std::min((int)mips, 16));
        uint64_t newFormat = format;
        std::string note;
        auto encodeLevel = [&](const Image& lv, Bytes& o) -> bool {
            if ((newFormat >> 32) != 0) {                 // channel-described: take RGBA8888 only
                if (newFormat == 0x0808080861626772ull) { o = encodeRgb(lv, true); return true; }
                if (newFormat == 0x0008080800626772ull) { o = encodeRgb(lv, false); return true; }
                return false;
            }
            switch ((uint32_t)newFormat) {
                case 6: case 22: o = encodeEtc1(lv); return true;
                case 7: o = encodeDxt(lv, 1); return true;
                case 9: o = encodeDxt(lv, 3); return true;
                case 11: o = encodeDxt(lv, 5); return true;
            }
            return false;
        };
        if ((format >> 32) == 0 && format <= 3) {
            newFormat = 0x0808080861626772ull;             // 'rgba' 8.8.8.8
            note = " (PVRTC cannot be written; the texture is now uncompressed RGBA)";
        }
        Bytes probe;
        if (!encodeLevel(Image{1, 1, 4, Bytes(4, 0)}, probe)) { rep = "this PVR's pixel format cannot be written"; return false; }
        Bytes body;
        for (int l = 0; l < levels; ++l) {
            Image lv = resizeRgba(rgba, levelDim((int)width, l), levelDim((int)height, l));
            Bytes o;
            encodeLevel(lv, o);
            body.insert(body.end(), o.begin(), o.end());
        }
        out.assign(d, d + headerLen);
        memcpy(&out[8], &newFormat, 8);
        if (newFormat != format) put32(out, 20, 0);       // channel type: unsigned byte, normalised
        out.insert(out.end(), body.begin(), body.end());
        char buf[160];
        snprintf(buf, sizeof(buf), "PVR %ux%u, %d mip level(s) written", width, height, levels);
        rep = buf + note;
        return true;
    }

    // ---- DDS ----
    if (len >= 128 && !memcmp(d, "DDS ", 4)) {
        uint32_t height = le32(d + 12), width = le32(d + 16), flags = le32(d + 8);
        uint32_t mips = (flags & 0x20000) ? le32(d + 28) : 1;
        if (!width || !height || width > 16384 || height > 16384 || (size_t)width * height > kMaxPixels) {
            rep = "damaged DDS header";
            return false;
        }
        int levels = std::max(1, std::min((int)mips, 16));
        char fourcc[5] = {0, 0, 0, 0, 0};
        memcpy(fourcc, d + 84, 4);
        uint32_t bits = le32(d + 88);
        uint32_t mask[4] = { le32(d + 92), le32(d + 96), le32(d + 100), le32(d + 104) };
        if (!fourcc[0] && !mask[0] && !mask[1] && !mask[2]) {
            mask[0] = 0xF000; mask[1] = 0x0F00; mask[2] = 0x00F0; mask[3] = 0x000F;
        }
        if (!bits) bits = 16;
        auto encodeLevel = [&](const Image& lv, Bytes& o) -> bool {
            if (!memcmp(fourcc, "ETC ", 4) || !memcmp(fourcc, "ETC1", 4)) { o = encodeEtc1(lv); return true; }
            if (!memcmp(fourcc, "DXT1", 4)) { o = encodeDxt(lv, 1); return true; }
            if (!memcmp(fourcc, "DXT3", 4)) { o = encodeDxt(lv, 3); return true; }
            if (!memcmp(fourcc, "DXT5", 4)) { o = encodeDxt(lv, 5); return true; }
            if (!memcmp(fourcc, "ATC ", 4)) { o = encodeAtc(lv, 1); return true; }
            if (!memcmp(fourcc, "ATCA", 4)) { o = encodeAtc(lv, 3); return true; }
            if (!memcmp(fourcc, "ATCI", 4)) { o = encodeAtc(lv, 5); return true; }
            if (!fourcc[0] && bits >= 16 && bits <= 32) { o = encodeMasked(lv, (int)bits / 8, mask); return true; }
            return false;
        };
        Bytes probe;
        if (!encodeLevel(Image{1, 1, 4, Bytes(4, 0)}, probe)) { rep = "this DDS's pixel format cannot be written"; return false; }
        Bytes body;
        for (int l = 0; l < levels; ++l) {
            Image lv = resizeRgba(rgba, levelDim((int)width, l), levelDim((int)height, l));
            Bytes o;
            encodeLevel(lv, o);
            body.insert(body.end(), o.begin(), o.end());
        }
        out.assign(d, d + 128);
        out.insert(out.end(), body.begin(), body.end());
        char buf[160];
        snprintf(buf, sizeof(buf), "DDS %ux%u, %d mip level(s) written", width, height, levels);
        rep = buf;
        return true;
    }
    rep = "not a texture container this tool can write";
    return false;
}

static bool importPlainPicture(const Bytes& original, const Image& rgba, Bytes& out, std::string& report);

bool importPicture(const std::string& assetPath, const Bytes& original, const Image& picture,
                   Bytes& out, std::string& report) {
    out.clear();
    report.clear();
    if (!picture.ok()) { report = "the picture is empty"; return false; }
    Image rgba = toRgba(picture);
    const uint8_t* d = original.data();
    size_t len = original.size();
    (void)assetPath;

    bool ok = false;
    if (len >= 8 && !memcmp(d, "SBIN", 4)) {
        // the game keeps .sba images bottom-up
        Image stored = rgba;
        flipImageVertically(stored);
        ok = sbaReplace(d, len, stored, out, report);
    } else if (len >= 128 && !memcmp(d, "DDS ", 4)) {
        Image stored = rgba;                               // DDS is bottom-up as well
        flipImageVertically(stored);
        ok = importIntoContainer(original, stored, out, report, false);
    } else if (len >= 52 && (!memcmp(d, "PVR\x03", 4) || !memcmp(d + 44, "PVR!", 4))) {
        ok = importIntoContainer(original, rgba, out, report, false);
    } else {
        return importPlainPicture(original, rgba, out, report);
    }
    // a different shape is stretched to the texture's; worth saying
    if (ok) {
        Image before;
        bool known = false;
        if (len >= 8 && !memcmp(d, "SBIN", 4)) {
            auto e = readSba(d, len);
            if (!e.empty() && e[0].width > 0) { before.width = e[0].width; before.height = e[0].height; known = true; }
        } else {
            known = decodeTextureFile(d, len, before);
        }
        if (known && before.height > 0 && picture.height > 0) {
            double a = (double)picture.width / picture.height, b = (double)before.width / before.height;
            if (a / b > 1.02 || b / a > 1.02) {
                char buf[160];
                snprintf(buf, sizeof(buf), "\nThe picture (%dx%d) was stretched to the texture's %dx%d.",
                         picture.width, picture.height, before.width, before.height);
                report += buf;
            }
        }
    }
    return ok;
}

static bool importPlainPicture(const Bytes& original, const Image& rgba, Bytes& out, std::string& report) {
    const uint8_t* d = original.data();
    size_t len = original.size();
    if (len >= 8 && d[0] == 0x89 && d[1] == 'P') {
        Image orig;
        bool had = decodePng(d, len, orig);
        Image lvl = had ? resizeRgba(rgba, orig.width, orig.height) : rgba;
        out = encodePng(had && orig.channels == 3 ? dropAlpha(lvl) : lvl);
        report = "PNG written";
        return !out.empty();
    }
    if (len >= 3 && d[0] == 0xFF && d[1] == 0xD8) {
        Image orig;
        bool had = decodeJpeg(d, len, orig);
        Image lvl = had ? resizeRgba(rgba, orig.width, orig.height) : rgba;
        out = encodeJpeg(dropAlpha(lvl), 92);
        report = "JPEG written";
        return !out.empty();
    }
    report = "this file is not a texture the tool can write into";
    return false;
}

Bytes rr3WrapZ(const Bytes& plain) {
    Bytes out(4);
    put32(out, 0, (uint32_t)plain.size());
    Bytes z = zlibCompress(plain.data(), plain.size());
    out.insert(out.end(), z.begin(), z.end());
    return out;
}

} // namespace nfsnl
