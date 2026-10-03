// nfsnl_rr3.cpp - Real Racing 3
//
// A second game profile, and the first one that shares almost nothing with
// No Limits. What it needs:
//
//   .z            uint32 uncompressed size, then a zlib stream. Almost every
//                 file in .depot is wrapped in this.
//   .z.bin        a chain of those blocks, each a whole file. Car shadows use
//                 it: one frame per sun angle.
//   .etc.dds      a plain 128-byte DDS header whose FourCC is "ETC " for ETC1
//                 blocks, or zero for uncompressed RGBA4444. The old route was
//                 QuickBMS to turn it into a .pvr and then PVRTexTool to look
//                 at it; both steps are done here instead.
//   .m3g          a real JSR-184 M3G file. No Limits uses the same extension
//                 for something completely different, so the magic decides
//                 which reader runs.
//   .points       where the wheels, the steering wheel and the mirrors go.
//
// The .points file is the interesting one. A car's exterior model holds no
// wheels at all - it declares the wheel and tyre materials and leaves the
// meshes to the game - and a handful of parts that do ship with the model (the
// steering wheel, the rev-counter needle, the driver's hands) are modelled at
// the origin. Every importer that ignores .points therefore piles those parts
// up in the middle of the car and has no wheels, which is exactly the problem
// this file exists to fix.
//
// The units were worked out against geometry rather than assumed. A .points
// entry is in 1/32 of the model's own unit, with the axes in the authoring
// program's order, so
//
//     model.x = +points.x / 32
//     model.y = +points.z / 32      (up)
//     model.z = -points.y / 32      (along the car)
//
// Checked on the 1979 Porsche 935: POINT_BRAKELIGHT_LEFT lands at
// (-0.459, 0.473, 1.849) and the LOD_A_BRAKES_LEFT mesh sits at
// (-0.459, 0.471, 1.841) - eight millimetres out on a 4.7 m car. The
// headlight, mirror and exhaust points agree to the same accuracy.
#include "nfsnl.h"
#include <array>
#include <cctype>
#include <cstring>
#include <set>
#include <cstdio>
#include <cmath>
#include <algorithm>

namespace nfsnl {

namespace {

inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline uint16_t rd16(const uint8_t* p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}
inline float rdf(const uint8_t* p) {
    uint32_t v = rd32(p);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

std::string lowered(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

bool endsWith(const std::string& s, const char* suffix) {
    size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// A .points value is 1/32 of a model unit, and its axes are the authoring
// program's rather than the model's. Both facts were measured, not assumed -
// see the note at the top of the file.
const float kPointScale = 1.0f / 32.0f;

void translateMeshBy(Mesh& m, const float t[3]) {
    for (size_t i = 0; i + 2 < m.positions.size(); i += 3) {
        m.positions[i + 0] += t[0];
        m.positions[i + 1] += t[1];
        m.positions[i + 2] += t[2];
    }
    for (int c = 0; c < 3; ++c) { m.bboxMin[c] += t[c]; m.bboxMax[c] += t[c]; }
}

void pointToModel(const float in[3], float out[3]) {
    out[0] =  in[0] * kPointScale;
    out[1] =  in[2] * kPointScale;
    out[2] = -in[1] * kPointScale;
}

} // namespace

// ============================================================== containers

bool rr3InflateZ(const uint8_t* data, size_t len, Bytes& out) {
    out.clear();
    if (len < 6) return false;
    uint32_t want = rd32(data);
    // a runaway size here would otherwise ask for a multi-gigabyte allocation
    if (want > (512u << 20)) return false;
    if (!inflateZlib(data + 4, len - 4, out)) {
        out.clear();
        if (!inflateRaw(data + 4, len - 4, out)) return false;
    }
    // the stated size is a check, not a promise; trust the stream
    return !out.empty();
}

bool rr3BinFrames(const uint8_t* data, size_t len, std::vector<Bytes>& out,
                  size_t maxFrames) {
    out.clear();
    if (len < 12) return false;
    // Newer files put a 16-byte marker in front of the chain. Older ones, and
    // the QuickBMS script everybody uses, start straight at the first block.
    size_t off = (rd32(data) == 0xFFFFFFFFu) ? 16 : 0;

    while (off + 8 <= len) {
        uint32_t packed = rd32(data + off);
        uint32_t plain  = rd32(data + off + 4);
        // `packed` counts itself as well as the payload, which is why the
        // QuickBMS script subtracts four before reading
        if (packed < 5 || (size_t)off + 4 + packed > len) break;
        if (plain > (512u << 20)) break;

        Bytes frame;
        if (!inflateZlib(data + off + 8, packed - 4, frame)) {
            frame.clear();
            if (!inflateRaw(data + off + 8, packed - 4, frame)) break;
        }
        out.push_back(std::move(frame));
        off += (size_t)packed + 4;
        if (maxFrames && out.size() >= maxFrames) return true;
    }
    return !out.empty();
}

bool rr3IsContainer(const std::string& name, const uint8_t* data, size_t len) {
    std::string n = lowered(name);
    if (endsWith(n, ".bin")) {
        if (len >= 12 && rd32(data) == 0xFFFFFFFFu) return true;
        // a plain chain: the first block must look like one
        if (len >= 10) {
            uint32_t packed = rd32(data);
            if (packed >= 5 && (size_t)packed + 4 <= len &&
                data[8] == 0x78) return true;
        }
        return false;
    }
    if (!endsWith(n, ".z")) return false;
    // "<size><zlib>" - the zlib header is the giveaway
    return len >= 6 && data[4] == 0x78;
}

bool rr3Unwrap(const std::string& name, const uint8_t* data, size_t len,
               Bytes& out, std::string* innerName, size_t* frameCount) {
    out.clear();
    if (frameCount) *frameCount = 1;
    std::string n = name;
    std::string low = lowered(n);

    if (endsWith(low, ".bin")) {
        std::vector<Bytes> frames;
        // Only the first frame is needed to show the texture; counting the
        // rest means walking the chain, which is cheap next to decoding them.
        if (!rr3BinFrames(data, len, frames, 0)) return false;
        out = frames.front();
        if (frameCount) *frameCount = frames.size();
        n = n.substr(0, n.size() - 4);
        low = lowered(n);
    } else if (endsWith(low, ".z")) {
        if (!rr3InflateZ(data, len, out)) return false;
    } else {
        return false;
    }

    // a .z.bin unwraps twice: the chain, then each frame's own header
    if (endsWith(low, ".z")) {
        n = n.substr(0, n.size() - 2);
        if (out.size() >= 6 && out[4] == 0x78) {
            Bytes inner;
            if (rr3InflateZ(out.data(), out.size(), inner)) out.swap(inner);
        }
    }
    if (innerName) *innerName = n;
    return !out.empty();
}

// ============================================================== .points

// Real Racing (2009) and Real Racing GTI: no names, just a count and that
// many (lateral, fore, up) triples of int16 millimetres, in a fixed order:
// the four wheel centres, head- and taillights, exhausts, the cockpit and
// the cameras. The names below are what they turn out to be, measured on
// car_exotic (lights at the nose, exhausts at the tail, a left-hand drive
// seat).
static std::vector<Hardpoint> rr1ReadPoints(const uint8_t* data, size_t len) {
    std::vector<Hardpoint> out;
    if (len < 2 + 6 * 4) return out;
    uint16_t count = rd16(data);
    if (count < 4 || count > 256 || 2 + (size_t)count * 6 != len) return out;
    static const char* const kNames[] = {
        "POINT_WHEEL_FL", "POINT_WHEEL_FR", "POINT_WHEEL_BL", "POINT_WHEEL_BR",
        "POINT_HEADLIGHT_L", "POINT_HEADLIGHT_R", "POINT_TAILLIGHT_L", "POINT_TAILLIGHT_R",
        "POINT_EXHAUST_L", "POINT_EXHAUST_R", "POINT_STEERING_WHEEL", "POINT_DRIVER_HEAD",
        "POINT_CAMERA_CHASE", "POINT_CAMERA_COCKPIT", "POINT_CAMERA_COCKPIT_2",
        "POINT_CAMERA_FAR", "POINT_CENTRE",
    };
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* q = data + 2 + (size_t)i * 6;
        int16_t lat = (int16_t)rd16(q), fore = (int16_t)rd16(q + 2), up = (int16_t)rd16(q + 4);
        Hardpoint h;
        h.name = i < sizeof(kNames) / sizeof(kNames[0]) ? kNames[i] : "POINT_" + std::to_string(i);
        h.pos[0] = lat / 1000.0f;
        h.pos[1] = up / 1000.0f;
        h.pos[2] = -fore / 1000.0f;
        out.push_back(h);
    }
    return out;
}

std::vector<Hardpoint> rr3ReadPoints(const uint8_t* data, size_t len) {
    std::vector<Hardpoint> out;
    if (len < 4) return out;
    uint16_t version = rd16(data);
    uint16_t count   = rd16(data + 2);
    if (2 + (size_t)version * 6 == len && version >= 4) return rr1ReadPoints(data, len);
    if (version == 0 || version > 8 || count > 4096) return out;

    size_t p = 4;
    for (uint16_t i = 0; i < count && p + 4 <= len; ++i) {
        uint16_t type = rd16(data + p);
        uint16_t recLen = rd16(data + p + 2);
        p += 4;
        if (recLen == 0 || p + recLen > len) break;
        size_t rec = p;
        p += recLen;

        // name, then as many floats as the record has room for
        size_t nameLen = 0;
        while (rec + nameLen < len && nameLen < recLen && data[rec + nameLen] != 0)
            ++nameLen;
        if (nameLen + 1 > recLen) continue;
        Hardpoint h;
        h.name.assign((const char*)data + rec, nameLen);
        const uint8_t* f = data + rec + nameLen + 1;
        size_t floats = (recLen - nameLen - 1) / 4;
        if (floats < 3) continue;

        // A spline is a run of points rather than one: two uint16 (a subtype
        // and a count) then that many triples. Each point is emitted with an
        // index on its name, so the whole curve survives the export.
        if (type == 1 && recLen > nameLen + 5) {
            const uint8_t* sp = data + rec + nameLen + 1;
            size_t avail = recLen - nameLen - 1;
            uint16_t nodes = rd16(sp + 2);
            if (nodes && (size_t)nodes * 12 + 4 <= avail && nodes <= 512) {
                for (uint16_t k = 0; k < nodes; ++k) {
                    const uint8_t* q = sp + 4 + (size_t)k * 12;
                    float rawp[3] = { rdf(q), rdf(q + 4), rdf(q + 8) };
                    Hardpoint node;
                    char suffix[16];
                    snprintf(suffix, sizeof(suffix), ".%02u", (unsigned)k);
                    node.name = h.name + suffix;
                    pointToModel(rawp, node.pos);
                    out.push_back(std::move(node));
                }
                continue;
            }
        }

        float raw[3] = { rdf(f), rdf(f + 4), rdf(f + 8) };
        // Real Racing 2 writes the same records with 16.16 fixed-point
        // numbers instead of floats. Read as floats those come out as NaN or
        // as vanishingly small values, which no real position is.
        bool fixed = false;
        for (float v : raw)
            if (!std::isfinite(v) || (v != 0.0f && std::fabs(v) < 1e-30f)) fixed = true;
        if (fixed)
            for (int k = 0; k < 3; ++k)
                raw[k] = (float)(int32_t)rd32(f + 4 * k) / 65536.0f;
        pointToModel(raw, h.pos);

        // a hinge carries a 3x3 basis after the position, in the same axis
        // order, so each of its rows converts the same way a position does
        if (type == 2 && floats >= 12) {
            for (int r = 0; r < 3; ++r) {
                float row[3] = { rdf(f + 12 + r * 12),
                                 rdf(f + 16 + r * 12),
                                 rdf(f + 20 + r * 12) };
                float conv[3];
                conv[0] =  row[0];
                conv[1] =  row[2];
                conv[2] = -row[1];
                h.basis[r * 3 + 0] = conv[0];
                h.basis[r * 3 + 1] = conv[1];
                h.basis[r * 3 + 2] = conv[2];
            }
            h.hinge = true;
        }
        out.push_back(std::move(h));
    }
    return out;
}

// ============================================================== .banim
//
// A rear wing is modelled at the origin like the steering wheel, but it is not
// in the .points file - it moves, so its rest position lives in the animation
// that moves it. `<car>_wing.banim` holds that animation, and `<car>_
// animations.xml` beside it names the clips (wing_initial, pivot_up,
// pivot_down) and their frame ranges.
//
// The container is a keyframe store: a header, then runs of (value, time)
// float pairs, the times in milliseconds. A channel that holds the same value
// at every one of its keys is not animated at all - it is where the part sits
// - and the two constant channels are what this reads. Their unit is 1/65536
// of a model unit, and the axes are the .points order, so
//
//     model.y =  up   / 65536
//     model.z = -fore / 65536
//
// Checked on the 2013 Koenigsegg Agera R: the constants are -124668 and 55277,
// which put the wing at z = +1.902, y = 0.843 - mounted on its struts above
// the rear deck, exactly as the game draws it. Left at the origin it is buried
// inside the bodywork and invisible, which is what 0.7 did.
//
// Only that one file has been seen, so the reader checks its own answer: it
// wants exactly two constant channels, and it refuses a position that falls
// outside the model it is being applied to.

namespace {

const float kBanimScale = 1.0f / 65536.0f;

} // namespace

bool rr3ReadBanimRest(const uint8_t* data, size_t len, float out[3],
                      std::string* note) {
    if (len < 160 || data[0] != 0x10 || data[1] != 0x7F ||
        data[2] != 0x7F || data[3] != 0x7F) {
        if (note) *note = "not a .banim file";
        return false;
    }

    // Walk the (value, time) pairs and group them into runs of one value.
    // Times are milliseconds and never negative, which is what tells a value
    // apart from a time when a run is only one pair long.
    struct Run { float value; int keys; };
    std::vector<Run> runs;
    for (size_t p = 144; p + 8 <= len; p += 8) {
        float value = rdf(data + p);
        float time  = rdf(data + p + 4);
        if (!(time >= 0.0f && time < 600000.0f)) break;      // out of keyframes
        if (!(std::fabs(value) < 1e9f)) break;
        if (!runs.empty() && runs.back().value == value) runs.back().keys++;
        else runs.push_back(Run{value, 1});
        if (runs.size() > 4096) break;
    }

    // The transform channels come first and hold one value across all four of
    // the clip's key times; the animated channel follows, stepping every
    // 33 ms, and can flatten off for a few keys near the top of its curve -
    // so a run has to be as long as the clip's own key count to be a
    // transform, and only the first two are taken.
    size_t longest = 0;
    for (const Run& r : runs) longest = std::max(longest, (size_t)r.keys);
    std::vector<float> constants;
    for (const Run& r : runs) {
        if (r.value == 0.0f || r.keys < 4) continue;
        if ((size_t)r.keys < longest && longest >= 4 && r.keys < 4) continue;
        constants.push_back(r.value);
        if (constants.size() == 2) break;
    }

    if (constants.size() < 2) {
        if (note) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "%zu constant channel(s) in the .banim, needed 2 - not placed",
                     constants.size());
            *note = buf;
        }
        return false;
    }
    out[0] = 0.0f;
    out[1] =  constants[1] * kBanimScale;
    out[2] = -constants[0] * kBanimScale;
    return true;
}

// The format, read off five cars (Agera R, LaFerrari, P1, 911 Turbo S,
// Nevera):
//   0x00  magic: 10 7F 7F 7F (then u32 3 and u32 fps) or 00 7F 7F 7F (then
//         u32 fps) - the P1 has the older one
//         u32 node count, f32 length in ms, u32 (key count)
//   per node: u32 (its index), then 14 x (u32 key count, u32 first key)
//   the keys: (f32 time ms, f32 value) pairs, 8 bytes each
//   then an event table (AE_CAR ... AE_LOOPING) the viewer does not need.
// The 14 channels are 7 pairs; the first of each pair is the curve itself
// (the second holds its tangents, or zeros). The seven curves: lateral,
// fore, up in 1/65536 of a unit; then rotations in 1/65536 of a turn, the
// first about the car's width - the wing's pitch. The nodes are the moving
// parts in name order: WING_REAR before WING_STRUT_REAR. On the 911 the wing
// tilts and its strut does not, which is what pins that order.
bool rr3ReadBanim(const uint8_t* d, size_t len, PartAnim& out, std::string* note) {
    out = PartAnim();
    if (len < 0x90 || d[1] != 0x7F || d[2] != 0x7F || d[3] != 0x7F || (d[0] != 0x10 && d[0] != 0x00)) {
        if (note) *note = "not a .banim file";
        return false;
    }
    size_t o;
    uint32_t nodes;
    if (d[0] == 0x10) { nodes = rd32(d + 0x0c); out.duration = rdf(d + 0x10); o = 0x18; }
    else              { nodes = rd32(d + 0x08); out.duration = rdf(d + 0x0c); o = 0x14; }
    if (nodes == 0 || nodes > 64 || !(out.duration > 0 && out.duration < 600000)) {
        if (note) *note = "the .banim header is out of range";
        return false;
    }
    std::vector<std::array<std::pair<uint32_t, uint32_t>, 14>> tabs(nodes);
    for (uint32_t n = 0; n < nodes; ++n) {
        o += 4;
        if (o + 14 * 8 > len) { if (note) *note = "the .banim is cut short"; return false; }
        for (int c = 0; c < 14; ++c) tabs[n][c] = { rd32(d + o + c * 8), rd32(d + o + c * 8 + 4) };
        o += 14 * 8;
    }
    size_t base = o;
    out.nodes.resize(nodes);
    for (uint32_t n = 0; n < nodes; ++n)
        for (int c = 0; c < 7; ++c) {
            uint32_t cnt = tabs[n][c * 2].first, first = tabs[n][c * 2].second;
            if (cnt > 100000 || base + ((size_t)first + cnt) * 8 > len) {
                if (note) *note = "a .banim channel runs past the end of the file";
                return false;
            }
            for (uint32_t k = 0; k < cnt; ++k) {
                const uint8_t* q = d + base + ((size_t)first + k) * 8;
                out.nodes[n].ch[c].push_back({ rdf(q), rdf(q + 4) });
            }
        }
    return true;
}

namespace {

float sampleChannel(const std::vector<std::pair<float, float>>& k, float t) {
    if (k.empty()) return 0;
    if (t <= k.front().first) return k.front().second;
    if (t >= k.back().first) return k.back().second;
    for (size_t i = 1; i < k.size(); ++i)
        if (t <= k[i].first) {
            float t0 = k[i - 1].first, t1 = k[i].first;
            float a = t1 > t0 ? (t - t0) / (t1 - t0) : 1.0f;
            return k[i - 1].second + (k[i].second - k[i - 1].second) * a;
        }
    return k.back().second;
}

// a node's transform at time t, 4x4 column-major: rotation (pitch about x,
// then about y and z) then the translation
void nodeMatrix(const PartAnim::Node& n, float t, float m[16]) {
    const float kTurn = 6.28318530718f / 65536.0f;
    // the file's axes are (lateral, fore, up); the model's (x, up, -fore).
    // A turn about fore is one about -z, a turn about up one about y.
    float ax = sampleChannel(n.ch[3], t) * kTurn;
    float az = -sampleChannel(n.ch[4], t) * kTurn;
    float ay = sampleChannel(n.ch[5], t) * kTurn;
    float tx = sampleChannel(n.ch[0], t) / 65536.0f;
    float ty = sampleChannel(n.ch[2], t) / 65536.0f;
    float tz = -sampleChannel(n.ch[1], t) / 65536.0f;
    float cx = std::cos(ax), sx = std::sin(ax), cy = std::cos(ay), sy = std::sin(ay),
          cz = std::cos(az), sz = std::sin(az);
    // R = Rz * Ry * Rx
    float r[9] = { cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx,
                   sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx,
                   -sy,     cy * sx,                cy * cx };
    for (int i = 0; i < 16; ++i) m[i] = 0;
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) m[col * 4 + row] = r[row * 3 + col];
    m[12] = tx; m[13] = ty; m[14] = tz; m[15] = 1;
}

void matMul(const float a[16], const float b[16], float o[16]) {
    float r[16];
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr) {
            float v = 0;
            for (int k = 0; k < 4; ++k) v += a[k * 4 + rr] * b[c * 4 + k];
            r[c * 4 + rr] = v;
        }
    memcpy(o, r, sizeof(r));
}

