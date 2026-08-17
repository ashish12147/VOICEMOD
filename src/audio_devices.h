#pragma once

#include <string>
#include <vector>

struct AudioDeviceInfo {
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
    bool isLikelyVirtual = false;
    bool isGenericCable = false;
    bool isVoicemod = false;
};

struct AudioDeviceInventory {
    std::vector<AudioDeviceInfo> playback;
    std::vector<AudioDeviceInfo> recording;
};

bool EnumerateAudioDevices(AudioDeviceInventory& inventory, std::wstring& errorMessage);
bool IsLikelyVirtualAudioName(const std::wstring& name);
bool IsGenericVirtualCableName(const std::wstring& name);
bool IsVoicemodAudioName(const std::wstring& name);
std::wstring FriendlyHResult(long result);
