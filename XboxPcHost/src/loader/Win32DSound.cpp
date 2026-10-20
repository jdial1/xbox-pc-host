#include "pch.h"
#include <cmath>
#include "loader/Win32Layer.h"
#include "Log.h"

// DirectSound on XAudio2, the way audio engines of the 2000s and 2010s use it (FMOD Ex in Unity 5,
// many older games): a primary buffer that only carries the output format, and secondary buffers the
// game writes ahead into with Lock/Unlock while reading the play cursor. Each playing secondary buffer
// feeds an XAudio2 source voice 10 ms at a time from its ring; the play cursor is what that voice has
// played. The interfaces are declared here (dsound.h is desktop-only); layouts and ids are Windows'.
namespace
{
    struct DsBufferDesc // DSBUFFERDESC
    {
        DWORD size, flags, bufferBytes, reserved;
        WAVEFORMATEX* format;
        GUID algorithm3d;
    };
    struct DsBufferCaps { DWORD size, flags, bufferBytes, unlockTransferRate, playCpuOverhead; };
    struct DsCaps { DWORD size, flags, values[22]; };
    static_assert(sizeof(DsCaps) == 96);

    constexpr DWORD kPrimary = 0x1, kGetCurrentPosition2 = 0x10000;
    constexpr DWORD kStatusPlaying = 0x1, kStatusLooping = 0x4;
    constexpr DWORD kPlayLooping = 0x1, kLockFromWriteCursor = 0x1, kLockEntireBuffer = 0x2;
    constexpr HRESULT kDsOk = S_OK, kNoDriver = static_cast<HRESULT>(0x88780078), kBadFormat = static_cast<HRESULT>(0x88780064);