// inverse of a rotation + translation
void rigidInverse(const float m[16], float o[16]) {
    float r[16];
    for (int i = 0; i < 16; ++i) r[i] = 0;
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) r[col * 4 + row] = m[row * 4 + col];
    for (int row = 0; row < 3; ++row)
        r[12 + row] = -(r[0 * 4 + row] * m[12] + r[1 * 4 + row] * m[13] + r[2 * 4 + row] * m[14]);
    r[15] = 1;
    memcpy(o, r, sizeof(r));
}

void transformMesh(Mesh& mesh, const float m[16]) {
    for (size_t i = 0; i + 2 < mesh.positions.size(); i += 3) {
        float x = mesh.positions[i], y = mesh.positions[i + 1], z = mesh.positions[i + 2];
        mesh.positions[i]     = m[0] * x + m[4] * y + m[8] * z + m[12];
        mesh.positions[i + 1] = m[1] * x + m[5] * y + m[9] * z + m[13];
        mesh.positions[i + 2] = m[2] * x + m[6] * y + m[10] * z + m[14];
    }
    for (size_t i = 0; i + 2 < mesh.normals.size(); i += 3) {
        float x = mesh.normals[i], y = mesh.normals[i + 1], z = mesh.normals[i + 2];
        mesh.normals[i]     = m[0] * x + m[4] * y + m[8] * z;
        mesh.normals[i + 1] = m[1] * x + m[5] * y + m[9] * z;
        mesh.normals[i + 2] = m[2] * x + m[6] * y + m[10] * z;
    }
    for (int c = 0; c < 3; ++c) { mesh.bboxMin[c] = 1e30f; mesh.bboxMax[c] = -1e30f; }
    for (size_t i = 0; i + 2 < mesh.positions.size(); i += 3)
        for (int c = 0; c < 3; ++c) {
            mesh.bboxMin[c] = std::min(mesh.bboxMin[c], mesh.positions[i + c]);
            mesh.bboxMax[c] = std::max(mesh.bboxMax[c], mesh.positions[i + c]);
        }
}

// LOD_A_WING_REAR_mm_ext -> WING_REAR
std::string animPartBase(const Mesh& mesh) {
    std::string n = mesh.name;
    if (n.size() > 6 && n.compare(0, 4, "LOD_") == 0 && n[5] == '_') n = n.substr(6);
    size_t mm = n.find("_mm_");
    if (mm != std::string::npos) n = n.substr(0, mm);
    for (char& c : n) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return n;
}

} // namespace

void partAnimDelta(const Model& m, const Mesh& mesh, float t, float out[16]) {
    for (int i = 0; i < 16; ++i) out[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    if (mesh.animIndex < 0 || (size_t)mesh.animIndex >= m.anims.size()) return;
    const PartAnim& a = m.anims[mesh.animIndex];
    if (mesh.animNode < 0 || (size_t)mesh.animNode >= a.nodes.size()) return;
    float now[16], rest[16], inv[16];
    nodeMatrix(a.nodes[mesh.animNode], t, now);
    nodeMatrix(a.nodes[mesh.animNode], 0, rest);
    rigidInverse(rest, inv);
    matMul(now, inv, out);
}

float modelAnimDuration(const Model& m) {
    float d = 0;
    for (const PartAnim& a : m.anims) d = std::max(d, a.duration);
    return d;
}

void poseModel(Model& m, float t) {
    for (Mesh& mesh : m.meshes) {
        if (mesh.animIndex < 0) continue;
        float d[16];
        partAnimDelta(m, mesh, t, d);
        transformMesh(mesh, d);
    }
}

void rr3PlaceAnimated(Model& m, const uint8_t* banim, size_t banimLen,
                      const std::string& partHint) {
    // the whole animation first: every node, rotation included
    {
        PartAnim anim;
        std::string why;
        if (rr3ReadBanim(banim, banimLen, anim, &why)) {
            std::string want = partHint;
            for (char& c : want) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            // the moving parts: modelled at the origin, named for the hint
            std::vector<std::string> bases;
            for (const Mesh& mesh : m.meshes) {
                if (mesh.animIndex >= 0) continue;
                std::string b = animPartBase(mesh);
                if (!want.empty() && b.find(want) == std::string::npos) continue;
                bool atOrigin = true;
                for (int c = 0; c < 3; ++c)
                    if (std::fabs((mesh.bboxMin[c] + mesh.bboxMax[c]) * 0.5f) > 0.4f) atOrigin = false;
                if (!atOrigin) continue;
                if (std::find(bases.begin(), bases.end(), b) == bases.end()) bases.push_back(b);
            }
            // WING_REAR_BRAKES rides on WING_REAR: only the roots are nodes
            std::vector<std::string> roots;
            for (const std::string& b : bases) {
                bool child = false;
                for (const std::string& o : bases)
                    if (o != b && b.size() > o.size() && b.compare(0, o.size(), o) == 0 && b[o.size()] == '_')
                        child = true;
                if (!child) roots.push_back(b);
            }
            // ... unless the animation has a node for every part: the Evija's
            // WING, WING_REAR, WING_REAR_LEFT and WING_REAR_RIGHT each move on
            // their own (four nodes), and folding them into WING stacked the
            // whole mechanism on the wing
            if (roots.size() < anim.nodes.size() && bases.size() <= anim.nodes.size()) roots = bases;
            std::sort(roots.begin(), roots.end());
            if (!roots.empty()) {
                int idx = (int)m.anims.size();
                anim.name = partHint;
                m.anims.push_back(anim);
                int placed = 0;
                for (Mesh& mesh : m.meshes) {
                    if (mesh.animIndex >= 0) continue;
                    std::string b = animPartBase(mesh);
                    int node = -1;
                    size_t bestLen = 0;
                    for (size_t r = 0; r < roots.size(); ++r)
                        if ((b == roots[r] || (b.size() > roots[r].size() &&
                                               b.compare(0, roots[r].size(), roots[r]) == 0 &&
                                               b[roots[r].size()] == '_')) && roots[r].size() > bestLen) {
                            bestLen = roots[r].size();
                            node = (int)r;
                        }
                    if (node < 0 || std::find(bases.begin(), bases.end(), b) == bases.end()) continue;
                    if (node >= (int)anim.nodes.size()) node = (int)anim.nodes.size() - 1;
                    float rest[16];
                    nodeMatrix(anim.nodes[node], 0, rest);
                    transformMesh(mesh, rest);
                    mesh.animIndex = idx;
                    mesh.animNode = node;
                    ++placed;
                }
                // they are no longer at the origin: take them off that list
                for (std::string& w : m.warnings) {
                    const std::string head = "left at the origin (no matching point): ";
                    if (w.compare(0, head.size(), head) != 0) continue;
                    std::string rest = w.substr(head.size()), kept;
                    size_t at = 0;
                    while (at <= rest.size()) {
                        size_t comma = rest.find(", ", at);
                        std::string item = rest.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                        bool gone = false;
                        for (const Mesh& me : m.meshes)
                            if (me.name == item && me.animIndex == idx) gone = true;
                        if (!gone && !item.empty()) kept += (kept.empty() ? "" : ", ") + item;
                        if (comma == std::string::npos) break;
                        at = comma + 2;
                    }
                    w = kept.empty() ? std::string() : head + kept;
                }
                m.warnings.erase(std::remove(m.warnings.begin(), m.warnings.end(), std::string()),
                                 m.warnings.end());
                char buf[200];
                snprintf(buf, sizeof(buf),
                         "%d part(s) placed from the %s animation (%zu node(s), %.2f s) - "
                         "press Animate to see it move", placed, partHint.c_str(), anim.nodes.size(),
                         anim.duration / 1000.0);
                m.warnings.push_back(buf);
                return;
            }
        }
    }
    float pos[3];
    std::string note;
    if (!rr3ReadBanimRest(banim, banimLen, pos, &note)) {
        if (!note.empty()) m.warnings.push_back(note);
        return;
    }

    // where the model as a whole is, so a nonsense answer can be rejected
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const Mesh& mesh : m.meshes)
        for (int c = 0; c < 3; ++c) {
            lo[c] = std::min(lo[c], mesh.bboxMin[c]);
            hi[c] = std::max(hi[c], mesh.bboxMax[c]);
        }
    if (std::fabs(pos[1]) + std::fabs(pos[2]) < 0.2f) return;   // no offset stated
    for (int c = 1; c < 3; ++c) {
        float slack = 0.5f * (hi[c] - lo[c]) + 0.25f;
        if (pos[c] < lo[c] - slack || pos[c] > hi[c] + slack) {
            m.warnings.push_back(
                "the .banim rest position falls outside the model, so the "
                "part was left where it is");
            return;
        }
    }

    std::string want = lowered(partHint);
    int moved = 0;
    for (Mesh& mesh : m.meshes) {
        std::string low = lowered(mesh.name);
        if (!want.empty() && low.find(want) == std::string::npos) continue;
        // only a part that is still sitting on the origin needs placing
        float centre[3];
        bool atOrigin = true;
        for (int c = 0; c < 3; ++c) {
            centre[c] = (mesh.bboxMin[c] + mesh.bboxMax[c]) * 0.5f;
            if (std::fabs(centre[c]) > 0.25f) atOrigin = false;
        }
        if (!atOrigin) continue;
        translateMeshBy(mesh, pos);
        ++moved;
    }
    if (moved) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "%d part(s) placed from the .banim rest pose (%.3f, %.3f, %.3f)",
                 moved, pos[0], pos[1], pos[2]);
        m.warnings.push_back(buf);
    }
}


// ---- Real Racing 3's driver ----
//
// driver/driver_lod_a.m3g (and _b, _c_<car>, _d_<car>) is fifteen parts, each
// modelled round the origin: 00_helmet, 01_body, 02_groin, the arms, hands and
// legs. Its .banim (driver.banim, or driver_<car>.banim for a car with its own
// seating) has exactly fifteen nodes, one per part in that order, and the
// pose at time 0 puts each part where it belongs.
std::string rr3DriverAnimNameFor(const std::string& modelPath) {
    std::string leaf = baseName(stripExtension(modelPath));
    std::string low = lowered(leaf);
    if (low.compare(0, 11, "driver_lod_") != 0) return "";
    std::string dir = modelPath.substr(0, modelPath.size() - baseName(modelPath).size());
    // driver_lod_c_f1_mp44 -> driver_f1_mp44.banim; driver_lod_a -> driver.banim
    if (low.size() > 13 && low[12] == '_') return dir + "driver_" + leaf.substr(13) + ".banim";
    return dir + "driver.banim";
}

bool rr3PoseDriver(Model& m, const uint8_t* banim, size_t len) {
    PartAnim anim;
    std::string why;
    if (!rr3ReadBanim(banim, len, anim, &why)) {
        m.warnings.push_back("driver pose: " + why);
        return false;
    }
    // the far detail levels (driver_lod_c, _d) are one mesh already in the
    // seat: only the fifteen-part rig is posed
    int parts = 0;
    for (const Mesh& mesh : m.meshes)
        if (mesh.name.size() > 2 && isdigit((unsigned char)mesh.name[0]) && isdigit((unsigned char)mesh.name[1])) ++parts;
    if (parts * 2 < (int)anim.nodes.size()) return false;
    int idx = (int)m.anims.size();
    int placed = 0;
    for (Mesh& mesh : m.meshes) {
        const std::string& n = mesh.name;
        if (n.size() < 3 || !isdigit((unsigned char)n[0]) || !isdigit((unsigned char)n[1]) || n[2] != '_') continue;
        int node = (n[0] - '0') * 10 + (n[1] - '0');
        if (node >= (int)anim.nodes.size()) continue;
        float rest[16];
        nodeMatrix(anim.nodes[node], 0, rest);
        transformMesh(mesh, rest);
        mesh.animIndex = idx;
        mesh.animNode = node;
        ++placed;
    }
    if (!placed) return false;
    anim.name = "driver";
    m.anims.push_back(anim);
    char buf[160];
    snprintf(buf, sizeof(buf), "driver: %d part(s) posed from the .banim (%.2f s) - press Animate to see him steer",
             placed, anim.duration / 1000.0);
    m.warnings.push_back(buf);
    return true;
}


// ============================================================== .nct
//
// `106_2011_pagani_huayra.bin.nct` is a car's data file: the underlying thing
// is a `.bin`, and `.nct` is a layer on top of it. It is not compressed and it
// is not really encrypted either - it is one fixed pad XORed over the file
// from byte 0, the same pad for every car:
//
//     plain[i] = cipher[i] ^ PAD[i]
//
// Which is why XORing any two .nct files together cancels the pad and leaves
// readable game data - that is how the pad below was recovered, from four
// cars, with the plaintext cross-checked four ways at a time. Two examples of
// what pins a byte exactly: bytes 4..7 are the car id and read 106, 107, 108,
// 109 across the four samples, and bytes 12..15 are the model year as ASCII
// and read "2011", "2005", "2009", "2010". Both fix their pad bytes with no
// freedom left.
//
// What is honest about this: 237 of the 1069 pad bytes are pinned that way.
// The rest were solved statistically against the file's own structure and a
// few of them are wrong, which shows up as a garbled character inside an
// otherwise readable string. The pad generator itself was not identified -
// it is aperiodic over 1069 bytes, and a repeating key of every length up to
// 534, several LCGs, xorshift, java.util.Random and glibc rand were each
// tested and ruled out - so anything past 1069 bytes cannot be decoded at all.
// More .nct files would fix both limits.
//
// Editing works regardless: the same XOR encodes as it decodes, so a byte the
// user does not touch comes back exactly as it went in, whether or not this
// pad has that byte right.

namespace {
#include "nct_pad.inc"

// The pad shipped above came from four cars. A whole game holds one .nct per
// car, so the tool can finish the job off the user's own files - see
// nctLearnPad below. What it learns lives here and takes precedence.
std::vector<uint8_t> g_padLearned;
std::vector<uint8_t> g_padLearnedSure;    // one byte per offset, 0 or 1
bool g_ignoreShipped = false;             // only for testing the learner

bool nctPadSure(size_t i) {
    if (i < g_padLearnedSure.size() && g_padLearnedSure[i]) return true;
    if (g_ignoreShipped) return false;
    return i < kNctPadLen && (kNctPadSure[i >> 3] >> (i & 7)) & 1;
}

// the pad byte in force at an offset, learned first, shipped second
bool padAt(size_t i, uint8_t& b) {
    if (i < g_padLearned.size() && i < g_padLearnedSure.size() && g_padLearnedSure[i]) {
        b = g_padLearned[i];
        return true;
    }
    if (i < kNctPadLen && !g_ignoreShipped) { b = kNctPad[i]; return true; }
    if (i < g_padLearned.size()) { b = g_padLearned[i]; return true; }
    return false;
}
} // namespace

bool nctPadByteKnown(size_t i) { return nctPadSure(i); }

size_t nctPadLength() {
    return std::max(kNctPadLen, (size_t)g_padLearned.size());
}

size_t nctPadKnownBytes() {
    size_t n = 0, len = nctPadLength();
    for (size_t i = 0; i < len; ++i) if (nctPadSure(i)) ++n;
    return n;
}

bool nctTransform(const uint8_t* data, size_t len, Bytes& out, size_t* covered) {
    out.assign(data, data + len);
    size_t n = 0;
    for (size_t i = 0; i < len; ++i) {
        uint8_t b;
        if (!padAt(i, b)) break;
        out[i] = (uint8_t)(data[i] ^ b);
        ++n;
    }
    if (covered) *covered = n;
    return n > 0;
}

