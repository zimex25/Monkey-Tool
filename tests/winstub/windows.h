// windows.h - NOT a real Windows header.
//
// A stub that declares only the Win32 surface main_win32.cpp actually uses,
// so the file can be syntax-checked with g++ on Linux. It catches typos,
// wrong argument counts, bad types and plain C++ mistakes. It does NOT
// validate against the real SDK, and nothing here is ever built into the
// shipped program.
#pragma once
#include <cstdint>
#include <cstddef>

typedef void*          HANDLE;
typedef HANDLE         HINSTANCE;
typedef HANDLE         HWND;
typedef HANDLE         HDC;
typedef HANDLE         HMENU;
typedef HANDLE         HICON;
typedef HANDLE         HBRUSH;
typedef HANDLE         HPEN;
typedef HANDLE         HRSRC;
typedef HANDLE         HGLOBAL;
typedef HANDLE         HFONT;
typedef HANDLE         HGDIOBJ;
typedef HANDLE         HTREEITEM;
typedef HANDLE         HKEY;
typedef HANDLE         HMODULE;
typedef unsigned long  DWORD;
typedef unsigned short WORD;
typedef unsigned char  BYTE;
typedef unsigned int   UINT;
typedef int            BOOL;
typedef int            INT;
typedef long           LONG;
typedef void*          LPVOID;
typedef const void*    LPCVOID;
typedef char*          LPSTR;
typedef const char*    LPCSTR;
typedef wchar_t*       LPWSTR;
typedef const wchar_t* LPCWSTR;
typedef BYTE*          LPBYTE;
typedef DWORD          COLORREF;
typedef long           HRESULT;
typedef uintptr_t      UINT_PTR;
typedef uintptr_t      ULONG_PTR;
typedef UINT_PTR       WPARAM;
typedef intptr_t       LPARAM;
typedef intptr_t       LRESULT;
typedef unsigned long long ULONGLONG;

#define WINAPI
#define CALLBACK
#define MAX_PATH 260
#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)-1)
#define ERROR_SUCCESS 0L
#define S_OK 0L

#define MAKEINTRESOURCEW(i) ((LPCWSTR)(ULONG_PTR)(WORD)(i))
#define LOWORD(l) ((WORD)(((ULONG_PTR)(l)) & 0xffff))
#define HIWORD(l) ((WORD)((((ULONG_PTR)(l)) >> 16) & 0xffff))
#define RGB(r,g,b) ((COLORREF)(((BYTE)(r)|((WORD)((BYTE)(g))<<8))|(((DWORD)(BYTE)(b))<<16)))
#define GetRValue(c) ((BYTE)((c) & 0xff))
#define GetGValue(c) ((BYTE)(((c) >> 8) & 0xff))
#define GetBValue(c) ((BYTE)(((c) >> 16) & 0xff))
#define GET_WHEEL_DELTA_WPARAM(wp) ((short)HIWORD(wp))

#define SW_HIDE 0
#define SW_SHOW 5
#define VK_CONTROL 0x11

#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP   0x0202
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP   0x0205
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP   0x0208
#define WM_MOUSEMOVE   0x0200
#define WM_MOUSEWHEEL  0x020A
#define WM_CTLCOLOREDIT    0x0133
#define WM_CTLCOLORLISTBOX 0x0134