    struct DECLSPEC_UUID("279AFA85-4981-11CE-A521-0020AF0BE560") DECLSPEC_NOVTABLE GameDsBuffer : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE GetCaps(DsBufferCaps* caps) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetCurrentPosition(DWORD* play, DWORD* write) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetFormat(WAVEFORMATEX* format, DWORD size, DWORD* written) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetVolume(LONG* volume) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetPan(LONG* pan) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetFrequency(DWORD* frequency) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetStatus(DWORD* status) = 0;
        virtual HRESULT STDMETHODCALLTYPE Initialize(IUnknown* sound, const DsBufferDesc* desc) = 0;
        virtual HRESULT STDMETHODCALLTYPE Lock(DWORD offset, DWORD bytes, void** p1, DWORD* n1, void** p2, DWORD* n2, DWORD flags) = 0;
        virtual HRESULT STDMETHODCALLTYPE Play(DWORD reserved, DWORD priority, DWORD flags) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetCurrentPosition(DWORD position) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetFormat(const WAVEFORMATEX* format) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetVolume(LONG volume) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetPan(LONG pan) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetFrequency(DWORD frequency) = 0;
        virtual HRESULT STDMETHODCALLTYPE Stop() = 0;
        virtual HRESULT STDMETHODCALLTYPE Unlock(void* p1, DWORD n1, void* p2, DWORD n2) = 0;
        virtual HRESULT STDMETHODCALLTYPE Restore() = 0;
    };
    struct DECLSPEC_UUID("6825A449-7524-4D82-920F-50E36AB3AB1E") DECLSPEC_NOVTABLE GameDsBuffer8 : GameDsBuffer
    {
        virtual HRESULT STDMETHODCALLTYPE SetFX(DWORD count, void* descs, DWORD* results) = 0;
        virtual HRESULT STDMETHODCALLTYPE AcquireResources(DWORD flags, DWORD count, DWORD* results) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetObjectInPath(REFGUID object, DWORD index, REFGUID iid, void** out) = 0;
    };
    struct DECLSPEC_UUID("279AFA83-4981-11CE-A521-0020AF0BE560") DECLSPEC_NOVTABLE GameDs : IUnknown
    {
        virtual HRESULT STDMETHODCALLTYPE CreateSoundBuffer(const DsBufferDesc* desc, GameDsBuffer** out, IUnknown* outer) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetCaps(DsCaps* caps) = 0;
        virtual HRESULT STDMETHODCALLTYPE DuplicateSoundBuffer(GameDsBuffer* original, GameDsBuffer** out) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND window, DWORD level) = 0;
        virtual HRESULT STDMETHODCALLTYPE Compact() = 0;
        virtual HRESULT STDMETHODCALLTYPE GetSpeakerConfig(DWORD* config) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetSpeakerConfig(DWORD config) = 0;
        virtual HRESULT STDMETHODCALLTYPE Initialize(const GUID* device) = 0;
    };
    struct DECLSPEC_UUID("C50A7E93-F395-4834-9EF6-7FA99DE50966") DECLSPEC_NOVTABLE GameDs8 : GameDs
    {
        virtual HRESULT STDMETHODCALLTYPE VerifyCertification(DWORD* certified) = 0;
    };

    std::vector<uint8_t> CopyFormat(const WAVEFORMATEX* f)
    {
        size_t n = sizeof(WAVEFORMATEX) + (f->wFormatTag == WAVE_FORMAT_PCM ? 0 : f->cbSize);
        auto p = reinterpret_cast<const uint8_t*>(f);
        std::vector<uint8_t> v(p, p + n);
        if (f->wFormatTag == WAVE_FORMAT_PCM)
            reinterpret_cast<WAVEFORMATEX*>(v.data())->cbSize = 0;
        return v;
    }

    struct Buffer : winrt::implements<Buffer, GameDsBuffer8>, IXAudio2VoiceCallback
    {
        Buffer(IXAudio2* engine, DsBufferDesc const& desc) : m_engine(engine), m_flags(desc.flags)
        {
            if (desc.format)
                m_format = CopyFormat(desc.format);
            else // the primary buffer's default
            {
                WAVEFORMATEX f{ WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0 };
                m_format = CopyFormat(&f);
            }
            if (!(m_flags & kPrimary))
                m_ring.assign(desc.bufferBytes, Format().wBitsPerSample == 8 ? 0x80 : 0);
        }
        ~Buffer()
        {
            if (m_voice) m_voice->DestroyVoice(); // waits for callbacks in progress
        }

        WAVEFORMATEX const& Format() const { return *reinterpret_cast<const WAVEFORMATEX*>(m_format.data()); }
        bool Primary() const { return (m_flags & kPrimary) != 0; }
        DWORD Size() const { return static_cast<DWORD>(m_ring.size()); }

        // ---- IDirectSoundBuffer
        HRESULT STDMETHODCALLTYPE GetCaps(DsBufferCaps* caps) override
        {
            if (!caps || caps->size < sizeof(DsBufferCaps)) return E_INVALIDARG;
            caps->flags = m_flags | 0x8; // LOCSOFTWARE
            caps->bufferBytes = Primary() ? 4096 : Size();
            caps->unlockTransferRate = 0;
            caps->playCpuOverhead = 0;
            return kDsOk;
        }
        HRESULT STDMETHODCALLTYPE GetCurrentPosition(DWORD* play, DWORD* write) override
        {
            std::lock_guard lock(m_lock);
            DWORD p = PlayCursor();
            if (play) *play = p;
            if (write) *write = Primary() ? 0 : m_submit % std::max<DWORD>(Size(), 1);
            return kDsOk;
        }
        HRESULT STDMETHODCALLTYPE GetFormat(WAVEFORMATEX* format, DWORD size, DWORD* written) override
        {
            if (written) *written = static_cast<DWORD>(m_format.size());
            if (!format) return written ? kDsOk : E_INVALIDARG;
            memcpy(format, m_format.data(), std::min<size_t>(size, m_format.size()));
            return kDsOk;
        }
        HRESULT STDMETHODCALLTYPE GetVolume(LONG* v) override { if (v) *v = m_volume; return kDsOk; }
        HRESULT STDMETHODCALLTYPE GetPan(LONG* p) override { if (p) *p = 0; return kDsOk; }
        HRESULT STDMETHODCALLTYPE GetFrequency(DWORD* f) override { if (f) *f = m_frequency ? m_frequency : Format().nSamplesPerSec; return kDsOk; }
        HRESULT STDMETHODCALLTYPE GetStatus(DWORD* status) override
        {
            if (!status) return E_INVALIDARG;
            std::lock_guard lock(m_lock);
            *status = (m_playing ? kStatusPlaying : 0) | (m_playing && m_looping ? kStatusLooping : 0);
            return kDsOk;
        }
        HRESULT STDMETHODCALLTYPE Initialize(IUnknown*, const DsBufferDesc*) override { return static_cast<HRESULT>(0x88780082); } // DSERR_ALREADYINITIALIZED
        HRESULT STDMETHODCALLTYPE Lock(DWORD offset, DWORD bytes, void** p1, DWORD* n1, void** p2, DWORD* n2, DWORD flags) override
        {
            if (Primary() || !p1 || !n1) return E_INVALIDARG;
            std::lock_guard lock(m_lock);
            DWORD size = Size();
            if (flags & kLockFromWriteCursor) offset = m_submit % size;
            if (flags & kLockEntireBuffer) bytes = size;
            if (offset >= size || bytes > size) return E_INVALIDARG;
            DWORD first = std::min(bytes, size - offset);
            *p1 = m_ring.data() + offset;
            *n1 = first;
            if (p2) *p2 = bytes > first ? m_ring.data() : nullptr;
            if (n2) *n2 = bytes - first;
            return kDsOk;
        }
        HRESULT STDMETHODCALLTYPE Play(DWORD, DWORD, DWORD flags) override
        {
            if (Primary()) return kDsOk; // the primary buffer only carries the format
            std::lock_guard lock(m_lock);
            m_looping = (flags & kPlayLooping) != 0;
            if (m_playing) return kDsOk;
            if (!m_voice)
            {
                HRESULT hr = m_engine->CreateSourceVoice(&m_voice, &Format(), 0, 4.0f, this);
                if (FAILED(hr))
                {
                    HOST_LOG(L"DirectSound: CreateSourceVoice failed 0x%08X", static_cast<unsigned>(hr));
                    return static_cast<HRESULT>(0x8878000A); // DSERR_ALLOCATED
                }
                ApplyVolume();
                if (m_frequency) m_voice->SetFrequencyRatio(static_cast<float>(m_frequency) / Format().nSamplesPerSec);
            }
            m_playing = true;
            XAUDIO2_VOICE_STATE state{};
            m_voice->GetState(&state, 0);
            m_samplesAtStart = state.SamplesPlayed;
            m_cursorAtStart = m_submit;
            for (int i = 0; i < kAhead; ++i)
                SubmitChunk();
            m_voice->Start();
            return kDsOk;
        }
        HRESULT STDMETHODCALLTYPE SetCurrentPosition(DWORD position) override
        {
            std::lock_guard lock(m_lock);
            if (Primary() || position >= std::max<DWORD>(Size(), 1)) return Primary() ? kDsOk : E_INVALIDARG;
            bool wasPlaying = m_playing;
            StopLocked();
            m_submit = position;
            if (wasPlaying)
                Play(0, 0, m_looping ? kPlayLooping : 0); // the lock is recursive
            return kDsOk;
        }
        HRESULT STDMETHODCALLTYPE SetFormat(const WAVEFORMATEX* format) override
        {
            if (!format) return E_INVALIDARG;
            if (!Primary()) return static_cast<HRESULT>(0x88780032); // DSERR_INVALIDCALL
            m_format = CopyFormat(format);
            return kDsOk;
        }
        HRESULT STDMETHODCALLTYPE SetVolume(LONG volume) override
        {
            m_volume = std::clamp<LONG>(volume, -10000, 0);
            std::lock_guard lock(m_lock);
            ApplyVolume();
            return kDsOk;
        }
        HRESULT STDMETHODCALLTYPE SetPan(LONG) override { return kDsOk; }
        HRESULT STDMETHODCALLTYPE SetFrequency(DWORD frequency) override
        {
            m_frequency = frequency;
            std::lock_guard lock(m_lock);
            if (m_voice && !Primary())
                m_voice->SetFrequencyRatio(static_cast<float>(frequency ? frequency : Format().nSamplesPerSec) / Format().nSamplesPerSec);
            return kDsOk;
        }
        HRESULT STDMETHODCALLTYPE Stop() override
        {
            std::lock_guard lock(m_lock);
            StopLocked();
            return kDsOk;
        }
        HRESULT STDMETHODCALLTYPE Unlock(void*, DWORD, void*, DWORD) override { return kDsOk; }
        HRESULT STDMETHODCALLTYPE Restore() override { return kDsOk; }
        // ---- IDirectSoundBuffer8
        HRESULT STDMETHODCALLTYPE SetFX(DWORD count, void*, DWORD*) override { return count ? E_NOTIMPL : kDsOk; }
        HRESULT STDMETHODCALLTYPE AcquireResources(DWORD, DWORD, DWORD*) override { return kDsOk; }
        HRESULT STDMETHODCALLTYPE GetObjectInPath(REFGUID, DWORD, REFGUID, void** out) override
        {
            if (out) *out = nullptr;
            return E_NOINTERFACE;
        }

        // ---- IXAudio2VoiceCallback: a chunk has played, queue the next one
        void STDMETHODCALLTYPE OnBufferEnd(void* context) override
        {
            std::lock_guard lock(m_lock);
            auto chunk = static_cast<std::vector<uint8_t>*>(context);
            for (auto it = m_chunks.begin(); it != m_chunks.end(); ++it)
                if (it->get() == chunk)
                {
                    m_chunks.erase(it);
                    break;
                }
            if (m_playing)
                SubmitChunk();
            if (m_playing && m_chunks.empty())
                m_playing = false; // a one-shot buffer has played to its end
        }
        void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
        void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
        void STDMETHODCALLTYPE OnStreamEnd() override {}
        void STDMETHODCALLTYPE OnBufferStart(void*) override {}
        void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
        void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}

    private:
        static constexpr int kAhead = 3; // chunks queued on the voice: 30 ms

        DWORD ChunkBytes() const
        {
            DWORD align = std::max<DWORD>(Format().nBlockAlign, 1);
            DWORD bytes = std::max<DWORD>(Format().nAvgBytesPerSec / 100, align); // 10 ms
            return std::max<DWORD>(bytes / align * align, align);
        }

        // Copies the next 10 ms after the submit cursor and queues it; caller holds the lock.
        void SubmitChunk()
        {
            DWORD size = Size();
            if (!size || !m_voice) return;
            DWORD at = m_submit % size;
            DWORD bytes = std::min(ChunkBytes(), size);
            if (!m_looping)
            {
                if (m_submit >= size) return; // one-shot: everything is queued already
                bytes = std::min(bytes, size - m_submit);
            }
            auto chunk = std::make_unique<std::vector<uint8_t>>(bytes);
            DWORD first = std::min(bytes, size - at);
            memcpy(chunk->data(), m_ring.data() + at, first);
            if (bytes > first)
                memcpy(chunk->data() + first, m_ring.data(), bytes - first);
            XAUDIO2_BUFFER b{};
            b.AudioBytes = bytes;
            b.pAudioData = chunk->data();
            b.pContext = chunk.get();
            if (FAILED(m_voice->SubmitSourceBuffer(&b)))
                return;
            m_chunks.push_back(std::move(chunk));
            m_submit = m_looping ? (m_submit + bytes) % size : m_submit + bytes;
        }

        // What the voice has played since Play, as a byte offset in the ring; caller holds the lock.
        DWORD PlayCursor() const
        {
            DWORD size = Size();
            if (Primary() || !size) return 0;
            if (!m_playing || !m_voice) return m_submit % size;
            XAUDIO2_VOICE_STATE state{};
            m_voice->GetState(&state, 0);
            UINT64 played = (state.SamplesPlayed - m_samplesAtStart) * Format().nBlockAlign;
            return static_cast<DWORD>((m_cursorAtStart + played) % size);
        }

        void StopLocked()
        {
            if (!m_playing || !m_voice) { m_playing = false; return; }
            DWORD cursor = PlayCursor();
            m_playing = false;
            m_voice->Stop();
            m_voice->FlushSourceBuffers(); // OnBufferEnd runs for each; it won't queue more
            m_submit = cursor;
        }

        void ApplyVolume()
        {
            if (m_voice) m_voice->SetVolume(m_volume <= -10000 ? 0.0f : std::pow(10.0f, m_volume / 2000.0f));
        }

        IXAudio2* m_engine;
        DWORD m_flags;
        std::vector<uint8_t> m_format;
        std::vector<uint8_t> m_ring;
        std::recursive_mutex m_lock;
        IXAudio2SourceVoice* m_voice = nullptr;
        std::deque<std::unique_ptr<std::vector<uint8_t>>> m_chunks;
        bool m_playing = false, m_looping = false;
        DWORD m_submit = 0;          // where the next chunk comes from
        DWORD m_cursorAtStart = 0;   // the ring offset Play started from
        UINT64 m_samplesAtStart = 0;
        LONG m_volume = 0;
        DWORD m_frequency = 0;
    };

    struct Device : winrt::implements<Device, GameDs8>
    {
        explicit Device(IXAudio2* engine) : m_engine(engine) {}

        HRESULT STDMETHODCALLTYPE CreateSoundBuffer(const DsBufferDesc* desc, GameDsBuffer** out, IUnknown*) override
        {
            if (!desc || !out) return E_INVALIDARG;
            *out = nullptr;
            bool primary = (desc->flags & kPrimary) != 0;
            if (!primary && (!desc->format || desc->bufferBytes < 4 || desc->bufferBytes > 0x0FFFFFFF)) return E_INVALIDARG;
            if (!primary && desc->format->nChannels > 8) return kBadFormat;
            auto buffer = winrt::make_self<Buffer>(m_engine, *desc);
            *out = buffer.as<GameDsBuffer>().detach();
            return kDsOk;
        }
        HRESULT STDMETHODCALLTYPE GetCaps(DsCaps* caps) override
        {
            if (!caps || caps->size < sizeof(DsCaps)) return E_INVALIDARG;
            memset(caps->values, 0, sizeof(caps->values));
            caps->flags = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x100 | 0x200 | 0x400 | 0x800; // primary/secondary mono, stereo, 8 and 16-bit; continuous rate
            caps->values[0] = 100;     // dwMinSecondarySampleRate
            caps->values[1] = 200000;  // dwMaxSecondarySampleRate
            caps->values[2] = 1;       // dwPrimaryBuffers
            return kDsOk;
        }
        HRESULT STDMETHODCALLTYPE DuplicateSoundBuffer(GameDsBuffer*, GameDsBuffer** out) override
        {
            if (out) *out = nullptr;
            return E_NOTIMPL;
        }
        HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND, DWORD) override { return kDsOk; }
        HRESULT STDMETHODCALLTYPE Compact() override { return kDsOk; }
        HRESULT STDMETHODCALLTYPE GetSpeakerConfig(DWORD* config) override { if (config) *config = 4; return kDsOk; } // DSSPEAKER_STEREO
        HRESULT STDMETHODCALLTYPE SetSpeakerConfig(DWORD) override { return kDsOk; }
        HRESULT STDMETHODCALLTYPE Initialize(const GUID*) override { return kDsOk; }
        HRESULT STDMETHODCALLTYPE VerifyCertification(DWORD* certified) override { if (certified) *certified = 0; return kDsOk; }

    private:
        IXAudio2* m_engine;
    };

    HRESULT Create(REFIID iid, void** out)
    {
        if (!out) return E_INVALIDARG;
        *out = nullptr;
        IXAudio2* engine = GameAudioEngine();
        if (!engine) return kNoDriver;
        auto device = winrt::make_self<Device>(engine);
        HRESULT hr = device->QueryInterface(iid, out);
        HOST_LOG(L"DirectSound device created (0x%08X)", static_cast<unsigned>(hr));
        return hr;
    }

    HRESULT WINAPI Shim_DirectSoundCreate8(const GUID*, IUnknown** out, IUnknown*) { return Create(__uuidof(GameDs8), reinterpret_cast<void**>(out)); }
    HRESULT WINAPI Shim_DirectSoundCreate(const GUID*, IUnknown** out, IUnknown*) { return Create(__uuidof(GameDs), reinterpret_cast<void**>(out)); }

    // One output: the default device (no GUID) and the speakers behind it.
    using EnumW = BOOL(CALLBACK*)(GUID*, LPCWSTR, LPCWSTR, LPVOID);
    using EnumA = BOOL(CALLBACK*)(GUID*, LPCSTR, LPCSTR, LPVOID);
    GUID g_speakers = { 0xdef00000, 0x9c6d, 0x47ed, { 0xaa, 0xf1, 0x4d, 0xda, 0x8f, 0x2b, 0x5c, 0x03 } }; // DSDEVID_DefaultPlayback
    HRESULT WINAPI Shim_DirectSoundEnumerateW(EnumW fn, LPVOID context)
    {
        if (!fn) return E_INVALIDARG;
        if (!GameAudioEngine()) return kDsOk; // no devices
        if (fn(nullptr, L"Primary Sound Driver", L"", context))
            fn(&g_speakers, L"Speakers (Xbox)", L"{0.0.0.00000000}", context);
        return kDsOk;
    }
    HRESULT WINAPI Shim_DirectSoundEnumerateA(EnumA fn, LPVOID context)
    {
        if (!fn) return E_INVALIDARG;
        if (!GameAudioEngine()) return kDsOk;
        if (fn(nullptr, "Primary Sound Driver", "", context))
            fn(&g_speakers, "Speakers (Xbox)", "{0.0.0.00000000}", context);
        return kDsOk;
    }
    // No capture devices.
    HRESULT WINAPI Shim_DirectSoundCaptureEnumerate(void*, LPVOID) { return kDsOk; }
    HRESULT WINAPI Shim_DirectSoundCaptureCreate(const GUID*, void** out, IUnknown*) { if (out) *out = nullptr; return kNoDriver; }
}