// Finish the pad from a whole game's worth of .nct files.
//
// The cipher is one fixed pad XORed over every file, so at a given offset two
// cars produce the same ciphertext byte exactly when their plaintext bytes
// agree. These are struct-shaped data files: most offsets hold a zero in most
// cars, so the commonest ciphertext byte at an offset is the pad byte itself.
// One car cannot show that; two hundred can, and the margin says how firmly.
//
// Nothing is taken on faith. The bytes already pinned by known plaintext are
// used as a scoreboard: the same rule is run against them, and if it does not
// reproduce them it is not trusted for the rest either. That agreement figure
// is reported rather than hidden.
namespace {

// ---- evidence for the pad --------------------------------------------------
//
// Every clue about the pad is a vote: "at offset i, file f holds plaintext
// byte p", which says pad[i] = cipher_f[i] ^ p. Votes carry a weight for how
// much the clue is worth, and the pad byte at an offset is the value with the
// most weight behind it. Nothing is ever pinned outright by a single clue, so
// one wrong guess is outvoted by the files that disagree with it instead of
// spreading - which is what went wrong with plain crib dragging.
struct PadVotes {
    std::vector<std::array<float, 256>> w;
    explicit PadVotes(size_t n) : w(n) { for (auto& a : w) a.fill(0.0f); }
    void add(const Bytes& f, size_t off, uint8_t plain, float weight) {
        if (off < f.size() && off < w.size()) w[off][f[off] ^ plain] += weight;
    }
    int best(size_t i, float* top = nullptr, float* second = nullptr) const {
        int b = -1; float t = 0, s2 = 0;
        for (int v = 0; v < 256; ++v) {
            float x = w[i][v];
            if (x > t) { s2 = t; t = x; b = v; } else if (x > s2) s2 = x;
        }
        if (top) *top = t;
        if (second) *second = s2;
        return b;
    }
};

// The car a file belongs to, as its own name spells it:
//   "1969_dodge_charger_rt.liveries.bin.nct" -> "1969_dodge_charger_rt"
std::string nctStem(const std::string& name) {
    std::string b = baseName(name);
    for (const char* tail : {".liveries.bin.nct", ".bin.nct", ".nct", ".gui"})
        if (endsWith(b, tail)) return b.substr(0, b.size() - strlen(tail));
    return b;
}

bool pathChar(uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '/' ||
           c == '.' || c == '-' || (c >= 'A' && c <= 'Z');
}

// A liveries file, as far as its layout is known:
//
//     u32  ?            (differs per car)
//     the car's name    (no length, no terminator - "1969_nissan_z_432")
//     a few bytes       (a count; not needed here)
//     records:  u32 texture id, u32 length, that many characters
//
// and every one of those strings is a texture path ending ".ptc.pvr.z". The
// walk below uses nothing else: it finds where a record's string must end so
// that it ends in ".ptc.pvr.z" and the next record's zero bytes line up, and
// votes for what that says - the zeros, the length byte, the tail - and for
// the path's opening when it matches the car's own name.
void walkLiveries(const Bytes& f, const std::string& stem, const std::vector<uint8_t>& pad,
                  const std::vector<uint8_t>& trusted, PadVotes& V, size_t* records) {
    auto known = [&](size_t i) { return i < trusted.size() && trusted[i]; };
    static const char kTail[] = ".ptc.pvr.z";
    const size_t T = 10;
    auto P = [&](size_t i) -> uint8_t { return i < pad.size() ? (uint8_t)(f[i] ^ pad[i]) : 0; };
    auto zeros = [&](size_t q) {
        if (q + 8 > f.size()) return 2.5f;
        int z = 0;
        for (size_t k : {2, 3, 5, 6, 7}) if (P(q + k) == 0) ++z;
        return (float)z;
    };
    size_t q = 4 + stem.size();
    float bestZ = -1; size_t start = q;
    for (size_t s = q; s < q + 12 && s + 8 <= f.size(); ++s) {
        float z = zeros(s);
        if (z > bestZ) { bestZ = z; start = s; }
    }
    q = start;
    while (q + 8 + T < f.size()) {
        float bestScore = -1; size_t bestLen = 0; int bestTail = 0;
        for (size_t ln = T + 1; ln < 96 && q + 8 + ln <= f.size(); ++ln) {
            size_t e = q + 8 + ln;
            int tail = 0;
            for (size_t k = 0; k < T; ++k) if (P(e - T + k) == (uint8_t)kTail[k]) ++tail;
            int chars = 0;
            for (size_t k = q + 8; k < e - T; ++k) if (pathChar(P(k))) ++chars;
            float score = tail * 2.0f + zeros(e) * 1.5f + (P(q + 4) == ln ? 4.0f : 0.0f) +
                          3.0f * chars / (float)(ln - T);
            if (score > bestScore) { bestScore = score; bestLen = ln; bestTail = tail; }
        }
        // Verified: the string ends in ".ptc.pvr.z" where the length says.
        // Otherwise, when the length byte itself is already trusted (another
        // car's name covers that offset), take it on its word and cast the
        // same votes at a lower weight: this is how the walk gets past the
        // first record before anything further in is known. A wrong guess
        // here implies a different pad value in every file and is outvoted;
        // a right one implies the same value every time and adds up.
        float wgt = 2.0f;
        if (bestScore < 16.0f || bestTail < 5) {
            uint8_t ln = P(q + 4);
            if (!known(q + 4) || ln <= T || ln >= 96 || q + 8 + ln > f.size()) break;
            int hdrZeros = 0, hdrKnown = 0;
            for (size_t k : {2, 3, 5, 6, 7})
                if (known(q + k)) { ++hdrKnown; if (P(q + k) == 0) ++hdrZeros; }
            if (hdrZeros < hdrKnown) break;
            bestLen = ln;
            wgt = 0.7f;
        }
        size_t e = q + 8 + bestLen;
        for (size_t k : {2, 3, 5, 6, 7}) V.add(f, q + k, 0, wgt);
        V.add(f, q + 4, (uint8_t)bestLen, wgt);
        for (size_t k = 0; k < T; ++k) V.add(f, e - T + k, (uint8_t)kTail[k], wgt);
        // the path's opening, when it is one the game uses and this car's own
        size_t bodyLen = bestLen - T;
        for (const std::string& pre : { "livery/" + stem + "_ext_", "livery/" + stem,
                                        stem + "_", std::string("common/car_"),
                                        std::string("common/"), std::string("vehicle/") }) {
            if (pre.size() > bodyLen) continue;
            size_t agree = 0;
            for (size_t k = 0; k < pre.size(); ++k) if (P(q + 8 + k) == (uint8_t)pre[k]) ++agree;
            if (agree >= 4 && agree * 10 >= pre.size() * 7) {
                for (size_t k = 0; k < pre.size(); ++k) V.add(f, q + 8 + k, (uint8_t)pre[k], 1.5f);
                break;
            }
            // unverifiable yet: a faint vote for each opening the game uses,
            // which only counts once other files have said the same
            size_t unknownHere = 0;
            for (size_t k = 0; k < pre.size(); ++k) if (!known(q + 8 + k)) ++unknownHere;
            if (unknownHere * 2 > pre.size())
                for (size_t k = 0; k < pre.size(); ++k) V.add(f, q + 8 + k, (uint8_t)pre[k], 0.3f);
        }
        if (records) ++*records;
        q = e;
    }
}

} // namespace

bool nctLearnPad(const std::vector<Bytes>& files, std::string& report) {
    return nctLearnPad(files, std::vector<std::string>(), report);
}

bool nctLearnPad(const std::vector<Bytes>& files, const std::vector<std::string>& names,
                 std::string& report, bool useShipped) {
    report.clear();
    size_t usable = 0, longest = 0;
    std::vector<size_t> sizes;
    for (const Bytes& f : files) {
        if (f.size() < 16) continue;
        ++usable;
        sizes.push_back(f.size());
    }
    // Only offsets that several files reach can be voted on. One 14 MB
    // events table would otherwise ask for 14 GB of vote counters (the crash
    // behind "Finish the .nct pad"): the pad is learned as far as the eighth
    // longest file reaches, and never past 1 MB.
    if (!sizes.empty()) {
        std::sort(sizes.begin(), sizes.end(), std::greater<size_t>());
        longest = std::min<size_t>(sizes[std::min<size_t>(7, sizes.size() - 1)], (size_t)1 << 20);
    }
    if (usable < 8 || !longest) {
        report = "Not enough .nct / .gui files to work from - at least eight are "
                 "needed, and a whole game holds one per car.";
        return false;
    }
    bool named = names.size() == files.size();
    auto nameOf = [&](size_t k) { return named ? baseName(names[k]) : std::string(); };

    // The shipped pins count, but for less than a car's own name or a
    // verified record: some of them came from crib dragging on only twelve
    // files, and the structure of a whole game is the better witness.
    const float kShippedWeight = 1.2f;

    // ---- the evidence that does not depend on the pad ----
    PadVotes base(longest);
    // 1. what the shipped pad already pins from plaintext
    if (useShipped)
        for (size_t i = 0; i < longest && i < kNctPadLen; ++i)
            if ((kNctPadSure[i >> 3] >> (i & 7)) & 1) base.w[i][kNctPad[i]] += kShippedWeight;
    // 2. struct padding: in the per-car data files (not the liveries lists,
    //    not the .gui XML) the commonest ciphertext byte is the pad itself.
    //    Unanimous offsets are left out - padding and a shared constant look
    //    alike there - and so are offsets too few files reach.
    for (size_t i = 0; i < longest; ++i) {
        int count[256] = {0}, total = 0;
        for (size_t k = 0; k < files.size(); ++k) {
            const Bytes& f = files[k];
            if (i >= f.size() || f.size() < 16) continue;
            std::string n = nameOf(k);
            if (named && (endsWith(n, ".gui") || n.find(".liveries.") != std::string::npos))
                continue;
            ++count[f[i]]; ++total;
        }
        if (total < 5) continue;
        int b = 0, bn = 0, sn = 0;
        for (int v = 0; v < 256; ++v) {
            if (count[v] > bn) { sn = bn; bn = count[v]; b = v; }
            else if (count[v] > sn) sn = count[v];
        }
        if (bn * 2 > total && bn >= sn * 2 && bn + 2 < total)
            base.w[i][b] += std::min(6.0f, 0.8f * (bn - 1));
    }
    // 3. the car's own name, which a liveries file carries at byte 4, and a
    //    car file's model year at byte 12 (its name starts "107_2005_...")
    for (size_t k = 0; k < files.size() && named; ++k) {
        std::string n = nameOf(k), stem = nctStem(n);
        const Bytes& f = files[k];
        if (n.find(".liveries.") != std::string::npos) {
            for (size_t c = 0; c < stem.size(); ++c) base.add(f, 4 + c, (uint8_t)stem[c], 5.0f);
            for (size_t c = 1; c < 4; ++c) base.add(f, c, 0, 1.0f);
        } else if (endsWith(n, ".bin.nct")) {
            size_t u = stem.find('_');
            if (u != std::string::npos && u + 5 <= stem.size()) {
                std::string year = stem.substr(u + 1, 4);
                bool digits = true;
                for (char c : year) if (c < '0' || c > '9') digits = false;
                if (digits)
                    for (size_t c = 0; c < 4; ++c) base.add(f, 12 + c, (uint8_t)year[c], 3.0f);
            }
        } else if (endsWith(n, ".gui")) {
            static const char kXml[] = "<?xml version=\"1.0\" encoding=\"utf-8\"?>";
            for (size_t c = 0; c + 1 < sizeof(kXml); ++c) base.add(f, c, (uint8_t)kXml[c], 1.0f);
        }
    }

    // 4. the .gui files are XML: text, all of it. At an offset several of them
    //    reach, only one pad byte turns every one of them into characters
    //    that XML is made of; a byte that makes one of them a control code
    //    is out. Scored with how common each character is in these files, so
    //    that between two all-text readings the likelier one wins.
    if (named) {
        std::vector<size_t> gui;
        for (size_t k = 0; k < files.size(); ++k)
            if (endsWith(nameOf(k), ".gui")) gui.push_back(k);
        static float logp[256];
        static bool init = false;
        if (!init) {
            for (int c = 0; c < 256; ++c) logp[c] = -30.0f;           // impossible
            for (int c = 0x20; c < 0x7F; ++c) logp[c] = -6.0f;       // any printable
            for (const char* p = "abcdefghijklmnopqrstuvwxyz"; *p; ++p) logp[(uint8_t)*p] = -3.2f;
            for (const char* p = "etaoinsrl_"; *p; ++p) logp[(uint8_t)*p] = -2.6f;
            for (const char* p = "0123456789"; *p; ++p) logp[(uint8_t)*p] = -3.5f;
            for (const char* p = "\"= </>\t\n"; *p; ++p) logp[(uint8_t)*p] = -2.4f;
            logp['\r'] = -4.0f;
            for (const char* p = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"; *p; ++p) logp[(uint8_t)*p] = -4.2f;
            init = true;
        }
        for (size_t i = 0; i < longest && gui.size() >= 6; ++i) {
            size_t reach = 0;
            for (size_t k : gui) if (i < files[k].size()) ++reach;
            if (reach < 6) continue;
            float best = -1e30f, second = -1e30f; int bestB = -1;
            for (int b = 0; b < 256; ++b) {
                float sc = 0;
                for (size_t k : gui)
                    if (i < files[k].size()) sc += logp[files[k][i] ^ b];
                if (sc > best) { second = best; best = sc; bestB = b; }
                else if (sc > second) second = sc;
            }
            // every file text, and clearly ahead of the runner-up
            if (bestB >= 0 && best > -6.5f * reach && best - second >= 3.0f)
                base.w[i][bestB] += std::min(4.0f, 1.0f + 0.1f * reach);
        }
    }

    // ---- then the structure, walked with the pad as it stands, repeatedly ----
    std::vector<uint8_t> pad(longest, 0);
    for (size_t i = 0; i < longest; ++i) {
        int b = base.best(i);
        uint8_t shipped = 0;
        if (b >= 0) pad[i] = (uint8_t)b;
        else if (useShipped && padAt(i, shipped)) pad[i] = shipped;
    }
    PadVotes V = base;
    size_t records = 0;
    auto trustedFrom = [&](const PadVotes& src) {
        std::vector<uint8_t> t(longest, 0);
        for (size_t i = 0; i < longest; ++i) {
            float top = 0, second = 0;
            if (src.best(i, &top, &second) >= 0 && top >= 2.0f && top >= 2.0f * second) t[i] = 1;
        }
        return t;
    };
    std::vector<uint8_t> trusted = trustedFrom(base);
    for (int round = 0; round < 24 && named; ++round) {
        V = base;
        records = 0;
        for (size_t k = 0; k < files.size(); ++k) {
            std::string n = nameOf(k);
            if (n.find(".liveries.") == std::string::npos) continue;
            walkLiveries(files[k], nctStem(n), pad, trusted, V, &records);
        }
        trusted = trustedFrom(V);
        size_t changed = 0;
        for (size_t i = 0; i < longest; ++i) {
            int b = V.best(i);
            if (b >= 0 && (uint8_t)b != pad[i]) { pad[i] = (uint8_t)b; ++changed; }
        }
        if (!changed) break;
    }

    // ---- what is trusted ----
    std::vector<uint8_t> sure(longest, 0);
    size_t checked = 0, agreed = 0;
    for (size_t i = 0; i < longest; ++i) {
        float top = 0, second = 0;
        int b = V.best(i, &top, &second);
        if (b < 0) continue;
        if (top >= 2.0f && top >= 2.0f * second) sure[i] = 1;
        // the scoreboard: bytes the shipped pad pins, against what the files
        // say without that pin's own vote
        if (useShipped && i < kNctPadLen && ((kNctPadSure[i >> 3] >> (i & 7)) & 1)) {
            std::array<float, 256> w = V.w[i];
            w[kNctPad[i]] -= kShippedWeight;
            int alt = -1; float t = 0;
            for (int v = 0; v < 256; ++v) if (w[v] > t) { t = w[v]; alt = v; }
            if (alt >= 0 && t >= 2.0f) { ++checked; if ((uint8_t)alt == kNctPad[i]) ++agreed; }
        }
    }
    double rate = checked ? (100.0 * agreed / checked) : -1.0;
    char buf[800];
    if (checked >= 20 && rate < 80.0) {
        snprintf(buf, sizeof(buf),
                 "Read %u file(s), but what they say disagrees with the bytes "
                 "already known from plaintext (%.0f%% agreement on %u of them). "
                 "Nothing was changed.", (unsigned)usable, rate, (unsigned)checked);
        report = buf;
        return false;
    }
    size_t before = nctPadKnownBytes(), beforeLen = nctPadLength();
    g_ignoreShipped = !useShipped;
    g_padLearned.swap(pad);
    g_padLearnedSure.swap(sure);
    size_t known = nctPadKnownBytes();

    int n = snprintf(buf, sizeof(buf),
                     "Read %u file(s)%s.\n\nThe pad now runs %u bytes, of which %u are "
                     "known - it was %u of %u.",
                     (unsigned)usable, named ? "" : " (without their names)",
                     (unsigned)longest, (unsigned)known, (unsigned)before, (unsigned)beforeLen);
    if (records && n > 0 && (size_t)n < sizeof(buf))
        n += snprintf(buf + n, sizeof(buf) - n,
                      "\n\n%u livery record(s) were walked end to end.", (unsigned)records);
    if (rate >= 0 && n > 0 && (size_t)n < sizeof(buf))
        snprintf(buf + n, sizeof(buf) - n,
                 "\n\nChecked against the bytes already pinned by known plaintext, "
                 "the files agreed on %.0f%% of %u.", rate, (unsigned)checked);
    report = buf;
    return true;
}

bool nctPadSave(const std::string& path) {
    if (g_padLearned.empty()) return false;
    Bytes out;
    out.reserve(8 + g_padLearned.size() * 2);
    const char* magic = "MTNCTPAD";
    out.insert(out.end(), magic, magic + 8);
    uint32_t n = (uint32_t)g_padLearned.size();
    for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(n >> (8 * i)));
    out.insert(out.end(), g_padLearned.begin(), g_padLearned.end());
    out.insert(out.end(), g_padLearnedSure.begin(), g_padLearnedSure.end());
    return writeFile(path, out);
}

bool nctPadLoad(const std::string& path) {
    Bytes in;
    if (!readFile(path, in) || in.size() < 12 || memcmp(in.data(), "MTNCTPAD", 8) != 0)
        return false;
    uint32_t n = rd32(in.data() + 8);
    if (!n || in.size() < 12 + (size_t)n * 2) return false;
    g_padLearned.assign(in.begin() + 12, in.begin() + 12 + n);
    g_padLearnedSure.assign(in.begin() + 12 + n, in.begin() + 12 + n * 2);
    return true;
}

