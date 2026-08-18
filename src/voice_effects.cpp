#include "voice_effects.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr float kPi = 3.14159265358979323846F;
constexpr float kSqrtHalf = 0.70710678118654752440F;
constexpr float kLimiterKnee = 0.78F;

float MillisecondsCoefficient(float milliseconds) noexcept {
    if (!std::isfinite(milliseconds) || milliseconds <= 0.0F) {
        return 0.0F;
    }
    return std::exp(-1.0F /
        (milliseconds * 0.001F * static_cast<float>(VoiceEffectsProcessor::kSampleRate)));
}

} // namespace

void VoiceEffectsProcessor::Biquad::SetBypass() noexcept {
    b0 = 1.0F;
    b1 = 0.0F;
    b2 = 0.0F;
    a1 = 0.0F;
    a2 = 0.0F;
    ClearState();
}

void VoiceEffectsProcessor::Biquad::SetHighPass(float frequencyHz, float q) noexcept {
    const float frequency = std::clamp(frequencyHz, 10.0F,
        static_cast<float>(VoiceEffectsProcessor::kSampleRate) * 0.45F);
    const float safeQ = std::clamp(q, 0.1F, 10.0F);
    const float omega = 2.0F * kPi * frequency /
                        static_cast<float>(VoiceEffectsProcessor::kSampleRate);
    const float cosine = std::cos(omega);
    const float alpha = std::sin(omega) / (2.0F * safeQ);
    const float a0 = 1.0F + alpha;
    b0 = ((1.0F + cosine) * 0.5F) / a0;
    b1 = -(1.0F + cosine) / a0;
    b2 = b0;
    a1 = (-2.0F * cosine) / a0;
    a2 = (1.0F - alpha) / a0;
    ClearState();
}

void VoiceEffectsProcessor::Biquad::SetLowPass(float frequencyHz, float q) noexcept {
    const float frequency = std::clamp(frequencyHz, 10.0F,
        static_cast<float>(VoiceEffectsProcessor::kSampleRate) * 0.45F);
    const float safeQ = std::clamp(q, 0.1F, 10.0F);
    const float omega = 2.0F * kPi * frequency /
                        static_cast<float>(VoiceEffectsProcessor::kSampleRate);
    const float cosine = std::cos(omega);
    const float alpha = std::sin(omega) / (2.0F * safeQ);
    const float a0 = 1.0F + alpha;
    b0 = ((1.0F - cosine) * 0.5F) / a0;
    b1 = (1.0F - cosine) / a0;
    b2 = b0;
    a1 = (-2.0F * cosine) / a0;
    a2 = (1.0F - alpha) / a0;
    ClearState();
}

void VoiceEffectsProcessor::Biquad::SetPeaking(float frequencyHz, float q,
                                                float gainDb) noexcept {
    const float frequency = std::clamp(frequencyHz, 10.0F,
        static_cast<float>(VoiceEffectsProcessor::kSampleRate) * 0.45F);
    const float safeQ = std::clamp(q, 0.1F, 10.0F);
    const float safeGain = std::clamp(gainDb, -18.0F, 18.0F);
    const float amplitude = std::pow(10.0F, safeGain / 40.0F);
    const float omega = 2.0F * kPi * frequency /
                        static_cast<float>(VoiceEffectsProcessor::kSampleRate);
    const float cosine = std::cos(omega);
    const float alpha = std::sin(omega) / (2.0F * safeQ);
    const float a0 = 1.0F + alpha / amplitude;
    b0 = (1.0F + alpha * amplitude) / a0;
    b1 = (-2.0F * cosine) / a0;
    b2 = (1.0F - alpha * amplitude) / a0;
    a1 = b1;
    a2 = (1.0F - alpha / amplitude) / a0;
    ClearState();
}

void VoiceEffectsProcessor::Biquad::ClearState() noexcept {
    z1 = 0.0F;
    z2 = 0.0F;
}

float VoiceEffectsProcessor::Biquad::Process(float sample) noexcept {
    if (!std::isfinite(sample) || !std::isfinite(z1) || !std::isfinite(z2)) {
        ClearState();
        return 0.0F;
    }
    const float output = b0 * sample + z1;
    const float nextZ1 = b1 * sample - a1 * output + z2;
    const float nextZ2 = b2 * sample - a2 * output;
    if (!std::isfinite(output) || !std::isfinite(nextZ1) ||
        !std::isfinite(nextZ2) || std::abs(nextZ1) > 16.0F ||
        std::abs(nextZ2) > 16.0F) {
        ClearState();
        return 0.0F;
    }
    z1 = std::abs(nextZ1) < 1.0e-20F ? 0.0F : nextZ1;
    z2 = std::abs(nextZ2) < 1.0e-20F ? 0.0F : nextZ2;
    const float bounded = std::clamp(output, -8.0F, 8.0F);
    return std::abs(bounded) < 1.0e-20F ? 0.0F : bounded;
}

