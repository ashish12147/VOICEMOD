#pragma once

#include "voice_effects.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

enum class RouterState {
    Stopped,
    Starting,
    Running,
    Error
};

struct RouteConfig {
    uint32_t sourceProcessId = 0;
    std::wstring sourceName;
    uint64_t sourceProcessCreationTime = 0;
    std::wstring sourceExecutablePath;
    std::wstring destinationId;
    std::wstring destinationName;
    bool includeMicrophone = false;
    std::wstring microphoneId;
    std::wstring microphoneName;
    bool enableMonitor = false;
    std::wstring monitorId;
    std::wstring monitorName;
    float applicationGainDb = 0.0F;
    float microphoneGainDb = 0.0F;
    VoiceEffectMode voiceEffect = VoiceEffectMode::Natural;
    bool muted = false;
};

struct RouterSnapshot {
    RouterState state = RouterState::Stopped;
    std::wstring message = L"Ready";
    std::wstring sourceName;
    std::wstring microphoneName;
    std::wstring destinationName;
    std::wstring monitorName;
    float peak = 0.0F;
    float applicationPeak = 0.0F;
    float microphonePeak = 0.0F;
    bool muted = false;
    bool limiting = false;
    bool monitorActive = false;
    std::wstring monitorMessage;
    uint64_t droppedFrames = 0;
    uint64_t underrunFrames = 0;
    uint32_t queuedMilliseconds = 0;
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint32_t latencyMilliseconds = 0;
};

class AudioRouter {
public:
    AudioRouter() = default;
    ~AudioRouter();

    AudioRouter(const AudioRouter&) = delete;
    AudioRouter& operator=(const AudioRouter&) = delete;

    bool Start(const RouteConfig& config, std::wstring& errorMessage);
    void Stop();
    void SetApplicationGainDb(float gainDb) noexcept;
    void SetMicrophoneGainDb(float gainDb) noexcept;
    void SetVoiceEffect(VoiceEffectMode mode) noexcept;
    void SetMuted(bool muted) noexcept;
    [[nodiscard]] RouterSnapshot Snapshot() const;

private:
    void Worker(RouteConfig config);
    void SetSnapshotState(RouterState state, const std::wstring& message);

    mutable std::mutex lifecycleMutex_;
    std::condition_variable lifecycleCondition_;
    std::thread worker_;
    bool stopping_ = false;
    std::atomic_bool stopRequested_ = false;
    std::atomic<float> applicationGainDb_ = 0.0F;
    std::atomic<float> microphoneGainDb_ = 0.0F;
    std::atomic<uint8_t> voiceEffectMode_ = static_cast<uint8_t>(VoiceEffectMode::Natural);
    std::atomic_bool muted_ = false;

    mutable std::mutex snapshotMutex_;
    RouterSnapshot snapshot_;
};
