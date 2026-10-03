#pragma once
#include <windows.h>
typedef struct _ITEMIDLIST* LPITEMIDLIST;
struct BROWSEINFOW {
    HWND hwndOwner; LPITEMIDLIST pidlRoot; LPWSTR pszDisplayName;
    LPCWSTR lpszTitle; UINT ulFlags; LPVOID lpfn; LPARAM lParam; int iImage;
};
#define BIF_RETURNONLYFSDIRS 1
#define BIF_NEWDIALOGSTYLE 0x40
extern "C" {
LPITEMIDLIST SHBrowseForFolderW(BROWSEINFOW*);
BOOL SHGetPathFromIDListW(LPITEMIDLIST, LPWSTR);
}
