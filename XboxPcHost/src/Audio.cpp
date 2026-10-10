#include "pch.h"
#include "Audio.h"
#include "Log.h"

Audio::~Audio()
{
    // Voices must go before the engine that owns them.
    if (m_voice) m_voice->DestroyVoice();
    if (m_master) m_master->DestroyVoice();
}

bool Audio::Initialize()
{
    HRESULT hr = XAudio2Create(m_xaudio.put(), 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr))
    {
        HOST_LOG(L"XAudio2Create failed 0x%08x; running without audio", static_cast<unsigned>(hr));
        return false;
    }
    hr = m_xaudio->CreateMasteringVoice(&m_master);
    if (FAILED(hr))
    {
        HOST_LOG(L"CreateMasteringVoice failed 0x%08x; running without audio", static_cast<unsigned>(hr));
        m_xaudio = nullptr;
        return false;
    }

    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = Channels;
    fmt.nSamplesPerSec = SampleRate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = static_cast<WORD>(fmt.nChannels * fmt.wBitsPerSample / 8);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    hr = m_xaudio->CreateSourceVoice(&m_voice, &fmt);
    if (FAILED(hr))
    {
        HOST_LOG(L"CreateSourceVoice failed 0x%08x; running without audio", static_cast<unsigned>(hr));
        return false;
    }
    m_voice->Start();
    HOST_LOG(L"XAudio2 ready: %u Hz, %u channels", SampleRate, Channels);
    return true;
}

void Audio::Suspend()
{
    if (m_xaudio) m_xaudio->StopEngine();
}

void Audio::Resume()
{
    if (m_xaudio) m_xaudio->StartEngine();
}

UINT32 Audio::QueuedBuffers() const
{
    if (!m_voice) return 0;
    XAUDIO2_VOICE_STATE state{};
    m_voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    return state.BuffersQueued;
}

void Audio::Submit(const int16_t* frames, size_t frameCount)
{
    if (!m_voice || frameCount == 0) return;

    // Buffers finish in order, so anything beyond what's still queued is done.
    UINT32 queued = QueuedBuffers();
    while (m_inFlight.size() > queued) m_inFlight.pop_front();

    if (queued >= XAUDIO2_MAX_QUEUED_BUFFERS)
    {
        HOST_LOG(L"Audio queue full, dropping %zu frames", frameCount);
        return;
    }

    auto& data = m_inFlight.emplace_back(frames, frames + frameCount * Channels);
    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = static_cast<UINT32>(data.size() * sizeof(int16_t));
    buffer.pAudioData = reinterpret_cast<const BYTE*>(data.data());
    HRESULT hr = m_voice->SubmitSourceBuffer(&buffer);
    if (FAILED(hr))
    {
        m_inFlight.pop_back();
        HOST_LOG(L"SubmitSourceBuffer failed 0x%08x", static_cast<unsigned>(hr));
    }
}

void Audio::PlayTestTone(float hz, float seconds)
{
    size_t frames = static_cast<size_t>(SampleRate * seconds);
    std::vector<int16_t> pcm(frames * Channels);
    constexpr double twoPi = 6.283185307179586;
    for (size_t i = 0; i < frames; ++i)
    {
        // Short fade in/out so the beep doesn't click.
        double env = std::min({ 1.0, i / 480.0, (frames - i) / 480.0 });
        auto s = static_cast<int16_t>(std::sin(twoPi * hz * i / SampleRate) * env * 8000.0);
        pcm[i * 2] = s;
        pcm[i * 2 + 1] = s;
    }
    Submit(pcm.data(), frames);
}
