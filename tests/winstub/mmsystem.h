// stub of <mmsystem.h> - only the playback call this project uses
#pragma once
#include <windows.h>

#define SND_SYNC      0x0000
#define SND_ASYNC     0x0001
#define SND_NODEFAULT 0x0002
#define SND_MEMORY    0x0004
#define SND_PURGE     0x0040

extern "C" BOOL PlaySoundW(LPCWSTR, HMODULE, DWORD);

// ---- waveOut playback (0.9.2 player) ----
typedef struct HWAVEOUT__* HWAVEOUT;
typedef UINT MMRESULT;
typedef struct tWAVEFORMATEX {
    WORD  wFormatTag; WORD nChannels; DWORD nSamplesPerSec; DWORD nAvgBytesPerSec;
    WORD  nBlockAlign; WORD wBitsPerSample; WORD cbSize;
} WAVEFORMATEX;
typedef struct wavehdr_tag {
    LPSTR lpData; DWORD dwBufferLength; DWORD dwBytesRecorded; ULONG_PTR dwUser;
    DWORD dwFlags; DWORD dwLoops; struct wavehdr_tag* lpNext; ULONG_PTR reserved;
} WAVEHDR;
typedef struct mmtime_tag { UINT wType; union { DWORD ms; DWORD sample; DWORD cb; } u; } MMTIME;
#define WAVE_MAPPER ((UINT)-1)
#define CALLBACK_NULL 0x00000000
#define MMSYSERR_NOERROR 0
#define WHDR_DONE 0x00000001
#define TIME_BYTES 0x0004
#define WAVE_FORMAT_PCM 1
extern "C" MMRESULT waveOutOpen(HWAVEOUT*, UINT, const WAVEFORMATEX*, ULONG_PTR, ULONG_PTR, DWORD);
extern "C" MMRESULT waveOutPrepareHeader(HWAVEOUT, WAVEHDR*, UINT);
extern "C" MMRESULT waveOutUnprepareHeader(HWAVEOUT, WAVEHDR*, UINT);
extern "C" MMRESULT waveOutWrite(HWAVEOUT, WAVEHDR*, UINT);
extern "C" MMRESULT waveOutReset(HWAVEOUT);
extern "C" MMRESULT waveOutClose(HWAVEOUT);
extern "C" MMRESULT waveOutPause(HWAVEOUT);
extern "C" MMRESULT waveOutRestart(HWAVEOUT);
extern "C" MMRESULT waveOutGetPosition(HWAVEOUT, MMTIME*, UINT);
