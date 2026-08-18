#include "dsp.h"

#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {

constexpr float kLimiterCeiling = 0.89125094F; // -1 dBFS
constexpr float kLimiterKnee = 0.80F;

template <typename T>
T ReadUnaligned(const std::byte* data) noexcept {
    T value{};
    std::memcpy(&value, data, sizeof(T));
    return value;
}

template <typename T>
void WriteUnaligned(std::byte* data, T value) noexcept {
    std::memcpy(data, &value, sizeof(T));
}

} // namespace

DspResult MixPcm16Stereo(const std::byte* application,
                         const std::byte* microphone,
                         std::byte* output,
                         uint32_t frames,
                         bool muted) noexcept {
    DspResult result;
    if (output == nullptr || frames == 0) {
        return result;
    }
    constexpr size_t kChannels = 2;
    constexpr size_t kBytesPerSample = sizeof(int16_t);
    constexpr size_t kBytesPerFrame = kChannels * kBytesPerSample;
    if (muted) {
        std::memset(output, 0, static_cast<size_t>(frames) * kBytesPerFrame);
        return result;
    }

    for (uint32_t frame = 0; frame < frames; ++frame) {
        for (size_t channel = 0; channel < kChannels; ++channel) {
            const size_t offset = static_cast<size_t>(frame) * kBytesPerFrame +
                                  channel * kBytesPerSample;
            int16_t applicationSample = 0;
            int16_t microphoneSample = 0;
            if (application != nullptr) {
                applicationSample = ReadUnaligned<int16_t>(application + offset);
            }
            if (microphone != nullptr) {
                microphoneSample = ReadUnaligned<int16_t>(microphone + offset);
            }
            const float mixed = static_cast<float>(applicationSample) / 32768.0F +
                                static_cast<float>(microphoneSample) / 32768.0F;
            const float limited = AudioProcessor::LimitForMix(mixed, result.limiting);
            result.peak = std::max(result.peak, std::abs(limited));
            const auto encoded = static_cast<int16_t>(std::lround(
                std::clamp(limited, -1.0F, 1.0F) * 32767.0F));
            WriteUnaligned(output + offset, encoded);
        }
    }
    return result;
}

AudioProcessor::AudioProcessor(const WAVEFORMATEX& format, float initialGainDb)
    : channels_(format.nChannels),
      bytesPerSample_(format.nChannels == 0 ? 0 : static_cast<uint16_t>(format.nBlockAlign / format.nChannels)),
      validBits_(format.wBitsPerSample),
      blockAlign_(format.nBlockAlign),
      sampleRate_(format.nSamplesPerSec),
      currentGain_(DbToLinear(ClampGainDb(initialGainDb))) {
    WORD tag = format.wFormatTag;
    GUID subtype{};
    if (tag == WAVE_FORMAT_EXTENSIBLE && format.cbSize >= 22) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(&format);
        subtype = extensible->SubFormat;
        if (extensible->Samples.wValidBitsPerSample != 0) {
            validBits_ = extensible->Samples.wValidBitsPerSample;
        }
        if (subtype == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            tag = WAVE_FORMAT_IEEE_FLOAT;
        } else if (subtype == KSDATAFORMAT_SUBTYPE_PCM) {
            tag = WAVE_FORMAT_PCM;
        }
    }

    if (validBits_ < 2 || validBits_ > format.wBitsPerSample) {
        return;
    }
    paddingBits_ = static_cast<uint16_t>(format.wBitsPerSample - validBits_);

    if (tag == WAVE_FORMAT_IEEE_FLOAT && format.wBitsPerSample == 32 && validBits_ == 32 && bytesPerSample_ == 4) {
        sampleKind_ = SampleKind::Float32;
    } else if (tag == WAVE_FORMAT_PCM && format.wBitsPerSample == 16 && bytesPerSample_ == 2) {
        sampleKind_ = SampleKind::Pcm16;
    } else if (tag == WAVE_FORMAT_PCM && format.wBitsPerSample == 24 && bytesPerSample_ == 3) {
        sampleKind_ = SampleKind::Pcm24;
    } else if (tag == WAVE_FORMAT_PCM && format.wBitsPerSample == 32 && bytesPerSample_ == 4) {
        sampleKind_ = SampleKind::Pcm32;
    }
}