#define WS_VSCROLL 0x00200000L
#define WS_HSCROLL 0x00100000L
#define ES_AUTOHSCROLL 0x0080
#define ES_AUTOVSCROLL 0x0040
#define ES_WANTRETURN 0x1000
#define ES_NOHIDESEL 0x0100
#define EM_SETLIMITTEXT 0x00C5
#define OUT_DEFAULT_PRECIS 0
#define CLIP_DEFAULT_PRECIS 0
#define FIXED_PITCH 1
#define FF_MODERN 0x30
#define BS_AUTOCHECKBOX 0x0003
#define BM_SETCHECK 0x00F1
#define BM_GETCHECK 0x00F0
#define BST_CHECKED 1
#define PS_SOLID 0
#define NULL_BRUSH 5
#define FW_SEMIBOLD 600
#define DEFAULT_CHARSET 1
#define CLEARTYPE_QUALITY 5
#define DT_END_ELLIPSIS 0x8000
#define RT_RCDATA ((LPCWSTR)(ULONG_PTR)10)
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define WS_OVERLAPPED 0x00000000L
#define WS_CAPTION 0x00C00000L
#define WS_SYSMENU 0x00080000L
#define WM_CLOSE 0x0010
#define ES_MULTILINE 0x0004
#define ES_READONLY 0x0800
#define BS_PUSHBUTTON  0x0000
#define CBS_DROPDOWNLIST 0x0003
#define CB_ADDSTRING     0x0143
#define CB_RESETCONTENT  0x014B
#define CB_GETCURSEL     0x0147
#define CB_SETCURSEL     0x014E
#define CB_GETLBTEXT     0x0148
#define CBN_SELCHANGE    1
#define EN_CHANGE        0x0300

struct RECT { LONG left, top, right, bottom; };
struct POINT { LONG x, y; };
struct SIZE { LONG cx, cy; };
struct PAINTSTRUCT { HDC hdc; BOOL fErase; RECT rcPaint; };
struct MSG { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; };
struct FILETIME { DWORD dwLowDateTime, dwHighDateTime; };

struct WIN32_FIND_DATAW {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    wchar_t cFileName[MAX_PATH];
    wchar_t cAlternateFileName[14];
};
struct WIN32_FIND_DATAA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    char cFileName[MAX_PATH];
    char cAlternateFileName[14];
};
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)

typedef LRESULT (CALLBACK* WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef DWORD (WINAPI* LPTHREAD_START_ROUTINE)(LPVOID);

struct WNDCLASSEXW {
    UINT cbSize, style;
    WNDPROC lpfnWndProc;
    int cbClsExtra, cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HANDLE hCursor;
    HBRUSH hbrBackground;
    LPCWSTR lpszMenuName, lpszClassName;
    HICON hIconSm;
};

struct NMHDR { HWND hwndFrom; UINT_PTR idFrom; UINT code; };
typedef NMHDR* LPNMHDR;

// ---- window messages / styles ----
#define WM_CREATE 1
#define WM_DESTROY 2
#define WM_SIZE 5
#define WM_PAINT 15
#define WM_ERASEBKGND 20
#define WM_SETREDRAW 11
#define WM_SETFONT 48
#define WM_COMMAND 273
#define WM_NOTIFY 78
#define WM_CTLCOLORSTATIC 312
#define WM_APP 32768
#define WS_CHILD 0x40000000
#define WS_VISIBLE 0x10000000
#define WS_OVERLAPPEDWINDOW 0x00CF0000
#define WS_EX_CLIENTEDGE 0x200
#define SS_LEFTNOWORDWRAP 0x0C
#define CW_USEDEFAULT ((int)0x80000000)
#define COLOR_WINDOW 5
#define TRANSPARENT 1
#define SRCCOPY 0x00CC0020
#define HALFTONE 4
#define BI_RGB 0
#define DIB_RGB_COLORS 0
#define DEFAULT_GUI_FONT 17
#define IDC_ARROW ((LPCWSTR)(ULONG_PTR)32512)
#define IMAGE_ICON 1
#define LR_DEFAULTSIZE 0x40
#define LR_SHARED 0x8000
#define LR_LOADFROMFILE 0x0010
#define WM_SETICON 0x0080
#define ICON_SMALL 0
#define ICON_BIG 1
#define WM_QUERYUISTATE 0x0129
#define UISF_HIDEACCEL 0x2
#define DT_HIDEPREFIX 0x00100000
#define OBJID_MENU 0xFFFFFFFD
typedef struct tagMENUBARINFO {
    DWORD cbSize; RECT rcBar; HMENU hMenu; HWND hwndMenu; BOOL fBarFocused; BOOL fFocused;
} MENUBARINFO, *PMENUBARINFO;
#define SM_CXSMICON 49
#define SM_CYSMICON 50
#define MB_ICONERROR 0x10
#define MB_ICONWARNING 0x30
#define MB_ICONINFORMATION 0x40
#define MF_STRING 0
#define MF_POPUP 0x10
#define MF_SEPARATOR 0x800
#define MF_BYCOMMAND 0
#define DT_LEFT 0
#define DT_TOP 0
#define DT_WORDBREAK 0x10
#define DT_NOPREFIX 0x800


// ---- diagnostics ----
struct SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
};
struct EXCEPTION_RECORD {
    DWORD ExceptionCode, ExceptionFlags;
    EXCEPTION_RECORD* ExceptionRecord;
    LPVOID ExceptionAddress;
    DWORD NumberParameters;
    ULONG_PTR ExceptionInformation[15];
};
struct CONTEXT { DWORD ContextFlags; };
struct EXCEPTION_POINTERS { EXCEPTION_RECORD* ExceptionRecord; CONTEXT* ContextRecord; };
typedef LONG (WINAPI* PTOP_LEVEL_EXCEPTION_FILTER)(EXCEPTION_POINTERS*);
#define EXCEPTION_EXECUTE_HANDLER 1
extern "C" {
void GetLocalTime(SYSTEMTIME*);
PTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(PTOP_LEVEL_EXCEPTION_FILTER);
void ExitProcess(UINT);
DWORD GetTickCount();
}

