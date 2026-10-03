//go:build windows

package main

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

const appTitle = "Firemonkeys Save Editor  -  NFS No Limits / Real Racing 3"

var (
	user32   = syscall.NewLazyDLL("user32.dll")
	gdi32    = syscall.NewLazyDLL("gdi32.dll")
	kernel32 = syscall.NewLazyDLL("kernel32.dll")
	shell32  = syscall.NewLazyDLL("shell32.dll")
	ole32    = syscall.NewLazyDLL("ole32.dll")
	comctl32 = syscall.NewLazyDLL("comctl32.dll")

	pRegisterClassExW     = user32.NewProc("RegisterClassExW")
	pCreateWindowExW      = user32.NewProc("CreateWindowExW")
	pDefWindowProcW       = user32.NewProc("DefWindowProcW")
	pGetMessageW          = user32.NewProc("GetMessageW")
	pTranslateMessage     = user32.NewProc("TranslateMessage")
	pDispatchMessageW     = user32.NewProc("DispatchMessageW")
	pIsDialogMessageW     = user32.NewProc("IsDialogMessageW")
	pPostQuitMessage      = user32.NewProc("PostQuitMessage")
	pSendMessageW         = user32.NewProc("SendMessageW")
	pMessageBoxW          = user32.NewProc("MessageBoxW")
	pSetWindowTextW       = user32.NewProc("SetWindowTextW")
	pGetWindowTextW       = user32.NewProc("GetWindowTextW")
	pGetWindowTextLengthW = user32.NewProc("GetWindowTextLengthW")
	pShowWindow           = user32.NewProc("ShowWindow")
	pUpdateWindow         = user32.NewProc("UpdateWindow")
	pLoadCursorW          = user32.NewProc("LoadCursorW")
	pLoadIconW            = user32.NewProc("LoadIconW")
	pSetProcessDPIAware   = user32.NewProc("SetProcessDPIAware")
	pGetDC                = user32.NewProc("GetDC")
	pReleaseDC            = user32.NewProc("ReleaseDC")
	pEnableWindow         = user32.NewProc("EnableWindow")
	pAdjustWindowRect     = user32.NewProc("AdjustWindowRect")
	pInvalidateRect       = user32.NewProc("InvalidateRect")

	pCreateFontW     = gdi32.NewProc("CreateFontW")
	pGetDeviceCaps   = gdi32.NewProc("GetDeviceCaps")
	pGetStockObject  = gdi32.NewProc("GetStockObject")
	pGetModuleHandle = kernel32.NewProc("GetModuleHandleW")
	pCreateActCtxW   = kernel32.NewProc("CreateActCtxW")
	pActivateActCtx  = kernel32.NewProc("ActivateActCtx")

	pSHBrowseForFolderW   = shell32.NewProc("SHBrowseForFolderW")
	pSHGetPathFromIDListW = shell32.NewProc("SHGetPathFromIDListW")
	pDragAcceptFiles      = shell32.NewProc("DragAcceptFiles")
	pDragQueryFileW       = shell32.NewProc("DragQueryFileW")
	pDragFinish           = shell32.NewProc("DragFinish")
	pCoInitializeEx       = ole32.NewProc("CoInitializeEx")
	pCoTaskMemFree        = ole32.NewProc("CoTaskMemFree")
	pInitCommonControlsEx = comctl32.NewProc("InitCommonControlsEx")
)