bool AudioProcessor::IsSupported() const noexcept {
    return sampleKind_ != SampleKind::Unsupported && channels_ != 0 && blockAlign_ != 0 && sampleRate_ != 0;
}

float AudioProcessor::ClampGainDb(float gainDb) noexcept {
    if (!std::isfinite(gainDb)) {
        return 0.0F;
    }
    return std::clamp(gainDb, -24.0F, 6.0F);
}

float AudioProcessor::DbToLinear(float gainDb) noexcept {
    return std::pow(10.0F, gainDb / 20.0F);
}

float AudioProcessor::Limit(float sample, bool& limiting) noexcept {
    if (!std::isfinite(sample)) {
        limiting = true;
        return 0.0F;
    }

    const float magnitude = std::abs(sample);
    if (magnitude <= kLimiterKnee) {
        return sample;
    }

    limiting = true;
    const float span = kLimiterCeiling - kLimiterKnee;
    const float compressed = kLimiterKnee + span * (1.0F - std::exp(-(magnitude - kLimiterKnee) / span));
    return std::copysign(std::min(compressed, kLimiterCeiling), sample);
}

float AudioProcessor::LimitForMix(float sample, bool& limiting) noexcept {
    return Limit(sample, limiting);
}

float AudioProcessor::Decode(const std::byte* sample) const noexcept {
    switch (sampleKind_) {
    case SampleKind::Float32:
        return ReadUnaligned<float>(sample);
    case SampleKind::Pcm16:
        return static_cast<float>(ReadUnaligned<int16_t>(sample) >> paddingBits_) /
               std::ldexp(1.0F, validBits_ - 1);
    case SampleKind::Pcm24: {
        int32_t value = static_cast<int32_t>(static_cast<uint8_t>(sample[0])) |
                        (static_cast<int32_t>(static_cast<uint8_t>(sample[1])) << 8) |
                        (static_cast<int32_t>(static_cast<uint8_t>(sample[2])) << 16);
        if ((value & 0x00800000) != 0) {
            value |= static_cast<int32_t>(0xFF000000);
        }
        value >>= paddingBits_;
        return static_cast<float>(value) / std::ldexp(1.0F, validBits_ - 1);
    }
    case SampleKind::Pcm32:
        return static_cast<float>(static_cast<double>(ReadUnaligned<int32_t>(sample) >> paddingBits_) /
                                  std::ldexp(1.0, validBits_ - 1));
    default:
        return 0.0F;
    }
}

void AudioProcessor::Encode(std::byte* sample, float value) const noexcept {
    if (!std::isfinite(value)) {
        value = 0.0F;
    }
    value = std::clamp(value, -1.0F, 1.0F);
    switch (sampleKind_) {
    case SampleKind::Float32:
        WriteUnaligned(sample, value);
        break;
    case SampleKind::Pcm16: {
        const int64_t maximum = (int64_t{1} << (validBits_ - 1)) - 1;
        const auto encoded = static_cast<int16_t>(
            static_cast<int64_t>(std::llround(static_cast<double>(value) * maximum)) *
            (int64_t{1} << paddingBits_));
        WriteUnaligned(sample, encoded);
        break;
    }
    case SampleKind::Pcm24: {
        const int64_t maximum = (int64_t{1} << (validBits_ - 1)) - 1;
        const int32_t encoded = static_cast<int32_t>(
            static_cast<int64_t>(std::llround(static_cast<double>(value) * maximum)) *
            (int64_t{1} << paddingBits_));
        sample[0] = static_cast<std::byte>(encoded & 0xFF);
        sample[1] = static_cast<std::byte>((encoded >> 8) & 0xFF);
        sample[2] = static_cast<std::byte>((encoded >> 16) & 0xFF);
        break;
    }
    case SampleKind::Pcm32: {
        const int64_t maximum = (int64_t{1} << (validBits_ - 1)) - 1;
        const int64_t scaled = static_cast<int64_t>(std::llround(static_cast<double>(value) * maximum)) *
                               (int64_t{1} << paddingBits_);
        WriteUnaligned(sample, static_cast<int32_t>(scaled));
        break;
    }
    default:
        break;
    }
}

