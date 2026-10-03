// nfsnl_profiles.cpp - which games Monkey Tool knows about
//
// The tool opens on a profile, the way Frosty does: pick the game first, then
// point at its files. A profile says what to look for and what the formats
// inside mean, so adding one of the studio's other games is a profile plus
// whatever readers that game needs - not a second program.
//
// Undercover, Shift, Shift 2, No Limits, No Limits VR, Edge, Most Wanted,
// Hot Pursuit, Real Racing Next,
// Real Racing 3, Real Racing 2, Real Racing and Real Racing GTI.
#include "nfsnl.h"
#include "resource.h"

namespace nfsnl {

namespace {

GameProfile make(const char* id, const char* name, const char* publisher,
                 const char* folderHint, const char* note, bool supported,
                 int logo) {
    GameProfile p;
    p.id = id;
    p.name = name;
    p.publisher = publisher;
    p.folderHint = folderHint;
    p.note = note;
    p.archiveExtensions = {".pack", ".cab"};
    p.supported = supported;
    p.logoResource = logo;
    return p;
}

} // namespace

const std::vector<GameProfile>& gameProfiles() {
    static std::vector<GameProfile> profiles = [] {
        std::vector<GameProfile> p;
        p.push_back(make("nfs_no_limits", "Need for Speed: No Limits",
                         "Firemonkeys / Electronic Arts", "com.ea.game.nfs14_row",
                         "Android build. Models, textures, audio banks, effects.",
                         true, IDR_LOGO_NFSNL));
        {
            // Most Wanted (2012, mobile): SBIN version 3 data and textures
            // and IM2 models, loose or inside the Android .obb - read after
            // Hypercycle's NFSMW12MobileTools.
            GameProfile mw = make("nfs_most_wanted", "Need for Speed: Most Wanted",
                             "Firemonkeys / Electronic Arts", "com.ea.games.nfs13_row",
                             "Android build (the .obb or its unpacked files). Data, "
                             "textures and models.",
                             true, IDR_LOGO_NFSMW);
            mw.looseFiles = true;
            mw.archiveExtensions = {};
            mw.folderSteps = { "\\com.ea.games.nfs13_row", "\\com.ea.games.nfs13_na" };
            p.push_back(std::move(mw));
        }
        {
            // Hot Pursuit (2010, mobile): IM-M3G models (Most Wanted's layout
            // with JSR-184 delta-coded vertices and strip arrays), textures as
            // M3G Image2D files, SBIN version 2 prefabs (.m3g.sb)
            GameProfile hp = make("nfs_hot_pursuit", "Need for Speed: Hot Pursuit",
                             "Firemonkeys / Electronic Arts", "com.eamobile.nfshp_row",
                             "Android build, loose files. Models, textures and "
                             "prefab data.",
                             true, IDR_LOGO_NFSHP);
            hp.looseFiles = true;
            hp.archiveExtensions = {};
            hp.folderSteps = { "\\com.eamobile.nfshp_row_wf", "\\com.eamobile.nfshp_row",
                               "\\com.eamobile.nfshp_na" };
            p.push_back(std::move(hp));
        }
        {
            // No Limits VR (2016, Android): its .obb is not a zip. It is read
            // as whatever it turns out to be - an FMOBB file like NFS Edge's,
            // a zip, or No Limits-style .pack archives.
            GameProfile vr = make("nfs_no_limits_vr", "Need for Speed: No Limits VR",
                             "Firemonkeys / Electronic Arts", "com.ea.gp.nfs14vr",
                             "Android build: the folder with the .obb (or the .apk).",
                             true, IDR_LOGO_NFSNLVR);
            vr.looseFiles = true;
            vr.archiveExtensions = {};
            vr.folderSteps = { "\\com.ea.gp.nfs14vr" };
            p.push_back(std::move(vr));
        }
        {
            // NFS Edge (2017, Android, China): the whole game is one FMOBB
            // file, assets\obb\main.<n>.com.ddl.eanfs.qh.obb.png inside the
            // .apk - not a picture, whatever the name says. The files inside
            // are No Limits' generation: SBA textures (ETC, with the alpha in
            // a second _ETCAlpha texture), .m3g models, .sb data, FMOD audio.
            GameProfile edge = make("nfs_edge", "Need for Speed: Edge",
                             "Electronic Arts / Tencent", "the folder with the NFS Edge .apk",
                             "Android build: the .apk, or the main.*.obb.png taken out of it. "
                             "Models, textures, data, audio.",
                             true, IDR_LOGO_NFSEDGE);
            edge.looseFiles = true;
            edge.archiveExtensions = {};
            p.push_back(std::move(edge));
        }
        {
            // Real Racing Next: No Limits' .pack / .cab archives (LZHAM and
            // zstd), SB3D models with float positions, ASTC textures.
            GameProfile nx = make("real_racing_next", "Real Racing Next",
                             "Firemonkeys / Electronic Arts", "com.ea.games.r3next",
                             "Android build (.pack files). Cars, tracks, ASTC textures.",
                             true, IDR_LOGO_RRNEXT);
            nx.folderSteps = { "\\com.ea.games.r3next\\files\\packs", "\\files\\packs", "\\packs" };
            p.push_back(std::move(nx));
        }
        {
            GameProfile rr3 = make("real_racing_3", "Real Racing 3",
                             "Firemonkeys / Electronic Arts",
                             "com.ea.games.r3_row\\files\\.depot",
                             "Android build. Models with their wheel and "
                             "steering positions, ETC textures, sounds.",
                             true, IDR_LOGO_RR3);
            rr3.looseFiles = true;
            rr3.archiveExtensions = {};
            rr3.folderSteps = { "\\com.ea.games.r3_row\\files\\.depot",
                                "\\files\\.depot", "\\.depot" };
            p.push_back(std::move(rr3));
        }
        {
            // Real Racing 2 keeps its files loose, like Real Racing 3, but
            // unwrapped: .m3g models (JSR-184), .points hardpoints in 16.16
            // fixed point, and legacy PVR textures, most of them ATC.
            GameProfile rr2 = make("real_racing_2", "Real Racing 2",
                             "Firemint / Electronic Arts",
                             "com.ea.game.realracing2_OTD_row",
                             "Android build. Cars with their interiors, ATC and "
                             "PVR textures.",
                             true, IDR_LOGO_RR2);
            rr2.looseFiles = true;
            rr2.archiveExtensions = {};
            rr2.folderSteps = { "\\com.ea.game.realracing2_OTD_row" };
            p.push_back(std::move(rr2));
        }
        {
            // Real Racing (2009) and Real Racing GTI (2010) are iPhone games:
            // the .ipa is a zip, read in place. Inside: JSR-184 .m3g cars,
            // .points in int16 millimetres, PVRTC .pvr textures, .banim.
            GameProfile rr1 = make("real_racing", "Real Racing",
                             "Firemint", "the folder with the Real Racing .ipa",
                             "iOS only: the .ipa (or its unpacked Payload folder). "
                             "Cars, wheel points, textures.",
                             true, IDR_LOGO_RR1);
            rr1.looseFiles = true;
            rr1.archiveExtensions = {};
            p.push_back(std::move(rr1));
            GameProfile gti = make("real_racing_gti", "Real Racing GTI",
                             "Firemint / Volkswagen", "the folder with the Real Racing GTI .ipa",
                             "iOS only: the .ipa (or its unpacked Payload folder). "
                             "Cars, wheels, tracks, textures.",
                             true, IDR_LOGO_RRGTI);
            gti.looseFiles = true;
            gti.archiveExtensions = {};
            p.push_back(std::move(gti));
        }
        {
            // NFS Undercover (2009), Shift (2009) and Shift 2 Unleashed
            // (2011) on the iPhone: the .ipa read in place. Standard JSR-184
            // .m3g files (Shift's gzipped), the textures M3G Image2Ds in
            // EA's PVRTC 4 bpp, tracks carrying their atlases inside.
            struct IosNfs { const char* id; const char* name; const char* note; int logo; };
            static const IosNfs kIos[] = {
                { "nfs_undercover", "Need for Speed: Undercover",
                  "iOS: the .ipa (or its Payload folder). Cars with their body kits, "
                  "city maps with their textures, M3G textures.", IDR_LOGO_NFSUC },
                { "nfs_shift", "Need for Speed: Shift",
                  "iOS: the .ipa (or its Payload folder). Cars, cockpits, tracks, "
                  "M3G textures.", IDR_LOGO_NFSSHIFT },
                { "nfs_shift_2", "Need for Speed: Shift 2 Unleashed",
                  "iOS: the .ipa (or its Payload folder). Cars, cockpits, tracks, "
                  "M3G textures.", IDR_LOGO_NFSSHIFT2 },
            };
            for (const IosNfs& g : kIos) {
                GameProfile ip = make(g.id, g.name, "Electronic Arts / IronMonkey",
                                      "the folder with the .ipa", g.note, true, g.logo);
                ip.looseFiles = true;
                ip.archiveExtensions = {};
                p.push_back(std::move(ip));
            }
        }
        // the picker's grid: Need for Speed on the first row, Real Racing on
        // the second, each oldest first
        struct Place { const char* id; int row, col, icon, splash; };
        static const Place kPlaces[] = {
            { "nfs_undercover", 0, 0, IDR_ICON_NFSUC, IDR_SPLASH_NFSUC },
            { "nfs_shift", 0, 1, IDR_ICON_NFSSHIFT, IDR_SPLASH_NFSSHIFT },
            { "nfs_hot_pursuit", 0, 2, IDR_ICON_NFSHP, IDR_SPLASH_NFSHP },
            { "nfs_shift_2", 0, 3, IDR_ICON_NFSSHIFT2, IDR_SPLASH_NFSSHIFT2 },
            { "nfs_most_wanted", 0, 4, IDR_ICON_NFSMW, IDR_SPLASH_NFSMW },
            { "nfs_no_limits", 0, 5, IDR_ICON_NFSNL, IDR_SPLASH_NFSNL },
            { "nfs_no_limits_vr", 0, 6, IDR_ICON_NFSNLVR, IDR_SPLASH_NFSNLVR },
            { "nfs_edge", 0, 7, IDR_ICON_NFSEDGE, IDR_SPLASH_NFSEDGE },
            { "real_racing", 1, 0, IDR_ICON_RR1, IDR_SPLASH_RR1 },
            { "real_racing_gti", 1, 1, IDR_ICON_RRGTI, IDR_SPLASH_RRGTI },
            { "real_racing_2", 1, 2, IDR_ICON_RR2, IDR_SPLASH_RR2 },
            { "real_racing_3", 1, 3, IDR_ICON_RR3, IDR_SPLASH_RR3 },
            { "real_racing_next", 1, 4, IDR_ICON_RRNEXT, IDR_SPLASH_RRNEXT },
        };
        for (GameProfile& g : p)
            for (const Place& pl : kPlaces)
                if (g.id == pl.id) {
                    g.pickerRow = pl.row;
                    g.pickerColumn = pl.col;
                    g.iconResource = pl.icon;
                    g.splashResource = pl.splash;
                }
        return p;
    }();
    return profiles;
}

const GameProfile* findProfile(const std::string& id) {
    for (const GameProfile& p : gameProfiles())
        if (p.id == id) return &p;
    return nullptr;
}

std::string detectGameFolder(const std::string& path, const std::vector<std::string>& names) {
    std::string p = path;
    for (char& c : p) { if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); if (c == '\\') c = '/'; }
    // the Android package names - the surest sign there is
    struct Rule { const char* token; const char* id; };
    static const Rule kPath[] = {
        { "realracing2", "real_racing_2" }, { "com.ea.games.r3_", "real_racing_3" },
        { "r3_row", "real_racing_3" }, { "r3_na", "real_racing_3" },
        { "nfs14vr", "nfs_no_limits_vr" },
        { "nfs14", "nfs_no_limits" }, { "nfs13", "nfs_most_wanted" },
        { "nfshp", "nfs_hot_pursuit" }, { "r3next", "real_racing_next" },
        { "nfs14vr", "nfs_no_limits_vr" }, { "eanfs", "nfs_edge" }, { "nfsedge", "nfs_edge" },
        { "nfsuc_3d", "nfs_undercover" }, { "undercover", "nfs_undercover" },
        { "nfss2_", "nfs_shift_2" }, { "shift_2", "nfs_shift_2" }, { "shift 2", "nfs_shift_2" },
        { "shift2", "nfs_shift_2" }, { "nfss_3d", "nfs_shift" }, { "nfs_shift", "nfs_shift" },
        { "realracinggti", "real_racing_gti" }, { "real racing gti", "real_racing_gti" },
        { "real_racing_gti", "real_racing_gti" },
    };
    for (const Rule& r : kPath)
        if (p.find(r.token) != std::string::npos) return r.id;
    // what is inside
    int packs = 0, depot = 0, rr2cars = 0, sba = 0, pvr = 0, etc = 0, imm3g = 0, obb = 0;
    int edge = 0, gti = 0, rr1 = 0, uc = 0, shift1 = 0, shift2 = 0;
    for (const std::string& n0 : names) {
        std::string n = n0;
        for (char& c : n) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (n.find("eanfs") != std::string::npos || n.find("published.texture_etc") != std::string::npos)
            ++edge;
        // EA's iPhone NFS games, by the .ipa's or the .app's name
        if (n.find("undercover") != std::string::npos || n.find("nfsuc_3d") != std::string::npos) ++uc;
        else if (n.find("shift_2") != std::string::npos || n.find("shift2") != std::string::npos ||
                 n.find("nfss2_") != std::string::npos) ++shift2;
        else if (n.find("shift") != std::string::npos || n.find("nfss_3d") != std::string::npos) ++shift1;
        else if (n.find(".ipa") != std::string::npos || n.find(".rr_car") != std::string::npos) {
            if (n.find("gti") != std::string::npos) ++gti; else ++rr1;
        }
        if (n.find("gti_wheel") != std::string::npos) ++gti;
        if (n.size() > 5 && n.compare(n.size() - 5, 5, ".pack") == 0) ++packs;
        if (n == ".depot" || n.find("/.depot") != std::string::npos) ++depot;
        if (n.compare(0, 4, "car_") == 0 && n.find(".m3g") != std::string::npos) ++rr2cars;
        if (n.find(".sba") != std::string::npos) ++sba;
        if (n.find(".pvr") != std::string::npos) ++pvr;
        if (n.find(".etc.dds") != std::string::npos) ++etc;
        if (n.find("nfs13") != std::string::npos || n.find("main.") == 0) ++obb;
        if (n.find("texture_bonnet") != std::string::npos) ++imm3g;
    }
    if (edge) return "nfs_edge";
    if (uc) return "nfs_undercover";
    if (shift2) return "nfs_shift_2";
    if (shift1) return "nfs_shift";
    if (gti) return "real_racing_gti";
    if (rr1) return "real_racing";
    if (depot || etc > 3) return "real_racing_3";
    if (packs > 3) return "nfs_no_limits";
    if (rr2cars && pvr) return "real_racing_2";
    if (imm3g) return "nfs_hot_pursuit";
    if (sba > 3) return "nfs_most_wanted";
    return std::string();
}

} // namespace nfsnl
