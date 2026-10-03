#pragma once
#include <windows.h>
struct SHFILEOPSTRUCTW {
    HWND hwnd; UINT wFunc; LPCWSTR pFrom, pTo; WORD fFlags;
    BOOL fAnyOperationsAborted; LPVOID hNameMappings; LPCWSTR lpszProgressTitle;
};
#define FO_DELETE 3
#define FOF_SILENT 4
#define FOF_NOCONFIRMATION 0x10
#define FOF_NOERRORUI 0x400
#define FOF_NO_UI 0x614
extern "C" {
int SHFileOperationW(SHFILEOPSTRUCTW*);
HANDLE ShellExecuteW(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, int);
}

// 0.7.3: drag a save folder onto the Save Editor
typedef HANDLE HDROP;
extern "C" UINT DragQueryFileW(HDROP, UINT, LPWSTR, UINT);
extern "C" void DragFinish(HDROP);
extern "C" void DragAcceptFiles(HWND, BOOL);
