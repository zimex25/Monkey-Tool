#pragma once
// Stub: the part of the Windows Imaging Component the tool calls.
#include <windows.h>
#include <objbase.h>
#ifndef MT_STUB_GUID
#define MT_STUB_GUID
struct GUID { unsigned long Data1; unsigned short Data2, Data3; unsigned char Data4[8]; };
typedef GUID IID;
typedef GUID CLSID;
#endif
#ifndef SUCCEEDED
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#endif
#define CLSCTX_INPROC_SERVER 1
enum WICDecodeOptions { WICDecodeMetadataCacheOnDemand = 0 };
enum WICBitmapDitherType { WICBitmapDitherTypeNone = 0 };
enum WICBitmapPaletteType { WICBitmapPaletteTypeCustom = 0 };
struct WICRect;
struct IWICPalette;
struct IUnknownStub { virtual unsigned long Release() = 0; };
struct IWICStream : IUnknownStub {
    virtual HRESULT InitializeFromMemory(BYTE*, DWORD) = 0;
};
struct IWICBitmapSource : IUnknownStub {
    virtual HRESULT GetSize(UINT*, UINT*) = 0;
    virtual HRESULT CopyPixels(const WICRect*, UINT, UINT, BYTE*) = 0;
};
struct IWICBitmapFrameDecode : IWICBitmapSource {};
struct IWICBitmapDecoder : IUnknownStub {
    virtual HRESULT GetFrame(UINT, IWICBitmapFrameDecode**) = 0;
};
struct IWICFormatConverter : IWICBitmapSource {
    virtual HRESULT Initialize(IWICBitmapSource*, const GUID&, WICBitmapDitherType, IWICPalette*,
                               double, WICBitmapPaletteType) = 0;
};
struct IWICImagingFactory : IUnknownStub {
    virtual HRESULT CreateStream(IWICStream**) = 0;
    virtual HRESULT CreateDecoderFromStream(IWICStream*, const GUID*, WICDecodeOptions,
                                            IWICBitmapDecoder**) = 0;
    virtual HRESULT CreateFormatConverter(IWICFormatConverter**) = 0;
};
extern "C" HRESULT CoCreateInstance(const GUID&, void*, DWORD, const GUID&, void**);
