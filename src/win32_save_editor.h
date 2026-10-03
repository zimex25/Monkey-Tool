// win32_save_editor.h - the Save Editor window, part of Monkey Tool itself
#pragma once
#ifdef _WIN32
#include <windows.h>

struct SaveEditorTheme {
    bool dark = false;
    COLORREF windowBg = RGB(0xF3, 0xF3, 0xF3);
    COLORREF controlBg = RGB(0xFF, 0xFF, 0xFF);
    COLORREF text = RGB(0, 0, 0);
    COLORREF dimText = RGB(0x60, 0x60, 0x60);
    HICON icon = nullptr;
};

#include <functional>
#include <string>
#include "nfsnl.h"

// Monkey Tool's half of "Profile picture...": when the save keeps no picture
// of its own, the game draws the player's avatar from one of its textures.
// The main window owns the game library, so it does that part: game is
// "nfs" or "rr3", avatar the name the save gives (may be empty).
using SaveEditorAvatarHook = std::function<bool(HWND owner, const std::string& game,
                                                const std::string& avatar,
                                                const nfsnl::Image& pic, std::string& report)>;
void saveEditorSetAvatarHook(SaveEditorAvatarHook hook);

// Opens the window, or brings it to the front if it is already open.
void saveEditorOpen(HWND owner, HINSTANCE inst, const SaveEditorTheme& theme);
// For the message loop: Tab, Enter and the arrow keys between its controls.
bool saveEditorDialogMessage(MSG* msg);
// Called before the tool closes; false when the user chose to keep editing.
bool saveEditorMayClose(HWND owner);
// Ask Windows for a dark (or light) title bar on any window.
void themeTitleBar(HWND hwnd, bool dark);
// The theme changed in the main window.
void saveEditorSetTheme(const SaveEditorTheme& theme);
#endif