bool AudioProcessor::ConvertFrameCount(const std::byte* input, uint32_t inputFrames,
                                       std::byte* output, uint32_t outputFrames) const noexcept {
    if (!IsSupported() || input == nullptr || output == nullptr || inputFrames == 0 || outputFrames == 0) {
        return false;
    }
    if (inputFrames == outputFrames) {
        std::memcpy(output, input, static_cast<size_t>(inputFrames) * blockAlign_);
        return true;
    }

    const double scale = outputFrames > 1
        ? static_cast<double>(inputFrames - 1) / static_cast<double>(outputFrames - 1)
        : 0.0;
    for (uint32_t outputFrame = 0; outputFrame < outputFrames; ++outputFrame) {
        const double position = static_cast<double>(outputFrame) * scale;
        const uint32_t firstFrame = static_cast<uint32_t>(position);
        const uint32_t secondFrame = std::min(firstFrame + 1, inputFrames - 1);
        const float fraction = static_cast<float>(position - static_cast<double>(firstFrame));
        for (uint16_t channel = 0; channel < channels_; ++channel) {
            const std::byte* first = input + static_cast<size_t>(firstFrame) * blockAlign_ +
                                     static_cast<size_t>(channel) * bytesPerSample_;
            const std::byte* second = input + static_cast<size_t>(secondFrame) * blockAlign_ +
                                      static_cast<size_t>(channel) * bytesPerSample_;
            std::byte* destination = output + static_cast<size_t>(outputFrame) * blockAlign_ +
                                     static_cast<size_t>(channel) * bytesPerSample_;
            const float value = Decode(first) + (Decode(second) - Decode(first)) * fraction;
            Encode(destination, value);
        }
    }
    return true;
}

DspResult AudioProcessor::Process(std::byte* data, uint32_t frames, float targetGainDb, bool muted) {
    DspResult result;
    if (!IsSupported() || data == nullptr || frames == 0) {
        return result;
    }

    if (muted) {
        std::memset(data, 0, static_cast<size_t>(frames) * blockAlign_);
        return result;
    }

    const float targetGain = DbToLinear(ClampGainDb(targetGainDb));
    const float smoothing = 1.0F - std::exp(-1.0F / (0.008F * static_cast<float>(sampleRate_)));
    for (uint32_t frame = 0; frame < frames; ++frame) {
        currentGain_ += (targetGain - currentGain_) * smoothing;
        std::byte* frameData = data + static_cast<size_t>(frame) * blockAlign_;
        for (uint16_t channel = 0; channel < channels_; ++channel) {
            std::byte* sample = frameData + static_cast<size_t>(channel) * bytesPerSample_;
            const float scaled = Decode(sample) * currentGain_;
            const float limited = Limit(scaled, result.limiting);
            result.peak = std::max(result.peak, std::abs(limited));
            Encode(sample, limited);
        }
    }
    return result;
}

DriftCompensator::DriftCompensator(size_t targetFrames, size_t deadbandFrames) noexcept
    : targetFrames_(std::max<size_t>(1, targetFrames)),
      deadbandFrames_(std::max<size_t>(1, deadbandFrames)) {
}

