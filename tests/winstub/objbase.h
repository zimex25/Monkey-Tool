#pragma once
#include <windows.h>
#define COINIT_APARTMENTTHREADED 2
extern "C" {
HRESULT CoInitializeEx(LPVOID, DWORD);
void CoUninitialize();
void CoTaskMemFree(LPVOID);
}