namespace {

// How much of a file the pad is certain for, as a line for the text views.
std::string padCoverageLine(size_t len, size_t covered) {
    size_t sure = 0;
    for (size_t i = 0; i < len && i < covered; ++i) if (nctPadSure(i)) ++sure;
    char buf[320];
    snprintf(buf, sizeof(buf),
             "Pad: %u of this file's %u bytes are decoded, %u of them certain (%u%%).%s\r\n",
             (unsigned)covered, (unsigned)len, (unsigned)sure,
             (unsigned)(len ? sure * 100 / len : 0),
             sure < len ? " A '?' marks text that runs over an uncertain byte." : "");
    return buf;
}

bool rangeSure(size_t from, size_t to) {
    for (size_t k = from; k < to; ++k) if (!nctPadSure(k)) return false;
    return true;
}

std::string printable(const uint8_t* p, size_t n) {
    std::string v;
    for (size_t k = 0; k < n; ++k) v += (p[k] >= 0x20 && p[k] < 0x7F) ? (char)p[k] : '.';
    return v;
}

} // namespace

bool nctEncode(const uint8_t* plain, size_t len, Bytes& out,
               size_t* uncertain, size_t* pastPad) {
    size_t covered = 0;
    nctTransform(plain, len, out, &covered);     // the XOR is its own inverse
    if (pastPad) *pastPad = len - std::min(len, covered);
    if (uncertain) {
        size_t n = 0;
        for (size_t i = 0; i < covered; ++i) if (!nctPadSure(i)) ++n;
        *uncertain = n;
    }
    return covered == len;
}

NctLiveries nctReadLiveries(const uint8_t* plain, size_t len) {
    NctLiveries L;
    if (len < 16) return L;
    L.header = rd32(plain);
    size_t p = 4;
    while (p < len && p < 4 + 96 && pathChar(plain[p]) && plain[p] != '/') L.car += (char)plain[p++];
    if (L.car.size() < 3 || p + 4 > len) return L;
    L.count = rd32(plain + p);
    p += 4;
    L.tableStart = p;
    for (uint32_t r = 0; r < L.count && r < 4096; ++r) {
        if (p + 8 > len) break;
        uint32_t id = rd32(plain + p), n = rd32(plain + p + 4);
        if (n == 0 || n > 255 || p + 8 + n > len) {
            // A header the pad has wrong: the path still ends ".ptc.pvr.z",
            // so find that and carry on rather than dropping the rest of the
            // table. The record is marked uncertain either way.
            static const char kTail[] = ".ptc.pvr.z";
            size_t e = 0;
            for (size_t k = p + 8 + 10; k + 0 <= len && k <= p + 8 + 120; ++k)
                if (k >= 10 && !memcmp(plain + k - 10, kTail, 10)) { e = k; break; }
            if (!e) break;
            n = (uint32_t)(e - p - 8);
        }
        NctLiveries::Texture t;
        t.id = id;
        t.offset = p;
        t.path.assign((const char*)plain + p + 8, n);
        t.sure = rangeSure(p, p + 8 + n);
        L.textures.push_back(t);
        p += 8 + n;
    }
    L.tableEnd = p;
    L.ok = !L.textures.empty();
    return L;
}

