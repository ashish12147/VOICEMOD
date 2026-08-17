#include "app_processes.h"
#include "audio_devices.h"
#include "audio_router.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

int wmain() {
    AudioDeviceInventory inventory;
    std::vector<AppProcessInfo> applications;
    std::wstring error;
    if (!EnumerateDesktopApplications(applications, error) ||
        !EnumerateAudioDevices(inventory, error)) {
        std::wcerr << L"Enumeration failed: " << error << L"\n";
        return 1;
    }

    const AudioDeviceInfo* destination = nullptr;
    const int applicationIndex = FindPreferredApplicationIndex(applications);
    const AppProcessInfo* source = applicationIndex >= 0
        ? &applications[static_cast<size_t>(applicationIndex)] : nullptr;
    for (const auto& device : inventory.playback) {
        if (device.isGenericCable) {
            destination = &device;
            break;
        }
    }
    if (source == nullptr || destination == nullptr) {
        std::wcerr << L"Smoke test skipped: open Chrome and install a generic virtual cable first.\n";
        return 2;
    }

    std::wcout << L"Testing app-only route: " << BuildApplicationDisplayLabel(*source)
               << L" -> " << destination->name << L"\n";
    RouteConfig config;
    config.sourceProcessId = source->processId;
    config.sourceProcessCreationTime = source->creationTime;
    config.sourceExecutablePath = source->executablePath;
    config.sourceName = BuildApplicationDisplayLabel(*source);
    config.destinationId = destination->id;
    config.destinationName = destination->name;

    AudioRouter router;
    if (!router.Start(config, error)) {
        std::wcerr << L"Start failed: " << error << L"\n";
        return 3;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    const RouterSnapshot snapshot = router.Snapshot();
    std::wcout << L"State: " << static_cast<int>(snapshot.state) << L" — " << snapshot.message << L"\n";
    std::wcout << L"Format: " << snapshot.sampleRate << L" Hz, " << snapshot.channels
               << L" channels; estimated routing buffer " << snapshot.latencyMilliseconds << L" ms\n";
    router.Stop();
    return snapshot.state == RouterState::Running ? 0 : 4;
}
