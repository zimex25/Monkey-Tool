// stub of the Media Foundation player interface, for the syntax check only
#pragma once
#include <windows.h>
#include <objbase.h>
#ifndef MT_STUB_GUID
#define MT_STUB_GUID
struct GUID { unsigned long Data1; unsigned short Data2, Data3; unsigned char Data4[8]; };
typedef GUID IID;
typedef GUID CLSID;
#endif
#ifndef VT_I8
#define VT_I8 20
#define VT_UI8 21
#endif
typedef union { struct { DWORD LowPart; LONG HighPart; } u; LONGLONG QuadPart; } MT_LARGE_INTEGER;
typedef union { unsigned long long QuadPart; } MT_ULARGE_INTEGER;
typedef struct tagPROPVARIANT { unsigned short vt; unsigned short r1, r2, r3; union { MT_LARGE_INTEGER hVal; MT_ULARGE_INTEGER uhVal; }; } PROPVARIANT;
inline void PropVariantInit(PROPVARIANT* p) { p->vt = 0; }
HRESULT PropVariantClear(PROPVARIANT*);
typedef enum { MFP_MEDIAPLAYER_STATE_EMPTY = 0, MFP_MEDIAPLAYER_STATE_STOPPED, MFP_MEDIAPLAYER_STATE_PLAYING,
               MFP_MEDIAPLAYER_STATE_PAUSED, MFP_MEDIAPLAYER_STATE_SHUTDOWN } MFP_MEDIAPLAYER_STATE;
typedef UINT32 MFP_CREATION_OPTIONS;
enum { MFP_OPTION_NONE = 0 };
struct IMFPMediaPlayerCallback;
struct IMFPMediaPlayer {
    virtual HRESULT QueryInterface(const GUID&, void**) = 0;
    virtual ULONG AddRef() = 0;
    virtual ULONG Release() = 0;
    virtual HRESULT Play() = 0;
    virtual HRESULT Pause() = 0;
    virtual HRESULT Stop() = 0;
    virtual HRESULT SetPosition(const GUID&, const PROPVARIANT*) = 0;
    virtual HRESULT GetPosition(const GUID&, PROPVARIANT*) = 0;
    virtual HRESULT GetDuration(const GUID&, PROPVARIANT*) = 0;
    virtual HRESULT GetState(MFP_MEDIAPLAYER_STATE*) = 0;
    virtual HRESULT UpdateVideo() = 0;
    virtual HRESULT Shutdown() = 0;
};
