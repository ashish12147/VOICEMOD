#pragma once

#include <Windows.h>
#include <Audioclient.h>
#include <audioclientactivationparams.h>

#include <atomic>

[[nodiscard]] AUDIOCLIENT_ACTIVATION_PARAMS MakeProcessLoopbackActivationParams(
    DWORD processId) noexcept;
[[nodiscard]] WAVEFORMATEX MakeProcessLoopbackCaptureFormat() noexcept;

// Activates the Windows virtual process-loopback audio client. The returned interface
// has not yet been initialized or started. This follows Microsoft's asynchronous
// activation contract and waits for the completion callback before releasing it.
HRESULT ActivateProcessLoopbackClient(DWORD processId,
                                      const std::atomic_bool& stopRequested,
                                      HANDLE targetProcess,
                                      IAudioClient** audioClient) noexcept;