// The objects answer QueryInterface for the interfaces theirs are built on, too.
template <> bool winrt::is_guid_of<GameDsBuffer8>(guid const& id) noexcept { return winrt::is_guid_of<GameDsBuffer8, GameDsBuffer>(id); }
template <> bool winrt::is_guid_of<GameDs8>(guid const& id) noexcept { return winrt::is_guid_of<GameDs8, GameDs>(id); }

void RegisterDSound(Win32Layer& layer)
{
#define DS(name, fn) layer.Register("dsound!" name, reinterpret_cast<void*>(&fn))
    DS("DirectSoundCreate8", Shim_DirectSoundCreate8);
    DS("DirectSoundCreate", Shim_DirectSoundCreate);
    DS("DirectSoundEnumerateW", Shim_DirectSoundEnumerateW);
    DS("DirectSoundEnumerateA", Shim_DirectSoundEnumerateA);
    DS("DirectSoundCaptureEnumerateW", Shim_DirectSoundCaptureEnumerate);
    DS("DirectSoundCaptureEnumerateA", Shim_DirectSoundCaptureEnumerate);
    DS("DirectSoundCaptureCreate", Shim_DirectSoundCaptureCreate);
    DS("DirectSoundCaptureCreate8", Shim_DirectSoundCaptureCreate);
    // dsound's ordinals: 1 DirectSoundCreate, 2 DirectSoundEnumerateA, 3 DirectSoundEnumerateW, 11 DirectSoundCreate8
    DS("#1", Shim_DirectSoundCreate);
    DS("#2", Shim_DirectSoundEnumerateA);
    DS("#3", Shim_DirectSoundEnumerateW);
    DS("#11", Shim_DirectSoundCreate8);
#undef DS
}