// ---- process / file helpers ----
#define STARTF_USESHOWWINDOW 0x00000001
#define CREATE_NO_WINDOW 0x08000000
struct STARTUPINFOW {
    DWORD cb; LPWSTR lpReserved, lpDesktop, lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    WORD wShowWindow, cbReserved2; LPBYTE lpReserved2;
    HANDLE hStdInput, hStdOutput, hStdError;
};
struct PROCESS_INFORMATION { HANDLE hProcess, hThread; DWORD dwProcessId, dwThreadId; };
extern "C" {
BOOL DeleteFileW(LPCWSTR);
BOOL CreateProcessW(LPCWSTR, LPWSTR, LPVOID, LPVOID, BOOL, DWORD, LPVOID, LPCWSTR,
                    STARTUPINFOW*, PROCESS_INFORMATION*);
BOOL EnableWindow(HWND, BOOL);
HANDLE FindResourceW(HINSTANCE, LPCWSTR, LPCWSTR);
HANDLE LoadResource(HINSTANCE, HANDLE);
DWORD SizeofResource(HINSTANCE, HANDLE);
LPVOID LockResource(HANDLE);
HANDLE CreateFontW(int,int,int,int,int,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,DWORD,LPCWSTR);
HANDLE CreatePen(int,int,COLORREF);
BOOL Rectangle(HDC,int,int,int,int);
BOOL AdjustWindowRect(RECT*,DWORD,BOOL);
BOOL IsWindow(HWND);
BOOL SetForegroundWindow(HWND);
DWORD WaitForSingleObject(HANDLE, DWORD);
}

// ---- registry ----
#define HKEY_CURRENT_USER ((HKEY)(ULONG_PTR)0x80000001)
#define KEY_READ 0x20019
#define KEY_WRITE 0x20006
#define REG_DWORD 4
#define REG_SZ 1
#define IDYES 6
#define MB_ICONQUESTION 0x20
#define MB_YESNO 4
#define OFN_FILEMUSTEXIST 0x1000

// ---- GDI ----
struct BITMAPINFOHEADER {
    DWORD biSize; LONG biWidth, biHeight; WORD biPlanes, biBitCount;
    DWORD biCompression, biSizeImage; LONG biXPelsPerMeter, biYPelsPerMeter;
    DWORD biClrUsed, biClrImportant;
};
struct RGBQUAD { BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved; };
struct BITMAPINFO { BITMAPINFOHEADER bmiHeader; RGBQUAD bmiColors[1]; };

