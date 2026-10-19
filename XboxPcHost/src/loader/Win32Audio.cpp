#include "pch.h"
#include "loader/Win32Layer.h"
#include "Log.h"

// winmm's waveOut on XAudio2. Each waveOutOpen makes one source voice in the
// game's own format (XAudio2 converts and mixes it), and each waveOutWrite
// queues the game's buffer on it in place. When XAudio2 finishes a buffer,
// WHDR_DONE goes on its header, which is how games that poll learn it is free.
// The structs are declared here because the app partition hides winmm's.
namespace
{
    struct WaveHeader
    {
        char* data;
        DWORD length;
        DWORD bytesRecorded;
        DWORD_PTR user;
        DWORD flags;
        DWORD loops;
        WaveHeader* next;
        DWORD_PTR reserved;
    };
    static_assert(sizeof(WaveHeader) == 48);

    constexpr DWORD kWhdrDone = 0x1, kWhdrPrepared = 0x2, kWhdrBeginLoop = 0x4, kWhdrInQueue = 0x10;
    constexpr UINT kNoError = 0, kBadDeviceId = 2, kAllocated = 4, kNoDriver = 6, kNoMem = 7, kInvalParam = 11,
        kBadFormat = 32, kStillPlaying = 33, kUnprepared = 34;
    constexpr DWORD kCallbackTypeMask = 0x70000, kCallbackFunction = 0x30000, kFormatQuery = 0x1;
    constexpr UINT kWomDone = 0x3BD;

    using WaveProc = void(CALLBACK*)(void* handle, UINT message, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR param2);

    IXAudio2* g_engine = nullptr;
    std::mutex g_lock;

    struct Device final : IXAudio2VoiceCallback
    {
        IXAudio2SourceVoice* voice = nullptr;
        WaveProc callback = nullptr;
        DWORD_PTR instance = 0;
        std::atomic<int> queued{ 0 };

        void STDMETHODCALLTYPE OnBufferEnd(void* context) override
        {
            auto header = static_cast<WaveHeader*>(context);
            header->flags = (header->flags & ~kWhdrInQueue) | kWhdrDone;
            --queued;
            if (callback)
                callback(this, kWomDone, instance, reinterpret_cast<DWORD_PTR>(header), 0);
        }
        void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
        void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
        void STDMETHODCALLTYPE OnStreamEnd() override {}
        void STDMETHODCALLTYPE OnBufferStart(void*) override {}
        void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
        void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}
    };

    std::set<Device*> g_devices;

    Device* Find(void* handle)
    {
        std::lock_guard lock(g_lock);
        auto device = static_cast<Device*>(handle);
        return g_devices.count(device) ? device : nullptr;
    }

    UINT WINAPI Shim_waveOutGetNumDevs() { return g_engine ? 1 : 0; }

    UINT WINAPI Shim_waveOutOpen(void** handle, UINT deviceId, const WAVEFORMATEX* format, DWORD_PTR callback, DWORD_PTR instance, DWORD flags)
    {
        if (!g_engine)
            return kNoDriver;
        if (!format || (!handle && !(flags & kFormatQuery)))
            return kInvalParam;
        if (deviceId != 0 && deviceId != 0xFFFFFFFF) // device 0 or WAVE_MAPPER
            return kBadDeviceId;
        if (format->wFormatTag != WAVE_FORMAT_PCM || format->nChannels == 0 || format->nChannels > 2 ||
            (format->wBitsPerSample != 8 && format->wBitsPerSample != 16))
            return kBadFormat;
        if (flags & kFormatQuery)
            return kNoError;

        auto device = std::make_unique<Device>();
        if ((flags & kCallbackTypeMask) == kCallbackFunction)
        {
            device->callback = reinterpret_cast<WaveProc>(callback);
            device->instance = instance;
        }
        HRESULT hr = g_engine->CreateSourceVoice(&device->voice, format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, device.get());
        if (FAILED(hr))
        {
            HOST_LOG(L"waveOutOpen: CreateSourceVoice failed 0x%08x", static_cast<unsigned>(hr));
            return kAllocated;
        }
        device->voice->Start();
        HOST_LOG(L"waveOutOpen %lu Hz %u-bit %uch", format->nSamplesPerSec, format->wBitsPerSample, format->nChannels);

        Device* raw = device.release();
        std::lock_guard lock(g_lock);
        g_devices.insert(raw);
        *handle = raw;
        return kNoError;
    }

    UINT WINAPI Shim_waveOutClose(void* handle)
    {
        Device* device = Find(handle);
        if (!device)
            return kInvalParam;
        if (device->queued > 0)
            return kStillPlaying;
        {
            std::lock_guard lock(g_lock);
            g_devices.erase(device);
        }
        device->voice->DestroyVoice(); // waits for any callback in progress
        delete device;
        return kNoError;
    }

    UINT WINAPI Shim_waveOutPrepareHeader(void* handle, WaveHeader* header, UINT size)
    {
        if (!Find(handle) || !header || size < sizeof(WaveHeader))
            return kInvalParam;
        header->flags |= kWhdrPrepared;
        return kNoError;
    }

    UINT WINAPI Shim_waveOutUnprepareHeader(void* handle, WaveHeader* header, UINT size)
    {
        if (!Find(handle) || !header || size < sizeof(WaveHeader))
            return kInvalParam;
        if (header->flags & kWhdrInQueue)
            return kStillPlaying;
        header->flags &= ~kWhdrPrepared;
        return kNoError;
    }

    UINT WINAPI Shim_waveOutWrite(void* handle, WaveHeader* header, UINT size)
    {
        Device* device = Find(handle);
        if (!device || !header || size < sizeof(WaveHeader))
            return kInvalParam;
        if (!(header->flags & kWhdrPrepared))
            return kUnprepared;
        if (header->flags & kWhdrInQueue)
            return kStillPlaying;

        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes = header->length;
        buffer.pAudioData = reinterpret_cast<const BYTE*>(header->data);
        buffer.pContext = header;
        if ((header->flags & kWhdrBeginLoop) && header->loops > 1)
            buffer.LoopCount = std::min<DWORD>(header->loops - 1, XAUDIO2_MAX_LOOP_COUNT);

        header->flags = (header->flags & ~kWhdrDone) | kWhdrInQueue;
        ++device->queued;
        HRESULT hr = device->voice->SubmitSourceBuffer(&buffer);
        if (FAILED(hr))
        {
            header->flags &= ~kWhdrInQueue;
            --device->queued;
            return kNoMem;
        }
        return kNoError;
    }

    // Stops playback and gives every queued buffer back, each marked done.
    UINT WINAPI Shim_waveOutReset(void* handle)
    {
        Device* device = Find(handle);
        if (!device)
            return kInvalParam;
        device->voice->Stop();
        device->voice->FlushSourceBuffers();
        device->voice->Start();
        return kNoError;
    }

    UINT WINAPI Shim_waveOutGetErrorTextA(UINT error, char* text, UINT size)
    {
        if (!text || size == 0)
            return kInvalParam;
        snprintf(text, size, "waveOut error %u", error);
        return kNoError;
    }
}