const (
	WS_OVERLAPPED        = 0x00000000
	WS_CAPTION           = 0x00C00000
	WS_SYSMENU           = 0x00080000
	WS_MINIMIZEBOX       = 0x00020000
	WS_CHILD             = 0x40000000
	WS_VISIBLE           = 0x10000000
	WS_TABSTOP           = 0x00010000
	WS_VSCROLL           = 0x00200000
	WS_BORDER            = 0x00800000
	WS_CLIPCHILDREN      = 0x02000000
	WS_EX_CLIENTEDGE     = 0x00000200
	WS_EX_ACCEPTFILES    = 0x00000010
	ES_AUTOHSCROLL       = 0x0080
	ES_READONLY          = 0x0800
	ES_RIGHT             = 0x0002
	BS_PUSHBUTTON        = 0x0
	BS_DEFPUSHBUTTON     = 0x1
	BS_GROUPBOX          = 0x7
	SS_LEFT              = 0x0
	SS_RIGHT             = 0x2
	SS_CENTERIMAGE       = 0x200
	LBS_NOTIFY           = 0x0001
	LBS_NOINTEGRALHEIGHT = 0x0100
	LBS_USETABSTOPS      = 0x0080

	WM_CREATE         = 0x0001
	WM_DESTROY        = 0x0002
	WM_CLOSE          = 0x0010
	WM_SETFONT        = 0x0030
	WM_COMMAND        = 0x0111
	WM_SETREDRAW      = 0x000B
	WM_DROPFILES      = 0x0233
	WM_CTLCOLORSTATIC = 0x0138

	LB_ADDSTRING    = 0x0180
	LB_RESETCONTENT = 0x0184
	LB_GETCURSEL    = 0x0188
	LB_SETCURSEL    = 0x0186
	LB_SETITEMDATA  = 0x019A
	LB_GETITEMDATA  = 0x0199
	LB_SETTABSTOPS  = 0x0192
	LBN_SELCHANGE   = 1
	EN_CHANGE       = 0x0300
	BN_CLICKED      = 0
	EM_SETCUEBANNER = 0x1501

	MB_OK          = 0x0
	MB_ICONERROR   = 0x10
	MB_ICONWARNING = 0x30
	MB_ICONINFO    = 0x40
	MB_YESNO       = 0x4
	IDYES          = 6
)

type wndClassEx struct {
	cbSize        uint32
	style         uint32
	lpfnWndProc   uintptr
	cbClsExtra    int32
	cbWndExtra    int32
	hInstance     uintptr
	hIcon         uintptr
	hCursor       uintptr
	hbrBackground uintptr
	lpszMenuName  *uint16
	lpszClassName *uint16
	hIconSm       uintptr
}

type msgT struct {
	hwnd    uintptr
	message uint32
	wParam  uintptr
	lParam  uintptr
	time    uint32
	x, y    int32
	priv    uint32
}

type rectT struct{ l, t, r, b int32 }

type actCtx struct {
	cbSize                 uint32
	dwFlags                uint32
	lpSource               *uint16
	wProcessorArchitecture uint16
	wLangId                uint16
	lpAssemblyDirectory    *uint16
	lpResourceName         *uint16
	lpApplicationName      *uint16
	hModule                uintptr
}

type browseInfo struct {
	hwndOwner      uintptr
	pidlRoot       uintptr
	pszDisplayName *uint16
	lpszTitle      *uint16
	ulFlags        uint32
	lpfn           uintptr
	lParam         uintptr
	iImage         int32
}

func u16(s string) *uint16 { p, _ := syscall.UTF16PtrFromString(s); return p }

func send(h uintptr, m uint32, w, l uintptr) uintptr {
	r, _, _ := pSendMessageW.Call(h, uintptr(m), w, l)
	return r
}

func setText(h uintptr, s string) { pSetWindowTextW.Call(h, uintptr(unsafe.Pointer(u16(s)))) }

func getText(h uintptr) string {
	n, _, _ := pGetWindowTextLengthW.Call(h)
	buf := make([]uint16, n+1)
	pGetWindowTextW.Call(h, uintptr(unsafe.Pointer(&buf[0])), n+1)
	return syscall.UTF16ToString(buf)
}

func msgBox(owner uintptr, text, title string, flags uintptr) int {
	r, _, _ := pMessageBoxW.Call(owner, uintptr(unsafe.Pointer(u16(text))), uintptr(unsafe.Pointer(u16(title))), flags)
	return int(r)
}

func enable(h uintptr, on bool) {
	v := uintptr(0)
	if on {
		v = 1
	}
	pEnableWindow.Call(h, v)
}

// ------------------------------------------------------------------

type field interface {
	Name() string
	Value() string
	CanEdit() bool
	Set(string) error
}

type rrField struct{ r *Rec }

func (f rrField) Name() string       { return f.r.Name }
func (f rrField) Value() string      { return f.r.ValueString() }
func (f rrField) CanEdit() bool      { return f.r.Editable() }
func (f rrField) Set(s string) error { return f.r.SetFromString(s) }

type nfsField struct{ l *NLeaf }

func (f nfsField) Name() string       { return f.l.Path }
func (f nfsField) Value() string      { return f.l.Get() }
func (f nfsField) CanEdit() bool      { return f.l.Editable() }
func (f nfsField) Set(s string) error { return f.l.Set(s) }