std::string nctText(const std::string& name, const uint8_t* data, size_t len) {
    std::string s;
    char buf[640];
    snprintf(buf, sizeof(buf), "%s\r\n%u bytes\r\n\r\n", name.c_str(), (unsigned)len);
    s += buf;
    std::string ext = extensionOf(name);

    // a .gui that was never encoded is XML already
    if (ext == "gui" && len >= 5 && !memcmp(data, "<?xml", 5)) {
        s += "(stored as plain XML - no pad needed)\r\n\r\n";
        for (size_t i = 0; i < len; ++i) {
            if (data[i] == '\n' && (i == 0 || data[i - 1] != '\r')) s += '\r';
            s += (char)data[i];
        }
        return s;
    }

    Bytes plain;
    size_t covered = 0;
    if (!nctTransform(data, len, plain, &covered)) return s + "empty file\r\n";
    s += padCoverageLine(len, covered);
    s += "\r\n";

    // ---- a .gui menu: XML under the pad ----
    if (ext == "gui") {
        s += "Decoded XML:\r\n\r\n";
        for (size_t i = 0; i < plain.size(); ++i) {
            uint8_t c = plain[i];
            if (c == '\n') { s += "\r\n"; continue; }
            if (c == '\r') continue;
            if (c == '\t') { s += "    "; continue; }
            s += (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        }
        s += "\r\n\r\nSave as .xml to edit it, then File > Encode an edited file back"
             " to turn it into a .gui again.\r\n";
        return s;
    }

    // ---- a car's liveries list ----
    if (name.find(".liveries.") != std::string::npos) {
        NctLiveries L = nctReadLiveries(plain.data(), plain.size());
        if (L.ok) {
            snprintf(buf, sizeof(buf), "Car            %s\r\nTextures       %u (the file says %u)\r\n\r\n",
                     L.car.c_str(), (unsigned)L.textures.size(), (unsigned)L.count);
            s += buf;
            s += "Texture table - every file a livery of this car can draw on:\r\n\r\n";
            s += "  offset   id     path\r\n";
            for (const auto& t : L.textures) {
                snprintf(buf, sizeof(buf), "  %05X  %5u   %s%s\r\n", (unsigned)t.offset,
                         (unsigned)t.id, t.path.c_str(), t.sure ? "" : "   ?");
                s += buf;
            }
            s += "\r\n  ids below 100 are the game's shared textures (common/...),\r\n"
                 "  the rest belong to this car. livery/<car>_ext_<name> is a paint\r\n"
                 "  scheme's body texture - one per livery.\r\n\r\n";
            // the livery definitions that follow: shown as the strings they hold
            s += "Livery definitions (the part after the table), as the text in them:\r\n\r\n";
            int found = 0;
            for (size_t p = L.tableEnd; p + 4 < plain.size(); ) {
                uint32_t n = rd32(plain.data() + p);
                if (n >= 2 && n <= 200 && p + 4 + n <= plain.size()) {
                    bool text = true;
                    for (uint32_t k = 0; k < n && text; ++k) {
                        uint8_t c = plain[p + 4 + k];
                        if (c < 0x20 || c > 0x7E) text = false;
                    }
                    if (text) {
                        snprintf(buf, sizeof(buf), "  %05X  %s%s\r\n", (unsigned)p,
                                 printable(plain.data() + p + 4, n).c_str(),
                                 rangeSure(p, p + 4 + n) ? "" : "   ?");
                        s += buf;
                        ++found;
                        p += 4 + n;
                        continue;
                    }
                }
                ++p;
            }
            if (!found) s += "  (none readable yet - the pad does not reach them)\r\n";
            s += "\r\nTo add a livery: save this as .bin (the decoded file), add the\r\n"
                 "texture record - u32 id, u32 length, the path - and its definition\r\n"
                 "in a hex editor, raise the texture count after the car's name, and\r\n"
                 "use File > Encode an edited file back. Bytes that move onto an offset\r\n"
                 "the pad is not certain of will not survive; the encoder counts them.\r\n";
            return s;
        }
    }

    // ---- a car's data file ----
    if (plain.size() >= 16) {
        uint32_t id = rd32(plain.data() + 4);
        std::string year((const char*)plain.data() + 12, 4);
        bool yearOk = true;
        for (char c : year) if (c < '0' || c > '9') yearOk = false;
        snprintf(buf, sizeof(buf), "car id     %u\r\nmodel year %s\r\n\r\n",
                 id, yearOk ? year.c_str() : "(not readable here)");
        s += buf;
    }
    // The body is a run of records, each a uint32 length then that many bytes
    // of text with no terminator. Walking them is what makes the file
    // readable rather than a wall of hex.
    s += "Strings, with the offset each one starts at:\r\n\r\n";
    int found = 0;
    for (size_t p = 0; p + 4 < plain.size(); ) {
        uint32_t n = rd32(plain.data() + p);
        if (n >= 1 && n <= 64 && p + 4 + n <= plain.size()) {
            bool text = true;
            for (uint32_t k = 0; k < n; ++k) {
                uint8_t c = plain[p + 4 + k];
                if (c < 0x20 || c > 0x7E) { text = false; break; }
            }
            if (text) {
                snprintf(buf, sizeof(buf), "  %04X  %-40s %s\r\n", (unsigned)p,
                         printable(plain.data() + p + 4, n).c_str(),
                         rangeSure(p, p + 4 + n) ? "" : "?");
                s += buf;
                ++found;
                p += 4 + n;
                continue;
            }
        }
        ++p;
    }
    if (!found) s += "  (none found - the pad may not cover this file)\r\n";

    s += "\r\nHow this file is read\r\n"
         "  .nct (and .gui) is one fixed pad XORed over the file, the same pad for\r\n"
         "  every file, so the pad falls out of the files themselves: each car's\r\n"
         "  name, the texture lists' layout, struct padding and the menus' XML.\r\n"
         "  File > Finish the .nct pad from this game runs that over every file in\r\n"
         "  the game and remembers the result. Editing is exact either way for\r\n"
         "  bytes that do not move - the same XOR encodes as it decodes.\r\n";
    return s;
}

// ============================================================== JSR-184 M3G
//
// Three identifiers are in use: the stock JSR-184 one, and Firemint's own two
// for the later object layout. The container is the same either way - sections
// of length-prefixed objects - and only a few object types matter here:
//
//   20 VertexArray         positions, normals, texture coordinates
//   21 VertexBuffer        which arrays a mesh uses, plus a bias and a scale
//   11 TriangleStripArray  the indices
//   14 Mesh                the name, the material and the buffers
//   24 MaterialList        Firemint's own: the material names, in order
//
// Nothing here is guessed at: the layout follows the community's M3G2FBX,
// which has been read against real Real Racing 3 cars for years.

namespace {

struct Reader {
    const uint8_t* d;
    size_t n, p = 0;
    bool bad = false;

    Reader(const uint8_t* data, size_t len) : d(data), n(len) {}
    bool have(size_t k) const { return p + k <= n; }
    void skip(size_t k) { if (!have(k)) { bad = true; p = n; } else p += k; }
    uint8_t u8()  { if (!have(1)) { bad = true; return 0; } return d[p++]; }
    uint16_t u16() { if (!have(2)) { bad = true; return 0; } uint16_t v = rd16(d + p); p += 2; return v; }
    uint32_t u32() { if (!have(4)) { bad = true; return 0; } uint32_t v = rd32(d + p); p += 4; return v; }
    int16_t i16() { return (int16_t)u16(); }
    int32_t i32() { return (int32_t)u32(); }
    float f32()   { if (!have(4)) { bad = true; return 0; } float v = rdf(d + p); p += 4; return v; }
    std::string cstr() {
        std::string s;
        while (have(1)) { uint8_t c = d[p++]; if (!c) return s; s.push_back((char)c); }
        bad = true;
        return s;
    }
};

struct M3gObject {
    uint8_t type = 0;
    size_t offset = 0, length = 0;
};

struct VArray {
    int componentSize = 0, componentCount = 0, encoding = 0, count = 0;
    size_t offset = 0;
};

struct VBuffer {
    uint32_t positions = 0, normals = 0, colors = 0;
    float bias[3] = {0, 0, 0};
    float scale = 1.0f;
    std::vector<uint32_t> texcoords;
    std::vector<float> tcBias;    // 3 per set
    std::vector<float> tcScale;   // 1 per set
};

// Every identifier the container is known to use.
bool identifierAt(const uint8_t* d, size_t len, size_t off, int* version) {
    if (off + 12 > len) return false;
    static const uint8_t kJsr[12] = {0xAB,'J','S','R','1','8','4',0xBB,0x0D,0x0A,0x1A,0x0A};
    static const uint8_t kIm2[12] = {0xAB,'I','M','2','M','3','G',0xBB,0x0D,0x0A,0x1A,0x0A};
    static const uint8_t kIm3[12] = {0xAB,'I','M','3','M','3','G',0xBB,0x0D,0x0A,0x1A,0x0A};
    const uint8_t* p = d + off;
    if (!memcmp(p, kJsr, 12)) { if (version) *version = 1; return true; }
    if (!memcmp(p, kIm2, 12)) { if (version) *version = 2; return true; }
    if (!memcmp(p, kIm3, 12)) { if (version) *version = 3; return true; }
    static const uint8_t kIm4[12] = {0xAB,'I','M','4','M','3','G',0xBB,0x0D,0x0A,0x1A,0x0A};
    if (!memcmp(p, kIm4, 12)) { if (version) *version = 4; return true; }
    static const uint8_t kImH[12] = {0xAB,'I','M','-','M','3','G',0xBB,0x0D,0x0A,0x1A,0x0A};
    if (!memcmp(p, kImH, 12)) { if (version) *version = 5; return true; }
    return false;
}

} // namespace

bool isJsr184(const uint8_t* data, size_t len) {
    if (identifierAt(data, len, 0, nullptr)) return true;
    // some assets are gzipped whole
    if (len > 18 && data[0] == 0x1F && data[1] == 0x8B) return true;
    return false;
}

// The object table of an M3G file, the way NFSMW12MobileTools' "map" command
// lists it: one line per object with its type and size, and the text
// parameters (names) where the type carries them.
std::string m3gObjectMap(const uint8_t* data, size_t len) {
    std::string out;
    Bytes unpacked;
    if (len > 18 && data[0] == 0x1F && data[1] == 0x8B && inflateGzip(data, len, unpacked)) {
        data = unpacked.data();
        len = unpacked.size();
    } else if (m3gUnwrap(data, len, unpacked)) {     // No Limits' DA BD / EA gzip
        data = unpacked.data();
        len = unpacked.size();
    }
    int version = 0;
    if (!identifierAt(data, len, 0, &version)) return out;
    static const char* const kNames[] = {
        "header", "animation track settings", "animation track", "appearance", "?", "?",
        "compositing mode", "?", "polygon mode", "group", "image2D", "?", "?", "?",
        "mesh", "?", "cloned group", "texture reference", "?", "animation buffer",
        "vertex array", "vertex buffer", "?", "?", "material list" };
    char buf[256];
    snprintf(buf, sizeof(buf), "M3G, %s layout\r\n\r\nobjects\r\n",
             version == 1 ? "JSR-184" : version == 2 ? "IM2M3G (NFS Most Wanted 2012)"
             : version == 4 ? "IM4M3G (NFS No Limits)"
             : version == 5 ? "IM-M3G (NFS Hot Pursuit)" : "IM3M3G");
    out += buf;
    Reader r(data, len);
    r.p = 12;
    uint32_t index = 1;
    while (r.p + 9 <= len && !r.bad && index < 100000) {
        r.u8();
        uint32_t total = r.u32(), plain = r.u32();
        if (total < 9) break;
        size_t end = r.p + plain;
        // IM2/IM4 have no sections: one run of objects to the checksum
        if (version >= 2) end = len >= 4 ? len - 4 : len;
        if (end > len) break;
        while (r.p + 5 <= end && !r.bad) {
            uint8_t t = r.u8();
            uint32_t n = r.u32();
            size_t at = r.p;
            if (at + n > len) { r.bad = true; break; }
            const char* tn = t < sizeof(kNames) / sizeof(kNames[0]) ? kNames[t]
                             : t == 11 ? "triangle strips" : t == 100 ? "submesh"
                             : t == 101 ? "index buffer" : "?";
            if (t == 11) tn = "triangle strips";
            snprintf(buf, sizeof(buf), "  #%-5u %-24s %8u bytes", index, tn, n);
            out += buf;
            // text parameters: submeshes and images carry the names
            // (IM4 has one more header word in every object)
            size_t extra = version == 4 ? 4 : 0;
            size_t pp = t == 100 || t == 10 ? at + 8 + extra : 0;
            // groups, cloned groups and appearances name themselves after
            // their animation tracks
            if (version >= 2 && (t == 3 || t == 9 || t == 16) && at + 8 + extra <= at + n) {
                uint32_t tracks = rd32(data + at + 4 + extra);
                if (tracks < 1024) pp = at + 8 + extra + (size_t)tracks * 4;
            }
            if (pp && pp + 4 <= at + n) {
                uint32_t cnt = rd32(data + pp);
                pp += 4;
                for (uint32_t k = 0; k < cnt && k < 16 && pp + 8 <= at + n; ++k) {
                    uint32_t ty = rd32(data + pp), sz = rd32(data + pp + 4);
                    pp += 8;
                    if (pp + sz > at + n) break;
                    if ((ty == 0 || ty == 0x384) && sz) {
                        std::string v((const char*)data + pp, sz);
                        while (!v.empty() && v.back() == 0) v.pop_back();
                        out += "   \"" + v + "\"";
                    }
                    pp += sz;
                }
            }
            out += "\r\n";
            r.p = at + n;
            ++index;
        }
        r.p = end;
        r.u32();
        if (version >= 2) break;
    }
    return out;
}

Model loadJsr184(const uint8_t* data, size_t len, bool flipV) {
    Model model;

    // a whole-file gzip wrapper, when there is one
    Bytes unpacked;
    if (len > 18 && data[0] == 0x1F && data[1] == 0x8B) {
        if (inflateGzip(data, len, unpacked) && unpacked.size() > 12) {
            data = unpacked.data();
            len = unpacked.size();
        }
    }

    int version = 1;
    if (!identifierAt(data, len, 0, &version)) {
        model.warnings.push_back("not a JSR-184 M3G file");
        return model;
    }

    // ---- walk the sections, collecting every object's extent ----
    std::vector<M3gObject> objects;
    objects.push_back(M3gObject{});          // index 0 is the null object
    {
        Reader r(data, len);
        r.p = 12;
        while (r.p + 9 <= len && !r.bad) {
            r.u8();                          // compression scheme
            uint32_t total = r.u32();
            uint32_t plain = r.u32();
            if (total < 9) break;
            size_t sectionEnd = r.p + plain;
            if (sectionEnd > len) break;
            while (r.p < sectionEnd && !r.bad) {
                M3gObject o;
                o.type = r.u8();
                o.length = r.u32();
                o.offset = r.p;
                if (o.offset + o.length > len) { r.bad = true; break; }
                objects.push_back(o);
                r.p += o.length;
            }
            r.p = sectionEnd;
            r.u32();                         // section checksum
            if (objects.size() > 200000) break;
        }
    }
    if (objects.size() <= 1) {
        model.warnings.push_back("no objects in the M3G container");
        return model;
    }

    // ---- vertex arrays ----
    std::vector<VArray> arrays(objects.size());
    for (size_t i = 1; i < objects.size(); ++i) {
        if (objects[i].type != 20) continue;
        Reader r(data, len);
        r.p = objects[i].offset + 12;        // userID, animation tracks, params
        VArray a;
        a.componentSize  = r.u8();
        a.componentCount = r.u8();
        a.encoding       = r.u8();
        a.count          = r.u16();
        a.offset         = r.p;
        if (!r.bad) arrays[i] = a;
    }

    // ---- vertex buffers ----
    std::vector<VBuffer> buffers(objects.size());
    for (size_t i = 1; i < objects.size(); ++i) {
        if (objects[i].type != 21) continue;
        Reader r(data, len);
        r.p = objects[i].offset + 12;
        VBuffer b;
        r.u32();                             // default colour
        b.positions = r.u32();
        b.bias[0] = r.f32(); b.bias[1] = r.f32(); b.bias[2] = r.f32();
        b.scale   = r.f32();
        b.normals = r.u32();
        b.colors = r.u32();                  // per-vertex colours, 0 when none
        uint32_t sets = r.u32();
        if (sets > 16) sets = 0;
        for (uint32_t s = 0; s < sets && !r.bad; ++s) {
            b.texcoords.push_back(r.u32());
            b.tcBias.push_back(r.f32());
            b.tcBias.push_back(r.f32());
            b.tcBias.push_back(r.f32());
            b.tcScale.push_back(r.f32());
        }
        if (!r.bad) buffers[i] = b;
    }

    // ---- Firemint's material name list ----
    std::vector<std::string> materialNames;
    for (size_t i = 1; i < objects.size(); ++i) {
        if (objects[i].type != 24) continue;
        Reader r(data, len);
        r.p = objects[i].offset;
        uint32_t count = r.u32();
        if (count > 4096) break;
        for (uint32_t m = 0; m < count && !r.bad; ++m)
            materialNames.push_back(r.cstr());
        break;
    }
    // Real Racing 3's files - cars, tracks, skies - carry Firemint's material
    // list (type 24); Real Racing 2's never do. That list is how the V
    // convention below is told apart.
    bool rr3Naming = !materialNames.empty();
    for (const std::string& mn : materialNames) {
        Material mat;
        mat.name = mn;
        std::string low = lowered(mn);
        mat.alphaBlend = low.find("window") != std::string::npos ||
                         low.find("glass") != std::string::npos ||
                         low.find("shadow") != std::string::npos ||
                         low.find("blur") != std::string::npos;
        mat.additive   = low.find("glow") != std::string::npos ||
                         low.find("emissive") != std::string::npos;
        mat.twoSided   = true;
        model.materials.push_back(std::move(mat));
    }

    // ---- the later layout's parts (IM2M3G / IM3M3G) ----
    //
    // Read after Hypercycle's NFSMW12MobileTools, which maps Most Wanted
    // 2012's models object by object. A parameter list is a count, then
    // (type u32, size u32, bytes) records - types 0 and 0x384 are text.
    // A submesh (100) is 8 bytes of header, its parameters, then the index
    // buffer and the appearance it uses; an appearance (3) lists texture
    // references (17), each of which names an Image2D (10) whose parameters
    // carry the texture's name.
    auto readParams = [&](Reader& r, std::vector<std::string>* texts) -> bool {
        uint32_t n = r.u32();
        if (r.bad || n > 256) return false;
        for (uint32_t k = 0; k < n && !r.bad; ++k) {
            uint32_t ty = r.u32(), sz = r.u32();
            if (r.bad || !r.have(sz)) { r.bad = true; break; }
            if (texts && (ty == 0 || ty == 0x384) && sz) {
                std::string t((const char*)data + r.p, sz);
                while (!t.empty() && t.back() == 0) t.pop_back();
                if (!t.empty()) texts->push_back(t);
            }
            r.skip(sz);
        }
        return !r.bad;
    };
    struct SubInfo { uint32_t ib = 0, appearance = 0; std::vector<std::string> texts; };
    auto readSubMesh = [&](uint32_t idx, SubInfo& si) -> bool {
        if (idx == 0 || idx >= objects.size() || objects[idx].type != 100) return false;
        Reader r(data, len);
        r.p = objects[idx].offset + 8;
        if (!readParams(r, &si.texts)) return false;
        si.ib = r.u32();
        si.appearance = r.u32();
        return !r.bad && si.ib && si.ib < objects.size();
    };
    auto appearanceTexture = [&](uint32_t app) -> std::string {
        if (app == 0 || app >= objects.size() || objects[app].type != 3) return "";
        Reader r(data, len);
        r.p = objects[app].offset;
        r.u32();                                   // animation controllers
        uint32_t tracks = r.u32();
        if (tracks > 256) return "";
        r.skip((size_t)tracks * 4);
        if (!readParams(r, nullptr)) return "";
        r.u8();                                    // layer
        r.skip(16);                                // compositing, fog, polygon, material
        uint32_t texCount = r.u32();
        if (r.bad || texCount > 16) return "";
        for (uint32_t k = 0; k < texCount && !r.bad; ++k) {
            uint32_t ref = r.u32();
            if (ref == 0 || ref >= objects.size() || objects[ref].type != 17) continue;
            Reader t(data, len);
            t.p = objects[ref].offset + 12 + 2;
            uint32_t image = t.u32();
            if (t.bad || image == 0 || image >= objects.size()) continue;
            // An external reference (type 255): the texture is a file of its
            // own, named by the reference - Real Racing 2's tracks point at
            // alkeisha_shops_hd.pvr this way.
            if (objects[image].type == 255) {
                const uint8_t* u = data + objects[image].offset;
                size_t n = 0;
                while (n < objects[image].length && u[n]) ++n;
                if (n) return std::string((const char*)u, n);
                continue;
            }
            if (objects[image].type != 10) continue;
            Reader im(data, len);
            im.p = objects[image].offset + 8;
            std::vector<std::string> names;
            if (!readParams(im, &names) || names.empty()) continue;
            for (const std::string& n : names)
                if (n.find('.') != std::string::npos || n.find('/') != std::string::npos) return n;
            return names[0];
        }
        return "";
    };

    // ---- indices ----
    auto readIndices = [&](uint32_t bufferIndex, std::vector<uint32_t>& out) -> bool {
        if (bufferIndex == 0 || bufferIndex >= objects.size()) return false;
        Reader r(data, len);
        uint8_t t = objects[bufferIndex].type;
        bool strip = false;
        if (t == 11) {
            r.p = objects[bufferIndex].offset + 20;
            strip = r.u8() != 0;
        } else if (t == 100 || t == 103) {
            SubInfo si;
            uint32_t real = 0;
            if (t == 100 && readSubMesh(bufferIndex, si)) {
                real = si.ib;
            } else {
                r.p = objects[bufferIndex].offset + 12;
                real = r.u32();
            }
            if (real == 0 || real >= objects.size()) return false;
            r.p = objects[real].offset + 12;
        } else {
            return false;
        }
        (void)strip;

        uint8_t encoding = r.u8();
        std::vector<uint32_t> raw;
        if (encoding == 128 || encoding == 129 || encoding == 130) {
            uint32_t n = r.u32();
            if (n > 8000000) return false;
            raw.reserve(n);
            for (uint32_t k = 0; k < n && !r.bad; ++k) {
                if (encoding == 128) raw.push_back(r.u32());
                else if (encoding == 129) raw.push_back(r.u8());
                else raw.push_back(r.u16());
            }
        } else if (encoding <= 2) {
            // implicit indices: a first value and a run of strip lengths
            uint32_t first = 0;
            if (encoding == 0) first = r.u32();
            else if (encoding == 1) first = r.u8();
            else first = r.u16();
            uint32_t runs = r.u32();
            if (runs > 1000000) return false;
            for (uint32_t k = 0; k < runs && !r.bad; ++k) {
                uint32_t n = r.u32();
                if (n > 8000000) return false;
                for (uint32_t j = 0; j < n; ++j) raw.push_back(first++);
            }
        } else {
            return false;
        }
        if (r.bad || raw.empty()) return false;

        // The container calls everything a strip, but Real Racing 3 writes
        // plain triangle lists into it: an explicit index run whose length is
        // a multiple of three and whose triangles are all non-degenerate is a
        // list, and stitching it as a strip would shred the mesh.
        bool asList = raw.size() % 3 == 0;
        if (asList) {
            size_t degenerate = 0;
            for (size_t k = 0; k + 2 < raw.size(); k += 3)
                if (raw[k] == raw[k + 1] || raw[k + 1] == raw[k + 2] ||
                    raw[k] == raw[k + 2]) ++degenerate;
            if (degenerate * 8 > raw.size() / 3) asList = false;
        }
        if (asList) {
            out = std::move(raw);
        } else {
            for (size_t k = 0; k + 2 < raw.size(); ++k) {
                uint32_t a = raw[k], b = raw[k + 1], c = raw[k + 2];
                if (a == b || b == c || a == c) continue;
                if (k & 1) { out.push_back(a); out.push_back(c); out.push_back(b); }
                else       { out.push_back(a); out.push_back(b); out.push_back(c); }
            }
        }
        return !out.empty();
    };

    // Real Racing 1 and GTI state their UV scale as it is: raw texture
    // coordinates times the stated scale span 0..1. Real Racing 2 and 3
    // state four times the real one (spans up to 4 and beyond). Which one a
    // file uses shows in its largest coordinate.
    float uvQuarter = 0.25f;
    {
        double span = 0;
        for (size_t i = 1; i < objects.size(); ++i) {
            if (objects[i].type != 21) continue;
            const VBuffer& b = buffers[i];
            for (size_t s2 = 0; s2 < b.texcoords.size() && s2 < b.tcScale.size(); ++s2) {
                uint32_t tc = b.texcoords[s2];
                if (!tc || tc >= arrays.size() || arrays[tc].componentSize != 2) continue;
                const VArray& ta = arrays[tc];
                Reader vr(data, len);
                vr.p = ta.offset;
                for (int k = 0; k < ta.count * ta.componentCount && !vr.bad; ++k)
                    span = std::max(span, std::fabs((double)vr.i16() * b.tcScale[s2]));
            }
        }
        if (span > 0 && span <= 1.1) uvQuarter = 1.0f;
    }

    // ---- meshes ----
    for (size_t i = 1; i < objects.size(); ++i) {
        if (objects[i].type != 14) continue;
        Reader r(data, len);
        r.p = objects[i].offset + 8;

        std::string name;
        int materialId = -1;
        if (version == 1) {
            // a discriminator says how much node data precedes the material
            uint32_t kind = r.u32();
            if (kind == 5) r.skip(48);
            else if (kind == 6) { r.skip(56); materialId = (int)r.u32(); }
            else if (kind == 7) { r.skip(81); materialId = (int)r.u32(); }
            else continue;
            name = r.cstr();
            r.skip(50);
        } else {
            r.skip(14);
        }
        if (r.bad) continue;

        uint32_t vbIndex = r.u32();
        uint32_t submeshes = r.u32();
        if (r.bad || vbIndex == 0 || vbIndex >= objects.size()) continue;
        if (submeshes > 256) continue;
        const VBuffer& vb = buffers[vbIndex];
        if (vb.positions == 0 || vb.positions >= arrays.size()) continue;

        // positions
        const VArray& pa = arrays[vb.positions];
        if (pa.count <= 0) continue;
        std::vector<float> positions;
        positions.reserve((size_t)pa.count * 3);
        {
            Reader vr(data, len);
            vr.p = pa.offset;
            for (int k = 0; k < pa.count && !vr.bad; ++k) {
                float x, y, z;
                if (pa.componentSize == 4) { x = vr.f32(); y = vr.f32(); z = vr.f32(); }
                else { x = (float)vr.i16(); y = (float)vr.i16(); z = (float)vr.i16(); }
                positions.push_back(x * vb.scale + vb.bias[0]);
                positions.push_back(y * vb.scale + vb.bias[1]);
                positions.push_back(z * vb.scale + vb.bias[2]);
            }
            if (vr.bad) continue;
        }

        // normals
        std::vector<float> normals;
        if (vb.normals && vb.normals < arrays.size() && arrays[vb.normals].count == pa.count) {
            const VArray& na = arrays[vb.normals];
            Reader vr(data, len);
            vr.p = na.offset;
            normals.reserve((size_t)na.count * 3);
            for (int k = 0; k < na.count && !vr.bad; ++k) {
                float x, y, z;
                if (na.componentSize == 4) { x = vr.f32(); y = vr.f32(); z = vr.f32(); }
                else { x = vr.i16() / 32767.0f; y = vr.i16() / 32767.0f; z = vr.i16() / 32767.0f; }
                // Real Racing 3 stores unit normals as 4096, not 32767:
                // scaled to length one here, whatever the file's unit
                float l = std::sqrt(x * x + y * y + z * z);
                if (l > 1e-8f) { x /= l; y /= l; z /= l; }
                normals.push_back(x); normals.push_back(y); normals.push_back(z);
            }
            if (vr.bad) normals.clear();
        }

        // first texture coordinate set
        std::vector<float> uvs;
        if (!vb.texcoords.empty()) {
            uint32_t tcIndex = vb.texcoords[0];
            if (tcIndex && tcIndex < arrays.size() && arrays[tcIndex].count == pa.count) {
                const VArray& ta = arrays[tcIndex];
                float ts = vb.tcScale.empty() ? 1.0f : vb.tcScale[0];
                float tb0 = vb.tcBias.size() > 0 ? vb.tcBias[0] : 0.0f;
                float tb1 = vb.tcBias.size() > 1 ? vb.tcBias[1] : 0.0f;
                // The scale the vertex buffer states is four times the real
                // one, and this correction is applied exactly once. Measured
                // rather than assumed: every model checked states 1/512, while
                // the Agera R's body has raw u running 283..2028 and raw v
                // -3150..-101 - which is 0.14..0.99 and -1.54..-0.05 at
                // 1/2048, and four times too large at 1/512. The 3ds Max
                // script everyone uses divides by 2048 for the same reason.
                ts *= uvQuarter;
                // V runs from 0 down to -1 in the file, and the textures are
                // stored bottom-up, so the export value is 1 + v: that puts
                // the map the right way up against the flipped texture.
                Reader vr(data, len);
                vr.p = ta.offset;
                uvs.reserve((size_t)ta.count * 2);
                for (int k = 0; k < ta.count && !vr.bad; ++k) {
                    float u, v;
                    if (ta.componentSize == 4) {
                        u = vr.f32(); v = vr.f32();
                        if (ta.componentCount > 2) vr.f32();
                    } else {
                        u = (float)vr.i16(); v = (float)vr.i16();
                        if (ta.componentCount > 2) vr.i16();
                    }
                    u = u * ts + tb0;
                    v = v * ts + tb1;
                    uvs.push_back(u);
                    // Real Racing 3 samples its textures the other way up
                    // from Real Racing 2: checked on the BMW M1, whose Procar
                    // stripes and roundel only land on the body with V turned
                    // over, and on Monaco's casino facade; the Scirocco's
                    // dashboard only reads right as it was.
                    bool up = rr3Naming ? !flipV : flipV;
                    uvs.push_back(up ? -v : (1.0f + v));
                }
                if (vr.bad) uvs.clear();
            }
        }

        // per-vertex colour: JSR-184 keeps it as unsigned bytes, RGB or RGBA
        std::vector<float> colors;
        if (vb.colors && vb.colors < arrays.size() && arrays[vb.colors].count == pa.count &&
            arrays[vb.colors].componentSize == 1 &&
            (arrays[vb.colors].componentCount == 3 || arrays[vb.colors].componentCount == 4)) {
            const VArray& ca = arrays[vb.colors];
            Reader vr(data, len);
            vr.p = ca.offset;
            colors.reserve((size_t)ca.count * 4);
            for (int k = 0; k < ca.count && !vr.bad; ++k) {
                float c[4] = {1, 1, 1, 1};
                for (int j = 0; j < ca.componentCount; ++j) c[j] = vr.u8() / 255.0f;
                colors.insert(colors.end(), c, c + 4);
            }
            if (vr.bad) colors.clear();
        }

        std::vector<uint32_t> indices;
        std::string texture;
        for (uint32_t s = 0; s < submeshes && !r.bad; ++s) {
            uint32_t ib = r.u32();
            if (version == 1) {
                uint32_t app = r.u32();      // appearance, which may name a texture file
                if (texture.empty()) texture = appearanceTexture(app);
            } else {
                // the later layout keeps the name and the texture with the submesh
                SubInfo si;
                if (readSubMesh(ib, si)) {
                    if (name.empty() && !si.texts.empty()) name = si.texts[0];
                    if (texture.empty()) texture = appearanceTexture(si.appearance);
                }
            }
            std::vector<uint32_t> part;
            if (readIndices(ib, part))
                indices.insert(indices.end(), part.begin(), part.end());
        }
        if (indices.empty()) continue;

        Mesh mesh;
        mesh.name = name.empty() ? ("mesh_" + std::to_string(i)) : name;
        mesh.positions = std::move(positions);
        mesh.normals = std::move(normals);
        mesh.uvs = std::move(uvs);
        mesh.colors = std::move(colors);
        // drop anything that points outside the buffer rather than crashing
        uint32_t vcount = (uint32_t)(mesh.positions.size() / 3);
        for (size_t k = 0; k + 2 < indices.size(); k += 3) {
            if (indices[k] < vcount && indices[k + 1] < vcount && indices[k + 2] < vcount) {
                mesh.indices.push_back(indices[k]);
                mesh.indices.push_back(indices[k + 1]);
                mesh.indices.push_back(indices[k + 2]);
            }
        }
        if (mesh.indices.empty()) continue;

        if (materialId >= 0 && materialId < (int)materialNames.size())
            mesh.material = materialNames[materialId];
        if (!texture.empty()) mesh.texture = texture;

        // The name carries the detail level and the part, the way the game
        // spells them: LOD_A_DOOR_LEFT_mm_cab.
        std::string up = mesh.name;
        if (up.size() > 6 && up.compare(0, 4, "LOD_") == 0) {
            char letter = up[4];
            if (letter >= 'A' && letter <= 'Z') {
                char buf[8];
                snprintf(buf, sizeof(buf), "LOD%02d", letter - 'A');
                mesh.lod = buf;
                size_t cut = up.find('_', 4);
                mesh.part = cut == std::string::npos ? up : up.substr(cut + 1);
            }
        }
        // Real Racing 2 spells the detail level at the end instead:
        // MESH_BODY_HIGH, MESH_WHEEL_LOW
        if (mesh.lod.empty()) {
            static const struct { const char* suffix; const char* lod; } kTail[] = {
                { "_HIGH", "LOD00" }, { "_HI", "LOD00" }, { "_MEDIUM", "LOD01" }, { "_MED", "LOD01" },
                { "_MID", "LOD01" }, { "_VERYLOW", "LOD03" }, { "_VLOW", "LOD03" }, { "_LOWEST", "LOD03" },
                { "_LOW", "LOD02" }, { "_LOD0", "LOD00" }, { "_LOD1", "LOD01" }, { "_LOD2", "LOD02" },
                { "_LOD3", "LOD03" }, { "_LOD4", "LOD04" }, { "_LOD5", "LOD05" }, { "_FAR", "LOD04" },
                { "_DISTANT", "LOD04" }, { "_SIMPLE", "LOD04" } };
            std::string u = up;
            for (char& ch : u) if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
            for (const auto& t : kTail) {
                size_t n = strlen(t.suffix);
                if (u.size() > n && u.compare(u.size() - n, n, t.suffix) == 0) {
                    mesh.lod = t.lod;
                    mesh.part = mesh.name.substr(0, mesh.name.size() - n);
                    break;
                }
            }
        }
        if (mesh.part.empty()) mesh.part = mesh.name;

        // A car file carries each part in several states: whole and damaged
        // (HOOD / HOOD_DAMAGE, HEADLIGHT_LENS / ..._BROKEN_A), a scratch shell
        // laid over the paint, cracked and shattered glass, and the bonnet-
        // camera version of the front. Drawn together they sit inside each
        // other and the paint breaks up into black and grey patches, so the
        // extra states get groups of their own, after the LODs.
        {
            std::string u = mesh.name + " " + mesh.material;
            for (char& ch : u) if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
            if (u.find("BONNETCAM") != std::string::npos)
                mesh.lod = "BONNETCAM";
            else if (u.find("_DAMAGE") != std::string::npos || u.find("BROKEN") != std::string::npos ||
                     u.find("_MM_SCRATCHES") != std::string::npos ||
                     u.find("WINDOWS_SHATTERED") != std::string::npos ||
                     u.find("WINDOWS_CRACKED") != std::string::npos)
                mesh.lod = "DAMAGE";
        }

        for (size_t k = 0; k + 2 < mesh.positions.size(); k += 3) {
            for (int c = 0; c < 3; ++c) {
                float v = mesh.positions[k + c];
                if (k == 0) { mesh.bboxMin[c] = mesh.bboxMax[c] = v; }
                else {
                    mesh.bboxMin[c] = std::min(mesh.bboxMin[c], v);
                    mesh.bboxMax[c] = std::max(mesh.bboxMax[c], v);
                }
            }
        }
        model.meshes.push_back(std::move(mesh));
    }

    // A file with damage or bonnet-camera states but no detail levels in its
    // names: everything else is the car as it is normally seen, so it gets a
    // level of its own and the viewer does not open on the wrecked shell
    // laid over the whole one.
    {
        bool anyLod = false, anyState = false;
        for (const Mesh& m : model.meshes) {
            if (m.lod.compare(0, 3, "LOD") == 0) anyLod = true;
            else if (!m.lod.empty()) anyState = true;
        }
        if (anyState && !anyLod)
            for (Mesh& m : model.meshes) if (m.lod.empty()) m.lod = "LOD00";
    }

    model.valid = !model.meshes.empty();
    if (!model.valid)
        model.warnings.push_back("the M3G parsed but held no drawable meshes");
    return model;
}

// ============================================================== placement

namespace {

// Which hardpoint a part modelled at the origin belongs on. The game's own
// names make this unambiguous; nothing here is a guess about geometry.
struct PlacementRule { const char* meshContains; const char* pointName; };

const PlacementRule kRules[] = {
    { "STEERING_WHEEL",  "POINT_STEERING_WHEEL" },
    { "HAND_STEER",      "POINT_STEERING_WHEEL" },
    { "NEEDLE_RPM",      "POINT_NEEDLE_RPM_1"   },
    { "NEEDLE_SPEED",    "POINT_NEEDLE_SPEED_1" },
    // Both arms are modelled around the steering wheel: on the 935 they
    // are mirror images at x = +-0.18, which is the rim's own radius, and
    // reach back 0.5 towards the seat. HAND_GEAR is the right arm while it is
    // on the wheel; POINT_GEARSTICK_HAND is the offset the game moves it by
    // for a gear change (it sits within 0.15 of the origin), not a place.
    { "HAND_GEAR",       "POINT_STEERING_WHEEL" },
    { "ARM_",            "POINT_STEERING_WHEEL" },
    { "HANDS",           "POINT_STEERING_WHEEL" },
    { "STEERINGWHEEL",   "POINT_STEERING_WHEEL" },
    { "STEER_WHEEL",     "POINT_STEERING_WHEEL" },
    { "WHEEL_STEER",     "POINT_STEERING_WHEEL" },
    { "STEERING",        "POINT_STEERING_WHEEL" },
    { "SHIFTER",         "POINT_GEARSTICK"      },
    { "NEEDLE_SPEED",    "POINT_NEEDLE_SPEED"   },
    { "NEEDLE_RPM",      "POINT_NEEDLE_RPM"     },
    { "GEARSTICK",       "POINT_GEARSTICK"      },
    { "GEAR_STICK",      "POINT_GEARSTICK"      },
};

const Hardpoint* findPoint(const std::vector<Hardpoint>& pts, const std::string& name) {
    for (const Hardpoint& h : pts) if (h.name == name) return &h;
    return nullptr;
}

// A part that still sits at the origin has a bounding box wrapped around zero
// and is small. A part already placed in the car's own space does not, so this
// is what keeps the body, the doors and the mirrors from being moved.
bool sitsAtOrigin(const Mesh& m) {
    for (int c = 0; c < 3; ++c) {
        float centre = (m.bboxMin[c] + m.bboxMax[c]) * 0.5f;
        float half   = (m.bboxMax[c] - m.bboxMin[c]) * 0.5f;
        if (std::fabs(centre) > 0.12f) return false;
        if (half > 0.60f) return false;
    }
    return true;
}

} // namespace

void rr3MergeCockpitPoints(std::vector<Hardpoint>& exterior, const std::vector<Hardpoint>& interior) {
    for (const Hardpoint& h : interior) {
        bool wanted = false;
        for (const PlacementRule& rule : kRules)
            if (h.name == rule.pointName) wanted = true;
        if (!wanted || findPoint(exterior, h.name)) continue;
        exterior.push_back(h);
    }
    // a cockpit that spells its steering point differently
    // (POINT_STEERINGWHEEL, POINT_STEERING_WHEEL_1, ...)
    if (!findPoint(exterior, "POINT_STEERING_WHEEL"))
        for (const Hardpoint& h : interior) {
            std::string up = h.name;
            for (char& c : up) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            if (up.find("STEER") == std::string::npos) continue;
            Hardpoint sw = h;
            sw.name = "POINT_STEERING_WHEEL";
            exterior.push_back(sw);
            break;
        }
}

std::string rr3InteriorPointsNameFor(const std::string& modelPath) {
    std::string p = rr3PointsNameFor(modelPath);
    if (p == modelPath || p.size() < 7) return std::string();
    std::string stem = p.substr(0, p.size() - 7);          // drop ".points"
    if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, "_int") == 0) return std::string();
    return stem + "_int.points";
}

