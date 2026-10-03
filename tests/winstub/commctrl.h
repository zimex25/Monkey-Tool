#pragma once
#define TVN_ITEMEXPANDINGW (0U-459U)
#define TVE_EXPAND 2
#define TVIF_CHILDREN 0x40
#define TVM_SETITEMW 0x113F
#define TVIF_HANDLE 0x10
#define TVI_LAST ((HTREEITEM)(ULONG_PTR)-0xFFFE)

#include <windows.h>
#define WC_TREEVIEWW L"SysTreeView32"
#define ICC_TREEVIEW_CLASSES 2
#define TVS_HASBUTTONS 1
#define TVS_HASLINES 2
#define TVS_LINESATROOT 4
#define TVS_SHOWSELALWAYS 0x20
#define TVIF_TEXT 1
#define TVIF_PARAM 4
#define TVI_ROOT ((HTREEITEM)(ULONG_PTR)-0x10000)
#define TVI_SORT ((HTREEITEM)(ULONG_PTR)-0x0FFFD)
#define TVM_INSERTITEMW 0x1132
#define TVM_DELETEITEM 0x1101
#define TVM_GETNEXTITEM 0x110A
#define TVM_SETBKCOLOR 0x111D
#define TVM_SETTEXTCOLOR 0x111E
#define TVM_SETLINECOLOR 0x1128
#define TVGN_CARET 9
#define TVN_SELCHANGEDW (0U-402U)
#define NM_DBLCLK (0U-3U)
struct INITCOMMONCONTROLSEX { DWORD dwSize, dwICC; };
struct TVITEMW {
    UINT mask; HTREEITEM hItem; UINT state, stateMask;
    LPWSTR pszText; int cchTextMax, iImage, iSelectedImage, cChildren;
    LPARAM lParam;
};
struct TVINSERTSTRUCTW { HTREEITEM hParent, hInsertAfter; TVITEMW item; };
struct NMTREEVIEWW { NMHDR hdr; UINT action; TVITEMW itemOld, itemNew; POINT ptDrag; };
typedef NMTREEVIEWW* LPNMTREEVIEWW;
extern "C" BOOL InitCommonControlsEx(const INITCOMMONCONTROLSEX*);
#define TVM_EXPAND 0x1102
#define TreeView_Expand(h,i,c) (BOOL)SendMessageW((h), TVM_EXPAND, (WPARAM)(c), (LPARAM)(i))
#define TreeView_DeleteAllItems(h) (BOOL)SendMessageW((h), TVM_DELETEITEM, 0, (LPARAM)TVI_ROOT)
#define TreeView_GetSelection(h) (HTREEITEM)SendMessageW((h), TVM_GETNEXTITEM, TVGN_CARET, 0)
#define TreeView_SetBkColor(h,c) (COLORREF)SendMessageW((h), TVM_SETBKCOLOR, 0, (LPARAM)(c))
#define TreeView_SetTextColor(h,c) (COLORREF)SendMessageW((h), TVM_SETTEXTCOLOR, 0, (LPARAM)(c))
#define TreeView_SetLineColor(h,c) (COLORREF)SendMessageW((h), TVM_SETLINECOLOR, 0, (LPARAM)(c))

#define PROGRESS_CLASSW L"msctls_progress32"
#define PBM_SETRANGE32 0x0406
#define PBM_SETPOS     0x0402

// 0.7.3: tooltip on the Save Editor button
#define TOOLTIPS_CLASSW L"tooltips_class32"
#define TTS_ALWAYSTIP 0x01
#define TTF_IDISHWND 0x0001
#define TTF_SUBCLASS 0x0010
#define TTM_ADDTOOLW 0x0432
struct TOOLINFOW { UINT cbSize; UINT uFlags; HWND hwnd; UINT_PTR uId; RECT rect;
                   HINSTANCE hinst; LPWSTR lpszText; LPARAM lParam; void* lpReserved; };
#ifndef ICC_BAR_CLASSES
#define ICC_BAR_CLASSES 0x4
#endif
#ifndef ICC_PROGRESS_CLASS
#define ICC_PROGRESS_CLASS 0x20
#endif

