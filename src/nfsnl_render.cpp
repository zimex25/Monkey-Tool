// nfsnl_render.cpp - a small software renderer for the model viewer
//
// Deliberately software rather than OpenGL: it needs no context, no driver
// and no extension loading, it behaves the same on every machine, and - the
// reason that matters here - it can be run and looked at on a build machine
// that has no Windows at all. The Win32 side only has to blit the result.
#include "nfsnl.h"
#include <map>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace nfsnl {

namespace {

struct Vec3 { float x, y, z; };

inline Vec3 sub(const Vec3& a, const Vec3& b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x};
}
inline float dot(const Vec3& a, const Vec3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline Vec3 norm(const Vec3& v) {
    float l = std::sqrt(dot(v, v));
    return l > 1e-9f ? Vec3{v.x/l, v.y/l, v.z/l} : Vec3{0, 0, 1};
}

// one vertex after transform: screen position, depth, shading and uv
struct Vtx {
    float sx, sy;     // screen pixels
    float z;          // view depth, positive in front
    float invW;
    float shade;
    float u, v;
};

uint8_t sampleClamp(const Image& t, int x, int y, int c) {
    x = std::min(std::max(x, 0), t.width - 1);
    y = std::min(std::max(y, 0), t.height - 1);
    return t.pixels[((size_t)y * t.width + x) * t.channels + std::min(c, t.channels - 1)];
}

} // namespace

static bool isGlowCard(const Mesh& mesh) {
    const std::string& mat = mesh.material;
    return mat.find("additive") != std::string::npos || mat.find("alphaadd") != std::string::npos ||
           mat.find("falloff") != std::string::npos || mat.find("lightfx") != std::string::npos ||
           mesh.name.find("_glow") != std::string::npos || mesh.name.find("additive") != std::string::npos;
}

std::vector<uint8_t> visibleMeshes(const Model& m, const RenderView& view) {
    std::vector<uint8_t> vis = kitMask(m, view.kit);
    vis.resize(m.meshes.size(), 1);
    for (size_t i = 0; i < m.meshes.size(); ++i) {
        const Mesh& mesh = m.meshes[i];
        // a part with no detail level of its own belongs to every level
        if (!view.lodFilter.empty() && !mesh.lod.empty() && mesh.lod != view.lodFilter) vis[i] = 0;
        if (mesh.positions.empty() || mesh.indices.empty()) vis[i] = 0;
        if (view.hideGlow && isGlowCard(mesh)) vis[i] = 0;
    }
    return vis;
}

void viewAxes(const RenderView& view, float right[3], float up[3], float fwd[3]) {
    float cy = std::cos(view.yaw), sy = std::sin(view.yaw);
    float cp = std::cos(view.pitch), sp = std::sin(view.pitch);
    right[0] = -cy;     right[1] = 0;   right[2] = sy;
    up[0] = sy * sp;    up[1] = cp;     up[2] = cy * sp;
    fwd[0] = sy * cp;   fwd[1] = -sp;   fwd[2] = cy * cp;
}

float floorHeight(const Model& m, const RenderView& view) {
    std::vector<uint8_t> vis = visibleMeshes(m, view);
    float lo = 1e30f;
    for (size_t i = 0; i < m.meshes.size(); ++i) {
        if (!vis[i]) continue;
        const Mesh& mesh = m.meshes[i];
        for (size_t k = 1; k < mesh.positions.size(); k += 3) lo = std::min(lo, mesh.positions[k]);
    }
    if (lo > 1e29f) lo = view.centre[1] - view.radius;
    return lo + view.modelPos[1];
}

float gridStep(float radius) {
    float target = std::max(1e-4f, radius / 6.0f);
    float p = std::pow(10.0f, std::floor(std::log10(target)));
    for (float k : { 1.0f, 2.0f, 5.0f, 10.0f })
        if (p * k >= target) return p * k;
    return p * 10.0f;
}

AlphaStats alphaStats(const Image& img) {
    AlphaStats st;
    if (!img.ok() || img.channels != 4) return st;
    size_t n = (size_t)img.width * img.height, clear = 0, mid = 0;
    for (size_t i = 0; i < n; ++i) {
        uint8_t a = img.pixels[i * 4 + 3];
        if (a < 20) ++clear;
        else if (a < 235) ++mid;
    }
    if (n) { st.clear = (float)clear / n; st.mid = (float)mid / n; }
    st.has = st.clear + st.mid > 0.002f;
    return st;
}