uint32_t DriftCompensator::InputFrames(size_t queuedFrames, uint32_t outputFrames) noexcept {
    if (outputFrames == 0) {
        return 0;
    }

    const int64_t queueError = static_cast<int64_t>(queuedFrames) - static_cast<int64_t>(targetFrames_);
    const uint64_t absoluteError = queueError < 0
        ? static_cast<uint64_t>(-queueError)
        : static_cast<uint64_t>(queueError);
    if (absoluteError > deadbandFrames_) {
        const double normalizedError = static_cast<double>(queueError) / static_cast<double>(targetFrames_);
        const double rateCorrection = std::clamp(normalizedError * 0.0015, -0.001, 0.001);
        frameAccumulator_ += rateCorrection * static_cast<double>(outputFrames);
    }

    if (frameAccumulator_ >= 1.0 && queuedFrames >= static_cast<size_t>(outputFrames) + 1) {
        frameAccumulator_ -= 1.0;
        return outputFrames + 1;
    }
    if (frameAccumulator_ <= -1.0 && outputFrames > 1 &&
        queuedFrames >= static_cast<size_t>(outputFrames) - 1) {
        frameAccumulator_ += 1.0;
        return outputFrames - 1;
    }
    return outputFrames;
}

void DriftCompensator::Reset() noexcept {
    frameAccumulator_ = 0.0;
}

AudioFrameRing::AudioFrameRing(size_t capacityFrames, size_t bytesPerFrame)
    : storage_(capacityFrames * bytesPerFrame),
      capacityFrames_(capacityFrames),
      bytesPerFrame_(bytesPerFrame) {
}

size_t AudioFrameRing::Push(const std::byte* source, size_t frames) {
    if (source == nullptr || frames == 0 || capacityFrames_ == 0 || bytesPerFrame_ == 0) {
        return 0;
    }

    size_t dropped = 0;
    if (frames >= capacityFrames_) {
        dropped = sizeFrames_ + (frames - capacityFrames_);
        source += (frames - capacityFrames_) * bytesPerFrame_;
        frames = capacityFrames_;
        readFrame_ = 0;
        writeFrame_ = 0;
        sizeFrames_ = 0;
    } else if (sizeFrames_ + frames > capacityFrames_) {
        dropped = sizeFrames_ + frames - capacityFrames_;
        readFrame_ = (readFrame_ + dropped) % capacityFrames_;
        sizeFrames_ -= dropped;
    }

    CopyIn(source, frames);
    return dropped;
}

size_t AudioFrameRing::Pop(std::byte* destination, size_t frames) {
    if (destination == nullptr || frames == 0 || capacityFrames_ == 0 || bytesPerFrame_ == 0) {
        return 0;
    }
    const size_t available = std::min(frames, sizeFrames_);
    CopyOut(destination, available);
    return available;
}

void AudioFrameRing::Clear() noexcept {
    readFrame_ = 0;
    writeFrame_ = 0;
    sizeFrames_ = 0;
}

void AudioFrameRing::CopyIn(const std::byte* source, size_t frames) {
    const size_t first = std::min(frames, capacityFrames_ - writeFrame_);
    std::memcpy(storage_.data() + writeFrame_ * bytesPerFrame_, source, first * bytesPerFrame_);
    if (frames > first) {
        std::memcpy(storage_.data(), source + first * bytesPerFrame_, (frames - first) * bytesPerFrame_);
    }
    writeFrame_ = (writeFrame_ + frames) % capacityFrames_;
    sizeFrames_ += frames;
}

void AudioFrameRing::CopyOut(std::byte* destination, size_t frames) {
    const size_t first = std::min(frames, capacityFrames_ - readFrame_);
    std::memcpy(destination, storage_.data() + readFrame_ * bytesPerFrame_, first * bytesPerFrame_);
    if (frames > first) {
        std::memcpy(destination + first * bytesPerFrame_, storage_.data(), (frames - first) * bytesPerFrame_);
    }
    readFrame_ = (readFrame_ + frames) % capacityFrames_;
    sizeFrames_ -= frames;
}
