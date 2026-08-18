#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Fixed-format, allocation-free voice effects for ChromeMic's process-loopback
// stream. Instances are stateful and intended to be owned by one audio thread.
enum class VoiceEffectMode : uint8_t {
    Natural = 0,
    ClearMic,
    Broadcast,
    Radio,
    Robot,
    DeepTone
};

struct VoiceEffectsResult {
    float peak = 0.0F;
    bool limiting = false;
};

class VoiceEffectsProcessor {
public:
    static constexpr uint32_t kSampleRate = 44100;
    static constexpr uint16_t kChannels = 2;
    static constexpr float kOutputCeiling = 0.89125094F; // -1 dBFS.

    explicit VoiceEffectsProcessor(
        VoiceEffectMode mode = VoiceEffectMode::Natural) noexcept;

    // Changing mode clears all filter, dynamics, oscillator, and delay state so
    // audio from the previous effect cannot bleed into the new effect.
    void SetMode(VoiceEffectMode mode) noexcept;
    [[nodiscard]] VoiceEffectMode Mode() const noexcept { return mode_; }
    void Reset() noexcept;

    // Processes interleaved signed PCM16 stereo at exactly 44.1 kHz in place.
    // Apply this before the application's final gain/mute stage. A caller that
    // mutes by skipping this stage should Reset on the mute transition so a
    // delay-based mode cannot retain audio for the subsequent unmute.
    // A null buffer or zero frames is a safe no-op. No allocation or exception
    // can occur on the audio thread. Every emitted sample is finite and is
    // bounded to kOutputCeiling, including after non-finite intermediate math.
    VoiceEffectsResult Process(int16_t* interleavedStereo,
                               size_t frames) noexcept;

private:
    struct Biquad {
        float b0 = 1.0F;
        float b1 = 0.0F;
        float b2 = 0.0F;
        float a1 = 0.0F;
        float a2 = 0.0F;
        float z1 = 0.0F;
        float z2 = 0.0F;

        void SetBypass() noexcept;
        void SetHighPass(float frequencyHz, float q) noexcept;
        void SetLowPass(float frequencyHz, float q) noexcept;
        void SetPeaking(float frequencyHz, float q, float gainDb) noexcept;
        void ClearState() noexcept;
        float Process(float sample) noexcept;
    };

    struct ChannelFilters {
        Biquad highPass;
        Biquad toneA;
        Biquad toneB;
        Biquad lowPass;
    };

    static constexpr size_t kDelayCapacity = 2048;
    static constexpr size_t kDelayMask = kDelayCapacity - 1;

    static VoiceEffectMode NormalizeMode(VoiceEffectMode mode) noexcept;
    static float Decode(int16_t sample) noexcept;
    static int16_t Encode(float sample, VoiceEffectsResult& result) noexcept;
    static float Sanitize(float value) noexcept;

    void ConfigureFilters() noexcept;
    float Filter(size_t channel, float sample) noexcept;
    float DynamicsGain(float left, float right, float threshold,
                       float ratio, float makeupGain,
                       float attackMilliseconds,
                       float releaseMilliseconds) noexcept;
    float ReadDelay(size_t channel, float delayFrames) const noexcept;

    VoiceEffectMode mode_ = VoiceEffectMode::Natural;
    std::array<ChannelFilters, kChannels> filters_{};
    std::array<std::array<float, kDelayCapacity>, kChannels> delay_{};
    size_t delayWriteFrame_ = 0;
    float envelope_ = 0.0F;
    float dynamicsGain_ = 1.0F;
    float robotPhase_ = 0.0F;
    float pitchPhase_ = 0.0F;
};
