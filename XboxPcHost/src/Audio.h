#pragma once

// XAudio2 engine with one 48 kHz stereo 16-bit stream. Stage 4 will feed the
// game's audio (DirectSound/XAudio/waveOut) into Submit(); the engine itself
// is exposed for games that bring their own XAudio2 voices.
class Audio
{
public:
    static constexpr UINT32 SampleRate = 48000;
    static constexpr UINT32 Channels = 2;

    ~Audio();
    bool Initialize();
    void Suspend();
    void Resume();

    // Queues interleaved stereo frames. Copies the data. Call from one thread.
    void Submit(const int16_t* frames, size_t frameCount);
    // Short sine beep, to check the speakers work.
    void PlayTestTone(float hz = 440.0f, float seconds = 0.25f);
    UINT32 QueuedBuffers() const;

    IXAudio2* Engine() const { return m_xaudio.get(); }
    bool Ready() const { return m_voice != nullptr; }

private:
    winrt::com_ptr<IXAudio2> m_xaudio;
    IXAudio2MasteringVoice* m_master = nullptr;
    IXAudio2SourceVoice* m_voice = nullptr;
    // XAudio2 reads from our memory until a buffer finishes, so keep them alive.
    std::deque<std::vector<int16_t>> m_inFlight;
};
