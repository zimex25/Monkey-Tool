// main_win32.cpp - Monkey Tool, Windows GUI
//
//  * Open  -> pick the com.ea.game.nfs14_row folder; everything is unpacked
//             immediately into a temp folder that is deleted on exit.
//  * Tree  -> assets under their real in-game paths.
//  * Double-click an asset -> Save As dialog whose file-type dropdown picks
//             the output format (FBX/OBJ for models, PNG/JPG/BMP/TGA/DDS for
//             textures, raw for everything else).
//  * Convert all -> batch export of everything currently shown.
//  * View -> Dark / Light theme, remembered between runs.
//
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601     // Windows 7: the video player's Media Foundation
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <objbase.h>
#include <uxtheme.h>
#include <mmsystem.h>
// the video player (Media Foundation's MFPlay, Windows 7 and later); a build
// without the header opens videos in the system's own player instead
#if defined(__has_include)
#  if __has_include(<mfplay.h>)
#    include <mfplay.h>
#    define MT_HAVE_MFPLAY 1
#  endif
#endif
#include <GL/gl.h>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <set>
#include <array>
#include <atomic>
#include <algorithm>
#include <functional>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <cstdarg>
#include <cwchar>
#include <exception>
#include <cstdarg>

#include "nfsnl.h"
#include "resource.h"
#include "win32_save_editor.h"
#include "nfsnl_save.h"
#include <cstring>

#ifdef _MSC_VER
#  pragma comment(lib, "comctl32.lib")
#  pragma comment(lib, "comdlg32.lib")
#  pragma comment(lib, "shell32.lib")
#  pragma comment(lib, "ole32.lib")
#  pragma comment(lib, "uxtheme.lib")
#  pragma comment(lib, "winmm.lib")
#endif

using nfsnl::Bytes;

// ----------------------------------------------------------------- state

// A row in the tree. The bytes are not here and not on disk: `lib` is the
// index into the library, which produces them on demand.
// Height of the text block under the preview: the asset, its size, the file it
// comes from and what the reader had to say, without the last lines cut off.
static const int kInfoH = 135;

struct Entry {
    std::string path;      // in-game path
    size_t lib = 0;        // index into g_lib.entries
    size_t size = 0;
    std::string kind;
    uint64_t time = 0;     // the file's date on disk (its archive's, inside one)
};

enum {
    IDC_TREE = 1001,
    IDC_STATUS,
    IDC_OPEN,
    IDC_CONVERT,
    IDC_FILTER,
    IDC_POSX,
    IDC_POSY,
    IDC_POSZ,
    IDC_POSLABEL,
    IDC_LODCOMBO,
    IDC_KITCOMBO,
    IDC_PAINTCOMBO,
    IDC_RESETVIEW,
    IDC_ANIMATE,
    IDC_TEXTURED,
    IDC_VCOLORS,
    IDC_TEXTVIEW,
    IDC_PLAY,
    IDC_STOP,
    IDC_PREV,
    IDC_BACK,
    IDC_FWD,
    IDC_NEXT,
    IDC_SEEK,
    IDC_TIME,
    IDC_SORT_NAME,
    IDC_SORT_EXT,
    IDC_SORT_SIZE,
    IDC_SORT_DATE,
    IDC_SEARCH,
    IDC_SEARCHLABEL,
    IDC_SAVEEDITOR,
    IDC_PROGRESS,
    IDC_TB_OPEN,
    IDC_TB_EXPORT,
    IDC_TB_IMPORT,
    IDC_TB_SAVE,
    IDC_TB_NEWCAR,
    IDC_RIMLABEL,
    IDC_RIMCOMBO,
    IDC_RIMAPPLY,
    ID_FILE_OPEN = 2001,
    ID_FILE_GAME,
    ID_FILE_VGMSTREAM,
    ID_FILE_LZHAM,
    ID_FILE_NCT,
    ID_FILE_NCT_ENCODE,
    ID_TOOLS_SAVE_EDITOR,
    ID_TOOLS_SHOW_SOURCE,
    ID_TOOLS_IMPORT,
    ID_FILE_OPEN_PICTURE,
    ID_FILE_CONVERT,
    ID_FILE_EXPORT_SELECTED,
    ID_FILE_EXTRACT_SELECTED,
    ID_FILE_EXIT,
    ID_FILE_SAVE_PENDING,
    ID_VIEW_ALL,
    ID_VIEW_TEX,
    ID_VIEW_MODEL,
    ID_VIEW_SOUND,
    ID_THEME_DARK,
    ID_THEME_LIGHT,
    ID_HELP_ABOUT,
    WM_APP_DONE = WM_APP + 1,
    WM_APP_PROGRESS = WM_APP + 2,
    WM_APP_STARTUP = WM_APP + 3,
    WM_APP_EXPORTED = WM_APP + 4,
    ID_CTX_EXTRACT = 2900,
    ID_CTX_CONVERT,
    ID_CTX_SAVE,
    ID_CTX_SEL_EXTRACT,
    ID_CTX_SEL_CONVERT,
};

// ----------------------------------------------------------------- theme

struct Palette {
    COLORREF windowBg;
    COLORREF controlBg;
    COLORREF text;
    COLORREF dimText;
    COLORREF line;
    COLORREF previewBg;
};

static const Palette kLight = {
    RGB(0xF5, 0xF5, 0xF7),   // windowBg
    RGB(0xFF, 0xFF, 0xFF),   // controlBg
    RGB(0x1A, 0x1A, 0x1A),   // text
    RGB(0x5A, 0x5A, 0x60),   // dimText
    RGB(0xD0, 0xD0, 0xD6),   // line
    RGB(0xEC, 0xEC, 0xF0),   // previewBg
};
static const Palette kDark = {
    RGB(0x1E, 0x1E, 0x22),
    RGB(0x26, 0x26, 0x2B),
    RGB(0xEC, 0xEC, 0xF0),
    RGB(0xA0, 0xA0, 0xAA),
    RGB(0x3A, 0x3A, 0x42),
    RGB(0x18, 0x18, 0x1C),
};

static bool g_dark = false;
static HBRUSH g_bgBrush = nullptr;
static HBRUSH g_controlBrush = nullptr;

static const Palette& pal() { return g_dark ? kDark : kLight; }

static HINSTANCE g_inst;
static HWND g_main, g_tree, g_status, g_searchBox, g_searchLabel, g_progress;
static std::set<HTREEITEM> g_multi;      // the rows picked with Ctrl / Shift + click
static HTREEITEM g_anchor = nullptr;     // where a Shift + click range starts
// the column strip over the tree, Total Commander style: click to sort,
// click again to turn the order round
static HWND g_sortBtn[4];
static int g_sortKey = 0;          // 0 name, 1 extension, 2 size, 3 date
static bool g_sortDesc = false;
static HWND g_saveEdBtn, g_tooltip;
// the Frosty-style tool buttons beside it: open a game, export, import
static HWND g_tbOpen, g_tbExport, g_tbImport;
static HIMAGELIST g_treeImages;
static HFONT g_treeFont, g_headerFont;
static const nfsnl::Image* resourceImage(int id);
static int treeIconFor(const std::string& path);
static void drawImageAlpha(HDC hdc, const nfsnl::Image& img, int x, int y, int w, int h, COLORREF over);
static std::vector<Entry> g_all;
static nfsnl::Library g_lib;
static const nfsnl::GameProfile* g_profile = nullptr;
static std::string g_gameFolder;
static std::map<HTREEITEM, int> g_itemToEntry;
static std::map<HTREEITEM, std::string> g_itemToDir;   // folder rows -> their path

// A bank shows the sounds it holds as children in the tree. The children are
// not library entries - they live inside one bank - so they are mapped
// separately: tree item -> (which bank asset, which sound in it).
struct SoundRef { int entry = -1; int index = -1; };
static std::map<HTREEITEM, SoundRef> g_itemToSound;
static std::map<HTREEITEM, bool> g_bankExpanded;
static std::string g_filter = "all";
// What is typed in the search box above the tree, lower-cased once so the
// filter does not lower-case 72,000 paths on every keystroke.
static std::string g_search;
static const UINT_PTR IDT_SEARCH = 42;
static std::atomic<bool> g_busy{false};

// Progress text is handed to the window as an owned copy. It used to be a
// shared std::string written by the worker while the window read it, which is
// a data race: harmless-looking until indexing got fast enough in 0.5 to post
// updates in a burst, at which point the window could read a string whose
// buffer was being reallocated underneath it.
// How far indexing has got, 0..1000, or -1 before anything is known. The
// worker writes it and the window reads it when the text arrives, so the bar
// and the words it sits next to always describe the same moment.
static std::atomic<int> g_progressPermille{-1};

static void postProgress(const std::string& text) {
    std::string* copy = new std::string(text);
    if (!PostMessageW(g_main, WM_APP_PROGRESS, 0, (LPARAM)copy)) delete copy;
}

static void postProgress(const std::string& text, size_t done, size_t total) {
    g_progressPermille.store(total ? (int)((done * 1000) / total) : -1);
    postProgress(text);
}

// preview
static nfsnl::Image g_preview;
static bool g_previewWave = false;   // g_preview is a sound's waveform, not a texture
static std::string g_previewText;
static RECT g_previewRect{};

// 3D viewer - shares the preview pane; the model replaces the still image
static nfsnl::Model g_viewModel;
static nfsnl::RenderView g_view;
static std::vector<nfsnl::Image> g_viewTexStore;       // one per mesh, may be empty
static std::vector<const nfsnl::Image*> g_viewTextures;
static std::map<std::string, int> g_texCache;          // asset path -> g_viewTexStore
// pictures imported this session, by asset path: the 3D view shows these in
// place of what is in the game files, so a new texture can be seen on the car
static std::map<std::string, nfsnl::Image> g_texOverride;
// Imported textures and models, shown in the tool and waiting for the Save
// button: the new bytes of each asset (as the game reads them, before any .z
// wrapper), by asset path.
struct PendingSave { size_t entry = 0; Bytes bytes; std::string what; };
static std::map<std::string, PendingSave> g_pending;
static HWND g_tbSave, g_tbNewCar;
static void updateToolbarState();
static nfsnl::Image g_viewFrame;
static bool g_viewActive = false;
static bool g_viewDirty = true;
// the OpenGL view (see the GPU viewer section)
static HWND g_glWnd = nullptr;
static HDC g_glDC = nullptr;
static HGLRC g_glRC = nullptr;
static bool g_glOk = false;
static std::vector<GLuint> g_glTex;        // one per g_viewTexStore image
static std::vector<nfsnl::AlphaStats> g_glTexAlpha;   // and how clear each one is
// Real Racing 3's shader ramps baked to sphere maps, and which one each part
// takes (-1: none) - drawn as an added reflection/highlight pass
static std::vector<GLuint> g_glMatcap;
static std::vector<int> g_meshMatcap;
static int g_shaderParts = 0;
static GLuint g_glLists = 0;
static size_t g_glListCount = 0;
static bool g_glListsDirty = true;
static bool g_glListsWithColour = false;
static GLuint g_glFont = 0;
static void glFreeModel();
static void glUploadTextures();
static void glUploadShaders();
static RECT g_viewRect{};        // where the model is drawn
static RECT g_viewBarRect{};     // the strip of controls under it
static HWND g_posLabel, g_posX, g_posY, g_posZ, g_lodCombo, g_resetBtn, g_texCheck, g_vcolCheck;
static HWND g_kitCombo;          // body kit: Stock, All parts, Kit B ...
static HWND g_paintCombo;        // NFS Shift / Undercover: the car's paint (texture_car_<name>_01 ...)
// No Limits rim paint, under a wheel texture: CAR COLOR [the game's rim
// colours] [Put into texture]
static HWND g_rimLabel, g_rimCombo, g_rimApply;
static std::vector<nfsnl::NlRimColour> g_rimColours;
static bool g_rimColoursRead = false;
static nfsnl::Image g_rimBase;              // the texture as the game has it
static std::vector<uint8_t> g_rimMask;      // the texels the tinted meshes use
static std::string g_rimPath;               // the texture being painted
static std::string g_rimName;               // the colour picked
static float g_rimRgb[3] = { 1, 1, 1 };
static bool g_rimHave = false;
static int g_rimSel = 0;
static COLORREF g_rimCustom[16];
static HWND g_animBtn;           // plays a car's moving parts (RR3 rear wings)
static bool g_animPlaying = false;
static float g_animT = 0;        // ms into the model's animation
static DWORD g_animStart = 0;
static const UINT_PTR IDT_ANIM = 44;
static std::string g_kitLetters; // the kit letters of the open model
// The one combo box serves two jobs: LOD levels for a model, and the list of
// sounds inside a bank.
enum ComboMode { COMBO_NONE = 0, COMBO_LOD = 1, COMBO_SOUNDS = 2, COMBO_FSB = 3 };
static int g_comboMode = COMBO_NONE;
static nfsnl::Bank g_bank;
static Bytes g_bankData;

static int g_dragMode = 0;       // 0 none, 1 orbit, 2 pan, 3 move the model
static POINT g_dragFrom{};
static bool g_suppressPosEdit = false;

// text view - any asset with no picture and no model is shown as text
static HWND g_textBox;
static bool g_textActive = false;

// sound player
static HWND g_playBtn, g_stopBtn, g_prevBtn, g_backBtn, g_fwdBtn, g_nextBtn, g_seek, g_timeLabel;
static Bytes g_playable;         // a ready-to-play WAV, empty when there is none
static bool g_soundActive = false;

// ----------------------------------------------------------------- helpers

static std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// ---- log file -------------------------------------------------------------
//
// A tool that cannot be run on the machine it is written on needs to say
// where it got to. Every step of opening a game is logged next to the exe, and
// a crash logs the fault address, so one file turns "it crashed" into a line
// number.
static std::string logPath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring w(buf, n);
    size_t slash = w.find_last_of(L"\\/");
    if (slash != std::wstring::npos) w.erase(slash);
    return narrow(w) + "\\MonkeyTool.log";
}

static void logLine(const char* fmt, ...) {
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    FILE* f = nfsnl::openFile(logPath(), "a");
    if (!f) return;
    fprintf(f, "%02d:%02d:%02d.%03d  %s\n", st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds, text);
    fclose(f);
}

static LONG WINAPI crashHandler(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord) {
        logLine("CRASH code 0x%08lX at %p",
                (unsigned long)ep->ExceptionRecord->ExceptionCode,
                ep->ExceptionRecord->ExceptionAddress);
    } else {
        logLine("CRASH (no record)");
    }
    MessageBoxW(nullptr,
        L"Monkey Tool hit a bug and has to close.\n\n"
        L"MonkeyTool.log, next to the program, says where it got to. "
        L"Sending that file makes the fault findable.",
        L"Monkey Tool", MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void setStatus(const std::string& s) {
    SetWindowTextW(g_status, widen(s).c_str());
}

// ---- theme persistence (HKCU\Software\MonkeyTool\DarkTheme) ----

static void loadThemePref() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\MonkeyTool", 0, KEY_READ, &k)
        == ERROR_SUCCESS) {
        DWORD v = 0, sz = sizeof(v), type = 0;
        if (RegQueryValueExW(k, L"DarkTheme", nullptr, &type, (LPBYTE)&v, &sz)
            == ERROR_SUCCESS && type == REG_DWORD)
            g_dark = v != 0;
        RegCloseKey(k);
        return;
    }
    // no preference yet: follow the Windows app theme
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD v = 1, sz = sizeof(v), type = 0;
        if (RegQueryValueExW(k, L"AppsUseLightTheme", nullptr, &type, (LPBYTE)&v, &sz)
            == ERROR_SUCCESS && type == REG_DWORD)
            g_dark = (v == 0);
        RegCloseKey(k);
    }
}

static void saveThemePref() {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\MonkeyTool", 0, nullptr, 0,
                        KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        DWORD v = g_dark ? 1 : 0;
        RegSetValueExW(k, L"DarkTheme", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        RegCloseKey(k);
    }
}

// Windows paints the menu bar itself and ignores WM_CTLCOLOR, so the items
// are owner-drawn to get dark text/background. The label for each item is
// kept here and referenced by dwItemData.
static std::vector<std::wstring> g_menuLabels;

static UINT_PTR menuLabelId(const std::wstring& text) {
    g_menuLabels.push_back(text);
    return (UINT_PTR)g_menuLabels.size();   // 1-based; 0 means "no label"
}

// Undocumented, but this is the only way to paint the band the menu bar sits
// on; the struct is the shell's own layout.
#ifndef WM_UAHDRAWMENU
#define WM_UAHDRAWMENU 0x0091
#endif
struct UAHMENU { HMENU hmenu; HDC hdc; DWORD dwFlags; };

static void themeMenu(HMENU menu, bool top) {
    MENUINFO mi{};
    mi.cbSize = sizeof(mi);
    mi.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
    mi.hbrBack = g_controlBrush;
    SetMenuInfo(menu, &mi);

    int n = GetMenuItemCount(menu);
    for (int i = 0; i < n; ++i) {
        MENUITEMINFOW mii{};
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_FTYPE | MIIM_SUBMENU | MIIM_DATA;
        if (!GetMenuItemInfoW(menu, i, TRUE, &mii)) continue;
        if (mii.fType & MFT_SEPARATOR) continue;

        if (!mii.dwItemData) {
            wchar_t buf[128] = {0};
            MENUITEMINFOW get{};
            get.cbSize = sizeof(get);
            get.fMask = MIIM_STRING;
            get.dwTypeData = buf;
            get.cch = 127;
            GetMenuItemInfoW(menu, i, TRUE, &get);
            mii.dwItemData = menuLabelId(buf);
        }
        mii.fMask = MIIM_FTYPE | MIIM_DATA;
        mii.fType = MFT_OWNERDRAW;
        SetMenuItemInfoW(menu, i, TRUE, &mii);

        if (mii.hSubMenu) themeMenu(mii.hSubMenu, false);
    }
    (void)top;
}

static void applyTheme() {
    if (g_bgBrush) DeleteObject(g_bgBrush);
    if (g_controlBrush) DeleteObject(g_controlBrush);
    g_bgBrush = CreateSolidBrush(pal().windowBg);
    g_controlBrush = CreateSolidBrush(pal().controlBg);
    themeTitleBar(g_main, g_dark);
    if (g_saveEdBtn) InvalidateRect(g_saveEdBtn, nullptr, TRUE);

    if (g_tree) {
        TreeView_SetBkColor(g_tree, pal().controlBg);
        TreeView_SetTextColor(g_tree, pal().text);
        TreeView_SetLineColor(g_tree, pal().line);
        // dark scrollbars / expanders where the OS supports it
        SetWindowTheme(g_tree, g_dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        InvalidateRect(g_tree, nullptr, TRUE);
    }
    if (g_main) {
        HMENU m = GetMenu(g_main);
        if (m) {
            CheckMenuRadioItem(m, ID_THEME_LIGHT, ID_THEME_DARK,
                               g_dark ? ID_THEME_DARK : ID_THEME_LIGHT, MF_BYCOMMAND);
            themeMenu(m, true);
            DrawMenuBar(g_main);
        }
        InvalidateRect(g_main, nullptr, TRUE);
    }
    if (g_status) InvalidateRect(g_status, nullptr, TRUE);

    // the viewer paints its own background, so it has to be told as well
    if (g_lodCombo) {
        const wchar_t* t = g_dark ? L"DarkMode_CFD" : L"Explorer";
        SetWindowTheme(g_lodCombo, t, nullptr);
        SetWindowTheme(g_kitCombo, t, nullptr);
        SetWindowTheme(g_paintCombo, t, nullptr);
        if (g_rimCombo) SetWindowTheme(g_rimCombo, t, nullptr);
        if (g_rimApply) SetWindowTheme(g_rimApply, g_dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        SetWindowTheme(g_resetBtn, g_dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        // A themed check box draws its label in the theme's own (black)
        // colour whatever WM_CTLCOLORSTATIC says, which is why "Textures"
        // vanished on the dark pane; unthemed, it takes the colour given.
        for (HWND c : { g_texCheck, g_vcolCheck })
            if (c) { SetWindowTheme(c, L"", L""); InvalidateRect(c, nullptr, TRUE); }
    }
    g_view.background[0] = (float)GetRValue(pal().previewBg);
    g_view.background[1] = (float)GetGValue(pal().previewBg);
    g_view.background[2] = (float)GetBValue(pal().previewBg);
    g_viewDirty = true;
}

// Produce one asset's bytes. Nothing is unpacked ahead of time and nothing
// is written: the library reads the one cabinet the asset lives in and hands
// back the slice.
static bool readAsset(const Entry& e, Bytes& out, std::string* error = nullptr) {
    // the index is being rebuilt on the worker thread; the entry it refers to
    // may no longer exist
    if (g_busy) {
        if (error) *error = "still reading the game's archives";
        return false;
    }
    // an import not saved yet is what the tool shows
    auto pend = g_pending.find(e.path);
    if (pend != g_pending.end()) { out = pend->second.bytes; return true; }
    return g_lib.read(e.lib, out, error);
}

// Which file on disk an asset comes out of: its .pack (and the cabinet in it,
// or the separate .cab that cabinet lives in), or the loose file itself. Shown
// under every preview so a problem asset can be traced to the one file worth
// sending.
static std::string sourceFileOf(const Entry& e, std::string* detail = nullptr) {
    if (g_busy || e.lib >= g_lib.entries.size()) return std::string();
    const nfsnl::LibraryEntry& le = g_lib.entries[e.lib];
    if (le.archive < 0 || (size_t)le.archive >= g_lib.archives.size()) return std::string();
    const nfsnl::Archive& a = g_lib.archives[le.archive];
    if (detail) {
        detail->clear();
        if (!le.whole && le.cabinet < a.cabinets.size()) {
            char buf[160];
            bool ext = (a.cabinets[le.cabinet].flags & nfsnl::CAB_EXTERNAL) != 0;
            snprintf(buf, sizeof(buf), "cabinet %u%s", (unsigned)le.cabinet,
                     ext ? " - stored separately as " : "");
            *detail = buf;
            if (ext)
                *detail += nfsnl::stripExtension(nfsnl::baseName(a.path)) + "\\" +
                           std::to_string(le.cabinet) + ".cab";
        }
    }
    return a.path;
}

static std::string sourceLine(const Entry& e) {
    std::string detail, file = sourceFileOf(e, &detail);
    if (file.empty()) return std::string();
    std::string line = "\r\nIn " + nfsnl::baseName(file);
    if (!detail.empty()) line += " (" + detail + ")";
    line += "  -  Tools > Show the file this asset is in";
    return line;
}

// A scratch file, for the few places that need a path rather than bytes -
// handing a sound to vgmstream, for instance. It is deleted the moment the
// helper is done with it, so nothing accumulates.
struct ScratchFile {
    std::string path;
    explicit ScratchFile(const std::string& suffix) {
        wchar_t buf[MAX_PATH];
        GetTempPathW(MAX_PATH, buf);
        wchar_t name[64];
        wsprintfW(name, L"monkeytool_%lu", GetTickCount());
        path = narrow(std::wstring(buf) + name) + suffix;
    }
    bool write(const Bytes& data) { return nfsnl::writeFile(path, data); }
    ~ScratchFile() {
        if (!path.empty()) DeleteFileW(widen(path).c_str());
    }
};

static bool pickFolder(HWND owner, const wchar_t* title, std::string& out) {
    BROWSEINFOW bi{};
    wchar_t display[MAX_PATH] = {0};
    bi.hwndOwner = owner;
    bi.pszDisplayName = display;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;
    wchar_t path[MAX_PATH] = {0};
    bool ok = SHGetPathFromIDListW(pidl, path) != FALSE;
    CoTaskMemFree(pidl);
    if (ok) out = narrow(path);
    return ok;
}

// recursive scan for .pack / .cab
// A folder with the same name as a .pack beside it holds that pack's external
// cabinets (<pack>\\0.cab, 1.cab ...). They are read through the pack's own
// manifest, so indexing them again as stand-alone archives would only produce
// a second, nameless copy of the same assets - and a pile of warnings.
static void scanArchives(const std::string& dir, std::vector<std::string>& out,
                         bool cabsBelongToPack = false) {
    std::wstring pattern = widen(dir) + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::string full = dir + "\\" + narrow(name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            DWORD attr = GetFileAttributesW(widen(full + ".pack").c_str());
            bool owned = attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
            scanArchives(full, out, owned);
        } else {
            std::string e = nfsnl::extensionOf(full);
            if (e == "pack" || (e == "cab" && !cabsBelongToPack)) out.push_back(full);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Real Racing 3 has no archives: .depot is a flat folder of loose files. Every
// one of them is an asset, so they are all taken and the tree is built from
// the paths they already have.
static void scanLooseFiles(const std::string& dir, const std::string& prefix,
                           std::vector<std::pair<std::string, std::string>>& out) {
    if (out.size() > 400000) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(widen(dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::string leaf = narrow(name);
        std::string full = dir + "\\" + leaf;
        std::string rel = prefix.empty() ? leaf : prefix + "/" + leaf;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            scanLooseFiles(full, rel, out);
        } else {
            out.push_back({full, rel});
        }
    } while (FindNextFileW(h, &fd) && out.size() <= 400000);
    FindClose(h);
}

// ----------------------------------------------------------------- tree

static bool passesFilter(const Entry& e) {
    if (g_filter != "all" && e.kind != g_filter) return false;
    if (g_search.empty()) return true;
    // The search matches the file's own name rather than the whole path, so
    // typing "bmw" finds the BMW assets instead of everything that happens to
    // sit under a folder with those letters in it. Several words all have to
    // match, in any order.
    std::string leaf = e.path;
    size_t slash = leaf.find_last_of('/');
    if (slash != std::string::npos) leaf.erase(0, slash + 1);
    for (char& c : leaf) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    size_t at = 0;
    while (at < g_search.size()) {
        size_t end = g_search.find(' ', at);
        if (end == std::string::npos) end = g_search.size();
        if (end > at && leaf.find(g_search.substr(at, end - at)) == std::string::npos)
            return false;
        at = end + 1;
    }
    return true;
}

static void updateSortLabels() {
    const wchar_t* labels[4] = { L"Name", L"Ext", L"Size", L"Date" };
    for (int k = 0; k < 4; ++k) {
        if (!g_sortBtn[k]) continue;
        std::wstring t = labels[k];
        if (k == g_sortKey) t += g_sortDesc ? L"  \u2193" : L"  \u2191";
        SetWindowTextW(g_sortBtn[k], t.c_str());
    }
}

static void rebuildTree() {
    SendMessageW(g_tree, WM_SETREDRAW, FALSE, 0);
    g_multi.clear();
    g_anchor = nullptr;
    TreeView_DeleteAllItems(g_tree);
    g_itemToEntry.clear();
    g_itemToDir.clear();
    g_itemToSound.clear();
    g_bankExpanded.clear();

    std::map<std::string, HTREEITEM> dirs;
    std::function<HTREEITEM(const std::string&)> ensureDir =
        [&](const std::string& d) -> HTREEITEM {
            if (d.empty()) return TVI_ROOT;
            auto it = dirs.find(d);
            if (it != dirs.end()) return it->second;
            size_t slash = d.find_last_of('/');
            std::string parent = slash == std::string::npos ? "" : d.substr(0, slash);
            std::string leaf = slash == std::string::npos ? d : d.substr(slash + 1);
            HTREEITEM parentItem = ensureDir(parent);
            std::wstring wl = widen(leaf);
            TVINSERTSTRUCTW ins{};
            ins.hParent = parentItem;
            ins.hInsertAfter = TVI_LAST;
            ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
            ins.item.pszText = (LPWSTR)wl.c_str();
            ins.item.lParam = -1;
            ins.item.iImage = 0;
            ins.item.iSelectedImage = 1;
            HTREEITEM h = (HTREEITEM)SendMessageW(g_tree, TVM_INSERTITEMW, 0, (LPARAM)&ins);
            dirs[d] = h;
            g_itemToDir[h] = d;
            return h;
        };

    // what is shown, in the chosen order; folders first, by name, the way a
    // file manager lists them
    std::vector<int> order;
    order.reserve(g_all.size());
    std::vector<std::string> folders;
    for (int i = 0; i < (int)g_all.size(); ++i) {
        if (!passesFilter(g_all[i])) continue;
        order.push_back(i);
        size_t slash = g_all[i].path.find_last_of('/');
        if (slash != std::string::npos) folders.push_back(g_all[i].path.substr(0, slash));
    }
    auto lowerLeaf = [](const std::string& p) {
        std::string l = p.substr(p.find_last_of('/') + 1);
        for (char& c : l) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return l;
    };
    std::vector<std::string> keyName(g_all.size());
    for (int i : order) keyName[i] = lowerLeaf(g_all[i].path);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const Entry& x = g_all[a];
        const Entry& y = g_all[b];
        int c = 0;
        if (g_sortKey == 1) {
            std::string ex = nfsnl::extensionOf(keyName[a]), ey = nfsnl::extensionOf(keyName[b]);
            c = ex < ey ? -1 : ex > ey ? 1 : 0;
        } else if (g_sortKey == 2) {
            c = x.size < y.size ? -1 : x.size > y.size ? 1 : 0;
        } else if (g_sortKey == 3) {
            c = x.time < y.time ? -1 : x.time > y.time ? 1 : 0;
        }
        if (c == 0) c = keyName[a] < keyName[b] ? -1 : keyName[a] > keyName[b] ? 1 : 0;
        return g_sortDesc ? c > 0 : c < 0;
    });
    std::sort(folders.begin(), folders.end(), [](const std::string& a, const std::string& b) {
        return _stricmp(a.c_str(), b.c_str()) < 0;
    });
    folders.erase(std::unique(folders.begin(), folders.end()), folders.end());
    for (const std::string& d : folders) ensureDir(d);

    int shown = 0;
    for (int i : order) {
        const Entry& e = g_all[i];
        size_t slash = e.path.find_last_of('/');
        std::string dir = slash == std::string::npos ? "" : e.path.substr(0, slash);
        std::string leaf = slash == std::string::npos ? e.path : e.path.substr(slash + 1);
        HTREEITEM parent = ensureDir(dir);
        std::wstring wl = widen(leaf);
        TVINSERTSTRUCTW ins{};
        ins.hParent = parent;
        ins.hInsertAfter = TVI_LAST;
        ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
        ins.item.pszText = (LPWSTR)wl.c_str();
        ins.item.lParam = i;
        ins.item.iImage = ins.item.iSelectedImage = treeIconFor(e.path);
        // a bank gets an expander: its sounds are filled in when it is opened
        bool isBank = nfsnl::extensionOf(e.path) == "bnk";
        if (isBank) {
            ins.item.mask |= TVIF_CHILDREN;
            ins.item.cChildren = 1;
        }
        HTREEITEM h = (HTREEITEM)SendMessageW(g_tree, TVM_INSERTITEMW, 0, (LPARAM)&ins);
        g_itemToEntry[h] = i;
        shown++;
    }
    // A search that hides most of the tree is no use if the survivors are
    // still folded away inside folders, so the results are opened up - unless
    // there are so many that expanding them all would be the slow part.
    if (!g_search.empty() && shown > 0 && shown <= 800)
        for (auto& kv : dirs) TreeView_Expand(g_tree, kv.second, TVE_EXPAND);

    SendMessageW(g_tree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_tree, nullptr, TRUE);

    char buf[256];
    if (!g_search.empty())
        snprintf(buf, sizeof(buf), "%d asset(s) match \"%s\", of %d.",
                 shown, g_search.c_str(), (int)g_all.size());
    else
        snprintf(buf, sizeof(buf), "%d asset(s) shown of %d extracted.", shown,
                 (int)g_all.size());
    setStatus(buf);
}

// ----------------------------------------------------------------- 3D viewer

static void showViewerControls(bool show) {
    int cmd = show ? SW_SHOW : SW_HIDE;
    ShowWindow(g_lodCombo, cmd);
    ShowWindow(g_posLabel, cmd);
    ShowWindow(g_posX, cmd);
    ShowWindow(g_posY, cmd);
    ShowWindow(g_posZ, cmd);
    ShowWindow(g_resetBtn, cmd);
    ShowWindow(g_texCheck, cmd);
    ShowWindow(g_vcolCheck, cmd);
    ShowWindow(g_kitCombo, show && !g_kitLetters.empty() ? SW_SHOW : SW_HIDE);
    ShowWindow(g_paintCombo, show && SendMessageW(g_paintCombo, CB_GETCOUNT, 0, 0) > 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_animBtn, show && nfsnl::modelAnimDuration(g_viewModel) > 0 ? SW_SHOW : SW_HIDE);
}

static void stopAnimation() {
    if (g_main) KillTimer(g_main, IDT_ANIM);
    g_animPlaying = false;
    g_animT = 0;
    if (g_animBtn) SetWindowTextW(g_animBtn, L"Animate");
}

static void setPosBoxes() {
    wchar_t b[32];
    g_suppressPosEdit = true;
    swprintf(b, 32, L"%.3f", g_view.modelPos[0]); SetWindowTextW(g_posX, b);
    swprintf(b, 32, L"%.3f", g_view.modelPos[1]); SetWindowTextW(g_posY, b);
    swprintf(b, 32, L"%.3f", g_view.modelPos[2]); SetWindowTextW(g_posZ, b);
    g_suppressPosEdit = false;
}

static float readPosBox(HWND h) {
    wchar_t b[64];
    GetWindowTextW(h, b, 64);
    return (float)wcstod(b, nullptr);
}

// The model names its textures with a path relative to itself, e.g.
// "../../../textures/cars/x/texture_x.sba". Resolve that against the assets
// that were unpacked: first by the tail of the path, then by file name alone,
// because a few models point at a folder layout the pack does not repeat.
static int findAssetOne(const std::string& ref);

// A model's reference to a texture; NFS Edge's say .m3g for what the build
// ships as .sba, so the other texture extensions are tried after the name
// as written.
static int findAsset(const std::string& ref) {
    std::vector<std::string> c = nfsnl::textureRefCandidates(ref);
    if (c.size() <= 1) return findAssetOne(ref);
    for (const std::string& x : c) {
        int i = findAssetOne(x);
        if (i >= 0) return i;
    }
    return -1;
}

// Every asset by its file name: a lookup that used to walk all 70 000
// assets per texture is now one map probe. Rebuilt when the list changes.
static const std::vector<int>* assetsNamed(const std::string& leaf) {
    static std::unordered_map<std::string, std::vector<int>> byLeaf;
    static const void* builtFor = nullptr;
    static size_t builtSize = 0;
    if (builtFor != (const void*)g_all.data() || builtSize != g_all.size()) {
        byLeaf.clear();
        byLeaf.reserve(g_all.size());
        for (size_t i = 0; i < g_all.size(); ++i) {
            const std::string& a = g_all[i].path;
            byLeaf[a.substr(a.find_last_of('/') + 1)].push_back((int)i);
        }
        builtFor = g_all.data();
        builtSize = g_all.size();
    }
    auto it = byLeaf.find(leaf);
    return it == byLeaf.end() ? nullptr : &it->second;
}

static int findAssetOne(const std::string& ref) {
    std::string p = ref;
    while (p.compare(0, 3, "../") == 0) p.erase(0, 3);
    if (p.empty()) return -1;
    std::string leaf = p.substr(p.find_last_of('/') + 1);
    if (const std::vector<int>* same = assetsNamed(leaf)) {
        for (int i : *same) {
            const std::string& a = g_all[i].path;
            if (a.size() >= p.size() && a.compare(a.size() - p.size(), p.size(), p) == 0) return i;
        }
        return same->front();
    }
    // iOS file names are not case-sensitive: Real Racing's models name
    // Track_Grass.PNG for a file shipped as track_grass.pvr
    {
        static std::unordered_map<std::string, int> lowerLeaf;
        static const void* lowFor = nullptr;
        static size_t lowSize = 0;
        if (lowFor != (const void*)g_all.data() || lowSize != g_all.size()) {
            lowerLeaf.clear();
            for (size_t i = 0; i < g_all.size(); ++i) {
                std::string l = g_all[i].path.substr(g_all[i].path.find_last_of('/') + 1);
                for (char& c : l) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                lowerLeaf.emplace(l, (int)i);
            }
            lowFor = g_all.data();
            lowSize = g_all.size();
        }
        std::string l = leaf;
        for (char& c : l) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        auto it = lowerLeaf.find(l);
        if (it != lowerLeaf.end()) return it->second;
    }
    // Real Racing 2 names a texture's high-detail copy <name>_hd.pvr; a build
    // may ship only one of the pair, so either stands in for the other
    size_t dot = leaf.rfind('.');
    if (dot != std::string::npos && dot > 3) {
        std::string alt = leaf.compare(dot - 3, 3, "_hd") == 0
                              ? leaf.substr(0, dot - 3) + leaf.substr(dot)
                              : leaf.substr(0, dot) + "_hd" + leaf.substr(dot);
        if (const std::vector<int>* same = assetsNamed(alt)) return same->front();
    }
    return -1;
}

// One texture to decode: the file's bytes and those of its alpha companions,
// read on the UI thread (the archives are not thread-safe), decoded on a pool.
struct TexJob {
    Bytes raw;
    std::vector<Bytes> alphas;
    nfsnl::Image img;
    bool ok = false;
    int paintMode = 0;            // No Limits: the car's paint over it (#paint: refs)
    float paintRgb[3] = { 1, 1, 1 };
};
struct TexPool {
    std::vector<TexJob>* jobs;
    std::atomic<size_t> next{0};
    int maxSide;
};

static void shrinkForViewer(nfsnl::Image& img, int maxSide) {
    if (img.width > maxSide || img.height > maxSide) {
        float k = (float)maxSide / (float)std::max(img.width, img.height);
        img = nfsnl::resizeRgba(nfsnl::toRgba(img), std::max(1, (int)(img.width * k)),
                                std::max(1, (int)(img.height * k)));
    }
}

static DWORD WINAPI texDecodeProc(LPVOID param) {
    TexPool* pool = static_cast<TexPool*>(param);
    for (;;) {
        size_t i = pool->next.fetch_add(1);
        if (i >= pool->jobs->size()) break;
        TexJob& job = (*pool->jobs)[i];
        if (!nfsnl::decodeTextureFile(job.raw.data(), job.raw.size(), job.img)) continue;
        for (const Bytes& araw : job.alphas) {
            nfsnl::Image alpha;
            if (nfsnl::decodeTextureFile(araw.data(), araw.size(), alpha) &&
                nfsnl::applyEtcAlpha(job.img, alpha))
                break;
        }
        if (job.paintMode) nfsnl::nlPaintImage(job.img, job.paintMode, job.paintRgb);
        shrinkForViewer(job.img, pool->maxSide);
        job.ok = true;
        Bytes().swap(job.raw);
        job.alphas.clear();
    }
    return 0;
}

static void decodeTexJobs(std::vector<TexJob>& jobs, int maxSide) {
    if (jobs.empty()) return;
    TexPool pool;
    pool.jobs = &jobs;
    pool.maxSide = maxSide;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    size_t n = std::min<size_t>(std::max<DWORD>(1, si.dwNumberOfProcessors), 8);
    n = std::min(n, jobs.size());
    std::vector<HANDLE> threads;
    for (size_t t = 1; t < n; ++t) {
        HANDLE h = CreateThread(nullptr, 0, texDecodeProc, &pool, 0, nullptr);
        if (h) threads.push_back(h);
    }
    texDecodeProc(&pool);                 // the UI thread works too
    for (HANDLE h : threads) {
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }
}

static void loadViewerTextures() {
    g_viewTexStore.clear();
    g_texCache.clear();
    g_viewTextures.assign(g_viewModel.meshes.size(), nullptr);
    // A car uses a dozen textures; a Real Racing 3 track uses five hundred.
    // All of them are loaded, and when there are many they are shrunk for
    // the viewer (the files and the exports are untouched), so a whole
    // circuit fits in memory. The files are read in order, then decoded on
    // every core.
    std::set<std::string> distinct;
    for (const auto& mesh : g_viewModel.meshes)
        if (!mesh.texture.empty()) distinct.insert(mesh.texture);
    const size_t kMaxTextures = 4096;
    int maxSide = distinct.size() > 64 ? 256 : 4096;
    // texture name -> what it becomes: a store index (images in hand) or a job
    struct Pending { int store = -1; int job = -1; };
    std::map<std::string, Pending> plan;
    std::vector<nfsnl::Image> ready;
    std::vector<TexJob> jobs;
    size_t done = 0;
    for (const std::string& t : distinct) {
        if (ready.size() + jobs.size() >= kMaxTextures) break;
        Pending& p = plan[t];
        // a texture the model file carries itself (an M3G track's atlas)
        if (t.compare(0, 5, "#img:") == 0) {
            size_t k = (size_t)atoi(t.c_str() + 5);
            if (k >= g_viewModel.images.size()) continue;
            nfsnl::Image img = g_viewModel.images[k];
            shrinkForViewer(img, maxSide);
            ready.push_back(std::move(img));
            p.store = (int)ready.size() - 1;
            continue;
        }
        if (distinct.size() > 64 && (++done % 25) == 0) {
            char b[96];
            snprintf(b, sizeof(b), "Reading textures %u / %u ...", (unsigned)done,
                     (unsigned)distinct.size());
            setStatus(b);
        }
        // No Limits paint: "#paint:<mode>:<rrggbb>:<texture>"
        int paintMode = 0;
        float paintRgb[3] = { 1, 1, 1 };
        std::string ref = t;
        if (t.compare(0, 7, "#paint:") == 0) {
            std::string base;
            if (nfsnl::nlParsePaintRef(t, paintMode, paintRgb, base)) ref = base;
            if (paintMode == 2) {
                nfsnl::Image img;
                nfsnl::nlPaintImage(img, 2, paintRgb);
                ready.push_back(std::move(img));
                p.store = (int)ready.size() - 1;
                continue;
            }
        }
        int idx = findAsset(ref);
        if (idx < 0 || (size_t)idx >= g_all.size()) continue;
        auto ov = g_texOverride.find(g_all[idx].path);
        if (ov != g_texOverride.end()) {
            nfsnl::Image img = ov->second;
            if (paintMode) nfsnl::nlPaintImage(img, paintMode, paintRgb);
            shrinkForViewer(img, maxSide);
            ready.push_back(std::move(img));
            p.store = (int)ready.size() - 1;
            continue;
        }
        TexJob job;
        if (!readAsset(g_all[idx], job.raw)) continue;
        // the alpha as a second texture: <name>_ETCAlpha (NFS Edge),
        // <name>_alpha (Real Racing 3's trees and sky)
        for (const std::string& comp : nfsnl::alphaCompanions(g_all[idx].path)) {
            int ai = findAssetOne(comp);
            Bytes araw;
            if (ai >= 0 && g_all[ai].path == comp && readAsset(g_all[ai], araw))
                job.alphas.push_back(std::move(araw));
        }
        job.paintMode = paintMode;
        memcpy(job.paintRgb, paintRgb, sizeof(paintRgb));
        jobs.push_back(std::move(job));
        p.job = (int)jobs.size() - 1;
    }
    if (jobs.size() > 8) {
        char b[96];
        snprintf(b, sizeof(b), "Decoding %u textures ...", (unsigned)jobs.size());
        setStatus(b);
    }
    decodeTexJobs(jobs, maxSide);
    // the store is filled once, so the pointers below never move
    g_viewTexStore.reserve(ready.size() + jobs.size());
    std::vector<int> jobAt(jobs.size(), -1);
    for (auto& img : ready) g_viewTexStore.push_back(std::move(img));
    for (size_t j = 0; j < jobs.size(); ++j)
        if (jobs[j].ok) {
            g_viewTexStore.push_back(std::move(jobs[j].img));
            jobAt[j] = (int)g_viewTexStore.size() - 1;
        }
    for (const auto& kv : plan) {
        int at = kv.second.store >= 0 ? kv.second.store
                 : kv.second.job >= 0 ? jobAt[kv.second.job] : -1;
        g_texCache[kv.first] = at;
    }
    for (size_t i = 0; i < g_viewModel.meshes.size(); ++i) {
        auto it = g_texCache.find(g_viewModel.meshes[i].texture);
        if (it != g_texCache.end() && it->second >= 0)
            g_viewTextures[i] = &g_viewTexStore[it->second];
    }
}

static void fillLodCombo() {
    SendMessageW(g_lodCombo, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_lodCombo, CB_ADDSTRING, 0, (LPARAM)L"All LODs");
    std::vector<std::string> lods;
    bool helpers = false;
    for (const auto& m : g_viewModel.meshes) {
        if (m.lod.empty()) continue;
        if (m.lod == "HELPERS") { helpers = true; continue; }
        if (std::find(lods.begin(), lods.end(), m.lod) == lods.end()) lods.push_back(m.lod);
    }
    // the detail levels first, then the other states a file carries
    // (DAMAGE, BONNETCAM), then the helpers
    std::sort(lods.begin(), lods.end(), [](const std::string& a, const std::string& b) {
        bool la = a.compare(0, 3, "LOD") == 0, lb = b.compare(0, 3, "LOD") == 0;
        if (la != lb) return la;
        return a < b;
    });
    // the collision hulls and decal shell go last: they are never the view
    // anyone wants first, and sorted by name they came before LOD00
    if (helpers) lods.push_back("HELPERS");
    for (const auto& l : lods)
        SendMessageW(g_lodCombo, CB_ADDSTRING, 0, (LPARAM)widen(l).c_str());
    // Default to the highest LOD on its own: drawn together the LODs sit
    // inside each other and the view looks like a broken mesh.
    int sel = 0;
    if (!lods.empty() && lods[0] != "HELPERS") {
        g_view.lodFilter = lods[0];
        sel = 1;
    } else {
        g_view.lodFilter.clear();
    }
    SendMessageW(g_lodCombo, CB_SETCURSEL, sel, 0);
}

// The body kits a No Limits car carries. Stock is the car as it ships; the
// letters are the kits and tuning parts, which drawn all at once put three
// bumpers and two wings on one car.
static void fillKitCombo() {
    g_kitLetters = nfsnl::modelKits(g_viewModel);
    SendMessageW(g_kitCombo, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_kitCombo, CB_ADDSTRING, 0, (LPARAM)L"Stock");
    SendMessageW(g_kitCombo, CB_ADDSTRING, 0, (LPARAM)L"All parts");
    for (char c : g_kitLetters) {
        wchar_t b[16];
        swprintf(b, 16, L"Kit %c", (wchar_t)(c - 'a' + 'A'));
        SendMessageW(g_kitCombo, CB_ADDSTRING, 0, (LPARAM)b);
    }
    SendMessageW(g_kitCombo, CB_SETCURSEL, 0, 0);
    g_view.kit = "stock";
}

// ---- NFS Shift / Shift 2 (and Undercover where a car has them): the paint
// jobs. A car comes as texture_car_<name>.m3g and its liveries _01 ... _07,
// the body kit and the bonnet-camera bonnet the same way; the list swaps all
// three together.
struct PaintFamily { std::string prefix; };       // "texture_car_megane", "..._bodykit", "texture_bonnet_megane"
static std::vector<std::string> g_paintPrefix;    // per mesh: the family its texture belongs to, "" none
static std::vector<std::string> g_paintSuffixes;  // the list's entries: "", "_01", "_02" ...
static nfsnl::EaPhoneFiles eaPhoneFiles();

// ---- No Limits: a car's colours (its car setups) and a track's
// limited-time layers share the paint list
static std::string g_viewPath;            // the asset the viewer shows
enum { PAINT_NONE, PAINT_EA, PAINT_NL_CAR, PAINT_NL_LTS };
static int g_paintKind = PAINT_NONE;
struct NlSetupRef { std::string label, path; };
static std::vector<NlSetupRef> g_nlSetups;     // the list's entries after "textures"
static nfsnl::NlCarPaint g_nlPaint;            // what the viewer shows now
static std::string g_nlPaintLabel;
static std::vector<nfsnl::NlLtsGroup> g_ltsGroups;
static int g_ltsSel = 0;

static const std::vector<nfsnl::NlPaintColour>& nlColours() {
    static std::vector<nfsnl::NlPaintColour> list;
    static size_t forLib = (size_t)-1;
    if (forLib != g_all.size()) {
        forLib = g_all.size();
        list.clear();
        int ci = findAssetOne("data/colours/colours.sb");
        if (ci < 0) ci = findAssetOne("colours/colours.sb");
        Bytes raw, sb;
        if (ci >= 0 && readAsset(g_all[ci], raw) &&
            nfsnl::nlSbDecode(nfsnl::baseName(g_all[ci].path), raw.data(), raw.size(), sb))
            list = nfsnl::nlPaintColours(sb.data(), sb.size());
        logLine("No Limits colours: %zu", list.size());
    }
    return list;
}

// models/cars/<car>/<car>.sb3d -> <car>; "" for anything else
static std::string nlCarIdFor(const std::string& path) {
    if (nfsnl::extensionOf(path) != "sb3d" || path.find("cars/") == std::string::npos) return std::string();
    std::string leaf = nfsnl::stripExtension(nfsnl::baseName(path));
    if (leaf.compare(0, 6, "wheel_") == 0) return std::string();
    return leaf;
}

// data/car_setups/<car>/Bodykits/X_STOCK.sb ... : stock first, then the rest
static std::vector<NlSetupRef> nlCarSetups(const std::string& car) {
    std::vector<NlSetupRef> out;
    if (car.empty()) return out;
    std::string lowCar = car;
    for (char& c : lowCar) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    std::string want = "car_setups/" + lowCar + "/";
    for (const Entry& e : g_all) {
        if (nfsnl::extensionOf(e.path) != "sb") continue;
        std::string low = e.path;
        for (char& c : low) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (low.find(want) == std::string::npos) continue;
        std::string stem = nfsnl::stripExtension(nfsnl::baseName(e.path)), up = stem;
        for (char& c : up) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        std::string label;
        auto ends = [&](const char* t) {
            size_t n = strlen(t);
            return up.size() >= n && up.compare(up.size() - n, n, t) == 0;
        };
        if (ends("_STOCK")) label = "Stock";
        else if (ends("_STOCK_LTS")) label = "Stock (LTS)";
        else {
            label = stem;
            if (up.compare(0, 10, "CUSTOM_AI_") == 0) label = "AI " + stem.substr(10);
            for (char& c : label) if (c == '_') c = ' ';
            if (low.find("/customs/") != std::string::npos) label = "Custom: " + label;
        }
        out.push_back({ label, e.path });
    }
    std::sort(out.begin(), out.end(), [](const NlSetupRef& a, const NlSetupRef& b) {
        int ra = a.label == "Stock" ? 0 : a.label == "Stock (LTS)" ? 1 : 2;
        int rb = b.label == "Stock" ? 0 : b.label == "Stock (LTS)" ? 1 : 2;
        if (ra != rb) return ra < rb;
        return a.label < b.label;
    });
    return out;
}

static bool nlSetupPaint(const std::string& setupPath, nfsnl::NlCarPaint& out) {
    out = nfsnl::NlCarPaint();
    int idx = findAsset(setupPath);
    Bytes raw, sb;
    if (idx < 0 || !readAsset(g_all[idx], raw) ||
        !nfsnl::nlSbDecode(nfsnl::baseName(setupPath), raw.data(), raw.size(), sb))
        return false;
    out = nfsnl::nlCarSetupPaint(sb.data(), sb.size(), nlColours());
    return out.any();
}

// the paint the car's stock setup gives it (an empty paint when it has none)
static nfsnl::NlCarPaint nlStockPaint(const std::string& car, std::string* label = nullptr) {
    nfsnl::NlCarPaint p;
    for (const NlSetupRef& s : nlCarSetups(car))
        if (nlSetupPaint(s.path, p)) { if (label) *label = s.label; return p; }
    return nfsnl::NlCarPaint();
}

static std::string nlPaintSummary(const nfsnl::NlCarPaint& p) {
    std::string s;
    static const char* kWhat[] = { "body", "rims", "calipers" };
    for (int k = 0; k < 3; ++k)
        if (p.have[k]) s += (s.empty() ? "" : ", ") + std::string(kWhat[k]) + " " + p.name[k];
    return s;
}

static void viewerReloadTextures() {
    loadViewerTextures();
    if (g_glOk && g_glWnd) {
        glFreeModel();
        glUploadTextures();
        glUploadShaders();
        g_glListsDirty = true;
        InvalidateRect(g_glWnd, nullptr, FALSE);
    }
    g_viewDirty = true;
    InvalidateRect(g_main, &g_viewRect, FALSE);
}

static void applySidecars(nfsnl::Model& m, const std::string& path, const Bytes& points);
static void textureNlScene(nfsnl::Model& m, const std::string& path);

// the region's <scene>.scene.sb beside its .scene_static.sba
static bool nlReadRegionScene(const std::string& staticPath, Bytes& sb) {
    size_t cut = staticPath.find(".scene_static");
    if (cut == std::string::npos) return false;
    std::string want = staticPath.substr(0, cut) + ".scene.sb";
    int idx = findAsset(want);
    if (idx < 0) idx = findAssetOne(nfsnl::baseName(want));
    Bytes raw;
    if (idx < 0 || !readAsset(g_all[idx], raw)) return false;
    if (!nfsnl::nlSbDecode(nfsnl::baseName(want), raw.data(), raw.size(), sb)) sb = raw;
    return true;
}

// a layer's own static geometry: <name>.scene_static.sba somewhere in the library
static int nlFindLayerScene(const std::string& file, const std::string& regionPath) {
    std::string want = file;
    for (char& c : want) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    int best = -1, bestScore = -1;
    std::string region = nfsnl::baseName(regionPath.substr(0, regionPath.find(".scene_static")));
    for (char& c : region) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    for (size_t i = 0; i < g_all.size(); ++i) {
        std::string low = g_all[i].path;
        for (char& c : low) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        size_t ss = low.find(".scene_static.sba");
        if (ss == std::string::npos || ss + 17 != low.size()) continue;      // geometry, not lightmaps
        if (low == regionPath) continue;
        std::string stemPath = low.substr(0, ss);
        std::string leaf = nfsnl::baseName(stemPath);
        int score = -1;
        if (leaf == want) score = 4;
        else if (leaf.find(want) != std::string::npos) score = 3;
        else if (stemPath.find("/" + want + "/") != std::string::npos) score = 2;
        if (score < 0) continue;
        if (!region.empty() && low.find(region) != std::string::npos) score += 2;
        if (score > bestScore) { bestScore = score; best = (int)i; }
    }
    return best;
}

// the paint list for the model in the viewer; false when it is not a No
// Limits car with setups or a track with limited-time layers
static bool nlPaintComboFill() {
    g_nlSetups.clear();
    g_ltsGroups.clear();
    g_ltsSel = 0;
    g_nlPaint = nfsnl::NlCarPaint();
    g_nlPaintLabel.clear();
    std::string car = nlCarIdFor(g_viewPath);
    if (!car.empty()) {
        g_nlSetups = nlCarSetups(car);
        if (g_nlSetups.empty() || nlColours().empty()) { g_nlSetups.clear(); return false; }
        g_paintKind = PAINT_NL_CAR;
        SendMessageW(g_paintCombo, CB_ADDSTRING, 0, (LPARAM)L"Colours: textures");
        for (const NlSetupRef& r : g_nlSetups)
            SendMessageW(g_paintCombo, CB_ADDSTRING, 0, (LPARAM)widen("Colours: " + r.label).c_str());
        // the car as the game ships it: its stock setup's paint
        int sel = 0;
        for (size_t k = 0; k < g_nlSetups.size(); ++k)
            if (nlSetupPaint(g_nlSetups[k].path, g_nlPaint)) { sel = (int)k + 1; g_nlPaintLabel = g_nlSetups[k].label; break; }
        if (sel) nfsnl::nlApplyCarPaint(g_viewModel, g_nlPaint);
        SendMessageW(g_paintCombo, CB_SETCURSEL, sel, 0);
        if (sel) g_previewText += "\r\nColours of the car's " + g_nlPaintLabel + " setup: " +
                                  nlPaintSummary(g_nlPaint) + " (the list under the view picks another setup)";
        return true;
    }
    if (g_viewPath.find(".scene_static") != std::string::npos) {
        Bytes sb;
        if (!nlReadRegionScene(g_viewPath, sb)) return false;
        g_ltsGroups = nfsnl::nlSceneLtsGroups(sb.data(), sb.size());
        if (g_ltsGroups.empty()) return false;
        g_paintKind = PAINT_NL_LTS;
        SendMessageW(g_paintCombo, CB_ADDSTRING, 0, (LPARAM)L"Limited-time: none");
        for (const nfsnl::NlLtsGroup& g : g_ltsGroups)
            SendMessageW(g_paintCombo, CB_ADDSTRING, 0, (LPARAM)widen("Limited-time: " + nfsnl::nlLtsLabel(g)).c_str());
        SendMessageW(g_paintCombo, CB_SETCURSEL, 0, 0);
        g_previewText += "\r\n" + std::to_string(g_ltsGroups.size()) +
                         " limited-time layer(s) for this region (Halloween, Christmas, events) - "
                         "pick one in the list under the view";
        return true;
    }
    return false;
}

// a limited-time layer onto the region in the viewer (sel 0: none)
static std::string nlLoadLtsLayer(nfsnl::Model& target, const std::string& regionPath,
                                  const nfsnl::NlLtsGroup& g) {
    std::string report;
    int parts = 0, models = 0;
    for (const std::string& f : g.layerFiles) {
        int idx = nlFindLayerScene(f, regionPath);
        Bytes raw;
        if (idx < 0 || !readAsset(g_all[idx], raw)) {
            report += (report.empty() ? "" : "; ") + f + ".scene_static.sba is not in the library";
            continue;
        }
        nfsnl::Model layer = nfsnl::loadModel(raw.data(), raw.size());
        if (!layer.valid) { report += (report.empty() ? "" : "; ") + f + " did not load"; continue; }
        textureNlScene(layer, g_all[idx].path);
        for (nfsnl::Mesh& me : layer.meshes) {
            me.part = "lts:" + g.name;
            me.kit = 0;
            me.kitSlot.clear();
            target.meshes.push_back(std::move(me));
            ++parts;
        }
        for (const nfsnl::Material& mt : layer.materials) target.materials.push_back(mt);
    }
    // the models the layer's actors place; particle effects stay out
    std::map<std::string, nfsnl::Model> cache;
    int missing = 0;
    for (const nfsnl::NlPlacedModel& pm : g.models) {
        std::string ref = pm.file;
        if (ref.compare(0, 11, "/published/") == 0) ref.erase(0, 11);
        while (!ref.empty() && ref[0] == '/') ref.erase(0, 1);
        if (ref.find("models/fx/") != std::string::npos) continue;
        auto it = cache.find(ref);
        if (it == cache.end()) {
            nfsnl::Model mdl;
            int idx = findAssetOne(ref);
            Bytes raw;
            if (idx >= 0 && readAsset(g_all[idx], raw)) {
                mdl = nfsnl::loadModel(raw.data(), raw.size());
                if (mdl.valid) applySidecars(mdl, g_all[idx].path, Bytes());
            }
            it = cache.emplace(ref, std::move(mdl)).first;
        }
        if (!it->second.valid) { ++missing; continue; }
        nfsnl::Model copy = it->second;
        copy.meshes.erase(std::remove_if(copy.meshes.begin(), copy.meshes.end(),
                                         [](const nfsnl::Mesh& me) { return me.lod == "HELPERS"; }),
                          copy.meshes.end());
        nfsnl::transformModel(copy, pm.matrix);
        for (nfsnl::Mesh& me : copy.meshes) {
            me.part = "lts:" + g.name;
            me.kit = 0;
            me.kitSlot.clear();
            if (me.lod.empty()) me.lod = "LOD00";
            target.meshes.push_back(std::move(me));
        }
        for (const nfsnl::Material& mt : copy.materials) target.materials.push_back(mt);
        ++models;
    }
    char b[200];
    snprintf(b, sizeof(b), "%d layer part(s), %d model(s) placed%s", parts, models,
             missing ? (", " + std::to_string(missing) + " model(s) not in the library").c_str() : "");
    return b + (report.empty() ? std::string() : " - " + report);
}

static void nlPaintComboApply(int sel) {
    if (g_paintKind == PAINT_NL_CAR) {
        nfsnl::nlRemoveCarPaint(g_viewModel);
        g_nlPaint = nfsnl::NlCarPaint();
        g_nlPaintLabel.clear();
        if (sel > 0 && (size_t)sel <= g_nlSetups.size() && nlSetupPaint(g_nlSetups[sel - 1].path, g_nlPaint)) {
            g_nlPaintLabel = g_nlSetups[sel - 1].label;
            int n = nfsnl::nlApplyCarPaint(g_viewModel, g_nlPaint);
            setStatus("Colours of the " + g_nlPaintLabel + " setup (" + nlPaintSummary(g_nlPaint) + ") on " +
                      std::to_string(n) + " part(s)");
        } else if (sel > 0) {
            setStatus("This setup names no colours the game's colours.sb knows");
        } else {
            setStatus("Colours as the textures have them");
        }
        viewerReloadTextures();
        return;
    }
    if (g_paintKind == PAINT_NL_LTS) {
        auto& ms = g_viewModel.meshes;
        ms.erase(std::remove_if(ms.begin(), ms.end(),
                                [](const nfsnl::Mesh& me) { return me.part.compare(0, 4, "lts:") == 0; }),
                 ms.end());
        g_ltsSel = sel;
        if (sel > 0 && (size_t)sel <= g_ltsGroups.size()) {
            HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
            std::string r = nlLoadLtsLayer(g_viewModel, g_viewPath, g_ltsGroups[sel - 1]);
            SetCursor(old);
            setStatus("Limited-time " + nfsnl::nlLtsLabel(g_ltsGroups[sel - 1]) + ": " + r);
        } else {
            setStatus("The region without limited-time layers");
        }
        viewerReloadTextures();
    }
}

static void fillPaintCombo() {
    SendMessageW(g_paintCombo, CB_RESETCONTENT, 0, 0);
    g_paintPrefix.assign(g_viewModel.meshes.size(), std::string());
    g_paintSuffixes.clear();
    g_paintKind = PAINT_NONE;
    if (nlPaintComboFill()) return;
    std::string current;
    bool any = false;
    std::string carPrefix;
    for (size_t i = 0; i < g_viewModel.meshes.size(); ++i) {
        std::string leaf = nfsnl::baseName(g_viewModel.meshes[i].texture);
        if (leaf.size() < 5 || leaf.compare(leaf.size() - 4, 4, ".m3g") != 0) continue;
        if (leaf.compare(0, 12, "texture_car_") != 0 && leaf.compare(0, 15, "texture_bonnet_") != 0) continue;
        std::string stem = leaf.substr(0, leaf.size() - 4), suffix;
        if (stem.size() > 3 && stem[stem.size() - 3] == '_' && isdigit((unsigned char)stem[stem.size() - 2]) &&
            isdigit((unsigned char)stem[stem.size() - 1])) {
            suffix = stem.substr(stem.size() - 3);
            stem.erase(stem.size() - 3);
        }
        if (stem.find("_livery") != std::string::npos || stem.find("_garage") != std::string::npos ||
            stem.find("_exhaust") != std::string::npos)
            continue;
        g_paintPrefix[i] = stem;
        if (stem.compare(0, 12, "texture_car_") == 0 && stem.find("_bodykit") == std::string::npos) {
            carPrefix = stem;
            current = suffix;
        } else if (!any && carPrefix.empty()) {
            current = suffix;
        }
        any = true;
    }
    if (!any) return;
    g_paintKind = PAINT_EA;
    nfsnl::EaPhoneFiles f = eaPhoneFiles();
    std::string probe = carPrefix;
    if (probe.empty())
        for (const std::string& p : g_paintPrefix) if (!p.empty()) { probe = p; break; }
    if (!f.find(probe + ".m3g").empty()) g_paintSuffixes.push_back("");
    for (int k = 1; k <= 12; ++k) {
        char b[8];
        snprintf(b, sizeof(b), "_%02d", k);
        if (!f.find(probe + b + ".m3g").empty()) g_paintSuffixes.push_back(b);
    }
    int sel = 0;
    for (size_t k = 0; k < g_paintSuffixes.size(); ++k) {
        std::wstring label = g_paintSuffixes[k].empty() ? std::wstring(L"Paint: default")
                                                        : L"Paint " + widen(g_paintSuffixes[k].substr(1));
        SendMessageW(g_paintCombo, CB_ADDSTRING, 0, (LPARAM)label.c_str());
        if (g_paintSuffixes[k] == current) sel = (int)k;
    }
    SendMessageW(g_paintCombo, CB_SETCURSEL, sel, 0);
}

static void glUploadTextures();
static void glUploadShaders();
static void applyPaint(int sel) {
    if (g_paintKind == PAINT_NL_CAR || g_paintKind == PAINT_NL_LTS) { nlPaintComboApply(sel); return; }
    if (sel < 0 || (size_t)sel >= g_paintSuffixes.size()) return;
    nfsnl::EaPhoneFiles f = eaPhoneFiles();
    const std::string& suf = g_paintSuffixes[sel];
    int changed = 0;
    for (size_t i = 0; i < g_viewModel.meshes.size() && i < g_paintPrefix.size(); ++i) {
        if (g_paintPrefix[i].empty()) continue;
        // the family's own version, else its plain one
        std::string p = f.find(g_paintPrefix[i] + suf + ".m3g");
        if (p.empty()) p = f.find(g_paintPrefix[i] + ".m3g");
        if (p.empty()) p = f.find(g_paintPrefix[i] + "_01.m3g");
        if (!p.empty() && p != g_viewModel.meshes[i].texture) { g_viewModel.meshes[i].texture = p; ++changed; }
    }
    if (!changed) return;
    loadViewerTextures();
    if (g_glOk && g_glWnd) {
        glFreeModel();
        glUploadTextures();
        glUploadShaders();
        g_glListsDirty = true;
        InvalidateRect(g_glWnd, nullptr, FALSE);
    }
    g_viewDirty = true;
    InvalidateRect(g_main, &g_viewRect, FALSE);
}

static void openViewer(nfsnl::Model&& m) {
    g_viewModel = std::move(m);
    g_view = nfsnl::RenderView();
    // the two shading toggles stay as the user left them, model to model
    g_view.textured = SendMessageW(g_texCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_view.vertexColors = SendMessageW(g_vcolCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const Palette& p = pal();
    g_view.background[0] = (float)GetRValue(p.previewBg);
    g_view.background[1] = (float)GetGValue(p.previewBg);
    g_view.background[2] = (float)GetBValue(p.previewBg);
    nfsnl::frameModel(g_viewModel, g_view);
    stopAnimation();
    fillLodCombo();
    fillKitCombo();
    fillPaintCombo();
    g_comboMode = COMBO_LOD;
    loadViewerTextures();
    setPosBoxes();
    g_viewActive = true;
    g_viewDirty = true;
    showViewerControls(true);
    if (g_glOk && g_glWnd) {
        glFreeModel();
        glUploadTextures();
        glUploadShaders();
        g_glListsDirty = true;
        MoveWindow(g_glWnd, g_viewRect.left, g_viewRect.top, g_viewRect.right - g_viewRect.left,
                   g_viewRect.bottom - g_viewRect.top, TRUE);
        ShowWindow(g_glWnd, SW_SHOW);
        InvalidateRect(g_glWnd, nullptr, FALSE);
    }
}

static void closeViewer() {
    if (!g_viewActive) return;
    stopAnimation();
    g_viewActive = false;
    if (g_glWnd) ShowWindow(g_glWnd, SW_HIDE);
    glFreeModel();
    g_viewModel = nfsnl::Model();
    g_viewTexStore.clear();
    g_viewTextures.clear();
    g_texCache.clear();
    g_viewFrame = nfsnl::Image();
    showViewerControls(false);
}

// ----------------------------------------------------------------- GPU viewer
//
// The model view draws through OpenGL (1.1, which every Windows machine has)
// in a child window over the view area: display lists per part, textures on
// the card, so turning a car or a whole track keeps up with the mouse. The
// software renderer stays as the fallback when no GL context can be made
// (a remote desktop without a driver, say) and for the command line.

static void glFreeModel() {
    if (!g_glOk) return;
    wglMakeCurrent(g_glDC, g_glRC);
    if (!g_glTex.empty()) glDeleteTextures((GLsizei)g_glTex.size(), g_glTex.data());
    g_glTex.clear();
    if (!g_glMatcap.empty()) glDeleteTextures((GLsizei)g_glMatcap.size(), g_glMatcap.data());
    g_glMatcap.clear();
    g_meshMatcap.clear();
    if (g_glLists) glDeleteLists(g_glLists, (GLsizei)g_glListCount);
    g_glLists = 0;
    g_glListCount = 0;
    g_glListsDirty = true;
}

static void glUploadTextures() {
    if (!g_glOk) return;
    wglMakeCurrent(g_glDC, g_glRC);
    if (!g_glTex.empty()) glDeleteTextures((GLsizei)g_glTex.size(), g_glTex.data());
    g_glTex.assign(g_viewTexStore.size(), 0);
    g_glTexAlpha.assign(g_viewTexStore.size(), nfsnl::AlphaStats());
    if (g_glTex.empty()) return;
    glGenTextures((GLsizei)g_glTex.size(), g_glTex.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (size_t i = 0; i < g_viewTexStore.size(); ++i) {
        nfsnl::Image rgba = nfsnl::toRgba(g_viewTexStore[i]);
        if (g_viewTexStore[i].channels == 4) g_glTexAlpha[i] = nfsnl::alphaStats(rgba);
        glBindTexture(GL_TEXTURE_2D, g_glTex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        if (rgba.ok())
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba.width, rgba.height, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, rgba.pixels.data());
    }
}

// Real Racing 3: shaders/fresnel_<x>.rgb.pvr and spec_<x>.rgb.pvr, when the
// library has them, baked into one sphere map per shader the model uses
static void glUploadShaders() {
    g_meshMatcap.assign(g_viewModel.meshes.size(), -1);
    g_shaderParts = 0;
    if (!g_glOk) return;
    std::map<std::string, int> made;
    std::map<std::string, bool> missing;
    auto loadRamp = [&](const std::string& leaf, nfsnl::Image& img) {
        int idx = findAssetOne(leaf);
        if (idx < 0 || nfsnl::baseName(g_all[idx].path) != leaf) return false;
        Bytes raw;
        return readAsset(g_all[idx], raw) && nfsnl::decodeTextureFile(raw.data(), raw.size(), img);
    };
    for (size_t i = 0; i < g_viewModel.meshes.size(); ++i) {
        std::string sh = nfsnl::rr3ShaderFor(g_viewModel.meshes[i]);
        if (sh.empty() || missing[sh]) continue;
        auto it = made.find(sh);
        if (it == made.end()) {
            nfsnl::Image fr, sp;
            bool f = loadRamp("fresnel_" + sh + ".rgb.pvr", fr);
            bool p = loadRamp("spec_" + sh + ".rgb.pvr", sp);
            if (!f && !p) { missing[sh] = true; continue; }
            nfsnl::Image mc = nfsnl::rr3ShaderMatcap(f ? &fr : nullptr, p ? &sp : nullptr, 128);
            wglMakeCurrent(g_glDC, g_glRC);
            GLuint t = 0;
            glGenTextures(1, &t);
            glBindTexture(GL_TEXTURE_2D, t);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mc.width, mc.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         mc.pixels.data());
            g_glMatcap.push_back(t);
            it = made.emplace(sh, (int)g_glMatcap.size() - 1).first;
        }
        g_meshMatcap[i] = it->second;
        ++g_shaderParts;
    }
    if (g_shaderParts) {
        char b[160];
        snprintf(b, sizeof(b), "%d part(s) shaded with the game's fresnel/spec ramps (%u shader(s))",
                 g_shaderParts, (unsigned)made.size());
        g_viewModel.warnings.push_back(b);
    }
}

static void glBuildLists() {
    if (g_glLists) glDeleteLists(g_glLists, (GLsizei)g_glListCount);
    g_glListCount = g_viewModel.meshes.size();
    g_glLists = g_glListCount ? glGenLists((GLsizei)g_glListCount) : 0;
    g_glListsWithColour = g_view.vertexColors;
    for (size_t i = 0; i < g_glListCount && g_glLists; ++i) {
        const nfsnl::Mesh& m = g_viewModel.meshes[i];
        size_t nv = m.positions.size() / 3;
        glNewList(g_glLists + (GLuint)i, GL_COMPILE);
        if (nv && !m.indices.empty()) {
            glEnableClientState(GL_VERTEX_ARRAY);
            glVertexPointer(3, GL_FLOAT, 0, m.positions.data());
            bool n = m.normals.size() == m.positions.size();
            bool t = m.uvs.size() / 2 == nv;
            bool c = g_glListsWithColour && m.colors.size() == nv * 4;
            if (n) { glEnableClientState(GL_NORMAL_ARRAY); glNormalPointer(GL_FLOAT, 0, m.normals.data()); }
            if (t) { glEnableClientState(GL_TEXTURE_COORD_ARRAY); glTexCoordPointer(2, GL_FLOAT, 0, m.uvs.data()); }
            if (c) { glEnableClientState(GL_COLOR_ARRAY); glColorPointer(4, GL_FLOAT, 0, m.colors.data()); }
            glDrawElements(GL_TRIANGLES, (GLsizei)m.indices.size(), GL_UNSIGNED_INT, m.indices.data());
            glDisableClientState(GL_VERTEX_ARRAY);
            if (n) glDisableClientState(GL_NORMAL_ARRAY);
            if (t) glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            if (c) glDisableClientState(GL_COLOR_ARRAY);
        }
        glEndList();
    }
    g_glListsDirty = false;
}

// parts, triangles and vertices of what is drawn
static void viewStats(size_t& parts, size_t& tris, size_t& verts) {
    parts = tris = verts = 0;
    std::vector<uint8_t> vis = nfsnl::visibleMeshes(g_viewModel, g_view);
    for (size_t i = 0; i < g_viewModel.meshes.size(); ++i) {
        if (!vis[i]) continue;
        ++parts;
        tris += g_viewModel.meshes[i].indices.size() / 3;
        verts += g_viewModel.meshes[i].positions.size() / 3;
    }
}

static std::string viewStatsLine() {
    size_t parts, tris, verts;
    viewStats(parts, tris, verts);
    char b[160];
    snprintf(b, sizeof(b), "Polygons: %zu   Vertices: %zu   Parts: %zu", tris, verts, parts);
    return b;
}

static void glPaint() {
    if (!g_glOk || !g_glWnd) return;
    wglMakeCurrent(g_glDC, g_glRC);
    RECT rc;
    GetClientRect(g_glWnd, &rc);
    int w = rc.right, h = rc.bottom;
    if (w < 2 || h < 2) return;
    glViewport(0, 0, w, h);
    glClearColor(0.2f, 0.2f, 0.22f, 1);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    if (!g_viewActive) { SwapBuffers(g_glDC); return; }
    if (g_glListsDirty || g_glListsWithColour != g_view.vertexColors) glBuildLists();

    // ---- sky ----
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 1, 0, 1, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    float top[3], low[3];
    nfsnl::skyColours(top, low);
    glBegin(GL_QUADS);
    glColor3f(low[0] / 255, low[1] / 255, low[2] / 255);
    glVertex2f(0, 0); glVertex2f(1, 0); glVertex2f(1, 0.4f); glVertex2f(0, 0.4f);
    glVertex2f(0, 0.4f); glVertex2f(1, 0.4f);
    glColor3f(top[0] / 255, top[1] / 255, top[2] / 255);
    glVertex2f(1, 1); glVertex2f(0, 1);
    glEnd();

    // ---- camera: the same orbit the software renderer uses ----
    float R[3], U[3], F[3];
    nfsnl::viewAxes(g_view, R, U, F);
    float aspect = (float)w / (float)h;
    float nearZ = std::max(g_view.distance * 0.01f, g_view.radius * 0.001f);
    float farZ = g_view.distance + g_view.radius * 40.0f;
    float t = nearZ * std::tan(0.5f * g_view.fovY);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-t * aspect, t * aspect, -t, t, nearZ, farZ);
    glMatrixMode(GL_MODELVIEW);
    // headlight, fixed to the camera
    glLoadIdentity();
    GLfloat head[4] = { 0, 0.3f, 1, 0 };
    GLfloat headCol[4] = { 0.42f, 0.42f, 0.42f, 1 };
    glLightfv(GL_LIGHT1, GL_POSITION, head);
    glLightfv(GL_LIGHT1, GL_DIFFUSE, headCol);
    float off[3] = { g_view.modelPos[0] - g_view.centre[0], g_view.modelPos[1] - g_view.centre[1],
                     g_view.modelPos[2] - g_view.centre[2] };
    GLfloat M[16] = {
        R[0], U[0], -F[0], 0,
        R[1], U[1], -F[1], 0,
        R[2], U[2], -F[2], 0,
        R[0] * off[0] + R[1] * off[1] + R[2] * off[2] - g_view.pan[0],
        U[0] * off[0] + U[1] * off[1] + U[2] * off[2] - g_view.pan[1],
        -(F[0] * off[0] + F[1] * off[1] + F[2] * off[2]) - g_view.distance,
        1 };
    glLoadMatrixf(M);
    GLfloat key[4] = { 0.4f, 0.8f, 0.45f, 0 };
    GLfloat keyCol[4] = { 0.62f, 0.62f, 0.6f, 1 };
    GLfloat amb[4] = { 0.3f, 0.31f, 0.33f, 1 };
    glLightfv(GL_LIGHT0, GL_POSITION, key);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, keyCol);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, 1);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glShadeModel(GL_SMOOTH);

    // ---- the model ----
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_LIGHT1);
    glEnable(GL_NORMALIZE);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    // V as the software renderer samples it: t = 1 - v on a top-down image
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glTranslatef(0, 1, 0);
    glScalef(1, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    std::vector<uint8_t> vis = nfsnl::visibleMeshes(g_viewModel, g_view);
    // Which texture each part draws with, and how see-through it is:
    // opaque and cut-out parts go first (cut-outs through the alpha test),
    // then the blended ones - glass - from far to near, without writing depth.
    size_t nMesh = std::min(g_viewModel.meshes.size(), g_glListCount);
    std::vector<int> texOf(nMesh, -1);
    std::vector<uint8_t> modeOf(nMesh, nfsnl::ALPHA_OPAQUE);
    std::vector<float> alphaOf(nMesh, 1.0f);
    std::vector<size_t> order;
    std::vector<std::pair<float, size_t>> blended;
    for (size_t i = 0; i < nMesh; ++i) {
        if (!vis[i]) continue;
        const nfsnl::Mesh& m = g_viewModel.meshes[i];
        if (g_view.textured && i < g_viewTextures.size() && g_viewTextures[i]) {
            const nfsnl::Image* img = g_viewTextures[i];
            for (size_t k = 0; k < g_viewTexStore.size(); ++k)
                if (&g_viewTexStore[k] == img) { texOf[i] = (int)k; break; }
        }
        bool hasUv = m.uvs.size() / 2 == m.positions.size() / 3;
        if (texOf[i] >= 0 && ((size_t)texOf[i] >= g_glTex.size() || !hasUv)) texOf[i] = -1;
        const nfsnl::AlphaStats* st =
            texOf[i] >= 0 && (size_t)texOf[i] < g_glTexAlpha.size() ? &g_glTexAlpha[texOf[i]] : nullptr;
        modeOf[i] = (uint8_t)nfsnl::meshAlphaMode(m, st, &alphaOf[i]);
        if (!g_view.textured && modeOf[i] == nfsnl::ALPHA_CUTOUT) modeOf[i] = nfsnl::ALPHA_OPAQUE;
        if (modeOf[i] == nfsnl::ALPHA_BLEND) {
            float c[3];
            for (int k = 0; k < 3; ++k) c[k] = (m.bboxMin[k] + m.bboxMax[k]) * 0.5f + off[k];
            // distance along the view direction (F points into the screen)
            float d = c[0] * F[0] + c[1] * F[1] + c[2] * F[2];
            blended.push_back({ -d, i });
        } else {
            order.push_back(i);
        }
    }
    std::sort(blended.begin(), blended.end());
    size_t firstBlended = order.size();
    for (auto& b : blended) order.push_back(b.second);

    glAlphaFunc(GL_GREATER, 0.5f);
    for (size_t k = 0; k < order.size(); ++k) {
        size_t i = order[k];
        const nfsnl::Mesh& m = g_viewModel.meshes[i];
        int mode = modeOf[i];
        if (k == firstBlended) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        }
        if (mode == nfsnl::ALPHA_CUTOUT) glEnable(GL_ALPHA_TEST); else glDisable(GL_ALPHA_TEST);
        float a = alphaOf[i];
        int ti = texOf[i];
        if (ti >= 0) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, g_glTex[ti]);
            glColor4f(1, 1, 1, a);
        } else {
            glDisable(GL_TEXTURE_2D);
            if (g_view.textured && (m.color[0] != 1 || m.color[1] != 1 || m.color[2] != 1))
                glColor4f(m.color[0], m.color[1], m.color[2], a);
            else
                glColor4f(0.76f, 0.77f, 0.79f, a);
        }
        if (m.animIndex >= 0 && g_animT > 0) {
            float d[16];
            nfsnl::partAnimDelta(g_viewModel, m, g_animT, d);
            glPushMatrix();
            glMultMatrixf(d);
            glCallList(g_glLists + (GLuint)i);
            glPopMatrix();
        } else {
            glCallList(g_glLists + (GLuint)i);
        }
    }
    glDisable(GL_ALPHA_TEST);
    // Real Racing 3's shader ramps: reflection and highlight added over the
    // solid parts, looked up by the eye-space normal (sphere map)
    if (g_view.textured && !g_glMatcap.empty() && g_meshMatcap.size() >= nMesh) {
        GLboolean lit = glIsEnabled(GL_LIGHTING);
        glDisable(GL_LIGHTING);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glDepthMask(GL_FALSE);
        glEnable(GL_TEXTURE_2D);
        glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
        glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
        glEnable(GL_TEXTURE_GEN_S);
        glEnable(GL_TEXTURE_GEN_T);
        glColor4f(1, 1, 1, 1);
        for (size_t k = 0; k < firstBlended && k < order.size(); ++k) {
            size_t i = order[k];
            int mc = g_meshMatcap[i];
            if (mc < 0 || modeOf[i] != nfsnl::ALPHA_OPAQUE) continue;
            const nfsnl::Mesh& m = g_viewModel.meshes[i];
            glBindTexture(GL_TEXTURE_2D, g_glMatcap[mc]);
            if (m.animIndex >= 0 && g_animT > 0) {
                float d[16];
                nfsnl::partAnimDelta(g_viewModel, m, g_animT, d);
                glPushMatrix();
                glMultMatrixf(d);
                glCallList(g_glLists + (GLuint)i);
                glPopMatrix();
            } else {
                glCallList(g_glLists + (GLuint)i);
            }
        }
        glDisable(GL_TEXTURE_GEN_S);
        glDisable(GL_TEXTURE_GEN_T);
        if (lit) glEnable(GL_LIGHTING);
    }
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);

    float y0 = nfsnl::floorHeight(g_viewModel, g_view) - g_view.modelPos[1];

    // ---- shadow: after the model, so the depth test hides what is behind
    // it; nothing on the floor writes depth ----
    if (nfsnl::wantsShadow(g_view)) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        // contact shade: an ellipse round the footprint, dark in the middle
        float lo[3], hi[3];
        if (nfsnl::modelFootprint(g_viewModel, g_view, lo, hi)) {
            float mx = (lo[0] + hi[0]) * 0.5f, mz = (lo[2] + hi[2]) * 0.5f;
            float rx = (hi[0] - lo[0]) * 0.62f, rz = (hi[2] - lo[2]) * 0.58f;
            glBegin(GL_TRIANGLE_FAN);
            glColor4f(0, 0, 0, 0.24f);
            glVertex3f(mx, y0, mz);
            glColor4f(0, 0, 0, 0);
            for (int k = 0; k <= 48; ++k) {
                float a = 6.2831853f * k / 48;
                glVertex3f(mx + rx * std::cos(a), y0, mz + rz * std::sin(a));
            }
            glEnd();
        }
        // the model flattened onto the floor along a light from overhead,
        // four times with the light tilted a little for a soft edge; the
        // stencil lets each pass darken a pixel once, however many parts
        // overlap there
        GLint stencilBits = 0;
        glGetIntegerv(GL_STENCIL_BITS, &stencilBits);
        if (stencilBits > 0) glEnable(GL_STENCIL_TEST);
        // lit black with the lights off: the colour comes out (0,0,0,a)
        // whatever colour arrays the parts carry
        glEnable(GL_LIGHTING);
        glDisable(GL_LIGHT0);
        glDisable(GL_LIGHT1);
        glDisable(GL_COLOR_MATERIAL);
        GLfloat zero[4] = { 0, 0, 0, 1 };
        glLightModelfv(GL_LIGHT_MODEL_AMBIENT, zero);
        int passes = stencilBits > 0 ? 4 : 1;
        GLfloat shade[4] = { 0, 0, 0, stencilBits > 0 ? 0.13f : 0.3f };
        glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, shade);
        glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, shade);
        glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, zero);
        glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, zero);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-1.0f, -1.0f);
        for (int pass = 0; pass < passes; ++pass) {
            if (stencilBits > 0) {
                glStencilFunc(GL_GREATER, pass + 1, 0xFF);   // stencil < pass + 1
                glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            }
            float L[3];
            nfsnl::shadowLight(pass, L);
            float ax = L[0] / L[1], az = L[2] / L[1];
            GLfloat S[16] = { 1, 0, 0, 0,
                              -ax, 0, -az, 0,
                              0, 0, 1, 0,
                              ax * y0, y0, az * y0, 1 };
            glPushMatrix();
            glMultMatrixf(S);
            for (size_t i = 0; i < nMesh; ++i) {
                if (!vis[i] || modeOf[i] == nfsnl::ALPHA_BLEND) continue;
                const nfsnl::Mesh& am = g_viewModel.meshes[i];
                if (am.animIndex >= 0 && g_animT > 0) {
                    float d[16];
                    nfsnl::partAnimDelta(g_viewModel, am, g_animT, d);
                    glPushMatrix();
                    glMultMatrixf(d);
                    glCallList(g_glLists + (GLuint)i);
                    glPopMatrix();
                } else {
                    glCallList(g_glLists + (GLuint)i);
                }
            }
            glPopMatrix();
        }
        glDisable(GL_POLYGON_OFFSET_FILL);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_LIGHTING);
        glEnable(GL_COLOR_MATERIAL);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    // ---- floor: see-through, a light tint and the grid, drawn last and
    // without writing depth, so the model shows through it from below ----
    if (g_view.floor) {
        float step = nfsnl::gridStep(g_view.radius);
        int lines = 12;
        float cx = std::round(g_view.centre[0] / step) * step;
        float cz = std::round(g_view.centre[2] / step) * step;
        float ext = step * lines;
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glColor4f(0.86f, 0.89f, 0.93f, 0.18f);
        glBegin(GL_QUADS);
        glVertex3f(cx - ext, y0, cz - ext); glVertex3f(cx + ext, y0, cz - ext);
        glVertex3f(cx + ext, y0, cz + ext); glVertex3f(cx - ext, y0, cz + ext);
        glEnd();
        glBegin(GL_LINES);
        for (int i = -lines; i <= lines; ++i) {
            float o = i * step;
            if (i % 5 == 0) glColor4f(0.36f, 0.4f, 0.47f, 0.75f);
            else glColor4f(0.45f, 0.5f, 0.57f, 0.45f);
            glVertex3f(cx + o, y0, cz - ext); glVertex3f(cx + o, y0, cz + ext);
            glVertex3f(cx - ext, y0, cz + o); glVertex3f(cx + ext, y0, cz + o);
        }
        glEnd();
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    // ---- the counts, top left ----
    if (g_glFont) {
        glDisable(GL_DEPTH_TEST);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, w, 0, h, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        std::string line = viewStatsLine();
        glColor3f(0.05f, 0.08f, 0.12f);
        glRasterPos2i(9, h - 17);
        glListBase(g_glFont - 32);
        glCallLists((GLsizei)line.size(), GL_UNSIGNED_BYTE, line.c_str());
        glColor3f(1, 1, 1);
        glRasterPos2i(8, h - 16);
        glCallLists((GLsizei)line.size(), GL_UNSIGNED_BYTE, line.c_str());
    }
    SwapBuffers(g_glDC);
}

static LRESULT CALLBACK glViewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        glPaint();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    // the mouse drives the same camera code as before: hand it to the main
    // window in its own coordinates
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
    case WM_MOUSEMOVE: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        MapWindowPoints(hwnd, g_main, &pt, 1);
        return SendMessageW(g_main, msg, wp, MAKELPARAM(pt.x, pt.y));
    }
    case WM_MOUSEWHEEL:
        return SendMessageW(g_main, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void createGlView(HWND parent) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = glViewProc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"MonkeyToolGLView";
    RegisterClassExW(&wc);
    g_glWnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_CHILD | WS_CLIPSIBLINGS,
                              0, 0, 100, 100, parent, nullptr, g_inst, nullptr);
    if (!g_glWnd) return;
    g_glDC = GetDC(g_glWnd);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 24;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;       // the shadow is drawn once per pixel through it
    pfd.iLayerType = PFD_MAIN_PLANE;
    int pf = ChoosePixelFormat(g_glDC, &pfd);
    if (pf && SetPixelFormat(g_glDC, pf, &pfd)) g_glRC = wglCreateContext(g_glDC);
    if (g_glRC && wglMakeCurrent(g_glDC, g_glRC)) {
        g_glOk = true;
        // the counts' font: the interface font, as bitmaps for glCallLists
        SelectObject(g_glDC, GetStockObject(DEFAULT_GUI_FONT));
        g_glFont = glGenLists(96);
        if (!wglUseFontBitmapsW(g_glDC, 32, 96, g_glFont)) g_glFont = 0;
        logLine("viewer: OpenGL");
    } else {
        logLine("viewer: no OpenGL context - software rendering");
        DestroyWindow(g_glWnd);
        g_glWnd = nullptr;
    }
}

static void paintViewer(HDC hdc) {
    int w = g_viewRect.right - g_viewRect.left;
    int h = g_viewRect.bottom - g_viewRect.top;
    if (w < 8 || h < 8) return;
    if (g_glOk && g_glWnd) {
        InvalidateRect(g_glWnd, nullptr, FALSE);
        return;
    }
    if (g_viewDirty || g_viewFrame.width != w || g_viewFrame.height != h) {
        // moving parts at the current animation time: posed in place for
        // this frame and put back afterwards
        std::vector<std::pair<size_t, std::pair<std::vector<float>, std::vector<float>>>> keep;
        if (g_animT > 0)
            for (size_t i = 0; i < g_viewModel.meshes.size(); ++i) {
                nfsnl::Mesh& am = g_viewModel.meshes[i];
                if (am.animIndex < 0) continue;
                keep.push_back({ i, { am.positions, am.normals } });
                float d[16];
                nfsnl::partAnimDelta(g_viewModel, am, g_animT, d);
                for (size_t k = 0; k + 2 < am.positions.size(); k += 3) {
                    float x = am.positions[k], y = am.positions[k + 1], z = am.positions[k + 2];
                    am.positions[k]     = d[0] * x + d[4] * y + d[8] * z + d[12];
                    am.positions[k + 1] = d[1] * x + d[5] * y + d[9] * z + d[13];
                    am.positions[k + 2] = d[2] * x + d[6] * y + d[10] * z + d[14];
                }
            }
        nfsnl::renderModel(g_viewModel, g_view, &g_viewTextures, w, h, g_viewFrame);
        for (auto& k : keep) {
            g_viewModel.meshes[k.first].positions.swap(k.second.first);
            g_viewModel.meshes[k.first].normals.swap(k.second.second);
        }
        g_viewDirty = false;
    }
    if (!g_viewFrame.ok()) return;

    std::vector<uint8_t> bgra((size_t)w * h * 4);
    for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
        const uint8_t* s = &g_viewFrame.pixels[i * 3];
        uint8_t* d = &bgra[i * 4];
        d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(hdc, g_viewRect.left, g_viewRect.top, w, h, 0, 0, 0, h,
                      bgra.data(), &bi, DIB_RGB_COLORS);
    std::wstring line = widen(viewStatsLine());
    HFONT old = (HFONT)SelectObject(hdc, GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(10, 20, 30));
    TextOutW(hdc, g_viewRect.left + 9, g_viewRect.top + 7, line.c_str(), (int)line.size());
    SetTextColor(hdc, RGB(255, 255, 255));
    TextOutW(hdc, g_viewRect.left + 8, g_viewRect.top + 6, line.c_str(), (int)line.size());
    SelectObject(hdc, old);
}

// ----------------------------------------------------------------- sound

static std::string exeFolder() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring w(buf, n);
    size_t slash = w.find_last_of(L"\\/");
    return narrow(slash == std::wstring::npos ? w : w.substr(0, slash));
}

// Wwise Vorbis needs the codebook library that ships with the Wwise encoder,
// which is not in the game's files. vgmstream has it; if the user drops
// vgmstream-cli.exe next to MonkeyTool.exe, these sounds convert and play.
// A path the user pointed at once, kept in the registry so it is asked for
// only the first time.
static std::string readVgmstreamPref() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\MonkeyTool", 0, KEY_READ, &key)
        != ERROR_SUCCESS) return std::string();
    wchar_t buf[MAX_PATH] = {0};
    DWORD size = sizeof(buf), type = 0;
    LONG r = RegQueryValueExW(key, L"VgmstreamPath", nullptr, &type, (LPBYTE)buf, &size);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS || type != REG_SZ) return std::string();
    return narrow(buf);
}

static void writeVgmstreamPref(const std::string& path) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\MonkeyTool", 0, nullptr, 0,
                        KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    std::wstring w = widen(path);
    RegSetValueExW(key, L"VgmstreamPath", 0, REG_SZ, (const BYTE*)w.c_str(),
                   (DWORD)((w.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

// ------------------------------------------------------------- Save Editor
// The Save Editor is part of the tool: its window is win32_save_editor.cpp,
// its save reading and writing nfsnl_save.cpp. It opens in its own window,
// owned by this one, so both can be used side by side.
static HICON loadAppIcon(HINSTANCE inst, int cx, int cy);

static SaveEditorTheme saveEditorTheme() {
    SaveEditorTheme t;
    t.dark = g_dark;
    t.windowBg = pal().windowBg;
    t.controlBg = pal().controlBg;
    t.text = pal().text;
    t.dimText = pal().dimText;
    t.icon = loadAppIcon(g_inst, 0, 0);
    return t;
}

static bool setGameAvatar(HWND owner, const std::string& game, const std::string& avatar,
                          const nfsnl::Image& pic, std::string& report);

static void openSaveEditor(HWND owner) {
    saveEditorSetAvatarHook(&setGameAvatar);
    saveEditorOpen(owner, g_inst, saveEditorTheme());
    logLine("save editor: opened");
}

// A floppy disk with a spanner across it, drawn rather than loaded so the
// button needs no image resource and follows the theme.
// Open / Export / Import: the icon from the toolbar strip on the same face
// the Save Editor button uses
// The player's buttons as icons: play / pause, stop, previous, back, forward,
// next. Drawn in the theme's text colour on the control colour, so they
// follow the light and dark themes; the play button shows pause while
// something plays (its window text says which).
static void drawPlayerButton(const DRAWITEMSTRUCT* dis) {
    HDC dc = dis->hDC;
    RECT r = dis->rcItem;
    bool down = (dis->itemState & ODS_SELECTED) != 0;
    bool off = (dis->itemState & ODS_DISABLED) != 0;
    COLORREF bg = down ? (g_dark ? RGB(0x3A, 0x3A, 0x46) : RGB(0xCC, 0xE0, 0xFF)) : pal().controlBg;
    HBRUSH b = CreateSolidBrush(bg);
    FillRect(dc, &r, b);
    DeleteObject(b);
    HPEN frame = CreatePen(PS_SOLID, 1, pal().line);
    HGDIOBJ op = SelectObject(dc, frame);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    COLORREF ink = off ? pal().dimText : pal().text;
    HBRUSH fill = CreateSolidBrush(ink);
    HPEN pen = CreatePen(PS_SOLID, 1, ink);
    SelectObject(dc, fill);
    SelectObject(dc, pen);
    int cx = (r.left + r.right) / 2 + (down ? 1 : 0), cy = (r.top + r.bottom) / 2 + (down ? 1 : 0);
    int h = std::max(4, (int)(r.bottom - r.top) / 2 - 4);      // half the icon's height
    auto tri = [&](int x, int dir) {          // a triangle pointing right (dir 1) or left (-1)
        POINT p[3] = { { x, cy - h }, { x, cy + h }, { x + dir * h * 2 * 7 / 8, cy } };
        Polygon(dc, p, 3);
    };
    auto bar = [&](int x, int w) { Rectangle(dc, x, cy - h, x + w, cy + h + 1); };
    wchar_t text[16] = L"";
    GetWindowTextW(dis->hwndItem, text, 16);
    switch (dis->CtlID) {
    case IDC_PLAY:
        if (text[0] == L'P' && text[1] == L'a') { bar(cx - h + 1, h * 2 / 3 + 1); bar(cx + h / 3, h * 2 / 3 + 1); }
        else tri(cx - h * 3 / 4, 1);
        break;
    case IDC_STOP: Rectangle(dc, cx - h + 1, cy - h + 1, cx + h, cy + h); break;
    case IDC_PREV: bar(cx - h, 2); tri(cx + h * 3 / 4, -1); break;
    case IDC_NEXT: tri(cx - h * 3 / 4, 1); bar(cx + h - 1, 2); break;
    case IDC_BACK: tri(cx, -1); tri(cx + h * 3 / 2 + 1, -1); break;
    case IDC_FWD:  tri(cx - h * 3 / 2 - 1, 1); tri(cx, 1); break;
    }
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(frame);
    DeleteObject(fill);
    DeleteObject(pen);
}

static void drawToolButton(const DRAWITEMSTRUCT* dis, int iconIndex) {
    HDC dc = dis->hDC;
    RECT r = dis->rcItem;
    bool down = (dis->itemState & ODS_SELECTED) != 0;
    bool off = (dis->itemState & ODS_DISABLED) != 0;
    bool hot = (dis->itemState & ODS_HOTLIGHT) != 0 || (dis->itemState & ODS_FOCUS) != 0;
    COLORREF bg = down ? (g_dark ? RGB(0x3A, 0x3A, 0x46) : RGB(0xCC, 0xE0, 0xFF))
                       : (hot ? (g_dark ? RGB(0x33, 0x33, 0x3D) : RGB(0xE5, 0xEF, 0xFF))
                              : pal().controlBg);
    HBRUSH b = CreateSolidBrush(bg);
    FillRect(dc, &r, b);
    DeleteObject(b);
    HPEN frame = CreatePen(PS_SOLID, 1, pal().dimText);
    HGDIOBJ op = SelectObject(dc, frame);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(frame);
    const nfsnl::Image* strip = resourceImage(IDR_TOOLBARICONS);
    if (!strip || strip->height <= 0) return;
    int n = strip->height;
    nfsnl::Image one;
    one.width = one.height = n;
    one.channels = 4;
    one.pixels.assign((size_t)n * n * 4, 0);
    nfsnl::Image rgba = nfsnl::toRgba(*strip);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            int sx = iconIndex * n + x;
            if (sx >= rgba.width) continue;
            uint8_t* d = &one.pixels[((size_t)y * n + x) * 4];
            memcpy(d, &rgba.pixels[((size_t)y * rgba.width + sx) * 4], 4);
            if (off) d[3] = (uint8_t)(d[3] / 3);
        }
    int w = r.right - r.left, h = r.bottom - r.top;
    int x0 = r.left + (w - n) / 2 + (down ? 1 : 0), y0 = r.top + (h - n) / 2 + (down ? 1 : 0);
    drawImageAlpha(dc, one, x0, y0, n, n, bg);
}

// The tree's icons (folders, textures, models, sounds, data, scripts,
// archives, animations), from one strip in the exe
static int treeIconFor(const std::string& path) {
    std::string e = nfsnl::extensionOf(path);
    std::string k = nfsnl::classifyByName(path);
    if (e == "sb3d" || e == "m3g" || e == "fbx" || e == "obj") return 3;
    if (k == "texture") return 2;
    if (k == "sound") return 4;
    if (e == "banim" || e == "anim" || e == "sbfx") return 8;
    if (e == "pack" || e == "cab" || e == "obb" || e == "zip" || e == "apk" || e == "ipa" || e == "z") return 7;
    if (e == "lua" || e == "txt" || e == "xml" || e == "json" || e == "ini" || e == "config" || e == "cfg" ||
        e == "html" || e == "csv" || e == "evt" || e == "nct" || e == "gui")
        return 6;
    if (e == "sb" || e == "sba" || e == "sbin" || e == "bin" || e == "dat" || e == "points" || e == "rr_car")
        return 5;
    return 9;
}

static HIMAGELIST makeImageList(int resId) {
    const nfsnl::Image* strip = resourceImage(resId);
    if (!strip || strip->height <= 0) return nullptr;
    nfsnl::Image rgba = nfsnl::toRgba(*strip);
    int n = rgba.height, count = rgba.width / n;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = rgba.width;
    bi.bmiHeader.biHeight = -rgba.height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hb = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hb || !bits) return nullptr;
    uint8_t* d = (uint8_t*)bits;
    for (size_t i = 0; i < (size_t)rgba.width * rgba.height; ++i) {
        const uint8_t* s = &rgba.pixels[i * 4];
        d[i * 4 + 0] = s[2]; d[i * 4 + 1] = s[1]; d[i * 4 + 2] = s[0]; d[i * 4 + 3] = s[3];
    }
    HIMAGELIST il = ImageList_Create(n, n, ILC_COLOR32, count, 4);
    if (il) ImageList_Add(il, hb, nullptr);
    DeleteObject(hb);
    return il;
}


// ------------------------------------------------------------------- LZHAM
// The .m3g scene models are LZHAM-compressed and the decoder is a separate
// DLL. It is normally beside the exe, but it does not have to be: the same
// arrangement as vgmstream lets the user point at one built anywhere, and the
// path is remembered.
static std::string readLzhamPref() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\MonkeyTool", 0, KEY_READ, &key)
        != ERROR_SUCCESS) return std::string();
    wchar_t buf[MAX_PATH] = {0};
    DWORD size = sizeof(buf), type = 0;
    LONG r = RegQueryValueExW(key, L"LzhamPath", nullptr, &type, (LPBYTE)buf, &size);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS || type != REG_SZ) return std::string();
    return narrow(buf);
}

static void writeLzhamPref(const std::string& path) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\MonkeyTool", 0, nullptr, 0,
                        KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    std::wstring w = widen(path);
    RegSetValueExW(key, L"LzhamPath", 0, REG_SZ, (const BYTE*)w.c_str(),
                   (DWORD)((w.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

// Called once at start-up: the remembered path first, then the places a
// build is likely to have left one.
static void findLzham() {
    if (nfsnl::lzhamAvailable()) return;
    std::string saved = readLzhamPref();
    if (!saved.empty() &&
        GetFileAttributesW(widen(saved).c_str()) != INVALID_FILE_ATTRIBUTES &&
        nfsnl::lzhamSetPath(saved)) {
        logLine("LZHAM loaded from %s", saved.c_str());
        return;
    }
    const wchar_t* places[] = {
        L"\\lzham_x64.dll",
        L"\\dist\\lzham_x64.dll",
        L"\\..\\dist\\lzham_x64.dll",
        L"\\lzham\\lzham_x64.dll",
        L"\\tools\\lzham_x64.dll",
        L"\\lzham.dll",
    };
    std::wstring base = widen(exeFolder());
    for (const wchar_t* tail : places) {
        std::wstring cand = base + tail;
        if (GetFileAttributesW(cand.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (nfsnl::lzhamSetPath(narrow(cand))) {
            logLine("LZHAM loaded from %s", narrow(cand).c_str());
            return;
        }
    }
}

// Ask for one by hand, and say plainly whether it worked.
static bool askForLzham(HWND owner) {
    wchar_t file[MAX_PATH] = L"lzham_x64.dll";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"LZHAM decoder\0lzham_x64.dll;lzham.dll;lzhamdll_x64.dll\0"
                      L"Libraries (*.dll)\0*.dll\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Where is lzham_x64.dll?";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;

    std::string chosen = narrow(std::wstring(file));
    if (nfsnl::lzhamSetPath(chosen)) {
        writeLzhamPref(chosen);
        logLine("LZHAM set to %s", chosen.c_str());
        MessageBoxW(owner,
            L"LZHAM loaded. The .m3g scene models convert from now on, and this "
            L"file is remembered.",
            L"LZHAM", MB_ICONINFORMATION);
        return true;
    }
    MessageBoxW(owner,
        L"That file loaded but has no LZHAM decompress function in it, so it is "
        L"not the right DLL.\n\n"
        L"The one to use is built by BUILD_LZHAM.bat, or comes out of the GitHub "
        L"build as lzham_x64.dll. A 32-bit DLL will not load into this program "
        L"either - it needs the 64-bit one.",
        L"Not an LZHAM decoder", MB_ICONWARNING);
    return false;
}

static bool haveVgmstream(std::string* path) {
    // next to the exe, in a tools folder beside it, or in test_vgmstream's
    // usual layout - whichever the user happened to unzip
    std::string saved = readVgmstreamPref();
    if (!saved.empty() &&
        GetFileAttributesW(widen(saved).c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (path) *path = saved;
        return true;
    }
    const wchar_t* places[] = {
        L"\\vgmstream-cli.exe",
        L"\\vgmstream\\vgmstream-cli.exe",
        L"\\vgmstream-win64\\vgmstream-cli.exe",
        L"\\tools\\vgmstream-cli.exe",
        L"\\tools\\vgmstream\\vgmstream-cli.exe",
        L"\\test.exe",
    };
    std::wstring base = widen(exeFolder());
    for (const wchar_t* tail : places) {
        std::wstring cand = base + tail;
        DWORD attr = GetFileAttributesW(cand.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) continue;
        if (path) *path = narrow(cand);
        return true;
    }
    return false;
}

// Ask once, remember for good.
static bool askForVgmstream(HWND owner) {
    int answer = MessageBoxW(owner,
        L"These sounds are Wwise Vorbis. The codebooks that decode them ship with "
        L"the Wwise encoder, not with the game, so this program cannot decode them "
        L"on its own.\n\n"
        L"vgmstream can. If you have vgmstream-cli.exe, point at it now and it will "
        L"be used from here on - playing and saving as .wav both start working.\n\n"
        L"Locate vgmstream-cli.exe now?",
        L"Wwise Vorbis", MB_ICONQUESTION | MB_YESNO);
    if (answer != IDYES) return false;

    wchar_t file[MAX_PATH] = L"vgmstream-cli.exe";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"vgmstream-cli.exe\0vgmstream-cli.exe;test.exe\0"
                      L"Programs (*.exe)\0*.exe\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Where is vgmstream-cli.exe?";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    writeVgmstreamPref(narrow(std::wstring(file)));
    logLine("vgmstream set to %s", narrow(std::wstring(file)).c_str());
    return true;
}

// Decode a sound to WAV: this build first, vgmstream second.
static bool soundToWav(const uint8_t* data, size_t len, Bytes& wav);

static bool runVgmstream(const Bytes& sound, Bytes& wav, const std::string& ext = ".wem",
                         int subsong = 0) {
    std::string tool;
    if (!haveVgmstream(&tool)) return false;
    // vgmstream picks the format by the extension, so the copy keeps it
    ScratchFile in(ext), scratchOut(".out.wav");
    if (!in.write(sound)) return false;
    const std::string& input = in.path;
    const std::string& out = scratchOut.path;
    DeleteFileW(widen(out).c_str());
    std::wstring cmd = L"\"" + widen(tool) + L"\" -o \"" + widen(out) + L"\" " +
                       (subsong > 0 ? L"-s " + std::to_wstring(subsong) + L" " : std::wstring()) +
                       L"\"" + widen(input) + L"\"";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 60000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return nfsnl::readFile(out, wav) && wav.size() > 44;
}

// One sound in, a playable WAV out - decoded here when the codec allows it,
// handed to vgmstream when it does not.
static bool soundToWav(const uint8_t* data, size_t len, Bytes& wav) {
    std::string why;
    // an engine bank: decoded here, no vgmstream needed
    if (len >= 4 && memcmp(data, "Gnsu", 4) == 0)
        return nfsnl::gnsuToWav(data, len, wav, &why) && wav.size() > 44;
    // an EA SPS stream (Real Racing 2): EA-XAS decoded here, the rest by vgmstream
    if (nfsnl::spsInfo(data, len, nullptr, nullptr, nullptr, nullptr) &&
        nfsnl::spsToWav(data, len, wav, &why) && wav.size() > 44)
        return true;
    if (nfsnl::wemToWav(data, len, wav, &why) && wav.size() > 44) return true;
    Bytes copy(data, data + len);
    return runVgmstream(copy, wav);
}

// ----------------------------------------------------------------- player
//
// A small player on the waveOut API: the decoded sound plays from memory,
// with a position bar that can be dragged, pause, stop, five-second skips and
// previous / next sound in the tree. waveOut rather than PlaySound because
// PlaySound can neither pause, seek nor say where it is.
struct Player {
    HWAVEOUT h = nullptr;
    WAVEHDR hdr{};
    WAVEFORMATEX fmt{};
    Bytes pcm;              // the data chunk of the loaded wav
    size_t base = 0;        // where in pcm the current buffer started
    size_t source = 0;      // size of the g_playable it was loaded from
    bool paused = false;
    bool dragging = false;
};
static Player g_player;
static const UINT_PTR IDT_PLAYER = 43;

static bool parseWavPcm(const Bytes& wav, WAVEFORMATEX& fmt, size_t& off, size_t& len) {
    if (wav.size() < 44 || memcmp(wav.data(), "RIFF", 4) || memcmp(wav.data() + 8, "WAVE", 4))
        return false;
    bool haveFmt = false;
    size_t p = 12;
    while (p + 8 <= wav.size()) {
        uint32_t sz;
        memcpy(&sz, wav.data() + p + 4, 4);
        const uint8_t* b = wav.data() + p + 8;
        if (!memcmp(wav.data() + p, "fmt ", 4) && sz >= 16 && p + 8 + 16 <= wav.size()) {
            memset(&fmt, 0, sizeof(fmt));
            memcpy(&fmt.wFormatTag, b, 2);
            memcpy(&fmt.nChannels, b + 2, 2);
            memcpy(&fmt.nSamplesPerSec, b + 4, 4);
            memcpy(&fmt.nAvgBytesPerSec, b + 8, 4);
            memcpy(&fmt.nBlockAlign, b + 12, 2);
            memcpy(&fmt.wBitsPerSample, b + 14, 2);
            if (fmt.wFormatTag == 0xFFFE) fmt.wFormatTag = WAVE_FORMAT_PCM;   // extensible PCM
            haveFmt = true;
        } else if (!memcmp(wav.data() + p, "data", 4)) {
            off = p + 8;
            len = std::min<size_t>(sz, wav.size() - off);
            return haveFmt && fmt.nBlockAlign && fmt.nAvgBytesPerSec;
        }
        p += 8 + (size_t)sz + (sz & 1);
    }
    return false;
}

// A sound's picture: its waveform (each channel's min/max per column) on a
// dark panel, instead of the file's bytes as hex, which say nothing about a
// sound. Shown whenever a sound decodes, in every game.
static void showWaveform(const Bytes& wav) {
    WAVEFORMATEX wf{};
    size_t off = 0, n = 0;
    g_previewWave = false;
    if (!parseWavPcm(wav, wf, off, n) || wf.wBitsPerSample != 16 || !wf.nChannels) return;
    const int W = 1024, chN = std::min<int>(wf.nChannels, 2), laneH = 150, H = laneH * chN + 20;
    nfsnl::Image img;
    img.width = W; img.height = H; img.channels = 3;
    img.pixels.assign((size_t)W * H * 3, 0);
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        img.pixels[i * 3] = 0x1E; img.pixels[i * 3 + 1] = 0x22; img.pixels[i * 3 + 2] = 0x2A;
    }
    size_t frames = n / wf.nBlockAlign;
    if (!frames) return;
    const uint8_t* pcm = wav.data() + off;
    for (int c = 0; c < chN; ++c) {
        int mid = 10 + laneH * c + laneH / 2;
        for (int x = 0; x < W; ++x) {
            size_t a = frames * x / W, b = std::max(a + 1, frames * (x + 1) / W);
            int lo = 0, hi = 0;
            for (size_t f = a; f < b && f < frames; f += std::max<size_t>(1, (b - a) / 256)) {
                int16_t v;
                memcpy(&v, pcm + f * wf.nBlockAlign + c * 2, 2);
                lo = std::min<int>(lo, v); hi = std::max<int>(hi, v);
            }
            int y0 = mid - hi * (laneH / 2 - 4) / 32768, y1 = mid - lo * (laneH / 2 - 4) / 32768;
            for (int y = std::max(0, y0); y <= std::min(H - 1, y1); ++y) {
                uint8_t* d = &img.pixels[((size_t)y * W + x) * 3];
                d[0] = 0x4A; d[1] = 0xB8; d[2] = 0xF0;
            }
            uint8_t* m = &img.pixels[((size_t)mid * W + x) * 3];
            if (m[0] == 0x1E) { m[0] = 0x55; m[1] = 0x5C; m[2] = 0x6A; }
        }
    }
    g_preview = std::move(img);
    g_previewWave = true;
}

static void playerClose() {
    if (g_player.h) {
        waveOutReset(g_player.h);
        waveOutUnprepareHeader(g_player.h, &g_player.hdr, sizeof(WAVEHDR));
        waveOutClose(g_player.h);
        g_player.h = nullptr;
    }
    g_player.paused = false;
    if (g_main) KillTimer(g_main, IDT_PLAYER);
    if (g_playBtn) SetWindowTextW(g_playBtn, L"Play");
}

static size_t playerPos() {
    if (!g_player.h) return g_player.base;
    MMTIME t{};
    t.wType = TIME_BYTES;
    waveOutGetPosition(g_player.h, &t, sizeof(t));
    return std::min(g_player.base + (size_t)t.u.cb, g_player.pcm.size());
}

static void playerShowTime(size_t pos) {
    if (!g_timeLabel) return;
    double rate = g_player.fmt.nAvgBytesPerSec ? (double)g_player.fmt.nAvgBytesPerSec : 1.0;
    int now = (int)(pos / rate), all = (int)(g_player.pcm.size() / rate);
    wchar_t b[64];
    swprintf(b, 64, L"%d:%02d / %d:%02d", now / 60, now % 60, all / 60, all % 60);
    SetWindowTextW(g_timeLabel, b);
    if (!g_player.dragging && g_seek)
        SendMessageW(g_seek, TBM_SETPOS, TRUE, (LPARAM)(pos / std::max<size_t>(1, (size_t)(rate / 10))));
}

// take whatever g_playable now holds; false when it is not playable PCM
static bool playerLoad() {
    if (g_player.source == g_playable.size() && !g_player.pcm.empty()) return true;
    playerClose();
    g_player.pcm.clear();
    g_player.base = 0;
    size_t off = 0, n = 0;
    if (!parseWavPcm(g_playable, g_player.fmt, off, n)) return false;
    g_player.pcm.assign(g_playable.begin() + off, g_playable.begin() + off + n);
    g_player.source = g_playable.size();
    size_t tenth = std::max<size_t>(1, g_player.fmt.nAvgBytesPerSec / 10);
    SendMessageW(g_seek, TBM_SETRANGEMIN, FALSE, 0);
    SendMessageW(g_seek, TBM_SETRANGEMAX, TRUE, (LPARAM)(g_player.pcm.size() / tenth));
    playerShowTime(0);
    return true;
}

static bool playerStartAt(size_t off) {
    if (!playerLoad()) return false;
    playerClose();
    size_t align = std::max<WORD>(1, g_player.fmt.nBlockAlign);
    off -= off % align;
    if (off >= g_player.pcm.size()) off = 0;
    if (waveOutOpen(&g_player.h, WAVE_MAPPER, &g_player.fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        g_player.h = nullptr;
        setStatus("The sound device refused this format.");
        return false;
    }
    memset(&g_player.hdr, 0, sizeof(g_player.hdr));
    g_player.hdr.lpData = (LPSTR)(g_player.pcm.data() + off);
    g_player.hdr.dwBufferLength = (DWORD)(g_player.pcm.size() - off);
    waveOutPrepareHeader(g_player.h, &g_player.hdr, sizeof(WAVEHDR));
    waveOutWrite(g_player.h, &g_player.hdr, sizeof(WAVEHDR));
    g_player.base = off;
    g_player.paused = false;
    SetWindowTextW(g_playBtn, L"Pause");
    SetTimer(g_main, IDT_PLAYER, 100, nullptr);
    return true;
}

static void playerTogglePlay() {
    if (g_player.h && !g_player.paused) {
        waveOutPause(g_player.h);
        g_player.paused = true;
        SetWindowTextW(g_playBtn, L"Play");
        return;
    }
    if (g_player.h && g_player.paused) {
        waveOutRestart(g_player.h);
        g_player.paused = false;
        SetWindowTextW(g_playBtn, L"Pause");
        return;
    }
    if (!playerLoad()) return;
    size_t tenth = std::max<size_t>(1, g_player.fmt.nAvgBytesPerSec / 10);
    size_t at = (size_t)SendMessageW(g_seek, TBM_GETPOS, 0, 0) * tenth;
    playerStartAt(at >= g_player.pcm.size() ? 0 : at);
}

static void playerSkip(double seconds) {
    if (!playerLoad()) return;
    double pos = (double)playerPos() + seconds * g_player.fmt.nAvgBytesPerSec;
    if (pos < 0) pos = 0;
    bool wasPaused = g_player.paused;
    playerStartAt((size_t)pos);
    if (wasPaused && g_player.h) { waveOutPause(g_player.h); g_player.paused = true; SetWindowTextW(g_playBtn, L"Play"); }
    playerShowTime(playerPos());
}

static void playerTick() {
    if (!g_player.h) return;
    size_t pos = playerPos();
    playerShowTime(pos);
    if (g_player.hdr.dwFlags & WHDR_DONE) {        // played to the end
        playerClose();
        g_player.base = 0;
        playerShowTime(0);
    }
}

static void stopSound() {
    PlaySoundW(nullptr, nullptr, SND_PURGE);
    playerClose();
    g_player.base = 0;
    g_player.source = 0;
    g_player.pcm.clear();
    if (g_seek) SendMessageW(g_seek, TBM_SETPOS, TRUE, 0);
    if (g_timeLabel) SetWindowTextW(g_timeLabel, L"0:00 / 0:00");
}

static void showSoundControls(bool show) {
    for (HWND c : { g_playBtn, g_stopBtn, g_prevBtn, g_backBtn, g_fwdBtn, g_nextBtn, g_seek,
                    g_timeLabel })
        if (c) ShowWindow(c, show ? SW_SHOW : SW_HIDE);
    EnableWindow(g_playBtn, show && !g_playable.empty());
}

// ------------------------------------------------------------- video player
//
// NFS Undercover's .m4v and Shift's .mov films (H.264 in MP4 / QuickTime).
// They play in the viewer's place, with the sound player's controls under
// them, through Media Foundation's MFPlay and a copy in %TEMP% (the asset may
// be inside an archive). mfplay.dll is loaded when first needed, so the tool
// still starts where it is missing; there the film opens in the system's own
// player instead.

static bool isVideoExt(const std::string& x) { return x == "m4v" || x == "mov" || x == "mp4"; }

#ifdef MT_HAVE_MFPLAY
struct VideoPane {
    HWND area = nullptr;
    IMFPMediaPlayer* player = nullptr;
    std::wstring file;
    bool dragging = false;
};
static VideoPane g_video;
static const UINT_PTR IDT_VIDEO = 47;

static bool videoActive() { return g_video.player != nullptr; }

// a time from MFPlay, in 100 ns units: the duration comes as an unsigned
// 64-bit value, the position as a signed one
static LONGLONG videoTime(bool duration) {
    if (!g_video.player) return 0;
    PROPVARIANT v;
    PropVariantInit(&v);
    GUID kind{};                      // MFP_POSITIONTYPE_100NS is GUID_NULL
    HRESULT hr = duration ? g_video.player->GetDuration(kind, &v) : g_video.player->GetPosition(kind, &v);
    LONGLONG t = 0;
    if (SUCCEEDED(hr)) {
        if (v.vt == VT_I8) t = v.hVal.QuadPart;
        else if (v.vt == VT_UI8) t = (LONGLONG)v.uhVal.QuadPart;
    }
    PropVariantClear(&v);
    return t < 0 ? 0 : t;
}

static void videoShowTime() {
    LONGLONG pos = videoTime(false), dur = videoTime(true);
    int ps = (int)(pos / 10000000), ds = (int)(dur / 10000000);
    wchar_t b[64];
    if (dur > 0) swprintf(b, 64, L"%d:%02d / %d:%02d", ps / 60, ps % 60, ds / 60, ds % 60);
    else swprintf(b, 64, L"%d:%02d", ps / 60, ps % 60);
    SetWindowTextW(g_timeLabel, b);
    if (!g_video.dragging && dur > 0)
        SendMessageW(g_seek, TBM_SETPOS, TRUE, (LPARAM)(pos * 1000 / dur));
    MFP_MEDIAPLAYER_STATE st = MFP_MEDIAPLAYER_STATE_EMPTY;
    if (g_video.player) g_video.player->GetState(&st);
    SetWindowTextW(g_playBtn, st == MFP_MEDIAPLAYER_STATE_PLAYING ? L"Pause" : L"Play");
}

static void videoClose() {
    if (g_main) KillTimer(g_main, IDT_VIDEO);
    if (g_video.player) { g_video.player->Shutdown(); g_video.player->Release(); g_video.player = nullptr; }
    if (g_video.area) ShowWindow(g_video.area, SW_HIDE);
    if (!g_video.file.empty()) DeleteFileW(g_video.file.c_str());
    g_video.file.clear();
    g_video.dragging = false;
}

static void videoLayout() {
    if (!g_video.area) return;
    MoveWindow(g_video.area, g_viewRect.left, g_viewRect.top, g_viewRect.right - g_viewRect.left,
               g_viewRect.bottom - g_viewRect.top, TRUE);
    if (g_video.player) g_video.player->UpdateVideo();
}

static void videoTogglePlay() {
    if (!g_video.player) return;
    MFP_MEDIAPLAYER_STATE st = MFP_MEDIAPLAYER_STATE_EMPTY;
    g_video.player->GetState(&st);
    if (st == MFP_MEDIAPLAYER_STATE_PLAYING) g_video.player->Pause();
    else g_video.player->Play();
    videoShowTime();
}

static void videoStop() {
    if (!g_video.player) return;
    g_video.player->Stop();
    PROPVARIANT v;
    PropVariantInit(&v);
    v.vt = VT_I8;
    v.hVal.QuadPart = 0;
    GUID kind{};
    g_video.player->SetPosition(kind, &v);
    SendMessageW(g_seek, TBM_SETPOS, TRUE, 0);
    videoShowTime();
}

// the seek bar: 0..1000 of the film
static void videoSeekTo(int pos1000) {
    LONGLONG dur = videoTime(true);
    if (!g_video.player || dur <= 0) return;
    PROPVARIANT v;
    PropVariantInit(&v);
    v.vt = VT_I8;
    v.hVal.QuadPart = dur * pos1000 / 1000;
    GUID kind{};
    g_video.player->SetPosition(kind, &v);
    videoShowTime();
}

static LRESULT CALLBACK videoAreaProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        FillRect(dc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
        if (g_video.player) g_video.player->UpdateVideo();
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_LBUTTONDOWN) { videoTogglePlay(); return 0; }     // a click on the film pauses it
    return DefWindowProcW(hwnd, msg, wp, lp);
}

typedef HRESULT (WINAPI* MfpCreateFn)(LPCWSTR, BOOL, MFP_CREATION_OPTIONS, IMFPMediaPlayerCallback*, HWND,
                                      IMFPMediaPlayer**);

// the film, ready in the viewer's place; false when Windows cannot play it here
static bool videoOpen(HWND owner, const std::string& assetPath, const Bytes& data) {
    videoClose();
    static MfpCreateFn create = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE dll = LoadLibraryW(L"mfplay.dll");
        if (dll) create = (MfpCreateFn)(void*)GetProcAddress(dll, "MFPCreateMediaPlayer");
    }
    if (!create) return false;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"MonkeyTool";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring file = dir + L"\\" + widen(nfsnl::baseName(assetPath));
    if (!nfsnl::writeFile(narrow(file), data)) return false;
    if (!g_video.area) {
        WNDCLASSEXW wa{};
        wa.cbSize = sizeof(wa);
        wa.lpfnWndProc = videoAreaProc;
        wa.hInstance = g_inst;
        wa.hCursor = LoadCursor(nullptr, IDC_HAND);
        wa.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wa.lpszClassName = L"MonkeyToolVideoArea";
        RegisterClassExW(&wa);
        g_video.area = CreateWindowExW(0, L"MonkeyToolVideoArea", L"", WS_CHILD | WS_CLIPSIBLINGS,
                                       0, 0, 10, 10, owner, nullptr, g_inst, nullptr);
    }
    g_video.file = file;
    videoLayout();
    ShowWindow(g_video.area, SW_SHOW);
    HRESULT hr = create(file.c_str(), FALSE, MFP_OPTION_NONE, nullptr, g_video.area, &g_video.player);
    if (FAILED(hr) || !g_video.player) {
        g_video.player = nullptr;
        videoClose();
        return false;
    }
    SendMessageW(g_seek, TBM_SETRANGEMIN, FALSE, 0);
    SendMessageW(g_seek, TBM_SETRANGEMAX, TRUE, 1000);
    SendMessageW(g_seek, TBM_SETPOS, TRUE, 0);
    SetTimer(g_main, IDT_VIDEO, 250, nullptr);
    return true;
}
#else
static bool videoActive() { return false; }
static void videoClose() {}
static void videoLayout() {}
static void videoTogglePlay() {}
static void videoStop() {}
static void videoSeekTo(int) {}
static void videoShowTime() {}
static bool videoOpen(HWND, const std::string&, const Bytes&) { return false; }
#endif

// the system's own player, when Media Foundation is not there
static void videoOpenOutside(HWND owner, const std::string& assetPath, const Bytes& data) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"MonkeyTool";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring file = dir + L"\\" + widen(nfsnl::baseName(assetPath));
    if (nfsnl::writeFile(narrow(file), data))
        ShellExecuteW(owner, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void updatePreview(const Entry& e);

// is this tree row a sound the player can take?
static bool isSoundItem(HTREEITEM it) {
    if (g_itemToSound.count(it)) return true;
    auto e = g_itemToEntry.find(it);
    if (e == g_itemToEntry.end() || e->second < 0 || (size_t)e->second >= g_all.size()) return false;
    std::string x = nfsnl::extensionOf(g_all[e->second].path);
    return x == "wem" || x == "gnsu" || x == "sps" || x == "wav" || x == "ogg" || x == "mp3" || x == "flac" ||
           x == "m4a" || x == "aac" || x == "opus" || x == "wma" || x == "fsb" || x == "xma";
}

// previous / next sound among the rows beside the selected one, played at once
static void playerStep(int dir) {
    HTREEITEM cur = TreeView_GetSelection(g_tree);
    if (!cur) return;
    HTREEITEM it = cur;
    for (int guard = 0; guard < 100000; ++guard) {
        it = dir > 0 ? TreeView_GetNextSibling(g_tree, it) : TreeView_GetPrevSibling(g_tree, it);
        if (!it) return;
        if (isSoundItem(it)) break;
    }
    TreeView_SelectItem(g_tree, it);          // shows it and decodes it
    if (!g_playable.empty()) playerStartAt(0);
}

// ----------------------------------------------------------------- banks

static void fillBankCombo() {
    SendMessageW(g_lodCombo, CB_RESETCONTENT, 0, 0);
    for (const auto& e : g_bank.entries) {
        char label[128];
        if (e.kind == "gnsu")
            snprintf(label, sizeof(label), "%u - engine, %u Hz", e.id, e.sampleRate);
        else
            snprintf(label, sizeof(label), "%u - %s, %u Hz", e.id,
                     nfsnl::wemCodecName(e.codec), e.sampleRate);
        SendMessageW(g_lodCombo, CB_ADDSTRING, 0, (LPARAM)widen(label).c_str());
    }
    if (!g_bank.entries.empty()) SendMessageW(g_lodCombo, CB_SETCURSEL, 0, 0);
}

// FMOD banks (.fsb, and an .fev's bank beside it): one entry per sound,
// each decoded by vgmstream as its subsong
static Bytes g_fsbData;
static std::vector<std::string> g_fsbNames;
static void showWaveform(const Bytes& wav);

static void selectFsbSound(int index) {
    g_playable.clear();
    if (index < 0 || (size_t)index >= g_fsbNames.size() || g_fsbData.empty()) return;
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    bool ok = runVgmstream(g_fsbData, g_playable, ".fsb", index + 1);
    SetCursor(old);
    if (!ok) g_playable.clear();
    EnableWindow(g_playBtn, !g_playable.empty());
    showWaveform(g_playable);
    InvalidateRect(g_main, &g_previewRect, TRUE);
}

// Prepare one sound from the open bank for the Play button.
static void selectBankSound(int index) {
    g_playable.clear();
    if (index < 0 || (size_t)index >= g_bank.entries.size()) return;
    const nfsnl::BankEntry& e = g_bank.entries[index];
    if (e.offset + e.size > g_bankData.size()) return;
    const uint8_t* p = g_bankData.data() + e.offset;

    char head[256];
    if (e.kind == "gnsu") {
        bool ok = soundToWav(p, e.size, g_playable);
        if (!ok) g_playable.clear();
        snprintf(head, sizeof(head),
                 "\r\nSound %u is an engine bank (%.0f-%.0f rpm): %s", e.id, e.minRpm,
                 e.maxRpm, ok ? "press Play to hear its grains, idle to redline."
                              : "it did not decode.");
        g_previewText += head;
        EnableWindow(g_playBtn, !g_playable.empty());
        InvalidateRect(g_main, &g_previewRect, TRUE);
        return;
    }
    if (soundToWav(p, e.size, g_playable)) {
        snprintf(head, sizeof(head), "\r\nSound %u ready - press Play, or save the "
                                     "bank to write every sound as .wav.", e.id);
    } else {
        g_playable.clear();
        snprintf(head, sizeof(head),
                 "\r\nSound %u is %s. Point the tool at vgmstream-cli.exe "
                 "(it asks when you save) and this plays.",
                 e.id, nfsnl::wemCodecName(e.codec));
    }
    g_previewText += head;
    EnableWindow(g_playBtn, !g_playable.empty());
    showWaveform(g_playable);
    InvalidateRect(g_main, &g_previewRect, TRUE);
}

// Fill in a bank's sounds the first time its row is opened. Reading the bank
// costs one cabinet, so it is done on demand rather than for every bank in
// the game at start-up.
static void expandBank(HTREEITEM item, int entryIndex) {
    if (g_bankExpanded[item]) return;
    g_bankExpanded[item] = true;
    if (entryIndex < 0 || (size_t)entryIndex >= g_all.size()) return;

    Bytes data;
    if (!readAsset(g_all[entryIndex], data)) return;
    nfsnl::Bank bank;
    if (!nfsnl::readBank(data.data(), data.size(), bank) || bank.entries.empty()) {
        // no children after all - take the expander away again
        TVITEMW it{};
        it.mask = TVIF_HANDLE | TVIF_CHILDREN;
        it.hItem = item;
        it.cChildren = 0;
        SendMessageW(g_tree, TVM_SETITEMW, 0, (LPARAM)&it);
        return;
    }

    for (size_t k = 0; k < bank.entries.size(); ++k) {
        const nfsnl::BankEntry& be = bank.entries[k];
        char label[160];
        if (be.kind == "gnsu")
            snprintf(label, sizeof(label), "%u.gnsu  (engine, %.0f-%.0f rpm)",
                     be.id, be.minRpm, be.maxRpm);
        else if (be.kind == "wem")
            snprintf(label, sizeof(label), "%u.wem  (%s, %u Hz)", be.id,
                     nfsnl::wemCodecName(be.codec), be.sampleRate);
        else
            snprintf(label, sizeof(label), "%u.bin", be.id);

        std::wstring wl = widen(label);
        TVINSERTSTRUCTW ins{};
        ins.hParent = item;
        ins.hInsertAfter = TVI_LAST;
        ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
        ins.item.iImage = ins.item.iSelectedImage = 4;
        ins.item.pszText = (LPWSTR)wl.c_str();
        ins.item.lParam = -1;
        HTREEITEM child = (HTREEITEM)SendMessageW(g_tree, TVM_INSERTITEMW, 0, (LPARAM)&ins);
        if (child) g_itemToSound[child] = SoundRef{entryIndex, (int)k};
    }
}

static void showTextView(bool show);
static void setTextView(const std::string& text);
static void saveBankSound(HWND owner, const SoundRef& ref);

// One sound picked straight out of the tree: show what it is, and get it
// ready for Play.
static void previewBankSound(const SoundRef& ref) {
    closeViewer();
    showTextView(false);
    stopSound();
    g_playable.clear();
    g_comboMode = COMBO_NONE;
    ShowWindow(g_lodCombo, SW_HIDE);
    showViewerControls(false);
    showSoundControls(true);

    if (ref.entry < 0 || (size_t)ref.entry >= g_all.size()) return;
    const Entry& bankEntry = g_all[ref.entry];
    Bytes data;
    if (!readAsset(bankEntry, data)) return;
    nfsnl::Bank bank;
    if (!nfsnl::readBank(data.data(), data.size(), bank)) return;
    if (ref.index < 0 || (size_t)ref.index >= bank.entries.size()) return;
    const nfsnl::BankEntry& be = bank.entries[ref.index];
    if (be.offset + be.size > data.size()) return;
    const uint8_t* p = data.data() + be.offset;

    char head[640];
    if (be.kind == "gnsu") {
        nfsnl::GnsuInfo g;
        nfsnl::readGnsuInfo(p, be.size, g);
        bool ok = soundToWav(p, be.size, g_playable);
        if (!ok) g_playable.clear();
        snprintf(head, sizeof(head),
                 "%s\r\n  sound %u, %u bytes\r\n\r\nGranular engine bank (EA Gnsu)\r\n"
                 "  rev range   %.0f - %.0f rpm\r\n  sample rate %u Hz\r\n"
                 "  grains      %u\r\n  length      %.1f s\r\n\r\nThe game picks grains "
                 "by engine speed. %s",
                 bankEntry.path.c_str(), be.id, (unsigned)be.size,
                 g.minRpm, g.maxRpm, g.sampleRate, g.grainCount,
                 g.sampleRate ? (double)g.totalSamples / g.sampleRate : 0.0,
                 ok ? "Press Play to hear them all in order, idle to redline; "
                      "double-click saves it as .wav."
                    : "It did not decode; double-click saves the .gnsu.");
        g_previewText = head;
        setTextView(g_previewText);
        EnableWindow(g_playBtn, !g_playable.empty());
        InvalidateRect(g_main, &g_previewRect, TRUE);
        return;
    }

    bool ready = soundToWav(p, be.size, g_playable);
    snprintf(head, sizeof(head),
             "%s\r\n  sound %u, %u bytes\r\n\r\n%s, %u ch, %u Hz\r\n\r\n%s",
             bankEntry.path.c_str(), be.id, (unsigned)be.size,
             nfsnl::wemCodecName(be.codec), be.channels, be.sampleRate,
             ready ? "Ready - press Play, or double-click to save it as .wav."
                   : "Wwise Vorbis. Use File > Locate vgmstream-cli.exe and this "
                     "plays and saves as .wav; double-click saves the .wem for now.");
    g_previewText = head;
    setTextView(g_previewText);
    EnableWindow(g_playBtn, !g_playable.empty());
    InvalidateRect(g_main, &g_previewRect, TRUE);
}

// ----------------------------------------------------------------- preview

static void showTextView(bool show) {
    g_textActive = show;
    ShowWindow(g_textBox, show ? SW_SHOW : SW_HIDE);
}

static void setTextView(const std::string& text) {
    // a very long dump would make the control crawl; the file export has all
    // of it, so the view shows the first part and says so
    const size_t kMax = 400000;
    std::string t = text;
    if (t.size() > kMax) {
        t.resize(kMax);
        t += "\r\n...\r\n(shortened for display - save as .txt for the whole dump)";
    }
    SetWindowTextW(g_textBox, widen(t).c_str());
    showTextView(true);
}

// Real Racing 3 keeps a model's wheel, steering and mirror positions in a
// .points file beside it. Find that file in the library, if it is there.
static bool readSidecarPoints(const std::string& modelPath, Bytes& out) {
    out.clear();
    if (nfsnl::extensionOf(modelPath) != "m3g") return false;
    std::string want = nfsnl::rr3PointsNameFor(modelPath);
    if (want == modelPath) return false;
    for (size_t i = 0; i < g_all.size(); ++i)
        if (g_all[i].path == want) return readAsset(g_all[i], out);
    // the model may name a folder the points file does not repeat
    auto lower = [](std::string x) {
        for (char& c : x) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return x;
    };
    std::string leaf = lower(nfsnl::baseName(want));
    for (size_t i = 0; i < g_all.size(); ++i)
        if (lower(nfsnl::baseName(g_all[i].path)) == leaf) return readAsset(g_all[i], out);
    // An interior with no <car>_int.points of its own: some cars keep the
    // cockpit's points (steering wheel, needles, gearstick) in the car's
    // main .points, so try that before giving up.
    std::string stem = nfsnl::stripExtension(leaf);
    if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, "_int") == 0) {
        std::string outer = stem.substr(0, stem.size() - 4) + ".points";
        for (size_t i = 0; i < g_all.size(); ++i)
            if (lower(nfsnl::baseName(g_all[i].path)) == outer) return readAsset(g_all[i], out);
    }
    // Real Racing GTI: car_2door_gti.m3g uses car_gti.points, which only the
    // car's .rr_car record names
    std::string modelLeaf = nfsnl::baseName(modelPath);
    for (size_t i = 0; i < g_all.size(); ++i) {
        if (nfsnl::extensionOf(g_all[i].path) != "rr_car") continue;
        Bytes raw;
        nfsnl::Rr1Car c;
        if (!readAsset(g_all[i], raw) || !nfsnl::rr1ReadCar(raw.data(), raw.size(), c)) continue;
        if (c.model != modelLeaf || c.points.empty()) continue;
        int pi = findAssetOne(c.points);
        if (pi >= 0) return readAsset(g_all[pi], out);
    }
    return false;
}

// Real Racing 3 does not ship wheels inside the car model - the exterior
// declares the wheel and tyre materials and uses neither - so a wheel has to
// come from another file and be placed at the car's own POINT_WHEEL_*. Look
// for one near the car; rr3AttachWheels refuses anything that is not actually
// disc-shaped, so a wrong guess costs nothing.
static bool findWheelModel(const std::string& modelPath, nfsnl::Model& out) {
    std::string dir = modelPath;
    size_t slash = dir.find_last_of('/');
    dir = slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);

    // The car a model belongs to: 2013_koenigsegg_agerar_a.m3g -> the stem
    // 2013_koenigsegg_agerar, which is what its sibling files are named after.
    // Only one letter comes off: the Megane is 2015_renault_megane_trophy_r,
    // and taking letters off in a loop lost its "_r" and with it the
    // _shared.m3g, so a stranger's wheel went on instead.
    std::string stem = nfsnl::stripExtension(nfsnl::baseName(modelPath));
    if (stem.size() > 2 && stem[stem.size() - 2] == '_' &&
        isalpha((unsigned char)stem[stem.size() - 1]))
        stem.erase(stem.size() - 2);
    for (char& c : stem) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');

    std::vector<int> shared, named, elsewhere;
    for (size_t i = 0; i < g_all.size(); ++i) {
        const std::string& p = g_all[i].path;
        if (p == modelPath) continue;
        std::string ext = nfsnl::extensionOf(p);
        if (ext != "m3g" && ext != "sb3d") continue;
        std::string leaf = nfsnl::baseName(p);
        for (char& c : leaf) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        bool sameDir = !dir.empty() && p.compare(0, dir.size(), dir) == 0;
        // Real Racing 3 keeps a car's wheels in <car>_shared.m3g - the car's
        // own model declares the wheel and tyre materials and uses neither.
        if (sameDir && leaf == nfsnl::baseName(stem) + "_shared.m3g") {
            shared.push_back((int)i);
            continue;
        }
        if (leaf.find("wheel") == std::string::npos &&
            leaf.find("rim") == std::string::npos) continue;
        if (sameDir) named.push_back((int)i);
        else elsewhere.push_back((int)i);
    }
    std::vector<int> candidates = shared;
    candidates.insert(candidates.end(), named.begin(), named.end());
    candidates.insert(candidates.end(), elsewhere.begin(), elsewhere.end());

    for (int idx : candidates) {
        Bytes raw;
        if (!readAsset(g_all[idx], raw)) continue;
        nfsnl::Model m = nfsnl::loadModel(raw.data(), raw.size());
        if (!m.valid || m.meshes.empty()) continue;

        // <car>_shared.m3g goes over whole: rr3AttachWheels picks the tyre,
        // rim, discs and the calipers for each corner out of it by name.
        if (std::find(shared.begin(), shared.end(), idx) != shared.end()) {
            out = std::move(m);
            return true;
        }

        // A shared model holds more than the wheel, so keep only the parts
        // that say they are one. If nothing says so, the whole model is the
        // candidate and rr3AttachWheels still refuses it unless it is a disc.
        nfsnl::Model wheelOnly;
        wheelOnly.materials = m.materials;
        for (const nfsnl::Mesh& mesh : m.meshes) {
            std::string tag = mesh.name + "|" + mesh.material;
            for (char& c : tag) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (tag.find("wheel") == std::string::npos &&
                tag.find("tyre") == std::string::npos &&
                tag.find("tire") == std::string::npos &&
                tag.find("rim") == std::string::npos &&
                tag.find("rotor") == std::string::npos) continue;
            if (tag.find("blur") != std::string::npos) continue;   // spin card
            wheelOnly.meshes.push_back(mesh);
        }
        if (!wheelOnly.meshes.empty()) {
            wheelOnly.valid = true;
            out = std::move(wheelOnly);
            return true;
        }
        out = std::move(m);
        return true;
    }
    return false;
}

// A part that moves has its rest pose in the animation that moves it:
// <car>_wing.banim beside <car>_a.m3g. Collect every such file and the part
// each one names.
static void readSidecarBanims(const std::string& modelPath,
                              std::vector<std::pair<std::string, Bytes>>& out) {
    out.clear();
    std::string stem = nfsnl::stripExtension(modelPath);
    if (stem.size() > 2 && stem[stem.size() - 2] == '_') stem.erase(stem.size() - 2);
    std::string prefix = stem + "_";
    for (size_t i = 0; i < g_all.size(); ++i) {
        const std::string& p = g_all[i].path;
        if (nfsnl::extensionOf(p) != "banim") continue;
        if (p.compare(0, prefix.size(), prefix) != 0) continue;
        std::string hint = p.substr(prefix.size());
        hint = nfsnl::stripExtension(hint);
        Bytes raw;
        if (readAsset(g_all[i], raw)) out.push_back({hint, std::move(raw)});
    }
}

// Everything that turns a bare model into a placed one, in one place so the
// viewer and the exporter cannot disagree about what a car looks like.
// No Limits keeps a car's wheels in models/cars/wheels/wheel_<car>.sb3d (a
// few cars keep it in their own folder). Found by name, the car's own folder
// first.
static bool findNlWheelModel(const std::string& carPath, nfsnl::Model& out) {
    auto lower = [](std::string x) {
        for (char& c : x) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return x;
    };
    std::string want = lower(nfsnl::nlWheelNameFor(carPath));
    std::string dir = carPath.substr(0, carPath.find_last_of('/') + 1);
    int best = -1;
    for (size_t i = 0; i < g_all.size(); ++i) {
        if (lower(nfsnl::baseName(g_all[i].path)) != want) continue;
        if (best < 0 || g_all[i].path.compare(0, dir.size(), dir) == 0) best = (int)i;
    }
    // No wheel under the car's exact name: the closest one by name, token by
    // token from the front - make and model at least, so honda_s2000_gt
    // takes wheel_honda_s2000 but never another make's wheel.
    if (best < 0) {
        auto tokens = [](const std::string& x) {
            std::vector<std::string> t;
            size_t a = 0;
            while (a <= x.size()) {
                size_t b = x.find('_', a);
                t.push_back(x.substr(a, b == std::string::npos ? std::string::npos : b - a));
                if (b == std::string::npos) break;
                a = b + 1;
            }
            return t;
        };
        std::vector<std::string> car = tokens(lower(nfsnl::stripExtension(nfsnl::baseName(carPath))));
        size_t bestScore = 1;
        for (size_t i = 0; i < g_all.size(); ++i) {
            std::string leaf = lower(nfsnl::baseName(g_all[i].path));
            if (leaf.compare(0, 6, "wheel_") || nfsnl::extensionOf(leaf) != "sb3d") continue;
            std::vector<std::string> w = tokens(nfsnl::stripExtension(leaf.substr(6)));
            size_t k = 0;
            while (k < w.size() && k < car.size() && w[k] == car[k]) ++k;
            if (k > bestScore) { bestScore = k; best = (int)i; }
        }
    }
    if (best < 0) return false;
    logLine("wheels for %s: %s", carPath.c_str(), g_all[best].path.c_str());
    Bytes raw;
    if (!readAsset(g_all[best], raw)) return false;
    out = nfsnl::loadModel(raw.data(), raw.size());
    if (!out.valid || out.meshes.empty()) return false;
    // say which wheel was used and where it lives, so a wrong one can be
    // traced to its pack
    std::string src = sourceFileOf(g_all[best]);
    out.warnings.insert(out.warnings.begin(), "wheel: " + nfsnl::baseName(g_all[best].path) +
                        (src.empty() ? std::string() : " from " + nfsnl::baseName(src)));
    return true;
}

// Real Racing 3 textures by file stem: "1979_porsche_935_misc" finds
// 1979_porsche_935_misc.etc.dds (or the .pvr / .dxt.dds a different build
// ships); a stem ending in '*' takes the first file that starts with it.
// every texture in the library by lower-case file stem, built once per
// library: a track asks for hundreds of names, and walking forty thousand
// entries for each of them took long enough to look like a hang
static std::multimap<std::string, int> g_stemIndex;
static size_t g_stemIndexFor = (size_t)-1;
static std::multimap<std::string, std::string> g_folderIndex;
static void buildStemIndex() {
    if (g_stemIndexFor == g_all.size()) return;
    g_stemIndex.clear();
    g_folderIndex.clear();
    g_stemIndexFor = g_all.size();
    for (size_t i = 0; i < g_all.size(); ++i) {
        std::string leaf = nfsnl::baseName(g_all[i].path);
        for (char& c : leaf) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        size_t dot = leaf.find('.');
        if (dot == std::string::npos) continue;
        std::string ext = nfsnl::extensionOf(leaf);
        if (ext != "dds" && ext != "pvr" && ext != "ktx" && ext != "png" && ext != "jpg" &&
            ext != "sba")
            continue;
        g_stemIndex.emplace(leaf.substr(0, dot), (int)i);
        std::string key = nfsnl::rr3FolderKey(g_all[i].path);
        if (!key.empty()) g_folderIndex.emplace(key, g_all[i].path);
    }
}

static std::string findTextureByStem(const std::string& stem) {
    bool prefix = !stem.empty() && stem.back() == '*';
    std::string want = prefix ? stem.substr(0, stem.size() - 1) : stem;
    buildStemIndex();
    if (!prefix) {
        int best = -1;
        auto range = g_stemIndex.equal_range(want);
        for (auto it = range.first; it != range.second; ++it) {
            bool etc = g_all[it->second].path.find(".etc.") != std::string::npos;
            if (best < 0 || (etc && g_all[best].path.find(".etc.") == std::string::npos))
                best = it->second;
        }
        if (best < 0) return nfsnl::rr3FolderTexture(want, g_folderIndex);
        return g_all[best].path;
    }
    // a prefix: the stems in order, from the first that starts with it
    // (one search instead of a walk through every asset)
    static std::vector<std::pair<std::string, int>> sorted;
    static size_t sortedFor = (size_t)-1;
    if (sortedFor != g_stemIndexFor) {
        sorted.clear();
        sorted.reserve(g_stemIndex.size());
        for (const auto& kv : g_stemIndex) sorted.push_back(kv);
        std::sort(sorted.begin(), sorted.end());
        sortedFor = g_stemIndexFor;
    }
    int best = -1, bestRank = 1 << 30;
    std::string bestLeaf;
    for (auto it = std::lower_bound(sorted.begin(), sorted.end(), std::make_pair(want, -1));
         it != sorted.end() && it->first.compare(0, want.size(), want) == 0; ++it) {
        if (it->first.find("shadow") != std::string::npos) continue;
        int i = it->second;
        std::string leaf = nfsnl::baseName(g_all[i].path);
        for (char& ch : leaf) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        int rank = leaf.find(".etc.") != std::string::npos ? 0 : 1;
        if (best < 0 || rank < bestRank || (rank == bestRank && leaf < bestLeaf)) {
            best = i;
            bestRank = rank;
            bestLeaf = leaf;
        }
    }
    return best < 0 ? std::string() : g_all[best].path;
}

// Real Racing 3's shared textures, vehicles/common/...: lower-case file stem
// and asset path. Built once per library.
static std::vector<std::pair<std::string, std::string>> g_rr3Common;
static size_t g_rr3CommonFor = (size_t)-1;
static const std::vector<std::pair<std::string, std::string>>& commonTextures() {
    if (g_rr3CommonFor == g_all.size()) return g_rr3Common;
    g_rr3Common.clear();
    g_rr3CommonFor = g_all.size();
    for (size_t i = 0; i < g_all.size(); ++i) {
        std::string p = g_all[i].path;
        for (char& c : p) { if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); if (c == '\\') c = '/'; }
        if (p.compare(0, 7, "common/") != 0 && p.find("/common/") == std::string::npos) continue;
        std::string ext = nfsnl::extensionOf(p);
        if (ext != "dds" && ext != "pvr" && ext != "ktx" && ext != "png" && ext != "jpg") continue;
        std::string leaf = p.substr(p.find_last_of('/') + 1);
        g_rr3Common.push_back({ leaf.substr(0, leaf.find('.')), g_all[i].path });
    }
    return g_rr3Common;
}

// <car>.liveries.bin(.nct): the car's texture table
static bool readCarLiveries(const std::string& modelPath, nfsnl::NctLiveries& out) {
    std::string car = nfsnl::stripExtension(nfsnl::baseName(modelPath));
    for (char& c : car) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (car.size() > 2 && car[car.size() - 2] == '_') car.erase(car.size() - 2);
    if (car.size() > 4 && car.compare(car.size() - 4, 4, "_int") == 0) car.erase(car.size() - 4);
    std::string want = car + ".liveries";
    for (size_t i = 0; i < g_all.size(); ++i) {
        std::string leaf = nfsnl::baseName(g_all[i].path);
        for (char& c : leaf) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (leaf.compare(0, want.size(), want) != 0) continue;
        Bytes raw, plain;
        if (!readAsset(g_all[i], raw)) continue;
        if (!nfsnl::nctTransform(raw.data(), raw.size(), plain, nullptr)) continue;
        out = nfsnl::nctReadLiveries(plain.data(), plain.size());
        if (out.ok) return true;
    }
    return false;
}

// the cockpit's .points beside an exterior model, for the steering wheel
static bool readInteriorPoints(const std::string& modelPath, Bytes& out) {
    out.clear();
    if (nfsnl::extensionOf(modelPath) != "m3g") return false;
    std::string want = nfsnl::rr3InteriorPointsNameFor(modelPath);
    if (want.empty()) return false;
    for (size_t i = 0; i < g_all.size(); ++i)
        if (g_all[i].path == want) return readAsset(g_all[i], out);
    std::string leaf = nfsnl::baseName(want);
    for (char& c : leaf) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    for (size_t i = 0; i < g_all.size(); ++i) {
        std::string l = nfsnl::baseName(g_all[i].path);
        for (char& c : l) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (l == leaf) return readAsset(g_all[i], out);
    }
    return false;
}

// The game folder's files by name, for the NFS Undercover / Shift lookups.
// Built once per library.
static std::map<std::string, size_t> g_leafIndex;
static size_t g_leafIndexFor = (size_t)-1;
static nfsnl::EaPhoneFiles eaPhoneFiles() {
    if (g_leafIndexFor != g_all.size()) {
        g_leafIndex.clear();
        for (size_t i = 0; i < g_all.size(); ++i) g_leafIndex.emplace(nfsnl::baseName(g_all[i].path), i);
        g_leafIndexFor = g_all.size();
    }
    nfsnl::EaPhoneFiles f;
    for (auto& kv : g_leafIndex) f.leaves.push_back(kv.first);
    f.find = [](const std::string& leaf) {
        auto it = g_leafIndex.find(leaf);
        return it == g_leafIndex.end() ? std::string() : g_all[it->second].path;
    };
    f.read = [](const std::string& path, Bytes& out) {
        auto it = g_leafIndex.find(nfsnl::baseName(path));
        if (it != g_leafIndex.end() && g_all[it->second].path == path) return readAsset(g_all[it->second], out);
        for (size_t i = 0; i < g_all.size(); ++i) if (g_all[i].path == path) return readAsset(g_all[i], out);
        return false;
    };
    return f;
}

// a No Limits track: textures by material name, from the environment textures
static void textureNlScene(nfsnl::Model& m, const std::string& path) {
    static std::vector<std::pair<std::string, std::string>> envTex;
    static size_t envFor = (size_t)-1;
    if (envFor != g_all.size()) {
        envTex.clear();
        envFor = g_all.size();
        for (const Entry& t : g_all) {
            if (nfsnl::extensionOf(t.path) != "sba" || t.path.find("textures") == std::string::npos ||
                t.path.find(".scene_static") != std::string::npos)
                continue;
            std::string leaf = nfsnl::baseName(t.path);
            for (char& c : leaf) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            envTex.push_back({ leaf.substr(0, leaf.find('.')), t.path });
        }
    }
    // The region's own material list first: <scene>.lit_scene_<light>.
    // scene_static.lightmaps.sba names each part's diffuse texture. The
    // day lighting is preferred; any will do, the textures are the same.
    int exact = 0;
    {
        // Matched by the region's own name, wherever the file is: the
        // lightmaps live under texture_etc/prefabs/tracks/, not beside
        // the scene in prefabs/tracks/.
        std::string base = path.substr(0, path.find(".scene_static"));
        std::string leafBase = nfsnl::baseName(base);
        std::string dirOf = base.substr(0, base.size() - leafBase.size());
        std::string pick;
        int pickScore = -1;
        for (const Entry& t : g_all) {
            const std::string& q = t.path;
            if (q.find(".scene_static.lightmaps.sba") == std::string::npos) continue;
            std::string ql = nfsnl::baseName(q);
            if (ql.size() <= leafBase.size() + 1 || ql.compare(0, leafBase.size(), leafBase) != 0 ||
                ql[leafBase.size()] != '.')
                continue;
            int score = (q.compare(0, base.size(), base) == 0 ? 4 : 0) +
                        (q.find("_day") != std::string::npos ? 2 : 0) +
                        (q.find(dirOf.empty() ? std::string("/") : dirOf.substr(dirOf.find('/') + 1)) !=
                                 std::string::npos ? 1 : 0);
            if (score > pickScore) { pick = q; pickScore = score; }
        }
        int idx = pick.empty() ? -1 : findAsset(pick);
        Bytes lm;
        if (idx >= 0 && readAsset(g_all[idx], lm)) {
            std::vector<std::string> vars = nfsnl::nlReadMaterialVars(lm.data(), lm.size());
            exact = nfsnl::nlApplyMaterialVars(m, vars, [](const std::string& leaf) {
                std::string stem = leaf.substr(0, leaf.find('.'));
                return findTextureByStem(stem);
            });
            char b[200];
            snprintf(b, sizeof(b), "%d part(s) textured from %s", exact,
                     nfsnl::baseName(pick).c_str());
            m.warnings.push_back(b);
        } else {
            // no material list: the "#materialvars#N" placeholders go
            for (nfsnl::Mesh& me : m.meshes)
                if (me.texture.compare(0, 14, "#materialvars#") == 0) me.texture.clear();
            m.warnings.push_back("no .lightmaps.sba for this region in the library (it names the "
                                 "textures) - textures matched by material name instead");
        }
    }
    int n = nfsnl::nlAssignSceneTextures(m, path, envTex);
    if (n) {
        char b[128];
        snprintf(b, sizeof(b), "%d part(s) matched to a texture by material name", n);
        m.warnings.push_back(b);
    }
}

static void applySidecars(nfsnl::Model& m, const std::string& path,
                          const Bytes& points) {
    // NFS Undercover, Shift and Shift 2: standard M3G scenes. What the game
    // binds to them by name: a car's textures and wheels, a cockpit's
    // textures, a location's layout and atlases, a sky's picture.
    if (!m.meshes.empty() && m.meshes[0].part.compare(0, 5, "part_") == 0 &&
        m.meshes[0].material.compare(0, 4, "m3g_") == 0) {
        std::string leaf = nfsnl::stripExtension(nfsnl::baseName(path));
        nfsnl::eaPhoneDress(m, leaf, eaPhoneFiles());
        if (!m.images.empty()) {
            char b[96];
            snprintf(b, sizeof(b), "%u texture(s) inside the file", (unsigned)m.images.size());
            m.warnings.push_back(b);
        }
        return;
    }
    // Most Wanted's downloadable cars: their textures by the car's name
    if (nfsnl::extensionOf(path) == "m3g")
        nfsnl::mwFillCarTextures(m, nfsnl::stripExtension(nfsnl::baseName(path)));
    // Real Racing 3's driver: his parts posed by driver[_<car>].banim
    {
        std::string da = nfsnl::rr3DriverAnimNameFor(path);
        if (!da.empty()) {
            Bytes db;
            int di = findAssetOne(da);
            if (di < 0) di = findAssetOne(da.substr(0, da.size() - nfsnl::baseName(da).size()) + "driver.banim");
            if (di >= 0 && readAsset(g_all[di], db)) nfsnl::rr3PoseDriver(m, db.data(), db.size());
        }
    }
    if (nfsnl::extensionOf(path) == "sb3d" &&
        nfsnl::baseName(path).compare(0, 6, "wheel_") != 0) {
        // the car's prefab: the tyre size the game gives the wheel, and the
        // models it hangs on the car (a cop car's light bar)
        Bytes prefab;
        {
            std::string car = nfsnl::stripExtension(nfsnl::baseName(path));
            std::string want = "prefabs/cars/" + car + ".prefabs.sb";
            for (size_t i = 0; i < g_all.size() && prefab.empty(); ++i) {
                const std::string& q = g_all[i].path;
                if (q.size() < want.size() || q.compare(q.size() - want.size(), want.size(), want) != 0)
                    continue;
                Bytes raw;
                if (readAsset(g_all[i], raw)) nfsnl::nlSbDecode(nfsnl::baseName(q), raw.data(), raw.size(), prefab);
            }
        }
        if (!prefab.empty()) {
            int hung = 0;
            for (const nfsnl::PrefabModel& pm : nfsnl::nlPrefabModels(prefab.data(), prefab.size())) {
                std::string ref = pm.file;
                if (ref.compare(0, 11, "/published/") == 0) ref.erase(0, 11);
                while (!ref.empty() && ref[0] == '/') ref.erase(0, 1);
                if (nfsnl::stripExtension(nfsnl::baseName(ref)) == nfsnl::stripExtension(nfsnl::baseName(path)) ||
                    nfsnl::baseName(ref).compare(0, 6, "wheel_") == 0)
                    continue;
                int idx = findAssetOne(ref);
                Bytes raw;
                if (idx < 0 || !readAsset(g_all[idx], raw)) {
                    m.warnings.push_back(nfsnl::baseName(ref) + " (from the car's prefab) is not in the library");
                    continue;
                }
                nfsnl::Model extra = nfsnl::loadModel(raw.data(), raw.size());
                if (!extra.valid) {
                    m.warnings.push_back(nfsnl::baseName(ref) + " (from the car's prefab) did not load" +
                                         (extra.warnings.empty() ? std::string() : ": " + extra.warnings[0]));
                    continue;
                }
                for (nfsnl::Mesh& me : extra.meshes) {
                    if (me.lod == "HELPERS") continue;
                    for (size_t k = 0; k + 2 < me.positions.size(); k += 3)
                        for (int a = 0; a < 3; ++a) me.positions[k + a] += pm.pos[a];
                    for (int a = 0; a < 3; ++a) { me.bboxMin[a] += pm.pos[a]; me.bboxMax[a] += pm.pos[a]; }
                    me.part = nfsnl::stripExtension(nfsnl::baseName(ref));
                    me.kit = 0;
                    me.kitSlot.clear();
                    m.meshes.push_back(std::move(me));
                }
                for (const nfsnl::Material& mt : extra.materials) m.materials.push_back(mt);
                ++hung;
                m.warnings.push_back(nfsnl::baseName(ref) + " hung on the car where its prefab puts it");
            }
            (void)hung;
        }
        nfsnl::Model wheel;
        if (findNlWheelModel(path, wheel)) {
            if (!wheel.warnings.empty()) m.warnings.push_back(wheel.warnings.front());
            float radii[2] = { 0, 0 };
            bool haveRadii = !prefab.empty() && nfsnl::nlPrefabWheelRadii(prefab.data(), prefab.size(), radii);
            // data/visualparts/C_<car>_1.sb: how the stock arch widens the tyre
            nfsnl::NlWheelTweak tweak;
            {
                std::string car = nfsnl::stripExtension(nfsnl::baseName(path));
                for (size_t i = 0; i < g_all.size() && !tweak.valid; ++i) {
                    const std::string& q = g_all[i].path;
                    if (q.find("visualparts/") == std::string::npos ||
                        !nfsnl::nlVisualPartsMatches(nfsnl::baseName(q), car))
                        continue;
                    Bytes raw, sb;
                    if (readAsset(g_all[i], raw) &&
                        nfsnl::nlSbDecode(nfsnl::baseName(q), raw.data(), raw.size(), sb))
                        nfsnl::nlVisualPartsWheelTweak(sb.data(), sb.size(), tweak);
                }
            }
            nfsnl::nlAttachWheels(m, wheel, haveRadii ? radii : nullptr, &tweak);
            if (tweak.valid && (tweak.widthOffset[0] != 0 || tweak.widthOffset[1] != 0)) {
                char b[160];
                snprintf(b, sizeof(b), "tyres widened as the car's visual parts say: +%.0f mm front, +%.0f mm rear",
                         tweak.widthOffset[0] * 1000, tweak.widthOffset[1] * 1000);
                m.warnings.push_back(b);
            }
            if (haveRadii) {
                char b[160];
                snprintf(b, sizeof(b), "tyre radius from the car's prefab: %.3f m front, %.3f m rear",
                         radii[0], radii[1]);
                m.warnings.push_back(b);
            }
        }
    }
    // Real Racing Next: vehicles/<car>/<car>.sb3d names its atlases the Real
    // Racing 3 way (paint, misc, badges, lights, cab ...), and the skins
    // (.sbsk) point each at vehicles/<car>/car_textures/<car>_<atlas>.sba;
    // paint and combined use <car>_ext.sba. Tiled materials (leather,
    // carbon_fiber_interior ...) come from vehicles/common_textures.
    if (nfsnl::extensionOf(path) == "sb3d" && path.find("vehicles/") != std::string::npos) {
        std::string dir = path.substr(0, path.find_last_of('/'));
        std::string car = nfsnl::baseName(dir);
        int got = 0;
        std::map<std::string, std::string> memo;
        for (nfsnl::Mesh& me : m.meshes) {
            if (me.material.empty()) continue;
            if (!me.texture.empty() && findAsset(me.texture) >= 0) continue;
            std::string mat = me.material;
            for (char& c : mat) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            auto hit = memo.find(mat);
            if (hit == memo.end()) {
                std::vector<std::string> cand = nfsnl::rrNextTextureCandidates(car, mat);
                std::string found;
                for (const std::string& c : cand) {
                    int idx = findAssetOne(c);
                    if (idx >= 0 && nfsnl::baseName(g_all[idx].path) == c) { found = g_all[idx].path; break; }
                }
                hit = memo.emplace(mat, found).first;
            }
            if (!hit->second.empty()) { me.texture = hit->second; ++got; }
        }
        if (got) {
            char b[160];
            snprintf(b, sizeof(b), "%d part(s) textured from vehicles/%s/car_textures and common_textures",
                     got, car.c_str());
            m.warnings.push_back(b);
        }
        // the wheel, modelled once at the origin, onto the four arches
        int cpi = findAssetOne(dir + "/" + car + "_carpoints.sb");
        Bytes cp;
        if (cpi >= 0 && readAsset(g_all[cpi], cp)) nfsnl::rrNextAttachWheels(m, cp.data(), cp.size());
    }
    // NFS Edge: an .m3g car with the rig's wheel joints, and its wheels in
    // models/cars/wheels/wheel_<car>.m3g
    if (nfsnl::extensionOf(path) == "m3g" && points.empty() &&
        nfsnl::baseName(path).compare(0, 6, "wheel_") != 0) {
        bool joints = false, ownWheels = false;
        for (const auto& h : m.points) if (h.name.rfind("J_wheel_", 0) == 0) joints = true;
        // (Most Wanted's cars carry their wheels themselves)
        for (const auto& me : m.meshes) {
            std::string n = me.name;
            for (char& c : n) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (n.find("arch") == std::string::npos &&
                (n.find("wheel") != std::string::npos || n.find("tire") != std::string::npos ||
                 n.find("tyre") != std::string::npos))
                ownWheels = true;
        }
        if (joints && !ownWheels) {
            std::string want = "wheel_" + nfsnl::baseName(path);
            for (char& c : want) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            int found = -1;
            for (size_t i = 0; i < g_all.size() && found < 0; ++i) {
                std::string leaf = nfsnl::baseName(g_all[i].path);
                for (char& c : leaf) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                if (leaf == want) found = (int)i;
            }
            Bytes raw;
            if (found >= 0 && readAsset(g_all[found], raw)) {
                nfsnl::Model wheel = nfsnl::loadModel(raw.data(), raw.size());
                if (wheel.valid) nfsnl::nlAttachWheels(m, wheel);
            } else {
                m.warnings.push_back(want + " is not in the library - no wheels");
            }
        }
    }
    if (!points.empty()) {
        std::vector<nfsnl::Hardpoint> pts =
            nfsnl::rr3ReadPoints(points.data(), points.size());
        if (!pts.empty()) {
            // the steering wheel's point is in the cockpit's file
            Bytes inner;
            std::string innerName = nfsnl::baseName(nfsnl::rr3InteriorPointsNameFor(path));
            bool hadSteer = false;
            for (const auto& h : pts) if (h.name == "POINT_STEERING_WHEEL") hadSteer = true;
            if (!hadSteer && !innerName.empty()) {
                if (readInteriorPoints(path, inner)) {
                    std::vector<nfsnl::Hardpoint> ipts =
                        nfsnl::rr3ReadPoints(inner.data(), inner.size());
                    nfsnl::rr3MergeCockpitPoints(pts, ipts);
                    bool got = false;
                    for (const auto& h : pts) if (h.name == "POINT_STEERING_WHEEL") got = true;
                    std::string names;
                    for (size_t i = 0; i < ipts.size() && i < 12; ++i)
                        names += (i ? ", " : "") + ipts[i].name;
                    m.warnings.push_back(got ? "steering point taken from " + innerName
                                             : innerName + " has no steering point (" +
                                               std::to_string(ipts.size()) + " points: " + names + ")");
                } else {
                    m.warnings.push_back(innerName + " is not in the library");
                }
            }
            nfsnl::rr3PlaceParts(m, pts);
            bool wantsWheels = false;
            for (const auto& h : pts)
                if (h.name.rfind("POINT_WHEEL", 0) == 0) wantsWheels = true;
            nfsnl::Model wheel;
            // Real Racing 1 / GTI: the .rr_car record names the car's wheel
            // model and its textures
            nfsnl::Rr1Car car;
            bool haveCar = false;
            bool isRr1 = nfsnl::rr1IsCar(m);
            if (isRr1) {
                std::string leaf = nfsnl::baseName(path);
                for (size_t i = 0; i < g_all.size() && !haveCar; ++i) {
                    if (nfsnl::extensionOf(g_all[i].path) != "rr_car") continue;
                    Bytes raw;
                    nfsnl::Rr1Car c;
                    if (readAsset(g_all[i], raw) && nfsnl::rr1ReadCar(raw.data(), raw.size(), c) &&
                        c.model == leaf) { car = c; haveCar = true; }
                }
                nfsnl::rr1OrganizeCar(m, pts, haveCar ? &car : nullptr);
                if (haveCar) m.warnings.push_back("\"" + car.name + "\": textures and wheel from its .rr_car");
            }
            bool gotWheel = false;
            if (haveCar && !car.wheelModel.empty()) {
                int wi = findAssetOne(car.wheelModel);
                Bytes raw;
                if (wi >= 0 && readAsset(g_all[wi], raw)) {
                    wheel = nfsnl::loadModel(raw.data(), raw.size());
                    // the .rr_car often names a texture the game no longer
                    // ships (muscle_wheel.pvr for every exotic): the wheel
                    // model's own <name>.pvr beside it is the one it uses
                    std::string own = nfsnl::stripExtension(car.wheelModel) + ".pvr";
                    std::string tex = findAssetOne(own) >= 0 ? own : car.wheelTexture;
                    for (nfsnl::Mesh& me : wheel.meshes) if (me.texture.empty()) me.texture = tex;
                    gotWheel = wheel.valid;
                }
            }
            if (wantsWheels && (gotWheel || findWheelModel(path, wheel)))
                nfsnl::rr3AttachWheels(m, wheel, pts, isRr1);
        }
    }
    // Real Racing 2 pairs a model with its textures by file name
    nfsnl::rr2AssignTextures(m, path);
    if (path.find(".scene_static") != std::string::npos) textureNlScene(m, path);
    // Real Racing 3 by the material's _mm_ name
    if (nfsnl::extensionOf(path) == "m3g") {
        std::string report;
        nfsnl::NctLiveries liv;
        bool haveLiv = readCarLiveries(path, liv);
        int n = nfsnl::rr3AssignTextures(m, path, findTextureByStem, commonTextures(),
                                         haveLiv ? &liv : nullptr, &report);
        if (n) {
            char b[160];
            snprintf(b, sizeof(b), "%d part(s) textured (car folder, vehicles/common%s)", n,
                     haveLiv ? ", and the car's .liveries list" : "");
            m.warnings.push_back(b);
        }
        if (!report.empty()) m.warnings.push_back("no texture found for: " + report);
        // a Real Racing 3 track (processed/high/<track>.m3g): its sky, kept
        // one folder up as <track>_sky.m3g, drawn with it
        std::string norm = path;
        for (char& c : norm) if (c == '\\') c = '/';
        size_t hi = norm.rfind("/processed/high/");
        if (hi != std::string::npos && nfsnl::stripExtension(nfsnl::baseName(path)).find("_sky") == std::string::npos) {
            std::string skyPath = norm.substr(0, hi) + "/processed/" +
                                  nfsnl::stripExtension(nfsnl::baseName(path)) + "_sky.m3g";
            int si = -1;
            for (size_t i = 0; i < g_all.size() && si < 0; ++i) {
                std::string q = g_all[i].path;
                for (char& c : q) if (c == '\\') c = '/';
                std::string want = skyPath;
                for (char& c : want) if (c == '\\') c = '/';
                if (q == want) si = (int)i;
            }
            Bytes sb;
            if (si >= 0 && readAsset(g_all[si], sb)) {
                nfsnl::Model sky = nfsnl::loadModel(sb.data(), sb.size());
                if (sky.valid) {
                    nfsnl::rr3AssignTextures(sky, g_all[si].path, findTextureByStem, commonTextures(), nullptr, nullptr);
                    for (nfsnl::Mesh& me : sky.meshes) { me.name = "sky_" + me.name; m.meshes.push_back(std::move(me)); }
                    m.warnings.push_back("sky: " + nfsnl::baseName(g_all[si].path));
                }
            }
        }
    }
    std::vector<std::pair<std::string, Bytes>> banims;
    readSidecarBanims(path, banims);
    for (auto& b : banims)
        nfsnl::rr3PlaceAnimated(m, b.second.data(), b.second.size(), b.first);
}

// What a file says about itself, as text - but no pages of hex: a format the
// tool cannot read gets one plain line instead
static void showAssetText(const Entry& e, const Bytes& data) {
    std::string t = nfsnl::assetText(e.path, data.data(), data.size());
    size_t at = t.find("no readable structure");
    if (at != std::string::npos) {
        std::string ext = nfsnl::extensionOf(e.path);
        char b[200];
        snprintf(b, sizeof(b), "Monkey Tool does not support this format%s%s%s (%zu bytes).",
                 ext.empty() ? "" : " (.", ext.c_str(), ext.empty() ? "" : ")", data.size());
        std::string msg = b;
        if (t.find("This file is encrypted") != std::string::npos)
            msg += "\r\nIt is encrypted with a key only the game holds.";
        msg += "\r\nDouble-click saves the file as it is.";
        showTextView(false);
        g_previewText += "\r\n" + msg;
        return;
    }
    setTextView(t);
}


// ---- No Limits rim paint
static bool isNlRimTexture(const std::string& path) {
    std::string leaf = nfsnl::baseName(path);
    for (char& c : leaf) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return nfsnl::extensionOf(leaf) == "sba" && leaf.compare(0, 14, "texture_wheel_") == 0 &&
           leaf.find("reflection") == std::string::npos && leaf.find("normal") == std::string::npos &&
           leaf.find("tire") == std::string::npos && leaf.find("tyre") == std::string::npos;
}

static void showRimControls(bool show) {
    int cmd = show ? SW_SHOW : SW_HIDE;
    ShowWindow(g_rimLabel, cmd);
    ShowWindow(g_rimCombo, cmd);
    ShowWindow(g_rimApply, cmd);
}

static void rimShowSelection(int sel) {
    g_rimSel = sel;
    int custom = (int)g_rimColours.size() + 1;
    if (sel <= 0) {
        g_rimHave = false;
        g_preview = g_rimBase;
    } else {
        if (sel == custom) {
            CHOOSECOLORW cc{};
            cc.lStructSize = sizeof(cc);
            cc.hwndOwner = g_main;
            cc.lpCustColors = g_rimCustom;
            cc.rgbResult = RGB((int)(g_rimRgb[0] * 255), (int)(g_rimRgb[1] * 255), (int)(g_rimRgb[2] * 255));
            cc.Flags = CC_RGBINIT | CC_FULLOPEN;
            if (!ChooseColorW(&cc)) {
                SendMessageW(g_rimCombo, CB_SETCURSEL, g_rimHave ? g_rimSel : 0, 0);
                return;
            }
            g_rimRgb[0] = GetRValue(cc.rgbResult) / 255.0f;
            g_rimRgb[1] = GetGValue(cc.rgbResult) / 255.0f;
            g_rimRgb[2] = GetBValue(cc.rgbResult) / 255.0f;
            char nm[64];
            snprintf(nm, sizeof(nm), "custom #%02X%02X%02X", GetRValue(cc.rgbResult),
                     GetGValue(cc.rgbResult), GetBValue(cc.rgbResult));
            g_rimName = nm;
        } else if ((size_t)(sel - 1) < g_rimColours.size()) {
            const nfsnl::NlRimColour& c = g_rimColours[sel - 1];
            memcpy(g_rimRgb, c.rgb, sizeof(g_rimRgb));
            g_rimName = c.name;
        }
        g_rimHave = true;
        g_preview = g_rimBase;
        nfsnl::nlTintImage(g_preview, g_rimMask, g_rimRgb);
    }
    InvalidateRect(g_main, &g_previewRect, FALSE);
}

// a wheel texture was selected: the game's rim colours offered for it
static void setupRimPaint(const Entry& e, const nfsnl::Image& img) {
    if (!g_rimColoursRead) {
        g_rimColoursRead = true;
        int ci = findAssetOne("data/colours/colours.sb");
        if (ci < 0) ci = findAssetOne("data/colours.sb");
        Bytes raw, sb;
        if (ci >= 0 && readAsset(g_all[ci], raw) &&
            nfsnl::nlSbDecode(nfsnl::baseName(g_all[ci].path), raw.data(), raw.size(), sb))
            g_rimColours = nfsnl::nlRimColours(sb.data(), sb.size());
        logLine("rim colours: %zu", g_rimColours.size());
    }
    bool same = g_rimPath == e.path;
    g_rimPath = e.path;
    g_rimBase = img;
    g_rimMask.clear();
    // which texels are the paint: the wheel model's tinted meshes (its logos
    // and caps keep their colours); without the model, every grey texel
    std::string leaf = nfsnl::baseName(e.path);
    std::string stem = nfsnl::stripExtension(leaf).substr(8);          // wheel_x[_blur]
    if (stem.size() > 5 && stem.compare(stem.size() - 5, 5, "_blur") == 0) stem.erase(stem.size() - 5);
    int wi = findAssetOne(stem + ".sb3d");
    bool masked = false;
    if (wi >= 0) {
        Bytes raw;
        if (readAsset(g_all[wi], raw)) {
            nfsnl::Model wheel = nfsnl::loadModel(raw.data(), raw.size());
            masked = nfsnl::nlRimTintMask(wheel, leaf, img.width, img.height, g_rimMask);
            if (!masked) g_rimMask.clear();
        }
    }
    SendMessageW(g_rimCombo, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_rimCombo, CB_ADDSTRING, 0, (LPARAM)L"Original colours");
    for (const nfsnl::NlRimColour& c : g_rimColours) {
        std::string label = c.name;
        if (c.group != "CG_COMMON_RIM") {
            // CG_FERRARI_RIM -> Ferrari: ...
            std::string brand = c.group.size() > 7 ? c.group.substr(3, c.group.size() - 7) : c.group;
            for (size_t k = 0; k < brand.size(); ++k)
                if (k > 0 && brand[k] >= 'A' && brand[k] <= 'Z' && brand[k - 1] != '_') brand[k] = (char)(brand[k] - 'A' + 'a');
            label = brand + ": " + label;
        }
        SendMessageW(g_rimCombo, CB_ADDSTRING, 0, (LPARAM)widen(label).c_str());
    }
    SendMessageW(g_rimCombo, CB_ADDSTRING, 0, (LPARAM)L"Custom colour...");
    if (!same) { g_rimSel = 0; g_rimHave = false; }
    SendMessageW(g_rimCombo, CB_SETCURSEL, g_rimHave ? g_rimSel : 0, 0);
    if (g_rimHave) {
        g_preview = g_rimBase;
        nfsnl::nlTintImage(g_preview, g_rimMask, g_rimRgb);
    }
    showRimControls(true);
    char b[320];
    snprintf(b, sizeof(b), "\r\nCAR COLOR: %zu rim colour(s) from the game%s. Pick one, then "
             "\"Put into texture\" and Save.", g_rimColours.size(),
             masked ? " - painted where the wheel's tinted parts use this texture"
                    : " - no wheel model found, so every grey texel is painted");
    g_previewText += b;
}

// the painted texture into the game's file (pending until Save, like an import)
static bool importPictureInto(HWND hwnd, const Entry& e, const Bytes& original,
                              const nfsnl::Image& pic, const std::string& picPath,
                              std::string& reportOut, bool quiet);
static void putRimIntoTexture(HWND hwnd) {
    if (!g_rimHave) {
        MessageBoxW(hwnd, L"Pick a colour under CAR COLOR first.", L"Rim colour", MB_ICONINFORMATION);
        return;
    }
    const Entry* ent = nullptr;
    for (const Entry& x : g_all) if (x.path == g_rimPath) { ent = &x; break; }
    if (!ent) return;
    Entry e = *ent;
    Bytes original;
    std::string err;
    if (!readAsset(e, original, &err)) {
        MessageBoxW(hwnd, widen("Could not read " + e.path + ": " + err).c_str(), L"Rim colour", MB_ICONERROR);
        return;
    }
    nfsnl::Image painted = g_rimBase;
    nfsnl::nlTintImage(painted, g_rimMask, g_rimRgb);
    std::string report;
    int sel = g_rimSel;
    float rgb[3];
    memcpy(rgb, g_rimRgb, sizeof(rgb));
    std::string name = g_rimName;
    if (importPictureInto(hwnd, e, original, painted, "rim colour " + name, report, false)) {
        // the preview was rebuilt from the game's file: the pick shown again
        g_rimSel = sel;
        memcpy(g_rimRgb, rgb, sizeof(rgb));
        g_rimName = name;
        g_rimHave = true;
        SendMessageW(g_rimCombo, CB_SETCURSEL, sel, 0);
        g_preview = painted;
        InvalidateRect(g_main, &g_previewRect, FALSE);
        setStatus("Rim colour " + name + " put into " + nfsnl::baseName(e.path) + " - Save writes it");
    }
}

static void updatePreview(const Entry& e) {
    videoClose();
    g_preview = nfsnl::Image();
    g_previewWave = false;
    g_previewText.clear();
    closeViewer();
    showTextView(false);
    stopSound();
    g_playable.clear();
    g_soundActive = false;
    showSoundControls(false);
    g_comboMode = COMBO_NONE;
    g_bank = nfsnl::Bank();
    g_bankData.clear();
    ShowWindow(g_lodCombo, SW_HIDE);
    showRimControls(false);

    char head[MAX_PATH + 256];
    snprintf(head, sizeof(head), "%s\r\n%u bytes", e.path.c_str(), (unsigned)e.size);
    g_previewText = head;
    g_previewText += sourceLine(e);

    Bytes data;
    std::string readError;
    if (!readAsset(e, data, &readError)) {
        g_previewText += "\r\n";
        g_previewText += readError.empty() ? "(could not read asset)" : readError;
        InvalidateRect(g_main, &g_previewRect, TRUE);
        return;
    }
    std::string ext = nfsnl::extensionOf(e.path);
    if (isVideoExt(ext)) {
        // a film: it plays in the viewer's place, with the player's controls
        if (videoOpen(g_main, e.path, data)) {
            for (HWND c : { g_playBtn, g_stopBtn, g_seek, g_timeLabel }) ShowWindow(c, SW_SHOW);
            EnableWindow(g_playBtn, TRUE);
            SetWindowTextW(g_playBtn, L"Play");
            SetWindowTextW(g_timeLabel, L"0:00");
            g_previewText += "\r\nVideo - Play starts it, the bar seeks, a click on the picture pauses.";
        } else {
            videoOpenOutside(g_main, e.path, data);
            g_previewText += "\r\nVideo - Windows' own player has opened it (Media Foundation is not "
                             "available to the tool here).";
        }
        InvalidateRect(g_main, &g_previewRect, TRUE);
        return;
    }
    // No Limits' tracks: prefabs/tracks/<region>.scene_static.sba is geometry,
    // not a texture, whatever its extension says
    if (ext == "sba" && nfsnl::isNlScene(data.data(), data.size())) ext = "sb3d";
    if (ext == "sba" || ext == "pvr" || ext == "dds" || ext == "png" || ext == "jpg" || ext == "jpeg")
        g_previewText += "\r\nTools > Import PNG/JPG (Ctrl+I) puts your own picture into this texture.";
    if (ext == "png" || ext == "jpg" || ext == "jpeg") {
        nfsnl::Image img;
        char b2[160];
        if (nfsnl::decodeImageFile(data.data(), data.size(), img)) {
            g_preview = img;
            snprintf(b2, sizeof(b2), "\r\n%d x %d, %d channels", img.width, img.height, img.channels);
        } else {
            snprintf(b2, sizeof(b2), "\r\n(this picture did not decode)");
        }
        g_previewText += b2;
    } else if (ext == "sba") {
        auto entries = nfsnl::readSba(data.data(), data.size());
        if (!entries.empty()) {
            const auto& best = entries[0];
            char b2[256];
            snprintf(b2, sizeof(b2), "\r\n%d mip(s), payload is %s", (int)entries.size(),
                      best.format.c_str());
            g_previewText += b2;
            {
                // a No Limits UI texture pack: many named pictures on a few pages
                std::vector<nfsnl::TexturePackBox> boxes = nfsnl::readTexturePack(data.data(), data.size());
                if (!boxes.empty()) {
                    size_t vec = 0;
                    std::set<int> pages;
                    for (const auto& bx : boxes) { pages.insert(bx.blob); if (bx.name.size() > 4 && bx.name.compare(bx.name.size() - 4, 4, ".svg") == 0) ++vec; }
                    snprintf(b2, sizeof(b2), "\r\nUI texture pack: %u picture(s) on %u page(s)%s - the largest page is shown; "
                             "Save as PNG offers every picture under its own name.",
                             (unsigned)boxes.size(), (unsigned)pages.size(),
                             vec ? ", vector icons (.svg) not decoded" : "");
                    g_previewText += b2;
                }
            }
            nfsnl::Image img;
            if (nfsnl::decodeImageAuto(best, img)) {
                // NFS Edge: the alpha sits in <name>_ETCAlpha.sba beside it
                for (const std::string& comp : nfsnl::alphaCompanions(e.path)) {
                    int ai = findAssetOne(comp);
                    if (ai < 0 || g_all[ai].path != comp) continue;
                    Bytes araw;
                    nfsnl::Image alpha;
                    if (readAsset(g_all[ai], araw) &&
                        nfsnl::decodeTextureFile(araw.data(), araw.size(), alpha) &&
                        nfsnl::applyEtcAlpha(img, alpha)) {
                        g_previewText += "\r\nalpha taken from " + nfsnl::baseName(comp);
                        break;
                    }
                }
                g_preview = img;
                snprintf(b2, sizeof(b2), "\r\n%d x %d, %d channels", img.width, img.height, img.channels);
                g_previewText += b2;
                if (isNlRimTexture(e.path)) setupRimPaint(e, img);
            } else {
                g_previewText += "\r\n(no preview for this payload type)";
            }
        }
    } else if (ext == "pvr" || ext == "ktx") {
        // A bare PowerVR container: Real Racing 3's livery textures
        // (<car>_ext_NN.ptc.pvr, unwrapped from the .z) and loose .pvr files.
        // Decoded through the same call the save path uses, so the preview
        // and the written PNG match.
        nfsnl::SbaEntry pe;
        pe.format = "pvr";
        pe.data = data;
        nfsnl::Image img;
        char b2[256];
        if (data.size() >= 16 && !memcmp(data.data(), "PVR\x03", 4)) {
            uint32_t w = 0, h = 0;
            memcpy(&h, data.data() + 24, 4);
            memcpy(&w, data.data() + 28, 4);
            snprintf(b2, sizeof(b2), "\r\nPVR v3, %u x %u", w, h);
            g_previewText += b2;
        }
        if (nfsnl::decodeImageAuto(pe, img)) {
            g_preview = img;
            snprintf(b2, sizeof(b2), "\r\n%d x %d, %d channels - double-click to save as PNG",
                     img.width, img.height, img.channels);
            g_previewText += b2;
        } else {
            showAssetText(e, data);
            g_previewText += "\r\n(this PVR holds a pixel format with no decoder here)";
        }
    } else if (ext == "dds") {
        // Real Racing 3's textures: a DDS holding ETC1 blocks or RGBA4444
        nfsnl::Image img;
        if (nfsnl::decodeDdsContainer(data.data(), data.size(), img)) {
            // the same turn-over the save path does, so what is previewed and
            // what is written are the same way up
            nfsnl::flipImageVertically(img);
            // Real Racing 3's trees and sky: the see-through is <name>_alpha
            std::string alphaFrom;
            for (const std::string& comp : nfsnl::alphaCompanions(e.path)) {
                int ai = findAssetOne(comp);
                if (ai < 0 || g_all[ai].path != comp) continue;
                Bytes araw;
                nfsnl::Image alpha;
                if (readAsset(g_all[ai], araw) &&
                    nfsnl::decodeTextureFile(araw.data(), araw.size(), alpha) &&
                    nfsnl::applyEtcAlpha(img, alpha)) { alphaFrom = comp; break; }
            }
            g_preview = img;
            char b2[256];
            snprintf(b2, sizeof(b2), "\r\n%d x %d, %d channels", img.width,
                     img.height, img.channels);
            g_previewText += b2;
            if (!alphaFrom.empty()) g_previewText += "\r\nalpha taken from " + nfsnl::baseName(alphaFrom);
        } else {
            showAssetText(e, data);
            g_previewText += "\r\n(this DDS holds a codec with no decoder here)";
        }
    } else if (ext == "points" || ext == "nct" || ext == "gui") {
        showAssetText(e, data);
    } else if (ext == "sb3d" || ext == "m3g") {
        nfsnl::Model m = (ext == "sb3d")
                             ? nfsnl::loadSb3d(data.data(), data.size())
                             : nfsnl::loadModel(data.data(), data.size());
        if (m.valid) {
            // a Real Racing 3 model brings its hardpoints and its animated
            // parts' rest poses with it, in files beside it
            Bytes points;
            readSidecarPoints(e.path, points);
            applySidecars(m, e.path, points);
            for (const std::string& w : m.warnings) {
                g_previewText += "\r\n";
                g_previewText += w;
            }
            size_t nv = 0, nt = 0, uv = 0;
            for (auto& mesh : m.meshes) {
                nv += mesh.positions.size() / 3;
                nt += mesh.indices.size() / 3;
                if (mesh.uvs.size() / 2 == mesh.positions.size() / 3) uv++;
            }
            char b2[256];
            snprintf(b2, sizeof(b2), "\r\n%u mesh part(s), %u vertices, %u triangles, %u with UVs",
                      (unsigned)m.meshes.size(), (unsigned)nv, (unsigned)nt, (unsigned)uv);
            g_previewText += b2;
            g_previewText += "\r\nDrag to turn, right-drag to slide, wheel to zoom. "
                             "Ctrl+drag moves the model. Double-click the tree item to save as FBX or OBJ.";
            g_viewPath = e.path;
            openViewer(std::move(m));
        } else if (ext == "m3g" && [&]() {
                       nfsnl::Image img;
                       if (!nfsnl::decodeTextureFile(data.data(), data.size(), img)) return false;
                       g_preview = img;
                       char b3[160];
                       snprintf(b3, sizeof(b3), "\r\nA texture in an M3G file: %d x %d, %d channels"
                                " - double-click to save as PNG", img.width, img.height, img.channels);
                       g_previewText += b3;
                       return true;
                   }()) {
            // a texture shipped as .m3g (Hot Pursuit) - shown like any other
        } else {
            // No Limits' own .m3g is a compressed container, not a model, and
            // the text view explains what it is and what is missing.
            showAssetText(e, data);
            g_previewText += "\r\n";
            g_previewText += m.warnings.empty() ? "(model could not be read)" : m.warnings[0];
        }
    } else if (ext == "sounddef" || ext == "evt") {
        // Real Racing 3 data the Import editor changes: shown as that text,
        // and a sound definition plays the first .wav it names
        std::string text, why;
        if (nfsnl::dataEditableText(e.path, data.data(), data.size(), text, why)) {
            std::string crlf;
            for (char c : text) { if (c == '\n') crlf += '\r'; crlf += c; }
            setTextView(crlf);
        } else {
            showAssetText(e, data);
        }
        if (ext == "sounddef") {
            g_soundActive = true;
            g_playable.clear();
            for (const std::string& w : nfsnl::soundDefSamples(data.data(), data.size())) {
                int wi = findAssetOne(w);
                Bytes wav;
                if (wi < 0 || !readAsset(g_all[wi], wav)) continue;
                WAVEFORMATEX wf{};
                size_t off = 0, n = 0;
                if (parseWavPcm(wav, wf, off, n) && wf.wFormatTag == WAVE_FORMAT_PCM) g_playable = wav;
                else runVgmstream(wav, g_playable, ".wav");
                if (!g_playable.empty()) { g_previewText += "\r\nPlays " + w + " - press Play."; break; }
            }
            if (g_playable.empty()) g_previewText += "\r\nIts .wav files are not in the library (they are in sfx/).";
            showWaveform(g_playable);
            showSoundControls(true);
        }
        g_previewText += "\r\nImport (Ctrl+I) opens it in the editor and saves it back in the game's format.";
    } else if (ext == "wem" || ext == "bnk" || ext == "gnsu" || ext == "sps") {
        // no hex dump for a sound: its waveform once it decodes, below
        g_soundActive = true;
        std::string why;
        if (ext == "sps") {
            int codec = 0, ch = 0, rate = 0;
            uint32_t samples = 0;
            nfsnl::spsInfo(data.data(), data.size(), &codec, &ch, &rate, &samples);
            char b2[200];
            snprintf(b2, sizeof(b2), "\r\nEA SPS sound: %d channel(s), %d Hz, %.2f s", ch, rate,
                     rate ? (double)samples / rate : 0.0);
            g_previewText += b2;
            if (nfsnl::spsToWav(data.data(), data.size(), g_playable, &why)) {
                g_previewText += "\r\nDecoded here (EA-XAS) - press Play, or save it as a .wav.";
            } else if (runVgmstream(data, g_playable)) {
                g_previewText += "\r\nDecoded with vgmstream - press Play, or save it as a .wav.";
            } else {
                g_playable.clear();
                g_previewText += "\r\n" + why + " - use File > Locate vgmstream-cli.exe.";
            }
        } else if (ext == "gnsu") {
            if (nfsnl::gnsuToWav(data.data(), data.size(), g_playable, &why))
                g_previewText += "\r\nEngine bank decoded - press Play to hear its grains, "
                                 "idle to redline, or save it as a .wav.";
            else {
                g_playable.clear();
                g_previewText += "\r\n" + why;
            }
        } else if (ext == "wem") {
            if (nfsnl::wemToWav(data.data(), data.size(), g_playable, &why)) {
                g_previewText += "\r\nDecoded here - press Play, or save it as a .wav.";
            } else if (runVgmstream(data, g_playable)) {
                g_previewText += "\r\nDecoded with vgmstream - press Play, or save it as a .wav.";
            } else {
                g_playable.clear();
                g_previewText += "\r\nThis sound is Wwise Vorbis: the codebooks that "
                                 "decode it ship with the Wwise encoder, not with the "
                                 "game. Use File > Locate vgmstream-cli.exe, and it "
                                 "plays and saves as .wav from then on.";
            }
        } else {
            g_bank = nfsnl::Bank();
            g_bankData.clear();
            if (nfsnl::readBank(data.data(), data.size(), g_bank) &&
                !g_bank.entries.empty()) {
                g_bankData = data;
                char b2[160];
                snprintf(b2, sizeof(b2),
                         "\r\n%u sound(s) inside. Open this row in the tree to see "
                         "them listed, then double-click one to save it as .wav - "
                         "or double-click the bank itself to write them all out.",
                         (unsigned)g_bank.entries.size());
                g_previewText += b2;
                fillBankCombo();
                g_comboMode = COMBO_SOUNDS;
                ShowWindow(g_lodCombo, SW_SHOW);
                selectBankSound(0);
            }
        }
        showWaveform(g_playable);
        showSoundControls(true);
    } else if (ext == "fsb" || ext == "fev") {
        // FMOD: an .fsb bank holds the sounds (vgmstream decodes them one
        // subsong at a time); an .fev is the event project that plays them,
        // with its bank beside it under the same name
        g_soundActive = true;
        g_fsbData.clear();
        g_fsbNames.clear();
        if (ext == "fev") {
            std::vector<std::string> strs = nfsnl::fevStrings(data.data(), data.size());
            int events = 0, waves = 0;
            for (const std::string& t : strs) {
                if (nfsnl::extensionOf(t) == "wav") ++waves; else ++events;
            }
            char b2[200];
            snprintf(b2, sizeof(b2), "\r\nFMOD Designer project: %d name(s), %d wave file(s)", events, waves);
            g_previewText += b2;
            std::string bank = nfsnl::stripExtension(e.path) + ".fsb";
            int bi = findAssetOne(bank);
            if (bi >= 0 && readAsset(g_all[bi], g_fsbData))
                g_previewText += "\r\nits sounds are in " + nfsnl::baseName(bank);
            else
                g_previewText += "\r\n" + nfsnl::baseName(bank) + " (its sounds) is not in the library";
        } else {
            g_fsbData = data;
        }
        g_fsbNames = nfsnl::fsb5Names(g_fsbData.data(), g_fsbData.size());
        if (!g_fsbNames.empty()) {
            char b2[160];
            snprintf(b2, sizeof(b2), "\r\n%u sound(s) - pick one in the list and press Play.",
                     (unsigned)g_fsbNames.size());
            g_previewText += b2;
            SendMessageW(g_lodCombo, CB_RESETCONTENT, 0, 0);
            for (const std::string& n : g_fsbNames)
                SendMessageW(g_lodCombo, CB_ADDSTRING, 0, (LPARAM)widen(n).c_str());
            SendMessageW(g_lodCombo, CB_SETCURSEL, 0, 0);
            g_comboMode = COMBO_FSB;
            ShowWindow(g_lodCombo, SW_SHOW);
            selectFsbSound(0);
            if (g_playable.empty())
                g_previewText += haveVgmstream(nullptr) ? "\r\n(vgmstream could not decode it)"
                                                        : "\r\n(vgmstream is not beside MonkeyTool.exe)";
        } else if (!g_fsbData.empty()) {
            if (runVgmstream(g_fsbData, g_playable, ".fsb")) showWaveform(g_playable);
        }
        showSoundControls(true);
    } else if (ext == "wav" || ext == "ogg" || ext == "mp3" || ext == "flac" || ext == "m4a" ||
               ext == "aac" || ext == "opus" || ext == "wma" || ext == "fsb" || ext == "xma" ||
               ext == "adx" || ext == "at3" || ext == "mus" || ext == "sng") {
        // Ordinary sound files: a PCM .wav plays as it is, the rest go through
        // the bundled vgmstream, which reads MP3, Ogg, FLAC, AAC and more.
        g_soundActive = true;
        char b2[256];
        size_t off = 0, n = 0;
        WAVEFORMATEX wf{};
        if (ext == "wav" && parseWavPcm(data, wf, off, n) && wf.wFormatTag == WAVE_FORMAT_PCM) {
            g_playable = data;
        } else if (!runVgmstream(data, g_playable, "." + ext)) {
            g_playable.clear();
        }
        if (!g_playable.empty() && parseWavPcm(g_playable, wf, off, n)) {
            double secs = (double)n / std::max<DWORD>(1, wf.nAvgBytesPerSec);
            snprintf(b2, sizeof(b2), "\r\n%s sound, %u ch, %u Hz, %d:%02d - press Play; "
                     "double-click saves it as .wav.", ext.c_str(), wf.nChannels,
                     (unsigned)wf.nSamplesPerSec, (int)secs / 60, (int)secs % 60);
        } else {
            snprintf(b2, sizeof(b2), "\r\nThis .%s did not decode%s.", ext.c_str(),
                     haveVgmstream(nullptr) ? "" : " - vgmstream is not beside MonkeyTool.exe "
                     "(BUILD.bat puts it in dist\\vgmstream)");
            g_playable.clear();
        }
        g_previewText += b2;
        showWaveform(g_playable);
        showSoundControls(true);
    } else {
        // no picture, no model, no sound: show whatever the file does say
        showAssetText(e, data);
        if (g_textActive) g_previewText += "\r\nDouble-click to save this as a .txt.";
    }
    InvalidateRect(g_main, &g_previewRect, TRUE);
}

static void paintPreview(HDC hdc) {
    HBRUSH bg = CreateSolidBrush(pal().previewBg);
    FillRect(hdc, &g_previewRect, bg);
    DeleteObject(bg);

    RECT text = g_previewRect;
    text.top = text.bottom - (kInfoH - 5);
    text.left += 10;
    text.right -= 10;
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT old = (HFONT)SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, pal().text);
    std::wstring wt = widen(g_previewText);
    DrawTextW(hdc, wt.c_str(), -1, &text, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(hdc, old);

    if (g_viewActive) { paintViewer(hdc); return; }
    if (g_textActive) return;   // the text control covers the pane
    if (!g_preview.ok()) return;

    RECT area = g_previewRect;
    area.bottom -= kInfoH;
    if (IsWindowVisible(g_rimCombo)) area.bottom -= 30;     // the CAR COLOR strip
    int aw = area.right - area.left - 20, ah = area.bottom - area.top - 20;
    if (aw <= 0 || ah <= 0) return;
    // Fit the picture to the pane (small textures are enlarged, up to 8x, the
    // way Frosty shows them) on a chequered board that shows through wherever
    // the texture is transparent.
    double scale = std::min((double)aw / g_preview.width, (double)ah / g_preview.height);
    scale = std::min(scale, 8.0);
    int dw = std::max(1, (int)(g_preview.width * scale));
    int dh = std::max(1, (int)(g_preview.height * scale));
    int ox = (aw - dw) / 2, oy = (ah - dh) / 2;
    std::vector<uint8_t> bgra((size_t)aw * ah * 4);
    int ch = g_preview.channels;
    const uint8_t light = g_dark ? 0x4A : 0xFF, darkC = g_dark ? 0x36 : 0xCC;
    std::vector<int> srcX(dw);
    for (int x = 0; x < dw; ++x) srcX[x] = std::min(g_preview.width - 1, (int)(x / scale));
    for (int y = 0; y < ah; ++y) {
        int iy = y - oy;
        int sy = (iy >= 0 && iy < dh) ? std::min(g_preview.height - 1, (int)(iy / scale)) : -1;
        for (int x = 0; x < aw; ++x) {
            uint8_t bg = (((x >> 4) + (y >> 4)) & 1) ? darkC : light;
            uint8_t r = bg, g = bg, b = bg;
            int ix = x - ox;
            if (sy >= 0 && ix >= 0 && ix < dw) {
                const uint8_t* px = g_preview.pixels.data() +
                                    ((size_t)sy * g_preview.width + srcX[ix]) * ch;
                uint8_t pr, pg, pb, pa = 255;
                if (ch == 1) { pr = pg = pb = px[0]; }
                else { pr = px[0]; pg = px[1]; pb = px[2]; if (ch == 4) pa = px[3]; }
                r = (uint8_t)((pr * pa + bg * (255 - pa)) / 255);
                g = (uint8_t)((pg * pa + bg * (255 - pa)) / 255);
                b = (uint8_t)((pb * pa + bg * (255 - pa)) / 255);
            }
            uint8_t* d = &bgra[((size_t)y * aw + x) * 4];
            d[0] = b; d[1] = g; d[2] = r; d[3] = 255;
        }
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = aw;
    bi.bmiHeader.biHeight = -ah;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(hdc, area.left + 10, area.top + 10, aw, ah, 0, 0, 0, ah,
                      bgra.data(), &bi, DIB_RGB_COLORS);
    // size and format, like Frosty's corner label
    wchar_t lbl[96];
    swprintf(lbl, 96, L"%d x %d", g_preview.width, g_preview.height);
    if (g_previewWave) wcscpy(lbl, L"waveform");
    HFONT oldF = (HFONT)SelectObject(hdc, GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, g_dark ? RGB(230, 230, 230) : RGB(20, 20, 20));
    TextOutW(hdc, area.left + 16, area.top + 14, lbl, (int)wcslen(lbl));
    SelectObject(hdc, oldF);
}

// ----------------------------------------------------------------- saving

// Build the Save-As filter string for an asset; filterIndex maps to formats[].
static void buildFilter(const std::string& assetPath,
                        std::wstring& filter, std::vector<std::string>& formats) {
    formats = nfsnl::formatsFor(assetPath);
    std::wstring f;
    auto add = [&](const wchar_t* label, const wchar_t* pattern) {
        f += label; f.push_back(0);
        f += pattern; f.push_back(0);
    };
    for (const std::string& fmt : formats) {
        if (fmt == "fbx")      add(L"Autodesk FBX (*.fbx)", L"*.fbx");
        else if (fmt == "obj") add(L"Wavefront OBJ (*.obj)", L"*.obj");
        else if (fmt == "png") add(L"PNG image (*.png)", L"*.png");
        else if (fmt == "jpg") add(L"JPEG image (*.jpg)", L"*.jpg");
        else if (fmt == "bmp") add(L"Windows bitmap (*.bmp)", L"*.bmp");
        else if (fmt == "tga") add(L"Targa image (*.tga)", L"*.tga");
        else if (fmt == "dds") add(L"DirectDraw surface (*.dds)", L"*.dds");
        else if (fmt == "wav") add(L"WAV sound (*.wav)", L"*.wav");
        else if (fmt == "wem") add(L"Wwise sounds, one file each (*.wem)", L"*.wem");
        else if (fmt == "txt") add(L"Text (*.txt)", L"*.txt");
        else if (fmt == "sbin") add(L"Decrypted SBIN (*.sbin)", L"*.sbin");
        else if (fmt == "xml") add(L"XML (*.xml)", L"*.xml");
        else if (fmt == "bin") add(L"Decoded data (*.bin)", L"*.bin");
        else {
            // the file exactly as the game has it
            std::wstring ext = widen(nfsnl::extensionOf(assetPath));
            std::wstring label = L"Original file (*." + (ext.empty() ? std::wstring(L"*") : ext) + L")";
            std::wstring pat = ext.empty() ? std::wstring(L"*.*") : L"*." + ext;
            f += label; f.push_back(0);
            f += pat; f.push_back(0);
        }
    }
    f.push_back(0);
    filter.swap(f);
}

// The colour the game paints a wheel's rim when nothing is chosen: the
// stock setup of the car the wheel belongs to, else the wheel's DefaultColour
// in the visual parts (data/visualparts/wheels_common.sb ...).
static bool nlDefaultRimColour(const std::string& texturePath, float rgb[3], std::string& name) {
    std::string leaf = nfsnl::stripExtension(nfsnl::baseName(texturePath));
    for (char& c : leaf) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (leaf.compare(0, 14, "texture_wheel_") != 0) return false;
    std::string x = leaf.substr(14);
    if (x.size() > 5 && x.compare(x.size() - 5, 5, "_blur") == 0) x.erase(x.size() - 5);
    static std::map<std::string, std::pair<std::string, std::array<float, 3>>> memo;
    auto hit = memo.find(x);
    if (hit == memo.end()) {
        std::pair<std::string, std::array<float, 3>> found{ std::string(), { { 1, 1, 1 } } };
        nfsnl::NlCarPaint p = nlStockPaint(x);
        if (p.have[nfsnl::NL_RIM]) {
            found.first = p.name[nfsnl::NL_RIM];
            for (int k = 0; k < 3; ++k) found.second[k] = p.rgb[nfsnl::NL_RIM][k];
        } else {
            std::string want = "Mesh = wheel_" + x;
            std::string id;
            for (size_t i = 0; i < g_all.size() && id.empty(); ++i) {
                const std::string& q = g_all[i].path;
                if (q.find("visualparts/") == std::string::npos) continue;
                std::string ql = nfsnl::baseName(q);
                for (char& c : ql) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                if (ql.find("wheel") == std::string::npos && !nfsnl::nlVisualPartsMatches(nfsnl::baseName(q), x)) continue;
                Bytes raw, sb;
                if (!readAsset(g_all[i], raw) || !nfsnl::nlSbDecode(nfsnl::baseName(q), raw.data(), raw.size(), sb)) continue;
                std::string t = nfsnl::sbinObjectsText(sb.data(), sb.size(), 64u << 20);
                size_t at = 0;
                while ((at = t.find(want, at)) != std::string::npos) {
                    size_t endName = at + want.size();
                    at = endName;
                    if (endName < t.size() && t[endName] != '\r' && t[endName] != '\n') continue;   // a longer name
                    size_t dc = t.find("DefaultColour = ", endName);
                    size_t nextMesh = t.find("Mesh = ", endName);
                    if (dc == std::string::npos || (nextMesh != std::string::npos && nextMesh < dc)) continue;
                    size_t e2 = t.find_first_of("\r\n", dc);
                    id = t.substr(dc + 16, e2 == std::string::npos ? std::string::npos : e2 - dc - 16);
                    break;
                }
            }
            for (const nfsnl::NlPaintColour& c : nlColours())
                if (!id.empty() && c.id == id) {
                    found.first = c.name;
                    for (int k = 0; k < 3; ++k) found.second[k] = c.rgb[k];
                    break;
                }
        }
        hit = memo.emplace(x, found).first;
    }
    if (hit->second.first.empty()) return false;
    name = hit->second.first;
    for (int k = 0; k < 3; ++k) rgb[k] = hit->second.second[k];
    return true;
}

static void saveAsset(HWND owner, const Entry& e) {
    std::wstring filter;
    std::vector<std::string> formats;
    buildFilter(e.path, filter, formats);

    std::string stem = nfsnl::stripExtension(nfsnl::baseName(e.path));
    std::wstring name = widen(stem);
    name.resize(MAX_PATH, 0);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter.c_str();
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = &name[0];
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Save asset as";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    std::wstring defExt = widen(formats.empty() ? "bin" : formats[0]);
    ofn.lpstrDefExt = defExt.c_str();

    if (!GetSaveFileNameW(&ofn)) return;

    size_t idx = ofn.nFilterIndex >= 1 ? (size_t)(ofn.nFilterIndex - 1) : 0;
    if (idx >= formats.size()) idx = 0;
    std::string fmt = formats[idx];

    Bytes data;
    std::string readError;
    if (!readAsset(e, data, &readError)) {
        std::wstring msg = L"Could not read this asset from the game files.";
        if (!readError.empty()) msg += L"\n\n" + widen(readError);
        MessageBoxW(owner, msg.c_str(), L"Save failed", MB_ICONERROR);
        return;
    }
    // A bank is not one file: writing it out means writing every sound it
    // holds. Asked for wav, that is what each one becomes - decoded here when
    // the codec allows it, through vgmstream when it does not.
    if (nfsnl::extensionOf(e.path) == "bnk" && (fmt == "wem" || fmt == "wav")) {
        nfsnl::Bank bank;
        if (nfsnl::readBank(data.data(), data.size(), bank) && !bank.entries.empty()) {
            bool wantWav = (fmt == "wav");
            bool haveTool = haveVgmstream(nullptr);
            if (wantWav && !haveTool) {
                // only ask when something in the bank actually needs it
                bool needsTool = false;
                for (const auto& en : bank.entries)
                    if (en.kind == "wem" && en.codec == 0xFFFF) needsTool = true;
                if (needsTool) haveTool = askForVgmstream(owner);
            }

            std::string base = nfsnl::stripExtension(narrow(std::wstring(&name[0])));
            int wav = 0, raw = 0, engines = 0;
            for (const auto& en : bank.entries) {
                if (en.offset + en.size > data.size()) continue;
                const uint8_t* p = data.data() + en.offset;
                char suffix[64];
                snprintf(suffix, sizeof(suffix), "_%u", en.id);
                std::string stem = base + suffix;

                if (wantWav && en.kind == "wem") {
                    Bytes out;
                    if (soundToWav(p, en.size, out)) {
                        if (nfsnl::writeFile(stem + ".wav", out)) wav++;
                        continue;
                    }
                }
                // an engine bank: the original, and a .wav to listen to
                if (en.kind == "gnsu") {
                    Bytes out;
                    if (nfsnl::gnsuToWav(p, en.size, out) && nfsnl::writeFile(stem + ".wav", out))
                        wav++;
                }
                std::string ext = en.kind == "gnsu" ? "gnsu"
                                : (en.kind == "wem" ? "wem" : "bin");
                Bytes piece(p, p + en.size);
                if (nfsnl::writeFile(stem + "." + ext, piece)) {
                    if (ext == "gnsu") engines++; else raw++;
                }
            }

            char msg[320];
            if (wantWav && raw > 0)
                snprintf(msg, sizeof(msg),
                         "Wrote %d .wav, %d .wem (Wwise Vorbis - needs vgmstream), "
                         "%d engine bank(s).", wav, raw, engines);
            else
                snprintf(msg, sizeof(msg), "Wrote %d .wav and %d engine bank(s).",
                         wav ? wav : raw, engines);
            setStatus(msg);
            logLine("bank %s: %d wav, %d raw, %d gnsu", e.path.c_str(), wav, raw, engines);
            MessageBoxW(owner, widen(msg).c_str(), L"Bank written", MB_ICONINFORMATION);
            return;
        }
    }

    // Ordinary sound files save as .wav through vgmstream
    {
        std::string se = nfsnl::extensionOf(e.path);
        if (fmt == "wav" && (se == "ogg" || se == "mp3" || se == "flac" || se == "m4a" ||
                             se == "aac" || se == "opus" || se == "wma" || se == "fsb" ||
                             se == "xma" || se == "adx" || se == "at3")) {
            Bytes wav;
            if (runVgmstream(data, wav, "." + se)) {
                std::string target = nfsnl::stripExtension(narrow(std::wstring(&name[0]))) + ".wav";
                if (nfsnl::writeFile(target, wav)) {
                    setStatus("Saved " + target);
                    return;
                }
            }
            MessageBoxW(owner, L"This sound did not decode - is vgmstream beside MonkeyTool.exe?",
                        L"Save failed", MB_ICONERROR);
            return;
        }
    }
    // A sound this build cannot decode still converts when vgmstream is there
    if (nfsnl::extensionOf(e.path) == "wem" && fmt == "wav") {
        Bytes wav;
        std::string why;
        if (!nfsnl::wemToWav(data.data(), data.size(), wav, &why) &&
            !haveVgmstream(nullptr))
            askForVgmstream(owner);
        if (!nfsnl::wemToWav(data.data(), data.size(), wav, &why) &&
            runVgmstream(data, wav)) {
            std::string target = nfsnl::stripExtension(narrow(std::wstring(&name[0]))) + ".wav";
            if (nfsnl::writeFile(target, wav)) {
                char msg[MAX_PATH + 96];
                snprintf(msg, sizeof(msg), "Saved %u bytes to %s (via vgmstream)",
                         (unsigned)wav.size(), target.c_str());
                setStatus(msg);
                return;
            }
        }
    }

    // "Original file": the bytes exactly as the game has them, under the
    // asset's own extension - never run through a converter
    if (fmt == "raw") {
        std::string target = narrow(std::wstring(&name[0]));
        std::string own = nfsnl::extensionOf(e.path);
        if (!own.empty() && nfsnl::extensionOf(target) != own) {
            std::string ext = nfsnl::extensionOf(target);
            // a name the dialog gave a converter's extension (.fbx, .png) loses it
            if (ext == "fbx" || ext == "obj" || ext == "png" || ext == "jpg" || ext == "txt" || ext == "raw" ||
                ext == "wav" || ext == "bmp" || ext == "tga" || ext == "dds" || ext == "xml" || ext == "bin")
                target = nfsnl::stripExtension(target);
            target += "." + own;
        }
        if (!nfsnl::writeFile(target, data)) {
            MessageBoxW(owner, widen("Could not write " + target).c_str(), L"Save failed", MB_ICONERROR);
            return;
        }
        char msg[MAX_PATH + 96];
        snprintf(msg, sizeof(msg), "Saved %u bytes to %s (the original file)", (unsigned)data.size(), target.c_str());
        setStatus(msg);
        return;
    }

    Bytes out;
    std::string err, usedExt;
    Bytes points;
    readSidecarPoints(e.path, points);

    // A model with hardpoints is assembled here rather than inside the
    // converter, because the wheels come from a second file that only the
    // library can find. What gets written is exactly what the viewer shows.
    bool wroteModel = false;
    std::vector<std::pair<std::string, std::string>> exportTextures;   // ref, png name
    std::vector<nfsnl::Image> embeddedTextures;                         // the model's own (#img:N)
    if (fmt == "fbx" || fmt == "obj") {
        std::vector<std::pair<std::string, Bytes>> banims;
        readSidecarBanims(e.path, banims);
        if (!points.empty() || !banims.empty() || nfsnl::extensionOf(e.path) == "sb3d" ||
            nfsnl::extensionOf(e.path) == "m3g" || nfsnl::isNlScene(data.data(), data.size())) {
            nfsnl::Model m = nfsnl::loadModel(data.data(), data.size());
            if (m.valid) {
                applySidecars(m, e.path, points);
                // A No Limits track: what to take - the road alone or the
                // whole area, and the far backdrop or not - before the
                // textures question below
                if (nfsnl::isNlScene(data.data(), data.size())) {
                    int a = MessageBoxW(owner,
                        L"Export the whole track area?\n\n"
                        L"Yes - the road and everything around it: buildings, trees, terrain, "
                        L"bridges, signs.\n"
                        L"No - the road surface only (asphalt, sidewalks, road lines, barriers).",
                        L"Export track", MB_YESNOCANCEL | MB_ICONQUESTION);
                    if (a == IDCANCEL) return;
                    if (a == IDNO) {
                        auto isRoad = [](const nfsnl::Mesh& me) {
                            std::string t = me.part + " " + me.material;
                            for (char& c : t) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                            for (const char* k : { "road", "asphalt", "ashphalt", "sidewalk", "kerb", "curb",
                                                   "barrier", "lines", "track", "tarmac", "decal" })
                                if (t.find(k) != std::string::npos) return true;
                            return false;
                        };
                        m.meshes.erase(std::remove_if(m.meshes.begin(), m.meshes.end(),
                                                      [&](const nfsnl::Mesh& me) { return !isRoad(me); }),
                                       m.meshes.end());
                    } else {
                        bool hasBackdrop = false;
                        for (const auto& me : m.meshes) if (me.lod == "LOD01") hasBackdrop = true;
                        if (hasBackdrop &&
                            MessageBoxW(owner,
                                L"Also export the distant backdrop?\n\n"
                                L"The game keeps a second, low-detail copy of the hills and "
                                L"buildings for when they are far away. It sits on top of the "
                                L"detailed version, so most people leave it out.\n\n"
                                L"Yes - include it (as its own group)\nNo - detailed version only",
                                L"Export track", MB_YESNO | MB_ICONQUESTION) == IDNO)
                            m.meshes.erase(std::remove_if(m.meshes.begin(), m.meshes.end(),
                                                          [](const nfsnl::Mesh& me) { return me.lod == "LOD01"; }),
                                           m.meshes.end());
                    }
                    if (m.meshes.empty()) {
                        MessageBoxW(owner, L"Nothing is left to export with that choice.", L"Export track",
                                    MB_ICONINFORMATION);
                        return;
                    }
                }
                // A No Limits track with a limited-time layer shown in the
                // viewer: take it along if wanted
                if (nfsnl::isNlScene(data.data(), data.size()) && g_viewPath == e.path &&
                    g_paintKind == PAINT_NL_LTS && g_ltsSel > 0 && (size_t)g_ltsSel <= g_ltsGroups.size()) {
                    std::wstring q = L"Also export the limited-time layer shown in the viewer (" +
                                     widen(nfsnl::nlLtsLabel(g_ltsGroups[g_ltsSel - 1])) + L")?\n\n"
                                     L"Yes - the region with the event's decorations\nNo - the region alone";
                    int a = MessageBoxW(owner, q.c_str(), L"Export track", MB_YESNOCANCEL | MB_ICONQUESTION);
                    if (a == IDCANCEL) return;
                    if (a == IDYES)
                        for (const nfsnl::Mesh& me : g_viewModel.meshes)
                            if (me.part.compare(0, 4, "lts:") == 0) m.meshes.push_back(me);
                }
                // A No Limits car: the rims, brake calipers and body in the
                // colours the game paints them, or as the textures have them
                {
                    std::string car = nlCarIdFor(e.path);
                    if (!car.empty()) {
                        nfsnl::NlCarPaint paint;
                        std::string label;
                        if (g_viewPath == e.path && g_paintKind == PAINT_NL_CAR && g_nlPaint.any()) {
                            paint = g_nlPaint;
                            label = g_nlPaintLabel;
                        } else {
                            paint = nlStockPaint(car, &label);
                        }
                        if (paint.any()) {
                            std::wstring q = L"Export the rims, brake calipers and body in the car's painted colours?\n\n"
                                             L"Yes - painted, as in the game (" + widen(label) + L" setup: " +
                                             widen(nlPaintSummary(paint)) + L")\n"
                                             L"No - the textures' default colours";
                            int a = MessageBoxW(owner, q.c_str(), L"Export colours", MB_YESNOCANCEL | MB_ICONQUESTION);
                            if (a == IDCANCEL) return;
                            if (a == IDYES) nfsnl::nlApplyCarPaint(m, paint);
                        }
                    }
                }
                // The textures the model uses, found in the library: offered
                // with the model, as PNGs beside it, and then the model
                // names them by file name alone so any 3D program finds them.
                std::map<std::string, std::string> leafFor;     // texture ref -> png name
                std::set<std::string> leaves;
                for (const auto& mesh : m.meshes) {
                    const std::string& t = mesh.texture;
                    bool own = t.compare(0, 5, "#img:") == 0;
                    // a painted texture: "#paint:<mode>:<rrggbb>:<texture>"
                    int pMode = 0;
                    float pRgb[3];
                    std::string pBase;
                    bool painted = nfsnl::nlParsePaintRef(t, pMode, pRgb, pBase);
                    if (t.empty() || leafFor.count(t)) continue;
                    if (!own && !painted && findAsset(t) < 0) continue;
                    if (painted && pMode != 2 && findAsset(pBase) < 0) continue;
                    std::string bare = nfsnl::extensionOf(t) == "z" ? nfsnl::stripExtension(t) : t;
                    std::string leaf = own ? nfsnl::stripExtension(nfsnl::baseName(e.path)) + "_tex" + t.substr(5)
                                           : nfsnl::stripExtension(nfsnl::baseName(bare));
                    if (painted) {
                        std::string hex = t.substr(9, 6);
                        leaf = pMode == 2 ? "paint_" + hex
                                          : nfsnl::stripExtension(nfsnl::baseName(pBase)) + "_painted_" + hex;
                    }
                    for (char& c : leaf) if (c == '\\') c = '_';
                    std::string png = leaf + ".png";
                    for (int k = 2; leaves.count(png); ++k) png = leaf + "_" + std::to_string(k) + ".png";
                    leaves.insert(png);
                    leafFor[t] = png;
                }
                if (!leafFor.empty()) {
                    std::wstring q = L"Export the " + std::to_wstring(leafFor.size()) +
                                     L" texture(s) this model uses as well?\n\n"
                                     L"Yes - the model and its textures, as PNG files in the "
                                     L"same folder, already linked to the model.\n"
                                     L"No - the model only.";
                    int a = MessageBoxW(owner, q.c_str(), L"Export textures",
                                        MB_YESNOCANCEL | MB_ICONQUESTION);
                    if (a == IDCANCEL) return;
                    if (a == IDYES) {
                        for (auto& mesh : m.meshes) {
                            auto it = leafFor.find(mesh.texture);
                            if (it != leafFor.end()) mesh.texture = it->second;
                        }
                        for (auto& mt : m.materials) {
                            auto it = leafFor.find(mt.diffuse);
                            if (it != leafFor.end()) mt.diffuse = it->second;
                        }
                        exportTextures.assign(leafFor.begin(), leafFor.end());
                        embeddedTextures = m.images;
                    }
                }
                // paint the textures did not take along: the plain texture
                // name, and the body keeps its colour as the material's
                for (auto& mesh : m.meshes)
                    if (mesh.texture.compare(0, 7, "#paint:") == 0) mesh.texture = nfsnl::nlUnpaintRef(mesh.texture);
                wroteModel = nfsnl::writeModel(m, fmt, out);
                if (wroteModel) usedExt = fmt;
            }
        }
    }
    // A No Limits texture: a UI texture pack's pictures one by one, and a rim
    // or brake caliper painted the colour the game gives it
    if (!wroteModel && nfsnl::extensionOf(e.path) == "sba" &&
        (fmt == "png" || fmt == "jpg" || fmt == "bmp" || fmt == "tga")) {
        auto encode = [&](const nfsnl::Image& img) {
            if (fmt == "jpg") return nfsnl::encodeJpeg(img, 92);
            if (fmt == "bmp") return nfsnl::encodeBmp(img);
            if (fmt == "tga") return nfsnl::encodeTga(img);
            return nfsnl::encodePng(img);
        };
        std::string target = narrow(std::wstring(&name[0]));
        if (nfsnl::extensionOf(target) != fmt) target = nfsnl::stripExtension(target) + "." + fmt;
        std::vector<nfsnl::NamedImage> pics = nfsnl::texturePackPictures(data.data(), data.size());
        if (pics.size() > 1) {
            std::wstring q = L"This is a UI texture pack with " + std::to_wstring(pics.size()) +
                             L" pictures.\n\nYes - save every picture as its own file, under the game's own "
                             L"names, in a folder named after the pack\nNo - the largest page only";
            int a = MessageBoxW(owner, q.c_str(), L"Texture pack", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (a == IDCANCEL) return;
            if (a == IDYES) {
                std::string dir = nfsnl::stripExtension(target);
                int ok = 0;
                for (const nfsnl::NamedImage& pic : pics) {
                    std::string rel = nfsnl::stripExtension(pic.name) + "." + fmt;
                    for (char& c : rel) { if (c == '/') c = '\\'; if (c == ':' || c == '*' || c == '?') c = '_'; }
                    if (nfsnl::writeFile(dir + "\\" + rel, encode(pic.image))) ++ok;
                }
                char msg[MAX_PATH + 96];
                snprintf(msg, sizeof(msg), "Saved %d of %u picture(s) to %s", ok, (unsigned)pics.size(), dir.c_str());
                setStatus(msg);
                return;
            }
        }
        std::string leaf = nfsnl::baseName(e.path);
        for (char& c : leaf) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        bool rim = isNlRimTexture(e.path);
        bool caliper = leaf.compare(0, 21, "texture_brake_caliper") == 0 && leaf.find("alpha") == std::string::npos;
        float rgb[3] = { 1, 1, 1 };
        std::string colourName;
        bool haveColour = false;
        if (rim) haveColour = nlDefaultRimColour(e.path, rgb, colourName);
        if (caliper && g_paintKind == PAINT_NL_CAR && g_nlPaint.have[nfsnl::NL_BRAKE]) {
            memcpy(rgb, g_nlPaint.rgb[nfsnl::NL_BRAKE], sizeof(rgb));
            colourName = g_nlPaint.name[nfsnl::NL_BRAKE] + " (the car in the viewer)";
            haveColour = true;
        }
        if (haveColour) {
            std::wstring q = std::wstring(L"Export the ") + (rim ? L"rim" : L"brake caliper") +
                             L" texture in its default colour or painted?\n\n"
                             L"Yes - painted the colour the game gives it: " + widen(colourName) + L"\n"
                             L"No - the texture's default colour";
            int a = MessageBoxW(owner, q.c_str(), L"Export colours", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (a == IDCANCEL) return;
            nfsnl::Image img;
            if (a == IDYES && nfsnl::decodeTextureFile(data.data(), data.size(), img)) {
                img = nfsnl::toRgba(img);
                std::vector<uint8_t> mask;
                if (rim && g_rimPath == e.path && g_rimMask.size() == (size_t)img.width * img.height) mask = g_rimMask;
                nfsnl::nlTintImage(img, mask, rgb);
                if (!nfsnl::writeFile(target, encode(img))) {
                    MessageBoxW(owner, L"Could not write the file.", L"Save failed", MB_ICONERROR);
                    return;
                }
                setStatus("Saved " + target + " painted " + colourName);
                return;
            }
        }
    }
    if (!wroteModel &&
        (!nfsnl::convertAsset(e.path, data.data(), data.size(), fmt, out, &err, &usedExt,
                              points.empty() ? nullptr : points.data(), points.size()) ||
         out.empty())) {
        std::wstring msg = L"Could not convert this asset.";
        if (!err.empty()) msg += L"\n\n" + widen(err);
        MessageBoxW(owner, msg.c_str(), L"Save failed", MB_ICONERROR);
        return;
    }

    std::string target = narrow(std::wstring(&name[0]));
    // name the file after what the data actually is; when a payload could not
    // be decoded this differs from the requested format
    std::string want = usedExt.empty() ? fmt : usedExt;
    if (!want.empty() && nfsnl::extensionOf(target) != want)
        target = nfsnl::stripExtension(target) + "." + want;

    if (!nfsnl::writeFile(target, out)) {
        MessageBoxW(owner, L"Could not write the file.", L"Save failed", MB_ICONERROR);
        return;
    }
    char msg[MAX_PATH + 128];
    snprintf(msg, sizeof(msg), "Saved %u bytes to %s", (unsigned)out.size(), target.c_str());
    // the textures, beside the model
    if (!exportTextures.empty()) {
        std::string dir = target.substr(0, target.find_last_of("\\/") + 1);
        int ok = 0, bad = 0;
        HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
        for (const auto& t : exportTextures) {
            int pMode = 0;
            float pRgb[3];
            std::string pBase;
            bool painted = nfsnl::nlParsePaintRef(t.first, pMode, pRgb, pBase);
            const std::string& ref = painted ? pBase : t.first;
            int idx = ref.compare(0, 5, "#img:") == 0 ? -1 : findAsset(ref);
            nfsnl::Image img;
            bool have = false;
            if (painted && pMode == 2) {
                nfsnl::nlPaintImage(img, 2, pRgb);
                have = true;
            } else if (ref.compare(0, 5, "#img:") == 0) {
                size_t k = (size_t)atoi(t.first.c_str() + 5);
                if (k < embeddedTextures.size()) { img = embeddedTextures[k]; have = img.ok(); }
            } else if (idx >= 0) {
                auto ov = g_texOverride.find(g_all[idx].path);
                if (ov != g_texOverride.end()) { img = ov->second; have = true; }
                else {
                    Bytes raw;
                    have = readAsset(g_all[idx], raw) &&
                           nfsnl::decodeTextureFile(raw.data(), raw.size(), img);
                }
            }
            if (have && painted && pMode == 1) nfsnl::nlPaintImage(img, 1, pRgb);
            Bytes png = have ? nfsnl::encodePng(img) : Bytes();
            if (!png.empty() && nfsnl::writeFile(dir + t.second, png)) ++ok; else ++bad;
        }
        SetCursor(old);
        snprintf(msg, sizeof(msg), "Saved %s and %d texture(s)%s", target.c_str(), ok,
                 bad ? " (some did not decode)" : "");
        logLine("export: %s with %d texture(s), %d failed", target.c_str(), ok, bad);
    }
    setStatus(msg);
    if (!err.empty()) MessageBoxW(owner, widen(err).c_str(), L"Note", MB_ICONINFORMATION);
}

// Save one sound out of a bank: a .wav when it can be decoded, the original
// .wem or .gnsu when it cannot.
static void saveBankSound(HWND owner, const SoundRef& ref) {
    if (ref.entry < 0 || (size_t)ref.entry >= g_all.size()) return;
    const Entry& bankEntry = g_all[ref.entry];
    Bytes data;
    if (!readAsset(bankEntry, data)) return;
    nfsnl::Bank bank;
    if (!nfsnl::readBank(data.data(), data.size(), bank)) return;
    if (ref.index < 0 || (size_t)ref.index >= bank.entries.size()) return;
    const nfsnl::BankEntry& be = bank.entries[ref.index];
    if (be.offset + be.size > data.size()) return;
    const uint8_t* p = data.data() + be.offset;

    bool wantWav = be.kind == "wem" || be.kind == "gnsu";
    if (be.kind == "wem" && !haveVgmstream(nullptr) && be.codec == 0xFFFF) {
        Bytes probe;
        std::string why;
        if (!nfsnl::wemToWav(p, be.size, probe, &why)) askForVgmstream(owner);
    }

    Bytes wav;
    bool haveWav = wantWav && soundToWav(p, be.size, wav);
    // an engine bank offers the original too: the .wav is for listening,
    // the .gnsu is what the game reads
    const wchar_t* filter = haveWav
        ? (be.kind == "gnsu" ? L"WAV sound (*.wav)\0*.wav\0Engine bank (*.gnsu)\0*.gnsu\0"
                             : L"WAV sound (*.wav)\0*.wav\0")
        : (be.kind == "gnsu" ? L"Engine bank (*.gnsu)\0*.gnsu\0"
                             : L"Wwise sound (*.wem)\0*.wem\0");
    std::string ext = haveWav ? "wav" : (be.kind == "gnsu" ? "gnsu" : "wem");

    char stem[128];
    snprintf(stem, sizeof(stem), "%u", be.id);
    std::wstring name = widen(stem);
    name.resize(MAX_PATH, 0);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = &name[0];
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Save sound as";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    std::wstring defExt = widen(ext);
    ofn.lpstrDefExt = defExt.c_str();
    if (!GetSaveFileNameW(&ofn)) return;
    if (haveWav && be.kind == "gnsu" && ofn.nFilterIndex == 2) {
        haveWav = false;
        ext = "gnsu";
    }

    std::string target = narrow(std::wstring(&name[0]));
    if (nfsnl::extensionOf(target) != ext)
        target = nfsnl::stripExtension(target) + "." + ext;

    bool ok = haveWav ? nfsnl::writeFile(target, wav)
                      : nfsnl::writeFile(target, p, be.size);
    char msg[MAX_PATH + 96];
    if (ok) snprintf(msg, sizeof(msg), "Saved sound %u to %s", be.id, target.c_str());
    else    snprintf(msg, sizeof(msg), "Could not write %s", target.c_str());
    setStatus(msg);
}

// batch convert everything currently shown
// Writing many assets out runs on a worker thread, with the same progress
// bar as reading the game, so the window stays alive. "Extract" writes the
// files as the game has them; "convert" writes what they are for everyone
// else: PNG textures, FBX models, WAV sounds.
struct ExportJob {
    std::vector<int> entries;
    std::string outDir;
    std::string strip;       // folder prefix left out of the written paths
    bool convert = false;
};

static std::string exportFormatFor(const std::string& path) {
    std::string ext = nfsnl::extensionOf(path);
    if (ext == "sba" && path.find(".scene_static.sba") != std::string::npos &&
        path.find("lightprobes") == std::string::npos && path.find("exclusions") == std::string::npos &&
        path.find("lightmaps") == std::string::npos)
        return "fbx";
    if (ext == "sb3d" || ext == "m3g") return "fbx";
    if (ext == "sba" || ext == "pvr" || ext == "dds" || ext == "ktx") return "png";
    if (ext == "wem" || ext == "gnsu" || ext == "sps") return "wav";
    if (ext == "nct") return "txt";
    return "raw";
}

static DWORD WINAPI exportThreadProc(LPVOID param) {
    ExportJob* job = static_cast<ExportJob*>(param);
    int written = 0, failed = 0;
    size_t total = job->entries.size();
    DWORD lastTick = 0;
    for (size_t k = 0; k < total; ++k) {
        const Entry& e = g_all[job->entries[k]];
        DWORD now = GetTickCount();
        if (now - lastTick > 120 || k + 1 == total) {
            lastTick = now;
            char buf[512];
            snprintf(buf, sizeof(buf), "%s %u / %u: %s", job->convert ? "Exporting" : "Extracting",
                     (unsigned)(k + 1), (unsigned)total, nfsnl::baseName(e.path).c_str());
            postProgress(buf, k + 1, total);
        }
        Bytes data;
        // g_busy is set by this export itself, which readAsset() would take
        // for a library rebuild; the library is not being rebuilt, so read it
        // directly
        std::string readErr;
        if (!g_lib.read(e.lib, data, &readErr)) {
            ++failed;
            if (failed <= 20) logLine("export: could not read %s: %s", e.path.c_str(), readErr.c_str());
            continue;
        }
        Bytes out;
        std::string usedExt, err;
        std::string ext = nfsnl::extensionOf(e.path);
        if (job->convert) {
            std::string fmt = exportFormatFor(e.path);
            try {
                if (!nfsnl::convertAsset(e.path, data.data(), data.size(), fmt, out, &err, &usedExt) ||
                    out.empty()) {
                    out = data;          // keep the file rather than lose it
                    usedExt = ext;
                }
            } catch (...) {
                out = data;
                usedExt = ext;
            }
        } else {
            out.swap(data);
        }
        std::string rel = e.path;
        if (!job->strip.empty() && rel.compare(0, job->strip.size(), job->strip) == 0)
            rel.erase(0, job->strip.size());
        while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
        if (!usedExt.empty() && usedExt != ext) rel = nfsnl::stripExtension(rel) + "." + usedExt;
        for (auto& c : rel) if (c == '/') c = '\\';
        if (nfsnl::writeFile(job->outDir + "\\" + rel, out)) ++written;
        else {
            ++failed;
            if (failed <= 20) logLine("export: could not write %s\\%s", job->outDir.c_str(), rel.c_str());
        }
    }
    delete job;
    g_busy = false;
    PostMessageW(g_main, WM_APP_EXPORTED, (WPARAM)written, (LPARAM)failed);
    return 0;
}

static void startExport(HWND owner, std::vector<int> entries, const std::string& strip, bool convert) {
    if (g_busy) {
        MessageBoxW(owner, L"Still busy - one moment.", L"Monkey Tool", MB_ICONINFORMATION);
        return;
    }
    if (entries.empty()) {
        MessageBoxW(owner, L"There is nothing to write here.", L"Monkey Tool", MB_ICONINFORMATION);
        return;
    }
    std::string outDir;
    if (!pickFolder(owner, convert ? L"Choose a folder for the exported files"
                                   : L"Choose a folder for the extracted files", outDir))
        return;
    stopSound();
    closeViewer();
    ExportJob* job = new ExportJob;
    job->entries = std::move(entries);
    job->outDir = outDir;
    job->strip = strip;
    job->convert = convert;
    g_busy = true;
    logLine("export: %u asset(s) to %s (%s)", (unsigned)job->entries.size(), outDir.c_str(),
            convert ? "converted" : "as they are");
    HANDLE th = CreateThread(nullptr, 0, exportThreadProc, job, 0, nullptr);
    if (!th) { g_busy = false; delete job; return; }
    CloseHandle(th);
}

// every asset under a folder of the tree (search and filter applied)
static std::vector<int> entriesUnder(const std::string& dir) {
    std::vector<int> out;
    std::string prefix = dir.empty() ? std::string() : dir + "/";
    for (int i = 0; i < (int)g_all.size(); ++i)
        if (passesFilter(g_all[i]) && g_all[i].path.compare(0, prefix.size(), prefix) == 0)
            out.push_back(i);
    return out;
}

static void convertAllShown(HWND owner) {
    if (g_all.empty()) {
        MessageBoxW(owner, L"Open a game folder first.", L"Nothing to convert", MB_ICONINFORMATION);
        return;
    }
    startExport(owner, entriesUnder(""), "", true);
}

// ---- several rows at once: Ctrl+click adds or removes a row, Shift+click
// takes every row from the last one clicked, as in Windows Explorer. The
// tree control itself selects one row only; the others are drawn
// highlighted (NM_CUSTOMDRAW) and kept here.
static std::vector<int> selectedEntries(std::string* strip) {
    std::set<int> pick;
    for (HTREEITEM it : g_multi) {
        auto d = g_itemToDir.find(it);
        if (d != g_itemToDir.end()) { for (int i : entriesUnder(d->second)) pick.insert(i); continue; }
        auto e = g_itemToEntry.find(it);
        if (e != g_itemToEntry.end() && e->second >= 0 && (size_t)e->second < g_all.size()) pick.insert(e->second);
    }
    std::vector<int> out(pick.begin(), pick.end());
    // the folders the files share stay out of the copies' paths
    if (strip) {
        strip->clear();
        bool first = true;
        for (int i : out) {
            std::string dir = g_all[i].path.substr(0, g_all[i].path.find_last_of('/') == std::string::npos
                                                         ? 0 : g_all[i].path.find_last_of('/'));
            if (first) { *strip = dir; first = false; continue; }
            // the longest run of whole folders both paths start with
            std::string a = *strip + "/", b = dir + "/";
            size_t k = 0, keep = 0;
            while (k < a.size() && k < b.size() && a[k] == b[k]) { if (a[k] == '/') keep = k; ++k; }
            k = keep;
            strip->resize(k);
        }
    }
    return out;
}

static void showMultiStatus() {
    if (g_multi.size() < 2) return;
    std::string strip;
    size_t n = selectedEntries(&strip).size();
    char b[200];
    snprintf(b, sizeof(b), "%u rows selected (%u asset(s)) - right-click or File > Export selected to save them all.",
             (unsigned)g_multi.size(), (unsigned)n);
    setStatus(b);
}

static void exportSelected(HWND owner, bool convert) {
    std::string strip;
    std::vector<int> e = selectedEntries(&strip);
    if (e.empty()) {
        HTREEITEM sel = TreeView_GetSelection(g_tree);
        auto d = g_itemToDir.find(sel);
        auto it = g_itemToEntry.find(sel);
        if (d != g_itemToDir.end()) e = entriesUnder(d->second);
        else if (it != g_itemToEntry.end() && it->second >= 0) e.push_back(it->second);
        if (!e.empty()) {
            std::string p = g_all[e[0]].path;
            strip = p.find('/') == std::string::npos ? std::string() : p.substr(0, p.find_last_of('/'));
            if (d != g_itemToDir.end()) {
                size_t slash = d->second.find_last_of('/');
                strip = slash == std::string::npos ? std::string() : d->second.substr(0, slash);
            }
        }
    }
    startExport(owner, e, strip, convert);
}

// right-click on the tree: extract or export a whole folder
static void treeContextMenu(HWND hwnd) {
    POINT screen;
    GetCursorPos(&screen);
    POINT pt = screen;
    ScreenToClient(g_tree, &pt);
    TVHITTESTINFO hit{};
    hit.pt = pt;
    HTREEITEM item = (HTREEITEM)SendMessageW(g_tree, TVM_HITTEST, 0, (LPARAM)&hit);
    if (!item) return;
    if (g_multi.size() >= 2 && g_multi.count(item)) {
        std::string strip;
        size_t n = selectedEntries(&strip).size();
        HMENU menu = CreatePopupMenu();
        std::wstring a = L"Extract the selected assets (" + std::to_wstring(n) + L")...";
        std::wstring b = L"Export the selected assets - PNG, FBX, WAV (" + std::to_wstring(n) + L")...";
        AppendMenuW(menu, MF_STRING, ID_CTX_SEL_EXTRACT, a.c_str());
        AppendMenuW(menu, MF_STRING, ID_CTX_SEL_CONVERT, b.c_str());
        int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
        if (cmd == ID_CTX_SEL_EXTRACT || cmd == ID_CTX_SEL_CONVERT) exportSelected(hwnd, cmd == ID_CTX_SEL_CONVERT);
        return;
    }
    TreeView_SelectItem(g_tree, item);
    auto dir = g_itemToDir.find(item);
    auto ent = g_itemToEntry.find(item);
    HMENU menu = CreatePopupMenu();
    if (dir != g_itemToDir.end()) {
        size_t n = entriesUnder(dir->second).size();
        std::wstring a = L"Extract all assets from this folder (" + std::to_wstring(n) + L")...";
        std::wstring b = L"Export all assets from this folder - PNG, FBX, WAV (" +
                         std::to_wstring(n) + L")...";
        AppendMenuW(menu, MF_STRING, ID_CTX_EXTRACT, a.c_str());
        AppendMenuW(menu, MF_STRING, ID_CTX_CONVERT, b.c_str());
    } else if (ent != g_itemToEntry.end()) {
        AppendMenuW(menu, MF_STRING, ID_CTX_SAVE, L"Save as...");
    } else {
        DestroyMenu(menu);
        return;
    }
    int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0,
                                  hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd == ID_CTX_EXTRACT || cmd == ID_CTX_CONVERT) {
        // the folder's own name is kept: extracting "textures/cars" makes
        // a "cars" folder in the chosen place
        std::string d = dir->second;
        size_t slash = d.find_last_of('/');
        std::string strip = slash == std::string::npos ? std::string() : d.substr(0, slash);
        startExport(hwnd, entriesUnder(d), strip, cmd == ID_CTX_CONVERT);
    } else if (cmd == ID_CTX_SAVE && ent->second >= 0 && (size_t)ent->second < g_all.size()) {
        saveAsset(hwnd, g_all[ent->second]);
    }
}

// ----------------------------------------------------------------- extract

static void indexWorkerBody(std::string folder);

static void indexWorker(std::string folder) {
    g_busy = true;
    try {
        indexWorkerBody(folder);
    } catch (const std::exception& e) {
        logLine("indexing failed: %s", e.what());
        postProgress(std::string("Could not read the game files: ") + e.what());
    } catch (...) {
        logLine("indexing failed: unknown exception");
        postProgress("Could not read the game files.");
    }
    g_busy = false;
    PostMessageW(g_main, WM_APP_DONE, (WPARAM)(int)g_lib.warnings.size(),
                 (LPARAM)g_lib.archives.size());
}

static void nameLooseSounds();

// Each asset's date for sorting: the file's own for a loose file, its
// archive's for one inside a .pack or .obb.
static void fillEntryTimes() {
    std::map<int, uint64_t> byArchive;
    for (Entry& e : g_all) {
        if (e.lib >= g_lib.entries.size()) continue;
        int a = g_lib.entries[e.lib].archive;
        auto it = byArchive.find(a);
        if (it == byArchive.end()) {
            uint64_t t = 0;
            if (a >= 0 && (size_t)a < g_lib.archives.size()) {
                WIN32_FIND_DATAW fd;
                HANDLE h = FindFirstFileW(widen(g_lib.archives[a].path).c_str(), &fd);
                if (h != INVALID_HANDLE_VALUE) {
                    t = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) |
                        fd.ftLastWriteTime.dwLowDateTime;
                    FindClose(h);
                }
            }
            it = byArchive.emplace(a, t).first;
        }
        e.time = it->second;
    }
}

// Every asset of a loose-file game, indexed straight off the folder.
static void indexLooseGame(std::string folder) {
    std::vector<std::pair<std::string, std::string>> files;
    scanLooseFiles(folder, "", files);
    logLine("indexing: %u loose file(s) under %s",
            (unsigned)files.size(), folder.c_str());

    g_all.clear();
    g_lib.clear();
    DWORD lastTick = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        nfsnl::indexLooseFile(files[i].first, files[i].second, g_lib);
        DWORD now = GetTickCount();
        if (now - lastTick > 120 || i + 1 == files.size()) {
            lastTick = now;
            char buf[256];
            snprintf(buf, sizeof(buf), "Reading %u / %u file(s) ...",
                     (unsigned)(i + 1), (unsigned)files.size());
            postProgress(buf, i + 1, files.size());
        }
    }
    logLine("indexing: done, %u asset(s)", (unsigned)g_lib.entries.size());
    g_all.reserve(g_lib.entries.size());
    for (size_t i = 0; i < g_lib.entries.size(); ++i) {
        const nfsnl::LibraryEntry& le = g_lib.entries[i];
        Entry e;
        e.path = le.path;
        e.lib = i;
        e.size = le.length;
        e.kind = le.kind;
        g_all.push_back(std::move(e));
    }
    nameLooseSounds();
    fillEntryTimes();
}

// A loose <id>.wem is named after nothing but its Wwise media id. The banks
// say which of them they play (a Sound or Music Track object naming that
// id), so each such file is filed under its bank: android/audio/Music/...,
// android/audio/Ambience/... The ids themselves are the only names the game
// ships - Wwise keeps the text names in the authoring project, and without
// a SoundbanksInfo.xml they are not in the game's files at all.
static void nameLooseSounds() {
    std::vector<size_t> banks, wems;
    for (size_t i = 0; i < g_all.size(); ++i) {
        std::string e = nfsnl::extensionOf(g_all[i].path);
        if (e == "bnk") banks.push_back(i);
        else if (e == "wem") wems.push_back(i);
    }
    if (banks.empty() || wems.empty()) return;
    postProgress("Matching sounds to their banks ...", 0, banks.size());
    std::map<uint32_t, std::string> owner;
    for (size_t k = 0; k < banks.size(); ++k) {
        Bytes data;
        if (!readAsset(g_all[banks[k]], data)) continue;
        nfsnl::Bank bank;
        if (!nfsnl::readBank(data.data(), data.size(), bank)) continue;
        std::string name = nfsnl::stripExtension(nfsnl::baseName(g_all[banks[k]].path));
        for (uint32_t id : bank.sources)
            if (!owner.count(id)) owner[id] = name;
        postProgress("Matching sounds to their banks ...", k + 1, banks.size());
    }
    int named = 0;
    for (size_t i : wems) {
        std::string& p = g_all[i].path;
        std::string leaf = nfsnl::baseName(p);
        uint32_t id = (uint32_t)strtoul(nfsnl::stripExtension(leaf).c_str(), nullptr, 10);
        auto it = owner.find(id);
        if (!id || it == owner.end()) continue;
        p = p.substr(0, p.size() - leaf.size()) + it->second + "/" + leaf;
        ++named;
    }
    logLine("sounds: %d of %u loose .wem filed under their bank", named, (unsigned)wems.size());
}

static void indexWorkerBody(std::string folder) {

    // descend into the usual Android layout if the app root was chosen
    std::vector<std::string> subs = { "\\com.ea.game.nfs14_row\\files\\packs",
                                      "\\files\\packs", "\\packs" };
    if (g_profile && !g_profile->folderSteps.empty()) subs = g_profile->folderSteps;
    for (const std::string& s : subs) {
        std::string cand = folder + s;
        DWORD attr = GetFileAttributesW(widen(cand).c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            folder = cand;
            break;
        }
    }

    if (g_profile && g_profile->looseFiles) { indexLooseGame(folder); return; }

    std::vector<std::string> archives;
    scanArchives(folder, archives);
    logLine("indexing: %u archive(s) under %s",
            (unsigned)archives.size(), folder.c_str());
    // No archives at all: the folder holds loose files (another game, or an
    // unpacked copy). Read them as they are rather than show nothing.
    if (archives.empty()) {
        logLine("indexing: no archives - reading the folder's files as they are");
        indexLooseGame(folder);
        return;
    }
    // .pack files carry the names, so do them first
    std::stable_sort(archives.begin(), archives.end(),
                     [](const std::string& a, const std::string& b) {
                         return nfsnl::extensionOf(a) == "pack" &&
                                nfsnl::extensionOf(b) != "pack";
                     });

    g_all.clear();
    g_lib.clear();

    // Only the manifests are read here - a few kilobytes per archive. The
    // assets themselves stay in the game's files until something asks for
    // one, so opening a game costs no disk space at all.
    DWORD lastTick = 0;
    for (size_t i = 0; i < archives.size(); ++i) {
        nfsnl::indexArchive(archives[i], g_lib);
        // at most a few updates a second, whatever the machine manages
        DWORD now = GetTickCount();
        if (now - lastTick > 120 || i + 1 == archives.size()) {
            lastTick = now;
            char buf[256];
            snprintf(buf, sizeof(buf), "Reading %u / %u archive(s) ... %u assets so far",
                      (unsigned)(i + 1), (unsigned)archives.size(),
                      (unsigned)g_lib.entries.size());
            postProgress(buf, i + 1, archives.size());
        }
    }

    logLine("indexing: done, %u asset(s), %u warning(s)",
            (unsigned)g_lib.entries.size(), (unsigned)g_lib.warnings.size());
    g_all.reserve(g_lib.entries.size());
    for (size_t i = 0; i < g_lib.entries.size(); ++i) {
        const nfsnl::LibraryEntry& le = g_lib.entries[i];
        Entry e;
        e.path = le.path;
        e.lib = i;
        e.size = le.length;
        e.kind = le.kind;
        g_all.push_back(std::move(e));
    }
    nameLooseSounds();
    fillEntryTimes();
    // the caller clears g_busy and posts WM_APP_DONE, once, whatever happens
}

// Plain Win32 thread: std::thread is not available on every MinGW build.
struct IndexJob { std::string folder; };

static DWORD WINAPI indexThreadProc(LPVOID param) {
    IndexJob* job = static_cast<IndexJob*>(param);
    indexWorker(job->folder);
    delete job;
    return 0;
}

// ----------------------------------------------------------------- loading screen
//
// While a game is read, a Frosty-style card: the tool's icon and name on a
// dark gradient, the game's own art fading in on the right, and a strip
// along the bottom with what is being read and how far along it is.
static HWND g_splash = nullptr;
static std::string g_splashText = "Loading data";
static int g_splashPermille = -1, g_splashSpin = 0;
static const int kSplashW = 760, kSplashH = 300, kSplashStrip = 44;
static const UINT_PTR IDT_SPLASH = 46;

static void paintSplash(HWND hwnd, HDC target) {
    const int W = kSplashW, H = kSplashH;
    std::vector<uint8_t> px((size_t)W * H * 4);
    auto put = [&](int x, int y, int r, int g, int b) {
        uint8_t* d = &px[((size_t)y * W + x) * 4];
        d[0] = (uint8_t)b; d[1] = (uint8_t)g; d[2] = (uint8_t)r; d[3] = 255;
    };
    // the gradient
    for (int y = 0; y < H; ++y) {
        float t = (float)y / (H - kSplashStrip);
        int r = y < H - kSplashStrip ? (int)(0x46 + (0x2A - 0x46) * t) : 0x1C;
        int g = y < H - kSplashStrip ? (int)(0x46 + (0x2A - 0x46) * t) : 0x1C;
        int b = y < H - kSplashStrip ? (int)(0x4E + (0x31 - 0x4E) * t) : 0x21;
        for (int x = 0; x < W; ++x) put(x, y, r, g, b);
    }
    // the game's art on the right, fading in from the left over 160 px
    const nfsnl::Image* art = g_profile ? resourceImage(g_profile->splashResource) : nullptr;
    if (art) {
        nfsnl::Image a = nfsnl::toRgba(*art);
        int artH = H - kSplashStrip, artW = std::min(W - 280, artH * a.width / std::max(1, a.height));
        int x0 = W - artW;
        for (int y = 0; y < artH; ++y)
            for (int x = 0; x < artW; ++x) {
                int sx = x * a.width / artW, sy = y * a.height / artH;
                const uint8_t* s = &a.pixels[((size_t)sy * a.width + sx) * 4];
                float k = std::min(1.0f, x / 160.0f);
                k = k * k * (3 - 2 * k);
                uint8_t* d = &px[((size_t)y * W + x0 + x) * 4];
                d[0] = (uint8_t)(d[0] * (1 - k) + s[2] * k);
                d[1] = (uint8_t)(d[1] * (1 - k) + s[1] * k);
                d[2] = (uint8_t)(d[2] * (1 - k) + s[0] * k);
            }
    }
    // the progress line along the top of the strip: blue into cyan
    if (g_splashPermille >= 0) {
        int len = W * std::min(1000, g_splashPermille) / 1000;
        for (int y = H - kSplashStrip; y < H - kSplashStrip + 3; ++y)
            for (int x = 0; x < len; ++x) {
                float t = (float)x / W;
                put(x, y, (int)(0x2F + 0x10 * t), (int)(0x6F + 0x80 * t), (int)(0xD0 + 0x20 * t));
            }
    }
    HDC dc = CreateCompatibleDC(target);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(target, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) { DeleteDC(dc); return; }
    memcpy(bits, px.data(), px.size());
    HGDIOBJ old = SelectObject(dc, bmp);
    // the tool's own icon, big
    const nfsnl::Image* icon = resourceImage(IDR_APPICON_BIG);
    if (icon) drawImageAlpha(dc, *icon, 34, 46, 112, 112, RGB(0x40, 0x40, 0x48));
    SetBkMode(dc, TRANSPARENT);
    HFONT big = CreateFontW(-38, 0, 0, 0, FW_LIGHT, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI Light");
    HFONT mid = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HGDIOBJ of = SelectObject(dc, big);
    SetTextColor(dc, RGB(0xF2, 0xF2, 0xF2));
    TextOutW(dc, 166, 70, L"Monkey Tool", 11);
    SelectObject(dc, mid);
    SetTextColor(dc, RGB(0xB8, 0xB8, 0xC0));
    TextOutW(dc, 168, 120, L"by GM25", 7);
    TextOutW(dc, 36, 196, L"1.2.2", 5);
    if (g_profile) {
        std::wstring gn = widen(g_profile->name);
        TextOutW(dc, 36, 218, gn.c_str(), (int)gn.size());
    }
    // the strip: a turning arc and the text
    int cx = 24, cy = H - kSplashStrip / 2 + 1;
    HPEN arc = CreatePen(PS_SOLID, 2, RGB(0xE8, 0xE8, 0xE8));
    HGDIOBJ op = SelectObject(dc, arc);
    for (int k = 0; k < 8; ++k) {
        double a0 = (g_splashSpin * 30 + k * 30) * 3.14159265 / 180.0;
        if (k > 5) break;
        MoveToEx(dc, cx + (int)(7 * cos(a0)), cy + (int)(7 * sin(a0)), nullptr);
        double a1 = a0 + 30 * 3.14159265 / 180.0;
        LineTo(dc, cx + (int)(7 * cos(a1)), cy + (int)(7 * sin(a1)));
    }
    SelectObject(dc, op);
    DeleteObject(arc);
    SetTextColor(dc, RGB(0xE8, 0xE8, 0xE8));
    RECT tr{ 44, H - kSplashStrip + 3, W - 16, H };
    std::wstring wt = widen(g_splashText);
    DrawTextW(dc, wt.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, of);
    DeleteObject(big);
    DeleteObject(mid);
    BitBlt(target, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
}

static LRESULT CALLBACK SplashProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        paintSplash(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (wp == IDT_SPLASH) {
            g_splashSpin = (g_splashSpin + 1) % 12;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void showSplash() {
    if (g_splash) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SplashProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursor(nullptr, IDC_WAIT);
        wc.lpszClassName = L"MonkeyToolSplash";
        RegisterClassExW(&wc);
        registered = true;
    }
    RECT mr;
    GetWindowRect(g_main, &mr);
    int x = (mr.left + mr.right - kSplashW) / 2, y = (mr.top + mr.bottom - kSplashH) / 2;
    g_splashText = "Loading data";
    g_splashPermille = -1;
    g_splash = CreateWindowExW(WS_EX_TOOLWINDOW, L"MonkeyToolSplash", L"Monkey Tool", WS_POPUP | WS_BORDER,
                               x, y, kSplashW, kSplashH, g_main, nullptr, g_inst, nullptr);
    if (!g_splash) return;
    ShowWindow(g_splash, SW_SHOWNOACTIVATE);
    UpdateWindow(g_splash);
    SetTimer(g_splash, IDT_SPLASH, 80, nullptr);
}

static void updateSplash(const std::string& text, int permille) {
    if (!g_splash) return;
    if (!text.empty()) g_splashText = text;
    g_splashPermille = permille;
    InvalidateRect(g_splash, nullptr, FALSE);
}

static void closeSplash() {
    if (!g_splash) return;
    KillTimer(g_splash, IDT_SPLASH);
    DestroyWindow(g_splash);
    g_splash = nullptr;
}

static void startIndexThread(const std::string& folder) {
    showSplash();
    // Empty the tree here, on the window's own thread, before the worker
    // clears the list it points into. A click on a stale row would otherwise
    // look up an index that no longer exists.
    g_itemToEntry.clear();
    g_itemToSound.clear();
    g_bankExpanded.clear();
    // a search left over from the last game would hide the new one
    g_search.clear();
    SetWindowTextW(g_searchBox, L"");
    SendMessageW(g_tree, TVM_DELETEITEM, 0, (LPARAM)TVI_ROOT);
    closeViewer();
    showTextView(false);
    stopSound();
    g_preview = nfsnl::Image();
    g_previewText.clear();
    InvalidateRect(g_main, nullptr, TRUE);

    IndexJob* job = new IndexJob{folder};
    HANDLE h = CreateThread(nullptr, 0, indexThreadProc, job, 0, nullptr);
    if (h) CloseHandle(h);
    else { delete job; setStatus("Could not start the indexing thread."); }
}

// ----------------------------------------------------------------- window

static void layout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int statusH = 22;
    int treeW = (rc.right - rc.left) * 45 / 100;
    // A strip across the top: the search box exactly over the tree - same
    // left edge, same width - and the tool buttons over the preview.
    int searchH = 38;
    MoveWindow(g_searchBox, 0, 7, treeW, 24, TRUE);
    // Name | Ext | Size | Date, over the tree
    int colH = 20;
    int wName = treeW - 3 * 80;
    int cx = 0;
    const int widths[4] = { std::max(80, wName), 80, 80, 80 };
    for (int k = 0; k < 4; ++k) {
        if (g_sortBtn[k]) MoveWindow(g_sortBtn[k], cx, searchH, widths[k], colH, TRUE);
        cx += widths[k];
    }
    MoveWindow(g_tree, 0, searchH + colH, treeW, rc.bottom - statusH - searchH - colH, TRUE);
    // the Save Editor's button: a real toolbar-sized icon, not a 16 px one
    MoveWindow(g_tbOpen, treeW + 4, 2, 36, 34, TRUE);
    MoveWindow(g_tbExport, treeW + 44, 2, 36, 34, TRUE);
    MoveWindow(g_tbImport, treeW + 84, 2, 36, 34, TRUE);
    MoveWindow(g_tbSave, treeW + 124, 2, 36, 34, TRUE);
    MoveWindow(g_tbNewCar, treeW + 168, 2, 36, 34, TRUE);
    MoveWindow(g_saveEdBtn, treeW + 216, 2, 36, 34, TRUE);
    int barW = 190;
    bool barUp = IsWindowVisible(g_progress) != 0;
    MoveWindow(g_status, 0, rc.bottom - statusH,
               barUp ? std::max(40L, rc.right - barW - 8) : rc.right, statusH, TRUE);
    MoveWindow(g_progress, rc.right - barW - 4, rc.bottom - statusH + 3, barW, statusH - 6, TRUE);
    g_previewRect.left = treeW + 1;
    g_previewRect.top = searchH;
    g_previewRect.right = rc.right;
    g_previewRect.bottom = rc.bottom - statusH;

    // the viewer fills the pane above the text block, with its own strip of
    // controls between the two
    int barH = 28;
    g_viewBarRect = g_previewRect;
    g_viewBarRect.bottom -= kInfoH;
    g_viewBarRect.top = g_viewBarRect.bottom - barH;
    g_viewRect = g_previewRect;
    g_viewRect.top += 6;
    g_viewRect.left += 6;
    g_viewRect.right -= 6;
    g_viewRect.bottom = g_viewBarRect.top - 4;

    int x = g_viewBarRect.left + 8, y = g_viewBarRect.top + 2, h = 22;
    MoveWindow(g_lodCombo, x, y, 96, 200, TRUE);       x += 102;
    MoveWindow(g_posLabel, x, y + 4, 56, 16, TRUE);    x += 58;
    MoveWindow(g_posX, x, y, 58, h, TRUE);             x += 62;
    MoveWindow(g_posY, x, y, 58, h, TRUE);             x += 62;
    MoveWindow(g_posZ, x, y, 58, h, TRUE);             x += 66;
    MoveWindow(g_resetBtn, x, y, 76, h, TRUE);          x += 82;
    MoveWindow(g_texCheck, x, y + 2, 80, 18, TRUE);    x += 84;
    MoveWindow(g_vcolCheck, x, y + 2, 110, 18, TRUE);  x += 114;
    MoveWindow(g_kitCombo, x, y, 90, 200, TRUE);       x += 96;
    MoveWindow(g_paintCombo, x, y, 200, 300, TRUE);    x += 206;
    MoveWindow(g_animBtn, x, y, 76, h, TRUE);
    {
        int rx = g_viewBarRect.left + 8;
        MoveWindow(g_rimLabel, rx, y + 4, 70, 16, TRUE);   rx += 74;
        MoveWindow(g_rimCombo, rx, y, 240, 400, TRUE);     rx += 246;
        MoveWindow(g_rimApply, rx, y, 120, h, TRUE);
    }

    MoveWindow(g_textBox, g_viewRect.left, g_viewRect.top,
               g_viewRect.right - g_viewRect.left,
               g_viewRect.bottom - g_viewRect.top, TRUE);
    if (g_glWnd)
        MoveWindow(g_glWnd, g_viewRect.left, g_viewRect.top, g_viewRect.right - g_viewRect.left,
                   g_viewRect.bottom - g_viewRect.top, TRUE);
    // the player: |< << Play Stop >> >|  [position ..........]  0:12 / 1:30
    int bx = g_viewBarRect.left + 8;
    MoveWindow(g_prevBtn, bx, y, 34, h, TRUE);          bx += 38;
    MoveWindow(g_backBtn, bx, y, 34, h, TRUE);          bx += 38;
    MoveWindow(g_playBtn, bx, y, 40, h, TRUE);          bx += 44;
    MoveWindow(g_stopBtn, bx, y, 34, h, TRUE);          bx += 38;
    MoveWindow(g_fwdBtn, bx, y, 34, h, TRUE);           bx += 38;
    MoveWindow(g_nextBtn, bx, y, 34, h, TRUE);          bx += 42;
    int seekW = std::max(80, (int)(g_viewBarRect.right - 8 - 116 - bx));
    MoveWindow(g_seek, bx, y, seekW, h, TRUE);          bx += seekW + 6;
    MoveWindow(g_timeLabel, bx, y + 4, 110, 16, TRUE);
    videoLayout();
    g_viewDirty = true;
}

static void openGame(HWND hwnd, bool askProfile);
static HICON loadAppIcon(HINSTANCE inst, int cx, int cy);
static void encodeEditedFile(HWND hwnd);
static void importIntoSelected(HWND hwnd);
static void importModelIntoSelected(HWND hwnd, const Entry& e);
static bool savePending(HWND hwnd);
static void newCarFromModel(HWND hwnd);
static void addPending(const Entry& e, Bytes bytes, const std::string& what);
static void importDataIntoSelected(HWND hwnd, const Entry& e);

// Export: the selected asset through the Save As dialog (every format it
// can become, and the file itself), or a whole folder converted
static void exportSelected(HWND hwnd) {
    HTREEITEM sel = TreeView_GetSelection(g_tree);
    auto ent = g_itemToEntry.find(sel);
    if (ent != g_itemToEntry.end() && ent->second >= 0 && (size_t)ent->second < g_all.size()) {
        saveAsset(hwnd, g_all[ent->second]);
        return;
    }
    auto dir = g_itemToDir.find(sel);
    if (dir != g_itemToDir.end()) {
        std::string d = dir->second;
        size_t slash = d.find_last_of('/');
        std::string strip = slash == std::string::npos ? std::string() : d.substr(0, slash);
        startExport(hwnd, entriesUnder(d), strip, true);
        return;
    }
    MessageBoxW(hwnd, L"Select an asset or a folder in the tree first.", L"Export", MB_ICONINFORMATION);
}

// Import: a picture into a texture (every game); a model (.obj / .fbx) into a
// Real Racing 3 .m3g; an edited text file back into a Real Racing 3 data file
static void importSelected(HWND hwnd) {
    HTREEITEM sel = TreeView_GetSelection(g_tree);
    auto ent = g_itemToEntry.find(sel);
    if (ent == g_itemToEntry.end() || ent->second < 0 || (size_t)ent->second >= g_all.size()) {
        MessageBoxW(hwnd, L"Select the asset to replace in the tree first.", L"Import", MB_ICONINFORMATION);
        return;
    }
    const Entry e = g_all[ent->second];
    std::string ext = nfsnl::extensionOf(e.path);
    if (e.kind == "texture" || ext == "sba" || ext == "pvr" || ext == "dds" || ext == "png" || ext == "jpg") {
        importIntoSelected(hwnd);
        return;
    }
    bool rr3 = g_profile && g_profile->id == "real_racing_3";
    if (ext == "m3g" && rr3) { importModelIntoSelected(hwnd, e); return; }
    // everything else that is not a model or a sound opens in the data
    // editor (plain text, .sounddef, tables); encrypted files say so there
    bool model = ext == "m3g" || ext == "sb3d" || ext == "sba";
    if (rr3 || (!model && e.kind != "sound") || ext == "sounddef") { importDataIntoSelected(hwnd, e); return; }
    MessageBoxW(hwnd, L"Import puts a picture into a texture in every game, and opens data and text "
                      L"files in the editor.\n\n"
                      L"Models (.obj / .fbx into .m3g) can be imported for Real Racing 3.",
                L"Import", MB_ICONINFORMATION);
}
static void openPictureFile(HWND hwnd);

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_tree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                                 WS_CHILD | WS_VISIBLE | TVS_HASBUTTONS | TVS_HASLINES |
                                 TVS_LINESATROOT | TVS_SHOWSELALWAYS,
                                 0, 0, 100, 100, hwnd, (HMENU)IDC_TREE, g_inst, nullptr);
        // kept for its id, but the box carries its own grey "Search" hint now
        g_searchLabel = CreateWindowExW(0, L"STATIC", L"Search",
                                        WS_CHILD | SS_LEFTNOWORDWRAP,
                                        0, 0, 44, 16, hwnd, (HMENU)IDC_SEARCHLABEL,
                                        g_inst, nullptr);
        g_searchBox = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                      0, 0, 100, 22, hwnd, (HMENU)IDC_SEARCH,
                                      g_inst, nullptr);
        SendMessageW(g_searchBox, 0x1501 /* EM_SETCUEBANNER */, TRUE,
                     (LPARAM)L"Search files (several words: all must match)");
        g_saveEdBtn = CreateWindowExW(0, L"BUTTON", L"Save Editor",
                                      WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                      0, 0, 26, 24, hwnd, (HMENU)IDC_SAVEEDITOR, g_inst, nullptr);
        g_tbOpen = CreateWindowExW(0, L"BUTTON", L"Open game", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                   0, 0, 26, 24, hwnd, (HMENU)IDC_TB_OPEN, g_inst, nullptr);
        g_tbExport = CreateWindowExW(0, L"BUTTON", L"Export", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                     0, 0, 26, 24, hwnd, (HMENU)IDC_TB_EXPORT, g_inst, nullptr);
        g_tbImport = CreateWindowExW(0, L"BUTTON", L"Import", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                     0, 0, 26, 24, hwnd, (HMENU)IDC_TB_IMPORT, g_inst, nullptr);
        g_tbSave = CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_DISABLED,
                                   0, 0, 26, 24, hwnd, (HMENU)IDC_TB_SAVE, g_inst, nullptr);
        g_tbNewCar = CreateWindowExW(0, L"BUTTON", L"New car", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_DISABLED,
                                     0, 0, 26, 24, hwnd, (HMENU)IDC_TB_NEWCAR, g_inst, nullptr);
        g_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                    WS_POPUP | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT,
                                    CW_USEDEFAULT, CW_USEDEFAULT, hwnd, nullptr, g_inst, nullptr);
        if (g_tooltip) {
            TOOLINFOW ti{};
            ti.cbSize = sizeof(ti);
            ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            ti.hwnd = hwnd;
            ti.uId = (UINT_PTR)g_saveEdBtn;
            ti.lpszText = (LPWSTR)L"Save Editor - edit No Limits and Real Racing 3 saves (Ctrl+E)";
            SendMessageW(g_tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
            const struct { HWND h; const wchar_t* tip; } tips[] = {
                { g_tbOpen, L"Open a game (Ctrl+O)" },
                { g_tbExport, L"Export the selected asset or folder - PNG, DDS, FBX, OBJ, WAV, TXT or the file itself" },
                { g_tbImport, L"Import into the selected asset - a picture into a texture (every game); "
                              L"a model or edited data into Real Racing 3's files" },
                { g_tbSave, L"Save - write the imported textures and models into the game's files (Ctrl+S)" },
                { g_tbNewCar, L"New car - build a Real Racing 3 car (.m3g) from an OBJ / FBX model, "
                              L"on the frame of the selected car" },
            };
            for (const auto& t : tips) {
                ti.uId = (UINT_PTR)t.h;
                ti.lpszText = (LPWSTR)t.tip;
                SendMessageW(g_tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
            }
        }
        g_progress = CreateWindowExW(0, PROGRESS_CLASSW, L"",
                                     WS_CHILD, 0, 0, 100, 16, hwnd,
                                     (HMENU)IDC_PROGRESS, g_inst, nullptr);
        SendMessageW(g_progress, PBM_SETRANGE32, 0, 1000);
        g_status = CreateWindowExW(0, L"STATIC", L"Monkey Tool 1.2.2 - choose a game to begin.",
                                   WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                   0, 0, 100, 20, hwnd, (HMENU)IDC_STATUS, g_inst, nullptr);
        // viewer controls; hidden until a model is selected
        g_lodCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                                     WS_CHILD | WS_VSCROLL | CBS_DROPDOWNLIST,
                                     0, 0, 90, 200, hwnd, (HMENU)IDC_LODCOMBO, g_inst, nullptr);
        g_kitCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                                     WS_CHILD | WS_VSCROLL | CBS_DROPDOWNLIST,
                                     0, 0, 90, 200, hwnd, (HMENU)IDC_KITCOMBO, g_inst, nullptr);
        g_paintCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                                     WS_CHILD | WS_VSCROLL | CBS_DROPDOWNLIST,
                                     0, 0, 120, 300, hwnd, (HMENU)IDC_PAINTCOMBO, g_inst, nullptr);
        g_rimLabel = CreateWindowExW(0, L"STATIC", L"CAR COLOR",
                                     WS_CHILD | SS_LEFTNOWORDWRAP,
                                     0, 0, 70, 16, hwnd, (HMENU)IDC_RIMLABEL, g_inst, nullptr);
        g_rimCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                                     WS_CHILD | WS_VSCROLL | CBS_DROPDOWNLIST,
                                     0, 0, 220, 400, hwnd, (HMENU)IDC_RIMCOMBO, g_inst, nullptr);
        g_rimApply = CreateWindowExW(0, L"BUTTON", L"Put into texture",
                                     WS_CHILD | BS_PUSHBUTTON,
                                     0, 0, 120, 22, hwnd, (HMENU)IDC_RIMAPPLY, g_inst, nullptr);
        g_posLabel = CreateWindowExW(0, L"STATIC", L"Move X Y Z",
                                     WS_CHILD | SS_LEFTNOWORDWRAP,
                                     0, 0, 60, 16, hwnd, (HMENU)IDC_POSLABEL, g_inst, nullptr);
        g_posX = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0.000",
                                 WS_CHILD | ES_AUTOHSCROLL,
                                 0, 0, 56, 22, hwnd, (HMENU)IDC_POSX, g_inst, nullptr);
        g_posY = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0.000",
                                 WS_CHILD | ES_AUTOHSCROLL,
                                 0, 0, 56, 22, hwnd, (HMENU)IDC_POSY, g_inst, nullptr);
        g_posZ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0.000",
                                 WS_CHILD | ES_AUTOHSCROLL,
                                 0, 0, 56, 22, hwnd, (HMENU)IDC_POSZ, g_inst, nullptr);
        g_resetBtn = CreateWindowExW(0, L"BUTTON", L"Reset view",
                                     WS_CHILD | BS_PUSHBUTTON,
                                     0, 0, 76, 22, hwnd, (HMENU)IDC_RESETVIEW, g_inst, nullptr);
        g_animBtn = CreateWindowExW(0, L"BUTTON", L"Animate",
                                    WS_CHILD | BS_PUSHBUTTON,
                                    0, 0, 76, 22, hwnd, (HMENU)IDC_ANIMATE, g_inst, nullptr);
        g_texCheck = CreateWindowExW(0, L"BUTTON", L"Textures",
                                     WS_CHILD | BS_AUTOCHECKBOX,
                                     0, 0, 80, 22, hwnd, (HMENU)IDC_TEXTURED, g_inst, nullptr);
        SendMessageW(g_texCheck, BM_SETCHECK, BST_CHECKED, 0);
        // shows the meshes' vertex colour - on these cars, baked occlusion
        g_vcolCheck = CreateWindowExW(0, L"BUTTON", L"Vertex colours",
                                      WS_CHILD | BS_AUTOCHECKBOX,
                                      0, 0, 110, 22, hwnd, (HMENU)IDC_VCOLORS, g_inst, nullptr);
        g_textBox = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                                    ES_READONLY | ES_AUTOVSCROLL,
                                    0, 0, 100, 100, hwnd, (HMENU)IDC_TEXTVIEW, g_inst, nullptr);
        g_playBtn = CreateWindowExW(0, L"BUTTON", L"Play",
                                    WS_CHILD | BS_OWNERDRAW,
                                    0, 0, 64, 22, hwnd, (HMENU)IDC_PLAY, g_inst, nullptr);
        g_stopBtn = CreateWindowExW(0, L"BUTTON", L"Stop",
                                    WS_CHILD | BS_OWNERDRAW,
                                    0, 0, 64, 22, hwnd, (HMENU)IDC_STOP, g_inst, nullptr);
        {
            const wchar_t* labels[4] = { L"Name", L"Ext", L"Size", L"Date" };
            for (int k = 0; k < 4; ++k) {
                g_sortBtn[k] = CreateWindowExW(0, L"BUTTON", labels[k],
                                               WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_LEFT,
                                               0, 0, 80, 20, hwnd,
                                               (HMENU)(INT_PTR)(IDC_SORT_NAME + k), g_inst, nullptr);
                SendMessageW(g_sortBtn[k], WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
            }
            updateSortLabels();
        }
        g_prevBtn = CreateWindowExW(0, L"BUTTON", L"|<", WS_CHILD | BS_OWNERDRAW,
                                    0, 0, 34, 22, hwnd, (HMENU)IDC_PREV, g_inst, nullptr);
        g_backBtn = CreateWindowExW(0, L"BUTTON", L"<<", WS_CHILD | BS_OWNERDRAW,
                                    0, 0, 34, 22, hwnd, (HMENU)IDC_BACK, g_inst, nullptr);
        g_fwdBtn = CreateWindowExW(0, L"BUTTON", L">>", WS_CHILD | BS_OWNERDRAW,
                                   0, 0, 34, 22, hwnd, (HMENU)IDC_FWD, g_inst, nullptr);
        g_nextBtn = CreateWindowExW(0, L"BUTTON", L">|", WS_CHILD | BS_OWNERDRAW,
                                    0, 0, 34, 22, hwnd, (HMENU)IDC_NEXT, g_inst, nullptr);
        g_seek = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | TBS_HORZ | TBS_NOTICKS,
                                 0, 0, 200, 22, hwnd, (HMENU)IDC_SEEK, g_inst, nullptr);
        g_timeLabel = CreateWindowExW(0, L"STATIC", L"0:00 / 0:00", WS_CHILD,
                                      0, 0, 110, 18, hwnd, (HMENU)IDC_TIME, g_inst, nullptr);

        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        // the asset tree reads at 13 pt, its header at 10
        g_treeFont = CreateFontW(-17, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                 CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_headerFont = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                                   CLEARTYPE_QUALITY, 0, L"Segoe UI");
        SendMessageW(g_tree, WM_SETFONT, (WPARAM)(g_treeFont ? g_treeFont : f), TRUE);
        for (HWND hb : g_sortBtn) if (hb) SendMessageW(hb, WM_SETFONT, (WPARAM)(g_headerFont ? g_headerFont : f), TRUE);
        g_treeImages = makeImageList(IDR_TREEICONS);
        if (g_treeImages) TreeView_SetImageList(g_tree, g_treeImages, TVSIL_NORMAL);
        SendMessageW(g_status, WM_SETFONT, (WPARAM)f, TRUE);
        for (HWND c : {g_rimLabel, g_rimCombo, g_rimApply})
            SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
        for (HWND c : {g_lodCombo, g_kitCombo, g_paintCombo, g_posLabel, g_posX, g_posY, g_posZ, g_resetBtn, g_animBtn,
                       g_texCheck, g_vcolCheck, g_textBox, g_playBtn, g_stopBtn,
                       g_prevBtn, g_backBtn, g_fwdBtn, g_nextBtn, g_timeLabel,
                       g_searchBox, g_searchLabel})
            SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
        createGlView(hwnd);
        layout(hwnd);
        return 0;
    }

    // ---- viewer input: turn, slide, zoom, and move the model itself ----
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (!g_viewActive || !PtInRect(&g_viewRect, pt)) break;
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (msg == WM_LBUTTONDOWN) g_dragMode = ctrl ? 3 : 1;
        else g_dragMode = 2;
        g_dragFrom = pt;
        SetCapture(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g_dragMode) break;
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        float dx = (float)(pt.x - g_dragFrom.x), dy = (float)(pt.y - g_dragFrom.y);
        g_dragFrom = pt;
        if (g_dragMode == 1) {
            g_view.yaw   -= dx * 0.01f;      // the model follows the pointer
            g_view.pitch += dy * 0.01f;
            const float lim = 1.5533f;      // just short of straight down
            g_view.pitch = std::min(std::max(g_view.pitch, -lim), lim);
        } else {
            // both slide and move scale with the distance, so the model keeps
            // up with the pointer at every zoom level
            float k = g_view.distance * 0.0016f;
            if (g_dragMode == 2) {
                g_view.pan[0] -= dx * k;
                g_view.pan[1] += dy * k;
            } else {
                // move along the two axes that face the camera
                float cy = std::cos(g_view.yaw), sy = std::sin(g_view.yaw);
                g_view.modelPos[0] -= dx * k * cy;
                g_view.modelPos[2] += dx * k * sy;
                g_view.modelPos[1] -= dy * k;
                setPosBoxes();
            }
        }
        g_viewDirty = true;
        InvalidateRect(hwnd, &g_viewRect, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        if (g_dragMode) { g_dragMode = 0; ReleaseCapture(); return 0; }
        break;

    case WM_MOUSEWHEEL: {
        if (!g_viewActive) break;
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        if (!PtInRect(&g_viewRect, pt)) break;
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        g_view.distance *= (delta > 0) ? 0.88f : 1.0f / 0.88f;
        float minD = g_view.radius * 0.15f + 0.001f;
        g_view.distance = std::min(std::max(g_view.distance, minD), g_view.radius * 60.0f);
        g_viewDirty = true;
        InvalidateRect(hwnd, &g_viewRect, FALSE);
        return 0;
    }
    case WM_SIZE:
        layout(hwnd);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        paintPreview(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_MEASUREITEM: {
        LPMEASUREITEMSTRUCT mis = (LPMEASUREITEMSTRUCT)lp;
        if (mis->CtlType != ODT_MENU) break;
        const wchar_t* text = (mis->itemData && mis->itemData <= g_menuLabels.size())
                              ? g_menuLabels[mis->itemData - 1].c_str() : L"";
        HDC dc = GetDC(hwnd);
        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HFONT old = (HFONT)SelectObject(dc, f);
        SIZE sz{};
        GetTextExtentPoint32W(dc, text, (int)wcslen(text), &sz);
        SelectObject(dc, old);
        ReleaseDC(hwnd, dc);
        mis->itemWidth = sz.cx + 18;
        mis->itemHeight = sz.cy + 8;
        return TRUE;
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lp;
        if (dis->CtlType == ODT_BUTTON &&
            (dis->CtlID == IDC_PLAY || dis->CtlID == IDC_STOP || dis->CtlID == IDC_PREV ||
             dis->CtlID == IDC_NEXT || dis->CtlID == IDC_BACK || dis->CtlID == IDC_FWD)) {
            drawPlayerButton(dis);
            return TRUE;
        }
        if (dis->CtlType == ODT_BUTTON && dis->CtlID == IDC_SAVEEDITOR) {
            drawToolButton(dis, 5);
            return TRUE;
        }
        if (dis->CtlType == ODT_BUTTON &&
            (dis->CtlID == IDC_TB_OPEN || dis->CtlID == IDC_TB_EXPORT || dis->CtlID == IDC_TB_IMPORT ||
             dis->CtlID == IDC_TB_SAVE || dis->CtlID == IDC_TB_NEWCAR)) {
            int icon = dis->CtlID == IDC_TB_OPEN ? 0 : dis->CtlID == IDC_TB_EXPORT ? 1 :
                       dis->CtlID == IDC_TB_IMPORT ? 2 : dis->CtlID == IDC_TB_SAVE ? 3 : 4;
            drawToolButton(dis, icon);
            return TRUE;
        }
        if (dis->CtlType != ODT_MENU) break;
        const wchar_t* text = (dis->itemData && dis->itemData <= g_menuLabels.size())
                              ? g_menuLabels[dis->itemData - 1].c_str() : L"";
        bool selected = (dis->itemState & ODS_SELECTED) != 0;
        bool disabled = (dis->itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
        bool checked  = (dis->itemState & ODS_CHECKED) != 0;

        COLORREF bg = selected
            ? (g_dark ? RGB(0x3A, 0x3A, 0x46) : RGB(0xCC, 0xE0, 0xFF))
            : pal().controlBg;
        HBRUSH b = CreateSolidBrush(bg);
        FillRect(dis->hDC, &dis->rcItem, b);
        DeleteObject(b);

        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, disabled ? pal().dimText : pal().text);
        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HFONT old = (HFONT)SelectObject(dis->hDC, f);

        RECT tr = dis->rcItem;
        tr.left += 9;
        tr.right -= 9;
        // No DT_NOPREFIX: '&' marks the keyboard accelerator and must be
        // turned into an underline, not printed. DT_HIDEPREFIX drops the
        // underline until Windows says to show accelerator cues.
        UINT prefix = (LOWORD(SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0)) & UISF_HIDEACCEL)
                      ? DT_HIDEPREFIX : 0;
        DrawTextW(dis->hDC, text, -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | prefix);
        if (checked) {
            RECT cr = dis->rcItem;
            cr.left += 2; cr.right = cr.left + 8;
            DrawTextW(dis->hDC, L"\u2022", -1, &cr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(dis->hDC, old);
        return TRUE;
    }

    // The strip the menu bar sits on is non-client area, so owner-drawing the
    // items alone still leaves a light band behind and beside them in dark
    // mode. WM_UAHDRAWMENU is how the shell asks an app to paint that band;
    // it is undocumented but is what every dark-mode Win32 app uses, and the
    // handler only fills a rectangle, so on a build that never sends it
    // nothing changes.
    case WM_UAHDRAWMENU: {
        if (!g_dark) break;
        UAHMENU* um = (UAHMENU*)lp;
        MENUBARINFO mbi{};
        mbi.cbSize = sizeof(mbi);
        if (!GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi)) break;
        RECT win;
        GetWindowRect(hwnd, &win);
        RECT rc = mbi.rcBar;
        OffsetRect(&rc, -win.left, -win.top);
        FillRect(um->hdc, &rc, g_controlBrush);
        return 0;
    }

    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, g_bgBrush ? g_bgBrush : (HBRUSH)(COLOR_WINDOW + 1));
        return 1;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, pal().text);
        // the Move X Y Z label sits on the pane, not on the window background
        bool onPane = (HWND)lp == g_posLabel || (HWND)lp == g_texCheck || (HWND)lp == g_vcolCheck ||
                      (HWND)lp == g_timeLabel || (HWND)lp == g_rimLabel;
        SetBkColor(dc, onPane ? pal().previewBg : pal().windowBg);
        if (onPane) {
            static HBRUSH paneBrush = nullptr;
            static COLORREF paneColour = 0;
            if (!paneBrush || paneColour != pal().previewBg) {
                if (paneBrush) DeleteObject(paneBrush);
                paneColour = pal().previewBg;
                paneBrush = CreateSolidBrush(paneColour);
            }
            return (LRESULT)paneBrush;
        }
        return (LRESULT)(g_bgBrush ? g_bgBrush : (HBRUSH)(COLOR_WINDOW + 1));
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, pal().text);
        SetBkColor(dc, pal().controlBg);
        return (LRESULT)(g_controlBrush ? g_controlBrush : (HBRUSH)(COLOR_WINDOW + 1));
    }

    case WM_NOTIFY: {
        LPNMHDR nm = (LPNMHDR)lp;
        if (nm->idFrom == IDC_TREE) {
            if (nm->code == TVN_ITEMEXPANDINGW) {
                LPNMTREEVIEWW tv = (LPNMTREEVIEWW)lp;
                if (tv->action == TVE_EXPAND) {
                    auto it = g_itemToEntry.find(tv->itemNew.hItem);
                    if (it != g_itemToEntry.end() && it->second >= 0 &&
                        (size_t)it->second < g_all.size() &&
                        nfsnl::extensionOf(g_all[it->second].path) == "bnk")
                        expandBank(tv->itemNew.hItem, it->second);
                }
            } else if (nm->code == NM_RCLICK) {
                treeContextMenu(hwnd);
                return 1;
            } else if (nm->code == TVN_SELCHANGINGW) {
                LPNMTREEVIEWW tv = (LPNMTREEVIEWW)lp;
                HTREEITEM to = tv->itemNew.hItem, from = tv->itemOld.hItem;
                bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
                if (tv->action == TVC_BYMOUSE && to && (ctrl || shift)) {
                    if (ctrl && !shift) {
                        if (g_multi.empty() && from) g_multi.insert(from);
                        if (g_multi.count(to)) g_multi.erase(to); else g_multi.insert(to);
                        g_anchor = to;
                    } else {
                        // every visible row between the anchor and this one
                        HTREEITEM a = g_anchor ? g_anchor : from;
                        if (!ctrl) g_multi.clear();
                        if (a) {
                            // the rows as shown, top to bottom (the anchor may be
                            // scrolled out of view, so from the root)
                            std::vector<HTREEITEM> rows;
                            for (HTREEITEM it = TreeView_GetRoot(g_tree); it; ) {
                                rows.push_back(it);
                                HTREEITEM child = (TreeView_GetItemState(g_tree, it, TVIS_EXPANDED) & TVIS_EXPANDED)
                                                      ? TreeView_GetChild(g_tree, it) : nullptr;
                                if (child) { it = child; continue; }
                                HTREEITEM next = TreeView_GetNextSibling(g_tree, it);
                                while (!next && it) { it = TreeView_GetParent(g_tree, it); if (it) next = TreeView_GetNextSibling(g_tree, it); }
                                it = next;
                            }
                            auto ia = std::find(rows.begin(), rows.end(), a), ib = std::find(rows.begin(), rows.end(), to);
                            if (ia != rows.end() && ib != rows.end()) {
                                if (ia > ib) std::swap(ia, ib);
                                for (auto it = ia; it <= ib; ++it) g_multi.insert(*it);
                            } else g_multi.insert(to);
                        } else g_multi.insert(to);
                    }
                    if (g_multi.size() == 1 && g_multi.count(from)) g_multi.clear();
                    InvalidateRect(g_tree, nullptr, TRUE);
                    showMultiStatus();
                    return TRUE;          // the tree's own selection stays where it was
                }
                if (!g_multi.empty()) { g_multi.clear(); InvalidateRect(g_tree, nullptr, TRUE); }
                g_anchor = to;
                return FALSE;
            } else if (nm->code == NM_CUSTOMDRAW) {
                LPNMTVCUSTOMDRAW cd = (LPNMTVCUSTOMDRAW)lp;
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return g_multi.empty() ? CDRF_DODEFAULT : CDRF_NOTIFYITEMDRAW;
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT && g_multi.count((HTREEITEM)cd->nmcd.dwItemSpec)) {
                    cd->clrText = GetSysColor(COLOR_HIGHLIGHTTEXT);
                    cd->clrTextBk = GetSysColor(COLOR_HIGHLIGHT);
                }
                return CDRF_DODEFAULT;
            } else if (nm->code == TVN_SELCHANGEDW) {
                LPNMTREEVIEWW tv = (LPNMTREEVIEWW)lp;
                if (g_busy) {
                    // the library is being read by the export - no preview now
                    setStatus("Busy writing files - the preview comes back when that is done.");
                    return 0;
                }
                auto snd = g_itemToSound.find(tv->itemNew.hItem);
                if (snd != g_itemToSound.end()) {
                    previewBankSound(snd->second);
                    return 0;
                }
                auto it = g_itemToEntry.find(tv->itemNew.hItem);
                if (it != g_itemToEntry.end() && it->second >= 0 &&
                    (size_t)it->second < g_all.size())
                    updatePreview(g_all[it->second]);
                else {
                    g_preview = nfsnl::Image();
                    g_previewText = "Folder - expand it to see the assets inside.";
                    InvalidateRect(hwnd, &g_previewRect, TRUE);
                }
            } else if (nm->code == NM_DBLCLK) {
                if (g_busy) return 1;
                HTREEITEM sel = TreeView_GetSelection(g_tree);
                auto snd = g_itemToSound.find(sel);
                if (snd != g_itemToSound.end()) {
                    saveBankSound(hwnd, snd->second);
                    return 1;
                }
                auto it = g_itemToEntry.find(sel);
                if (it != g_itemToEntry.end() && it->second >= 0 &&
                    (size_t)it->second < g_all.size()) {
                    saveAsset(hwnd, g_all[it->second]);
                    return 1;   // don't also toggle expand
                }
            }
        }
        return 0;
    }

    // The search box rebuilds the tree, which on a full install means walking
    // 70,000-odd assets. Doing that on every keystroke makes typing feel like
    // wading, so a keystroke only restarts a short timer and the rebuild
    // happens once the typing stops.
    case WM_HSCROLL:
        if ((HWND)lp == g_seek && g_seek && videoActive()) {
            int code = LOWORD(wp);
            if (code == TB_THUMBTRACK) {
#ifdef MT_HAVE_MFPLAY
                g_video.dragging = true;
#endif
                return 0;
            }
#ifdef MT_HAVE_MFPLAY
            g_video.dragging = false;
#endif
            if (code != TB_ENDTRACK) videoSeekTo((int)SendMessageW(g_seek, TBM_GETPOS, 0, 0));
            return 0;
        }
        if ((HWND)lp == g_seek && g_seek) {
            int code = LOWORD(wp);
            if (code == TB_THUMBTRACK) { g_player.dragging = true; return 0; }
            if (code == TB_ENDTRACK || code == TB_THUMBPOSITION) {
                g_player.dragging = false;
                if (!g_playable.empty() && playerLoad()) {
                    size_t tenth = std::max<size_t>(1, g_player.fmt.nAvgBytesPerSec / 10);
                    size_t at = (size_t)SendMessageW(g_seek, TBM_GETPOS, 0, 0) * tenth;
                    if (g_player.h) {
                        bool wasPaused = g_player.paused;
                        playerStartAt(at);
                        if (wasPaused && g_player.h) {
                            waveOutPause(g_player.h);
                            g_player.paused = true;
                            SetWindowTextW(g_playBtn, L"Play");
                        }
                    } else {
                        g_player.base = at;
                    }
                    playerShowTime(at);
                }
            }
            return 0;
        }
        break;
    case WM_TIMER:
        if (wp == IDT_PLAYER) { playerTick(); return 0; }
#ifdef MT_HAVE_MFPLAY
        if (wp == IDT_VIDEO) { videoShowTime(); return 0; }
#endif
        if (wp == IDT_ANIM) {
            // out and back: rest -> fully moved -> rest, over and over
            float dur = nfsnl::modelAnimDuration(g_viewModel);
            if (!g_viewActive || dur <= 0) { stopAnimation(); return 0; }
            float e = (float)((GetTickCount() - g_animStart) % (DWORD)(2 * dur));
            g_animT = e <= dur ? e : 2 * dur - e;
            if (g_glOk && g_glWnd) InvalidateRect(g_glWnd, nullptr, FALSE);
            else { g_viewDirty = true; InvalidateRect(hwnd, &g_viewRect, FALSE); }
            return 0;
        }
        if (wp == IDT_SEARCH) {
            KillTimer(hwnd, IDT_SEARCH);
            wchar_t buf[256];
            GetWindowTextW(g_searchBox, buf, 256);
            std::string typed = narrow(buf);
            for (char& c : typed) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            // trim, so a stray space does not hide everything
            while (!typed.empty() && typed.back() == ' ') typed.pop_back();
            size_t first = typed.find_first_not_of(' ');
            typed = first == std::string::npos ? "" : typed.substr(first);
            if (typed != g_search) {
                g_search = typed;
                if (!g_all.empty()) rebuildTree();
            }
            return 0;
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SEARCH:
            if (HIWORD(wp) == EN_CHANGE) {
                SetTimer(hwnd, IDT_SEARCH, 220, nullptr);
                return 0;
            }
            break;
        case IDC_POSX: case IDC_POSY: case IDC_POSZ:
            if (HIWORD(wp) == EN_CHANGE && !g_suppressPosEdit && g_viewActive) {
                g_view.modelPos[0] = readPosBox(g_posX);
                g_view.modelPos[1] = readPosBox(g_posY);
                g_view.modelPos[2] = readPosBox(g_posZ);
                g_viewDirty = true;
                InvalidateRect(hwnd, &g_viewRect, FALSE);
            }
            return 0;
        case IDC_LODCOMBO:
            if (HIWORD(wp) == CBN_SELCHANGE && g_comboMode == COMBO_SOUNDS) {
                selectBankSound((int)SendMessageW(g_lodCombo, CB_GETCURSEL, 0, 0));
                return 0;
            }
            if (HIWORD(wp) == CBN_SELCHANGE && g_comboMode == COMBO_FSB) {
                stopSound();
                selectFsbSound((int)SendMessageW(g_lodCombo, CB_GETCURSEL, 0, 0));
                return 0;
            }
            if (HIWORD(wp) == CBN_SELCHANGE && g_viewActive) {
                int sel = (int)SendMessageW(g_lodCombo, CB_GETCURSEL, 0, 0);
                wchar_t buf[64] = L"";
                if (sel > 0) SendMessageW(g_lodCombo, CB_GETLBTEXT, sel, (LPARAM)buf);
                g_view.lodFilter = narrow(buf);
                g_viewDirty = true;
                InvalidateRect(hwnd, &g_viewRect, FALSE);
            }
            return 0;
        case IDC_RIMCOMBO:
            if (HIWORD(wp) == CBN_SELCHANGE)
                rimShowSelection((int)SendMessageW(g_rimCombo, CB_GETCURSEL, 0, 0));
            return 0;
        case IDC_RIMAPPLY:
            putRimIntoTexture(hwnd);
            return 0;
        case IDC_PAINTCOMBO:
            if (HIWORD(wp) == CBN_SELCHANGE && g_viewActive)
                applyPaint((int)SendMessageW(g_paintCombo, CB_GETCURSEL, 0, 0));
            return 0;
        case IDC_KITCOMBO:
            if (HIWORD(wp) == CBN_SELCHANGE && g_viewActive) {
                int sel = (int)SendMessageW(g_kitCombo, CB_GETCURSEL, 0, 0);
                if (sel <= 0) g_view.kit = "stock";
                else if (sel == 1) g_view.kit = "all";
                else if ((size_t)(sel - 2) < g_kitLetters.size())
                    g_view.kit = std::string(1, g_kitLetters[sel - 2]);
                g_viewDirty = true;
                InvalidateRect(hwnd, &g_viewRect, FALSE);
            }
            return 0;
        case IDC_PLAY:
            if (videoActive()) { videoTogglePlay(); return 0; }
            if (!g_playable.empty()) playerTogglePlay();
            return 0;
        case IDC_STOP:
            if (videoActive()) { videoStop(); return 0; }
            playerClose();
            g_player.base = 0;
            playerShowTime(0);
            return 0;
        case IDC_SORT_NAME:
        case IDC_SORT_EXT:
        case IDC_SORT_SIZE:
        case IDC_SORT_DATE: {
            int k = LOWORD(wp) - IDC_SORT_NAME;
            if (k == g_sortKey) g_sortDesc = !g_sortDesc;
            else { g_sortKey = k; g_sortDesc = false; }
            updateSortLabels();
            if (!g_all.empty()) rebuildTree();
            return 0;
        }
        case IDC_BACK: playerSkip(-5.0); return 0;
        case IDC_FWD:  playerSkip(5.0);  return 0;
        case IDC_PREV: playerStep(-1);   return 0;
        case IDC_NEXT: playerStep(1);    return 0;
        case IDC_TEXTURED:
        case IDC_VCOLORS:
            if (g_viewActive) {
                g_view.textured = SendMessageW(g_texCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_view.vertexColors = SendMessageW(g_vcolCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_viewDirty = true;
                InvalidateRect(hwnd, &g_viewRect, FALSE);
            }
            return 0;
        case IDC_ANIMATE: {
            if (!g_viewActive) return 0;
            if (g_animPlaying) {
                KillTimer(hwnd, IDT_ANIM);
                g_animPlaying = false;
                SetWindowTextW(g_animBtn, L"Animate");
            } else {
                g_animPlaying = true;
                g_animStart = GetTickCount();
                SetWindowTextW(g_animBtn, L"Stop");
                SetTimer(hwnd, IDT_ANIM, 16, nullptr);
            }
            return 0;
        }
        case IDC_RESETVIEW: {
            if (!g_viewActive) return 0;
            std::string keepLod = g_view.lodFilter;
            std::string keepKit = g_view.kit;
            nfsnl::RenderView fresh;
            fresh.lodFilter = keepLod;
            fresh.kit = keepKit;
            fresh.textured = SendMessageW(g_texCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            fresh.vertexColors = SendMessageW(g_vcolCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            const Palette& p = pal();
            fresh.background[0] = (float)GetRValue(p.previewBg);
            fresh.background[1] = (float)GetGValue(p.previewBg);
            fresh.background[2] = (float)GetBValue(p.previewBg);
            g_view = fresh;
            nfsnl::frameModel(g_viewModel, g_view);
            setPosBoxes();
            g_viewDirty = true;
            InvalidateRect(hwnd, &g_previewRect, TRUE);
            return 0;
        }
        case ID_FILE_OPEN:
            openGame(hwnd, false);
            return 0;
        case ID_FILE_GAME:
            openGame(hwnd, true);
            return 0;
        case ID_FILE_CONVERT:
            convertAllShown(hwnd);
            return 0;
        case ID_FILE_EXPORT_SELECTED:
        case ID_FILE_EXTRACT_SELECTED:
            exportSelected(hwnd, LOWORD(wp) == ID_FILE_EXPORT_SELECTED);
            return 0;
        case ID_FILE_VGMSTREAM: {
            std::string existing;
            if (haveVgmstream(&existing)) {
                std::wstring msg = L"vgmstream is already set up:\n\n" + widen(existing) +
                                   L"\n\nPoint at a different copy?";
                if (MessageBoxW(hwnd, msg.c_str(), L"vgmstream",
                                MB_ICONQUESTION | MB_YESNO) != IDYES) return 0;
            }
            if (askForVgmstream(hwnd))
                setStatus("vgmstream is set up - Wwise Vorbis sounds now play and save as .wav.");
            return 0;
        }
        case ID_FILE_LZHAM: {
            if (nfsnl::lzhamAvailable()) {
                std::wstring msg = L"LZHAM is already loaded:\n\n" +
                                   widen(nfsnl::lzhamBackend()) +
                                   L"\n\nPoint at a different one?";
                if (MessageBoxW(hwnd, msg.c_str(), L"LZHAM",
                                MB_ICONQUESTION | MB_YESNO) != IDYES) return 0;
            }
            if (askForLzham(hwnd))
                setStatus("LZHAM is loaded - .m3g scene models now convert.");
            return 0;
        }
        case ID_FILE_NCT: {
            // One .nct per car, and the cipher is one pad XORed over all of
            // them, so a whole game finishes what four sample files started.
            // The .gui menus share the pad, and their names say what each
            // file is - a liveries list, a car's data, or XML - which is
            // what lets the learner use their structure.
            std::vector<Bytes> files;
            std::vector<std::string> names;
            for (const Entry& e : g_all) {
                std::string x = nfsnl::extensionOf(e.path);
                if (x != "nct" && x != "gui") continue;
                Bytes b;
                if (!readAsset(e, b) || b.size() < 16) continue;
                if (x == "gui" && !memcmp(b.data(), "<?xml", 5)) continue;   // not encoded
                files.push_back(std::move(b));
                names.push_back(e.path);
                if (files.size() >= 6000) break;
            }
            if (files.empty()) {
                MessageBoxW(hwnd,
                    L"No .nct or .gui files are open. Open Real Racing 3's .depot "
                    L"folder first - it holds one .nct per car.",
                    L"Nothing to read", MB_ICONINFORMATION);
                return 0;
            }
            std::string report;
            bool ok = nfsnl::nctLearnPad(files, names, report);
            if (ok) {
                std::string where = exeFolder() + "\\nct_pad.bin";
                if (nfsnl::nctPadSave(where))
                    report += "\n\nSaved next to the program, so it is there next time.";
                logLine("nct: %s", report.c_str());
                // anything on screen was decoded with the old pad
                HTREEITEM sel = TreeView_GetSelection(g_tree);
                auto it = g_itemToEntry.find(sel);
                if (it != g_itemToEntry.end() && it->second >= 0 &&
                    (size_t)it->second < g_all.size())
                    updatePreview(g_all[it->second]);
            }
            MessageBoxW(hwnd, widen(report).c_str(), L"Real Racing 3 .nct",
                        ok ? MB_ICONINFORMATION : MB_ICONWARNING);
            return 0;
        }
        case ID_FILE_NCT_ENCODE:
            encodeEditedFile(hwnd);
            return 0;
        case ID_TOOLS_SHOW_SOURCE: {
            // select the asset's .pack (or its separate .cab) in Explorer, so
            // it can be copied or sent as it is
            HTREEITEM sel = TreeView_GetSelection(g_tree);
            auto it = g_itemToEntry.find(sel);
            if (it == g_itemToEntry.end() || it->second < 0 || (size_t)it->second >= g_all.size()) {
                MessageBoxW(hwnd, L"Select an asset in the list first.", L"Show file", MB_ICONINFORMATION);
                return 0;
            }
            const Entry& en = g_all[it->second];
            std::string detail, file = sourceFileOf(en, &detail);
            if (file.empty()) return 0;
            const nfsnl::LibraryEntry& le = g_lib.entries[en.lib];
            const nfsnl::Archive& a = g_lib.archives[le.archive];
            if (!le.whole && le.cabinet < a.cabinets.size() &&
                (a.cabinets[le.cabinet].flags & nfsnl::CAB_EXTERNAL)) {
                std::string cab = nfsnl::stripExtension(file) + "\\" + std::to_string(le.cabinet) + ".cab";
                if (GetFileAttributesW(widen(cab).c_str()) != INVALID_FILE_ATTRIBUTES) file = cab;
            }
            std::wstring args = L"/select,\"" + widen(file) + L"\"";
            ShellExecuteW(hwnd, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            logLine("show source: %s -> %s", en.path.c_str(), file.c_str());
            return 0;
        }
        case ID_TOOLS_IMPORT:
            importIntoSelected(hwnd);
            return 0;
        case ID_FILE_OPEN_PICTURE:
            openPictureFile(hwnd);
            return 0;
        case ID_TOOLS_SAVE_EDITOR:
        case IDC_SAVEEDITOR:
            openSaveEditor(hwnd);
            return 0;
        case IDC_TB_OPEN:
            PostMessageW(hwnd, WM_COMMAND, ID_FILE_GAME, 0);
            return 0;
        case IDC_TB_EXPORT:
            exportSelected(hwnd);
            return 0;
        case IDC_TB_IMPORT:
            importSelected(hwnd);
            return 0;
        case IDC_TB_SAVE:
        case ID_FILE_SAVE_PENDING:
            savePending(hwnd);
            return 0;
        case IDC_TB_NEWCAR:
            newCarFromModel(hwnd);
            return 0;
        case ID_FILE_EXIT:
            SendMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        case ID_VIEW_ALL:   g_filter = "all";     rebuildTree(); return 0;
        case ID_VIEW_TEX:   g_filter = "texture"; rebuildTree(); return 0;
        case ID_VIEW_MODEL: g_filter = "model";   rebuildTree(); return 0;
        case ID_VIEW_SOUND: g_filter = "sound";   rebuildTree(); return 0;
        case ID_THEME_DARK:
        case ID_THEME_LIGHT:
            g_dark = (LOWORD(wp) == ID_THEME_DARK);
            saveThemePref();
            applyTheme();
            saveEditorSetTheme(saveEditorTheme());
            return 0;
        case ID_HELP_ABOUT:
            MessageBoxW(hwnd,
                L"Monkey Tool 1.2.2\n\n"
                L"Games: NFS No Limits, No Limits VR, Edge, Most Wanted,\n"
                L"Hot Pursuit; Real Racing Next, 3, 2, Real Racing and GTI.\n\n"
                L"Need for Speed: No Limits - unpacks the .pack archives and\n"
                L"converts textures to PNG/JPG/BMP/TGA and models to FBX/OBJ.\n\n"
                L"Real Racing 3 - reads .depot straight off disk: the zlib\n"
                L"wrappers come off on the way, the ETC textures decode here\n"
                L"instead of in PVRTexTool, and a model is placed using the\n"
                L".points file beside it, so the steering wheel, the needles\n"
                L"and the wheels land where the game puts them.\n\n"
                L"Type in the Search box above the tree to narrow it.\n"
                L"Double-click any asset to save it; the file-type dropdown\n"
                L"in the save dialog chooses the format.\n\n"
                L"Car models (.sb3d) need libzstd.dll next to this program.\n"
                L"No Limits' LZHAM-packed .m3g models and cabinets are read by\n"
                L"the decoder built into this program - no DLL needed.\n"
                L"Wwise Vorbis sounds need vgmstream-cli.exe.\n\n"
                L"The floppy-disk button (Ctrl+E) opens the built-in Save Editor for\n"
                L"No Limits and Real Racing 3 saves.\n\n"
                L"Tool by GM25, September 2026.",
                L"About Monkey Tool", MB_ICONINFORMATION);
            return 0;
        }
        return 0;

    // The picker runs modally, so it waits until the program is in its own
    // message loop rather than running it half-built from wWinMain.
    case WM_APP_STARTUP:
        openGame(hwnd, true);
        return 0;

    case WM_APP_PROGRESS: {
        std::string* text = (std::string*)lp;
        int pm = g_progressPermille.load();
        // while the loading window is up it alone shows the progress: no
        // status line and no second bar in the corner
        bool splash = g_splash != nullptr;
        if (text) {
            if (splash) updateSplash(*text, pm);
            else setStatus(*text);
            delete text;
        }
        if (pm >= 0 && !splash) {
            if (!IsWindowVisible(g_progress)) {
                ShowWindow(g_progress, SW_SHOW);
                layout(hwnd);
            }
            SendMessageW(g_progress, PBM_SETPOS, (WPARAM)pm, 0);
        }
        return 0;
    }

    case WM_APP_EXPORTED: {
        g_progressPermille.store(-1);
        if (IsWindowVisible(g_progress)) {
            ShowWindow(g_progress, SW_HIDE);
            layout(hwnd);
        }
        char buf[256];
        snprintf(buf, sizeof(buf), "Wrote %d file(s), %d could not be written.", (int)wp, (int)lp);
        setStatus(buf);
        logLine("export: %s", buf);
        MessageBoxW(hwnd, widen(buf).c_str(), L"Done", MB_ICONINFORMATION);
        return 0;
    }

    case WM_APP_DONE: {
        closeSplash();
        g_pending.clear();          // a new index: pending imports belonged to the old one
        g_texOverride.clear();
        g_progressPermille.store(-1);
        if (IsWindowVisible(g_progress)) {
            ShowWindow(g_progress, SW_HIDE);
            layout(hwnd);
        }
        rebuildTree();
        updateToolbarState();
        int problems = (int)wp;
        char buf[320];
        snprintf(buf, sizeof(buf),
                 "Ready. %d assets from %d archive(s). zstd: %s, LZHAM: %s",
                 (int)g_all.size(), (int)lp,
                 nfsnl::zstdAvailable() ? "yes" : "missing",
                 nfsnl::lzhamAvailable() ? "yes" : "missing (.m3g models stay compressed)");
        setStatus(buf);
        if (problems && !nfsnl::zstdAvailable())
            MessageBoxW(hwnd,
                L"Some content is Zstandard-compressed (this includes all 3D models).\n\n"
                L"Download libzstd.dll and place it in the same folder as this program, "
                L"then open the game folder again.",
                L"libzstd.dll not found", MB_ICONINFORMATION);
        return 0;
    }

    case WM_CLOSE:
        // unsaved edits in the Save Editor are not thrown away silently
        if (!saveEditorMayClose(hwnd)) return 0;
        if (!g_pending.empty()) {
            int a = MessageBoxW(hwnd, (L"You have " + std::to_wstring(g_pending.size()) +
                                       L" imported file(s) not saved yet.\n\nSave them before closing?").c_str(),
                                L"Monkey Tool", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (a == IDCANCEL) return 0;
            if (a == IDYES && !savePending(hwnd)) return 0;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_bgBrush) { DeleteObject(g_bgBrush); g_bgBrush = nullptr; }
        if (g_controlBrush) { DeleteObject(g_controlBrush); g_controlBrush = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}


// ----------------------------------------------------------- profile picker
//
// The tool opens on a game, the way Frosty does: pick the profile first, then
// point it at that game's files. Each card is drawn by hand so the logo, the
// name and the state (supported or not) all sit on one surface that follows
// the theme.

static const nfsnl::GameProfile* g_pickResult = nullptr;
static bool g_pickDone = false;
static int g_pickHover = -1;
static std::map<int, nfsnl::Image> g_logoCache;

// Game logos live in the exe as PNG bytes and are decoded by the tool's own
// reader, so the picker needs no image library.
static const nfsnl::Image* logoFor(const nfsnl::GameProfile& p) {
    if (!p.logoResource) return nullptr;
    auto it = g_logoCache.find(p.logoResource);
    if (it != g_logoCache.end()) return it->second.ok() ? &it->second : nullptr;
    nfsnl::Image img;
    HRSRC res = FindResourceW(g_inst, MAKEINTRESOURCEW(p.logoResource), (LPCWSTR)RT_RCDATA);
    if (res) {
        HGLOBAL h = LoadResource(g_inst, res);
        DWORD sz = SizeofResource(g_inst, res);
        const uint8_t* bytes = (const uint8_t*)LockResource(h);
        if (bytes && sz) nfsnl::decodePng(bytes, sz, img);
    }
    g_logoCache[p.logoResource] = std::move(img);
    const nfsnl::Image& stored = g_logoCache[p.logoResource];
    return stored.ok() ? &stored : nullptr;
}

static void drawImageAlpha(HDC hdc, const nfsnl::Image& img, int x, int y, int w, int h,
                           COLORREF over) {
    if (!img.ok() || w <= 0 || h <= 0) return;
    std::vector<uint8_t> bgra((size_t)img.width * img.height * 4);
    uint8_t br = GetRValue(over), bg = GetGValue(over), bb = GetBValue(over);
    int ch = img.channels;
    for (int i = 0; i < img.width * img.height; ++i) {
        const uint8_t* s = &img.pixels[(size_t)i * ch];
        uint8_t r = s[0], g = ch >= 3 ? s[1] : s[0], b = ch >= 3 ? s[2] : s[0];
        uint8_t a = ch == 4 ? s[3] : 255;
        uint8_t* d = &bgra[(size_t)i * 4];
        d[0] = (uint8_t)((b * a + bb * (255 - a)) / 255);
        d[1] = (uint8_t)((g * a + bg * (255 - a)) / 255);
        d[2] = (uint8_t)((r * a + br * (255 - a)) / 255);
        d[3] = 255;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = img.width;
    bi.bmiHeader.biHeight = -img.height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(hdc, HALFTONE);
    StretchDIBits(hdc, x, y, w, h, 0, 0, img.width, img.height,
                  bgra.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
}

// Any PNG or JPEG the exe carries (icons, loading art), decoded once
static const nfsnl::Image* resourceImage(int id) {
    static std::map<int, nfsnl::Image> cache;
    if (!id) return nullptr;
    auto it = cache.find(id);
    if (it != cache.end()) return it->second.ok() ? &it->second : nullptr;
    nfsnl::Image img;
    HRSRC res = FindResourceW(g_inst, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    if (res) {
        HGLOBAL h = LoadResource(g_inst, res);
        DWORD sz = SizeofResource(g_inst, res);
        const uint8_t* bytes = (const uint8_t*)LockResource(h);
        if (bytes && sz) nfsnl::decodeImageFile(bytes, sz, img);
    }
    cache[id] = std::move(img);
    return cache[id].ok() ? &cache[id] : nullptr;
}

// ---- the picker's layout: a Frosty-style "Select Game" box of app icons,
// Need for Speed on one row and Real Racing on the next ----
static const int kTileW = 108, kTileH = 124, kIconSz = 72;
static const int kPickSide = 20, kPickHeader = 52, kRowLabel = 24, kPickFooter = 56;
static int g_pickSel = -1;            // the tile clicked once (Select opens it)

static RECT tileRect(const nfsnl::GameProfile& p) {
    RECT r;
    r.left = kPickSide + p.pickerColumn * kTileW;
    r.top = kPickHeader + kRowLabel + p.pickerRow * (kTileH + kRowLabel);
    r.right = r.left + kTileW - 6;
    r.bottom = r.top + kTileH;
    return r;
}

static RECT pickButton(HWND hwnd, int which) {        // 0 Cancel, 1 Select
    RECT rc;
    GetClientRect(hwnd, &rc);
    RECT r;
    r.top = rc.bottom - kPickFooter + 12;
    r.bottom = r.top + 32;
    if (which == 0) { r.left = kPickSide; r.right = r.left + 96; }
    else { r.right = rc.right - kPickSide; r.left = r.right - 96; }
    return r;
}

static void paintPicker(HWND hwnd, HDC target) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    // drawn off screen, then copied: no flicker while the mouse moves
    HDC hdc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(hdc, bmp);

    COLORREF body = g_dark ? RGB(0x1B, 0x1B, 0x1F) : RGB(0xF4, 0xF4, 0xF6);
    COLORREF band = g_dark ? RGB(0x2A, 0x2A, 0x30) : RGB(0xE4, 0xE4, 0xEA);
    COLORREF text = g_dark ? RGB(0xEE, 0xEE, 0xEE) : RGB(0x20, 0x20, 0x24);
    COLORREF dim = g_dark ? RGB(0x9A, 0x9A, 0xA2) : RGB(0x60, 0x60, 0x68);
    HBRUSH b = CreateSolidBrush(body);
    FillRect(hdc, &rc, b);
    DeleteObject(b);
    RECT head{0, 0, rc.right, kPickHeader - 8};
    b = CreateSolidBrush(band);
    FillRect(hdc, &head, b);
    RECT foot{0, rc.bottom - kPickFooter, rc.right, rc.bottom};
    FillRect(hdc, &foot, b);
    DeleteObject(b);

    SetBkMode(hdc, TRANSPARENT);
    HFONT title = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                              CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT small = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                              CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT label = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                              CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(hdc, title);
    SetTextColor(hdc, text);
    DrawTextW(hdc, L"Select Game", -1, &head, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(hdc, label);
    SetTextColor(hdc, dim);
    const wchar_t* rows[2] = { L"NEED FOR SPEED", L"REAL RACING" };
    for (int row = 0; row < 2; ++row) {
        RECT lr{kPickSide, kPickHeader + row * (kTileH + kRowLabel), rc.right - kPickSide,
                kPickHeader + row * (kTileH + kRowLabel) + kRowLabel};
        DrawTextW(hdc, rows[row], -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    const auto& profiles = nfsnl::gameProfiles();
    for (size_t i = 0; i < profiles.size(); ++i) {
        const nfsnl::GameProfile& p = profiles[i];
        RECT r = tileRect(p);
        bool sel = (int)i == g_pickSel, hot = (int)i == g_pickHover;
        if (sel || hot) {
            COLORREF face = sel ? (g_dark ? RGB(0x2E, 0x4A, 0x6E) : RGB(0xCC, 0xE0, 0xFA))
                                : (g_dark ? RGB(0x2C, 0x2C, 0x34) : RGB(0xE6, 0xEA, 0xF2));
            HBRUSH fb = CreateSolidBrush(face);
            HPEN pen = CreatePen(PS_SOLID, 1, sel ? RGB(0x4A, 0x8C, 0xE0) : face);
            HGDIOBJ ob = SelectObject(hdc, fb), op = SelectObject(hdc, pen);
            RoundRect(hdc, r.left, r.top, r.right, r.bottom, 8, 8);
            SelectObject(hdc, ob);
            SelectObject(hdc, op);
            DeleteObject(fb);
            DeleteObject(pen);
        }
        const nfsnl::Image* icon = resourceImage(p.iconResource);
        COLORREF under = sel ? (g_dark ? RGB(0x2E, 0x4A, 0x6E) : RGB(0xCC, 0xE0, 0xFA))
                             : hot ? (g_dark ? RGB(0x2C, 0x2C, 0x34) : RGB(0xE6, 0xEA, 0xF2)) : body;
        int ix = r.left + (r.right - r.left - kIconSz) / 2, iy = r.top + 8;
        if (icon) drawImageAlpha(hdc, *icon, ix, iy, kIconSz, kIconSz, under);
        SelectObject(hdc, small);
        SetTextColor(hdc, p.supported ? text : dim);
        std::string shortName = p.name;
        if (shortName.compare(0, 16, "Need for Speed: ") == 0) shortName = shortName.substr(16);
        RECT tr{r.left + 2, iy + kIconSz + 4, r.right - 2, r.bottom - 2};
        DrawTextW(hdc, widen(shortName).c_str(), -1, &tr,
                  DT_CENTER | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    // the two buttons
    for (int k = 0; k < 2; ++k) {
        RECT br = pickButton(hwnd, k);
        bool enabled = k == 0 || g_pickSel >= 0;
        COLORREF face = k == 1 && enabled ? RGB(0x2F, 0x6F, 0xD0)
                                          : (g_dark ? RGB(0x3A, 0x3A, 0x42) : RGB(0xFF, 0xFF, 0xFF));
        HBRUSH fb = CreateSolidBrush(face);
        HPEN pen = CreatePen(PS_SOLID, 1, g_dark ? RGB(0x50, 0x50, 0x5A) : RGB(0xB8, 0xB8, 0xC0));
        HGDIOBJ ob = SelectObject(hdc, fb), op = SelectObject(hdc, pen);
        RoundRect(hdc, br.left, br.top, br.right, br.bottom, 6, 6);
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
        DeleteObject(fb);
        DeleteObject(pen);
        SelectObject(hdc, label);
        SetTextColor(hdc, k == 1 && enabled ? RGB(0xFF, 0xFF, 0xFF) : enabled ? text : dim);
        DrawTextW(hdc, k == 0 ? L"Cancel" : L"Select", -1, &br,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(hdc, small);
    SetTextColor(hdc, dim);
    RECT vr{kPickSide + 110, rc.bottom - kPickFooter, rc.right - kPickSide - 110, rc.bottom};
    DrawTextW(hdc, L"Monkey Tool 1.2.2 by GM25", -1, &vr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(hdc, oldFont);
    DeleteObject(title);
    DeleteObject(small);
    DeleteObject(label);
    BitBlt(target, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(hdc);
}

static int pickHit(HWND, POINT pt) {
    const auto& profiles = nfsnl::gameProfiles();
    for (size_t i = 0; i < profiles.size(); ++i) {
        RECT r = tileRect(profiles[i]);
        if (PtInRect(&r, pt)) return (int)i;
    }
    return -1;
}

static void pickOpen(HWND hwnd, int i) {
    const auto& profiles = nfsnl::gameProfiles();
    if (i < 0 || (size_t)i >= profiles.size()) return;
    if (!profiles[i].supported) {
        std::wstring msg = widen(profiles[i].name) + L" is not supported yet.\n\n" + widen(profiles[i].note);
        MessageBoxW(hwnd, msg.c_str(), L"Not yet", MB_ICONINFORMATION);
        return;
    }
    g_pickResult = &profiles[i];
    DestroyWindow(hwnd);
}

static LRESULT CALLBACK PickerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        paintPicker(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int hover = pickHit(hwnd, pt);
        if (hover != g_pickHover) {
            g_pickHover = hover;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        RECT c = pickButton(hwnd, 0), s2 = pickButton(hwnd, 1);
        if (PtInRect(&c, pt)) { g_pickResult = nullptr; DestroyWindow(hwnd); return 0; }
        if (PtInRect(&s2, pt)) { if (g_pickSel >= 0) pickOpen(hwnd, g_pickSel); return 0; }
        int hit = pickHit(hwnd, pt);
        if (hit != g_pickSel) { g_pickSel = hit; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int hit = pickHit(hwnd, pt);
        if (hit >= 0) pickOpen(hwnd, hit);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_RETURN && g_pickSel >= 0) pickOpen(hwnd, g_pickSel);
        else if (wp == VK_ESCAPE) { g_pickResult = nullptr; DestroyWindow(hwnd); }
        return 0;
    case WM_CLOSE:
        g_pickResult = nullptr;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        // deliberately no PostQuitMessage: this is a modal window inside the
        // program's own message loop, and quitting the loop here would take
        // the whole program with it
        g_pickDone = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Show the picker and wait for an answer. Returns null if the window was
// closed, which means the person does not want to open anything.
static const nfsnl::GameProfile* chooseProfile(HWND owner) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = PickerProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"MonkeyToolPicker";
        wc.hIcon = loadAppIcon(g_inst, 0, 0);
        wc.hIconSm = loadAppIcon(g_inst, GetSystemMetrics(SM_CXSMICON),
                                 GetSystemMetrics(SM_CYSMICON));
        RegisterClassExW(&wc);
        registered = true;
    }
    g_pickResult = nullptr;
    g_pickDone = false;
    g_pickHover = -1;
    g_pickSel = -1;
    if (g_profile) {
        const auto& profiles = nfsnl::gameProfiles();
        for (size_t i = 0; i < profiles.size(); ++i) if (&profiles[i] == g_profile) g_pickSel = (int)i;
    }

    int columns = 1;
    for (const nfsnl::GameProfile& gp : nfsnl::gameProfiles())
        columns = std::max(columns, gp.pickerColumn + 1);
    int width = kPickSide * 2 + columns * kTileW - 6;
    int height = kPickHeader + 2 * (kRowLabel + kTileH) + 8 + kPickFooter;
    RECT r{0, 0, width, height};
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    HWND dlg = CreateWindowExW(0, L"MonkeyToolPicker", L"Load Profile",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                               x, y, w, h, owner, nullptr, g_inst, nullptr);
    if (!dlg) return nullptr;
    themeTitleBar(dlg, g_dark);
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
    SetFocus(dlg);

    MSG msg;
    while (!g_pickDone && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (owner) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
    return g_pickResult;
}

static void openGameBody(HWND hwnd, bool askProfile);

// A quick look at what a folder holds: names two levels down, at most a few
// thousand, lower case - enough to tell the games apart.
static void sampleFolderNames(const std::string& dir, const std::string& prefix, int depth,
                              std::vector<std::string>& out) {
    if (depth > 3 || out.size() > 4000) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(widen(dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::string leaf = narrow(name);
        for (char& c : leaf) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out.push_back(leaf);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            sampleFolderNames(dir + "\\" + narrow(name), prefix + leaf + "/", depth + 1, out);
    } while (FindNextFileW(h, &fd) && out.size() <= 4000);
    FindClose(h);
}

// Pick a game, then its folder, then read the manifests. Anything that throws
// on the way becomes a message and a line in the log, never a dead program.
static void openGame(HWND hwnd, bool askProfile) {
    if (!g_pending.empty()) {
        int a = MessageBoxW(hwnd, (L"You have " + std::to_wstring(g_pending.size()) +
                                   L" imported file(s) not saved yet.\n\nSave them first?").c_str(),
                            L"Monkey Tool", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (a == IDCANCEL) return;
        if (a == IDYES && !savePending(hwnd)) return;
    }
    try {
        openGameBody(hwnd, askProfile);
    } catch (const std::exception& e) {
        logLine("openGame failed: %s", e.what());
        MessageBoxW(hwnd, (L"Could not open the game:\n\n" + widen(e.what())).c_str(),
                    L"Monkey Tool", MB_ICONERROR);
    } catch (...) {
        logLine("openGame failed: unknown exception");
        MessageBoxW(hwnd, L"Could not open the game.", L"Monkey Tool", MB_ICONERROR);
    }
}

static void openGameBody(HWND hwnd, bool askProfile) {
    if (g_busy) {
        MessageBoxW(hwnd, L"Still reading the archives - one moment.",
                    L"Monkey Tool", MB_ICONINFORMATION);
        return;
    }
    if (askProfile || !g_profile) {
        logLine("picker: opening");
        const nfsnl::GameProfile* p = chooseProfile(hwnd);
        if (!p) { logLine("picker: closed without a choice"); return; }
        g_profile = p;
        logLine("picker: chose %s", g_profile->id.c_str());
    }
    std::wstring title = L"Where is " + widen(g_profile->folderHint) + L"?";
    std::string folder;
    if (!pickFolder(hwnd, title.c_str(), folder)) {
        logLine("folder: cancelled");
        return;
    }
    logLine("folder: %s", folder.c_str());
    // A folder of another game than the one picked would load nothing, or
    // load wrongly. The folder usually says which game it is, so ask.
    {
        std::vector<std::string> names;
        sampleFolderNames(folder, "", 0, names);
        std::string id = nfsnl::detectGameFolder(folder, names);
        const nfsnl::GameProfile* seen = id.empty() ? nullptr : nfsnl::findProfile(id);
        if (seen && seen != g_profile && seen->supported) {
            std::wstring q = L"This folder looks like " + widen(seen->name) + L", but " +
                             widen(g_profile->name) + L" is selected.\n\nSwitch to " +
                             widen(seen->name) + L"?";
            int a = MessageBoxW(hwnd, q.c_str(), L"Monkey Tool", MB_YESNO | MB_ICONQUESTION);
            logLine("folder: looks like %s, user %s", id.c_str(), a == IDYES ? "switched" : "kept");
            if (a == IDYES) g_profile = seen;
        }
    }
    g_gameFolder = folder;
    std::wstring caption = L"Monkey Tool 1.2.2 - " + widen(g_profile->name);
    SetWindowTextW(hwnd, caption.c_str());
    setStatus("");
    startIndexThread(folder);
    logLine("indexing thread started");
}

// ----------------------------------------------------------------- pictures

static const wchar_t kPictureFilter[] =
    L"Pictures (*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.dds;*.pvr)\0"
    L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff;*.dds;*.pvr\0All files\0*.*\0";

static bool pickPicture(HWND owner, const wchar_t* title, std::string& path, nfsnl::Image& img) {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = kPictureFilter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    path = narrow(std::wstring(file));
    Bytes data;
    if (!nfsnl::readFile(path, data) || !nfsnl::decodeImageFile(data.data(), data.size(), img)) {
        MessageBoxW(owner, (L"Monkey Tool could not read this picture:\n" + widen(path)).c_str(),
                    L"Picture", MB_ICONERROR);
        return false;
    }
    return true;
}

// File > Open a picture: any PNG / JPG from the disk, shown in the preview
static void openPictureFile(HWND hwnd) {
    std::string path;
    nfsnl::Image img;
    if (!pickPicture(hwnd, L"Open a picture", path, img)) return;
    closeViewer();
    showTextView(false);
    stopSound();
    showSoundControls(false);
    g_comboMode = COMBO_NONE;
    ShowWindow(g_lodCombo, SW_HIDE);
    g_preview = img;
    char buf[MAX_PATH + 200];
    snprintf(buf, sizeof(buf), "%s\r\n%d x %d, %d channels\r\n"
             "Select a texture in the tree and use Tools > Import PNG/JPG (Ctrl+I) to put a "
             "picture into it.", path.c_str(), img.width, img.height, img.channels);
    g_previewText = buf;
    InvalidateRect(g_main, &g_previewRect, TRUE);
}

static bool importPictureInto(HWND hwnd, const Entry& e, const Bytes& original,
                              const nfsnl::Image& pic, const std::string& picPath,
                              std::string& reportOut, bool quiet = false);

// Tools > Import PNG/JPG: the selected texture, rebuilt around a picture in
// its own size, codec and mip levels, saved where the user says.
static void importIntoSelected(HWND hwnd) {
    HTREEITEM sel = TreeView_GetSelection(g_tree);
    auto it = g_itemToEntry.find(sel);
    if (it == g_itemToEntry.end() || it->second < 0 || (size_t)it->second >= g_all.size()) {
        MessageBoxW(hwnd, L"Select a texture in the list first (.sba, .pvr, .dds, .png or .jpg).",
                    L"Import picture", MB_ICONINFORMATION);
        return;
    }
    const Entry e = g_all[it->second];
    std::string ext = nfsnl::extensionOf(e.path);
    if (ext != "sba" && ext != "pvr" && ext != "dds" && ext != "png" && ext != "jpg" && ext != "jpeg") {
        MessageBoxW(hwnd, L"This asset is not a texture. Select a .sba, .pvr, .dds, .png or .jpg.",
                    L"Import picture", MB_ICONINFORMATION);
        return;
    }
    Bytes original;
    std::string err;
    if (!readAsset(e, original, &err)) {
        MessageBoxW(hwnd, (L"Could not read the texture.\n\n" + widen(err)).c_str(),
                    L"Import picture", MB_ICONERROR);
        return;
    }
    std::string picPath;
    nfsnl::Image pic;
    if (!pickPicture(hwnd, L"The picture to put into this texture", picPath, pic)) return;
    std::string report;
    importPictureInto(hwnd, e, original, pic, picPath, report);
}

// The picture rebuilt into the texture `e` (whose bytes are `original`), then
// saved where the user says. Shared by Import PNG/JPG and the Save Editor's
// profile picture. False when it did not happen; `report` says why.
static bool importPictureInto(HWND hwnd, const Entry& e, const Bytes& original,
                              const nfsnl::Image& pic, const std::string& picPath,
                              std::string& reportOut, bool quiet) {
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    Bytes result;
    std::string report;
    bool ok = nfsnl::importPicture(e.path, original, pic, result, report);
    SetCursor(old);
    if (!ok) {
        reportOut = "The picture could not be put into this texture:\n\n" + report;
        if (!quiet) MessageBoxW(hwnd, widen(reportOut).c_str(), L"Import picture", MB_ICONERROR);
        return false;
    }

    // The picture is in the tool from now on - preview, 3D view, export -
    // and goes into the game's file when Save is clicked.
    if (!quiet) {
        g_texOverride[e.path] = pic;
        addPending(e, result, "texture from " + nfsnl::baseName(picPath));
        logLine("import: %s + %s pending (%s)", e.path.c_str(), picPath.c_str(), report.c_str());
        reportOut = report;
        updatePreview(e);
        return true;
    }
    // Where it goes. A loose file (Real Racing, Most Wanted unpacked) is
    // offered back in its own place under its own name, with the original
    // kept as .bak; an asset inside a .pack or .obb is saved on its own.
    const nfsnl::LibraryEntry& le = g_lib.entries[e.lib];
    std::string diskFile;
    if (le.whole && le.archive >= 0 && (size_t)le.archive < g_lib.archives.size())
        diskFile = g_lib.archives[le.archive].path;
    bool wrapped = le.rr3Wrapped && nfsnl::extensionOf(diskFile) == "z";
    if (wrapped) result = nfsnl::rr3WrapZ(result);
    std::string leaf = diskFile.empty() ? nfsnl::baseName(e.path) : nfsnl::baseName(diskFile);
    std::wstring name = widen(diskFile.empty() ? leaf : diskFile);
    name.resize(MAX_PATH, 0);
    std::wstring ext2 = widen(nfsnl::extensionOf(leaf));
    std::wstring filt = L"Texture (*." + ext2 + L")";
    filt.push_back(0);
    filt += L"*." + ext2;
    filt.push_back(0);
    filt += L"All files";
    filt.push_back(0);
    filt += L"*.*";
    filt.push_back(0);
    OPENFILENAMEW sfn{};
    sfn.lStructSize = sizeof(sfn);
    sfn.hwndOwner = hwnd;
    sfn.lpstrFilter = filt.c_str();
    sfn.lpstrFile = &name[0];
    sfn.nMaxFile = MAX_PATH;
    sfn.lpstrTitle = L"Save the new texture as";
    sfn.lpstrDefExt = ext2.c_str();
    sfn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&sfn)) { reportOut.clear(); return false; }
    std::string where = narrow(std::wstring(name.c_str()));

    std::string backup;
    if (!diskFile.empty() && _wcsicmp(widen(where).c_str(), widen(diskFile).c_str()) == 0) {
        backup = diskFile + ".bak";
        if (GetFileAttributesW(widen(backup).c_str()) == INVALID_FILE_ATTRIBUTES)
            CopyFileW(widen(diskFile).c_str(), widen(backup).c_str(), TRUE);
    }
    if (!nfsnl::writeFile(where, result)) {
        reportOut = "Could not write\n" + where;
        if (!quiet) MessageBoxW(hwnd, widen(reportOut).c_str(), L"Import picture", MB_ICONERROR);
        return false;
    }
    g_texOverride[e.path] = pic;
    logLine("import: %s + %s -> %s (%s)", e.path.c_str(), picPath.c_str(), where.c_str(), report.c_str());
    std::string msg = report + "\n\nSaved as\n" + where;
    if (!backup.empty()) msg += "\n\nThe original is kept as\n" + backup;
    else if (diskFile.empty())
        msg += "\n\nThis texture lives inside a game archive, which the tool does not rewrite: "
               "the new file is the texture on its own. The 3D view shows your picture on "
               "models for the rest of this session.";
    reportOut = msg;
    if (!quiet) MessageBoxW(hwnd, widen(msg).c_str(), L"Import picture", MB_ICONINFORMATION);
    updatePreview(e);
    return true;
}

// The Save Editor's "Profile picture...", when the save keeps no picture: the
// game draws the player's avatar from a texture, found here by the name the
// save gives or, failing that, the game's default profile icon.
static bool setGameAvatar(HWND owner, const std::string& game, const std::string& avatar,
                          const nfsnl::Image& pic, std::string& report) {
    if (g_all.empty()) {
        report = "This save keeps no picture of its own: the game draws your avatar from one of "
                 "its textures.\n\nOpen the game in Monkey Tool first (File > Open game), then "
                 "click \"Profile picture...\" again, and the picture goes into that texture.";
        return false;
    }
    auto lower = [](std::string x) {
        for (char& c : x) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return x;
    };
    // Real Racing 3 keeps no avatar in its files at all: the picture beside
    // your name is your EA / Game Center / Facebook account's, fetched
    // online (the save's only picture-like field is the list of photo
    // filters you used)
    if (game == "rr3") {
        report = "Real Racing 3 has no profile picture in its save or its game files: the picture "
                 "beside your name is your EA / Google Play / Game Center / Facebook account's "
                 "picture, which the game downloads.\n\nChange it in that account, and the game "
                 "shows the new one the next time it connects.";
        return false;
    }
    std::string want = lower(avatar);
    int best = -1, bestScore = 0;
    for (size_t i = 0; i < g_all.size(); ++i) {
        std::string ext = nfsnl::extensionOf(g_all[i].path);
        if (ext != "sba" && ext != "pvr" && ext != "dds" && ext != "png" && ext != "jpg") continue;
        std::string p = lower(g_all[i].path);
        std::string stem = nfsnl::stripExtension(nfsnl::baseName(p));
        stem = stem.substr(0, stem.find('.'));
        int score = 0;
        // No Limits: the avatars are 2x/textures/ui/defaultavatars/texture_<name>.sba;
        // texture_icon_common_default_profile.sba is a vector (SVG) icon of
        // the HUD, not a picture, and cannot take one
        if (!want.empty() && (stem == want || stem == "texture_" + want)) score = 100;
        else if (!want.empty() && stem.find(want) != std::string::npos) score = 60;
        else if (stem == "texture_avatar_default") score = 50;
        else if (p.find("defaultavatars/") != std::string::npos && stem.find("default") != std::string::npos) score = 30;
        if (score && p.find("/remap/") != std::string::npos) score -= 5;   // event copies after the main one
        if (score && p.find("/2x/") != std::string::npos) score += 2;       // the sharper copy first
        if (score > bestScore) { bestScore = score; best = (int)i; }
    }
    if (best < 0) {
        report = "This save keeps no picture of its own, and no avatar texture was found in the "
                 "open game" + (avatar.empty() ? std::string(".") : " (the save names \"" + avatar + "\").");
        return false;
    }
    const Entry e = g_all[best];
    std::string q = "Your picture goes into the game's avatar texture:\n\n" + e.path +
                    (avatar.empty() ? std::string() : "\n\n(the save names the avatar \"" + avatar + "\")") +
                    "\n\nNext you choose where the new texture file is saved. Continue?";
    if (MessageBoxW(owner, widen(q).c_str(), L"Profile picture", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        report.clear();
        return false;
    }
    Bytes original;
    std::string err;
    if (!readAsset(e, original, &err)) { report = "Could not read " + e.path + ": " + err; return false; }
    // an avatar is square: crop the picture to the texture's own shape first
    nfsnl::Image cur;
    nfsnl::Image fitted = pic;
    if (nfsnl::decodeTextureFile(original.data(), original.size(), cur) && cur.width > 0)
        fitted = nfsnl::saves::fitPicture(pic, cur.width, cur.height);
    return importPictureInto(owner, e, original, fitted, "profile picture", report, true);
}

// The other half of editing a .nct / .gui: the decoded file (saved from the
// tree as .bin or .xml) goes back through the same XOR. When the original is
// open in the tool, the edit is compared with it, because only a changed byte
// on an offset the pad is unsure of can come out wrong - an unchanged one
// round-trips exactly whatever the pad says there.
static void encodeEditedFile(HWND hwnd) {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Decoded files (*.bin;*.xml)\0*.bin;*.xml\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"The edited, decoded file to encode";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;
    std::string in = narrow(std::wstring(file));
    Bytes plain;
    if (!nfsnl::readFile(in, plain) || plain.empty()) {
        MessageBoxW(hwnd, L"Could not read that file.", L"Encode", MB_ICONERROR);
        return;
    }
    bool isXml = nfsnl::extensionOf(in) == "xml" ||
                 (plain.size() >= 5 && !memcmp(plain.data(), "<?xml", 5));
    std::string target = nfsnl::stripExtension(in) + (isXml ? ".gui" : ".nct");

    // the original, when the same name is open in the tool
    std::string leaf = nfsnl::baseName(target);
    Bytes originalPlain;
    for (const Entry& e : g_all) {
        if (nfsnl::baseName(e.path) != leaf) continue;
        Bytes cipher;
        if (readAsset(e, cipher)) {
            size_t cv = 0;
            nfsnl::nctTransform(cipher.data(), cipher.size(), originalPlain, &cv);
        }
        break;
    }

    Bytes out;
    size_t uncertain = 0, pastPad = 0;
    nfsnl::nctEncode(plain.data(), plain.size(), out, &uncertain, &pastPad);
    size_t risky = 0;
    if (!originalPlain.empty()) {
        for (size_t i = 0; i < plain.size(); ++i) {
            bool changed = i >= originalPlain.size() || plain[i] != originalPlain[i];
            if (changed && !nfsnl::nctPadByteKnown(i)) ++risky;
        }
    }

    std::wstring name = widen(nfsnl::baseName(target));
    name.resize(MAX_PATH, 0);
    OPENFILENAMEW sfn{};
    sfn.lStructSize = sizeof(sfn);
    sfn.hwndOwner = hwnd;
    sfn.lpstrFilter = isXml ? L"Real Racing 3 menu (*.gui)\0*.gui\0"
                            : L"Real Racing 3 data (*.nct)\0*.nct\0";
    sfn.lpstrFile = &name[0];
    sfn.nMaxFile = MAX_PATH;
    sfn.lpstrTitle = L"Save the encoded file as";
    sfn.lpstrDefExt = isXml ? L"gui" : L"nct";
    sfn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&sfn)) return;
    std::string where = narrow(std::wstring(&name[0]));
    if (!nfsnl::writeFile(where, out)) {
        MessageBoxW(hwnd, L"Could not write the file.", L"Encode", MB_ICONERROR);
        return;
    }
    char msg[900];
    int n = snprintf(msg, sizeof(msg), "Encoded %u bytes to\n%s\n\n",
                     (unsigned)out.size(), where.c_str());
    if (pastPad)
        n += snprintf(msg + n, sizeof(msg) - n,
                      "%u byte(s) at the end are past the pad and were copied unchanged - "
                      "the game will not read them as written. Run File > Finish the .nct "
                      "pad on the whole game to extend it.\n\n", (unsigned)pastPad);
    if (!originalPlain.empty())
        n += snprintf(msg + n, sizeof(msg) - n, risky
            ? "Compared with the original: %u changed byte(s) sit on offsets the pad is "
              "not certain of, and may come out wrong in the game.\n"
            : "Compared with the original: every changed byte sits on an offset the pad "
              "is certain of.%.0u\n", (unsigned)risky);
    else
        n += snprintf(msg + n, sizeof(msg) - n,
                      "The original is not open here, so the edit could not be checked; "
                      "%u of the file's offsets are not certain in the pad.\n",
                      (unsigned)uncertain);
    MessageBoxW(hwnd, widen(msg).c_str(), L"Encode",
                (risky || pastPad) ? MB_ICONWARNING : MB_ICONINFORMATION);
    logLine("nct encode: %s -> %s, %u uncertain, %u risky, %u past pad", in.c_str(),
            where.c_str(), (unsigned)uncertain, (unsigned)risky, (unsigned)pastPad);
}

static HMENU buildMenu() {
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, ID_FILE_OPEN, L"&Open game files...\tCtrl+O");
    AppendMenuW(file, MF_STRING, ID_FILE_GAME, L"Change &game...");
    AppendMenuW(file, MF_STRING, ID_FILE_CONVERT, L"&Convert all shown to a folder...");
    AppendMenuW(file, MF_STRING, ID_FILE_EXPORT_SELECTED, L"&Export selected to a folder (PNG, FBX, WAV)...");
    AppendMenuW(file, MF_STRING, ID_FILE_EXTRACT_SELECTED, L"E&xtract selected to a folder...");
    AppendMenuW(file, MF_STRING, ID_FILE_OPEN_PICTURE, L"Open a &picture (PNG, JPG)...");
    AppendMenuW(file, MF_STRING, ID_FILE_SAVE_PENDING, L"&Save imported files\tCtrl+S");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, ID_FILE_VGMSTREAM, L"Locate &vgmstream-cli.exe...");
    AppendMenuW(file, MF_STRING, ID_FILE_LZHAM, L"Use another &LZHAM decoder DLL (optional)...");
    AppendMenuW(file, MF_STRING, ID_FILE_NCT,
                L"Finish the .&nct pad from this game...");
    AppendMenuW(file, MF_STRING, ID_FILE_NCT_ENCODE,
                L"&Encode an edited file back to .nct / .gui...");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, ID_FILE_EXIT, L"E&xit");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, ID_VIEW_ALL, L"&All assets");
    AppendMenuW(view, MF_STRING, ID_VIEW_TEX, L"&Textures (.sba)");
    AppendMenuW(view, MF_STRING, ID_VIEW_MODEL, L"&3D models (.sb3d)");
    AppendMenuW(view, MF_STRING, ID_VIEW_SOUND, L"&Sounds");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, ID_THEME_LIGHT, L"&Light theme");
    AppendMenuW(view, MF_STRING, ID_THEME_DARK, L"&Dark theme");

    HMENU tools = CreatePopupMenu();
    AppendMenuW(tools, MF_STRING, ID_TOOLS_SAVE_EDITOR, L"&Save Editor...\tCtrl+E");
    AppendMenuW(tools, MF_STRING, ID_TOOLS_IMPORT,
                L"&Import PNG/JPG into this texture...\tCtrl+I");
    AppendMenuW(tools, MF_STRING, ID_TOOLS_SHOW_SOURCE,
                L"Show the &file this asset is in\tCtrl+L");
    AppendMenuW(tools, MF_STRING, ID_FILE_NCT_ENCODE,
                L"&Encode an edited file back to .nct / .gui...");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, ID_HELP_ABOUT, L"&About Monkey Tool");

    HMENU bar = CreateMenu();
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&File");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"&View");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)tools, L"&Tools");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Help");
    return bar;
}

// The icon comes from the compiled-in resource. If it is not there - the
// build script carries on without it when windres is missing - monkeytool.ico
// sitting next to the exe is used instead, so the title bar is never blank.
static HICON loadAppIcon(HINSTANCE inst, int cx, int cy) {
    UINT flags = LR_SHARED | (cx == 0 ? LR_DEFAULTSIZE : 0u);
    HICON h = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                cx, cy, flags);
    if (h) return h;

    wchar_t path[MAX_PATH + 32];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return nullptr;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return nullptr;
    wcscpy(slash + 1, L"monkeytool.ico");
    return (HICON)LoadImageW(nullptr, path, IMAGE_ICON, cx, cy,
                             LR_LOADFROMFILE | (cx == 0 ? LR_DEFAULTSIZE : 0u));
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    g_inst = inst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES |
                                          ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);
    SetUnhandledExceptionFilter(crashHandler);
    std::set_terminate([] {
        logLine("terminate() called - an exception escaped");
        MessageBoxW(nullptr, L"Monkey Tool hit an internal error and has to close.\n\n"
                             L"See MonkeyTool.log next to the program.",
                    L"Monkey Tool", MB_ICONERROR);
        ExitProcess(3);
    });
    logLine("---- Monkey Tool 1.2.2 starting ----");
    findLzham();
    // a pad finished from a previous session's game files
    if (nfsnl::nctPadLoad(exeFolder() + "\\nct_pad.bin"))
        logLine("nct: pad loaded from nct_pad.bin, %u byte(s) known",
                (unsigned)nfsnl::nctPadKnownBytes());
    logLine("codecs: zstd %s, LZHAM %s",
            nfsnl::zstdAvailable() ? "yes" : "missing",
            nfsnl::lzhamAvailable() ? nfsnl::lzhamBackend().c_str() : "missing");
    loadThemePref();
    g_bgBrush = CreateSolidBrush(pal().windowBg);
    g_controlBrush = CreateSolidBrush(pal().controlBg);

    HICON appIcon = loadAppIcon(inst, 0, 0);
    HICON appIconSmall = loadAppIcon(inst, GetSystemMetrics(SM_CXSMICON),
                                     GetSystemMetrics(SM_CYSMICON));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;   // painted in WM_ERASEBKGND instead
    wc.hIcon = appIcon;
    wc.hIconSm = appIconSmall;
    wc.lpszClassName = L"MonkeyTool";
    RegisterClassExW(&wc);

    g_main = CreateWindowExW(0, wc.lpszClassName, L"Monkey Tool 1.2.2",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                             1100, 700, nullptr, buildMenu(), inst, nullptr);
    // also set them on the window itself - a class icon alone is not always
    // picked up for the title bar and the taskbar button
    if (appIcon) SendMessageW(g_main, WM_SETICON, ICON_BIG, (LPARAM)appIcon);
    if (appIconSmall) SendMessageW(g_main, WM_SETICON, ICON_SMALL, (LPARAM)appIconSmall);

    applyTheme();
    // full window on start, like Frosty; the restore button makes it smaller
    (void)show;
    ShowWindow(g_main, SW_SHOWMAXIMIZED);
    UpdateWindow(g_main);

    // Start the way Frosty does: choose the game, then its files. This is
    // posted rather than called, so the picker opens from inside the message
    // loop below.
    PostMessageW(g_main, WM_APP_STARTUP, 0, 0);

    MSG msg;
    // the menu's shortcuts, wherever the focus is
    ACCEL keys[] = {
        { FVIRTKEY | FCONTROL, 'O', ID_FILE_OPEN },
        { FVIRTKEY | FCONTROL, 'E', ID_TOOLS_SAVE_EDITOR },
        { FVIRTKEY | FCONTROL, 'L', ID_TOOLS_SHOW_SOURCE },
        { FVIRTKEY | FCONTROL, 'I', ID_TOOLS_IMPORT },
        { FVIRTKEY | FCONTROL, 'S', ID_FILE_SAVE_PENDING },
    };
    HACCEL accel = CreateAcceleratorTableW(keys, (int)(sizeof(keys) / sizeof(keys[0])));
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (accel && g_main && TranslateAcceleratorW(g_main, accel, &msg)) continue;
        if (saveEditorDialogMessage(&msg)) continue;   // Tab between its fields
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (accel) DestroyAcceleratorTable(accel);
    CoUninitialize();
    return 0;
}

// ---- Real Racing 3 modding: model and data import ----

// New bytes for the asset `e`, saved where the user says: over the loose file
// itself (the original kept once as .bak) or anywhere else. A Real Racing 3
// .z goes back into its wrapper. False when cancelled or not written.
static bool saveOverAsset(HWND hwnd, const Entry& e, Bytes bytes, const wchar_t* title,
                          std::string& where, std::string& backup) {
    const nfsnl::LibraryEntry& le = g_lib.entries[e.lib];
    std::string diskFile;
    if (le.whole && le.archive >= 0 && (size_t)le.archive < g_lib.archives.size())
        diskFile = g_lib.archives[le.archive].path;
    if (le.rr3Wrapped && nfsnl::extensionOf(diskFile) == "z") bytes = nfsnl::rr3WrapZ(bytes);
    std::string leaf = diskFile.empty() ? nfsnl::baseName(e.path) : nfsnl::baseName(diskFile);
    std::wstring name = widen(diskFile.empty() ? leaf : diskFile);
    name.resize(MAX_PATH, 0);
    std::wstring ext = widen(nfsnl::extensionOf(leaf));
    std::wstring filt = L"Game file (*." + ext + L")";
    filt.push_back(0);
    filt += L"*." + ext;
    filt.push_back(0);
    filt += L"All files";
    filt.push_back(0);
    filt += L"*.*";
    filt.push_back(0);
    OPENFILENAMEW sfn{};
    sfn.lStructSize = sizeof(sfn);
    sfn.hwndOwner = hwnd;
    sfn.lpstrFilter = filt.c_str();
    sfn.lpstrFile = &name[0];
    sfn.nMaxFile = MAX_PATH;
    sfn.lpstrTitle = title;
    sfn.lpstrDefExt = ext.c_str();
    sfn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&sfn)) return false;
    where = narrow(std::wstring(name.c_str()));
    backup.clear();
    if (!diskFile.empty() && _wcsicmp(widen(where).c_str(), widen(diskFile).c_str()) == 0) {
        backup = diskFile + ".bak";
        if (GetFileAttributesW(widen(backup).c_str()) == INVALID_FILE_ATTRIBUTES)
            CopyFileW(widen(diskFile).c_str(), widen(backup).c_str(), TRUE);
    }
    if (!nfsnl::writeFile(where, bytes)) {
        MessageBoxW(hwnd, (L"Could not write\n" + widen(where)).c_str(), title, MB_ICONERROR);
        return false;
    }
    return true;
}

// The file on disk an asset lives in, when it is a file of its own (loose
// Real Racing / Most Wanted files); empty for an asset inside a .pack or .obb.
static std::string diskFileOf(const Entry& e) {
    const nfsnl::LibraryEntry& le = g_lib.entries[e.lib];
    if (le.whole && le.archive >= 0 && (size_t)le.archive < g_lib.archives.size())
        return g_lib.archives[le.archive].path;
    return std::string();
}

static std::string pendingTarget(const Entry& e) {
    std::string f = diskFileOf(e);
    return f.empty() ? "a file you choose (this asset is inside a game archive)"
                     : f + "\n(the original is kept once as .bak)";
}

static void updateToolbarState() {
    if (g_tbSave) EnableWindow(g_tbSave, g_pending.empty() ? FALSE : TRUE);
    bool rr3 = g_profile && g_profile->id == "real_racing_3";
    if (g_tbNewCar) EnableWindow(g_tbNewCar, rr3 && !g_busy ? TRUE : FALSE);
    if (g_main) {
        std::wstring caption = L"Monkey Tool 1.2.2";
        if (g_profile) caption += L" - " + widen(g_profile->name);
        if (!g_pending.empty()) caption += L"  *";
        SetWindowTextW(g_main, caption.c_str());
    }
}

static void addPending(const Entry& e, Bytes bytes, const std::string& what) {
    auto it = std::find_if(g_all.begin(), g_all.end(), [&](const Entry& x) { return x.path == e.path; });
    PendingSave p;
    p.entry = it == g_all.end() ? 0 : (size_t)(it - g_all.begin());
    p.bytes = std::move(bytes);
    p.what = what;
    g_pending[e.path] = std::move(p);
    char b[400];
    snprintf(b, sizeof(b), "%s: %s imported - not saved yet. Click Save (Ctrl+S) to write it to %s.",
             nfsnl::baseName(e.path).c_str(), what.c_str(),
             diskFileOf(e).empty() ? "a file you choose" : diskFileOf(e).c_str());
    setStatus(b);
    updateToolbarState();
}

// Save: every pending import written into its own file (the original kept
// once as .bak), or - for an asset inside an archive - where the user says.
static bool savePending(HWND hwnd) {
    if (g_pending.empty()) {
        MessageBoxW(hwnd, L"Nothing to save: import a picture into a texture, or a model into a car, first.",
                    L"Save", MB_ICONINFORMATION);
        return true;
    }
    std::string done, failed;
    std::vector<std::string> saved;
    std::vector<std::string> keys;
    for (const auto& kv : g_pending) keys.push_back(kv.first);
    for (const std::string& key : keys) {
        const PendingSave p = g_pending[key];      // a copy: the map changes below
        if (p.entry >= g_all.size() || g_all[p.entry].path != key) { failed += key + " (the game was reopened)\n"; continue; }
        const Entry& e = g_all[p.entry];
        const nfsnl::LibraryEntry& le = g_lib.entries[e.lib];
        std::string disk = diskFileOf(e);
        Bytes bytes = p.bytes;
        if (le.rr3Wrapped && nfsnl::extensionOf(disk) == "z") bytes = nfsnl::rr3WrapZ(bytes);
        std::string where, backup;
        if (!disk.empty()) {
            where = disk;
            backup = disk + ".bak";
            if (GetFileAttributesW(widen(backup).c_str()) == INVALID_FILE_ATTRIBUTES)
                CopyFileW(widen(disk).c_str(), widen(backup).c_str(), TRUE);
            else backup.clear();
            if (!nfsnl::writeFile(where, bytes)) { failed += where + "\n"; continue; }
        } else {
            // inside a .pack / .obb: saved on its own, where the user says
            std::wstring title = L"Save " + widen(nfsnl::baseName(e.path)) + L" as";
            if (!saveOverAsset(hwnd, e, p.bytes, title.c_str(), where, backup)) { failed += e.path + " (cancelled)\n"; continue; }
            saved.push_back(key);
            done += nfsnl::baseName(e.path) + "  ->  " + where + "\n";
            continue;
        }
        saved.push_back(key);
        done += nfsnl::baseName(e.path) + "  ->  " + where + (backup.empty() ? "" : "   (original: .bak)") + "\n";
        logLine("save: %s -> %s", e.path.c_str(), where.c_str());
    }
    for (const std::string& k : saved) g_pending.erase(k);
    updateToolbarState();
    std::string msg;
    if (!done.empty()) msg += "Saved:\n\n" + done;
    if (!failed.empty()) msg += "\nNot saved:\n\n" + failed;
    if (!g_pending.empty()) msg += "\n" + std::to_string(g_pending.size()) + " file(s) still waiting - click Save again.";
    setStatus(done.empty() ? "Nothing was saved." : "Saved " + std::to_string(saved.size()) + " file(s).");
    MessageBoxW(hwnd, widen(msg).c_str(), L"Save", failed.empty() ? MB_ICONINFORMATION : MB_ICONWARNING);
    return failed.empty();
}

// Names the car does not know, offered for fixing before a model goes in:
// "FOUND INCORRECT PART NAME: x - replace with y?". False when cancelled.
static bool askNameFixes(HWND hwnd, const Bytes& car, nfsnl::Model& model, bool newCar) {
    std::vector<nfsnl::NameFix> fixes = nfsnl::rr3SuggestNames(car, model, newCar);
    if (fixes.empty()) return true;
    std::wstring msg = L"Some names in your model are not the ones this car uses. The game finds a "
                       L"car's parts and materials by name, and a name it does not know can make it "
                       L"leave the part out or crash.\n\n";
    int shown = 0;
    for (const nfsnl::NameFix& f : fixes) {
        if (++shown > 30) { msg += L"... and " + std::to_wstring(fixes.size() - 30) + L" more\n"; break; }
        msg += (f.material ? L"FOUND INCORRECT MATERIAL NAME: " : L"FOUND INCORRECT PART NAME: ") +
               widen(f.from) + L"\n      correct name:  " + widen(f.to) + L"\n";
    }
    msg += L"\nDo you want to fix these and use the correct names?\n\n"
           L"Yes - use the correct names (recommended)\nNo - keep your names\nCancel - stop";
    int a = MessageBoxW(hwnd, msg.c_str(), L"Incorrect names", MB_YESNOCANCEL | MB_ICONQUESTION);
    if (a == IDCANCEL) return false;
    if (a == IDYES) nfsnl::rr3ApplyNameFixes(model, fixes);
    logLine("names: %u suggestion(s), %s", (unsigned)fixes.size(), a == IDYES ? "fixed" : "kept");
    return true;
}

// New car: a whole Real Racing 3 car .m3g built from an OBJ / FBX, on the
// frame of the selected car (its header, materials and mesh layout). The
// result replaces the selected .m3g when Save is clicked.
static void newCarFromModel(HWND hwnd) {
    if (!g_profile || g_profile->id != "real_racing_3") {
        MessageBoxW(hwnd, L"New car works on Real Racing 3 - open Real Racing 3 first.", L"New car", MB_ICONINFORMATION);
        return;
    }
    HTREEITEM sel = TreeView_GetSelection(g_tree);
    auto it = g_itemToEntry.find(sel);
    if (it == g_itemToEntry.end() || it->second < 0 || (size_t)it->second >= g_all.size() ||
        nfsnl::extensionOf(g_all[it->second].path) != "m3g") {
        MessageBoxW(hwnd, L"Select the car model to turn into your car first - a Real Racing 3 car's "
                          L"<car>_a.m3g (vehicles/<car>/<car>_a.m3g).\n\nThe new model takes that "
                          L"car's place in the game: its wheels, sounds and data stay.",
                    L"New car", MB_ICONINFORMATION);
        return;
    }
    const Entry e = g_all[it->second];
    Bytes tmpl;
    std::string err;
    if (!readAsset(e, tmpl, &err)) {
        MessageBoxW(hwnd, (L"Could not read the car.\n\n" + widen(err)).c_str(), L"New car", MB_ICONERROR);
        return;
    }
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"3D model (*.obj;*.fbx)\0*.obj;*.fbx\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Your car model (OBJ or FBX) - it becomes a Real Racing 3 .m3g";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;
    std::string path = narrow(std::wstring(file));
    Bytes src;
    if (!nfsnl::readFile(path, src)) {
        MessageBoxW(hwnd, (L"Could not read\n" + widen(path)).c_str(), L"New car", MB_ICONERROR);
        return;
    }
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    nfsnl::Model model = nfsnl::readImportModel(src.data(), src.size(), path);
    SetCursor(old);
    if (model.valid && !askNameFixes(hwnd, tmpl, model, true)) return;
    old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    Bytes result;
    std::string report;
    bool ok = model.valid && nfsnl::rr3BuildCar(tmpl, model, result, report);
    SetCursor(old);
    if (!ok) {
        if (!model.valid) report = model.warnings.empty() ? "no meshes in the model" : model.warnings[0];
        MessageBoxW(hwnd, (L"The car could not be built:\n\n" + widen(report)).c_str(), L"New car", MB_ICONERROR);
        return;
    }
    addPending(e, result, "new car from " + nfsnl::baseName(path));
    logLine("new car: %s from %s pending (%s)", e.path.c_str(), path.c_str(), report.c_str());
    // the long per-mesh list is for the log; the message keeps the summary
    std::string summary = report.substr(0, report.find("\n\n"));
    size_t modelNote = report.find("\nModel:");
    if (modelNote != std::string::npos) summary += report.substr(modelNote);
    std::string msg = summary + "\n\nThe 3D view shows your car now. Name the parts LOD_A_..., LOD_B_... "
                      "for the detail levels, and give them the car's materials (usemtl) so the game "
                      "textures them.\n\nClick Save (the blue disk, Ctrl+S) to write it into\n" + pendingTarget(e);
    MessageBoxW(hwnd, widen(msg).c_str(), L"New car", MB_ICONINFORMATION);
    updatePreview(e);
}

// Import > a model into a Real Racing 3 .m3g: the OBJ / FBX's objects replace
// the meshes of the same name (LOD_A_BODY_mm_ext ...), and objects the car
// does not have yet are added, cloned from a mesh of the same material.
static void importModelIntoSelected(HWND hwnd, const Entry& e) {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"3D model (*.obj;*.fbx)\0*.obj;*.fbx\0Wavefront OBJ (*.obj)\0*.obj\0"
                      L"FBX, binary or ASCII (*.fbx)\0*.fbx\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"The model to put into this car (export it from here first to keep the mesh names)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;
    std::string path = narrow(std::wstring(file));
    Bytes src, m3g;
    std::string err;
    if (!nfsnl::readFile(path, src)) {
        MessageBoxW(hwnd, (L"Could not read\n" + widen(path)).c_str(), L"Import model", MB_ICONERROR);
        return;
    }
    if (!readAsset(e, m3g, &err)) {
        MessageBoxW(hwnd, (L"Could not read the car model.\n\n" + widen(err)).c_str(), L"Import model", MB_ICONERROR);
        return;
    }
    nfsnl::Model imported = nfsnl::readImportModel(src.data(), src.size(), path);
    if (imported.valid && !askNameFixes(hwnd, m3g, imported, false)) return;
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    // where the viewer put each part, so a model exported from here (parts
    // on their .points) goes back where the file keeps them
    nfsnl::Model raw = nfsnl::loadModel(m3g.data(), m3g.size());
    nfsnl::Model placed = raw;
    Bytes points;
    readSidecarPoints(e.path, points);
    applySidecars(placed, e.path, points);
    Bytes result;
    std::string report;
    bool ok = imported.valid &&
              nfsnl::rr3ReplaceMeshes(m3g, imported, nfsnl::meshPlacements(raw, placed), result, report);
    SetCursor(old);
    if (!imported.valid) {
        report = imported.warnings.empty() ? "no meshes in the model" : imported.warnings[0];
        MessageBoxW(hwnd, (L"The model could not be read:\n\n" + widen(report)).c_str(), L"Import model", MB_ICONERROR);
        return;
    }
    if (!ok) {
        MessageBoxW(hwnd, widen(report).c_str(), L"Import model", MB_ICONERROR);
        return;
    }
    addPending(e, result, "model from " + nfsnl::baseName(path));
    logLine("import model: %s + %s pending (%s)", e.path.c_str(), path.c_str(), report.c_str());
    std::string msg = report + "\n\nThe 3D view now shows the new model. Click Save (the blue disk, "
                               "Ctrl+S) to write it into\n" + pendingTarget(e);
    MessageBoxW(hwnd, widen(msg).c_str(), L"Import model", MB_ICONINFORMATION);
    updatePreview(e);
}

// ---- the data editor: a data file as text, saved back in the game's format ----
static HWND g_dataWnd = nullptr, g_dataEdit = nullptr;
static HWND g_dataButtons[4] = {};
static Entry g_dataEntry;
static Bytes g_dataOriginal;
enum { IDC_DATA_SAVE = 4801, IDC_DATA_OPEN, IDC_DATA_EXPORT, IDC_DATA_CLOSE };

static std::string dataEditorText() {
    int n = GetWindowTextLengthW(g_dataEdit);
    std::wstring w((size_t)n + 1, 0);
    GetWindowTextW(g_dataEdit, &w[0], n + 1);
    w.resize((size_t)n);
    std::string t = narrow(w), o;
    o.reserve(t.size());
    for (char c : t) if (c != '\r') o += c;
    return o;
}

static void dataEditorSetText(const std::string& text) {
    std::string crlf;
    crlf.reserve(text.size() + text.size() / 16);
    for (char c : text) { if (c == '\n') crlf += '\r'; if (c != '\r') crlf += c; }
    SetWindowTextW(g_dataEdit, widen(crlf).c_str());
}

static LRESULT CALLBACK DataEditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE: {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(g_dataEdit, 8, 8, w - 16, h - 56, TRUE);
        int x = 8;
        for (int k = 0; k < 3; ++k) {
            MoveWindow(g_dataButtons[k], x, h - 40, 170, 30, TRUE);
            x += 178;
        }
        MoveWindow(g_dataButtons[3], w - 108, h - 40, 100, 30, TRUE);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_DATA_SAVE: {
            std::string why;
            Bytes out;
            if (!nfsnl::dataFromEditedText(g_dataEntry.path, dataEditorText(), g_dataOriginal.data(),
                                           g_dataOriginal.size(), out, why)) {
                MessageBoxW(hwnd, (L"The text does not encode:\n\n" + widen(why)).c_str(), L"Data editor", MB_ICONERROR);
                return 0;
            }
            std::string where, backup;
            if (!saveOverAsset(hwnd, g_dataEntry, out, L"Save the data file as", where, backup)) return 0;
            std::string m = "Saved as\n" + where;
            if (out == g_dataOriginal) m += "\n\n(no changes - the file is the same as the original)";
            if (!backup.empty()) m += "\n\nThe original is kept as\n" + backup;
            MessageBoxW(hwnd, widen(m).c_str(), L"Data editor", MB_ICONINFORMATION);
            g_dataOriginal = out;
            updatePreview(g_dataEntry);
            return 0;
        }
        case IDC_DATA_OPEN: {
            wchar_t file[MAX_PATH] = L"";
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"Text (*.txt;*.xml;*.json)\0*.txt;*.xml;*.json\0All files\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (!GetOpenFileNameW(&ofn)) return 0;
            Bytes t;
            if (nfsnl::readFile(narrow(std::wstring(file)), t)) dataEditorSetText(std::string(t.begin(), t.end()));
            return 0;
        }
        case IDC_DATA_EXPORT: {
            std::wstring name = widen(nfsnl::baseName(g_dataEntry.path) + ".txt");
            name.resize(MAX_PATH, 0);
            OPENFILENAMEW sfn{};
            sfn.lStructSize = sizeof(sfn);
            sfn.hwndOwner = hwnd;
            sfn.lpstrFilter = L"Text (*.txt)\0*.txt\0All files\0*.*\0";
            sfn.lpstrFile = &name[0];
            sfn.nMaxFile = MAX_PATH;
            sfn.lpstrDefExt = L"txt";
            sfn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (!GetSaveFileNameW(&sfn)) return 0;
            std::string t = dataEditorText();
            nfsnl::writeFile(narrow(std::wstring(name.c_str())), Bytes(t.begin(), t.end()));
            return 0;
        }
        case IDC_DATA_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_dataWnd = nullptr;
        g_dataEdit = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Import > a data file: opened in the editor as text; Save puts it back in the
// game's own format.
static void importDataIntoSelected(HWND hwnd, const Entry& e) {
    Bytes data;
    std::string err;
    if (!readAsset(e, data, &err)) {
        MessageBoxW(hwnd, (L"Could not read the file.\n\n" + widen(err)).c_str(), L"Data editor", MB_ICONERROR);
        return;
    }
    std::string text, why;
    if (!nfsnl::dataEditableText(e.path, data.data(), data.size(), text, why)) {
        MessageBoxW(hwnd, (widen(nfsnl::baseName(e.path)) + L": " + widen(why)).c_str(), L"Data editor",
                    MB_ICONINFORMATION);
        return;
    }
    static bool registered = false;
    HINSTANCE inst = g_inst;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DataEditorProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MonkeyToolDataEditor";
        RegisterClassExW(&wc);
        registered = true;
    }
    if (g_dataWnd) DestroyWindow(g_dataWnd);
    g_dataEntry = e;
    g_dataOriginal = data;
    std::wstring title = L"Data editor - " + widen(nfsnl::baseName(e.path));
    g_dataWnd = CreateWindowExW(0, L"MonkeyToolDataEditor", title.c_str(),
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 900, 700,
                                hwnd, nullptr, inst, nullptr);
    if (!g_dataWnd) return;
    g_dataEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                                     ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN | ES_NOHIDESEL,
                                 0, 0, 10, 10, g_dataWnd, nullptr, inst, nullptr);
    SendMessageW(g_dataEdit, EM_SETLIMITTEXT, 0x7FFFFFFE, 0);
    static HFONT mono = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                    FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(g_dataEdit, WM_SETFONT, (WPARAM)mono, TRUE);
    struct { int id; const wchar_t* label; } buttons[] = {
        { IDC_DATA_SAVE, L"Save to game format..." },
        { IDC_DATA_OPEN, L"Load text file..." },
        { IDC_DATA_EXPORT, L"Save as .txt..." },
        { IDC_DATA_CLOSE, L"Close" },
    };
    int bi = 0;
    for (auto& b : buttons) {
        HWND bw = CreateWindowExW(0, L"BUTTON", b.label, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  0, 0, 10, 10, g_dataWnd, (HMENU)(INT_PTR)b.id, inst, nullptr);
        SendMessageW(bw, WM_SETFONT, (WPARAM)g_headerFont, TRUE);
        g_dataButtons[bi++] = bw;
    }
    dataEditorSetText(text);
    RECT rc;
    GetClientRect(g_dataWnd, &rc);
    SendMessageW(g_dataWnd, WM_SIZE, 0, MAKELPARAM(rc.right, rc.bottom));
}
