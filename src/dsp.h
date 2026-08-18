#pragma once

#include <Windows.h>
#include <mmreg.h>

#include <cstddef>
#include <cstdint>
#include <vector>

struct DspResult {
    float peak = 0.0F;
    bool limiting = false;
};

// Mixes two optional stereo PCM16 sources and applies the final -1 dBFS safety
// limiter. Null source pointers are treated as silence.
DspResult MixPcm16Stereo(const std::byte* application,
                         const std::byte* microphone,
                         std::byte* output,
                         uint32_t frames,
                         bool muted) noexcept;

class AudioProcessor {
public:
    AudioProcessor(const WAVEFORMATEX& format, float initialGainDb = 0.0F);

    [[nodiscard]] bool IsSupported() const noexcept;
    DspResult Process(std::byte* data, uint32_t frames, float targetGainDb, bool muted);
    bool ConvertFrameCount(const std::byte* input, uint32_t inputFrames,
                           std::byte* output, uint32_t outputFrames) const noexcept;

    static float ClampGainDb(float gainDb) noexcept;
    static float DbToLinear(float gainDb) noexcept;
    static float LimitForMix(float sample, bool& limiting) noexcept;

private:
    enum class SampleKind { Unsupported, Float32, Pcm16, Pcm24, Pcm32 };

    static float Limit(float sample, bool& limiting) noexcept;
    float Decode(const std::byte* sample) const noexcept;
    void Encode(std::byte* sample, float value) const noexcept;

    SampleKind sampleKind_ = SampleKind::Unsupported;
    uint16_t channels_ = 0;
    uint16_t bytesPerSample_ = 0;
    uint16_t validBits_ = 0;
    uint16_t paddingBits_ = 0;
    uint16_t blockAlign_ = 0;
    uint32_t sampleRate_ = 0;
    float currentGain_ = 1.0F;
};

class DriftCompensator {
public:
    DriftCompensator(size_t targetFrames, size_t deadbandFrames) noexcept;

    [[nodiscard]] uint32_t InputFrames(size_t queuedFrames, uint32_t outputFrames) noexcept;
    void Reset() noexcept;

private:
    size_t targetFrames_ = 1;
    size_t deadbandFrames_ = 1;
    double frameAccumulator_ = 0.0;
};

class AudioFrameRing {
public:
    AudioFrameRing(size_t capacityFrames, size_t bytesPerFrame);

    size_t Push(const std::byte* source, size_t frames);
    size_t Pop(std::byte* destination, size_t frames);
    void Clear() noexcept;

    [[nodiscard]] size_t SizeFrames() const noexcept { return sizeFrames_; }
    [[nodiscard]] size_t CapacityFrames() const noexcept { return capacityFrames_; }

private:
    void CopyIn(const std::byte* source, size_t frames);
    void CopyOut(std::byte* destination, size_t frames);

    std::vector<std::byte> storage_;
    size_t capacityFrames_ = 0;
    size_t bytesPerFrame_ = 0;
    size_t readFrame_ = 0;
    size_t writeFrame_ = 0;
    size_t sizeFrames_ = 0;
};
