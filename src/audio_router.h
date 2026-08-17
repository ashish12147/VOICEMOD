#pragma once

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
    float gainDb = 0.0F;
    bool muted = false;
};

struct RouterSnapshot {
    RouterState state = RouterState::Stopped;
    std::wstring message = L"Ready";
    std::wstring sourceName;
    std::wstring destinationName;
    float peak = 0.0F;
    bool muted = false;
    bool limiting = false;
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
    void SetGainDb(float gainDb) noexcept;
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
    std::atomic<float> gainDb_ = 0.0F;
    std::atomic_bool muted_ = false;

    mutable std::mutex snapshotMutex_;
    RouterSnapshot snapshot_;
};