const maxQuick = 14

type app struct {
	hwnd, font                 uintptr
	scale                      float64
	edFolder, btnOpen, lblGame uintptr
	quickLbl, quickEd          [maxQuick]uintptr
	btnCalib, lblHint          uintptr
	edFilter, list, edVal      uintptr
	btnApply, btnSave, lblStat uintptr
	grpQuick, grpAll           uintptr

	game  string // "rr3" | "nfs"
	rr    *RR3Save
	nfs   *NFSSave
	keys  map[string]uint64
	all   []field
	shown []field
	dirty bool
}

var A = &app{}

const (
	idOpen = 100 + iota
	idCalib
	idFilter
	idList
	idApply
	idSave
	idQuick0 = 200
)

func (a *app) px(v int) int32 { return int32(float64(v)*a.scale + 0.5) }

func (a *app) ctl(ex uint32, class, text string, style uint32, x, y, w, h int, id int) uintptr {
	hw, _, _ := pCreateWindowExW.Call(uintptr(ex), uintptr(unsafe.Pointer(u16(class))), uintptr(unsafe.Pointer(u16(text))),
		uintptr(style|WS_CHILD|WS_VISIBLE), uintptr(a.px(x)), uintptr(a.px(y)), uintptr(a.px(w)), uintptr(a.px(h)),
		a.hwnd, uintptr(id), 0, 0)
	send(hw, WM_SETFONT, a.font, 1)
	return hw
}

func (a *app) build() {
	a.ctl(0, "STATIC", "Save folder:", SS_LEFT|SS_CENTERIMAGE, 12, 14, 80, 24, 0)
	a.edFolder = a.ctl(WS_EX_CLIENTEDGE, "EDIT", "", ES_AUTOHSCROLL|ES_READONLY, 94, 14, 470, 24, 0)
	a.btnOpen = a.ctl(0, "BUTTON", "Open folder...", BS_PUSHBUTTON|WS_TABSTOP, 572, 13, 136, 26, idOpen)
	a.lblGame = a.ctl(0, "STATIC", "Click \"Open folder...\" (or drag a folder onto this window) and pick the folder with your save files.", SS_LEFT, 12, 46, 696, 20, 0)

	a.grpQuick = a.ctl(0, "BUTTON", "Quick edit", BS_GROUPBOX, 12, 70, 696, 262, 0)
	for i := 0; i < maxQuick; i++ {
		col, row := i%2, i/2
		x := 24 + col*344
		y := 94 + row*28
		a.quickLbl[i] = a.ctl(0, "STATIC", "", SS_RIGHT|SS_CENTERIMAGE, x, y, 150, 24, 0)
		a.quickEd[i] = a.ctl(WS_EX_CLIENTEDGE, "EDIT", "", ES_AUTOHSCROLL|WS_TABSTOP, x+158, y, 160, 24, idQuick0+i)
	}
	a.btnCalib = a.ctl(0, "BUTTON", "Calibrate from game", BS_PUSHBUTTON|WS_TABSTOP, 24, 294, 170, 28, idCalib)
	a.lblHint = a.ctl(0, "STATIC", "", SS_LEFT, 204, 292, 496, 36, 0)

	a.grpAll = a.ctl(0, "BUTTON", "All fields (advanced)", BS_GROUPBOX, 12, 340, 696, 290, 0)
	a.ctl(0, "STATIC", "Search:", SS_LEFT|SS_CENTERIMAGE, 24, 362, 56, 24, 0)
	a.edFilter = a.ctl(WS_EX_CLIENTEDGE, "EDIT", "", ES_AUTOHSCROLL|WS_TABSTOP, 82, 362, 614, 24, idFilter)
	send(a.edFilter, EM_SETCUEBANNER, 1, uintptr(unsafe.Pointer(u16("type part of a field name, e.g. Cash, Level, Balance, xp"))))
	a.list = a.ctl(WS_EX_CLIENTEDGE, "LISTBOX", "", LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|WS_VSCROLL|WS_TABSTOP|LBS_USETABSTOPS, 24, 392, 672, 196, idList)
	a.ctl(0, "STATIC", "Value:", SS_LEFT|SS_CENTERIMAGE, 24, 596, 56, 24, 0)
	a.edVal = a.ctl(WS_EX_CLIENTEDGE, "EDIT", "", ES_AUTOHSCROLL|WS_TABSTOP, 82, 596, 470, 24, 0)
	a.btnApply = a.ctl(0, "BUTTON", "Apply", BS_PUSHBUTTON|WS_TABSTOP, 560, 595, 136, 26, idApply)

	a.lblStat = a.ctl(0, "STATIC", "Backups are made automatically every time you save.", SS_LEFT|SS_CENTERIMAGE, 12, 642, 548, 28, 0)
	a.btnSave = a.ctl(0, "BUTTON", "Save changes", BS_PUSHBUTTON|WS_TABSTOP, 572, 640, 136, 32, idSave)
	a.setLoaded(false)
}