VoiceEffectsProcessor::VoiceEffectsProcessor(VoiceEffectMode mode) noexcept
    : mode_(NormalizeMode(mode)) {
    Reset();
}

VoiceEffectMode VoiceEffectsProcessor::NormalizeMode(VoiceEffectMode mode) noexcept {
    switch (mode) {
    case VoiceEffectMode::Natural:
    case VoiceEffectMode::ClearMic:
    case VoiceEffectMode::Broadcast:
    case VoiceEffectMode::Radio:
    case VoiceEffectMode::Robot:
    case VoiceEffectMode::DeepTone:
        return mode;
    default:
        return VoiceEffectMode::Natural;
    }
}

void VoiceEffectsProcessor::SetMode(VoiceEffectMode mode) noexcept {
    const VoiceEffectMode normalized = NormalizeMode(mode);
    if (normalized != mode_) {
        mode_ = normalized;
        Reset();
    }
}

void VoiceEffectsProcessor::Reset() noexcept {
    envelope_ = 0.0F;
    dynamicsGain_ = 1.0F;
    robotPhase_ = 0.0F;
    pitchPhase_ = 0.0F;
    delayWriteFrame_ = 0;
    for (auto& channel : delay_) {
        channel.fill(0.0F);
    }
    ConfigureFilters();
}

float VoiceEffectsProcessor::Decode(int16_t sample) noexcept {
    return static_cast<float>(sample) / 32768.0F;
}

float VoiceEffectsProcessor::Sanitize(float value) noexcept {
    if (!std::isfinite(value) || std::abs(value) < 1.0e-20F) {
        return 0.0F;
    }
    return std::clamp(value, -8.0F, 8.0F);
}

int16_t VoiceEffectsProcessor::Encode(float sample,
                                      VoiceEffectsResult& result) noexcept {
    if (!std::isfinite(sample)) {
        sample = 0.0F;
        result.limiting = true;
    }

    const float magnitude = std::abs(sample);
    float limited = sample;
    if (magnitude > kLimiterKnee) {
        result.limiting = true;
        const float span = kOutputCeiling - kLimiterKnee;
        const float compressed = kLimiterKnee + span *
            std::tanh((magnitude - kLimiterKnee) / span);
        limited = std::copysign(std::min(compressed, kOutputCeiling), sample);
    }
    limited = std::clamp(limited, -kOutputCeiling, kOutputCeiling);
    result.peak = std::max(result.peak, std::abs(limited));

    const long encoded = std::lround(limited * 32767.0F);
    return static_cast<int16_t>(std::clamp<long>(
        encoded, std::numeric_limits<int16_t>::min(),
        std::numeric_limits<int16_t>::max()));
}

void VoiceEffectsProcessor::ConfigureFilters() noexcept {
    for (auto& channel : filters_) {
        channel.highPass.SetBypass();
        channel.toneA.SetBypass();
        channel.toneB.SetBypass();
        channel.lowPass.SetBypass();

        switch (mode_) {
        case VoiceEffectMode::Natural:
            break;
        case VoiceEffectMode::ClearMic:
            channel.highPass.SetHighPass(90.0F, kSqrtHalf);
            channel.toneA.SetPeaking(3200.0F, 0.85F, 3.0F);
            channel.toneB.SetPeaking(220.0F, 0.8F, -1.5F);
            channel.lowPass.SetLowPass(14000.0F, kSqrtHalf);
            break;
        case VoiceEffectMode::Broadcast:
            channel.highPass.SetHighPass(70.0F, kSqrtHalf);
            channel.toneA.SetPeaking(170.0F, 0.8F, 2.5F);
            channel.toneB.SetPeaking(3600.0F, 0.9F, 1.5F);
            channel.lowPass.SetLowPass(10500.0F, kSqrtHalf);
            break;
        case VoiceEffectMode::Radio:
            channel.highPass.SetHighPass(300.0F, 0.8F);
            channel.toneA.SetPeaking(1500.0F, 0.75F, 4.0F);
            channel.toneB.SetPeaking(500.0F, 1.0F, -2.0F);
            channel.lowPass.SetLowPass(3400.0F, 0.8F);
            break;
        case VoiceEffectMode::Robot:
            channel.highPass.SetHighPass(120.0F, kSqrtHalf);
            channel.toneA.SetPeaking(2100.0F, 0.9F, 2.0F);
            channel.lowPass.SetLowPass(6500.0F, kSqrtHalf);
            break;
        case VoiceEffectMode::DeepTone:
            channel.highPass.SetHighPass(55.0F, kSqrtHalf);
            channel.toneA.SetPeaking(140.0F, 0.8F, 4.0F);
            channel.toneB.SetPeaking(3000.0F, 0.9F, -1.5F);
            channel.lowPass.SetLowPass(9000.0F, kSqrtHalf);
            break;
        }
    }
}