extern "C" {
HWND CreateWindowExW(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int,
                     HWND, HMENU, HINSTANCE, LPVOID);
BOOL DestroyWindow(HWND);
BOOL ShowWindow(HWND, int);
BOOL UpdateWindow(HWND);
BOOL MoveWindow(HWND, int, int, int, int, BOOL);
BOOL GetClientRect(HWND, RECT*);
BOOL InvalidateRect(HWND, const RECT*, BOOL);
LRESULT DefWindowProcW(HWND, UINT, WPARAM, LPARAM);
LRESULT SendMessageW(HWND, UINT, WPARAM, LPARAM);
BOOL PostMessageW(HWND, UINT, WPARAM, LPARAM);
void PostQuitMessage(int);
BOOL GetMessageW(MSG*, HWND, UINT, UINT);
BOOL TranslateMessage(const MSG*);
LRESULT DispatchMessageW(const MSG*);
WORD RegisterClassExW(const WNDCLASSEXW*);
BOOL SetWindowTextW(HWND, LPCWSTR);
int GetWindowTextW(HWND, LPWSTR, int);
#define WM_TIMER 0x0113
#define WM_HSCROLL 0x0114
#define WM_USER 0x0400
#define MB_YESNOCANCEL 0x00000003L
#define IDCANCEL 2
UINT_PTR SetTimer(HWND, UINT_PTR, UINT, void*);
BOOL KillTimer(HWND, UINT_PTR);
BOOL IsWindowVisible(HWND);
HWND SetCapture(HWND);
BOOL ReleaseCapture();
BOOL PtInRect(const RECT*, POINT);
BOOL ScreenToClient(HWND, POINT*);
short GetKeyState(int);
int SetDIBitsToDevice(HDC, int, int, DWORD, DWORD, int, int, UINT, UINT,
                      const void*, const BITMAPINFO*, UINT);
int MessageBoxW(HWND, LPCWSTR, LPCWSTR, UINT);
HANDLE LoadCursor(HINSTANCE, LPCWSTR);
HANDLE LoadImageW(HINSTANCE, LPCWSTR, UINT, int, int, UINT);
DWORD GetModuleFileNameW(HMODULE, LPWSTR, DWORD);
BOOL GetMenuBarInfo(HWND, LONG, LONG, PMENUBARINFO);
BOOL GetWindowRect(HWND, RECT*);
BOOL OffsetRect(RECT*, int, int);
int GetSystemMetrics(int);
HMENU CreateMenu();
HMENU CreatePopupMenu();
BOOL AppendMenuW(HMENU, UINT, UINT_PTR, LPCWSTR);
HMENU GetMenu(HWND);
BOOL CheckMenuRadioItem(HMENU, UINT, UINT, UINT, UINT);
HDC BeginPaint(HWND, PAINTSTRUCT*);
BOOL EndPaint(HWND, const PAINTSTRUCT*);
int FillRect(HDC, const RECT*, HBRUSH);
int DrawTextW(HDC, LPCWSTR, int, RECT*, UINT);
COLORREF SetTextColor(HDC, COLORREF);
COLORREF SetBkColor(HDC, COLORREF);
int SetBkMode(HDC, int);
HBRUSH CreateSolidBrush(COLORREF);
BOOL DeleteObject(HGDIOBJ);
HGDIOBJ GetStockObject(int);
HGDIOBJ SelectObject(HDC, HGDIOBJ);
int SetStretchBltMode(HDC, int);
int StretchDIBits(HDC, int, int, int, int, int, int, int, int,
                  LPCVOID, const BITMAPINFO*, UINT, DWORD);
DWORD GetTempPathW(DWORD, LPWSTR);
BOOL CreateDirectoryW(LPCWSTR, LPVOID);
DWORD GetFileAttributesW(LPCWSTR);
DWORD GetTickCount();
HANDLE FindFirstFileW(LPCWSTR, WIN32_FIND_DATAW*);
BOOL FindNextFileW(HANDLE, WIN32_FIND_DATAW*);
HANDLE FindFirstFileA(LPCSTR, WIN32_FIND_DATAA*);
BOOL FindNextFileA(HANDLE, WIN32_FIND_DATAA*);
BOOL FindClose(HANDLE);
HANDLE CreateThread(LPVOID, size_t, LPTHREAD_START_ROUTINE, LPVOID, DWORD, DWORD*);
BOOL CloseHandle(HANDLE);
#define INFINITE 0xFFFFFFFFu
typedef struct { WORD wProcessorArchitecture; WORD wReserved; DWORD dwPageSize; LPVOID lpMinimumApplicationAddress; LPVOID lpMaximumApplicationAddress; size_t dwActiveProcessorMask; DWORD dwNumberOfProcessors; DWORD dwProcessorType; DWORD dwAllocationGranularity; WORD wProcessorLevel; WORD wProcessorRevision; } SYSTEM_INFO;
void GetSystemInfo(SYSTEM_INFO*);
HMODULE LoadLibraryA(LPCSTR);
void* GetProcAddress(HMODULE, LPCSTR);
int MultiByteToWideChar(UINT, DWORD, LPCSTR, int, LPWSTR, int);
int WideCharToMultiByte(UINT, DWORD, LPCWSTR, int, LPSTR, int, LPCSTR, BOOL*);
int wsprintfW(LPWSTR, LPCWSTR, ...);
long RegOpenKeyExW(HKEY, LPCWSTR, DWORD, DWORD, HKEY*);
long RegCreateKeyExW(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, DWORD, LPVOID, HKEY*, DWORD*);
long RegQueryValueExW(HKEY, LPCWSTR, DWORD*, DWORD*, LPBYTE, DWORD*);
long RegSetValueExW(HKEY, LPCWSTR, DWORD, DWORD, const BYTE*, DWORD);
long RegCloseKey(HKEY);
}
#define CP_UTF8 65001