func (a *app) setLoaded(on bool) {
	for _, h := range []uintptr{a.edFilter, a.list, a.edVal, a.btnApply, a.btnSave} {
		enable(h, on)
	}
	if !on {
		for i := 0; i < maxQuick; i++ {
			setText(a.quickLbl[i], "")
			setText(a.quickEd[i], "")
			enable(a.quickEd[i], false)
		}
		pShowWindow.Call(a.btnCalib, 0)
	}
}

func (a *app) status(s string) { setText(a.lblStat, s) }

// ---------------- loading ----------------

func findSave(root string) (game, dir string) {
	type item struct {
		p string
		d int
	}
	q := []item{{root, 0}}
	for len(q) > 0 {
		it := q[0]
		q = q[1:]
		ents, err := os.ReadDir(it.p)
		if err != nil {
			continue
		}
		for _, e := range ents {
			n := e.Name()
			if !e.IsDir() {
				if strings.EqualFold(n, "character.2.dat") {
					return "rr3", it.p
				}
				if strings.HasSuffix(n, "_m.sb") || nfsSectionRe.MatchString(n) {
					return "nfs", it.p
				}
			}
		}
		if it.d < 4 {
			for _, e := range ents {
				if e.IsDir() && !strings.HasPrefix(e.Name(), "save_editor_backup_") {
					q = append(q, item{filepath.Join(it.p, e.Name()), it.d + 1})
				}
			}
		}
	}
	return "", ""
}

func (a *app) openFolder(root string) {
	if a.dirty && msgBox(a.hwnd, "You have unsaved changes. Open another folder anyway?", "Unsaved changes", MB_YESNO|MB_ICONWARNING) != IDYES {
		return
	}
	game, dir := findSave(root)
	if game == "" {
		msgBox(a.hwnd, "No supported save found in:\n"+root+
			"\n\nPick the folder that contains:\n  - NFS No Limits: <number>_m.sb and <number>_0.sb ... (folder \"saves-encrypted\")\n  - Real Racing 3: character.2.dat (folder \"doc\")", "Nothing found", MB_ICONWARNING)
		return
	}
	a.rr, a.nfs, a.all, a.dirty = nil, nil, nil, false
	a.setLoaded(false)
	setText(a.edFolder, dir)
	var err error
	if game == "rr3" {
		a.rr, err = LoadRR3(filepath.Join(dir, "character.2.dat"))
	} else {
		a.nfs, err = LoadNFS(dir)
	}
	if err != nil {
		setText(a.lblGame, "Could not load the save.")
		msgBox(a.hwnd, "Could not read the save:\n\n"+err.Error(), "Error", MB_ICONERROR)
		return
	}
	a.game = game
	a.fillAll()
	a.fillQuick()
	a.setLoaded(true)
	if game == "rr3" {
		setText(a.lblGame, fmt.Sprintf("Real Racing 3  -  character.2.dat  (%d fields)", len(a.all)))
	} else {
		setText(a.lblGame, fmt.Sprintf("Need for Speed No Limits  -  player %s, %d section files  (%d fields)", a.nfs.UID, len(a.nfs.Files), len(a.all)))
	}
	a.status("Loaded. Change values, then click \"Save changes\". Close the game first!")
}

func (a *app) fillAll() {
	a.all = nil
	if a.rr != nil {
		for _, r := range a.rr.SortedRoot() {
			if r.Type() == tRef || r.Name == "" {
				continue
			}
			a.all = append(a.all, rrField{r})
		}
	} else if a.nfs != nil {
		for _, l := range a.nfs.Leaves {
			a.all = append(a.all, nfsField{l})
		}
	}
	a.refilter()
}

