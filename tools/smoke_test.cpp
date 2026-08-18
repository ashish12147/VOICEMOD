#include "app_processes.h"
#include "audio_devices.h"
#include "audio_router.h"

#include <chrono>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

int wmain(int argc, wchar_t** argv) {
    bool withMicrophone = false;
    bool withMonitor = false;
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--with-mic") {
            withMicrophone = true;
        } else if (argument == L"--with-monitor") {
            withMicrophone = true;
            withMonitor = true;
        } else {
            std::wcerr << L"Unknown option: " << argument << L"\n";
            return 1;
        }
    }

    AudioDeviceInventory inventory;
    std::vector<AppProcessInfo> applications;
    std::wstring error;
    if (!EnumerateDesktopApplications(applications, error) ||
        !EnumerateAudioDevices(inventory, error)) {
        std::wcerr << L"Enumeration failed: " << error << L"\n";
        return 1;
    }

    const AudioDeviceInfo* destination = nullptr;
    const AudioDeviceInfo* microphone = nullptr;
    const AudioDeviceInfo* monitor = nullptr;
    const int applicationIndex = FindPreferredApplicationIndex(applications);
    const AppProcessInfo* source = applicationIndex >= 0
        ? &applications[static_cast<size_t>(applicationIndex)] : nullptr;
    // Prefer VB-CABLE's canonical stereo playback side when the driver also
    // exposes its alternate multichannel endpoint.
    for (const auto& device : inventory.playback) {
        if (device.isGenericCable &&
            device.name.find(L"CABLE Input") != std::wstring::npos) {
            destination = &device;
            break;
        }
    }
    if (destination == nullptr) {
        for (const auto& device : inventory.playback) {
            if (device.isGenericCable) {
                destination = &device;
                break;
            }
        }
    }
    if (withMicrophone) {
        for (const auto& device : inventory.recording) {
            if (!device.isLikelyVirtual && !device.isGenericCable && !device.isVoicemod) {
                microphone = &device;
                break;
            }
        }
    }
    if (withMonitor) {
        for (const auto& device : inventory.playback) {
            if (!device.isLikelyVirtual && !device.isGenericCable && !device.isVoicemod &&
                (destination == nullptr || device.id != destination->id)) {
                monitor = &device;
                break;
            }
        }
    }
    if (source == nullptr || destination == nullptr) {
        std::wcerr << L"Smoke test skipped: open Chrome and install a generic virtual cable first.\n";
        return 2;
    }
    if ((withMicrophone && microphone == nullptr) || (withMonitor && monitor == nullptr)) {
        std::wcerr << L"Smoke test skipped: a physical microphone/headphone endpoint is unavailable.\n";
        return 2;
    }

    std::wcout << L"Testing route: " << BuildApplicationDisplayLabel(*source);
    if (microphone != nullptr) {
        std::wcout << L" + " << microphone->name;
    }
    std::wcout << L" -> " << destination->name;
    if (monitor != nullptr) {
        std::wcout << L"; mic loopback -> " << monitor->name;
    }
    std::wcout << std::endl;
    RouteConfig config;
    config.sourceProcessId = source->processId;
    config.sourceProcessCreationTime = source->creationTime;
    config.sourceExecutablePath = source->executablePath;
    config.sourceName = BuildApplicationDisplayLabel(*source);
    config.destinationId = destination->id;
    config.destinationName = destination->name;
    if (microphone != nullptr) {
        config.includeMicrophone = true;
        config.microphoneId = microphone->id;
        config.microphoneName = microphone->name;
        config.voiceEffect = VoiceEffectMode::ClearMic;
    }
    if (monitor != nullptr) {
        config.enableMonitor = true;
        config.monitorId = monitor->id;
        config.monitorName = monitor->name;
    }

    AudioRouter router;
    if (!router.Start(config, error)) {
        std::wcerr << L"Start failed: " << error << L"\n";
        return 3;
    }
    RouterSnapshot snapshot;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(12);
    do {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        snapshot = router.Snapshot();
        if (snapshot.state == RouterState::Error ||
            (snapshot.state == RouterState::Running &&
             (!withMonitor || snapshot.monitorActive))) {
            break;
        }
    } while (std::chrono::steady_clock::now() < deadline);
    std::wcerr << L"Smoke snapshot: state=" << static_cast<int>(snapshot.state)
               << L", monitor requested=" << (withMonitor ? L"yes" : L"no")
               << L", monitor active=" << (snapshot.monitorActive ? L"yes" : L"no")
               << L", message=" << snapshot.message << L", monitor message="
               << snapshot.monitorMessage << std::endl;
    std::wcout << L"State: " << static_cast<int>(snapshot.state) << L" — " << snapshot.message << L"\n";
    std::wcout << L"Format: " << snapshot.sampleRate << L" Hz, " << snapshot.channels
               << L" channels; estimated routing buffer " << snapshot.latencyMilliseconds << L" ms\n";
    router.Stop();
    const bool routePassed = snapshot.state == RouterState::Running;
    const bool monitorPassed = !withMonitor || snapshot.monitorActive;
    if (!monitorPassed) {
        std::wcerr << L"Monitor smoke failed: " << snapshot.monitorMessage << L"\n";
    }
    return routePassed && monitorPassed ? 0 : 4;
}