// ---- menus / owner-draw ----
#define WM_MEASUREITEM 44
#define WM_DRAWITEM 43
#define ODT_MENU 1
#define ODS_SELECTED 1
#define ODS_GRAYED 2
#define ODS_DISABLED 4
#define ODS_CHECKED 8
#define MIM_BACKGROUND 2
#define MIM_APPLYTOSUBMENUS 0x80000000
#define MIIM_STATE 1
#define MIIM_ID 2
#define MIIM_SUBMENU 4
#define MIIM_DATA 0x20
#define MIIM_STRING 0x40
#define MIIM_FTYPE 0x100
#define MFT_SEPARATOR 0x800
#define MFT_OWNERDRAW 0x100
#define DT_VCENTER 4
#define DT_SINGLELINE 32
struct MENUINFO {
    DWORD cbSize, fMask, dwStyle; UINT cyMax; HBRUSH hbrBack;
    DWORD dwContextHelpID; ULONG_PTR dwMenuData;
};
struct MENUITEMINFOW {
    UINT cbSize, fMask, fType, fState, wID;
    HMENU hSubMenu; HANDLE hbmpChecked, hbmpUnchecked;
    ULONG_PTR dwItemData; LPWSTR dwTypeData; UINT cch; HANDLE hbmpItem;
};
struct MEASUREITEMSTRUCT {
    UINT CtlType, CtlID, itemID, itemWidth, itemHeight; ULONG_PTR itemData;
};
typedef MEASUREITEMSTRUCT* LPMEASUREITEMSTRUCT;
struct DRAWITEMSTRUCT {
    UINT CtlType, CtlID, itemID, itemAction, itemState;
    HWND hwndItem; HDC hDC; RECT rcItem; ULONG_PTR itemData;
};
typedef DRAWITEMSTRUCT* LPDRAWITEMSTRUCT;
extern "C" {
BOOL SetMenuInfo(HMENU, const MENUINFO*);
int  GetMenuItemCount(HMENU);
BOOL GetMenuItemInfoW(HMENU, UINT, BOOL, MENUITEMINFOW*);
BOOL SetMenuItemInfoW(HMENU, UINT, BOOL, MENUITEMINFOW*);
BOOL DrawMenuBar(HWND);
HDC  GetDC(HWND);
int  ReleaseDC(HWND, HDC);
BOOL GetTextExtentPoint32W(HDC, LPCWSTR, int, SIZE*);
// 0.7.3: Save Editor button, accelerators
typedef intptr_t INT_PTR;
#define SW_SHOWNORMAL 1
#define ODS_FOCUS    0x0010
#define ODS_HOTLIGHT 0x0040
BOOL Polygon(HDC, const POINT*, int);
BOOL Ellipse(HDC, int, int, int, int);
BOOL MoveToEx(HDC, int, int, POINT*);
BOOL LineTo(HDC, int, int);
typedef HANDLE HACCEL;
struct ACCEL { BYTE fVirt; WORD key; WORD cmd; };
#define FVIRTKEY 0x01
#define FCONTROL 0x08
HACCEL CreateAcceleratorTableW(ACCEL*, int);
BOOL DestroyAcceleratorTable(HACCEL);
int TranslateAcceleratorW(HWND, HACCEL, MSG*);
#ifndef BS_OWNERDRAW
#define BS_OWNERDRAW 0x0000000B
#endif
#ifndef WS_EX_TOPMOST
#define WS_EX_TOPMOST 0x00000008
#endif
#ifndef WS_POPUP
#define WS_POPUP 0x80000000
#endif
#ifndef CW_USEDEFAULT
#define CW_USEDEFAULT ((int)0x80000000)
#endif
#ifndef ODT_BUTTON
#define ODT_BUTTON 4
#endif
#include <cstdio>
FILE* _wfopen(const wchar_t*, const wchar_t*);
// 0.7.3: built-in Save Editor window
DWORD GetEnvironmentVariableW(LPCWSTR, LPWSTR, DWORD);
int GetWindowTextLengthW(HWND);
#ifndef SS_LEFT
#define SS_LEFT 0x0
#endif
#ifndef SS_CENTERIMAGE
#define SS_CENTERIMAGE 0x200
#endif
#ifndef BS_GROUPBOX
#define BS_GROUPBOX 0x7
#endif
#ifndef LBS_NOINTEGRALHEIGHT
#define LBS_NOINTEGRALHEIGHT 0x0100
#endif
#ifndef LBS_NOTIFY
#define LBS_NOTIFY 0x0001
#endif
#ifndef LBS_USETABSTOPS
#define LBS_USETABSTOPS 0x0080
#endif
#ifndef SS_RIGHT
#define SS_RIGHT 0x2
#endif
#ifndef WS_TABSTOP
#define WS_TABSTOP 0x00010000L
#endif
#ifndef LB_ADDSTRING
#define LB_ADDSTRING 0x0180
#endif
#ifndef LB_RESETCONTENT
#define LB_RESETCONTENT 0x0184
#endif
#ifndef LB_GETCURSEL
#define LB_GETCURSEL 0x0188
#endif
#ifndef LB_SETCURSEL
#define LB_SETCURSEL 0x0186
#endif
#ifndef LBN_SELCHANGE
#define LBN_SELCHANGE 1
#endif
#ifndef EN_CHANGE
#define EN_CHANGE 0x0300
#endif
#ifndef BN_CLICKED
#define BN_CLICKED 0
#endif
#ifndef WM_SETREDRAW
#define WM_SETREDRAW 0x000B
#endif
#ifndef WM_DROPFILES
#define WM_DROPFILES 0x0233
#endif
#ifndef WM_CTLCOLORBTN
#define WM_CTLCOLORBTN 0x0135
#endif
#ifndef WM_SETICON
#define WM_SETICON 0x0080
#endif
#ifndef ICON_BIG
#define ICON_BIG 1
#endif
#ifndef ICON_SMALL
#define ICON_SMALL 0
#endif
#ifndef WS_EX_ACCEPTFILES
#define WS_EX_ACCEPTFILES 0x10L
#endif
#ifndef WS_MINIMIZEBOX
#define WS_MINIMIZEBOX 0x00020000L
#endif
#ifndef WS_CLIPCHILDREN
#define WS_CLIPCHILDREN 0x02000000L
#endif
#ifndef WS_OVERLAPPED
#define WS_OVERLAPPED 0x0L
#endif
#ifndef WS_CAPTION
#define WS_CAPTION 0x00C00000L
#endif
#ifndef WS_SYSMENU
#define WS_SYSMENU 0x00080000L
#endif
#ifndef CLEARTYPE_QUALITY
#define CLEARTYPE_QUALITY 5
#endif
#ifndef DEFAULT_CHARSET
#define DEFAULT_CHARSET 1
#endif
#ifndef FW_NORMAL
#define FW_NORMAL 400
#endif
#ifndef LOGPIXELSY
#define LOGPIXELSY 90
#endif
#ifndef ES_READONLY
#define ES_READONLY 0x0800L
#endif
#ifndef COLOR_BTNFACE
#define COLOR_BTNFACE 15
#endif
#ifndef SW_SHOWNORMAL
#define SW_SHOWNORMAL 1
#endif
#ifndef IDC_ARROW
#define IDC_ARROW ((LPCWSTR)32512)
#endif
BOOL IsDialogMessageW(HWND, MSG*);
BOOL AdjustWindowRect(RECT*, DWORD, BOOL);
int GetDeviceCaps(HDC, int);
BOOL SetForegroundWindow(HWND);
#ifndef OPAQUE
#define OPAQUE 2
#endif
#ifndef DT_CENTER
#define DT_CENTER 0x1
#endif
#ifndef RDW_INVALIDATE
#define RDW_INVALIDATE 0x1
#define RDW_ERASE 0x4
#define RDW_ALLCHILDREN 0x80
#endif
BOOL RoundRect(HDC, int, int, int, int, int, int);
HMODULE LoadLibraryW(LPCWSTR);
BOOL RedrawWindow(HWND, const RECT*, HANDLE, UINT);
BOOL OffsetRect(RECT*, int, int);
}

