// Freestanding test program for the waveOut shims: plays a tenth of a second
// of tone and exits with a code saying how far it got.
//   1 opened   2 buffer marked WHDR_DONE   4 unprepared   8 a device exists   16 closed
#include <windows.h>
#include <mmsystem.h>

static short samples[2205]; // 0.1 s at 22050 Hz, mono, 16-bit

void start(void)
{
    static WAVEFORMATEX fmt; // static so it is zeroed without a memset call
    static WAVEHDR hdr;
    HWAVEOUT out = NULL;
    int mask = 0;
    int i;

    if (waveOutGetNumDevs() >= 1) mask |= 8;

    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = 22050;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 2;
    fmt.nAvgBytesPerSec = 22050 * 2;

    if (waveOutOpen(&out, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR)
    {
        mask |= 1;
        for (i = 0; i < 2205; i++)
            samples[i] = (short)(((i % 50) - 25) * 200); // a rough 441 Hz saw
        hdr.lpData = (LPSTR)samples;
        hdr.dwBufferLength = sizeof(samples);
        waveOutPrepareHeader(out, &hdr, sizeof(hdr));
        waveOutWrite(out, &hdr, sizeof(hdr));
        for (i = 0; i < 400 && !(hdr.dwFlags & WHDR_DONE); i++)
            Sleep(5);
        if (hdr.dwFlags & WHDR_DONE) mask |= 2;
        if (waveOutUnprepareHeader(out, &hdr, sizeof(hdr)) == MMSYSERR_NOERROR) mask |= 4;
        waveOutReset(out);
        if (waveOutClose(out) == MMSYSERR_NOERROR) mask |= 16;
    }
    ExitProcess(mask);
}