std::string rr3SharedNameFor(const std::string& modelPath) {
    std::string p = rr3PointsNameFor(modelPath);
    if (p == modelPath || p.size() < 7) return std::string();
    std::string stem = p.substr(0, p.size() - 7);          // drop ".points"
    if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, "_int") == 0) return std::string();
    return stem + "_shared.m3g";
}

void rr3PlaceParts(Model& m, const std::vector<Hardpoint>& given) {
    m.points = given;
    if (given.empty()) return;
    // No steering point at all (no cockpit file beside the car either): the
    // driver's point is where the wheel is - on the 935 the two are 5 cm
    // apart - which beats leaving the wheel under the floor at the origin.
    std::vector<Hardpoint> points = given;
    bool fromDriver = false;
    if (!findPoint(points, "POINT_STEERING_WHEEL")) {
        const Hardpoint* driver = findPoint(points, "POINT_DRIVER");
        if (driver) {
            Hardpoint sw = *driver;
            sw.name = "POINT_STEERING_WHEEL";
            sw.hinge = false;
            points.push_back(sw);
            fromDriver = true;
        }
    }

    int moved = 0;
    std::vector<std::string> parked;
    for (Mesh& mesh : m.meshes) {
        std::string upper = mesh.name;
        for (char& c : upper) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        const Hardpoint* target = nullptr;
        for (const PlacementRule& rule : kRules) {
            if (upper.find(rule.meshContains) == std::string::npos) continue;
            target = findPoint(points, rule.pointName);
            if (target) break;
        }
        float c[3];
        for (int k = 0; k < 3; ++k) c[k] = (mesh.bboxMin[k] + mesh.bboxMax[k]) * 0.5f;
        if (!target) {
            if (sitsAtOrigin(mesh)) parked.push_back(mesh.name);
            continue;
        }
        // A part the name ties to a point is moved when it is modelled
        // around the origin rather than already in place: its centre nearer
        // the origin than the point. The arms hang 0.2-0.3 off their pivot,
        // more than sitsAtOrigin allows, and are still caught this way.
        float dO = std::sqrt(c[0]*c[0] + c[1]*c[1] + c[2]*c[2]);
        float dx = c[0] - target->pos[0], dy = c[1] - target->pos[1], dz = c[2] - target->pos[2];
        float dP = std::sqrt(dx*dx + dy*dy + dz*dz);
        if (!(dO < dP)) continue;
        translateMeshBy(mesh, target->pos);
        ++moved;
        if (fromDriver && target->name == "POINT_STEERING_WHEEL") {
            m.warnings.push_back(mesh.name + " placed at POINT_DRIVER (no steering point in the "
                                 ".points files found)");
            fromDriver = false;
        }
    }
    if (moved) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%d part(s) placed from the .points file", moved);
        m.warnings.push_back(buf);
    }

    // Real Racing 2 ships the wheel inside the car: MESH_WHEEL_HIGH,
    // MESH_BRAKE_HIGH (and _LOW) modelled once, round the origin. One copy
    // goes on each POINT_WHEEL_xx - hub on the point, the right-hand side the
    // mirror of the left - and the one at the origin goes.
    {
        static const char* kPt[4] = { "POINT_WHEEL_FL", "POINT_WHEEL_FR", "POINT_WHEEL_BL", "POINT_WHEEL_BR" };
        static const char* kTag[4] = { "FL", "FR", "BL", "BR" };
        auto isOwnWheel = [](const Mesh& me) {
            std::string u = me.name;
            for (char& c : u) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            if (u.find("BLUR") != std::string::npos) return false;
            for (const char* k : { "MESH_WHEEL", "MESH_BRAKE", "MESH_TYRE", "MESH_TIRE", "MESH_RIM",
                                   "MESH_DISC", "MESH_ROTOR", "MESH_CALIPER" })
                if (u.compare(0, strlen(k), k) == 0) return sitsAtOrigin(me);
            return false;
        };
        std::vector<size_t> own;
        for (size_t i = 0; i < m.meshes.size(); ++i) if (isOwnWheel(m.meshes[i])) own.push_back(i);
        int corners = 0;
        for (int w = 0; w < 4; ++w) if (findPoint(points, kPt[w])) ++corners;
        if (!own.empty() && corners) {
            // the hub: the middle of the rim/tyre meshes (the brake sits off it)
            float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
            bool any = false;
            for (size_t i : own) {
                std::string u = m.meshes[i].name;
                for (char& c : u) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
                if (u.find("WHEEL") == std::string::npos && u.find("TYRE") == std::string::npos &&
                    u.find("TIRE") == std::string::npos && u.find("RIM") == std::string::npos) continue;
                any = true;
                for (int k = 0; k < 3; ++k) {
                    lo[k] = std::min(lo[k], m.meshes[i].bboxMin[k]);
                    hi[k] = std::max(hi[k], m.meshes[i].bboxMax[k]);
                }
            }
            if (!any)
                for (size_t i : own)
                    for (int k = 0; k < 3; ++k) {
                        lo[k] = std::min(lo[k], m.meshes[i].bboxMin[k]);
                        hi[k] = std::max(hi[k], m.meshes[i].bboxMax[k]);
                    }
            float hub[3] = { (lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f };
            std::vector<Mesh> added;
            for (int w = 0; w < 4; ++w) {
                const Hardpoint* h = findPoint(points, kPt[w]);
                if (!h) continue;
                bool right = h->pos[0] > 0;
                for (size_t i : own) {
                    Mesh copy = m.meshes[i];
                    copy.name = std::string("WHEEL_") + kTag[w] + "_" + m.meshes[i].name;
                    copy.part = std::string("wheel_") + kTag[w];
                    for (size_t k = 0; k + 2 < copy.positions.size(); k += 3) {
                        float x = copy.positions[k] - hub[0];
                        if (right) x = -x;
                        copy.positions[k] = x + h->pos[0];
                        copy.positions[k + 1] += h->pos[1] - hub[1];
                        copy.positions[k + 2] += h->pos[2] - hub[2];
                    }
                    if (right) {
                        for (size_t k = 0; k + 2 < copy.indices.size(); k += 3)
                            std::swap(copy.indices[k + 1], copy.indices[k + 2]);
                        for (size_t k = 0; k + 2 < copy.normals.size(); k += 3)
                            copy.normals[k] = -copy.normals[k];
                    }
                    for (int k = 0; k < 3; ++k) { copy.bboxMin[k] = 1e30f; copy.bboxMax[k] = -1e30f; }
                    for (size_t k = 0; k + 2 < copy.positions.size(); k += 3)
                        for (int c = 0; c < 3; ++c) {
                            copy.bboxMin[c] = std::min(copy.bboxMin[c], copy.positions[k + c]);
                            copy.bboxMax[c] = std::max(copy.bboxMax[c], copy.positions[k + c]);
                        }
                    added.push_back(std::move(copy));
                }
            }
            std::vector<std::string> gone;
            for (size_t n = own.size(); n-- > 0;) {
                gone.push_back(m.meshes[own[n]].name);
                m.meshes.erase(m.meshes.begin() + (long)own[n]);
            }
            for (Mesh& a : added) m.meshes.push_back(std::move(a));
            parked.erase(std::remove_if(parked.begin(), parked.end(), [&](const std::string& n) {
                             return std::find(gone.begin(), gone.end(), n) != gone.end();
                         }), parked.end());
            char buf[128];
            snprintf(buf, sizeof(buf), "the car's own wheel (%u part(s)) put on its %d wheel point(s)",
                     (unsigned)own.size(), corners);
            m.warnings.push_back(buf);
        }
    }

    // Anything still modelled at the origin with no rule for it would show up
    // in the middle of the car. Naming it here is what lets a rule be added.
    // (Real Racing 1 / GTI parts carry no names and are modelled in place.)
    if (!parked.empty() && !findPoint(points, "POINT_CENTRE")) {
        std::string msg = "left at the origin (no matching point): ";
        for (size_t i = 0; i < parked.size() && i < 8; ++i)
            msg += (i ? ", " : "") + parked[i];
        if (parked.size() > 8) msg += ", ...";
        m.warnings.push_back(msg);
    }
}