// ---- added for the picture import (0.7.9) ----
typedef HANDLE HCURSOR;
HCURSOR SetCursor(HCURSOR);
#ifndef IDC_WAIT
#define IDC_WAIT ((LPCWSTR)(ULONG_PTR)32514)
#endif
int _wcsicmp(const wchar_t*, const wchar_t*);
extern "C" int _stricmp(const char*, const char*);
#define BS_LEFT 0x00000100L
BOOL CopyFileW(LPCWSTR, LPCWSTR, BOOL);

// ---- 0.9.4: OpenGL viewer ----
struct PIXELFORMATDESCRIPTOR {
    WORD nSize, nVersion; DWORD dwFlags; BYTE iPixelType, cColorBits, cRedBits, cRedShift,
    cGreenBits, cGreenShift, cBlueBits, cBlueShift, cAlphaBits, cAlphaShift, cAccumBits,
    cAccumRedBits, cAccumGreenBits, cAccumBlueBits, cAccumAlphaBits, cDepthBits, cStencilBits,
    cAuxBuffers, iLayerType, bReserved; DWORD dwLayerMask, dwVisibleMask, dwDamageMask;
};
typedef struct HGLRC__* HGLRC;
#define PFD_DRAW_TO_WINDOW 0x00000004
#define PFD_SUPPORT_OPENGL 0x00000020
#define PFD_DOUBLEBUFFER 0x00000001
#define PFD_TYPE_RGBA 0
#define PFD_MAIN_PLANE 0
#define CS_OWNDC 0x0020
extern "C" int ChoosePixelFormat(HDC, const PIXELFORMATDESCRIPTOR*);
extern "C" BOOL SetPixelFormat(HDC, int, const PIXELFORMATDESCRIPTOR*);
extern "C" BOOL SwapBuffers(HDC);
extern "C" HGLRC wglCreateContext(HDC);
extern "C" BOOL wglMakeCurrent(HDC, HGLRC);
extern "C" BOOL wglDeleteContext(HGLRC);
extern "C" BOOL wglUseFontBitmapsW(HDC, DWORD, DWORD, DWORD);
int MapWindowPoints(HWND, HWND, POINT*, UINT);
#define MAKELPARAM(l, h) ((LPARAM)(DWORD)(((WORD)(l)) | ((DWORD)((WORD)(h))) << 16))
#ifndef WS_CLIPSIBLINGS
#define WS_CLIPSIBLINGS 0x04000000L
#endif
#ifndef WS_CLIPCHILDREN
#define WS_CLIPCHILDREN 0x02000000L
#endif
BOOL TextOutW(HDC, int, int, LPCWSTR, int);
BOOL GetCursorPos(POINT*);
BOOL DestroyMenu(HMENU);
BOOL TrackPopupMenu(HMENU, UINT, int, int, int, HWND, const void*);
#define TPM_RETURNCMD 0x0100L
#define TPM_RIGHTBUTTON 0x0002L
#ifndef IDNO
#define IDNO 7
#endif
#ifndef MB_YESNO
#define MB_YESNO 0x00000004L
#endif

