// main_cli.cpp - command line front end (works on Windows, Linux and macOS)
#include <filesystem>
#include "nfsnl.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#include <dirent.h>
#include <array>
#include <map>
#include <functional>
#include <map>

// Real Racing 3 textures for a model on disk: the car's folder, its livery/
// subfolder and the shared vehicles/common next to it. Paths come back
// relative to `root` (the model's folder), the way the export names them.
static int cliRr3Textures(nfsnl::Model& m, const std::string& modelPath, std::string root,
                          std::string* report) {
    using namespace nfsnl;
    if (!root.empty() && root.back() != '/' && root.back() != '\\') root += "/";
    static const char* const kExt[] = { ".etc.dds", ".etc.dds.z", ".pvr", ".pvr.z", ".ptc.pvr.z",
                                        ".dxt.dds", ".dxt.dds.z", ".dds", ".dds.z", ".png" };
    auto exists = [](const std::string& p) {
        FILE* f = fopen(p.c_str(), "rb");
        if (f) fclose(f);
        return f != nullptr;
    };
    auto listDir = [](const std::string& dir) {
        std::vector<std::string> out;
        DIR* d = opendir(dir.c_str());
        if (!d) return out;
        while (dirent* e = readdir(d)) out.push_back(e->d_name);
        closedir(d);
        std::sort(out.begin(), out.end());
        return out;
    };
    auto lower = [](std::string x) {
        for (char& c : x) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return x;
    };
    auto isTex = [&](const std::string& leaf) {
        std::string l = lower(leaf);
        if (l.size() > 2 && l.compare(l.size() - 2, 2, ".z") == 0) l.erase(l.size() - 2);
        std::string e = extensionOf(l);
        return e == "dds" || e == "pvr" || e == "png" || e == "ktx";
    };
    std::vector<std::pair<std::string, std::string>> common;
    for (const std::string& leaf : listDir(root + "../common"))
        if (isTex(leaf)) common.push_back({ lower(leaf.substr(0, leaf.find('.'))), "../common/" + leaf });
    // every texture under the model's folder, by stem - a track keeps its
    // textures in resources/<kind>/<object>/ below it
    std::map<std::string, std::string> deep;
    std::multimap<std::string, std::string> byFolder;
    std::function<void(const std::string&, int)> walk = [&](const std::string& rel, int depth) {
        if (depth > 6 || deep.size() > 200000) return;
        for (const std::string& leaf : listDir(root + rel)) {
            if (leaf == "." || leaf == "..") continue;
            std::string r = rel + leaf;
            DIR* sub = opendir((root + r).c_str());
            if (sub) { closedir(sub); walk(r + "/", depth + 1); continue; }
            if (!isTex(leaf)) continue;
            std::string st = lower(leaf.substr(0, leaf.find('.')));
            byFolder.emplace(rr3FolderKey(r), r);
            auto it = deep.find(st);
            // the ETC build first, like the viewer
            if (it == deep.end() || (leaf.find(".etc.") != std::string::npos &&
                                     it->second.find(".etc.") == std::string::npos))
                deep[st] = r;
        }
    };
    walk("", 0);
    auto find = [&](const std::string& stem) -> std::string {
        auto d = deep.find(stem);
        if (d != deep.end()) return d->second;
        bool prefix = !stem.empty() && stem.back() == '*';
        std::string want = prefix ? stem.substr(0, stem.size() - 1) : stem;
        for (const char* sub : { "", "livery/", "../common/" }) {
            if (!prefix) {
                for (const char* ext : kExt)
                    if (exists(root + sub + want + ext)) return std::string(sub) + want + ext;
                continue;
            }
            std::string pick;
            for (const std::string& leaf : listDir(root + sub)) {
                std::string st = lower(leaf.substr(0, leaf.find('.')));
                if (isTex(leaf) && st.compare(0, want.size(), want) == 0 &&
                    st.find("shadow") == std::string::npos && (pick.empty() || lower(leaf) < lower(pick)))
                    pick = leaf;
            }
            if (!pick.empty()) return std::string(sub) + pick;
        }
        return prefix ? std::string() : rr3FolderTexture(want, byFolder);
    };
    // the car's .liveries list, when it is beside the model or one folder up
    NctLiveries liv;
    bool haveLiv = false;
    std::string car = lower(stripExtension(baseName(modelPath)));
    if (car.size() > 2 && car[car.size() - 2] == '_') car.erase(car.size() - 2);
    if (car.size() > 4 && car.compare(car.size() - 4, 4, "_int") == 0) car.erase(car.size() - 4);
    for (const char* sub : { "", "../", "livery/" })
        for (const std::string& leaf : listDir(root + sub)) {
            if (haveLiv || lower(leaf).compare(0, car.size() + 9, car + ".liveries") != 0) continue;
            Bytes raw, plain;
            if (readFile(root + sub + leaf, raw) && nctTransform(raw.data(), raw.size(), plain, nullptr)) {
                liv = nctReadLiveries(plain.data(), plain.size());
                haveLiv = liv.ok;
            }
        }
    return rr3AssignTextures(m, modelPath, find, common, haveLiv ? &liv : nullptr, report);
}

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#endif

using namespace nfsnl;

// A .cab inside a folder named after a .pack beside it is one of that pack's
// external cabinets: it is read through the pack, not listed on its own.
static bool packOwnsFolder(const std::string& dir) {
    FILE* f = fopen((dir + ".pack").c_str(), "rb");
    if (f) fclose(f);
    return f != nullptr;
}

static void scanArchives(const std::string& dir, std::vector<std::string>& out) {
    bool owned = packOwnsFolder(dir);
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        std::string full = dir + "\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) scanArchives(full, out);
        else {
            std::string e = extensionOf(full);
            if (e == "pack" || (e == "cab" && !owned)) out.push_back(full);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* de;
    while ((de = readdir(d))) {
        std::string name = de->d_name;
        if (name == "." || name == "..") continue;
        std::string full = dir + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st)) continue;
        if (S_ISDIR(st.st_mode)) scanArchives(full, out);
        else {
            std::string e = extensionOf(full);
            if (e == "pack" || (e == "cab" && !owned)) out.push_back(full);
        }
    }
    closedir(d);
#endif
}

// every file under a folder, for the games that keep theirs loose (or in
// one .obb / .apk / .ipa / FMOBB file)
static void scanLoose(const std::string& dir, const std::string& rel,
                      std::vector<std::pair<std::string, std::string>>& out) {
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        std::string full = dir + "\\" + name, r = rel.empty() ? name : rel + "/" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) scanLoose(full, r, out);
        else out.push_back({ full, r });
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* de;
    while ((de = readdir(d))) {
        std::string name = de->d_name;
        if (name == "." || name == "..") continue;
        std::string full = dir + "/" + name, r = rel.empty() ? name : rel + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st)) continue;
        if (S_ISDIR(st.st_mode)) scanLoose(full, r, out);
        else out.push_back({ full, r });
    }
    closedir(d);
#endif
}

static bool fileExists(const std::string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    if (f) fclose(f);
    return f != nullptr;
}