namespace {

// What a mesh of a <car>_shared.m3g is, from its name with the LOD_x_ prefix
// and the _mm_<material> suffix taken off.
enum SharedRole { SR_NONE, SR_TYRE, SR_TYRE_REAR, SR_WHEEL, SR_WHEEL_REAR, SR_ROTOR, SR_ROTOR_REAR,
                  SR_CAL_FL, SR_CAL_FR, SR_CAL_RL, SR_CAL_RR };

SharedRole sharedRole(const std::string& full, char* lodLetter) {
    std::string n = full;
    *lodLetter = 0;
    if (n.size() > 6 && n.compare(0, 4, "LOD_") == 0 && n[5] == '_') {
        *lodLetter = n[4];
        n = n.substr(6);
    }
    size_t mm = n.find("_mm_");
    if (mm != std::string::npos) n = n.substr(0, mm);
    if (n.find("BLUR") != std::string::npos) return SR_NONE;   // motion-blur stand-in
    if (n == "TYRE" || n == "TIRE") return SR_TYRE;
    if (n == "TYRE_REAR" || n == "TIRE_REAR") return SR_TYRE_REAR;
    if (n == "WHEEL") return SR_WHEEL;
    if (n == "WHEEL_REAR") return SR_WHEEL_REAR;
    if (n == "ROTOR") return SR_ROTOR;
    if (n == "ROTOR_REAR") return SR_ROTOR_REAR;
    if (n == "BRAKE_CALIPER_FRONT_LEFT")  return SR_CAL_FL;
    if (n == "BRAKE_CALIPER_FRONT_RIGHT") return SR_CAL_FR;
    if (n == "BRAKE_CALIPER_REAR_LEFT")   return SR_CAL_RL;
    if (n == "BRAKE_CALIPER_REAR_RIGHT")  return SR_CAL_RR;
    return SR_NONE;
}

void refreshBounds(Mesh& m) {
    for (int c = 0; c < 3; ++c) { m.bboxMin[c] = 1e30f; m.bboxMax[c] = -1e30f; }
    for (size_t i = 0; i + 2 < m.positions.size(); i += 3)
        for (int c = 0; c < 3; ++c) {
            m.bboxMin[c] = std::min(m.bboxMin[c], m.positions[i + c]);
            m.bboxMax[c] = std::max(m.bboxMax[c], m.positions[i + c]);
        }
}

// The corner assembly of a <car>_shared.m3g, measured on the Targa and the
// C11 and checked against the 935's and the Agera's body and .points:
//
//  * every piece is modelled in one wheel-local frame whose x = 0 plane is
//    the OUTER face of the wheel - the spokes sit there - and +x runs inward,
//    towards the brakes. POINT_WHEEL_xx sits on the body's outer edge, so the
//    x = 0 plane goes exactly on the point; nothing is centred.
//  * unchanged, that frame is the LEFT side of the car (x < 0, inward is +x),
//    which is why the "LEFT" calipers are the ones at +x. The right side is
//    the same wheel turned half a turn about the vertical axis - turned, not
//    mirrored, so the rim's lettering still reads the right way round.
//  * calipers do not turn with the wheel: FRONT/REAR_LEFT/RIGHT are each
//    already in place for their own corner, at the back of the disc.
//  * WHEEL_REAR and ROTOR_REAR, when present, replace WHEEL and ROTOR at the
//    back (the C11 has wider rears); BLUR meshes are the spinning stand-ins
//    and are left out.
//  * the tyre's radius against the point's height above the ground is the
//    scale - 1.0 when the shared file was made for this car.
// The radius of the wheel arch over a hub: the nearest point of the outer
// body skin (paint, not struts, chassis or liners) in the half circle above
// the hub, within the outer 12 cm of the car's side. 0 when the car has no
// arch there (an open-wheel car).
float archRadius(const Model& car, const float hub[3]) {
    bool haveLodA = false;
    for (const Mesh& m : car.meshes) if (m.name.compare(0, 6, "LOD_A_") == 0) { haveLodA = true; break; }
    float sector[12];
    for (float& v : sector) v = 1e30f;
    float side = std::fabs(hub[0]);
    for (const Mesh& m : car.meshes) {
        const std::string& n = m.name;
        if (haveLodA && n.compare(0, 6, "LOD_A_") != 0) continue;
        if (n.compare(0, 6, "WHEEL_") == 0) continue;
        static const char* skip[] = { "GLOW", "SHADOW", "INTERIOR", "STEERING", "WHEELGUARD", "STRUT",
                                      "CHASSIS", "_mm_chassis", "_mm_cab", "DRIVER", "BLUR" };
        bool bad = false;
        for (const char* k : skip) if (n.find(k) != std::string::npos) { bad = true; break; }
        if (bad) continue;
        for (size_t i = 0; i + 2 < m.positions.size(); i += 3) {
            const float* q = &m.positions[i];
            if ((q[0] > 0) != (hub[0] > 0)) continue;
            float ax = std::fabs(q[0]);
            if (ax < side - 0.12f || ax > side + 0.05f) continue;
            float dy = q[1] - hub[1], dz = q[2] - hub[2];
            if (dy < 0) continue;
            float ang = std::atan2(dy, dz);                 // 0 .. pi
            int k = std::min(11, std::max(0, (int)(ang / 3.14159265f * 12.0f)));
            sector[k] = std::min(sector[k], std::sqrt(dy * dy + dz * dz));
        }
    }
    // every sector over the wheel must be closed, or it is not an arch
    float lo = 1e30f;
    for (int k = 2; k < 10; ++k) {
        if (sector[k] > 1.0f) return 0;
        lo = std::min(lo, sector[k]);
    }
    return lo;
}

int attachSharedCorners(Model& car, const Model& wheel, const std::vector<Hardpoint>& points) {
    static const char* kPoint[4] = { "POINT_WHEEL_FL", "POINT_WHEEL_FR",
                                     "POINT_WHEEL_BL", "POINT_WHEEL_BR" };
    static const char* kTag[4] = { "FL", "FR", "BL", "BR" };
    // the wheels go on the detail level the car file holds (_d.m3g: LOD03)
    std::string carLod;
    for (const Mesh& m : car.meshes)
        if (m.lod.size() == 5 && m.lod.compare(0, 3, "LOD") == 0 && (carLod.empty() || m.lod < carLod)) carLod = m.lod;

    // the finest LOD present
    char best = 0;
    for (const Mesh& m : wheel.meshes) {
        char l;
        if (sharedRole(m.name, &l) == SR_NONE) continue;
        if (!best || (l && l < best)) best = l;
    }
    std::vector<std::pair<const Mesh*, SharedRole>> parts;
    bool hasWheelRear = false, hasRotorRear = false, hasTyreRear = false;
    float tyreRadius = 0, tyreRadiusRear = 0;
    auto radiusOf = [](const Mesh& m) {
        float r = 0;
        for (size_t i = 0; i + 2 < m.positions.size(); i += 3)
            r = std::max(r, std::sqrt(m.positions[i + 1] * m.positions[i + 1] +
                                      m.positions[i + 2] * m.positions[i + 2]));
        return r;
    };
    for (const Mesh& m : wheel.meshes) {
        char l;
        SharedRole r = sharedRole(m.name, &l);
        if (r == SR_NONE || l != best) continue;
        parts.push_back({&m, r});
        if (r == SR_WHEEL_REAR) hasWheelRear = true;
        if (r == SR_ROTOR_REAR) hasRotorRear = true;
        if (r == SR_TYRE) tyreRadius = std::max(tyreRadius, radiusOf(m));
        if (r == SR_TYRE_REAR) { hasTyreRear = true; tyreRadiusRear = std::max(tyreRadiusRear, radiusOf(m)); }
    }
    if (parts.empty()) return -1;          // not a shared corner file
    if (tyreRadius <= 1e-4f)
        for (auto& pr : parts)
            if (pr.second == SR_WHEEL) tyreRadius = std::max(tyreRadius, radiusOf(*pr.first));
    if (tyreRadiusRear <= 1e-4f) tyreRadiusRear = tyreRadius;

    // Parts of the car that belong to a front wheel and turn with it
    // (an F1 car's WHEELGUARD_FRONT_LEFT): modelled round the hub like the
    // calipers, so they go on the wheel's point, not at the car's origin.
    for (Mesh& m : car.meshes) {
        const std::string& n = m.name;
        size_t at = n.find("WHEELGUARD_");
        if (at == std::string::npos) continue;
        int w = -1;
        if (n.find("FRONT_LEFT", at) != std::string::npos) w = 0;
        else if (n.find("FRONT_RIGHT", at) != std::string::npos) w = 1;
        else if (n.find("REAR_LEFT", at) != std::string::npos) w = 2;
        else if (n.find("REAR_RIGHT", at) != std::string::npos) w = 3;
        const Hardpoint* h = w >= 0 ? findPoint(points, kPoint[w]) : nullptr;
        if (!h) continue;
        for (size_t i = 0; i + 2 < m.positions.size(); i += 3)
            for (int c = 0; c < 3; ++c) m.positions[i + c] += h->pos[c];
        refreshBounds(m);
    }

    int placed = 0;
    float scaleShown[2] = { 0, 0 };
    bool fitted = false;
    for (int w = 0; w < 4; ++w) {
        const Hardpoint* h = findPoint(points, kPoint[w]);
        if (!h) continue;
        bool rear = w >= 2;
        bool right = h->pos[0] > 0;
        float radius = rear ? tyreRadiusRear : tyreRadius;
        // The size: the tyre reaches the ground under the hub (the point's
        // height is the rolling radius), and fills the arch to 3.5 cm of its
        // lip. RR3 sizes its wheels from the car's data, which is not in these
        // files; the Zenvo's hub height is its model tyre's own radius, which
        // leaves it lost in its arches, so the arch wins when it is larger.
        // Only the diameter grows - the width stays as modelled, so the tyre
        // does not push into the suspension.
        float scale = (radius > 1e-4f && h->pos[1] > 1e-3f) ? h->pos[1] / radius : 1.0f;
        if (scale < 0.5f || scale > 2.0f) scale = 1.0f;   // a shared file from another car
        float arch = archRadius(car, h->pos);
        if (radius > 1e-4f && arch > radius * 0.8f && arch < radius * 1.8f) {
            float byArch = (arch - 0.035f) / radius;
            if (byArch > scale) { scale = byArch; fitted = true; }
        }
        scaleShown[rear ? 1 : 0] = scale;

        for (auto& pr : parts) {
            SharedRole r = pr.second;
            bool turn = true;
            switch (r) {
                case SR_TYRE:       if (rear && hasTyreRear) continue; break;
                case SR_TYRE_REAR:  if (!rear) continue; break;
                case SR_WHEEL:      if (rear && hasWheelRear) continue; break;
                case SR_WHEEL_REAR: if (!rear) continue; break;
                case SR_ROTOR:      if (rear && hasRotorRear) continue; break;
                case SR_ROTOR_REAR: if (!rear) continue; break;
                case SR_CAL_FL: if (rear || right) continue; turn = false; break;
                case SR_CAL_FR: if (rear || !right) continue; turn = false; break;
                case SR_CAL_RL: if (!rear || right) continue; turn = false; break;
                case SR_CAL_RR: if (!rear || !right) continue; turn = false; break;
                default: continue;
            }
            Mesh copy = *pr.first;
            copy.name = std::string("WHEEL_") + kTag[w] + "_" + pr.first->name;
            copy.part = std::string("wheel_") + kTag[w];
            if (!carLod.empty()) copy.lod = carLod;
            bool rotate = right && turn;
            for (size_t i = 0; i + 2 < copy.positions.size(); i += 3) {
                float x = copy.positions[i];
                float y = copy.positions[i + 1] * scale;
                float z = copy.positions[i + 2] * scale;
                if (rotate) { x = -x; z = -z; }
                copy.positions[i]     = x + h->pos[0];
                copy.positions[i + 1] = y + h->pos[1];
                copy.positions[i + 2] = z + h->pos[2];
            }
            if (scale != 1.0f)   // radial stretch: normals scale by the inverse
                for (size_t i = 0; i + 2 < copy.normals.size(); i += 3) {
                    copy.normals[i + 1] /= scale;
                    copy.normals[i + 2] /= scale;
                }
            if (rotate)
                for (size_t i = 0; i + 2 < copy.normals.size(); i += 3) {
                    copy.normals[i] = -copy.normals[i];
                    copy.normals[i + 2] = -copy.normals[i + 2];
                }
            refreshBounds(copy);
            car.meshes.push_back(std::move(copy));
        }
        ++placed;
    }
    if (placed) {
        char buf[200];
        snprintf(buf, sizeof(buf), "%d wheel corner(s) built from the _shared.m3g "
                 "(tyre, rim, disc, caliper), size x%.3f front, x%.3f rear%s", placed,
                 (double)scaleShown[0], (double)scaleShown[1],
                 fitted ? " (sized to fill the wheel arches)" : "");
        car.warnings.push_back(buf);
    }
    return placed;
}

} // namespace

int rr3AttachWheels(Model& car, const Model& wheel,
                    const std::vector<Hardpoint>& points, bool insetToPoint) {
    static const char* kWheelPoints[4] =
        { "POINT_WHEEL_FL", "POINT_WHEEL_FR", "POINT_WHEEL_BL", "POINT_WHEEL_BR" };
    static const char* kWheelNames[4] = { "FL", "FR", "BL", "BR" };
    if (!wheel.valid || wheel.meshes.empty()) return 0;
    // the car brought its own wheel (Real Racing 2), already on its points
    for (const Mesh& me : car.meshes)
        if (me.name.compare(0, 6, "WHEEL_") == 0 && me.part.compare(0, 6, "wheel_") == 0 &&
            me.name.find("MESH_") != std::string::npos)
            return 0;

    // a <car>_shared.m3g: tyre, rim, disc and calipers by name
    int shared = attachSharedCorners(car, wheel, points);
    if (shared >= 0) return shared;

    // The wheel model is authored around its own hub. Its radius is half the
    // larger of the two cross-axle extents; the hardpoint's height above the
    // ground is the radius the car wants, so the ratio is the scale.
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    // (from the vertices: not every reader fills in the boxes)
    for (const Mesh& mesh : wheel.meshes)
        for (size_t i = 0; i + 2 < mesh.positions.size(); i += 3)
            for (int c = 0; c < 3; ++c) {
                lo[c] = std::min(lo[c], mesh.positions[i + c]);
                hi[c] = std::max(hi[c], mesh.positions[i + c]);
            }
    float size[3] = { hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2] };
    // the axle is the shortest axis; the wheel's diameter is the largest
    float diameter = std::max(size[0], std::max(size[1], size[2]));
    if (diameter <= 1e-6f) return 0;

    // Refuse anything that is not disc-shaped. A wheel is two roughly equal
    // large axes and one much shorter one; a body, a spoiler or a seat is not,
    // and attaching one of those four times over would be far worse than
    // leaving the hardpoints for the user to hang their own wheel on.
    {
        float s[3] = { size[0], size[1], size[2] };
        std::sort(s, s + 3);
        bool disc = s[2] > 1e-6f && s[1] > s[2] * 0.75f && s[0] < s[2] * 0.75f;
        if (!disc) {
            car.warnings.push_back(
                "a wheel model was offered but is not disc-shaped, so it was "
                "not attached; the wheel hardpoints are exported instead");
            return 0;
        }
    }
    float centre[3] = { (lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f,
                        (lo[2] + hi[2]) * 0.5f };

    // the wheels belong to the detail level the car file holds: _d.m3g is
    // LOD03 throughout, and wheels left on LOD00 showed alone while the body
    // was hidden, and vanished once LOD03 was picked
    std::string carLod;
    for (const Mesh& m : car.meshes)
        if (m.lod.size() == 5 && m.lod.compare(0, 3, "LOD") == 0 && (carLod.empty() || m.lod < carLod)) carLod = m.lod;
    int placed = 0;
    for (int w = 0; w < 4; ++w) {
        const Hardpoint* h = findPoint(points, kWheelPoints[w]);
        if (!h) continue;
        float wantRadius = h->pos[1];               // height of the hub above y=0
        float scale = wantRadius > 1e-4f ? (wantRadius * 2.0f) / diameter : 1.0f;
        bool right = h->pos[0] > 0;

        for (const Mesh& src : wheel.meshes) {
            Mesh copy = src;
            copy.name = src.name + "_" + kWheelNames[w];
            copy.part = copy.name;
            if (!carLod.empty()) copy.lod = carLod;
            for (size_t i = 0; i + 2 < copy.positions.size(); i += 3) {
                float x = (copy.positions[i + 0] - centre[0]) * scale;
                float y = (copy.positions[i + 1] - centre[1]) * scale;
                float z = (copy.positions[i + 2] - centre[2]) * scale;
                // a wheel is modelled for one side; the other is its mirror,
                // and the winding is flipped below so it is not inside out
                if (right) x = -x;
                if (insetToPoint) x += (right ? -1.0f : 1.0f) * (hi[0] - lo[0]) * 0.5f * scale;
                copy.positions[i + 0] = x + h->pos[0];
                copy.positions[i + 1] = y + h->pos[1];
                copy.positions[i + 2] = z + h->pos[2];
            }
            if (right) {
                for (size_t i = 0; i + 2 < copy.indices.size(); i += 3)
                    std::swap(copy.indices[i + 1], copy.indices[i + 2]);
                for (size_t i = 0; i + 2 < copy.normals.size(); i += 3)
                    copy.normals[i] = -copy.normals[i];
            }
            for (int c = 0; c < 3; ++c) {
                copy.bboxMin[c] = 1e30f; copy.bboxMax[c] = -1e30f;
            }
            for (size_t i = 0; i + 2 < copy.positions.size(); i += 3)
                for (int c = 0; c < 3; ++c) {
                    copy.bboxMin[c] = std::min(copy.bboxMin[c], copy.positions[i + c]);
                    copy.bboxMax[c] = std::max(copy.bboxMax[c], copy.positions[i + c]);
                }
            car.meshes.push_back(std::move(copy));
        }
        ++placed;
    }
    if (placed) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%d wheel(s) attached at their .points positions", placed);
        car.warnings.push_back(buf);
    }
    return placed;
}

} // namespace nfsnl

