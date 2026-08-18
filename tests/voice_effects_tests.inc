#pragma once

#include "voice_effects.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

// Optional header-style tests. Existing test runners can call
// voice_effects_tests::Run(Check), where Check is callable as
// check(bool, const char*). To compile this file directly, define
// CHROMEMIC_VOICE_EFFECTS_STANDALONE_TEST and force C++ input (/TP with MSVC).
namespace voice_effects_tests {

constexpr double kTestPi = 3.1415926535897932384626433832795;

inline std::vector<int16_t> MakeVoiceLike(size_t frames) {
    std::vector<int16_t> samples(frames * VoiceEffectsProcessor::kChannels);
    for (size_t frame = 0; frame < frames; ++frame) {
        const double time = static_cast<double>(frame) /
                            static_cast<double>(VoiceEffectsProcessor::kSampleRate);
        const double value = 0.18 * std::sin(2.0 * kTestPi * 173.0 * time) +
                             0.11 * std::sin(2.0 * kTestPi * 997.0 * time) +
                             0.05 * std::sin(2.0 * kTestPi * 3121.0 * time);
        const auto encoded = static_cast<int16_t>(std::lround(value * 32767.0));
        samples[frame * 2] = encoded;
        samples[frame * 2 + 1] = encoded;
    }
    return samples;
}

inline std::vector<int16_t> MakeTone(double frequencyHz, double amplitude,
                                     size_t frames) {
    std::vector<int16_t> samples(frames * VoiceEffectsProcessor::kChannels);
    for (size_t frame = 0; frame < frames; ++frame) {
        const double phase = 2.0 * kTestPi * frequencyHz *
            static_cast<double>(frame) /
            static_cast<double>(VoiceEffectsProcessor::kSampleRate);
        const auto encoded = static_cast<int16_t>(
            std::lround(std::sin(phase) * amplitude * 32767.0));
        samples[frame * 2] = encoded;
        samples[frame * 2 + 1] = encoded;
    }
    return samples;
}

inline double RootMeanSquare(const std::vector<int16_t>& samples,
                             size_t beginFrame, size_t endFrame) {
    if (beginFrame >= endFrame || endFrame * 2 > samples.size()) {
        return 0.0;
    }
    double sum = 0.0;
    for (size_t frame = beginFrame; frame < endFrame; ++frame) {
        const double sample = static_cast<double>(samples[frame * 2]) / 32768.0;
        sum += sample * sample;
    }
    return std::sqrt(sum / static_cast<double>(endFrame - beginFrame));
}

inline double SpectralAmplitude(const std::vector<int16_t>& samples,
                                size_t beginFrame, size_t endFrame,
                                double frequencyHz) {
    if (beginFrame >= endFrame || endFrame * 2 > samples.size()) {
        return 0.0;
    }
    const size_t count = endFrame - beginFrame;
    double real = 0.0;
    double imaginary = 0.0;
    double windowSum = 0.0;
    for (size_t offset = 0; offset < count; ++offset) {
        const double window = count > 1
            ? 0.5 - 0.5 * std::cos(2.0 * kTestPi *
                static_cast<double>(offset) / static_cast<double>(count - 1))
            : 1.0;
        const double sample = static_cast<double>(
            samples[(beginFrame + offset) * 2]) / 32768.0;
        const double phase = 2.0 * kTestPi * frequencyHz *
            static_cast<double>(offset) /
            static_cast<double>(VoiceEffectsProcessor::kSampleRate);
        real += sample * window * std::cos(phase);
        imaginary -= sample * window * std::sin(phase);
        windowSum += window;
    }
    return windowSum > 0.0 ? 2.0 * std::hypot(real, imaginary) / windowSum : 0.0;
}

template <typename CheckFunction>
void Run(CheckFunction&& check) {
    constexpr std::array<VoiceEffectMode, 6> modes = {
        VoiceEffectMode::Natural,
        VoiceEffectMode::ClearMic,
        VoiceEffectMode::Broadcast,
        VoiceEffectMode::Radio,
        VoiceEffectMode::Robot,
        VoiceEffectMode::DeepTone
    };

    VoiceEffectsProcessor noOp;
    const VoiceEffectsResult nullResult = noOp.Process(nullptr, 100);
    check(nullResult.peak == 0.0F && !nullResult.limiting,
          "voice effects null input is a safe no-op");

    VoiceEffectsProcessor invalid(static_cast<VoiceEffectMode>(255));
    check(invalid.Mode() == VoiceEffectMode::Natural,
          "invalid voice effect fails closed to Natural");

    std::vector<int16_t> naturalSamples = {
        1000, -1000, 12000, -12000, 20000, -20000, 0, 0
    };
    const std::vector<int16_t> naturalOriginal = naturalSamples;
    VoiceEffectsProcessor natural(VoiceEffectMode::Natural);
    natural.Process(naturalSamples.data(), naturalSamples.size() / 2);
    bool transparent = true;
    for (size_t index = 0; index < naturalSamples.size(); ++index) {
        transparent = transparent &&
            std::abs(static_cast<int>(naturalSamples[index]) -
                     static_cast<int>(naturalOriginal[index])) <= 1;
    }
    check(transparent,
          "Natural is bit-near-transparent below the final limiter knee");

    const int maximumEncoded = static_cast<int>(
        std::ceil(VoiceEffectsProcessor::kOutputCeiling * 32767.0F));
    for (const VoiceEffectMode mode : modes) {
        std::vector<int16_t> adversarial(8192);
        for (size_t index = 0; index < adversarial.size(); ++index) {
            adversarial[index] = (index & 1U) != 0U
                ? std::numeric_limits<int16_t>::max()
                : std::numeric_limits<int16_t>::min();
        }
        VoiceEffectsProcessor processor(mode);
        const VoiceEffectsResult result = processor.Process(
            adversarial.data(), adversarial.size() / 2);
        bool bounded = std::isfinite(result.peak) &&
                       result.peak <= VoiceEffectsProcessor::kOutputCeiling + 1.0e-6F;
        for (const int16_t sample : adversarial) {
            bounded = bounded && std::abs(static_cast<int>(sample)) <= maximumEncoded;
        }
        check(bounded, "every voice mode preserves -1 dBFS final headroom");
    }

    const std::vector<int16_t> original = MakeVoiceLike(
        VoiceEffectsProcessor::kSampleRate * 2U);
    for (const VoiceEffectMode mode : modes) {
        VoiceEffectsProcessor processor(mode);
        std::vector<int16_t> first = original;
        std::vector<int16_t> second = original;
        processor.Process(first.data(), first.size() / 2);
        processor.Reset();
        processor.Process(second.data(), second.size() / 2);
        check(first == second,
              "Reset makes each voice effect byte-for-byte deterministic");

        VoiceEffectsProcessor wholeProcessor(mode);
        VoiceEffectsProcessor chunkProcessor(mode);
        std::vector<int16_t> whole = original;
        std::vector<int16_t> chunked = original;
        wholeProcessor.Process(whole.data(), whole.size() / 2);
        constexpr std::array<size_t, 5> chunkPattern = {1, 17, 127, 441, 997};
        size_t offset = 0;
        size_t chunkIndex = 0;
        while (offset < chunked.size() / 2) {
            const size_t count = std::min(chunkPattern[chunkIndex % chunkPattern.size()],
                                          chunked.size() / 2 - offset);
            chunkProcessor.Process(chunked.data() + offset * 2, count);
            offset += count;
            ++chunkIndex;
        }
        check(whole == chunked,
              "voice effect output is independent of callback chunking");

        bool stereoCoherent = true;
        for (size_t frame = 0; frame < first.size() / 2; ++frame) {
            stereoCoherent = stereoCoherent && first[frame * 2] == first[frame * 2 + 1];
        }
        check(stereoCoherent,
              "linked dynamics and modulation preserve coherent stereo voice");
    }

    std::vector<int16_t> naturalReference = original;
    VoiceEffectsProcessor naturalReferenceProcessor(VoiceEffectMode::Natural);
    naturalReferenceProcessor.Process(naturalReference.data(), naturalReference.size() / 2);
    for (size_t modeIndex = 1; modeIndex < modes.size(); ++modeIndex) {
        std::vector<int16_t> effected = original;
        VoiceEffectsProcessor processor(modes[modeIndex]);
        processor.Process(effected.data(), effected.size() / 2);
        uint64_t absoluteDifference = 0;
        for (size_t index = 0; index < effected.size(); ++index) {
            absoluteDifference += static_cast<uint64_t>(std::abs(
                static_cast<int>(effected[index]) -
                static_cast<int>(naturalReference[index])));
        }
        check(absoluteDifference > effected.size() * 64ULL,
              "each named voice mode is materially distinct from Natural");
    }

    const size_t toneFrames = VoiceEffectsProcessor::kSampleRate * 2U;
    std::vector<int16_t> radioBass = MakeTone(100.0, 0.12, toneFrames);
    std::vector<int16_t> radioVoice = MakeTone(1000.0, 0.12, toneFrames);
    VoiceEffectsProcessor radioBassProcessor(VoiceEffectMode::Radio);
    VoiceEffectsProcessor radioVoiceProcessor(VoiceEffectMode::Radio);
    radioBassProcessor.Process(radioBass.data(), toneFrames);
    radioVoiceProcessor.Process(radioVoice.data(), toneFrames);
    const double bassRms = RootMeanSquare(
        radioBass, VoiceEffectsProcessor::kSampleRate, toneFrames);
    const double voiceRms = RootMeanSquare(
        radioVoice, VoiceEffectsProcessor::kSampleRate, toneFrames);
    check(voiceRms > bassRms * 3.0,
          "Radio applies the documented telephone-style band pass");

    std::vector<int16_t> deepTone = MakeTone(440.0, 0.12, toneFrames);
    VoiceEffectsProcessor deepProcessor(VoiceEffectMode::DeepTone);
    deepProcessor.Process(deepTone.data(), toneFrames);
    double shiftedBandAmplitude = 0.0;
    for (double frequency = 325.0; frequency <= 360.0; frequency += 0.5) {
        shiftedBandAmplitude = std::max(shiftedBandAmplitude, SpectralAmplitude(
            deepTone, VoiceEffectsProcessor::kSampleRate, toneFrames, frequency));
    }
    const double dryAmplitude = SpectralAmplitude(
        deepTone, VoiceEffectsProcessor::kSampleRate, toneFrames, 440.0);
    check(shiftedBandAmplitude > dryAmplitude * 2.0,
          "DeepTone moves dominant pitch energy down by roughly four to five semitones");
}

} // namespace voice_effects_tests

#if defined(CHROMEMIC_VOICE_EFFECTS_STANDALONE_TEST)
int main() {
    int failures = 0;
    voice_effects_tests::Run([&failures](bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    });
    if (failures == 0) {
        std::cout << "All voice-effects tests passed.\n";
    }
    return failures == 0 ? 0 : 1;
}
#endif
