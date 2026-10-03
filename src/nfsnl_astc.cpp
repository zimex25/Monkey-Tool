// nfsnl_astc.cpp - ASTC LDR block decoding (Real Racing Next's textures)
//
// Written from the Khronos Data Format Specification's ASTC chapter: block
// mode, partitions and their hash, colour endpoint modes 0-13 (LDR ones;
// HDR modes decode to the error colour), integer sequence encoding with
// trits and quints, weight infill and dual planes, void-extent blocks.
// 2D only - that is all a texture needs.
#include "nfsnl.h"

#include <cstring>

namespace nfsnl {

namespace {

struct Bits128 {
    uint8_t b[16];
    uint32_t get(int start, int count) const {       // LSB-first
        uint32_t v = 0;
        for (int i = 0; i < count; ++i) {
            int p = start + i;
            if (p < 0 || p >= 128) continue;
            v |= (uint32_t)((b[p >> 3] >> (p & 7)) & 1) << i;
        }
        return v;
    }
};

// the 21 quantisation levels and how each is stored
struct Range { int levels, trits, quints, bits; };
const Range kRanges[21] = {
    {2, 0, 0, 1}, {3, 1, 0, 0}, {4, 0, 0, 2}, {5, 0, 1, 0}, {6, 1, 0, 1}, {8, 0, 0, 3},
    {10, 0, 1, 1}, {12, 1, 0, 2}, {16, 0, 0, 4}, {20, 0, 1, 2}, {24, 1, 0, 3}, {32, 0, 0, 5},
    {40, 0, 1, 3}, {48, 1, 0, 4}, {64, 0, 0, 6}, {80, 0, 1, 4}, {96, 1, 0, 5}, {128, 0, 0, 7},
    {160, 0, 1, 5}, {192, 1, 0, 6}, {256, 0, 0, 8},
};

int iseBits(int count, const Range& r) {
    if (r.trits) return (8 * count + 4) / 5 + count * r.bits;
    if (r.quints) return (7 * count + 2) / 3 + count * r.bits;
    return count * r.bits;
}

void decodeTrits(uint32_t T, int t[5]) {
    uint32_t C;
    if (((T >> 2) & 7) == 7) {
        C = (((T >> 5) & 7) << 2) | (T & 3);
        t[4] = 2; t[3] = 2;
    } else {
        C = T & 0x1F;
        if (((T >> 5) & 3) == 3) { t[4] = 2; t[3] = (T >> 7) & 1; }
        else { t[4] = (T >> 7) & 1; t[3] = (T >> 5) & 3; }
    }
    if ((C & 3) == 3) {
        t[2] = 2; t[1] = (C >> 4) & 1;
        t[0] = (((C >> 3) & 1) << 1) | (((C >> 2) & 1) & ~((C >> 3) & 1) & 1);
    } else if (((C >> 2) & 3) == 3) {
        t[2] = 2; t[1] = 2; t[0] = C & 3;
    } else {
        t[2] = (C >> 4) & 1; t[1] = (C >> 2) & 3;
        t[0] = (((C >> 1) & 1) << 1) | ((C & 1) & ~((C >> 1) & 1) & 1);
    }
}

void decodeQuints(uint32_t Q, int q[3]) {
    if (((Q >> 1) & 3) == 3 && ((Q >> 5) & 3) == 0) {
        uint32_t q0b = Q & 1;
        q[2] = (int)((q0b << 2) | ((((Q >> 4) & 1) & ~q0b & 1) << 1) | (((Q >> 3) & 1) & ~q0b & 1));
        q[1] = 4; q[0] = 4;
        return;
    }
    uint32_t C;
    if (((Q >> 1) & 3) == 3) {
        q[2] = 4;
        C = (((Q >> 3) & 3) << 3) | ((~(Q >> 5) & 3) << 1) | (Q & 1);
    } else {
        q[2] = (Q >> 5) & 3;
        C = Q & 0x1F;
    }
    if ((C & 7) == 5) { q[1] = 4; q[0] = (C >> 3) & 3; }
    else { q[1] = (C >> 3) & 3; q[0] = C & 7; }
}

// Read `count` ISE values of range r starting at `start`; each value is
// returned as (high part D, low bits m).
void readIse(const Bits128& bl, int start, int count, const Range& r, std::vector<int>& D,
             std::vector<int>& M) {
    D.assign(count, 0);
    M.assign(count, 0);
    int p = start;
    int b = r.bits;
    if (r.trits) {
        for (int i = 0; i < count; i += 5) {
            int m[5] = {0, 0, 0, 0, 0};
            uint32_t T = 0;
            static const int tb[5] = {2, 2, 1, 2, 1};   // trit bits after each value
            int tpos = 0;
            for (int k = 0; k < 5; ++k) {
                m[k] = (int)bl.get(p, b); p += b;
                T |= bl.get(p, tb[k]) << tpos; p += tb[k]; tpos += tb[k];
            }
            int t[5];
            decodeTrits(T, t);
            for (int k = 0; k < 5 && i + k < count; ++k) { D[i + k] = t[k]; M[i + k] = m[k]; }
        }
    } else if (r.quints) {
        for (int i = 0; i < count; i += 3) {
            int m[3] = {0, 0, 0};
            uint32_t Q = 0;
            static const int qb[3] = {3, 2, 2};
            int qpos = 0;
            for (int k = 0; k < 3; ++k) {
                m[k] = (int)bl.get(p, b); p += b;
                Q |= bl.get(p, qb[k]) << qpos; p += qb[k]; qpos += qb[k];
            }
            int q[3];
            decodeQuints(Q, q);
            for (int k = 0; k < 3 && i + k < count; ++k) { D[i + k] = q[k]; M[i + k] = m[k]; }
        }
    } else {
        for (int i = 0; i < count; ++i) { M[i] = (int)bl.get(p, b); p += b; }
    }
}

int replicate(int v, int from, int to) {
    if (from <= 0) return 0;
    int out = 0, have = 0;
    while (have < to) {
        int take = std::min(from, to - have);
        out = (out << take) | (v >> (from - take));
        have += take;
    }
    return out;
}

int unquantColor(int D, int m, const Range& r) {
    int b = r.bits;
    if (!r.trits && !r.quints) return replicate(m, b, 8);
    int a = m & 1;
    int A = a ? 0x1FF : 0;
    int B = 0, C = 0;
    int bb = (m >> 1) & 1, c = (m >> 2) & 1, d = (m >> 3) & 1, e = (m >> 4) & 1, f = (m >> 5) & 1;
    if (r.trits) {
        switch (b) {
            case 1: C = 204; B = 0; break;
            case 2: C = 93;  B = (bb << 8) | (bb << 4) | (bb << 2) | (bb << 1); break;
            case 3: C = 44;  { int cb = (c << 1) | bb; B = (cb << 7) | (cb << 2) | cb; } break;
            case 4: C = 22;  { int dcb = (d << 2) | (c << 1) | bb; B = (dcb << 6) | dcb; } break;
            case 5: C = 11;  { int edcb = (e << 3) | (d << 2) | (c << 1) | bb; B = (edcb << 5) | (edcb >> 2); } break;
            case 6: C = 5;   { int fedcb = (f << 4) | (e << 3) | (d << 2) | (c << 1) | bb; B = (fedcb << 4) | (fedcb >> 4); } break;
        }
    } else {
        switch (b) {
            case 1: C = 113; B = 0; break;
            case 2: C = 54;  B = (bb << 8) | (bb << 3) | (bb << 2); break;
            case 3: C = 26;  { int cb = (c << 1) | bb; B = (cb << 7) | (cb << 1) | (cb >> 1); } break;
            case 4: C = 13;  { int dcb = (d << 2) | (c << 1) | bb; B = (dcb << 6) | (dcb >> 1); } break;
            case 5: C = 6;   { int edcb = (e << 3) | (d << 2) | (c << 1) | bb; B = (edcb << 5) | (edcb >> 3); } break;
        }
    }
    int T = D * C + B;
    T ^= A;
    return (A & 0x80) | (T >> 2);
}

int unquantWeight(int D, int m, const Range& r) {
    int b = r.bits;
    int v;
    if (!r.trits && !r.quints) {
        v = replicate(m, b, 6);
    } else if (b == 0) {
        v = r.trits ? D * 32 : D * 16;
        return v;           // 0, 32, 64 or 0 .. 64 by 16: no adjustment
    } else {
        int a = m & 1;
        int A = a ? 0x7F : 0;
        int B = 0, C = 0;
        int bb = (m >> 1) & 1, c = (m >> 2) & 1;
        if (r.trits) {
            if (b == 1) { C = 50; B = 0; }
            else if (b == 2) { C = 23; B = (bb << 6) | (bb << 2) | bb; }
            else { C = 11; int cb = (c << 1) | bb; B = (cb << 5) | cb; }
        } else {
            if (b == 1) { C = 28; B = 0; }
            else { C = 13; B = (bb << 6) | (bb << 1); }
        }
        int T = D * C + B;
        T ^= A;
        v = (A & 0x20) | (T >> 2);
    }
    if (v > 32) ++v;
    return v;
}

uint32_t hash52(uint32_t p) {
    p ^= p >> 15; p -= p << 17; p += p << 7; p += p << 4;
    p ^= p >> 5;  p += p << 16; p ^= p >> 7; p ^= p >> 3;
    p ^= p << 6;  p ^= p >> 17;
    return p;
}

int selectPartition(int seed, int x, int y, int z, int count, bool small) {
    if (small) { x <<= 1; y <<= 1; z <<= 1; }
    seed += (count - 1) * 1024;
    uint32_t r = hash52((uint32_t)seed);
    int s[12];
    s[0] = r & 0xF; s[1] = (r >> 4) & 0xF; s[2] = (r >> 8) & 0xF; s[3] = (r >> 12) & 0xF;
    s[4] = (r >> 16) & 0xF; s[5] = (r >> 20) & 0xF; s[6] = (r >> 24) & 0xF; s[7] = (r >> 28) & 0xF;
    s[8] = (r >> 18) & 0xF; s[9] = (r >> 22) & 0xF; s[10] = (r >> 26) & 0xF;
    s[11] = ((r >> 30) | (r << 2)) & 0xF;
    for (int& v : s) v *= v;
    int sh1, sh2;
    if (seed & 1) { sh1 = (seed & 2) ? 4 : 5; sh2 = count == 3 ? 6 : 5; }
    else { sh1 = count == 3 ? 6 : 5; sh2 = (seed & 2) ? 4 : 5; }
    int sh3 = (seed & 0x10) ? sh1 : sh2;
    s[0] >>= sh1; s[1] >>= sh2; s[2] >>= sh1; s[3] >>= sh2; s[4] >>= sh1; s[5] >>= sh2;
    s[6] >>= sh1; s[7] >>= sh2; s[8] >>= sh3; s[9] >>= sh3; s[10] >>= sh3; s[11] >>= sh3;
    int a = (int)((s[0] * x + s[1] * y + s[10] * z + (r >> 14)) & 0x3F);
    int b = (int)((s[2] * x + s[3] * y + s[11] * z + (r >> 10)) & 0x3F);
    int c = (int)((s[4] * x + s[5] * y + s[8] * z + (r >> 6)) & 0x3F);
    int d = (int)((s[6] * x + s[7] * y + s[9] * z + (r >> 2)) & 0x3F);
    if (count < 4) d = 0;
    if (count < 3) c = 0;
    if (a >= b && a >= c && a >= d) return 0;
    if (b >= c && b >= d) return 1;
    if (c >= d) return 2;
    return 3;
}

int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

void bitTransferSigned(int& a, int& b) {
    b >>= 1;
    b |= a & 0x80;
    a >>= 1;
    a &= 0x3F;
    if (a & 0x20) a -= 0x40;
}

// one endpoint pair from a colour endpoint mode; false for HDR modes
bool decodeEndpoints(int mode, const int* v, int e0[4], int e1[4]) {
    auto set = [](int* e, int r, int g, int b, int a) { e[0] = r; e[1] = g; e[2] = b; e[3] = a; };
    auto blue = [](int* e, int r, int g, int b, int a) {
        e[0] = (r + b) >> 1; e[1] = (g + b) >> 1; e[2] = b; e[3] = a;
    };
    switch (mode) {
        case 0: set(e0, v[0], v[0], v[0], 255); set(e1, v[1], v[1], v[1], 255); return true;
        case 1: {
            int l0 = (v[0] >> 2) | (v[1] & 0xC0);
            int l1 = std::min(255, l0 + (v[1] & 0x3F));
            set(e0, l0, l0, l0, 255); set(e1, l1, l1, l1, 255); return true;
        }
        case 4: set(e0, v[0], v[0], v[0], v[2]); set(e1, v[1], v[1], v[1], v[3]); return true;
        case 5: {
            int a0 = v[0], b0 = v[1], a2 = v[2], b2 = v[3];
            bitTransferSigned(b0, a0);
            bitTransferSigned(b2, a2);
            set(e0, a0, a0, a0, a2);
            int l = clamp255(a0 + b0), al = clamp255(a2 + b2);
            set(e1, l, l, l, al);
            return true;
        }
        case 6:
            set(e0, (v[0] * v[3]) >> 8, (v[1] * v[3]) >> 8, (v[2] * v[3]) >> 8, 255);
            set(e1, v[0], v[1], v[2], 255);
            return true;
        case 8: {
            int s0 = v[0] + v[2] + v[4], s1 = v[1] + v[3] + v[5];
            if (s1 >= s0) { set(e0, v[0], v[2], v[4], 255); set(e1, v[1], v[3], v[5], 255); }
            else { blue(e0, v[1], v[3], v[5], 255); blue(e1, v[0], v[2], v[4], 255); }
            return true;
        }
        case 9: {
            int w[6] = {v[0], v[1], v[2], v[3], v[4], v[5]};
            bitTransferSigned(w[1], w[0]);
            bitTransferSigned(w[3], w[2]);
            bitTransferSigned(w[5], w[4]);
            if (w[1] + w[3] + w[5] >= 0) {
                set(e0, w[0], w[2], w[4], 255);
                set(e1, clamp255(w[0] + w[1]), clamp255(w[2] + w[3]), clamp255(w[4] + w[5]), 255);
            } else {
                blue(e0, clamp255(w[0] + w[1]), clamp255(w[2] + w[3]), clamp255(w[4] + w[5]), 255);
                blue(e1, w[0], w[2], w[4], 255);
            }
            for (int k = 0; k < 4; ++k) { e0[k] = clamp255(e0[k]); e1[k] = clamp255(e1[k]); }
            return true;
        }
        case 10:
            set(e0, (v[0] * v[3]) >> 8, (v[1] * v[3]) >> 8, (v[2] * v[3]) >> 8, v[4]);
            set(e1, v[0], v[1], v[2], v[5]);
            return true;
        case 12: {
            int s0 = v[0] + v[2] + v[4], s1 = v[1] + v[3] + v[5];
            if (s1 >= s0) { set(e0, v[0], v[2], v[4], v[6]); set(e1, v[1], v[3], v[5], v[7]); }
            else { blue(e0, v[1], v[3], v[5], v[7]); blue(e1, v[0], v[2], v[4], v[6]); }
            return true;
        }
        case 13: {
            int w[8] = {v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]};
            bitTransferSigned(w[1], w[0]);
            bitTransferSigned(w[3], w[2]);
            bitTransferSigned(w[5], w[4]);
            bitTransferSigned(w[7], w[6]);
            if (w[1] + w[3] + w[5] >= 0) {
                set(e0, w[0], w[2], w[4], w[6]);
                set(e1, w[0] + w[1], w[2] + w[3], w[4] + w[5], w[6] + w[7]);
            } else {
                blue(e0, w[0] + w[1], w[2] + w[3], w[4] + w[5], w[6] + w[7]);
                blue(e1, w[0], w[2], w[4], w[6]);
            }
            for (int k = 0; k < 4; ++k) { e0[k] = clamp255(e0[k]); e1[k] = clamp255(e1[k]); }
            return true;
        }
    }
    return false;
}

void errorBlock(uint8_t* out, int bw, int bh) {
    for (int i = 0; i < bw * bh; ++i) { out[i * 4] = 255; out[i * 4 + 1] = 0; out[i * 4 + 2] = 255; out[i * 4 + 3] = 255; }
}

// one block to bw*bh RGBA texels
void decodeBlock(const uint8_t* src, int bw, int bh, uint8_t* out) {
    Bits128 bl;
    memcpy(bl.b, src, 16);
    uint32_t mode = bl.get(0, 11);

    // void extent: one colour for the whole block
    if ((mode & 0x1FF) == 0x1FC) {
        int c[4];
        for (int k = 0; k < 4; ++k) c[k] = (int)(bl.get(64 + 16 * k, 16) >> 8);
        for (int i = 0; i < bw * bh; ++i)
            for (int k = 0; k < 4; ++k) out[i * 4 + k] = (uint8_t)c[k];
        return;
    }

    // block mode: weight grid, weight range, dual plane
    int wx = 0, wy = 0, R = 0, H = (mode >> 9) & 1, D = (mode >> 10) & 1;
    if (mode & 3) {
        R = (int)(((mode >> 4) & 1) | ((mode & 3) << 1));
        int A = (mode >> 5) & 3, B = (mode >> 7) & 3;
        switch ((mode >> 2) & 3) {
            case 0: wx = B + 4; wy = A + 2; break;
            case 1: wx = B + 8; wy = A + 2; break;
            case 2: wx = A + 2; wy = B + 8; break;
            case 3:
                B &= 1;
                if (mode & 0x100) { wx = B + 2; wy = A + 2; }
                else { wx = A + 2; wy = B + 6; }
                break;
        }
    } else {
        R = (int)(((mode >> 1) & 6) | ((mode >> 4) & 1));
        if ((mode & 0xF) == 0) { errorBlock(out, bw, bh); return; }
        int A = (mode >> 5) & 3, B = (mode >> 9) & 3;
        switch ((mode >> 7) & 3) {
            case 0: wx = 12; wy = A + 2; break;
            case 1: wx = A + 2; wy = 12; break;
            case 2: wx = A + 6; wy = B + 6; D = 0; H = 0; break;
            case 3:
                if (((mode >> 5) & 3) == 0) { wx = 6; wy = 10; }
                else if (((mode >> 5) & 3) == 1) { wx = 10; wy = 6; }
                else { errorBlock(out, bw, bh); return; }
                break;
        }
    }
    if (R < 2 || wx > bw || wy > bh) { errorBlock(out, bw, bh); return; }
    // weight levels: 2,3,4,5,6,8 without the high-precision bit, 10 .. 32 with it
    const Range& wRange = kRanges[(H ? 6 : 0) + (R - 2)];
    int planes = D + 1;
    int weightCount = wx * wy * planes;
    if (weightCount > 64) { errorBlock(out, bw, bh); return; }
    int weightBits = iseBits(weightCount, wRange);
    if (weightBits < 24 || weightBits > 96) { errorBlock(out, bw, bh); return; }

    int parts = (int)bl.get(11, 2) + 1;
    if (parts == 4 && D) { errorBlock(out, bw, bh); return; }

    int cem[4] = {0, 0, 0, 0};
    int colorStart, extraCemBits = 0;
    int seed = 0;
    int belowWeights = 128 - weightBits;
    if (parts == 1) {
        cem[0] = (int)bl.get(13, 4);
        colorStart = 17;
    } else {
        seed = (int)bl.get(13, 10);
        uint32_t enc = bl.get(23, 6);
        colorStart = 29;
        if ((enc & 3) == 0) {
            for (int i = 0; i < parts; ++i) cem[i] = (int)(enc >> 2);
        } else {
            extraCemBits = 3 * parts - 4;
            uint32_t all = (enc >> 2) | (bl.get(belowWeights - extraCemBits, extraCemBits) << 4);
            int base = (int)(enc & 3) - 1;
            for (int i = 0; i < parts; ++i) {
                int cbit = (all >> i) & 1;
                int m = (all >> (parts + 2 * i)) & 3;
                cem[i] = ((base + cbit) << 2) | m;
            }
        }
    }
    int ccs = 0;
    int colorEnd = belowWeights - extraCemBits;
    if (D) { colorEnd -= 2; ccs = (int)bl.get(colorEnd, 2); }

    int colorCount = 0;
    for (int i = 0; i < parts; ++i) colorCount += ((cem[i] >> 2) + 1) * 2;
    if (colorCount > 18) { errorBlock(out, bw, bh); return; }
    int avail = colorEnd - colorStart;
    int cr = -1;
    for (int i = 20; i >= 0; --i)
        if (iseBits(colorCount, kRanges[i]) <= avail) { cr = i; break; }
    if (cr < 4) { errorBlock(out, bw, bh); return; }

    std::vector<int> cd, cm;
    readIse(bl, colorStart, colorCount, kRanges[cr], cd, cm);
    int cv[18];
    for (int i = 0; i < colorCount; ++i) cv[i] = unquantColor(cd[i], cm[i], kRanges[cr]);

    int ep[4][2][4];
    int ci = 0;
    for (int i = 0; i < parts; ++i) {
        int n = ((cem[i] >> 2) + 1) * 2;
        if (!decodeEndpoints(cem[i], cv + ci, ep[i][0], ep[i][1])) { errorBlock(out, bw, bh); return; }
        ci += n;
    }

    // weights: stored from the top of the block down, bits reversed
    Bits128 rev;
    for (int i = 0; i < 16; ++i) {
        uint8_t v = src[15 - i], r = 0;
        for (int k = 0; k < 8; ++k) r |= ((v >> k) & 1) << (7 - k);
        rev.b[i] = r;
    }
    std::vector<int> wd, wm;
    readIse(rev, 0, weightCount, wRange, wd, wm);
    int wq[64];
    for (int i = 0; i < weightCount; ++i) wq[i] = unquantWeight(wd[i], wm[i], wRange);

    // infill onto the block's texels
    int Ds = (1024 + bw / 2) / std::max(1, bw - 1);
    int Dt = (1024 + bh / 2) / std::max(1, bh - 1);
    bool small = bw * bh < 31;
    for (int t = 0; t < bh; ++t)
        for (int s = 0; s < bw; ++s) {
            int w[2] = {0, 0};
            for (int pl = 0; pl < planes; ++pl) {
                int cs = Ds * s, ct = Dt * t;
                int gs = (cs * (wx - 1) + 32) >> 6, gt = (ct * (wy - 1) + 32) >> 6;
                int js = gs >> 4, fs = gs & 0xF, jt = gt >> 4, ft = gt & 0xF;
                int w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
                auto W = [&](int x, int y) {
                    x = std::min(x, wx - 1); y = std::min(y, wy - 1);
                    return wq[(y * wx + x) * planes + pl];
                };
                w[pl] = (W(js, jt) * w00 + W(js + 1, jt) * w01 + W(js, jt + 1) * w10 +
                         W(js + 1, jt + 1) * w11 + 8) >> 4;
            }
            int p = parts > 1 ? selectPartition(seed, s, t, 0, parts, small) : 0;
            uint8_t* o = out + (t * bw + s) * 4;
            for (int k = 0; k < 4; ++k) {
                int wt = (D && k == ccs) ? w[1] : w[0];
                int c0 = ep[p][0][k] * 257, c1 = ep[p][1][k] * 257;
                int c = (c0 * (64 - wt) + c1 * wt + 32) >> 6;
                o[k] = (uint8_t)(c >> 8);
            }
        }
}

} // namespace

bool decodeAstc(const uint8_t* data, size_t len, int width, int height, int bw, int bh, Image& out) {
    if (bw < 4 || bh < 4 || bw > 12 || bh > 12 || width <= 0 || height <= 0) return false;
    int bx = (width + bw - 1) / bw, by = (height + bh - 1) / bh;
    if ((size_t)bx * by * 16 > len) return false;
    out.width = width;
    out.height = height;
    out.channels = 4;
    out.pixels.assign((size_t)width * height * 4, 0);
    std::vector<uint8_t> block((size_t)bw * bh * 4);
    for (int y = 0; y < by; ++y)
        for (int x = 0; x < bx; ++x) {
            decodeBlock(data + ((size_t)y * bx + x) * 16, bw, bh, block.data());
            for (int t = 0; t < bh; ++t)
                for (int s = 0; s < bw; ++s) {
                    int px = x * bw + s, py = y * bh + t;
                    if (px >= width || py >= height) continue;
                    memcpy(&out.pixels[((size_t)py * width + px) * 4], &block[(t * bw + s) * 4], 4);
                }
        }
    return true;
}

} // namespace nfsnl