func (a *app) refilter() {
	f := strings.ToLower(strings.TrimSpace(getText(a.edFilter)))
	a.shown = a.shown[:0]
	for _, x := range a.all {
		if f == "" || strings.Contains(strings.ToLower(x.Name()), f) {
			a.shown = append(a.shown, x)
		}
	}
	send(a.list, WM_SETREDRAW, 0, 0)
	send(a.list, LB_RESETCONTENT, 0, 0)
	for _, x := range a.shown {
		v := x.Value()
		if len(v) > 60 {
			v = v[:57] + "..."
		}
		lock := ""
		if !x.CanEdit() {
			lock = "   (read-only)"
		}
		send(a.list, LB_ADDSTRING, 0, uintptr(unsafe.Pointer(u16(x.Name()+"  =  "+v+lock))))
	}
	send(a.list, WM_SETREDRAW, 1, 0)
	pInvalidateRect.Call(a.list, 0, 1)
	setText(a.edVal, "")
}

func (a *app) selected() field {
	i := int(send(a.list, LB_GETCURSEL, 0, 0))
	if i < 0 || i >= len(a.shown) {
		return nil
	}
	return a.shown[i]
}

func (a *app) applyAdvanced() {
	if err := a.applyQuick(); err != nil { // keep what was typed in Quick edit
		msgBox(a.hwnd, err.Error(), "Quick edit", MB_ICONWARNING)
		return
	}
	f := a.selected()
	if f == nil {
		msgBox(a.hwnd, "Select a field in the list first.", "Apply", MB_ICONINFO)
		return
	}
	if !f.CanEdit() {
		msgBox(a.hwnd, "This field is read-only in the editor.", "Apply", MB_ICONINFO)
		return
	}
	if err := f.Set(getText(a.edVal)); err != nil {
		msgBox(a.hwnd, err.Error(), "Invalid value", MB_ICONWARNING)
		return
	}
	a.dirty = true
	i := send(a.list, LB_GETCURSEL, 0, 0)
	a.refilter()
	send(a.list, LB_SETCURSEL, i, 0)
	if s := a.selected(); s != nil {
		setText(a.edVal, s.Value())
	}
	a.fillQuick()
	a.status("Changed " + f.Name() + " (not saved yet).")
}

// ---------------- quick fields ----------------

func (a *app) fillQuick() {
	for i := 0; i < maxQuick; i++ {
		setText(a.quickLbl[i], "")
		setText(a.quickEd[i], "")
		enable(a.quickEd[i], false)
	}
	if a.nfs != nil {
		pShowWindow.Call(a.btnCalib, 0)
		setText(a.lblHint, "")
		i := 0
		for _, m := range nfsMain {
			l := a.nfs.ByPath[m.Path]
			if l == nil || i >= maxQuick {
				continue
			}
			setText(a.quickLbl[i], m.Label+":")
			setText(a.quickEd[i], l.Get())
			enable(a.quickEd[i], true)
			i++
		}
		return
	}
	if a.rr == nil {
		return
	}
	pShowWindow.Call(a.btnCalib, 5)
	calibrated := false
	for i, h := range rr3Hidden {
		setText(a.quickLbl[i], h.Label+":")
		enable(a.quickEd[i], true)
		if v, ok := a.rr.HiddenGet(h, a.keys); ok {
			setText(a.quickEd[i], strconv.FormatUint(v, 10))
			calibrated = true
		}
	}
	n := len(rr3Hidden)
	for _, p := range rr3Plain {
		if r := a.rr.ByName[p.Field]; r != nil && n < maxQuick {
			setText(a.quickLbl[n], p.Label+":")
			setText(a.quickEd[n], r.ValueString())
			enable(a.quickEd[n], true)
			n++
		}
	}
	if calibrated {
		setText(a.lblHint, "RR3 hides money/level with a per-field key. Values shown use your saved calibration. If they don't match the game, type the in-game numbers and click Calibrate again.")
	} else {
		setText(a.lblHint, "One-time step: type the R$, Gold, M$ and level the game shows NOW into the boxes above, then click \"Calibrate from game\". After that you can edit them.")
	}
}

type plainQuick struct{ Label, Field string }