AlphaMode meshAlphaMode(const Mesh& mesh, const AlphaStats* tex, float* alphaScale) {
    std::string t = mesh.material + " " + mesh.name;
    for (char& c : t) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    auto has = [&](const char* k) { return t.find(k) != std::string::npos; };
    if (alphaScale) *alphaScale = 1.0f;
    // No Limits says it outright: *_opaque, *_alpha, *_alphaadd
    bool saysOpaque = has("opaque") && !has("_alpha");
    if (saysOpaque) return ALPHA_OPAQUE;
    // NFS Undercover / Shift: the file's own blending, as the loader named it
    if (has("m3g_alpha_cutout")) return ALPHA_CUTOUT;
    if (has("m3g_alpha")) return ALPHA_BLEND;
    // Parts that are solid whatever their texture's alpha holds: Real Racing
    // 3's tyre tread keeps a mask there (drawn as a cut-out the tyres
    // vanished), and paint, rims and brakes use it for gloss.
    if (has("blur")) return ALPHA_BLEND;
    for (const char* k : { "tyre", "tire", "rubber", "_mm_ext", "_mm_wheel", "_mm_rotor", "_mm_caliper",
                           "_mm_chassis", "_mm_carbon", "paint" })
        if (has(k)) return ALPHA_OPAQUE;
    // The driver (Real Racing 3's HAND_STEER, HAND_GEAR, ARMS, the body and
    // helmet): solid skin and cloth, whatever mask the texture's alpha holds
    for (const char* k : { "hand_", "_hand", "arm_", "_arm", "arms", "driver", "glove", "helmet",
                           "_suit", "body_driver" })
        if (has(k)) return ALPHA_OPAQUE;
    bool glass = has("glass") || has("window") || has("windscreen") || has("lens") ||
                 has("transparent");
    // Real Racing 3's own parts (LOD_A_..., _mm_...) are never half see-through
    // unless they are glass or lights: the alpha there is gloss or a mask,
    // so at most a cut-out
    bool rr3Part = has("_mm_") || has("lod_a_") || has("lod_b_") || has("vehicle interior");
    // Real Racing 2 (MESH_BODY_HIGH ...): the alpha of its exterior texture is
    // the reflection mask, and blended it left the whole car see-through
    bool rr2Part = mesh.name.compare(0, 5, "MESH_") == 0;
    bool rr3Seethrough = glass || has("light") || has("led") || has("shadow") || has("decal");
    bool saysAlpha = glass || has("alpha");
    if (rr2Part && !(glass || has("light") || has("lamp") || has("shadow") || has("blur"))) {
        if (tex && tex->has && (has("alpha") || has("decal") || has("grill") || has("net") || has("fence")))
            return ALPHA_CUTOUT;
        return ALPHA_OPAQUE;
    }
    if (mesh.color[3] < 0.99f) {
        if (alphaScale) *alphaScale = std::max(0.05f, mesh.color[3]);
        return ALPHA_BLEND;
    }
    if (tex && tex->has) {
        // mostly fully clear or fully solid: a cut-out (badges, grilles,
        // decals); a lot of in-between: see-through (glass, tints)
        float part = tex->clear + tex->mid;
        if (rr3Part && !rr3Seethrough) return ALPHA_CUTOUT;
        if (tex->mid > part * 0.3f || (glass && tex->mid > 0.02f)) return ALPHA_BLEND;
        return ALPHA_CUTOUT;
    }
    // glass with no alpha in its picture: tinted, half see-through
    if (glass) {
        if (alphaScale) *alphaScale = 0.5f;
        return ALPHA_BLEND;
    }
    (void)saysAlpha;
    return ALPHA_OPAQUE;
}

std::string rr3ShaderFor(const Mesh& mesh) {
    std::string t = mesh.material + " " + mesh.name;
    for (char& c : t) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    auto has = [&](const char* k) { return t.find(k) != std::string::npos; };
    if (!has("_mm_") && !has("lod_a_") && !has("lod_b_") && !has("lod_c_")) return std::string();
    if (has("shadow") || has("blur") || has("decal")) return std::string();
    if (has("window") || has("glass") || has("windscreen")) return "glass";
    if (has("tyre") || has("tire")) return "tires";
    if (has("rotor") || has("disc")) return "rotor";
    if (has("chrome")) return "chrome";
    if (has("carbon")) return "carbon_fibre";
    if (has("light") || has("lamp") || has("led")) return "light_housing";
    if (has("rim") || has("wheel") || has("caliper")) return "wheel_alloy";
    if (has("_mm_ext") || has("paint") || has("body")) return "gloss";
    if (has("hand") || has("arm") || has("driver") || has("seat") || has("belt")) return "cloth";
    if (has("_mm_int") || has("_mm_mat") || has("_mm_sw") || has("dash")) return "dash_matte";
    return "matte";
}