/* the real windows.h defines these as empty macros */
#define far
#define near
#define FAR
#define NEAR
#ifndef interface
#define interface struct
#endif

/* off-screen painting and keys (1.1) */
typedef void* HBITMAP;
HDC CreateCompatibleDC(HDC);
HBITMAP CreateCompatibleBitmap(HDC, int, int);
BOOL DeleteDC(HDC);
BOOL BitBlt(HDC, int, int, int, int, HDC, int, int, DWORD);
#ifndef VK_RETURN
#define VK_RETURN 0x0D
#define VK_ESCAPE 0x1B
#endif
#ifndef CS_DBLCLKS
#define CS_DBLCLKS 0x0008
#endif
#ifndef WM_LBUTTONDBLCLK
#define WM_LBUTTONDBLCLK 0x0203
#endif
#ifndef WM_KEYDOWN
#define WM_KEYDOWN 0x0100
#endif
HWND SetFocus(HWND);
#ifndef SW_SHOWMAXIMIZED
#define SW_SHOWMAXIMIZED 3
#endif

HBITMAP CreateDIBSection(HDC, const BITMAPINFO*, UINT, void**, HANDLE, DWORD);
#ifndef FW_LIGHT
#define FW_LIGHT 300
#endif
#ifndef WS_EX_TOOLWINDOW
#define WS_EX_TOOLWINDOW 0x00000080L
#endif
#ifndef WS_DISABLED
#define WS_DISABLED 0x08000000L
#endif
#ifndef WS_BORDER
#define WS_BORDER 0x00800000L
#endif
#ifndef SW_SHOWNOACTIVATE
#define SW_SHOWNOACTIVATE 4
#endif
#ifndef IDC_WAIT
#define IDC_WAIT ((LPCWSTR)32514)
#endif
#ifndef MT_STUB_EXTRA_VIDEO
#define MT_STUB_EXTRA_VIDEO
#ifndef IDC_HAND
#define IDC_HAND ((LPCWSTR)32649)
#endif
#ifndef BLACK_BRUSH
#define BLACK_BRUSH 4
#endif
#ifndef SUCCEEDED
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#endif
#ifndef FAILED
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#endif
typedef long long LONGLONG;
#endif
#ifndef MT_STUB_UINT32
#define MT_STUB_UINT32
typedef unsigned int UINT32;
typedef unsigned long ULONG;
#endif
#ifndef MT_STUB_VIDEO2
#define MT_STUB_VIDEO2
HWND GetParent(HWND);
#ifndef VK_SPACE
#define VK_SPACE 0x20
#endif
#endif
#ifndef VK_SHIFT
#define VK_SHIFT 0x10
#endif
#ifndef COLOR_HIGHLIGHT
#define COLOR_HIGHLIGHT 13
#define COLOR_HIGHLIGHTTEXT 14
#endif
DWORD GetSysColor(int);
#ifndef CB_GETCOUNT
#define CB_GETCOUNT 0x0146
#endif
