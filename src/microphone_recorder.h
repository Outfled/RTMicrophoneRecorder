#pragma once

#include <Windows.h>
#include <mmsystem.h>

/* Key to press to stop recording */
#define STOP_REC_KEY    VK_ESCAPE

DWORD RecordMicrophoneAudio(LPCWSTR lpszOutputFile);