Image rr3ShaderMatcap(const Image* fresnel, const Image* spec, int size) {
    Image out;
    if (size < 8) return out;
    Image fr, sp;
    if (fresnel && fresnel->ok()) fr = toRgba(*fresnel);
    if (spec && spec->ok()) sp = toRgba(*spec);
    auto ramp = [](const Image& im, float x, float rgb[3]) {
        rgb[0] = rgb[1] = rgb[2] = 0;
        if (!im.ok()) return;
        int w = im.width;
        int i = std::max(0, std::min(w - 1, (int)(x * (w - 1) + 0.5f)));
        const uint8_t* p = &im.pixels[(size_t)i * 4];     // first row
        for (int k = 0; k < 3; ++k) rgb[k] = p[k] / 255.0f;
    };
    out.width = out.height = size;
    out.channels = 4;
    out.pixels.assign((size_t)size * size * 4, 0);
    // light from over the viewer's shoulder, in eye space
    float L[3] = { -0.35f, 0.7f, 0.62f };
    float ll = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    for (float& v : L) v /= ll;
    float H[3] = { L[0], L[1], L[2] + 1.0f };
    float hl = std::sqrt(H[0] * H[0] + H[1] * H[1] + H[2] * H[2]);
    for (float& v : H) v /= hl;
    for (int j = 0; j < size; ++j)
        for (int i = 0; i < size; ++i) {
            // row 0 is t = 0, the bottom of the sphere, as GL uploads it
            float x = (i + 0.5f) / size * 2 - 1, y = (j + 0.5f) / size * 2 - 1;
            float r2 = x * x + y * y;
            uint8_t* d = &out.pixels[((size_t)j * size + i) * 4];
            d[3] = 255;
            if (r2 >= 1.0f) { x /= std::sqrt(r2) * 1.0001f; y /= std::sqrt(r2) * 1.0001f; r2 = x * x + y * y; }
            float nz = std::sqrt(std::max(0.0f, 1 - r2));
            // under the viewer, the sphere's normal is the part's normal
            float ndv = nz;
            float ry = 2 * nz * y;                      // reflected ray, up component
            float env[3];
            if (ry >= 0) {
                float k = std::min(1.0f, ry);
                env[0] = 0.86f + (0.46f - 0.86f) * k;
                env[1] = 0.89f + (0.63f - 0.89f) * k;
                env[2] = 0.93f + (0.86f - 0.93f) * k;
            } else {
                float k = std::min(1.0f, -ry * 3);
                env[0] = 0.86f + (0.30f - 0.86f) * k;
                env[1] = 0.89f + (0.30f - 0.89f) * k;
                env[2] = 0.93f + (0.32f - 0.93f) * k;
            }
            float f[3], s[3];
            ramp(fr, 1.0f - ndv, f);
            float ndh = std::max(0.0f, x * H[0] + y * H[1] + nz * H[2]);
            ramp(sp, ndh, s);
            for (int k = 0; k < 3; ++k) {
                float v = f[k] * env[k] * 0.55f + s[k] * 0.8f;
                d[k] = (uint8_t)std::max(0.0f, std::min(255.0f, v * 255.0f));
            }
        }
    return out;
}

bool wantsShadow(const RenderView& view) {
    return view.shadow && view.floor && view.radius > 0 && view.radius < 40.0f;
}

void shadowLight(int jitter, float L[3]) {
    static const float d[4][2] = { {0, 0}, {0.05f, 0}, {0, 0.05f}, {0.05f, 0.05f} };
    float x = 0.18f + d[jitter & 3][0], y = 1.0f, z = 0.14f + d[jitter & 3][1];
    float n = std::sqrt(x * x + y * y + z * z);
    L[0] = x / n; L[1] = y / n; L[2] = z / n;
}

bool modelFootprint(const Model& m, const RenderView& view, float lo[3], float hi[3]) {
    std::vector<uint8_t> vis = visibleMeshes(m, view);
    for (int k = 0; k < 3; ++k) { lo[k] = 1e30f; hi[k] = -1e30f; }
    for (size_t i = 0; i < m.meshes.size(); ++i) {
        if (!vis[i]) continue;
        const std::vector<float>& p = m.meshes[i].positions;
        for (size_t j = 0; j + 2 < p.size(); j += 3)
            for (int k = 0; k < 3; ++k) {
                lo[k] = std::min(lo[k], p[j + k]);
                hi[k] = std::max(hi[k], p[j + k]);
            }
    }
    return lo[0] <= hi[0];
}

