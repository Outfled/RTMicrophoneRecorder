#include "microphone_recorder.h"
#include <mmsystem.h>
#include <PathCch.h>
#include <fstream>
#include <cmath>


#pragma comment ( lib, "winmm.lib" )
#pragma comment ( lib, "PathCch.lib" )


/* Audio sample rate */
#define SAMPLE_RATE         44100

#define AUDIO_HDR_COUNT     2
#define AUDIO_BUF_COUNT     3


static VOID GenerateTempFilePath(WCHAR *, SIZE_T);
static VOID ConvertDumpToWAV(LPCWSTR, LPCWSTR, WAVEFORMATEX);
static DWORD WINAPI UpdateElapsedThread(LPVOID);

static char g_AudioBuffers[AUDIO_HDR_COUNT][SAMPLE_RATE * 2 * 2 / 2];
static int  g_nElapsedSeconds;

#pragma warning ( push )
#pragma warning ( disable : 6258 ) // "Using TerminateThread does not allow proper thread clean up"
DWORD RecordMicrophoneAudio(LPCWSTR lpszOutputFile)
{
    HANDLE          hThread;
    DWORD           dwResult;
    HWAVEIN         hWaveIn;
    WAVEHDR         wHeader[AUDIO_HDR_COUNT]{};
    WAVEFORMATEX    wFormat{};
    WCHAR           szDumpFile[MAX_PATH];
    std::fstream    fRawAudio;

    g_nElapsedSeconds = 0;

    /* Create (suspended) thread for updating the elapsed recording time */
    hThread = CreateThread(NULL, 0, UpdateElapsedThread, NULL, CREATE_SUSPENDED, NULL);
    if (hThread == NULL || hThread == INVALID_HANDLE_VALUE)
    {
        wprintf(L"Error Occurred: %d\n", GetLastError());
        return GetLastError();
    }

    /* Set wave audio format data */
    wFormat.wFormatTag      = WAVE_FORMAT_PCM;		/* Simple & Uncrompressed Format */
    wFormat.nChannels       = 1;					/* Mono */
    wFormat.nSamplesPerSec  = SAMPLE_RATE;		    /* Sample Rate */
    wFormat.nAvgBytesPerSec = SAMPLE_RATE * 2;	    /* nSamplesPerSec * n.Channels * wBitsPerSample / 8	*/
    wFormat.nBlockAlign     = 2;				    /* n.Channels * wBitsPerSample / 8	*/
    wFormat.wBitsPerSample  = 16;					/* 16 for High Quality, 8 for Telephone-Grade	*/
    wFormat.cbSize          = 0;

    /* Open the default microphone wave */
    dwResult = waveInOpen(&hWaveIn, WAVE_MAPPER, &wFormat, NULL, NULL, CALLBACK_NULL | WAVE_FORMAT_DIRECT);
    if (dwResult != NO_ERROR)
    {
        wprintf(L"[waveInOpen] Error Occurred: %d\n", dwResult);
        return dwResult;
    }

    /* Initialize the audio headers */
    for (int cur = 0; cur < AUDIO_HDR_COUNT; ++cur)
    {
        wHeader[cur].lpData         = g_AudioBuffers[cur];
        wHeader[cur].dwBufferLength = SAMPLE_RATE * 2 * 2 / 2;

        /* Prepare header */
          dwResult = waveInPrepareHeader(hWaveIn, &wHeader[cur], sizeof(wHeader[cur]));
        if (dwResult != NO_ERROR)
        {
            wprintf(L"[waveInPrepareHeader] Error Occurred: %d\n", dwResult);
            return dwResult;
        }

        /* Add header to queue */
        dwResult = waveInAddBuffer(hWaveIn, &wHeader[cur], sizeof(wHeader[cur]));
        if (dwResult != NO_ERROR)
        {
            wprintf(L"[waveInAddBuffer] Error Occurred: %d\n", dwResult);
            return dwResult;
        }
    }

    /* Generate a temporary file for writing raw audio bytes */
    GenerateTempFilePath(szDumpFile, ARRAYSIZE(szDumpFile));
    fRawAudio.open(szDumpFile, std::ios_base::out | std::ios_base::binary);

    /* Start recording */
    dwResult = waveInStart(hWaveIn);
    if (dwResult == NO_ERROR)
    {
        ResumeThread(hThread);

        wprintf(L"Recording has started. Press escape key to stop\n");

        /* Loop until user presses escape */
        while (0 == (GetAsyncKeyState(STOP_REC_KEY) & 0x8000))
        {
            /* Enum each audio header */
            for (auto &hdr : wHeader)
            {
                /* Check if header is full */
                if (hdr.dwFlags & WHDR_DONE)
                {
                    /* 
                    * The current header is full.
                    * Write the header bytes to the dump file & then re-add header
                    */
                    fRawAudio.write(hdr.lpData, hdr.dwBufferLength);

                    hdr.dwFlags         = 0;    // Clear the WHDR_DONE flag
                    hdr.dwBytesRecorded = 0;    // Set the number of recorded bytes to 0

                    /* Prepare & add header */
                    waveInPrepareHeader(hWaveIn, &hdr, sizeof(hdr));
                    waveInAddBuffer(hWaveIn, &hdr, sizeof(hdr));
                }
            }
        }

        /* Cleanup */
        waveInStop(hWaveIn);
        fRawAudio.close();

        TerminateThread(hThread, 0);
        ++g_nElapsedSeconds;

        /* Convert the temp file to a .wav file */
        ConvertDumpToWAV(lpszOutputFile, szDumpFile, wFormat);
        DeleteFile(szDumpFile);

        wprintf(L"Recording has been saved to %s\n", lpszOutputFile);
    }

    /* Release headers */
    for (int h = 0; h < AUDIO_HDR_COUNT; ++h)
    {
        waveInUnprepareHeader(hWaveIn, &wHeader[h], sizeof(wHeader[h]));
    }

    waveInClose(hWaveIn);
    if (fRawAudio.is_open()) {
        fRawAudio.close();
    }

    return ERROR_SUCCESS;
}
#pragma warning ( pop )


