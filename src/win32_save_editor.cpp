// win32_save_editor.cpp - the Save Editor window
//
// The Firemonkeys Save Editor, built into Monkey Tool: the same window, the
// same quick fields and the same "all fields" list as the separate program
// had, drawn by the tool itself and running in its process. The reading and
// writing of the saves is in nfsnl_save.cpp; this file is only the window.
#ifdef _WIN32
#include "win32_save_editor.h"
#include "nfsnl_save.h"

#include <commctrl.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace nfsnl;
using namespace nfsnl::saves;

namespace {

// ------------------------------------------------------------------ helpers
std::wstring W(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
std::string N(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
std::string getText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring w(n + 1, 0);
    GetWindowTextW(h, &w[0], n + 1);
    w.resize(n);
    return N(w);
}
void setText(HWND h, const std::string& s) { SetWindowTextW(h, W(s).c_str()); }
std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}
std::string lowerStr(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

enum {
    ID_OPEN = 100, ID_CALIB, ID_FILTER, ID_LIST, ID_APPLY, ID_SAVE, ID_PICTURE, ID_NAME,
    ID_QUICK0 = 200,
};
const int kMaxQuick = 14;

// ------------------------------------------------------------------ state
struct Editor {
    HWND hwnd = nullptr;
    HINSTANCE inst = nullptr;
    HFONT font = nullptr;
    double scale = 1.0;
    SaveEditorTheme theme;
    HBRUSH bgBrush = nullptr, ctlBrush = nullptr;

    HWND edFolder = nullptr, btnOpen = nullptr, lblGame = nullptr;
    HWND quickLbl[kMaxQuick] = {}, quickEd[kMaxQuick] = {};
    HWND btnCalib = nullptr, lblHint = nullptr;
    HWND edFilter = nullptr, list = nullptr, edVal = nullptr;
    HWND btnApply = nullptr, btnSave = nullptr, lblStat = nullptr;
    HWND buttons[5] = {};
    HWND btnPicture = nullptr;
    HWND lblName = nullptr, edName = nullptr;
    SaveEditorAvatarHook avatarHook;

    std::string game;              // "rr3" | "nfs" | ""
    NfsSave nfs;
    Rr3Save rr;
    std::map<std::string, uint64_t> keys;
    // a field is (true, index into rr.root) or (false, index into nfs.leaves)
    std::vector<std::pair<bool, size_t>> all, shown;
    bool dirty = false;
};
Editor E;

std::string keysPath() {
    wchar_t buf[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::string base = (n && n < MAX_PATH) ? N(buf) : std::string(".");
    return base + "\\MonkeyTool\\rr3_keys.txt";
}
// The separate editor kept its calibration here; it is read once so nobody
// has to calibrate again after switching to the built-in one.
std::string oldKeysPath() {
    wchar_t buf[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::string base = (n && n < MAX_PATH) ? N(buf) : std::string(".");
    return base + "\\FiremonkeysSaveEditor\\rr3_keys.txt";
}

int px(int v) { return (int)(v * E.scale + 0.5); }

HWND ctl(DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style,
         int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(ex, cls, text, style | WS_CHILD | WS_VISIBLE,
                             px(x), px(y), px(w), px(h), E.hwnd, (HMENU)(INT_PTR)id,
                             E.inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)E.font, TRUE);
    return c;
}

void status(const std::string& s) { setText(E.lblStat, s); }

std::string fieldName(const std::pair<bool, size_t>& f) {
    return f.first ? E.rr.root[f.second].name : E.nfs.leaves[f.second].path;
}
std::string fieldValue(const std::pair<bool, size_t>& f) {
    return f.first ? E.rr.root[f.second].valueString() : E.nfs.get(E.nfs.leaves[f.second]);
}
bool fieldEditable(const std::pair<bool, size_t>& f) {
    return f.first ? E.rr.root[f.second].editable() : E.nfs.editable(E.nfs.leaves[f.second]);
}
bool fieldSet(const std::pair<bool, size_t>& f, const std::string& v, std::string& err) {
    if (f.first) return E.rr.root[f.second].setFromString(v, err);
    return E.nfs.set(E.nfs.leaves[f.second], v, err);
}

// ------------------------------------------------------------------ theme
// The two frames ("Quick edit", "All fields") are painted here rather than
// being group-box controls: with visual styles on, a group box fills itself
// white whatever colour the window asks for, which is what left white slabs
// in the dark theme.
struct Frame { int x, y, w, h; const wchar_t* caption; };
const Frame kFrames[2] = {
    { 12, 70, 696, 262, L"Quick edit" },
    { 12, 340, 696, 290, L"All fields (advanced)" },
};

COLORREF mix(COLORREF a, COLORREF b, int pct) {
    return RGB((GetRValue(a) * (100 - pct) + GetRValue(b) * pct) / 100,
               (GetGValue(a) * (100 - pct) + GetGValue(b) * pct) / 100,
               (GetBValue(a) * (100 - pct) + GetBValue(b) * pct) / 100);
}

void paintFrames(HDC dc) {
    HPEN pen = CreatePen(PS_SOLID, 1, mix(E.theme.windowBg, E.theme.text, 30));
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    HGDIOBJ of = SelectObject(dc, E.font);
    SetBkMode(dc, OPAQUE);
    SetBkColor(dc, E.theme.windowBg);
    SetTextColor(dc, E.theme.text);
    for (const Frame& f : kFrames) {
        int top = px(f.y + 8);
        RoundRect(dc, px(f.x), top, px(f.x + f.w), px(f.y + f.h), px(6), px(6));
        RECT tr = { px(f.x + 10), px(f.y), px(f.x + f.w - 10), px(f.y + 17) };
        DrawTextW(dc, (std::wstring(L" ") + f.caption + L" ").c_str(), -1, &tr,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(dc, of);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
}

// Buttons are drawn here too, so that they follow the theme: a stock push
// button stays light grey in the dark theme on most Windows builds.
void drawButton(const DRAWITEMSTRUCT* d) {
    bool down = (d->itemState & ODS_SELECTED) != 0;
    bool off = (d->itemState & ODS_DISABLED) != 0;
    bool focus = (d->itemState & ODS_FOCUS) != 0;
    COLORREF face = E.theme.dark ? mix(E.theme.windowBg, E.theme.text, down ? 22 : 12)
                                 : (down ? RGB(0xCC, 0xE0, 0xFF) : RGB(0xFD, 0xFD, 0xFD));
    COLORREF edge = focus ? RGB(0x3B, 0x82, 0xF6) : mix(E.theme.windowBg, E.theme.text, off ? 18 : 35);
    HBRUSH b = CreateSolidBrush(face);
    HPEN p = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ ob = SelectObject(d->hDC, b), op = SelectObject(d->hDC, p);
    RECT r = d->rcItem;
    FillRect(d->hDC, &r, E.bgBrush);
    RoundRect(d->hDC, r.left, r.top, r.right, r.bottom, px(5), px(5));
    SelectObject(d->hDC, ob);
    SelectObject(d->hDC, op);
    DeleteObject(b);
    DeleteObject(p);
    wchar_t text[128] = {0};
    GetWindowTextW(d->hwndItem, text, 127);
    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC, off ? mix(E.theme.windowBg, E.theme.text, 45) : E.theme.text);
    HGDIOBJ of = SelectObject(d->hDC, E.font);
    if (down) OffsetRect(&r, 1, 1);
    DrawTextW(d->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(d->hDC, of);
}

// The dark title bar Windows 10 20H1 and later draw on request (attribute 20;
// 19 on the builds before). dwmapi is loaded here so nothing else needs it.
void darkTitleBar(HWND h, bool dark) {
    static HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm || !h) return;
    typedef HRESULT (WINAPI *SetAttr)(HWND, DWORD, LPCVOID, DWORD);
    SetAttr set = (SetAttr)(void*)GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (!set) return;
    BOOL on = dark ? TRUE : FALSE;
    if (set(h, 20, &on, sizeof(on)) != 0) set(h, 19, &on, sizeof(on));
}

void applyControlTheme() {
    const wchar_t* t = E.theme.dark ? L"DarkMode_Explorer" : L"Explorer";
    if (E.list) SetWindowTheme(E.list, t, nullptr);           // its scroll bar
    for (HWND e : { E.edFolder, E.edFilter, E.edVal })
        if (e) SetWindowTheme(e, E.theme.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
    for (HWND q : E.quickEd) if (q) SetWindowTheme(q, E.theme.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
    darkTitleBar(E.hwnd, E.theme.dark);
}

// ------------------------------------------------------------------ building
void setLoaded(bool on) {
    for (HWND h : { E.edFilter, E.list, E.edVal, E.btnApply, E.btnSave, E.btnPicture, E.edName }) EnableWindow(h, on);
    if (!on) {
        setText(E.edName, "");
        for (int i = 0; i < kMaxQuick; ++i) {
            setText(E.quickLbl[i], "");
            setText(E.quickEd[i], "");
            EnableWindow(E.quickEd[i], FALSE);
        }
        ShowWindow(E.btnCalib, SW_HIDE);
    }
}

void build() {
    ctl(0, L"STATIC", L"Save folder:", SS_LEFT | SS_CENTERIMAGE, 12, 14, 80, 24, 0);
    E.edFolder = ctl(WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY, 94, 14, 470, 24, 0);
    E.btnOpen = ctl(0, L"BUTTON", L"Open folder...", BS_OWNERDRAW | WS_TABSTOP, 572, 13, 136, 26, ID_OPEN);
    E.lblGame = ctl(0, L"STATIC",
                    L"Click \"Open folder...\" (or drag a folder onto this window) and pick "
                    L"the folder with your save files.", SS_LEFT, 12, 46, 552, 20, 0);
    E.btnPicture = ctl(0, L"BUTTON", L"Profile picture...", BS_OWNERDRAW | WS_TABSTOP,
                       572, 42, 136, 26, ID_PICTURE);
    E.lblName = ctl(0, L"STATIC", L"Player name:", SS_LEFT | SS_CENTERIMAGE, 12, 68, 80, 22, 0);
    E.edName = ctl(WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 94, 68, 260, 22, ID_NAME);


    for (int i = 0; i < kMaxQuick; ++i) {
        int col = i % 2, row = i / 2;
        int x = 24 + col * 344, y = 94 + row * 28;
        E.quickLbl[i] = ctl(0, L"STATIC", L"", SS_RIGHT | SS_CENTERIMAGE, x, y, 150, 24, 0);
        E.quickEd[i] = ctl(WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP,
                           x + 158, y, 160, 24, ID_QUICK0 + i);
    }
    E.btnCalib = ctl(0, L"BUTTON", L"Calibrate from game", BS_OWNERDRAW | WS_TABSTOP,
                     24, 294, 170, 28, ID_CALIB);
    E.lblHint = ctl(0, L"STATIC", L"", SS_LEFT, 204, 292, 496, 36, 0);


    ctl(0, L"STATIC", L"Search:", SS_LEFT | SS_CENTERIMAGE, 24, 362, 56, 24, 0);
    E.edFilter = ctl(WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 82, 362, 614, 24, ID_FILTER);
    SendMessageW(E.edFilter, 0x1501 /* EM_SETCUEBANNER */, TRUE,
                 (LPARAM)L"type part of a field name, e.g. Cash, Level, Balance, xp");
    E.list = ctl(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                 LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP | LBS_USETABSTOPS,
                 24, 392, 672, 196, ID_LIST);
    ctl(0, L"STATIC", L"Value:", SS_LEFT | SS_CENTERIMAGE, 24, 596, 56, 24, 0);
    E.edVal = ctl(WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 82, 596, 470, 24, 0);
    E.btnApply = ctl(0, L"BUTTON", L"Apply", BS_OWNERDRAW | WS_TABSTOP, 560, 595, 136, 26, ID_APPLY);

    E.lblStat = ctl(0, L"STATIC", L"Backups are made automatically every time you save.",
                    SS_LEFT | SS_CENTERIMAGE, 12, 642, 548, 28, 0);
    E.btnSave = ctl(0, L"BUTTON", L"Save changes", BS_OWNERDRAW | WS_TABSTOP, 572, 640, 136, 32, ID_SAVE);
    E.buttons[0] = E.btnOpen; E.buttons[1] = E.btnCalib;
    E.buttons[2] = E.btnApply; E.buttons[3] = E.btnSave; E.buttons[4] = E.btnPicture;
    applyControlTheme();
    setLoaded(false);
}

// ------------------------------------------------------------------ lists
void refilter() {
    std::string f = lowerStr(trimmed(getText(E.edFilter)));
    E.shown.clear();
    for (const auto& x : E.all)
        if (f.empty() || lowerStr(fieldName(x)).find(f) != std::string::npos) E.shown.push_back(x);
    SendMessageW(E.list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(E.list, LB_RESETCONTENT, 0, 0);
    for (const auto& x : E.shown) {
        std::string v = fieldValue(x);
        if (v.size() > 60) v = v.substr(0, 57) + "...";
        std::string line = fieldName(x) + "  =  " + v + (fieldEditable(x) ? "" : "   (read-only)");
        SendMessageW(E.list, LB_ADDSTRING, 0, (LPARAM)W(line).c_str());
    }
    SendMessageW(E.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(E.list, nullptr, TRUE);
    setText(E.edVal, "");
}

const std::pair<bool, size_t>* selected() {
    LRESULT i = SendMessageW(E.list, LB_GETCURSEL, 0, 0);
    if (i < 0 || (size_t)i >= E.shown.size()) return nullptr;
    return &E.shown[(size_t)i];
}

void fillAll() {
    E.all.clear();
    if (E.game == "rr3") {
        for (size_t i : E.rr.sortedRoot()) {
            const Rr3Rec& r = E.rr.root[i];
            if (r.type() == 4 /* reference */ || r.name.empty()) continue;
            E.all.push_back({ true, i });
        }
    } else if (E.game == "nfs") {
        for (size_t i = 0; i < E.nfs.leaves.size(); ++i) E.all.push_back({ false, i });
    }
    refilter();
}

void fillQuick() {
    for (int i = 0; i < kMaxQuick; ++i) {
        setText(E.quickLbl[i], "");
        setText(E.quickEd[i], "");
        EnableWindow(E.quickEd[i], FALSE);
    }
    // the player's name, in either game
    {
        std::string name;
        bool have = false;
        if (E.game == "nfs") {
            long li = nfsPlayerNameLeaf(E.nfs);
            if (li >= 0) { name = E.nfs.get(E.nfs.leaves[(size_t)li]); have = true; }
        } else if (E.game == "rr3") {
            if (Rr3Rec* r = rr3PlayerName(E.rr)) { name = r->s; have = true; }
        }
        setText(E.edName, have ? name : std::string());
        EnableWindow(E.edName, have ? TRUE : FALSE);
        SendMessageW(E.edName, 0x1501 /* EM_SETCUEBANNER */, TRUE,
                     (LPARAM)(have ? L"" : L"this save keeps no name (the game takes it from your account)"));
    }
    if (E.game == "nfs") {
        ShowWindow(E.btnCalib, SW_HIDE);
        setText(E.lblHint, "");
        int i = 0;
        for (const NfsMain& m : nfsMainFields()) {
            const NfsLeaf* l = E.nfs.find(m.path);
            if (!l || i >= kMaxQuick) continue;
            setText(E.quickLbl[i], std::string(m.label) + ":");
            setText(E.quickEd[i], E.nfs.get(*l));
            EnableWindow(E.quickEd[i], TRUE);
            ++i;
        }
        return;
    }
    if (E.game != "rr3") return;
    ShowWindow(E.btnCalib, SW_SHOW);
    bool calibrated = false;
    const auto& hidden = rr3HiddenFields();
    for (size_t i = 0; i < hidden.size(); ++i) {
        setText(E.quickLbl[i], std::string(hidden[i].label) + ":");
        EnableWindow(E.quickEd[i], TRUE);
        uint64_t v;
        if (E.rr.hiddenGet(hidden[i], E.keys, v)) {
            setText(E.quickEd[i], std::to_string(v));
            calibrated = true;
        }
    }
    size_t n = hidden.size();
    for (const Rr3Plain& p : rr3PlainFields()) {
        Rr3Rec* r = E.rr.find(p.field);
        if (r && n < (size_t)kMaxQuick) {
            setText(E.quickLbl[n], std::string(p.label) + ":");
            setText(E.quickEd[n], r->valueString());
            EnableWindow(E.quickEd[n], TRUE);
            ++n;
        }
    }
    setText(E.lblHint, calibrated
        ? "RR3 hides money/level with a per-field key. Values shown use your saved "
          "calibration. If they don't match the game, type the in-game numbers and click "
          "Calibrate again."
        : "One-time step: type the R$, Gold, M$ and level the game shows NOW into the boxes "
          "above, then click \"Calibrate from game\". After that you can edit them.");
}

// ------------------------------------------------------------------ actions
void openFolder(const std::string& root, bool prompt = true) {
    if (prompt && E.dirty &&
        MessageBoxW(E.hwnd, L"You have unsaved changes. Open another folder anyway?",
                    L"Unsaved changes", MB_YESNO | MB_ICONWARNING) != IDYES)
        return;
    std::string game, dir;
    if (!findSave(root, game, dir)) {
        std::string msg = "No supported save found in:\n" + root +
            "\n\nPick the folder that contains:\n"
            "  - NFS No Limits: <number>_m.sb and <number>_0.sb ... (folder \"saves-encrypted\")\n"
            "  - Real Racing 3: character.2.dat (folder \"doc\")";
        MessageBoxW(E.hwnd, W(msg).c_str(), L"Nothing found", MB_ICONWARNING);
        return;
    }
    E.game.clear();
    E.all.clear();
    E.shown.clear();
    E.dirty = false;
    setLoaded(false);
    setText(E.edFolder, dir);
    std::string err;
    bool ok = game == "rr3" ? E.rr.load(dir + "\\character.2.dat", err) : E.nfs.load(dir, err);
    if (!ok) {
        setText(E.lblGame, "Could not load the save.");
        MessageBoxW(E.hwnd, W("Could not read the save:\n\n" + err).c_str(), L"Error", MB_ICONERROR);
        return;
    }
    E.game = game;
    fillAll();
    fillQuick();
    setLoaded(true);
    char buf[256];
    if (game == "rr3")
        snprintf(buf, sizeof(buf), "Real Racing 3  -  character.2.dat  (%u fields)", (unsigned)E.all.size());
    else
        snprintf(buf, sizeof(buf), "Need for Speed No Limits  -  player %s, %u section files  (%u fields)",
                 E.nfs.uid.c_str(), (unsigned)E.nfs.files.size(), (unsigned)E.all.size());
    setText(E.lblGame, buf);
    status("Loaded. Change values, then click \"Save changes\". Close the game first!");
}

bool applyQuick(std::string& err) {
    // the player's name
    {
        std::string t = trimmed(getText(E.edName));
        if (!t.empty() && E.game == "nfs") {
            long li = nfsPlayerNameLeaf(E.nfs);
            if (li >= 0) {
                NfsLeaf leaf = E.nfs.leaves[(size_t)li];
                if (t != E.nfs.get(leaf)) {
                    if (!E.nfs.setString(leaf, t, err)) { err = "Player name: " + err; return false; }
                    E.dirty = true;
                }
            }
        } else if (!t.empty() && E.game == "rr3") {
            Rr3Rec* r = rr3PlayerName(E.rr);
            if (r && t != r->s) {
                std::string e;
                if (!r->setFromString(t, e)) { err = "Player name: " + e; return false; }
                E.dirty = true;
            }
        }
    }
    if (E.game == "nfs") {
        int i = 0;
        for (const NfsMain& m : nfsMainFields()) {
            const NfsLeaf* l = E.nfs.find(m.path);
            if (!l || i >= kMaxQuick) continue;
            std::string t = trimmed(getText(E.quickEd[i]));
            ++i;
            if (t.empty() || t == E.nfs.get(*l)) continue;
            if (!E.nfs.setMain(m, t, err)) return false;
            E.dirty = true;
        }
        return true;
    }
    if (E.game != "rr3") return true;
    const auto& hidden = rr3HiddenFields();
    for (size_t i = 0; i < hidden.size(); ++i) {
        const Rr3Hidden& h = hidden[i];
        std::string t = trimmed(getText(E.quickEd[i]));
        uint64_t cur;
        bool ok = E.rr.hiddenGet(h, E.keys, cur);
        if (t.empty() || (ok && t == std::to_string(cur))) continue;
        if (!ok) {
            err = std::string(h.label) + " is not calibrated yet. Type the current in-game values "
                  "and click \"Calibrate from game\" first";
            return false;
        }
        uint64_t v;
        std::string e;
        if (!rr3ParseNum(t, v, e)) { err = std::string(h.label) + ": " + e; return false; }
        if (!strcmp(h.label, "Driver level") && (v < 1 || v > 999)) {
            err = "driver level must be between 1 and 999";
            return false;
        }
        if (!E.rr.hiddenSet(h, E.keys, v, err)) return false;
        E.dirty = true;
    }
    size_t n = hidden.size();
    for (const Rr3Plain& p : rr3PlainFields()) {
        Rr3Rec* r = E.rr.find(p.field);
        if (!r || n >= (size_t)kMaxQuick) continue;
        std::string t = trimmed(getText(E.quickEd[n]));
        ++n;
        if (t.empty() || t == r->valueString()) continue;
        std::string e;
        if (!r->setFromString(t, e)) { err = std::string(p.label) + ": " + e; return false; }
        E.dirty = true;
    }
    return true;
}

void calibrate() {
    if (E.game != "rr3") return;
    int got = 0;
    std::string errs;
    const auto& hidden = rr3HiddenFields();
    for (size_t i = 0; i < hidden.size(); ++i) {
        std::string t = trimmed(getText(E.quickEd[i]));
        if (t.empty()) continue;
        uint64_t v;
        std::string e;
        if (!rr3ParseNum(t, v, e)) { errs += std::string(hidden[i].label) + ": " + e + "\n"; continue; }
        if (!E.rr.calibrate(hidden[i], E.keys, v, e)) { errs += e + "\n"; continue; }
        ++got;
    }
    if (!got) {
        MessageBoxW(E.hwnd, W("Type the values the game shows right now (at least R$) into the "
                              "boxes, then click Calibrate.\n\n" + errs).c_str(),
                    L"Calibrate", MB_ICONINFORMATION);
        return;
    }
    // the three wallets share one key in the game: fill in any left empty
    static const char* wallets[3] = { "rdollars", "gold", "mdollars" };
    for (const char* name : wallets)
        if (E.keys.count(name)) {
            for (const char* o : wallets) if (!E.keys.count(o)) E.keys[o] = E.keys[name];
            break;
        }
    saveKeys(keysPath(), E.keys);
    fillQuick();
    std::string msg = "Calibration saved. It is reused next time, so you only do this once.\n\n"
                      "Check that every number now matches the game. Then type the new values "
                      "and click \"Save changes\".";
    if (!errs.empty()) msg += "\n\nProblems:\n" + errs;
    MessageBoxW(E.hwnd, W(msg).c_str(), L"Calibrate", MB_ICONINFORMATION);
}

void applyAdvanced() {
    std::string err;
    if (!applyQuick(err)) {      // keep what was typed in Quick edit
        MessageBoxW(E.hwnd, W(err).c_str(), L"Quick edit", MB_ICONWARNING);
        return;
    }
    const auto* f = selected();
    if (!f) {
        MessageBoxW(E.hwnd, L"Select a field in the list first.", L"Apply", MB_ICONINFORMATION);
        return;
    }
    auto field = *f;
    if (!fieldEditable(field)) {
        MessageBoxW(E.hwnd, L"This field is read-only in the editor.", L"Apply", MB_ICONINFORMATION);
        return;
    }
    if (!fieldSet(field, getText(E.edVal), err)) {
        MessageBoxW(E.hwnd, W(err).c_str(), L"Invalid value", MB_ICONWARNING);
        return;
    }
    E.dirty = true;
    LRESULT i = SendMessageW(E.list, LB_GETCURSEL, 0, 0);
    refilter();
    SendMessageW(E.list, LB_SETCURSEL, (WPARAM)i, 0);
    if (const auto* s = selected()) setText(E.edVal, fieldValue(*s));
    fillQuick();
    status("Changed " + fieldName(field) + " (not saved yet).");
}

void save() {
    std::string err;
    if (!applyQuick(err)) {
        MessageBoxW(E.hwnd, W(err).c_str(), L"Cannot save", MB_ICONWARNING);
        return;
    }
    if (!E.dirty) {
        MessageBoxW(E.hwnd, L"Nothing was changed.", L"Save", MB_ICONINFORMATION);
        return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%04u%02u%02u_%02u%02u%02u", t.wYear, t.wMonth, t.wDay,
             t.wHour, t.wMinute, t.wSecond);
    std::string bdir;
    bool ok = E.game == "rr3" ? E.rr.save(stamp, bdir, err) : E.nfs.save(stamp, bdir, err);
    if (!ok) {
        MessageBoxW(E.hwnd, W("Save failed:\n\n" + err).c_str(), L"Error", MB_ICONERROR);
        return;
    }
    E.dirty = false;
    std::string folder = getText(E.edFolder), filter = getText(E.edFilter);
    openFolder(folder, false);
    setText(E.edFilter, filter);
    size_t slash = bdir.find_last_of("\\/");
    status("Saved. Backup: " + (slash == std::string::npos ? bdir : bdir.substr(slash + 1)));
    MessageBoxW(E.hwnd, W("Saved successfully.\n\nOriginal files were backed up to:\n" + bdir +
                          "\n\nCopy the edited files back to the game folder on your device "
                          "(with the game closed). Turn off cloud save sync or the server "
                          "copy may overwrite your edit.").c_str(),
                L"Saved", MB_ICONINFORMATION);
}

// Profile picture: a PNG, JPG or BMP. Real Racing 3 may keep the picture in
// the save (a blob holding a PNG/JPEG): then it is replaced there, at the
// stored picture's size, and written with "Save changes". Otherwise the game
// draws the avatar from its own textures, and Monkey Tool's main window puts
// the picture into that texture.
void profilePicture() {
    if (E.game.empty()) return;
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = E.hwnd;
    ofn.lpstrFilter = L"Pictures (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Your new profile picture";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;
    Bytes raw;
    Image pic;
    if (!readFile(N(file), raw) || !decodeImageFile(raw.data(), raw.size(), pic) || !pic.ok()) {
        MessageBoxW(E.hwnd, L"This picture could not be read. Use a PNG, JPG or BMP file.",
                    L"Profile picture", MB_ICONWARNING);
        return;
    }
    std::string report;
    if (E.game == "rr3") {
        long at = rr3FindPicture(E.rr);
        if (at >= 0) {
            if (!rr3SetPicture(E.rr, (size_t)at, pic, report)) {
                MessageBoxW(E.hwnd, W("The picture could not be put into the save:\n\n" + report).c_str(),
                            L"Profile picture", MB_ICONERROR);
                return;
            }
            E.dirty = true;
            fillAll();
            status("Profile picture replaced (not saved yet) - " + report);
            MessageBoxW(E.hwnd, W("The save's own picture was replaced:\n" + report +
                                  "\n\nClick \"Save changes\" to write it.").c_str(),
                        L"Profile picture", MB_ICONINFORMATION);
            return;
        }
    }
    std::string field;
    std::string avatar = E.game == "rr3" ? rr3AvatarName(E.rr, &field) : nfsAvatarName(E.nfs, &field);
    if (!E.avatarHook) {
        MessageBoxW(E.hwnd, L"This save keeps no picture of its own.", L"Profile picture", MB_ICONINFORMATION);
        return;
    }
    if (E.avatarHook(E.hwnd, E.game, avatar, pic, report)) {
        status("Profile picture written into the game's avatar texture.");
        MessageBoxW(E.hwnd, W(report).c_str(), L"Profile picture", MB_ICONINFORMATION);
    } else if (!report.empty()) {
        MessageBoxW(E.hwnd, W(report).c_str(), L"Profile picture", MB_ICONWARNING);
    }
}

std::string browseFolder(HWND owner) {
    BROWSEINFOW bi{};
    wchar_t name[MAX_PATH] = {0};
    bi.hwndOwner = owner;
    bi.pszDisplayName = name;
    bi.lpszTitle = L"Pick the folder with the save files (NFS: saves-encrypted, RR3: doc). "
                   L"A parent folder works too.";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | 0x10 /* BIF_EDITBOX */;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return std::string();
    wchar_t path[MAX_PATH] = {0};
    bool ok = SHGetPathFromIDListW(pidl, path) != FALSE;
    CoTaskMemFree(pidl);
    return ok ? N(path) : std::string();
}

void makeBrushes() {
    if (E.bgBrush) DeleteObject(E.bgBrush);
    if (E.ctlBrush) DeleteObject(E.ctlBrush);
    E.bgBrush = CreateSolidBrush(E.theme.windowBg);
    E.ctlBrush = CreateSolidBrush(E.theme.controlBg);
}

LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == ID_OPEN && code == BN_CLICKED) {
            std::string d = browseFolder(hwnd);
            if (!d.empty()) openFolder(d);
        } else if (id == ID_CALIB && code == BN_CLICKED) {
            calibrate();
        } else if (id == ID_SAVE && code == BN_CLICKED) {
            save();
        } else if (id == ID_PICTURE && code == BN_CLICKED) {
            profilePicture();
        } else if (id == ID_APPLY && code == BN_CLICKED) {
            applyAdvanced();
        } else if (id == ID_FILTER && code == EN_CHANGE) {
            refilter();
        } else if (id == ID_LIST && code == LBN_SELCHANGE) {
            if (const auto* f = selected()) {
                setText(E.edVal, fieldValue(*f));
                EnableWindow(E.edVal, fieldEditable(*f));
                EnableWindow(E.btnApply, fieldEditable(*f));
            }
        }
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        wchar_t buf[1024] = {0};
        DragQueryFileW(drop, 0, buf, 1024);
        DragFinish(drop);
        std::string p = N(buf);
        DWORD a = GetFileAttributesW(buf);
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            size_t s = p.find_last_of("\\/");
            if (s != std::string::npos) p = p.substr(0, s);
        }
        openFolder(p);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        HWND c = (HWND)lp;
        bool readOnlyEdit = c == E.edFolder;
        SetTextColor(dc, E.theme.text);
        SetBkColor(dc, readOnlyEdit ? E.theme.controlBg : E.theme.windowBg);
        return (LRESULT)(readOnlyEdit ? E.ctlBrush : E.bgBrush);
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, E.theme.text);
        SetBkColor(dc, E.theme.controlBg);
        return (LRESULT)E.ctlBrush;
    }
    case WM_CTLCOLORBTN:
        SetBkColor((HDC)wp, E.theme.windowBg);
        return (LRESULT)E.bgBrush;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, E.bgBrush);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paintFrames(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* d = (const DRAWITEMSTRUCT*)lp;
        if (d->CtlType == ODT_BUTTON) { drawButton(d); return TRUE; }
        break;
    }
    case WM_CLOSE:
        if (E.dirty &&
            MessageBoxW(hwnd, L"You have unsaved changes. Close the Save Editor anyway?",
                        L"Unsaved changes", MB_YESNO | MB_ICONWARNING) != IDYES)
            return 0;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        E.hwnd = nullptr;
        E.game.clear();
        E.all.clear();
        E.shown.clear();
        E.dirty = false;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

void saveEditorSetAvatarHook(SaveEditorAvatarHook hook) { E.avatarHook = std::move(hook); }

void saveEditorSetTheme(const SaveEditorTheme& theme) {
    E.theme = theme;
    if (!E.hwnd) return;
    makeBrushes();
    applyControlTheme();
    RedrawWindow(E.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void saveEditorOpen(HWND owner, HINSTANCE inst, const SaveEditorTheme& theme) {
    E.theme = theme;
    if (E.hwnd) {
        makeBrushes();
        applyControlTheme();
        ShowWindow(E.hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(E.hwnd);
        InvalidateRect(E.hwnd, nullptr, TRUE);
        return;
    }
    E.inst = inst;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = theme.icon;
        wc.hIconSm = theme.icon;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MonkeyToolSaveEditor";
        RegisterClassExW(&wc);
        registered = true;
    }
    HDC dc = GetDC(nullptr);
    int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    if (dpi <= 0) dpi = 96;
    E.scale = dpi / 96.0;
    if (!E.font)
        E.font = CreateFontW(-(int)(9.0 * dpi / 72.0 + 0.5), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                             DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    makeBrushes();
    if (E.keys.empty()) {
        E.keys = loadKeys(keysPath());
        if (E.keys.empty()) {
            E.keys = loadKeys(oldKeysPath());
            if (!E.keys.empty()) saveKeys(keysPath(), E.keys);
        }
    }

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT rc = { 0, 0, px(720), px(684) };
    AdjustWindowRect(&rc, style, FALSE);
    E.hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, L"MonkeyToolSaveEditor",
                             L"Save Editor  -  NFS No Limits / Real Racing 3", style,
                             CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                             owner, nullptr, inst, nullptr);
    if (!E.hwnd) {
        MessageBoxW(owner, L"Could not create the Save Editor window.", L"Save Editor", MB_ICONERROR);
        return;
    }
    if (theme.icon) {
        SendMessageW(E.hwnd, WM_SETICON, ICON_BIG, (LPARAM)theme.icon);
        SendMessageW(E.hwnd, WM_SETICON, ICON_SMALL, (LPARAM)theme.icon);
    }
    build();
    DragAcceptFiles(E.hwnd, TRUE);
    ShowWindow(E.hwnd, SW_SHOWNORMAL);
    UpdateWindow(E.hwnd);
}

void themeTitleBar(HWND hwnd, bool dark) { darkTitleBar(hwnd, dark); }

bool saveEditorDialogMessage(MSG* msg) {
    return E.hwnd && IsDialogMessageW(E.hwnd, msg);
}

bool saveEditorMayClose(HWND owner) {
    if (!E.hwnd || !E.dirty) return true;
    return MessageBoxW(owner, L"The Save Editor has unsaved changes. Close anyway?",
                       L"Unsaved changes", MB_YESNO | MB_ICONWARNING) == IDYES;
}

#endif // _WIN32