var rr3Plain = []plainQuick{
	{"XP total earned", "m_xp.m_totalEarned"},
	{"Level progress (0-1)", "m_xp.m_currentDriverLevelProgress"},
	{"Races won in a row", "m_raceStats.m_iWonRacesInARow"},
}

func (a *app) calibrate() {
	if a.rr == nil {
		return
	}
	got := 0
	var errs []string
	for i, h := range rr3Hidden {
		t := strings.TrimSpace(getText(a.quickEd[i]))
		if t == "" {
			continue
		}
		v, err := parseNum(t)
		if err != nil {
			errs = append(errs, h.Label+": "+err.Error())
			continue
		}
		if err := a.rr.Calibrate(h, a.keys, v); err != nil {
			errs = append(errs, err.Error())
			continue
		}
		got++
	}
	if got == 0 {
		msgBox(a.hwnd, "Type the values the game shows right now (at least R$) into the boxes, then click Calibrate.\n\n"+strings.Join(errs, "\n"), "Calibrate", MB_ICONINFO)
		return
	}
	// the three wallets share one key in the game code: fill in any that were left empty
	for _, name := range []string{"rdollars", "gold", "mdollars"} {
		if _, ok := a.keys[name]; ok {
			for _, o := range []string{"rdollars", "gold", "mdollars"} {
				if _, ok2 := a.keys[o]; !ok2 {
					a.keys[o] = a.keys[name]
				}
			}
			break
		}
	}
	saveKeys(a.keys)
	a.fillQuick()
	msg := "Calibration saved. It is reused next time, so you only do this once.\n\nCheck that every number now matches the game. Then type the new values and click \"Save changes\"."
	if len(errs) > 0 {
		msg += "\n\nProblems:\n" + strings.Join(errs, "\n")
	}
	msgBox(a.hwnd, msg, "Calibrate", MB_ICONINFO)
}

func (a *app) applyQuick() error {
	if a.nfs != nil {
		i := 0
		for _, m := range nfsMain {
			l := a.nfs.ByPath[m.Path]
			if l == nil || i >= maxQuick {
				continue
			}
			t := strings.TrimSpace(getText(a.quickEd[i]))
			i++
			if t == "" || t == l.Get() {
				continue
			}
			if err := a.nfs.SetMain(m, t); err != nil {
				return err
			}
			a.dirty = true
		}
		return nil
	}
	if a.rr == nil {
		return nil
	}
	for i, h := range rr3Hidden {
		t := strings.TrimSpace(getText(a.quickEd[i]))
		cur, ok := a.rr.HiddenGet(h, a.keys)
		if t == "" || (ok && t == strconv.FormatUint(cur, 10)) {
			continue
		}
		if !ok {
			return fmt.Errorf("%s is not calibrated yet. Type the current in-game values and click \"Calibrate from game\" first", h.Label)
		}
		v, err := parseNum(t)
		if err != nil {
			return fmt.Errorf("%s: %v", h.Label, err)
		}
		if h.Label == "Driver level" && (v < 1 || v > 999) {
			return fmt.Errorf("driver level must be between 1 and 999")
		}
		if err := a.rr.HiddenSet(h, a.keys, v); err != nil {
			return err
		}
		a.dirty = true
	}
	n := len(rr3Hidden)
	for _, p := range rr3Plain {
		r := a.rr.ByName[p.Field]
		if r == nil || n >= maxQuick {
			continue
		}
		t := strings.TrimSpace(getText(a.quickEd[n]))
		n++
		if t == "" || t == r.ValueString() {
			continue
		}
		if err := r.SetFromString(t); err != nil {
			return fmt.Errorf("%s: %v", p.Label, err)
		}
		a.dirty = true
	}
	return nil
}

func (a *app) save() {
	if err := a.applyQuick(); err != nil {
		msgBox(a.hwnd, err.Error(), "Cannot save", MB_ICONWARNING)
		return
	}
	if !a.dirty {
		msgBox(a.hwnd, "Nothing was changed.", "Save", MB_ICONINFO)
		return
	}
	stamp := time.Now().Format("20060102_150405")
	var bdir string
	var err error
	if a.rr != nil {
		bdir, err = a.rr.Save(stamp)
	} else {
		bdir, err = a.nfs.Save(stamp)
	}
	if err != nil {
		msgBox(a.hwnd, "Save failed:\n\n"+err.Error(), "Error", MB_ICONERROR)
		return
	}
	a.dirty = false
	folder := getText(a.edFolder)
	a.dirty = false
	a.openFolderNoPrompt(folder)
	a.status("Saved. Backup: " + filepath.Base(bdir))
	msgBox(a.hwnd, "Saved successfully.\n\nOriginal files were backed up to:\n"+bdir+
		"\n\nCopy the edited files back to the game folder on your device (with the game closed). Turn off cloud save sync or the server copy may overwrite your edit.", "Saved", MB_ICONINFO)
}

