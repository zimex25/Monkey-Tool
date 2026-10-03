#pragma once
#include <windows.h>
struct OPENFILENAMEW {
    DWORD lStructSize; HWND hwndOwner; HINSTANCE hInstance;
    LPCWSTR lpstrFilter; LPWSTR lpstrCustomFilter; DWORD nMaxCustFilter, nFilterIndex;
    LPWSTR lpstrFile; DWORD nMaxFile; LPWSTR lpstrFileTitle; DWORD nMaxFileTitle;
    LPCWSTR lpstrInitialDir, lpstrTitle; DWORD Flags; WORD nFileOffset, nFileExtension;
    LPCWSTR lpstrDefExt; LPARAM lCustData; LPVOID lpfnHook; LPCWSTR lpTemplateName;
    LPVOID pvReserved; DWORD dwReserved, FlagsEx;
};
#define OFN_OVERWRITEPROMPT 2
#define OFN_PATHMUSTEXIST 0x800
#define OFN_NOCHANGEDIR 8
extern "C" BOOL GetSaveFileNameW(OPENFILENAMEW*);
BOOL GetOpenFileNameW(OPENFILENAMEW*);
struct CHOOSECOLORW {
    DWORD lStructSize; HWND hwndOwner; HWND hInstance; COLORREF rgbResult; COLORREF* lpCustColors;
    DWORD Flags; LPARAM lCustData; LPVOID lpfnHook; LPCWSTR lpTemplateName;
};
#define CC_RGBINIT 1
#define CC_FULLOPEN 2
BOOL ChooseColorW(CHOOSECOLORW*);