namespace nfsnl {

namespace {

std::string lowerCopy(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

// what comes after _mm_ in the material (or, failing that, the mesh) name
std::string mmSuffix(const Mesh& mesh) {
    for (const std::string* n : { &mesh.material, &mesh.name }) {
        std::string low = lowerCopy(*n);
        size_t at = low.find("_mm_");
        if (at != std::string::npos) {
            std::string x = low.substr(at + 4);
            while (!x.empty() && (x.back() == ' ' || x.back() == 0)) x.pop_back();
            return x;
        }
    }
    // the driver model's materials carry no _mm_
    std::string low = lowerCopy(mesh.material);
    if (low.find("driver") != std::string::npos) return "driver";
    if (low.find("arms") != std::string::npos) return "arms";
    return std::string();
}

// "livery/1970_porsche_917k_ext_23.ptc.pvr.z" -> "1970_porsche_917k_ext_23"
std::string leafStem(const std::string& path) {
    std::string leaf = lowerCopy(path.substr(path.find_last_of("/\\") + 1));
    return leaf.substr(0, leaf.find('.'));
}

// A shared texture's name with the game's decoration taken off, so that it
// can be compared with a material's _mm_ name:
// car_windows_02 -> windows, car_windows_cracked_d_02 -> windows_cracked_d,
// car_tyre_tread_1972 -> tyre_tread
std::string commonKey(std::string st) {
    if (st.compare(0, 4, "car_") == 0) st.erase(0, 4);
    for (;;) {
        size_t u = st.find_last_of('_');
        if (u == std::string::npos || u + 1 >= st.size()) break;
        bool digits = true;
        for (size_t k = u + 1; k < st.size(); ++k) if (st[k] < '0' || st[k] > '9') digits = false;
        if (!digits) break;
        st.erase(u);
    }
    return st;
}

// how well a shared texture fits a material: 3 exact, 2 more specific
// (tyre -> tyre_tread), 1 the same trailing word (mat04 -> suede_..._mat04)
int commonFit(const std::string& x, const std::string& stem) {
    std::string key = commonKey(stem);
    if (key == x) return 3;
    if (key.compare(0, x.size() + 1, x + "_") == 0 &&
        // the plain glass is not the cracked or the shattered one
        ((key.find("crack") == std::string::npos && key.find("shatter") == std::string::npos) ||
         x.find("crack") != std::string::npos || x.find("shatter") != std::string::npos))
        return 2;
    if (key.size() > x.size() + 1 && key.compare(key.size() - x.size() - 1, x.size() + 1, "_" + x) == 0)
        return 1;
    return 0;
}

} // namespace

int rr3AssignTextures(Model& m, const std::string& modelPath,
                      const std::function<std::string(const std::string&)>& find,
                      const std::vector<std::pair<std::string, std::string>>& common,
                      const NctLiveries* liveries, std::string* report) {
    // 1979_porsche_935_a.m3g / 1979_porsche_935_int_a.m3g -> 1979_porsche_935
    std::string car = lowerCopy(stripExtension(baseName(modelPath)));
    if (car.size() > 2 && car[car.size() - 2] == '_' && car.back() >= 'a' && car.back() <= 'z')
        car.erase(car.size() - 2);
    bool interior = false;
    if (car.size() > 4 && car.compare(car.size() - 4, 4, "_int") == 0) {
        car.erase(car.size() - 4);
        interior = true;
    }
    if (car.size() > 7 && car.compare(car.size() - 7, 7, "_shared") == 0) car.erase(car.size() - 7);
    if (car.empty()) return 0;

    std::map<std::string, std::string> found;     // stem -> path ("" = not there)
    auto look = [&](const std::string& stem) -> std::string {
        auto it = found.find(stem);
        if (it != found.end()) return it->second;
        std::string p = find(stem);
        found[stem] = p;
        return p;
    };

    // The car's .liveries file lists every texture its materials draw on,
    // the shared ones in common/ included, and names the default paint.
    std::vector<std::string> table;               // its common/ textures' stems
    std::string defaultLivery;
    if (liveries && liveries->ok)
        for (const NctLiveries::Texture& t : liveries->textures) {
            if (!t.sure) continue;
            std::string st = leafStem(t.path);
            if (lowerCopy(t.path).compare(0, 7, "common/") == 0) table.push_back(st);
            if (defaultLivery.empty() && lowerCopy(t.path).compare(0, 7, "livery/") == 0)
                defaultLivery = st;
        }

    // the best shared texture for a material: the ones the car's own table
    // names first, then anything in common/
    auto fromCommon = [&](const std::string& x) -> std::string {
        int bestFit = 0;
        std::string best;
        for (const std::string& st : table) {
            int f = commonFit(x, st);
            if (f > bestFit) {
                std::string p = look(st);
                if (!p.empty()) { bestFit = f; best = p; }
            }
        }
        if (bestFit >= 2) return best;
        std::string bestStem;
        for (const auto& c : common) {
            int f = commonFit(x, c.first);
            if (f > bestFit || (f == bestFit && f > 0 && !bestStem.empty() && c.first < bestStem)) {
                bestFit = f;
                best = c.second;
                bestStem = c.first;
            }
        }
        return best;
    };

    int assigned = 0;
    std::map<std::string, std::string> materialTex;
    std::map<std::string, std::string> missing;   // _mm_ name -> what was looked for
    for (Mesh& mesh : m.meshes) {
        std::string x = mmSuffix(mesh);
        if (x.empty() && (mesh.texture.find('\\') != std::string::npos ||
                          lowerCopy(mesh.texture).find(".psd") != std::string::npos)) {
            // A track's appearances still name the artist's source file:
            // C:/.../Tracks/monaco/resources/arc/arc_monaco_common/ (backslashes)
            // arc_common_monaco_wall_06.psd. The game ships that picture as
            // <same name>.etc.dds, so the name alone finds it.
            std::string src = mesh.texture;
            size_t cut = src.find_last_of("\\/");
            std::string stem = lowerCopy(src.substr(cut == std::string::npos ? 0 : cut + 1));
            stem = stem.substr(0, stem.find('.'));
            std::string path = stem.empty() ? std::string() : look(stem);
            if (!path.empty()) {
                mesh.texture = path;
                ++assigned;
                if (!mesh.material.empty()) materialTex[mesh.material] = path;
                continue;
            }
            mesh.texture = stem;             // not in the library: keep the name
        }
        if (x.empty()) {
            // A track: materials are "<shader>-<texture>" -
            // nobias-arc_common_monaco_wall_04, glass_nobias-arc_monaco_albert_bldg_001,
            // tree-org_tree_oak_b_type_a - and the texture is a file of that
            // name somewhere under the track's resources or Tracks/common.
            std::string mat = lowerCopy(mesh.material);
            while (!mat.empty() && (mat.back() == ' ' || mat.back() == 0)) mat.pop_back();
            if (mat.empty() || mat.find('#') != std::string::npos ||
                mat.find("default") != std::string::npos)
                continue;
            size_t dash = mat.find_last_of('-');
            std::string stem = dash == std::string::npos ? mat : mat.substr(dash + 1);
            for (char& ch : stem) if (ch == ' ') ch = '_';
            if (stem.empty()) continue;
            std::string path;
            for (const std::string& t : { stem, stem + "_01", stem + "_a" }) {
                path = look(t);
                if (!path.empty()) break;
            }
            // A sky reused from another track keeps that track's name:
            // Brands Hatch's hatch_sky.m3g asks for cota_sky_hills, and the
            // picture the game draws is the track's own hatch_sky_hills
            if (path.empty() && stem.find("sky") != std::string::npos) {
                size_t sk = car.find("_sky");
                size_t us = stem.find('_');
                if (sk != std::string::npos && sk > 0 && us != std::string::npos)
                    path = look(car.substr(0, sk) + stem.substr(us));
            }
            if (path.empty()) {
                if (!missing.count(stem)) missing[stem] = stem;
                continue;
            }
            if (mesh.texture.find('/') == std::string::npos) { mesh.texture = path; ++assigned; }
            materialTex[mesh.material] = path;
            continue;
        }
        std::vector<std::string> tries;           // the car's own files, in order
        bool shared = false;                      // then look in common/
        if (x == "ext") {
            if (!defaultLivery.empty()) tries.push_back(defaultLivery);
            for (const char* s : { "_ext_01", "_ext_1", "_ext_default", "_ext" }) tries.push_back(car + s);
            tries.push_back(car + "_ext_*");
        } else if (x == "tyre" || x == "tire") {
            shared = true;                        // common/car_tyre_tread_<year>
        } else if (x == "wheel") {
            // most cars: <car>_wheel; some name the colour instead
            // (2015_renault_megane_trophy_r_wheel_red)
            tries = { car + "_wheel", car + "_wheel_*" };
        } else if (x == "rotor" || x == "rotor_rear" || x == "wheel_rear" || x == "caliper") {
            tries = { car + "_" + x, car + "_wheel", car + "_wheel_*" };
        } else if (x == "wheel_blur") {
            tries = { car + "_wheel_blur", car + "_wheel" };
        } else if (x == "sw" || x == "steering_wheel") {
            tries = { car + "_sw", car + "_int" };
        } else if (x.compare(0, 7, "windows") == 0 || x == "driver" || x == "arms" ||
                   x.compare(0, 3, "mat") == 0 || x == "scratches") {
            tries = { car + "_" + x };
            shared = true;
        } else {
            tries = { car + "_" + x };
            shared = true;
            if (interior) tries.push_back(car + "_int");
        }
        std::string path;
        for (const std::string& t : tries) {
            path = look(t);
            if (!path.empty()) break;
        }
        if (path.empty() && shared) path = fromCommon(x);
        if (path.empty()) {
            if (!missing.count(x)) missing[x] = tries.empty() ? "common/" + x : tries.front();
            if (x.compare(0, 7, "windows") == 0) {
                mesh.color[0] = 0.16f; mesh.color[1] = 0.19f; mesh.color[2] = 0.22f; mesh.color[3] = 0.45f;
            }
            continue;
        }
        if (mesh.texture.empty() || mesh.texture.find('/') == std::string::npos) {
            mesh.texture = path;
            ++assigned;
        }
        if (!mesh.material.empty()) materialTex[mesh.material] = path;
    }

    // the material list: what the file declares, with the textures found and
    // the render states the names imply
    for (Material& mt : m.materials) {
        auto it = materialTex.find(mt.name);
        if (it != materialTex.end() && mt.diffuse.empty()) mt.diffuse = it->second;
        std::string low = lowerCopy(mt.name);
        if (low.find("windows") != std::string::npos || low.find("scratches") != std::string::npos)
            mt.alphaBlend = true;
        if (low.find("glow") != std::string::npos) { mt.additive = true; mt.alphaBlend = true; }
    }
    for (auto& kv : materialTex) {
        bool seen = false;
        for (const Material& mt : m.materials) if (mt.name == kv.first) seen = true;
        if (seen) continue;
        Material mt;
        mt.name = kv.first;
        mt.diffuse = kv.second;
        m.materials.push_back(mt);
    }
    if (report) {
        report->clear();
        int listed = 0, more = 0;
        for (auto& kv : missing) {
            if (kv.first == "headlight_glow") continue;
            if (listed >= 12) { ++more; continue; }
            if (!report->empty()) *report += ", ";
            *report += kv.first;
            ++listed;
        }
        if (more) *report += ", and " + std::to_string(more) + " more";
    }
    return assigned;
}

} // namespace nfsnl

namespace nfsnl {

// A texture shipped as an M3G file of its own (Hot Pursuit's
// texture_*.m3g): the first Image2D in it. Object3D's common start (user id,
// animation tracks, user parameters), then format, mutable flag, width,
// height, a palette and the pixels. The pixel byte count, not the format
// code, decides the channel count - EA's files put their own values there.
bool decodeM3gImage(const uint8_t* data, size_t len, Image& out) {
    out = Image();
    int version = 0;
    if (!identifierAt(data, len, 0, &version)) return false;
    // gather the object bytes: sectioned (JSR-184, maybe zlib) or one run
    Bytes objects;
    size_t p = 12;
    while (p + 9 <= len) {
        uint8_t comp = data[p];
        uint32_t total = rd32(data + p + 1), plain = rd32(data + p + 5);
        if (total < 13 || p + total > len || plain > (1u << 28)) break;
        const uint8_t* body = data + p + 9;
        size_t bodyLen = total - 13;
        if (comp == 0) objects.insert(objects.end(), body, body + std::min<size_t>(bodyLen, plain));
        else {
            Bytes un;
            if (!inflateZlib(body, bodyLen, un)) break;
            objects.insert(objects.end(), un.begin(), un.end());
        }
        p += total;
        if (version >= 2) break;         // the IM layouts have one run only
    }
    size_t q = 0;
    while (q + 5 <= objects.size()) {
        uint8_t type = objects[q];
        uint32_t n = rd32(objects.data() + q + 1);
        if (q + 5 + (size_t)n > objects.size()) break;
        if (type == 10 && n >= 30) {
            Reader r(objects.data() + q + 5, n);
            r.u32();                                  // user id
            uint32_t tracks = r.u32();
            if (tracks > 1024) return false;
            r.p += (size_t)tracks * 4;
            uint32_t params = r.u32();
            for (uint32_t k = 0; k < params && k < 256 && !r.bad; ++k) {
                r.u32();
                uint32_t sz = r.u32();
                if (sz > n) return false;
                r.p += sz;
            }
            int fmt = r.u8();                         // format
            bool mut = r.u8() != 0;
            uint32_t w = r.u32(), h = r.u32();
            if (r.bad || mut || !w || !h || w > 8192 || h > 8192) return false;
            uint32_t pal = r.u32();
            if (pal > n) return false;
            r.p += pal;
            uint32_t pix = r.u32();
            if (r.bad || r.p + pix > n || pix == 0) return false;
            size_t count = (size_t)w * h;
            // NFS Undercover and Shift: EA's PVRTC 4 bpp codes, mips after
            if (fmt == 124 || fmt == 125)
                return decodeM3gPixels(fmt, (int)w, (int)h, objects.data() + q + 5 + r.p, pix, out);
            int ch = pix == count * 4 ? 4 : pix == count * 3 ? 3 : pix == count * 2 ? 2
                   : pix == count ? 1 : 0;
            if (!ch) return false;
            const uint8_t* px = objects.data() + q + 5 + r.p;
            out.width = (int)w;
            out.height = (int)h;
            out.channels = ch == 2 ? 4 : ch;
            if (ch == 2) {                            // luminance + alpha
                out.pixels.resize(count * 4);
                for (size_t i = 0; i < count; ++i) {
                    out.pixels[i * 4] = out.pixels[i * 4 + 1] = out.pixels[i * 4 + 2] = px[i * 2];
                    out.pixels[i * 4 + 3] = px[i * 2 + 1];
                }
            } else {
                out.pixels.assign(px, px + count * ch);
            }
            return true;
        }
        q += 5 + (size_t)n;
    }
    return false;
}

// ================================================================ Real Racing 1 / GTI
//
// A car is a .rr_car record: its display name, then fixed-width file names -
// model, wheel model, wheel texture, points, exterior, interior and steering
// wheel textures (GTI adds lights and window tape). The model's 36 meshes
// carry no names; they come in the same order in every car:
//    0-3   the body, LOD 0 to 3          4   the brake-light glow
//    5     the cockpit                   6   the steering wheel (at the origin)
//    7     the bonnet and screen as seen from inside
//    16,17 the driver's hands (at the steering wheel's origin)
//    33-35 the body, roof lining and dashboard as seen from inside
//    the rest: gauge digits, needles and warning lights (single quads)
bool rr1ReadCar(const uint8_t* d, size_t n, Rr1Car& car) {
    std::vector<std::string> s;
    std::string cur;
    for (size_t i = 0; i <= n; ++i) {
        uint8_t c = i < n ? d[i] : 0;
        if (c >= 0x20 && c < 0x7F) { cur.push_back((char)c); continue; }
        if (cur.size() >= 4) s.push_back(cur);
        cur.clear();
    }
    if (s.size() < 5) return false;
    size_t k = 0;
    if (extensionOf(s[0]) != "m3g") car.name = s[k++];
    auto next = [&](std::string& out) { if (k < s.size()) out = s[k++]; };
    next(car.model);
    if (extensionOf(car.model) != "m3g") return false;
    next(car.wheelModel);
    next(car.wheelTexture);
    next(car.points);
    next(car.exterior);
    next(car.interior);
    next(car.steering);
    while (k < s.size()) car.extra.push_back(s[k++]);
    return true;
}

bool rr1IsCar(const Model& m) {
    if (m.meshes.size() < 8) return false;
    for (size_t i = 0; i < m.meshes.size(); ++i)
        if (m.meshes[i].name.compare(0, 5, "mesh_") != 0) return false;
    return true;
}

void rr1OrganizeCar(Model& m, const std::vector<Hardpoint>& pts, const Rr1Car* car) {
    if (!rr1IsCar(m)) return;
    const Hardpoint* sw = findPoint(pts, "POINT_STEERING_WHEEL");
    for (size_t i = 0; i < m.meshes.size(); ++i) {
        Mesh& me = m.meshes[i];
        bool exterior = false, steering = false;
        if (i <= 3) {
            char b[8];
            snprintf(b, sizeof(b), "LOD%02d", (int)i);
            me.lod = b;
            me.part = "body";
            exterior = true;
        } else if (i == 4) {
            me.lod = "LOD00"; me.part = "brake_light_glow"; exterior = true;
        } else if (i == 6) {
            me.lod = "COCKPIT"; me.part = "steering_wheel"; steering = true;
        } else if (i == 16 || i == 17) {
            me.lod = "COCKPIT"; me.part = i == 16 ? "hand_left" : "hand_right"; steering = true;
        } else {
            me.lod = "COCKPIT";
            if (i == 5) me.part = "cockpit";
            else if (i == 7) me.part = "bonnet_inside";
            else if (me.positions.size() <= 12) me.part = "gauge";
            else me.part = "cockpit_" + std::to_string(i);
            exterior = i == 7 || i == 33;
        }
        // the steering wheel and the hands are modelled round the origin
        if (steering && sw) {
            float c[3] = {0, 0, 0};
            size_t nv = me.positions.size() / 3;
            for (size_t v = 0; v < nv; ++v)
                for (int a = 0; a < 3; ++a) c[a] += me.positions[v * 3 + a];
            bool atOrigin = nv && std::fabs(c[0] / nv) < 0.35f && std::fabs(c[1] / nv) < 0.35f &&
                            std::fabs(c[2] / nv) < 0.35f;
            if (atOrigin) {
                for (size_t v = 0; v < nv; ++v)
                    for (int a = 0; a < 3; ++a) me.positions[v * 3 + a] += sw->pos[a];
                for (int a = 0; a < 3; ++a) { me.bboxMin[a] += sw->pos[a]; me.bboxMax[a] += sw->pos[a]; }
            }
        }
        if (car) {
            std::string t = steering && i == 6 ? car->steering : exterior ? car->exterior : car->interior;
            // the brake-light glow samples the car's lights texture when the
            // record names one (GTI: gti_lights.pvr), not the body's
            if (i == 4)
                for (const std::string& x : car->extra)
                    if (lowered(x).find("light") != std::string::npos) { t = x; break; }
            if (!t.empty()) me.texture = t;
        }
    }
    // the "left at the origin" note is about named parts; these are placed
    m.warnings.erase(std::remove_if(m.warnings.begin(), m.warnings.end(), [](const std::string& w) {
                         return w.compare(0, 18, "left at the origin") == 0; }), m.warnings.end());
    m.warnings.push_back("Real Racing 1 car: body LOD 0-3, the cockpit in its own group, "
                         "the steering wheel and hands on the steering point");
}

} // namespace nfsnl


namespace nfsnl {

std::string rr3FolderKey(const std::string& path) {
    std::string p = path;
    for (char& c : p) { if (c == '\\') c = '/'; if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); }
    size_t s1 = p.find_last_of('/');
    if (s1 == std::string::npos || s1 == 0) return std::string();
    size_t s0 = p.find_last_of('/', s1 - 1);
    std::string parent = p.substr(s0 == std::string::npos ? 0 : s0 + 1, s1 - (s0 == std::string::npos ? 0 : s0 + 1));
    if (parent == "billboarded" && s0 != std::string::npos && s0 > 0) {
        size_t sg = p.find_last_of('/', s0 - 1);
        return p.substr(sg == std::string::npos ? 0 : sg + 1, s0 - (sg == std::string::npos ? 0 : sg + 1)) + "/billboarded";
    }
    return parent;
}

// Real Racing 3's shared track objects are filed by name, but the picture in
// the folder may be a sibling's: common/org/org_tree_oak_d/ holds
// org_tree_oak_c.etc.dds.z, common/veh/veh_caravan_e/ veh_caravan_d. The
// billboard versions (<name>_type_a ...) sit in the folder's billboarded/,
// some types only.
std::string rr3FolderTexture(const std::string& stem, const std::multimap<std::string, std::string>& byFolder) {
    auto pick = [&](const std::string& key, const std::string& want) -> std::string {
        std::string best;
        int bestRank = 1 << 30;
        auto range = byFolder.equal_range(key);
        for (auto it = range.first; it != range.second; ++it) {
            std::string leaf = baseName(it->second);
            for (char& c : leaf) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (leaf.find("_alpha.") != std::string::npos || leaf.find("_etcalpha") != std::string::npos) continue;
            int rank = (want.empty() || leaf.find(want) != std::string::npos ? 0 : 2) +
                       (leaf.find(".etc.") != std::string::npos ? 0 : 1);
            if (rank < bestRank || (rank == bestRank && it->second < best)) { bestRank = rank; best = it->second; }
        }
        return best;
    };
    std::string p = pick(stem, std::string());
    if (!p.empty()) return p;
    size_t ty = stem.rfind("_type_");
    if (ty != std::string::npos) return pick(stem.substr(0, ty) + "/billboarded", stem.substr(ty));
    return std::string();
}

} // namespace nfsnl