float VoiceEffectsProcessor::Filter(size_t channel, float sample) noexcept {
    ChannelFilters& filter = filters_[channel];
    float output = filter.highPass.Process(sample);
    output = filter.toneA.Process(output);
    output = filter.toneB.Process(output);
    return filter.lowPass.Process(output);
}

float VoiceEffectsProcessor::DynamicsGain(float left, float right,
                                           float threshold, float ratio,
                                           float makeupGain,
                                           float attackMilliseconds,
                                           float releaseMilliseconds) noexcept {
    const float level = std::max(std::abs(Sanitize(left)),
                                 std::abs(Sanitize(right)));
    const float envelopeCoefficient = level > envelope_
        ? MillisecondsCoefficient(attackMilliseconds)
        : MillisecondsCoefficient(releaseMilliseconds);
    envelope_ = envelopeCoefficient * envelope_ +
                (1.0F - envelopeCoefficient) * level;
    envelope_ = std::isfinite(envelope_) ? std::clamp(envelope_, 0.0F, 8.0F) : 0.0F;
    if (envelope_ < 1.0e-20F) {
        envelope_ = 0.0F;
    }

    float targetGain = 1.0F;
    const float safeThreshold = std::clamp(threshold, 0.001F, 1.0F);
    const float safeRatio = std::clamp(ratio, 1.0F, 20.0F);
    if (envelope_ > safeThreshold) {
        const float compressedLevel = safeThreshold *
            std::pow(envelope_ / safeThreshold, 1.0F / safeRatio);
        targetGain = compressedLevel / envelope_;
    }
    targetGain *= std::clamp(makeupGain, 0.0F, 4.0F);
    targetGain = std::isfinite(targetGain) ? std::clamp(targetGain, 0.0F, 4.0F) : 0.0F;

    const float gainCoefficient = targetGain < dynamicsGain_
        ? MillisecondsCoefficient(3.0F)
        : MillisecondsCoefficient(80.0F);
    dynamicsGain_ = gainCoefficient * dynamicsGain_ +
                    (1.0F - gainCoefficient) * targetGain;
    dynamicsGain_ = std::isfinite(dynamicsGain_)
        ? std::clamp(dynamicsGain_, 0.0F, 4.0F) : 0.0F;
    return dynamicsGain_;
}

float VoiceEffectsProcessor::ReadDelay(size_t channel,
                                       float delayFrames) const noexcept {
    const float safeDelay = std::clamp(delayFrames, 1.0F,
        static_cast<float>(kDelayCapacity - 2));
    float position = static_cast<float>(delayWriteFrame_) - safeDelay;
    if (position < 0.0F) {
        position += static_cast<float>(kDelayCapacity);
    }
    const size_t first = static_cast<size_t>(position) & kDelayMask;
    const size_t second = (first + 1) & kDelayMask;
    const float fraction = position - std::floor(position);
    return delay_[channel][first] +
           (delay_[channel][second] - delay_[channel][first]) * fraction;
}