VOID GenerateTempFilePath(WCHAR *pszBuffer, SIZE_T cchBuffer)
{
    WCHAR   szDirectory[MAX_PATH];
    WCHAR   szFileName[MAX_PATH];
    BOOL    bResult;

    ZeroMemory(pszBuffer, cchBuffer * sizeof(WCHAR));

    /* Get the current user temp. file path */
    bResult = GetTempPath(MAX_PATH, szDirectory);
    if (bResult)
    {
        /* Generate a temporary file */
        bResult = GetTempFileName(szDirectory, L"DMP", 0, pszBuffer);
        if (bResult)
        {
            /* Delete the generated file */
            DeleteFile(pszBuffer);

            /* Rename the file extension */
            PathCchRenameExtension(pszBuffer, cchBuffer, L".bin");
        }
    }
}

DWORD WINAPI UpdateElapsedThread(LPVOID lpv)
{
    while (TRUE)
    {
        Sleep(1000);
        ++g_nElapsedSeconds;
    }

    return 0;
}


/* RIFF WAV files use little endian format */
template<typename Word>
std::ostream &WriteData(std::ostream &out, Word Data, unsigned Size = sizeof(Word))
{
    for (; Size; --Size, Data >>= 8) {
        out.put(static_cast<char>(Data & 0xFF));
    }

    return out;
}

VOID ConvertDumpToWAV(
    LPCWSTR lpszWAVFile,
    LPCWSTR lpszDumpFile,
    WAVEFORMATEX wFormat
)
{
    std::ifstream   fDumpFile;
    std::ofstream   fWAVFile;
    SIZE_T          nDataChunkPos;
    SIZE_T          nDumpSize;
    LPBYTE          lpbRawAudio;
    SIZE_T          nFinalLength;

    /* Open each file */
    fDumpFile.open(lpszDumpFile, std::ios_base::binary);
    fWAVFile.open(lpszWAVFile, std::ios_base::binary);


    /* Write the RIFF chunk descriptor */
    fWAVFile << "RIFF----WAVEfmt "; 

    /* Write the wave audio format sub-chunk */
    WriteData(fWAVFile, 16, 4);		// Subchunk1Size
    WriteData(fWAVFile, 1, 2);		// AudioFormat
    WriteData(fWAVFile, 1, 2);		// NumChannels
    WriteData(fWAVFile, 44100, 4);	// SampleRate
    WriteData(fWAVFile, 88200, 4);	// ByteRate  (Sample Rate * BitsPerSample * Channels) / 8
    WriteData(fWAVFile, 2, 2);		// BlockAlign (NumChannels * BitsPerSample / 8)
    WriteData(fWAVFile, 16, 2);		// BitsPerSample

    /* Write the data sub-chunk */
    nDataChunkPos = fWAVFile.tellp();
    fWAVFile << "data----";

    /* Get the dump/temp file size */
    fDumpFile.seekg(0, fDumpFile.end);
    nDumpSize = fDumpFile.tellg();

    /* Allocate buffer for reading dump file */
    lpbRawAudio = new BYTE[nDumpSize];

    /* Read the bytes from the dump file */
    fDumpFile.seekg(0);
    fDumpFile.read((char *)lpbRawAudio, nDumpSize);

    /* Write the audio bytes into the .wav file */
    for (size_t c = 0; c < nDumpSize; ++c)
    {
        WriteData(fWAVFile, lpbRawAudio[c]);
    }

    /* Get the final size of the .wav file */
    nFinalLength = fWAVFile.tellp();

    /* Replace the dashes (-) in the data sub-chunk */
    fWAVFile.seekp(nDataChunkPos + 4);
    WriteData(fWAVFile, wFormat.nSamplesPerSec * g_nElapsedSeconds * 16 / 8, 4);

    /* Replace the dashes (-) in the RIFF chunk header */
    fWAVFile.seekp(0 + 4);
    WriteData(fWAVFile, nFinalLength - 8, 4);

    /* Cleanup */
    delete[] lpbRawAudio;
    fDumpFile.close();
    fWAVFile.close();
}