func (a *app) openFolderNoPrompt(dir string) {
	a.dirty = false
	filter := getText(a.edFilter)
	a.openFolder(dir)
	setText(a.edFilter, filter)
}

// ---------------- keys storage (RR3 calibration) ----------------

func keysPath() string {
	d, err := os.UserConfigDir()
	if err != nil {
		d = "."
	}
	return filepath.Join(d, "FiremonkeysSaveEditor", "rr3_keys.txt")
}

func loadKeys() map[string]uint64 {
	k := map[string]uint64{}
	for n, v := range rr3DefaultKeys {
		k[n] = v
	}
	b, err := os.ReadFile(keysPath())
	if err != nil {
		return k
	}
	for _, line := range strings.Split(string(b), "\n") {
		p := strings.SplitN(strings.TrimSpace(line), "=", 2)
		if len(p) == 2 {
			if v, err := strconv.ParseUint(strings.TrimSpace(p[1]), 16, 64); err == nil {
				k[strings.TrimSpace(p[0])] = v
			}
		}
	}
	return k
}

func saveKeys(k map[string]uint64) {
	os.MkdirAll(filepath.Dir(keysPath()), 0755)
	var names []string
	for n := range k {
		names = append(names, n)
	}
	sort.Strings(names)
	var sb strings.Builder
	sb.WriteString("# Real Racing 3 hidden-value keys (made by Calibrate)\n")
	for _, n := range names {
		fmt.Fprintf(&sb, "%s=%016x\n", n, k[n])
	}
	os.WriteFile(keysPath(), []byte(sb.String()), 0644)
}

// ---------------- window procedure ----------------

func wndProc(hwnd uintptr, m uint32, w, l uintptr) uintptr {
	switch m {
	case WM_COMMAND:
		id, code := int(w&0xffff), int(w>>16)
		switch {
		case id == idOpen && code == BN_CLICKED:
			if d := browseFolder(hwnd); d != "" {
				A.openFolder(d)
			}
		case id == idCalib && code == BN_CLICKED:
			A.calibrate()
		case id == idSave && code == BN_CLICKED:
			A.save()
		case id == idApply && code == BN_CLICKED:
			A.applyAdvanced()
		case id == idFilter && code == EN_CHANGE:
			A.refilter()
		case id == idList && code == LBN_SELCHANGE:
			if f := A.selected(); f != nil {
				setText(A.edVal, f.Value())
				enable(A.edVal, f.CanEdit())
				enable(A.btnApply, f.CanEdit())
			}
		}
		return 0
	case WM_DROPFILES:
		buf := make([]uint16, 1024)
		pDragQueryFileW.Call(w, 0, uintptr(unsafe.Pointer(&buf[0])), 1024)
		pDragFinish.Call(w)
		p := syscall.UTF16ToString(buf)
		if st, err := os.Stat(p); err == nil && !st.IsDir() {
			p = filepath.Dir(p)
		}
		A.openFolder(p)
		return 0
	case WM_CLOSE:
		if A.dirty && msgBox(hwnd, "You have unsaved changes. Quit anyway?", "Unsaved changes", MB_YESNO|MB_ICONWARNING) != IDYES {
			return 0
		}
	case WM_DESTROY:
		pPostQuitMessage.Call(0)
		return 0
	}
	r, _, _ := pDefWindowProcW.Call(hwnd, uintptr(m), w, l)
	return r
}

func browseFolder(owner uintptr) string {
	name := make([]uint16, 260)
	bi := browseInfo{hwndOwner: owner, pszDisplayName: &name[0],
		lpszTitle: u16("Pick the folder with the save files (NFS: saves-encrypted, RR3: doc). A parent folder works too."),
		ulFlags:   0x1 | 0x10 | 0x40}
	pidl, _, _ := pSHBrowseForFolderW.Call(uintptr(unsafe.Pointer(&bi)))
	if pidl == 0 {
		return ""
	}
	defer pCoTaskMemFree.Call(pidl)
	path := make([]uint16, 1024)
	pSHGetPathFromIDListW.Call(pidl, uintptr(unsafe.Pointer(&path[0])))
	return syscall.UTF16ToString(path)
}