VoiceEffectsResult VoiceEffectsProcessor::Process(
    int16_t* interleavedStereo, size_t frames) noexcept {
    VoiceEffectsResult result;
    if (interleavedStereo == nullptr || frames == 0) {
        return result;
    }

    constexpr float robotAngularStep =
        2.0F * kPi * 72.0F / static_cast<float>(kSampleRate);
    constexpr size_t robotDelayFrames = 331; // 7.51 ms metallic comb.
    constexpr float pitchRatio = 0.78F;       // About -4.3 semitones.
    constexpr float pitchMinimumDelay = 64.0F;
    constexpr float pitchDelaySpan = 960.0F;
    constexpr float pitchPhaseStep = (1.0F - pitchRatio) / pitchDelaySpan;
    constexpr float radioDrive = 2.2F;
    constexpr float radioNormalization = 0.82F / 0.97574313F; // tanh(2.2).
    constexpr float broadcastDrive = 1.25F;
    constexpr float broadcastNormalization = 0.86F / 0.84828365F; // tanh(1.25).

    for (size_t frame = 0; frame < frames; ++frame) {
        float left = Decode(interleavedStereo[frame * kChannels]);
        float right = Decode(interleavedStereo[frame * kChannels + 1]);

        if (mode_ != VoiceEffectMode::Natural) {
            left = Filter(0, left);
            right = Filter(1, right);
        }

        switch (mode_) {
        case VoiceEffectMode::Natural:
            break;
        case VoiceEffectMode::ClearMic: {
            const float gain = DynamicsGain(left, right, 0.34F, 2.0F,
                                            1.08F, 4.0F, 90.0F);
            left *= gain;
            right *= gain;
            break;
        }
        case VoiceEffectMode::Broadcast: {
            const float gain = DynamicsGain(left, right, 0.18F, 3.5F,
                                            1.55F, 3.0F, 120.0F);
            left = std::tanh(left * gain * broadcastDrive) * broadcastNormalization;
            right = std::tanh(right * gain * broadcastDrive) * broadcastNormalization;
            break;
        }
        case VoiceEffectMode::Radio: {
            const float gain = DynamicsGain(left, right, 0.12F, 4.0F,
                                            1.7F, 2.0F, 80.0F);
            left = std::tanh(left * gain * radioDrive) * radioNormalization;
            right = std::tanh(right * gain * radioDrive) * radioNormalization;
            break;
        }
        case VoiceEffectMode::Robot: {
            const size_t readFrame =
                (delayWriteFrame_ + kDelayCapacity - robotDelayFrames) & kDelayMask;
            const float delayedLeft = delay_[0][readFrame];
            const float delayedRight = delay_[1][readFrame];
            delay_[0][delayWriteFrame_] = Sanitize(left + 0.36F * delayedLeft);
            delay_[1][delayWriteFrame_] = Sanitize(right + 0.36F * delayedRight);
            const float gain = DynamicsGain(left, right, 0.20F, 2.2F,
                                            1.2F, 3.0F, 90.0F);
            const float carrier = std::sin(robotPhase_);
            const float modulation = 0.16F + 0.84F * carrier;
            left = (left + 0.55F * delayedLeft) * modulation * gain * 0.82F;
            right = (right + 0.55F * delayedRight) * modulation * gain * 0.82F;
            robotPhase_ += robotAngularStep;
            if (!std::isfinite(robotPhase_) || robotPhase_ >= 2.0F * kPi) {
                robotPhase_ = std::isfinite(robotPhase_)
                    ? robotPhase_ - 2.0F * kPi : 0.0F;
            }
            delayWriteFrame_ = (delayWriteFrame_ + 1) & kDelayMask;
            break;
        }
        case VoiceEffectMode::DeepTone: {
            delay_[0][delayWriteFrame_] = Sanitize(left);
            delay_[1][delayWriteFrame_] = Sanitize(right);
            float secondPhase = pitchPhase_ + 0.5F;
            if (secondPhase >= 1.0F) {
                secondPhase -= 1.0F;
            }
            const float firstWindow = 0.5F - 0.5F *
                std::cos(2.0F * kPi * pitchPhase_);
            const float secondWindow = 0.5F - 0.5F *
                std::cos(2.0F * kPi * secondPhase);
            const float firstDelay = pitchMinimumDelay +
                                     pitchPhase_ * pitchDelaySpan;
            const float secondDelay = pitchMinimumDelay +
                                      secondPhase * pitchDelaySpan;
            const float wetLeft = ReadDelay(0, firstDelay) * firstWindow +
                                  ReadDelay(0, secondDelay) * secondWindow;
            const float wetRight = ReadDelay(1, firstDelay) * firstWindow +
                                   ReadDelay(1, secondDelay) * secondWindow;
            const float gain = DynamicsGain(wetLeft, wetRight, 0.20F, 3.0F,
                                            1.35F, 4.0F, 120.0F);
            left = (0.12F * left + 0.92F * wetLeft) * gain;
            right = (0.12F * right + 0.92F * wetRight) * gain;
            pitchPhase_ += pitchPhaseStep;
            if (!std::isfinite(pitchPhase_) || pitchPhase_ >= 1.0F) {
                pitchPhase_ = std::isfinite(pitchPhase_)
                    ? pitchPhase_ - 1.0F : 0.0F;
            }
            delayWriteFrame_ = (delayWriteFrame_ + 1) & kDelayMask;
            break;
        }
        }

        interleavedStereo[frame * kChannels] = Encode(Sanitize(left), result);
        interleavedStereo[frame * kChannels + 1] = Encode(Sanitize(right), result);
    }
    return result;
}