// ---- trackbar (0.9.2 player) ----
#define TRACKBAR_CLASSW L"msctls_trackbar32"
#define TBS_HORZ 0x0000
#define TBS_NOTICKS 0x0010
#define TBM_GETPOS (WM_USER)
#define TBM_SETPOS (WM_USER + 5)
#define TBM_SETRANGEMIN (WM_USER + 7)
#define TBM_SETRANGEMAX (WM_USER + 8)
#define TB_THUMBPOSITION 4
#define TB_THUMBTRACK 5
#define TB_ENDTRACK 8

// ---- 0.9.2: player next / previous ----
#ifndef TVGN_NEXT
#define TVGN_NEXT 0x0001
#define TVGN_PREVIOUS 0x0002
#endif
#ifndef TVM_SELECTITEM
#define TVM_SELECTITEM 0x110B
#endif
#define TreeView_GetNextSibling(h,i) (HTREEITEM)SendMessageW((h), TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)(i))
#define TreeView_GetPrevSibling(h,i) (HTREEITEM)SendMessageW((h), TVM_GETNEXTITEM, TVGN_PREVIOUS, (LPARAM)(i))
#define TreeView_SelectItem(h,i) (BOOL)SendMessageW((h), TVM_SELECTITEM, TVGN_CARET, (LPARAM)(i))

#define NM_RCLICK (0U-5U)
#define TVM_HITTEST 0x1111
#define TVHT_ONITEM 0x0046
struct TVHITTESTINFO { POINT pt; UINT flags; HTREEITEM hItem; };

/* image lists for the tree's icons (1.1) */
typedef void* HIMAGELIST;
HIMAGELIST ImageList_Create(int, int, UINT, int, int);
int ImageList_Add(HIMAGELIST, HBITMAP, HBITMAP);
#define ILC_COLOR32 0x0020
#define TVSIL_NORMAL 0
#define TVIF_IMAGE 0x0002
#define TVIF_SELECTEDIMAGE 0x0020
HIMAGELIST TreeView_SetImageList(HWND, HIMAGELIST, int);
#ifndef TB_LINEUP
#define TB_LINEUP 0
#define TB_LINEDOWN 1
#define TB_PAGEUP 2
#define TB_PAGEDOWN 3
#endif
#ifndef TBM_SETRANGE
#define TBM_SETRANGE (WM_USER + 6)
#endif
#ifndef TVN_SELCHANGINGW
#define TVN_SELCHANGINGW (0U-401U)
#endif
#ifndef TVC_BYMOUSE
#define TVC_BYMOUSE 1
#endif
#ifndef TVIS_EXPANDED
#define TVIS_EXPANDED 0x20
#endif
#ifndef TreeView_GetRoot
#define TreeView_GetRoot(h) (HTREEITEM)SendMessageW((h), TVM_GETNEXTITEM, 0, 0)
#endif
#ifndef TreeView_GetItemState
#define TreeView_GetItemState(h,i,m) (UINT)SendMessageW((h), WM_USER + 39, (WPARAM)(i), (LPARAM)(m))
#endif
#ifndef NM_CUSTOMDRAW
#define NM_CUSTOMDRAW (0U-12U)
#endif
#ifndef CDDS_PREPAINT
#define CDDS_PREPAINT 0x1
#define CDDS_ITEMPREPAINT 0x10001
#define CDRF_DODEFAULT 0x0
#define CDRF_NOTIFYITEMDRAW 0x20
#endif
struct NMCUSTOMDRAW { NMHDR hdr; DWORD dwDrawStage; HDC hdc; RECT rc; ULONG_PTR dwItemSpec; UINT uItemState; LPARAM lItemlParam; };
struct NMTVCUSTOMDRAW { NMCUSTOMDRAW nmcd; COLORREF clrText; COLORREF clrTextBk; int iLevel; };
typedef NMTVCUSTOMDRAW* LPNMTVCUSTOMDRAW;
#ifndef TreeView_GetChild
#define TreeView_GetChild(h,i) (HTREEITEM)SendMessageW((h), TVM_GETNEXTITEM, 4, (LPARAM)(i))
#define TreeView_GetParent(h,i) (HTREEITEM)SendMessageW((h), TVM_GETNEXTITEM, 3, (LPARAM)(i))
#endif