void skyColours(float top[3], float horizon[3]) {
    // Frosty Editor's "Default Lit": a clear blue overhead fading to a pale
    // haze at the horizon
    top[0] = 92;  top[1] = 150; top[2] = 214;
    horizon[0] = 222; horizon[1] = 231; horizon[2] = 240;
}

void frameModel(const Model& m, RenderView& view) {
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    size_t n = 0;
    // Light cards on the ground (Most Wanted's falloff quads reach 3 m ahead
    // of the car) and a scene's backdrop cylinder or sky are not what anyone
    // wants framed; they only count when nothing else is there.
    auto extra = [](const Mesh& mesh) {
        const std::string& a = mesh.name;
        const std::string& b = mesh.material;
        for (const char* w : { "falloff", "Falloff", "additive", "_glow", "background", "skydome",
                               "lightfx" })
            if (a.find(w) != std::string::npos || b.find(w) != std::string::npos) return true;
        return false;
    };
    // the most detailed level alone decides the framing (LOD00 usually; a
    // file may start further down)
    std::string top;
    for (const Mesh& mesh : m.meshes)
        if (mesh.lod.compare(0, 3, "LOD") == 0 && (top.empty() || mesh.lod < top)) top = mesh.lod;
    if (top.empty()) top = "LOD00";
    for (int pass = 0; pass < 2 && !n; ++pass)
    for (const Mesh& mesh : m.meshes) {
        if (!mesh.lod.empty() && mesh.lod != top) continue;
        if (pass == 0 && extra(mesh)) continue;
        for (size_t i = 0; i + 2 < mesh.positions.size(); i += 3) {
            for (int k = 0; k < 3; ++k) {
                float v = mesh.positions[i + k];
                lo[k] = std::min(lo[k], v);
                hi[k] = std::max(hi[k], v);
            }
            ++n;
        }
    }
    if (!n) { view.distance = 5; return; }
    for (int k = 0; k < 3; ++k) view.centre[k] = (lo[k] + hi[k]) * 0.5f;
    float span = 0, sq = 0;
    for (int k = 0; k < 3; ++k) {
        float d = hi[k] - lo[k];
        span = std::max(span, d);
        sq += d * d;
    }
    // Fit the bounding sphere into the vertical field of view. Doing it by the
    // sphere rather than the longest side means the model stays inside the
    // frame at every orbit angle and in a pane of any shape - a car turned
    // side-on is much wider on screen than it is nose-on.
    view.radius = 0.5f * std::sqrt(sq) + 1e-4f;
    view.distance = view.radius / std::tan(0.5f * view.fovY) * 1.05f;
    view.modelSize = span;
}

