// Shared resource IDs for the .rc file and the C++ source.
//
// These MUST be numeric and visible to BOTH. An identifier the resource
// compiler has never seen is treated as a *string-named* resource, not an
// integer ID - so LoadImage(MAKEINTRESOURCE(101)) finds nothing and the
// window shows no icon. That was the icon bug.
#pragma once
#define IDI_APPICON 101

// Game logos for the profile picker, stored as PNG bytes (RCDATA) and
// decoded at runtime by the tool's own PNG reader.
#define IDR_LOGO_NFSNL  201
#define IDR_LOGO_RR1    202
#define IDR_LOGO_RR2    203
#define IDR_LOGO_RR3    204
#define IDR_LOGO_RRNEXT 205
#define IDR_LOGO_NFSHP  206
#define IDR_LOGO_NFSMW  207
#define IDR_LOGO_NFSEDGE 208
#define IDR_LOGO_RRGTI  209
#define IDR_LOGO_NFSNLVR 210
#define IDR_LOGO_NFSUC  211
#define IDR_LOGO_NFSSHIFT 212
#define IDR_LOGO_NFSSHIFT2 213

// the game picker's icons and the loading screen's art, per game
#define IDR_ICON_NFSHP 301
#define IDR_ICON_NFSMW 302
#define IDR_ICON_NFSNL 303
#define IDR_ICON_NFSNLVR 304
#define IDR_ICON_NFSEDGE 305
#define IDR_ICON_RR1 306
#define IDR_ICON_RRGTI 307
#define IDR_ICON_RR2 308
#define IDR_ICON_RR3 309
#define IDR_ICON_RRNEXT 310
#define IDR_ICON_NFSUC 311
#define IDR_ICON_NFSSHIFT 312
#define IDR_ICON_NFSSHIFT2 313
#define IDR_SPLASH_NFSHP 321
#define IDR_SPLASH_NFSMW 322
#define IDR_SPLASH_NFSNL 323
#define IDR_SPLASH_NFSNLVR 324
#define IDR_SPLASH_NFSEDGE 325
#define IDR_SPLASH_RR1 326
#define IDR_SPLASH_RRGTI 327
#define IDR_SPLASH_RR2 328
#define IDR_SPLASH_RR3 329
#define IDR_SPLASH_RRNEXT 330
#define IDR_SPLASH_NFSUC 331
#define IDR_SPLASH_NFSSHIFT 332
#define IDR_SPLASH_NFSSHIFT2 333
#define IDR_APPICON_BIG 340
#define IDR_TREEICONS 341
#define IDR_TOOLBARICONS 342