static void usage() {
    printf(
"Monkey Tool (command line)\n"
"\n"
"  nfsnl list    <gameFolder>\n"
"  nfsnl extract <gameFolder> <outFolder>              raw .sba/.sb3d/.wem\n"
"  nfsnl convert <gameFolder> <outFolder> [opts]       converted assets\n"
"  nfsnl one     <assetFile>  <outFile>   --format F   convert a single file\n"
"                (a No Limits car also takes --wheel wheel_<car>.sb3d)\n"
"  nfsnl render  <modelFile>  <outPng>    [opts]       viewer frame to PNG\n"
"  nfsnl browse  <gameFolder> [filter]                 index only, no unpacking\n"
"  nfsnl where   <gameFolder> <name>                   which .pack an asset is in\n"
"  nfsnl import  <texture> <picture> <outFile>         put a PNG/JPG into a texture\n"
"                (.sba, .pvr, .dds, .z - same size, codec and mip levels)\n"
"  nfsnl import  <car.m3g> <model.obj|fbx> <out.m3g>   Real Racing 3: meshes by name\n"
"  nfsnl newcar  <template.m3g> <model.obj|fbx> <out.m3g>  Real Racing 3: a whole new car\n"
"  nfsnl data    to <file> <out.txt>                  a data file as editable text\n"
"  nfsnl data    from <file> <in.txt> <outFile>       and the edited text back\n"
"  nfsnl sb      decode <file.sb> <out.sbin>           No Limits .sb -> SBIN\n"
"  nfsnl sb      encode <in.sbin> <file.sb>            and back (name decides the key)\n"
"  nfsnl text    <file> <out.txt>                      what the tool knows about a file\n"
"  nfsnl hash    <file>                                FNV-1 of a file, as SBIN uses\n"
"  nfsnl rims    <colours.sb>                          No Limits rim colours\n"
"  nfsnl paint   <setup.sb> <colours.sb>               No Limits: a car setup's body/rim/caliper colours\n"
"  nfsnl pack    <texturepack.sba> [outFolder]         No Limits UI texture pack: every picture as PNG\n"
"  nfsnl lts     <region.scene.sb>                     No Limits track: its limited-time layers\n"
"  nfsnl rimpaint <texture_wheel_x.sba> <colours.sb|#rrggbb> <n> <out.png> [--wheel wheel_x.sb3d]\n"
"                                                      a wheel texture in rim colour n\n"
"\n"
"Options for render:\n"
"  --size W H            image size            (default 960 540)\n"
"  --yaw D --pitch D     camera angles, degrees\n"
"  --pos X Y Z           move the model\n"
"  --lod LOD00           draw one LOD only\n"
"  --kit stock|all|y     No Limits body kit: stock (default), all, or a letter\n"
"  --tex <folder>        textures root, for textured shading\n"
"  --setup <setup.sb>    No Limits: paint the car as this car setup does\n"
"  --colours <colours.sb> (with --setup) the game's data/colours/colours.sb\n"
"  --vcolor              multiply in the vertex colours (baked AO)\n"
"\n"
"Options for convert:\n"
"  --tex  png|jpg|bmp|tga|dds|raw     texture output  (default png)\n"
"  --mdl  fbx|obj                     model output    (default fbx)\n"
"  --only textures|models|sounds      limit what is written\n"
"\n");
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 1; }
    std::string cmd = argv[1];

    if (cmd == "import") {
        if (argc < 5) { usage(); return 1; }
        std::string tex = argv[2], pic = argv[3], outPath = argv[4];
        std::string pext = extensionOf(pic);
        if (pext == "obj" || pext == "fbx") {
            // a model into a Real Racing 3 .m3g, by mesh name
            Bytes m3g, src;
            if (!readFile(tex, m3g) || !readFile(pic, src)) { printf("cannot read the inputs\n"); return 1; }
            Model imported = readImportModel(src.data(), src.size(), pic);
            if (!imported.valid) { printf("%s: %s\n", pic.c_str(), imported.warnings.empty() ? "no meshes" : imported.warnings[0].c_str()); return 1; }
            Model raw = loadModel(m3g.data(), m3g.size());
            Model placed = raw;
            Bytes pts;
            if (readFile(rr3PointsNameFor(tex), pts)) {
                // placed the way "one" exports it: the cockpit's points too
                std::vector<Hardpoint> hp = rr3ReadPoints(pts.data(), pts.size());
                Bytes inner;
                std::string innerPath = rr3InteriorPointsNameFor(tex);
                if (!hp.empty() && !innerPath.empty() && readFile(innerPath, inner))
                    rr3MergeCockpitPoints(hp, rr3ReadPoints(inner.data(), inner.size()));
                if (!hp.empty()) rr3PlaceParts(placed, hp);
            }
            Bytes result;
            std::string report;
            bool ok = rr3ReplaceMeshes(m3g, imported, meshPlacements(raw, placed), result, report);
            printf("%s\n", report.c_str());
            if (!ok) return 1;
            if (!writeFile(outPath, result)) { printf("cannot write %s\n", outPath.c_str()); return 1; }
            printf("wrote %s (%zu bytes)\n", outPath.c_str(), result.size());
            return 0;
        }
        Bytes original, picture;
        if (!readFile(tex, original)) { printf("cannot read %s\n", tex.c_str()); return 1; }
        if (!readFile(pic, picture)) { printf("cannot read %s\n", pic.c_str()); return 1; }
        Image img;
        if (!decodeImageFile(picture.data(), picture.size(), img)) {
            printf("%s is not a picture this tool can read\n", pic.c_str());
            return 1;
        }
        // a Real Racing 3 .z comes off first and goes back on at the end
        bool wrapped = extensionOf(tex) == "z";
        Bytes plain = original;
        std::string inner = tex;
        if (wrapped && !rr3Unwrap(baseName(tex), original.data(), original.size(), plain, &inner, nullptr)) {
            printf("the .z wrapper did not unpack\n");
            return 1;
        }
        Bytes result;
        std::string report;
        if (!importPicture(inner, plain, img, result, report)) {
            printf("import failed: %s\n", report.c_str());
            return 1;
        }
        if (wrapped) result = rr3WrapZ(result);
        if (!writeFile(outPath, result)) { printf("cannot write %s\n", outPath.c_str()); return 1; }
        printf("%s\nwrote %s (%zu bytes)\n", report.c_str(), outPath.c_str(), result.size());
        return 0;
    }
    if (cmd == "newcar") {
        // a whole new Real Racing 3 car .m3g from an OBJ / FBX
        if (argc < 5) { usage(); return 1; }
        Bytes tmpl, src;
        if (!readFile(argv[2], tmpl) || !readFile(argv[3], src)) { printf("cannot read the inputs\n"); return 1; }
        Model imported = readImportModel(src.data(), src.size(), argv[3]);
        if (!imported.valid) { printf("%s: no meshes\n", argv[3]); return 1; }
        bool fix = false;
        for (int k = 5; k < argc; ++k) if (std::string(argv[k]) == "--fix") fix = true;
        std::vector<NameFix> fixes = rr3SuggestNames(tmpl, imported, true);
        for (const NameFix& f : fixes)
            printf("FOUND INCORRECT %s NAME: %s -> %s\n", f.material ? "MATERIAL" : "PART", f.from.c_str(), f.to.c_str());
        if (fix) rr3ApplyNameFixes(imported, fixes);
        else if (!fixes.empty()) printf("(add --fix to use the car's names)\n");
        Bytes result;
        std::string report;
        bool ok = rr3BuildCar(tmpl, imported, result, report);
        printf("%s\n", report.c_str());
        if (!ok || !writeFile(argv[4], result)) return 1;
        printf("wrote %s (%zu bytes)\n", argv[4], result.size());
        return 0;
    }
    if (cmd == "data") {
        // data to|from: a data file as editable text, and back
        if (argc < 5) { usage(); return 1; }
        std::string how = argv[2], file = argv[3];
        Bytes data;
        if (!readFile(file, data)) { printf("cannot read %s\n", file.c_str()); return 1; }
        if (how == "to") {
            std::string text, why;
            if (!dataEditableText(file, data.data(), data.size(), text, why)) { printf("%s\n", why.c_str()); return 1; }
            Bytes o(text.begin(), text.end());
            if (!writeFile(argv[4], o)) return 1;
            printf("wrote %s\n", argv[4]);
            return 0;
        }
        if (how == "from" && argc >= 6) {
            Bytes t;
            if (!readFile(argv[4], t)) { printf("cannot read %s\n", argv[4]); return 1; }
            Bytes o;
            std::string why;
            if (!dataFromEditedText(file, std::string(t.begin(), t.end()), data.data(), data.size(), o, why)) {
                printf("%s\n", why.c_str());
                return 1;
            }
            if (!writeFile(argv[5], o)) return 1;
            printf("wrote %s (%zu bytes)%s\n", argv[5], o.size(), o == data ? " - identical to the original" : "");
            return 0;
        }
        usage();
        return 1;
    }
    if (cmd == "sb") {
        if (argc < 5) { usage(); return 1; }
        std::string mode = argv[2], in = argv[3], outPath = argv[4];
        Bytes data;
        if (!readFile(in, data)) { printf("cannot read %s\n", in.c_str()); return 1; }
        Bytes result;
        if (mode == "decode") {
            std::string how;
            if (!nlSbDecode(in, data.data(), data.size(), result, &how)) {
                printf("did not decode: %s\n", how.c_str());
                return 1;
            }
            printf("%s\n", how.c_str());
        } else if (mode == "encode") {
            if (data.size() < 8 || memcmp(data.data(), "SBIN", 4)) { printf("%s is not SBIN\n", in.c_str()); return 1; }
            result = nlSbEncode(outPath, data);
        } else {
            usage();
            return 1;
        }
        if (!writeFile(outPath, result)) { printf("cannot write %s\n", outPath.c_str()); return 1; }
        printf("wrote %s (%zu bytes)\n", outPath.c_str(), result.size());
        return 0;
    }
    if (cmd == "text") {
        if (argc < 4) { usage(); return 1; }
        Bytes data;
        if (!readFile(argv[2], data)) { printf("cannot read %s\n", argv[2]); return 1; }
        std::string t = assetText(argv[2], data.data(), data.size());
        Bytes b(t.begin(), t.end());
        if (!writeFile(argv[3], b)) { printf("cannot write %s\n", argv[3]); return 1; }
        printf("wrote %s (%zu bytes)\n", argv[3], b.size());
        return 0;
    }
    if (cmd == "hash") {
        if (argc < 3) { usage(); return 1; }
        Bytes data;
        if (!readFile(argv[2], data)) { printf("cannot read %s\n", argv[2]); return 1; }
        printf("FNV-1 %08X  length %zu\n", sbinFnv1(data.data(), data.size()), data.size());
        return 0;
    }

    if (cmd == "lts" || cmd == "pack" || cmd == "paint") {
        if (argc < 3) { usage(); return 1; }
        Bytes raw, sb;
        if (!readFile(argv[2], raw)) { printf("cannot read %s\n", argv[2]); return 1; }
        if (cmd == "pack") {
            // every picture of a No Limits UI texture pack, by its own name
            std::vector<NamedImage> pics = texturePackPictures(raw.data(), raw.size());
            if (pics.empty()) { printf("%s is not a texture pack (or none of its pictures decodes)\n", argv[2]); return 1; }
            std::string dir = argc > 3 ? argv[3] : ".";
            int n = 0;
            for (const NamedImage& p : pics) {
                std::string rel = stripExtension(p.name) + ".png";
                for (char& c : rel) if (c == '\\' || c == ':') c = '_';
                if (writeFile(dir + "/" + rel, encodePng(p.image))) ++n;
            }
            printf("%d of %zu picture(s) written to %s\n", n, pics.size(), dir.c_str());
            return 0;
        }
        if (!nlSbDecode(baseName(argv[2]), raw.data(), raw.size(), sb)) sb = raw;
        if (cmd == "paint") {
            // a car setup's colours: nfsnl paint <setup.sb> <colours.sb>
            if (argc < 4) { usage(); return 1; }
            Bytes cr, cs;
            if (!readFile(argv[3], cr) || !nlSbDecode(baseName(argv[3]), cr.data(), cr.size(), cs)) { printf("cannot read %s\n", argv[3]); return 1; }
            NlCarPaint p = nlCarSetupPaint(sb.data(), sb.size(), nlPaintColours(cs.data(), cs.size()));
            static const char* kKind[] = { "body", "rim", "brake caliper", "window" };
            for (int k = 0; k < NL_PAINT_KINDS; ++k)
                if (p.have[k]) printf("%-14s %-32s #%02X%02X%02X\n", kKind[k], p.name[k].c_str(),
                                      (int)(p.rgb[k][0] * 255 + .5f), (int)(p.rgb[k][1] * 255 + .5f), (int)(p.rgb[k][2] * 255 + .5f));
            return 0;
        }
        std::vector<NlLtsGroup> groups = nlSceneLtsGroups(sb.data(), sb.size());
        printf("%zu limited-time group(s)\n", groups.size());
        for (const NlLtsGroup& g : groups) {
            printf("  %s (%s)\n", g.name.c_str(), nlLtsLabel(g).c_str());
            for (const std::string& f : g.layerFiles) printf("    layer scene: %s\n", f.c_str());
            for (const std::string& f : g.layerNames) printf("    layer: %s\n", f.c_str());
            for (size_t i = 0; i < g.models.size() && i < 8; ++i)
                printf("    model %s at %.1f %.1f %.1f\n", g.models[i].file.c_str(), g.models[i].matrix[12],
                       g.models[i].matrix[13], g.models[i].matrix[14]);
            if (g.models.size() > 8) printf("    ... %zu models\n", g.models.size());
        }
        return 0;
    }

    if (cmd == "rims" || cmd == "rimpaint") {
        auto colours = [](const std::string& f, std::vector<NlRimColour>& out) {
            Bytes raw, sb;
            if (!readFile(f, raw) || !nlSbDecode(baseName(f), raw.data(), raw.size(), sb)) return false;
            out = nlRimColours(sb.data(), sb.size());
            return !out.empty();
        };
        if (cmd == "rims") {
            if (argc < 3) { usage(); return 1; }
            std::vector<NlRimColour> list;
            if (!colours(argv[2], list)) { printf("no rim colours in %s\n", argv[2]); return 1; }
            for (size_t i = 0; i < list.size(); ++i)
                printf("%3zu  %-28s %-34s %.3f %.3f %.3f\n", i, list[i].group.c_str(), list[i].name.c_str(),
                       list[i].rgb[0], list[i].rgb[1], list[i].rgb[2]);
            return 0;
        }
        if (argc < 6) { usage(); return 1; }
        std::string tex = argv[2], src = argv[3], outPath = argv[5], wheelPath;
        for (int i = 6; i + 1 < argc; ++i) if (!strcmp(argv[i], "--wheel")) wheelPath = argv[i + 1];
        float rgb[3] = { 1, 1, 1 };
        if (src[0] == '#' && src.size() == 7) {
            unsigned v = (unsigned)strtoul(src.c_str() + 1, nullptr, 16);
            rgb[0] = ((v >> 16) & 255) / 255.0f; rgb[1] = ((v >> 8) & 255) / 255.0f; rgb[2] = (v & 255) / 255.0f;
        } else {
            std::vector<NlRimColour> list;
            if (!colours(src, list)) { printf("no rim colours in %s\n", src.c_str()); return 1; }
            size_t k = (size_t)atoi(argv[4]);
            if (k >= list.size()) { printf("colour %zu of %zu\n", k, list.size()); return 1; }
            memcpy(rgb, list[k].rgb, sizeof(rgb));
            printf("%s (%s)\n", list[k].name.c_str(), list[k].group.c_str());
        }
        Bytes raw;
        Image img;
        if (!readFile(tex, raw) || !decodeTextureFile(raw.data(), raw.size(), img)) { printf("cannot decode %s\n", tex.c_str()); return 1; }
        std::vector<uint8_t> mask;
        if (!wheelPath.empty()) {
            Bytes wb;
            Model wheel;
            if (readFile(wheelPath, wb)) wheel = loadModel(wb.data(), wb.size());
            if (nlRimTintMask(wheel, baseName(tex), img.width, img.height, mask)) printf("mask from %s\n", wheelPath.c_str());
            else { mask.clear(); printf("no tinted mesh of %s uses this texture - grey texels painted\n", wheelPath.c_str()); }
        }
        nlTintImage(img, mask, rgb);
        if (!writeFile(outPath, encodePng(img))) { printf("cannot write %s\n", outPath.c_str()); return 1; }
        printf("wrote %s\n", outPath.c_str());
        return 0;
    }

    if (cmd == "one") {
        if (argc < 4) { usage(); return 1; }
        std::string in = argv[2], out = argv[3], fmt, wheelPath;
        for (int i = 4; i < argc - 1; ++i) {
            if (!strcmp(argv[i], "--format")) fmt = argv[i + 1];
            if (!strcmp(argv[i], "--wheel")) wheelPath = argv[i + 1];
        }
        if (fmt.empty()) {
            auto f = formatsFor(in);
            fmt = f.empty() ? "raw" : f[0];
        }
        Bytes data;
        if (!readFile(in, data)) { printf("cannot read %s\n", in.c_str()); return 1; }
        // Real Racing 3 keeps almost everything zlib-wrapped in a .z, and a
        // few textures in a .z.bin on top of that. Unwrap first, then treat
        // the payload as whatever its remaining extension says it is.
        if (rr3IsContainer(in, data.data(), data.size())) {
            Bytes inner;
            std::string innerName;
            size_t frames = 1;
            if (rr3Unwrap(in, data.data(), data.size(), inner, &innerName, &frames)) {
                printf("unwrapped %s -> %s (%zu byte(s)%s)\n", baseName(in).c_str(),
                       baseName(innerName).c_str(), inner.size(),
                       frames > 1 ? ", first of several frames" : "");
                data.swap(inner);
                in = innerName;
                if (fmt.empty()) fmt = formatsFor(in).front();
            }
        }
        // a Real Racing 3 model has its hardpoints in a .points file beside it
        Bytes points;
        std::string pointsPath = rr3PointsNameFor(in);
        if (pointsPath != in && readFile(pointsPath, points))
            printf("using %s\n", pointsPath.c_str());
        // a No Limits car with its wheel model: wheel_<car>.sb3d beside it,
        // or named with --wheel
        if (extensionOf(in) == "sb3d" && (fmt == "fbx" || fmt == "obj") &&
            baseName(in).compare(0, 6, "wheel_") != 0) {
            if (wheelPath.empty()) {
                std::string dir = in.substr(0, in.find_last_of("/\\") + 1);
                std::string guess = dir + nlWheelNameFor(in);
                FILE* f = fopen(guess.c_str(), "rb");
                if (f) { fclose(f); wheelPath = guess; }
            }
            Bytes wb;
            if (!wheelPath.empty() && readFile(wheelPath, wb)) {
                Model car = loadModel(data.data(), data.size());
                Model wheel = loadModel(wb.data(), wb.size());
                if (car.valid && nlAttachWheels(car, wheel) > 0) {
                    Bytes result;
                    if (writeModel(car, fmt, result) && writeFile(out, result)) {
                        printf("using %s\nwrote %s (%zu bytes) with wheels\n",
                               wheelPath.c_str(), out.c_str(), result.size());
                        return 0;
                    }
                }
            }
        }
        // a Real Racing 2 car: textures paired by file name, parts placed
        // from its .points
        if (extensionOf(in) == "m3g" && (fmt == "fbx" || fmt == "obj") &&
            baseName(in).compare(0, 4, "car_") == 0) {
            Model car = loadModel(data.data(), data.size());
            if (car.valid) {
                if (!points.empty()) {
                    std::vector<Hardpoint> pts = rr3ReadPoints(points.data(), points.size());
                    if (!pts.empty()) rr3PlaceParts(car, pts);
                }
                rr2AssignTextures(car, in);
                Bytes result;
                if (writeModel(car, fmt, result) && writeFile(out, result)) {
                    printf("wrote %s (%zu bytes)\n", out.c_str(), result.size());
                    return 0;
                }
            }
        }
        // a Real Racing 3 car: the steering wheel's point from the cockpit's
        // .points, and textures from the files beside the model
        if (extensionOf(in) == "m3g" && (fmt == "fbx" || fmt == "obj") &&
            identifierVersionOf(data.data(), data.size()) == 1) {
            Model car = loadModel(data.data(), data.size());
            if (car.valid) {
                std::vector<Hardpoint> pts;
                if (!points.empty()) pts = rr3ReadPoints(points.data(), points.size());
                Bytes inner;
                std::string innerPath = rr3InteriorPointsNameFor(in);
                if (!pts.empty() && !innerPath.empty() && readFile(innerPath, inner)) {
                    rr3MergeCockpitPoints(pts, rr3ReadPoints(inner.data(), inner.size()));
                    printf("using %s for the cockpit points\n", innerPath.c_str());
                }
                if (!pts.empty()) rr3PlaceParts(car, pts);
                std::string dir = in.substr(0, in.find_last_of("/\\") + 1);
                std::string missing;
                int n = cliRr3Textures(car, in, dir.empty() ? "./" : dir, &missing);
                if (!missing.empty()) printf("  no texture found for: %s\n", missing.c_str());
                for (const std::string& w : car.warnings) printf("  %s\n", w.c_str());
                if (n) printf("  %d part(s) textured\n", n);
                Bytes result;
                if (writeModel(car, fmt, result) && writeFile(out, result)) {
                    printf("wrote %s (%zu bytes)\n", out.c_str(), result.size());
                    return 0;
                }
            }
        }
        Bytes result;
        std::string err;
        if (!convertAsset(in, data.data(), data.size(), fmt, result, &err, nullptr,
                          points.empty() ? nullptr : points.data(), points.size()) ||
            result.empty()) {
            printf("convert failed: %s\n", err.c_str());
            return 1;
        }
        if (!writeFile(out, result)) { printf("cannot write %s\n", out.c_str()); return 1; }
        printf("wrote %s (%zu bytes)%s%s\n", out.c_str(), result.size(),
               err.empty() ? "" : " - ", err.c_str());
        return 0;
    }

    // Index the archives and read a few assets straight out of them, which is
    // exactly what the window does - nothing is written to disk.
    // Which .pack an asset is in: the file to send when something is wrong
    if (cmd == "where") {
        if (argc < 4) { usage(); return 1; }
        std::vector<std::string> archives;
        scanArchives(argv[2], archives);
        Library lib;
        for (const auto& a : archives) indexArchive(a, lib);
        std::string needle = argv[3];
        for (char& c : needle) c = (char)tolower((unsigned char)c);
        int n = 0;
        for (const auto& e : lib.entries) {
            std::string p = e.path;
            for (char& c : p) c = (char)tolower((unsigned char)c);
            if (p.find(needle) == std::string::npos) continue;
            const Archive& a = lib.archives[e.archive];
            bool ext = !e.whole && e.cabinet < a.cabinets.size() &&
                       (a.cabinets[e.cabinet].flags & CAB_EXTERNAL);
            printf("%s\n    in %s", e.path.c_str(), a.path.c_str());
            if (!e.whole) printf("  (cabinet %u%s)", e.cabinet, ext ? ", separate .cab" : "");
            printf("\n");
            ++n;
        }
        printf("\n%d match(es)\n", n);
        return 0;
    }

    if (cmd == "browse") {
        if (argc < 3) { usage(); return 1; }
        std::string folder = argv[2];
        std::string filter;
        bool convertAll = false;
        std::string outRaw;       // --out: write each matching file as it is
        for (int i = 3; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--convert") convertAll = true;
            else if (a == "--out" && i + 1 < argc) outRaw = argv[++i];
            else filter = a;
        }
        std::vector<std::string> archives;
        scanArchives(folder, archives);
        printf("%zu archive(s)\n", archives.size());
        Library lib;
        for (const std::string& a : archives) indexArchive(a, lib);
        if (archives.empty()) {
            // loose files, or one container file given directly
            std::vector<std::pair<std::string, std::string>> files;
            scanLoose(folder, "", files);
            if (files.empty()) files.push_back({ folder, baseName(folder) });
            for (auto& f : files) indexLooseFile(f.first, f.second, lib);
        }
        for (const std::string& w : lib.warnings) printf("  warning: %s\n", w.c_str());
        printf("%zu asset(s) indexed, %zu warning(s)\n",
               lib.entries.size(), lib.warnings.size());
        size_t shown = 0, bytes = 0, failed = 0;
        std::map<std::string, std::array<int, 2>> convStats;
        std::map<std::string, std::string> convErrs;
        for (size_t i = 0; i < lib.entries.size(); ++i) {
            const LibraryEntry& e = lib.entries[i];
            if (!filter.empty() && e.path.find(filter) == std::string::npos) continue;
            Bytes data;
            std::string err;
            if (!lib.read(i, data, &err)) {
                if (failed < 5) printf("  FAIL %s - %s\n", e.path.c_str(), err.c_str());
                failed++;
                continue;
            }
            bytes += data.size();
            if (!outRaw.empty()) writeFile(outRaw + "/" + e.path, data);
            if (convertAll) {
                // exactly what the window does when an asset is double-clicked
                auto fmts = formatsFor(e.path);
                Bytes outBytes;
                std::string err, used;
                bool ok = false;
                try {
                    ok = convertAsset(e.path, data.data(), data.size(),
                                      fmts.empty() ? "raw" : fmts[0], outBytes, &err, &used);
                } catch (...) { err = "exception"; }
                std::string x = extensionOf(e.path) + " -> " + (fmts.empty() ? "raw" : fmts[0]);
                convStats[x][ok ? 0 : 1]++;
                if (!ok && convErrs[x].empty()) convErrs[x] = e.path + ": " + err;
                if (!ok && getenv("MT_FAILS")) printf("  FAILCONV %s: %s\n", e.path.c_str(), err.c_str());
            }
            if (shown < 10)
                printf("  %-70s %zu bytes\n", e.path.c_str(), data.size());
            shown++;
        }
        printf("read %zu asset(s), %zu bytes, %zu failed; cache holds %zu MB\n",
               shown, bytes, failed, lib.cacheBytes / (1024 * 1024));
        for (auto& c : convStats)
            printf("  convert %-22s ok %5d  failed %5d  %s\n", c.first.c_str(), c.second[0],
                   c.second[1], convErrs[c.first].c_str());
        return 0;
    }

    if (cmd == "render") {
        if (argc < 4) { usage(); return 1; }
        std::string in = argv[2], out = argv[3], texRoot, forceTex;
        bool flipUvTest = false;
        std::string onlyPart;       // draw the parts whose name holds this
        int W = 960, H = 540;
        std::string renderWheel;
        std::string layoutFile;     // NFS Undercover / Shift: an event's layout of the road pieces
        std::string setupFile, coloursFile;   // No Limits: a car setup's paint, and the game's colours
        float animT = -1;
        RenderView view;
        float posX = 0, posY = 0, posZ = 0;
        bool haveYaw = false, havePitch = false;
        float yawDeg = 0, pitchDeg = 0;
        for (int i = 4; i < argc; ++i) {
            std::string a = argv[i];
            auto next = [&](int n) { return i + n < argc ? (float)atof(argv[i + n]) : 0.0f; };
            if (a == "--size" && i + 2 < argc) { W = atoi(argv[i+1]); H = atoi(argv[i+2]); i += 2; }
            else if (a == "--yaw")   { yawDeg = next(1); haveYaw = true; ++i; }
            else if (a == "--pitch") { pitchDeg = next(1); havePitch = true; ++i; }
            else if (a == "--pos" && i + 3 < argc) {
                posX = next(1); posY = next(2); posZ = next(3); i += 3;
            }
            else if (a == "--lod" && i + 1 < argc) { view.lodFilter = argv[++i]; }
            else if (a == "--kit" && i + 1 < argc) { view.kit = argv[++i]; }
            else if (a == "--vcolor") { view.vertexColors = true; }
            else if (a == "--tex" && i + 1 < argc) { texRoot = argv[++i]; }
            else if (a == "--forcetex" && i + 1 < argc) { forceTex = argv[++i]; }
            else if (a == "--flipuv") { flipUvTest = true; }
            else if (a == "--only" && i + 1 < argc) { onlyPart = argv[++i]; }
            else if (a == "--twosided") view.twoSided = true;
            else if (a == "--wheel" && i + 1 < argc) { renderWheel = argv[++i]; }
            else if (a == "--anim" && i + 1 < argc) { animT = (float)atof(argv[++i]); }
            else if (a == "--layout" && i + 1 < argc) { layoutFile = argv[++i]; }
            else if (a == "--setup" && i + 1 < argc) { setupFile = argv[++i]; }
            else if (a == "--colours" && i + 1 < argc) { coloursFile = argv[++i]; }
        }
        Bytes data;
        if (!readFile(in, data)) { printf("cannot read %s\n", in.c_str()); return 1; }
        Model m = loadModel(data.data(), data.size());
        if (!onlyPart.empty()) {
            std::vector<Mesh> keep;
            for (Mesh& me : m.meshes) if (me.name.find(onlyPart) != std::string::npos) keep.push_back(std::move(me));
            m.meshes.swap(keep);
            if (m.meshes.empty()) { printf("no part named like %s\n", onlyPart.c_str()); return 1; }
        }
        if (flipUvTest)      // a check of the V convention: every mesh's V turned over
            for (Mesh& me : m.meshes)
                for (size_t k = 1; k < me.uvs.size(); k += 2) me.uvs[k] = 1.0f - me.uvs[k];
        if (!m.valid || m.meshes.empty()) { printf("no meshes in %s\n", in.c_str()); return 1; }
        if (!renderWheel.empty()) {
            Bytes wb;
            bool joints = false;     // NFS Edge's .m3g cars carry J_wheel_* joints too
            for (const Hardpoint& h : m.points) if (h.name.rfind("J_wheel_", 0) == 0) joints = true;
            if ((extensionOf(in) == "sb3d" || joints) && readFile(renderWheel, wb)) {
                Model wheel = loadModel(wb.data(), wb.size());
                // prefabs/cars/<car>.prefabs.sb, beside models/ (models/cars/<car>/<car>.sb3d)
                float radii[2] = { 0, 0 };
                bool have = false;
                {
                    std::string car = stripExtension(baseName(in));
                    std::string root = in;
                    for (int up = 0; up < 4; ++up) {
                        size_t sl = root.find_last_of("/\\");
                        root = sl == std::string::npos ? std::string(".") : root.substr(0, sl);
                    }
                    Bytes raw, sb;
                    std::string pf = root + "/prefabs/cars/" + car + ".prefabs.sb";
                    if (readFile(pf, raw) && nlSbDecode(baseName(pf), raw.data(), raw.size(), sb) &&
                        nlPrefabWheelRadii(sb.data(), sb.size(), radii)) {
                        have = true;
                        printf("tyre radius %.3f front, %.3f rear (%s)\n", radii[0], radii[1], pf.c_str());
                    }
                }
                // data/visualparts/C_<car>_1.sb: the stock arch's tyre width and radius tweaks
                NlWheelTweak tweak;
                {
                    std::string car = stripExtension(baseName(in));
                    std::string root = in;
                    for (int up = 0; up < 4; ++up) {
                        size_t sl = root.find_last_of("/\\");
                        root = sl == std::string::npos ? std::string(".") : root.substr(0, sl);
                    }
                    std::vector<std::string> cand;
                    if (getenv("MT_VISUALPARTS")) cand.push_back(getenv("MT_VISUALPARTS"));
                    std::error_code ec;
                    for (auto& e : std::filesystem::directory_iterator(root + "/data/visualparts", ec))
                        if (nlVisualPartsMatches(e.path().filename().string(), car))
                            cand.push_back(e.path().string());
                    for (const std::string& f : cand) {
                        Bytes raw, sb;
                        if (readFile(f, raw) && nlSbDecode(baseName(f), raw.data(), raw.size(), sb) &&
                            nlVisualPartsWheelTweak(sb.data(), sb.size(), tweak)) {
                            printf("tyre width +%.3f front, +%.3f rear (%s)\n", tweak.widthOffset[0],
                                   tweak.widthOffset[1], f.c_str());
                            break;
                        }
                    }
                }
                printf("%d wheel(s) placed\n", nlAttachWheels(m, wheel, have ? radii : nullptr, &tweak));
                if (getenv("MT_PREFAB")) {
                    Bytes raw, sb;
                    if (readFile(getenv("MT_PREFAB"), raw) &&
                        nlSbDecode(baseName(getenv("MT_PREFAB")), raw.data(), raw.size(), sb))
                        for (const PrefabModel& pm : nlPrefabModels(sb.data(), sb.size()))
                            printf("prefab model %s at %.3f %.3f %.3f\n", pm.file.c_str(), pm.pos[0], pm.pos[1], pm.pos[2]);
                }
            }
        }
        {
            Bytes points;
            std::string pointsPath = rr3PointsNameFor(in);
            if (!fileExists(pointsPath)) {
                // Real Racing GTI: the .rr_car names the points file
                std::string dir = in.substr(0, in.find_last_of("/\\") + 1);
                std::vector<std::pair<std::string, std::string>> files;
                scanLoose(dir.empty() ? "." : dir, "", files);
                for (auto& f : files) {
                    if (extensionOf(f.first) != "rr_car") continue;
                    Bytes raw;
                    Rr1Car c;
                    if (readFile(f.first, raw) && rr1ReadCar(raw.data(), raw.size(), c) &&
                        c.model == baseName(in) && !c.points.empty()) { pointsPath = dir + c.points; break; }
                }
            }
            if (pointsPath != in && readFile(pointsPath, points)) {
                std::vector<Hardpoint> pts = rr3ReadPoints(points.data(), points.size());
                // the steering wheel's point lives in the cockpit's file
                Bytes inner;
                std::string innerPath = rr3InteriorPointsNameFor(in);
                if (!pts.empty() && !innerPath.empty() && readFile(innerPath, inner)) {
                    rr3MergeCockpitPoints(pts, rr3ReadPoints(inner.data(), inner.size()));
                    printf("using %s for the cockpit points\n", innerPath.c_str());
                }
                if (!pts.empty()) {
                    rr3PlaceParts(m, pts);
                    if (rr1IsCar(m)) {
                        // the .rr_car beside it that names this model
                        Rr1Car car;
                        bool have = false;
                        std::string dir = in.substr(0, in.find_last_of("/\\") + 1);
                        std::vector<std::pair<std::string, std::string>> files;
                        scanLoose(dir.empty() ? "." : dir, "", files);
                        for (auto& f : files) {
                            if (extensionOf(f.first) != "rr_car") continue;
                            Bytes raw;
                            Rr1Car c;
                            if (readFile(f.first, raw) && rr1ReadCar(raw.data(), raw.size(), c) &&
                                c.model == baseName(in)) { car = c; have = true; break; }
                        }
                        rr1OrganizeCar(m, pts, have ? &car : nullptr);
                        if (have) {
                            printf("using %s (%s)\n", car.name.c_str(), car.exterior.c_str());
                            if (renderWheel.empty()) renderWheel = dir + car.wheelModel;
                        }
                    }
                    printf("using %s (%zu hardpoint(s))\n", pointsPath.c_str(), pts.size());
                    // wheels: --wheel, or the car's own <car>_shared.m3g beside it
                    std::string wp = renderWheel;
                    if (wp.empty()) wp = rr3SharedNameFor(in);
                    Bytes wb;
                    if (!wp.empty() && readFile(wp, wb)) {
                        Model wheel = loadModel(wb.data(), wb.size());
                        // Real Racing 1: the wheel's texture is <wheel>.pvr beside it
                        {
                            std::string own = stripExtension(wp) + ".pvr";
                            Bytes tb;
                            if (readFile(own, tb))
                                for (Mesh& me : wheel.meshes) if (me.texture.empty()) me.texture = baseName(own);
                        }
                        printf("wheels from %s: %d\n", wp.c_str(), rr3AttachWheels(m, wheel, pts, rr1IsCar(m)));
                    }
                }
            }
            // Real Racing 3's driver: fifteen parts posed by his .banim
            {
                std::string da = rr3DriverAnimNameFor(in);
                Bytes db;
                if (!da.empty() && (readFile(da, db) ||
                                    readFile(in.substr(0, in.size() - baseName(in).size()) + "driver.banim", db)))
                    rr3PoseDriver(m, db.data(), db.size());
            }
            // an animated part keeps its rest pose in its own .banim
            Bytes banim;
            std::string banimPath = stripExtension(pointsPath) + "_wing.banim";
            if (readFile(banimPath, banim)) {
                rr3PlaceAnimated(m, banim.data(), banim.size(), "wing");
                printf("using %s\n", banimPath.c_str());
            }
            rr2AssignTextures(m, in);
            mwFillCarTextures(m, stripExtension(baseName(in)));
            if (extensionOf(in) == "m3g" && !texRoot.empty()) {
                // Real Racing 3: <car>_<material>.etc.dds, livery/ and ../common
                std::string missing;
                int n = cliRr3Textures(m, in, texRoot, &missing);
                printf("  %d part(s) given a texture%s%s\n", n, missing.empty() ? "" : "; none for: ",
                       missing.c_str());
            }
            for (const std::string& w : m.warnings) printf("  %s\n", w.c_str());
        }
        if (animT >= 0) {
            poseModel(m, animT);
            printf("posed at %.0f ms of %.0f\n", animT, modelAnimDuration(m));
        }
        if (getenv("MT_POINTS"))
            for (const Hardpoint& h : m.points)
                printf("POINT %-50s %7.3f %7.3f %7.3f  basis %6.3f %6.3f %6.3f / %6.3f %6.3f %6.3f / %6.3f %6.3f %6.3f\n", h.name.c_str(), h.pos[0], h.pos[1], h.pos[2], h.basis[0], h.basis[1], h.basis[2], h.basis[3], h.basis[4], h.basis[5], h.basis[6], h.basis[7], h.basis[8]);
        // Real Racing Next: vehicles/<car>/<car>.sb3d samples textures from
        // vehicles/<car>/car_textures, found anywhere under --tex
        if (extensionOf(in) == "sb3d" && in.find("vehicles/") != std::string::npos && !texRoot.empty()) {
            std::string dir = in.substr(0, in.find_last_of('/'));
            std::string car = baseName(dir);
            std::vector<std::pair<std::string, std::string>> files;
            scanLoose(texRoot, "", files);
            std::map<std::string, std::string> byLeaf;
            for (auto& f : files) byLeaf.emplace(baseName(f.first), f.first);
            int got = 0;
            for (Mesh& me : m.meshes) {
                std::string mat = me.material;
                for (char& c : mat) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                if (mat.empty()) continue;
                for (const std::string& c : rrNextTextureCandidates(car, mat)) {
                    auto it = byLeaf.find(c);
                    if (it != byLeaf.end()) { me.texture = it->second; ++got; break; }
                }
            }
            printf("  %d part(s) given a Real Racing Next texture\n", got);
            Bytes cp;
            if (readFile(dir + "/" + car + "_carpoints.sb", cp))
                printf("  %d wheel(s) placed from the carpoints\n", rrNextAttachWheels(m, cp.data(), cp.size()));
        }
        // NFS Undercover / Shift / Shift 2: what the game binds by name
        {
            std::string leaf = stripExtension(baseName(in));
            bool scene = !m.meshes.empty() && m.meshes[0].part.compare(0, 5, "part_") == 0 &&
                         m.meshes[0].material.compare(0, 4, "m3g_") == 0;
            if (scene) {
                std::string root = texRoot.empty() ? (in.find_last_of("/\\") == std::string::npos ? std::string(".") : in.substr(0, in.find_last_of("/\\") + 1)) : texRoot;
                std::vector<std::pair<std::string, std::string>> files;
                scanLoose(root, "", files);
                std::map<std::string, std::string> byLeaf;
                for (auto& f : files) byLeaf.emplace(baseName(f.first), f.first);
                if (!layoutFile.empty()) byLeaf[baseName(layoutFile)] = layoutFile;
                EaPhoneFiles ef;
                for (auto& kv : byLeaf) ef.leaves.push_back(kv.first);
                ef.find = [&](const std::string& want) {
                    auto it = byLeaf.find(want);
                    return it == byLeaf.end() ? std::string() : it->second;
                };
                ef.read = [](const std::string& p, Bytes& b) {
                    if (!readFile(p, b)) return false;
                    if (getenv("MT_TILES") && baseName(p).find("_") != std::string::npos && extensionOf(p) == "bin") {
                        std::vector<EaTile> t;           // debugging: keep tiles lo..hi (the layout is re-read)
                        if (eaPhoneReadLayout(b.data(), b.size(), t)) {
                            int lo = 0, hi = 0;
                            sscanf(getenv("MT_TILES"), "%d-%d", &lo, &hi);
                            if (b[0] == 0) { if (b.size() > 6) b[6] = (uint8_t)std::min<int>(hi + 1, b[6]); }
                            else b[0] = (uint8_t)std::min<int>(hi + 1, b[0]);
                        }
                    }
                    return true;
                };
                for (const std::string& s : eaPhoneDress(m, leaf, ef, layoutFile.empty() ? std::string() : baseName(layoutFile)))
                    printf("  %s\n", s.c_str());
                if (texRoot.empty()) texRoot = root;
            }
        }
        if (getenv("MT_BBOX"))
            for (const Mesh& me : m.meshes) {
                float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
                for (size_t k = 0; k + 2 < me.positions.size(); k += 3)
                    for (int c = 0; c < 3; ++c) { lo[c] = std::min(lo[c], me.positions[k + c]); hi[c] = std::max(hi[c], me.positions[k + c]); }
                printf("BBOX %-40s [%s] {%s} %s kit %c slot %-24s %6.2f %6.2f %6.2f  %6.2f %6.2f %6.2f\n", me.name.c_str(), me.material.c_str(), me.texture.c_str(), me.lod.c_str(), me.kit ? me.kit : 0x2d, me.kitSlot.c_str(), lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
            }
        if (getenv("MT_PARTRANGE")) {          // debugging: keep parts lo..hi of part_<n>
            int lo = 0, hi = 0;
            sscanf(getenv("MT_PARTRANGE"), "%d-%d", &lo, &hi);
            std::vector<Mesh> keep;
            for (Mesh& me : m.meshes) {
                int n = me.part.compare(0, 5, "part_") == 0 ? atoi(me.part.c_str() + 5) : -1;
                if (n >= lo && n <= hi) keep.push_back(std::move(me));
            }
            m.meshes.swap(keep);
        }
        if (getenv("MT_FORCETEX"))
            for (Mesh& me : m.meshes) me.texture = getenv("MT_FORCETEX");
        frameModel(m, view);
        if (haveYaw)   view.yaw   = yawDeg   * 3.14159265f / 180.0f;
        if (havePitch) view.pitch = pitchDeg * 3.14159265f / 180.0f;
        view.modelPos[0] = posX; view.modelPos[1] = posY; view.modelPos[2] = posZ;

        // No Limits: the paint a car setup gives the body, rims and calipers
        if (!setupFile.empty()) {
            Bytes sr, ss, cr, cs;
            if (coloursFile.empty()) coloursFile = "colours.sb";
            if (readFile(setupFile, sr) && nlSbDecode(baseName(setupFile), sr.data(), sr.size(), ss) &&
                readFile(coloursFile, cr) && nlSbDecode(baseName(coloursFile), cr.data(), cr.size(), cs)) {
                std::vector<NlPaintColour> cols = nlPaintColours(cs.data(), cs.size());
                NlCarPaint paint = nlCarSetupPaint(ss.data(), ss.size(), cols);
                static const char* kKind[] = { "body", "rim", "brake", "window" };
                for (int k = 0; k < NL_PAINT_KINDS; ++k)
                    if (paint.have[k]) printf("  %s: %s (%.3f %.3f %.3f)\n", kKind[k], paint.name[k].c_str(),
                                              paint.rgb[k][0], paint.rgb[k][1], paint.rgb[k][2]);
                printf("%d part(s) painted (%zu colours known)\n", nlApplyCarPaint(m, paint), cols.size());
            } else {
                printf("cannot read %s / %s\n", setupFile.c_str(), coloursFile.c_str());
            }
        }

        // optional textures: one decoded diffuse per mesh, looked up by the
        // path the model itself states
        std::vector<Image> store;
        std::vector<const Image*> perMesh;
        // textures the file itself carries (#img:N) need no folder
        if (texRoot.empty() && !m.images.empty()) {
            perMesh.assign(m.meshes.size(), nullptr);
            size_t got = 0;
            for (size_t i = 0; i < m.meshes.size(); ++i) {
                const std::string& t = m.meshes[i].texture;
                if (t.compare(0, 5, "#img:") != 0) continue;
                size_t k = (size_t)atoi(t.c_str() + 5);
                if (k < m.images.size()) { perMesh[i] = &m.images[k]; ++got; }
            }
            printf("%zu of %zu mesh(es) textured from the file's own images\n", got, perMesh.size());
        }
        if (!texRoot.empty()) {
            store.resize(m.meshes.size());
            perMesh.assign(m.meshes.size(), nullptr);
            for (size_t i = 0; i < m.meshes.size(); ++i) {
                std::string t = m.meshes[i].texture;
                if (t.empty()) continue;
                if (t.compare(0, 5, "#img:") == 0) {
                    size_t k = (size_t)atoi(t.c_str() + 5);
                    if (k < m.images.size()) perMesh[i] = &m.images[k];
                    continue;
                }
                int paintMode = 0;
                float paintRgb[3];
                if (t.compare(0, 7, "#paint:") == 0) {
                    std::string base;
                    if (nlParsePaintRef(t, paintMode, paintRgb, base)) t = base;
                    if (paintMode == 2) {
                        nlPaintImage(store[i], 2, paintRgb);
                        perMesh[i] = &store[i];
                        continue;
                    }
                }
                std::string leaf = t.substr(t.find_last_of('/') + 1);
                Bytes tb;
                std::string found;
                for (const std::string& c : textureRefCandidates(t)) {
                    std::string cl = c.substr(c.find_last_of('/') + 1);
                    if (readFile(texRoot + "/" + c, tb)) { found = texRoot + "/" + c; break; }
                    if (readFile(texRoot + "/" + cl, tb)) { found = texRoot + "/" + cl; break; }
                }
                if (found.empty()) {
                    if (!readFile(texRoot + "/" + t, tb) && !readFile(texRoot + "/" + leaf, tb) &&
                        !readFile(t, tb))
                        continue;
                }
                // a Real Racing 3 .z comes off first
                if (extensionOf(leaf) == "z") {
                    Bytes plain;
                    if (rr3Unwrap(leaf, tb.data(), tb.size(), plain, nullptr, nullptr)) tb.swap(plain);
                }
                if (getenv("MT_TEXDBG")) printf("  tex %s -> %s\n", m.meshes[i].name.c_str(), found.empty() ? t.c_str() : found.c_str());
                if (decodeTextureFile(tb.data(), tb.size(), store[i])) {
                    if (paintMode) nlPaintImage(store[i], paintMode, paintRgb);
                    perMesh[i] = &store[i];
                    for (const std::string& comp : found.empty() ? std::vector<std::string>() : alphaCompanions(found)) {
                        Bytes ab;
                        Image alpha;
                        if (!readFile(comp, ab)) continue;
                        if (extensionOf(comp) == "z") {
                            Bytes plain;
                            if (rr3Unwrap(baseName(comp), ab.data(), ab.size(), plain, nullptr, nullptr)) ab.swap(plain);
                        }
                        if (decodeTextureFile(ab.data(), ab.size(), alpha) && applyEtcAlpha(store[i], alpha)) break;
                    }
                }
            }
            size_t got = 0;
            for (const Image* im : perMesh) if (im) ++got;
            printf("%zu of %zu mesh(es) textured\n", got, perMesh.size());
        }

        // one texture on every mesh - a way to check UV interpolation without
        // needing the model's own textures to hand
        Image forced;
        if (!forceTex.empty()) {
            Bytes tb;
            if (readFile(forceTex, tb)) {
                auto entries = readSba(tb.data(), tb.size());
                if (!entries.empty() && decodeImageAuto(entries[0], forced)) {
                    perMesh.assign(m.meshes.size(), &forced);
                    printf("forced texture %dx%d\n", forced.width, forced.height);
                }
            }
        }

        Image frame;
        renderModel(m, view, perMesh.empty() ? nullptr : &perMesh, W, H, frame);
        Bytes png = encodePng(frame);
        if (png.empty() || !writeFile(out, png)) { printf("cannot write %s\n", out.c_str()); return 1; }
        printf("wrote %s (%dx%d, %zu meshes, size %.3f)\n", out.c_str(), W, H,
               m.meshes.size(), view.modelSize);
        return 0;
    }

    if (argc < 3) { usage(); return 1; }
    std::string folder = argv[2];
    std::string outDir = argc > 3 ? argv[3] : "";
    std::string texFmt = "png", mdlFmt = "fbx", only;
    for (int i = 3; i < argc - 1; ++i) {
        if (!strcmp(argv[i], "--tex")) texFmt = argv[i + 1];
        else if (!strcmp(argv[i], "--mdl")) mdlFmt = argv[i + 1];
        else if (!strcmp(argv[i], "--only")) only = argv[i + 1];
    }

    // descend into the usual Android layout if given the app root
    const char* subs[] = {"/com.ea.game.nfs14_row/files/packs", "/files/packs", "/packs"};
    for (const char* s : subs) {
        std::string cand = folder + s;
        std::vector<std::string> probe;
        scanArchives(cand, probe);
        if (!probe.empty()) { folder = cand; break; }
    }

    std::vector<std::string> archives;
    scanArchives(folder, archives);
    std::stable_sort(archives.begin(), archives.end(),
                     [](const std::string& a, const std::string& b) {
                         return extensionOf(a) == "pack" && extensionOf(b) != "pack";
                     });
    if (archives.empty()) {
        printf("No .pack or .cab files under %s\n", folder.c_str());
        return 1;
    }
    printf("zstd: %s\n", zstdBackend().c_str());
    printf("%zu archive(s) under %s\n\n", archives.size(), folder.c_str());

    int totalAssets = 0, written = 0, failed = 0, blocked = 0;
    std::vector<std::string> warnings;

    for (const std::string& a : archives) {
        ExtractResult r;
        extractArchive(a, r);
        blocked += r.blockedByCodec;
        for (auto& w : r.warnings) warnings.push_back(w);
        for (auto& asset : r.assets) {
            totalAssets++;
            if (!only.empty()) {
                if (only == "textures" && asset.kind != "texture") continue;
                if (only == "models" && asset.kind != "model") continue;
                if (only == "sounds" && asset.kind != "sound") continue;
            }
            if (cmd == "list") {
                printf("%10zu  %s\n", asset.data.size(), asset.path.c_str());
                continue;
            }
            std::string rel = asset.path;
            Bytes out;
            if (cmd == "convert") {
                std::string ext = extensionOf(asset.path);
                std::string fmt = ext == "sb3d" ? mdlFmt : (ext == "sba" ? texFmt : "raw");
                std::string err, usedExt;
                if (!convertAsset(asset.path, asset.data.data(), asset.data.size(),
                                  fmt, out, &err, &usedExt) || out.empty()) {
                    failed++;
                    if (!err.empty()) warnings.push_back(asset.path + ": " + err);
                    continue;
                }
                // name the file after what it actually is, not what was asked
                // for - an undecodable payload stays .bin rather than a .png
                // that no viewer can open
                if (!usedExt.empty() && usedExt != ext)
                    rel = stripExtension(rel) + "." + usedExt;
                if (!err.empty()) warnings.push_back(asset.path + ": " + err);
            } else {
                out = asset.data;
            }
            if (writeFile(joinPath(outDir, rel), out)) written++;
            else failed++;
        }
    }

    if (cmd != "list") {
        printf("\n%d asset(s) found, %d written, %d failed\n", totalAssets, written, failed);
        if (blocked)
            printf("%d asset(s) skipped%s\n", blocked,
                   zstdAvailable() ? "" : " - libzstd not found (needed for models)");
    } else {
        printf("\n%d asset(s)\n", totalAssets);
    }
    if (!warnings.empty()) {
        printf("\n%zu warning(s):\n", warnings.size());
        for (size_t i = 0; i < warnings.size() && i < 20; ++i)
            printf("  %s\n", warnings[i].c_str());
    }
    return 0;
}