void renderModel(const Model& model, const RenderView& view,
                 const std::vector<const Image*>* textures,
                 int width, int height, Image& out) {
    if (width < 1 || height < 1) return;
    out.width = width;
    out.height = height;
    out.channels = 3;
    out.pixels.assign((size_t)width * height * 3, 0);

    // background: Frosty's sky (blue overhead, pale at the horizon), or the
    // old plain dark gradient when the sky is off
    float skyTop[3], skyLow[3];
    skyColours(skyTop, skyLow);
    for (int y = 0; y < height; ++y) {
        float t = (float)y / (float)std::max(1, height - 1);
        uint8_t r, g, b;
        if (view.sky) {
            float k = std::min(1.0f, t * 1.6f);
            r = (uint8_t)(skyTop[0] + (skyLow[0] - skyTop[0]) * k);
            g = (uint8_t)(skyTop[1] + (skyLow[1] - skyTop[1]) * k);
            b = (uint8_t)(skyTop[2] + (skyLow[2] - skyTop[2]) * k);
        } else {
            r = (uint8_t)(view.background[0] * (1.0f - 0.45f * t));
            g = (uint8_t)(view.background[1] * (1.0f - 0.45f * t));
            b = (uint8_t)(view.background[2] * (1.0f - 0.45f * t));
        }
        for (int x = 0; x < width; ++x) {
            uint8_t* p = &out.pixels[((size_t)y * width + x) * 3];
            p[0] = r; p[1] = g; p[2] = b;
        }
    }

    std::vector<float> depth((size_t)width * height, 1e30f);

    // ---- camera ----
    float cy = std::cos(view.yaw), sy = std::sin(view.yaw);
    float cp = std::cos(view.pitch), sp = std::sin(view.pitch);
    // rows of the view rotation
    // right = forward x up. Up to 0.7.6 this had the opposite sign, which
    // made a left-handed camera: every model was drawn mirrored left to right
    // - hence number plates reading backwards in the viewer (exports were
    // never affected). Checked with three boxes whose sides are known.
    Vec3 right{ -cy, 0, sy };
    Vec3 upv  { sy * sp, cp, cy * sp };
    Vec3 fwd  { sy * cp, -sp, cy * cp };

    float focal = 0.5f * (float)height / std::tan(0.5f * view.fovY);
    float halfW = 0.5f * (float)width, halfH = 0.5f * (float)height;

    Vec3 light = norm(Vec3{0.4f, 0.8f, 0.45f});

    auto project = [&](float x, float y, float z, Vtx& o) -> bool {
        // model translation, then orbit about the centre
        x += view.modelPos[0] - view.centre[0];
        y += view.modelPos[1] - view.centre[1];
        z += view.modelPos[2] - view.centre[2];
        Vec3 p{x, y, z};
        float vx = dot(p, right) - view.pan[0];
        float vy = dot(p, upv) - view.pan[1];
        float vz = dot(p, fwd) + view.distance;
        if (vz < 0.001f) return false;
        o.z = vz;
        o.invW = 1.0f / vz;
        o.sx = halfW + vx * focal * o.invW;
        o.sy = halfH - vy * focal * o.invW;
        return true;
    };

    // ---- the shadow on the floor: the model flattened onto the floor along
    // a light from almost overhead, blurred for a soft edge, plus a faint
    // contact shade under the whole footprint. Drawn before the model, which
    // then covers whatever of it is behind the car.
    if (wantsShadow(view)) {
        float y0 = floorHeight(model, view) - view.modelPos[1];
        std::vector<float> hard((size_t)width * height, 0.0f), blob((size_t)width * height, 0.0f);
        auto fill = [&](const Vtx& A, const Vtx& B, const Vtx& C, float va, float vb, float vc,
                        std::vector<float>& buf) {
            float area = (B.sx - A.sx) * (C.sy - A.sy) - (C.sx - A.sx) * (B.sy - A.sy);
            if (std::fabs(area) < 1e-6f) return;
            int minX = std::max(0, (int)std::floor(std::min({A.sx, B.sx, C.sx})));
            int maxX = std::min(width - 1, (int)std::ceil(std::max({A.sx, B.sx, C.sx})));
            int minY = std::max(0, (int)std::floor(std::min({A.sy, B.sy, C.sy})));
            int maxY = std::min(height - 1, (int)std::ceil(std::max({A.sy, B.sy, C.sy})));
            float inv = 1.0f / area;
            for (int y = minY; y <= maxY; ++y)
                for (int x = minX; x <= maxX; ++x) {
                    float px = x + 0.5f, py = y + 0.5f;
                    float w0 = ((B.sx - A.sx) * (py - A.sy) - (px - A.sx) * (B.sy - A.sy)) * inv;
                    float w1 = ((px - A.sx) * (C.sy - A.sy) - (C.sx - A.sx) * (py - A.sy)) * inv;
                    float w2 = 1.0f - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    float v = w2 * va + w1 * vb + w0 * vc;
                    float& d = buf[(size_t)y * width + x];
                    d = std::max(d, v);
                }
        };
        float L[3];
        shadowLight(0, L);
        std::vector<uint8_t> vis = visibleMeshes(model, view);
        for (size_t mi = 0; mi < model.meshes.size(); ++mi) {
            if (!vis[mi]) continue;
            const Mesh& mesh = model.meshes[mi];
            size_t nv = mesh.positions.size() / 3;
            std::vector<Vtx> pv(nv);
            std::vector<uint8_t> ok(nv, 0);
            for (size_t i = 0; i < nv; ++i) {
                const float* q = &mesh.positions[i * 3];
                float t = (q[1] - y0) / L[1];
                ok[i] = project(q[0] - L[0] * t, y0, q[2] - L[2] * t, pv[i]);
            }
            for (size_t f = 0; f + 2 < mesh.indices.size(); f += 3) {
                uint32_t a = mesh.indices[f], b = mesh.indices[f + 1], c = mesh.indices[f + 2];
                if (a >= nv || b >= nv || c >= nv || !ok[a] || !ok[b] || !ok[c]) continue;
                fill(pv[a], pv[b], pv[c], 1, 1, 1, hard);
            }
        }
        float lo[3], hi[3];
        if (modelFootprint(model, view, lo, hi)) {
            // an ellipse a little larger than the footprint, dark in the middle
            float mx = (lo[0] + hi[0]) * 0.5f, mz = (lo[2] + hi[2]) * 0.5f;
            float rx = (hi[0] - lo[0]) * 0.62f, rz = (hi[2] - lo[2]) * 0.58f;
            Vtx c;
            if (project(mx, y0, mz, c)) {
                const int N = 48;
                Vtx prev{};
                bool havePrev = false;
                for (int k = 0; k <= N; ++k) {
                    float a = 6.2831853f * k / N;
                    Vtx e;
                    bool okE = project(mx + rx * std::cos(a), y0, mz + rz * std::sin(a), e);
                    if (okE && havePrev) fill(c, prev, e, 1, 0, 0, blob);
                    prev = e;
                    havePrev = okE;
                }
            }
        }
        // soften the hard shadow: two box passes, radius in pixels from the
        // model's size on screen
        int r = std::max(1, (int)(focal * view.radius * 0.035f / std::max(0.01f, view.distance)));
        r = std::min(r, 24);
        std::vector<float> tmp(hard.size());
        for (int pass = 0; pass < 2; ++pass) {
            for (int y = 0; y < height; ++y) {
                const float* row = &hard[(size_t)y * width];
                float* o = &tmp[(size_t)y * width];
                float acc = 0;
                for (int x = -r; x <= r; ++x) acc += row[std::min(width - 1, std::max(0, x))];
                for (int x = 0; x < width; ++x) {
                    o[x] = acc / (2 * r + 1);
                    acc += row[std::min(width - 1, x + r + 1)] - row[std::max(0, x - r)];
                }
            }
            for (int x = 0; x < width; ++x) {
                float acc = 0;
                for (int y = -r; y <= r; ++y) acc += tmp[(size_t)std::min(height - 1, std::max(0, y)) * width + x];
                for (int y = 0; y < height; ++y) {
                    hard[(size_t)y * width + x] = acc / (2 * r + 1);
                    acc += tmp[(size_t)std::min(height - 1, y + r + 1) * width + x] -
                           tmp[(size_t)std::max(0, y - r) * width + x];
                }
            }
        }
        for (size_t i = 0; i < hard.size(); ++i) {
            float d = std::min(0.62f, 0.42f * hard[i] + 0.22f * blob[i]);
            if (d <= 0.002f) continue;
            uint8_t* p = &out.pixels[i * 3];
            for (int c = 0; c < 3; ++c) p[c] = (uint8_t)(p[c] * (1.0f - d));
        }
    }

    std::vector<uint8_t> inKit = kitMask(model, view.kit);
    // Opaque and cut-out parts first, then the see-through ones from the
    // farthest to the nearest, each blended over what is behind it.
    std::vector<AlphaStats> texAlpha;
    if (view.textured && textures) {
        texAlpha.resize(textures->size());
        std::map<const Image*, AlphaStats> seen;
        for (size_t i = 0; i < textures->size(); ++i) {
            const Image* t = (*textures)[i];
            if (!t) continue;
            auto it = seen.find(t);
            if (it == seen.end()) it = seen.emplace(t, alphaStats(*t)).first;
            texAlpha[i] = it->second;
        }
    }
    std::vector<uint8_t> modeOf(model.meshes.size(), ALPHA_OPAQUE);
    std::vector<float> alphaOf(model.meshes.size(), 1.0f);
    std::vector<size_t> order, blended;
    for (size_t i = 0; i < model.meshes.size(); ++i) {
        modeOf[i] = (uint8_t)meshAlphaMode(model.meshes[i],
                                           i < texAlpha.size() ? &texAlpha[i] : nullptr, &alphaOf[i]);
        if (!view.textured && modeOf[i] == ALPHA_CUTOUT) modeOf[i] = ALPHA_OPAQUE;
        (modeOf[i] == ALPHA_BLEND ? blended : order).push_back(i);
    }
    {
        std::vector<std::pair<float, size_t>> far;
        for (size_t i : blended) {
            const Mesh& me = model.meshes[i];
            float c[3] = { 0, 0, 0 };
            size_t nv = me.positions.size() / 3;
            for (size_t k = 0; k < nv; ++k) for (int a = 0; a < 3; ++a) c[a] += me.positions[k * 3 + a];
            if (nv) for (float& v : c) v /= (float)nv;
            Vtx o;
            float d = project(c[0], c[1], c[2], o) ? o.z : 0;
            far.push_back({ -d, i });
        }
        std::sort(far.begin(), far.end());
        for (auto& f : far) order.push_back(f.second);
    }
    for (size_t thisMesh : order) {
        const Mesh& mesh = model.meshes[thisMesh];
        const int mode = modeOf[thisMesh];
        const float meshAlpha = alphaOf[thisMesh];
        if (!view.lodFilter.empty() && !mesh.lod.empty() && mesh.lod != view.lodFilter) continue;
        if (!inKit[thisMesh]) continue;
        if (mesh.positions.empty() || mesh.indices.empty()) continue;

        // The additive glow cards are flat quads that only make sense when
        // they are added to what is behind them. Drawn solid they sit over
        // the lights as grey slabs, so the viewer leaves them out.
        if (view.hideGlow) {
            const std::string& mat = mesh.material;
            if (mat.find("additive") != std::string::npos ||
                mat.find("alphaadd") != std::string::npos ||
                mat.find("falloff") != std::string::npos ||
                mat.find("lightfx") != std::string::npos ||
                mesh.name.find("_glow") != std::string::npos ||
                mesh.name.find("additive") != std::string::npos)
                continue;
        }

        const Image* tex = nullptr;
        if (view.textured && textures && thisMesh < textures->size())
            tex = (*textures)[thisMesh];
        if (tex && !tex->ok()) tex = nullptr;
        bool hasUv = mesh.uvs.size() * 3 == mesh.positions.size() * 2;
        // A mesh with no texture is drawn in its material's colour (bronze
        // chrome, gloss black) when the view is textured; plain grey otherwise.
        int baseR = 190, baseG = 194, baseB = 200;
        if (view.textured && !tex &&
            (mesh.color[0] != 1 || mesh.color[1] != 1 || mesh.color[2] != 1)) {
            auto to8 = [](float c) {
                return (int)std::min(255.0f, std::max(0.0f, c) * 235.0f + 20.0f);
            };
            baseR = to8(mesh.color[0]); baseG = to8(mesh.color[1]); baseB = to8(mesh.color[2]);
        }

        size_t nv = mesh.positions.size() / 3;
        bool hasNormals = mesh.normals.size() == mesh.positions.size();
        bool useCol = view.vertexColors && mesh.colors.size() == nv * 4;

        std::vector<Vtx> verts(nv);
        std::vector<uint8_t> visible(nv, 0);
        for (size_t i = 0; i < nv; ++i) {
            Vtx& v = verts[i];
            if (!project(mesh.positions[i*3], mesh.positions[i*3+1],
                         mesh.positions[i*3+2], v)) continue;
            float sh = 0.55f;
            if (hasNormals) {
                Vec3 nvec{mesh.normals[i*3], mesh.normals[i*3+1], mesh.normals[i*3+2]};
                // A key light fixed above the car, plus a fill that comes
                // from the camera: with the key alone every face turned
                // away from it went to a quarter brightness, so a car seen
                // from behind or below was nearly black. The fill takes the
                // normal either way round, as some parts are authored with
                // their normals flipped.
                float key = std::max(0.0f, dot(nvec, light));
                float fill = std::fabs(dot(nvec, fwd));
                float sky = 0.5f + 0.5f * nvec.y;
                sh = std::min(1.1f, 0.22f + 0.5f * key + 0.35f * fill + 0.12f * sky);
            }
            if (useCol) {
                // the colour channel on these cars is almost always grey,
                // baked occlusion, so its brightness is what gets shown
                const float* c = &mesh.colors[i * 4];
                sh *= 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
            }
            v.shade = sh;
            if (hasUv) { v.u = mesh.uvs[i*2]; v.v = mesh.uvs[i*2+1]; }
            else { v.u = v.v = 0; }
            visible[i] = 1;
        }

        for (size_t f = 0; f + 2 < mesh.indices.size(); f += 3) {
            uint32_t ia = mesh.indices[f], ib = mesh.indices[f+1], ic = mesh.indices[f+2];
            if (ia >= nv || ib >= nv || ic >= nv) continue;
            if (!visible[ia] || !visible[ib] || !visible[ic]) continue;
            const Vtx& A = verts[ia];
            const Vtx& B = verts[ib];
            const Vtx& C = verts[ic];

            float area = (B.sx - A.sx) * (C.sy - A.sy) - (C.sx - A.sx) * (B.sy - A.sy);
            if (area == 0) continue;
            if (!view.twoSided && area < 0) continue;   // back face

            int minX = (int)std::floor(std::min({A.sx, B.sx, C.sx}));
            int maxX = (int)std::ceil (std::max({A.sx, B.sx, C.sx}));
            int minY = (int)std::floor(std::min({A.sy, B.sy, C.sy}));
            int maxY = (int)std::ceil (std::max({A.sy, B.sy, C.sy}));
            minX = std::max(minX, 0); minY = std::max(minY, 0);
            maxX = std::min(maxX, width - 1); maxY = std::min(maxY, height - 1);
            if (minX > maxX || minY > maxY) continue;

            float inv = 1.0f / area;
            for (int y = minY; y <= maxY; ++y) {
                for (int x = minX; x <= maxX; ++x) {
                    float px = x + 0.5f, py = y + 0.5f;
                    float w0 = ((B.sx - A.sx) * (py - A.sy) - (px - A.sx) * (B.sy - A.sy)) * inv;
                    float w1 = ((px - A.sx) * (C.sy - A.sy) - (C.sx - A.sx) * (py - A.sy)) * inv;
                    float w2 = 1.0f - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    // barycentric order: w2 -> A, w1 -> B, w0 -> C
                    float iw = w2 * A.invW + w1 * B.invW + w0 * C.invW;
                    if (iw <= 0) continue;
                    float z = 1.0f / iw;
                    size_t di = (size_t)y * width + x;
                    if (z >= depth[di]) continue;

                    float sh = (w2 * A.shade * A.invW + w1 * B.shade * B.invW +
                                w0 * C.shade * C.invW) * z;
                    int r = baseR, g = baseG, b = baseB;
                    float alpha = meshAlpha;
                    if (tex) {
                        float u = (w2 * A.u * A.invW + w1 * B.u * B.invW + w0 * C.u * C.invW) * z;
                        float v = (w2 * A.v * A.invW + w1 * B.v * B.invW + w0 * C.v * C.invW) * z;
                        u -= std::floor(u);
                        v -= std::floor(v);
                        int tx = (int)(u * tex->width);
                        int ty = (int)((1.0f - v) * tex->height);
                        r = sampleClamp(*tex, tx, ty, 0);
                        g = sampleClamp(*tex, tx, ty, 1);
                        b = sampleClamp(*tex, tx, ty, 2);
                        if (mode != ALPHA_OPAQUE && tex->channels == 4)
                            alpha *= sampleClamp(*tex, tx, ty, 3) / 255.0f;
                    }
                    if (mode == ALPHA_CUTOUT && alpha < 0.5f) continue;
                    uint8_t* p = &out.pixels[di * 3];
                    if (mode == ALPHA_BLEND) {
                        if (alpha <= 0.004f) continue;
                        p[0] = (uint8_t)(p[0] + (std::min(255.0f, r * sh) - p[0]) * alpha);
                        p[1] = (uint8_t)(p[1] + (std::min(255.0f, g * sh) - p[1]) * alpha);
                        p[2] = (uint8_t)(p[2] + (std::min(255.0f, b * sh) - p[2]) * alpha);
                        continue;                 // see-through: depth not written
                    }
                    depth[di] = z;
                    p[0] = (uint8_t)std::min(255.0f, r * sh);
                    p[1] = (uint8_t)std::min(255.0f, g * sh);
                    p[2] = (uint8_t)std::min(255.0f, b * sh);
                }
            }
        }
    }

    // ---- the floor: a grid at the model's lowest point, hidden where the
    // model is in front of it ----
    if (view.floor) {
        float y0 = floorHeight(model, view) - view.modelPos[1];
        float step = gridStep(view.radius);
        int lines = 12;
        float cx = std::round(view.centre[0] / step) * step;
        float cz = std::round(view.centre[2] / step) * step;
        float ext = step * lines;
        auto plotLine = [&](float ax, float az, float bx, float bz, bool major) {
            // enough samples for an unbroken line on screen
            int nSeg = 400;
            Vtx va, vb;
            if (project(ax, y0, az, va) && project(bx, y0, bz, vb)) {
                float len = std::fabs(va.sx - vb.sx) + std::fabs(va.sy - vb.sy);
                nSeg = (int)std::min(6000.0f, std::max(50.0f, len * 1.2f));
            } else {
                nSeg = 3000;
            }
            for (int k = 0; k <= nSeg; ++k) {
                float t = (float)k / nSeg;
                Vtx v;
                if (!project(ax + (bx - ax) * t, y0, az + (bz - az) * t, v)) continue;
                int px = (int)v.sx, py = (int)v.sy;
                if (px < 0 || py < 0 || px >= width || py >= height) continue;
                size_t di = (size_t)py * width + px;
                if (v.z >= depth[di] * 1.0005f) continue;
                uint8_t* p = &out.pixels[di * 3];
                int shade = major ? 110 : 150;
                for (int c = 0; c < 3; ++c) p[c] = (uint8_t)((p[c] + shade) / 2);
            }
        };
        for (int i = -lines; i <= lines; ++i) {
            float o = i * step;
            bool major = (i % 5) == 0;
            plotLine(cx + o, cz - ext, cx + o, cz + ext, major);
            plotLine(cx - ext, cz + o, cx + ext, cz + o, major);
        }
    }
}

} // namespace nfsnl