void SetGameAudio(IXAudio2* engine)
{
    g_engine = engine;
}

namespace
{
    DWORD WINAPI Shim_timeGetTime() { return static_cast<DWORD>(GetTickCount64()); }

    UINT WINAPI Shim_timeGetDevCaps(UINT* caps, UINT size)
    {
        if (!caps || size < 2 * sizeof(UINT))
            return 97; // TIMERR_NOCANDO
        caps[0] = 1;       // wPeriodMin, ms
        caps[1] = 1000000; // wPeriodMax
        return 0;
    }

    // Multimedia timer callbacks (used for MIDI playback) aren't offered; games check for 0.
    UINT WINAPI Shim_timeSetEvent(UINT, UINT, void*, DWORD_PTR, UINT) { return 0; }
    UINT WINAPI Shim_timeKillEvent(UINT) { return 97; } // TIMERR_NOCANDO: no such timer

    // No MIDI devices.
    UINT WINAPI Shim_midiOutGetNumDevs() { return 0; }
    UINT WINAPI Shim_MidiNoDriver() { return 6; } // MMSYSERR_NODRIVER
}

void RegisterAudio(Win32Layer& layer)
{
#define SHIM(fn) layer.Register("winmm!" #fn, reinterpret_cast<void*>(&Shim_##fn))
    SHIM(waveOutGetNumDevs);
    SHIM(waveOutOpen);
    SHIM(waveOutClose);
    SHIM(waveOutPrepareHeader);
    SHIM(waveOutUnprepareHeader);
    SHIM(waveOutWrite);
    SHIM(waveOutReset);
    SHIM(waveOutGetErrorTextA);
    SHIM(timeGetTime);
    SHIM(timeGetDevCaps);
    SHIM(timeSetEvent);
    SHIM(timeKillEvent);
    SHIM(midiOutGetNumDevs);
    for (const char* name : { "midiOutGetDevCapsW", "midiOutOpen", "midiOutClose", "midiOutReset", "midiOutLongMsg",
        "midiOutShortMsg", "midiOutPrepareHeader", "midiOutUnprepareHeader" })
        layer.Register((std::string("winmm!") + name).c_str(), reinterpret_cast<void*>(&Shim_MidiNoDriver));
#undef SHIM
}