const manifestXML = `<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
<dependency><dependentAssembly><assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.0.0" processorArchitecture="*" publicKeyToken="6595b64144ccf1df" language="*"/></dependentAssembly></dependency>
</assembly>`

func enableVisualStyles() {
	f := filepath.Join(os.TempDir(), "fmsaveeditor.manifest")
	if os.WriteFile(f, []byte(manifestXML), 0644) != nil {
		return
	}
	ac := actCtx{lpSource: u16(f)}
	ac.cbSize = uint32(unsafe.Sizeof(ac))
	h, _, _ := pCreateActCtxW.Call(uintptr(unsafe.Pointer(&ac)))
	if h != 0 && h != ^uintptr(0) {
		var cookie uintptr
		pActivateActCtx.Call(h, uintptr(unsafe.Pointer(&cookie)))
	}
	icc := struct{ size, icc uint32 }{8, 0xFF}
	pInitCommonControlsEx.Call(uintptr(unsafe.Pointer(&icc)))
}

func main() {
	runtime.LockOSThread()
	pSetProcessDPIAware.Call()
	pCoInitializeEx.Call(0, 2)
	enableVisualStyles()

	hdc, _, _ := pGetDC.Call(0)
	dpi, _, _ := pGetDeviceCaps.Call(hdc, 88)
	pReleaseDC.Call(0, hdc)
	if dpi == 0 {
		dpi = 96
	}
	A.scale = float64(dpi) / 96
	A.keys = loadKeys()
	A.font, _, _ = pCreateFontW.Call(uintptr(int32(-int(9*float64(dpi)/72+0.5))), 0, 0, 0, 400, 0, 0, 0, 1, 0, 0, 5, 0, uintptr(unsafe.Pointer(u16("Segoe UI"))))

	hinst, _, _ := pGetModuleHandle.Call(0)
	cls := u16("FMSaveEditorWnd")
	cursor, _, _ := pLoadCursorW.Call(0, 32512)
	icon, _, _ := pLoadIconW.Call(0, 32512)
	wc := wndClassEx{lpfnWndProc: syscall.NewCallback(wndProc), hInstance: hinst, hCursor: cursor, hIcon: icon, hIconSm: icon,
		hbrBackground: 15 + 1, lpszClassName: cls}
	wc.cbSize = uint32(unsafe.Sizeof(wc))
	pRegisterClassExW.Call(uintptr(unsafe.Pointer(&wc)))

	style := uint32(WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN)
	rc := rectT{0, 0, A.px(720), A.px(684)}
	pAdjustWindowRect.Call(uintptr(unsafe.Pointer(&rc)), uintptr(style), 0)
	A.hwnd, _, _ = pCreateWindowExW.Call(WS_EX_ACCEPTFILES, uintptr(unsafe.Pointer(cls)), uintptr(unsafe.Pointer(u16(appTitle))),
		uintptr(style), 0x80000000, 0x80000000, uintptr(rc.r-rc.l), uintptr(rc.b-rc.t), 0, 0, hinst, 0)
	if A.hwnd == 0 {
		msgBox(0, "Could not create the window.", "Error", MB_ICONERROR)
		return
	}
	A.build()
	pDragAcceptFiles.Call(A.hwnd, 1)
	pShowWindow.Call(A.hwnd, 1)
	pUpdateWindow.Call(A.hwnd)

	if len(os.Args) > 1 {
		A.openFolder(os.Args[1])
	}

	var m msgT
	for {
		r, _, _ := pGetMessageW.Call(uintptr(unsafe.Pointer(&m)), 0, 0, 0)
		if r == 0 || int32(r) == -1 {
			break
		}
		if ok, _, _ := pIsDialogMessageW.Call(A.hwnd, uintptr(unsafe.Pointer(&m))); ok != 0 {
			continue
		}
		pTranslateMessage.Call(uintptr(unsafe.Pointer(&m)))
		pDispatchMessageW.Call(uintptr(unsafe.Pointer(&m)))
	}
}
