// Swap chain consumer: acquires every frame Windows renders for a monitor and gives it straight back.

#include "driver.h"

#include <avrt.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace lumen_vdd {

FrameDrain::FrameDrain(IDDCX_SWAPCHAIN swapchain, LUID render_adapter, HANDLE frame_event)
    : swapchain_(swapchain), render_adapter_(render_adapter), frame_event_(frame_event) {
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    if (thread_ == nullptr) {
        // Without a consumer the swap chain must go, so that Windows offers a new one.
        Log("CreateThread failed: %lu", GetLastError());
        WdfObjectDelete(swapchain_);
    }
}

FrameDrain::~FrameDrain() {
    if (thread_ != nullptr) {
        SetEvent(stop_event_);
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
    }
    if (stop_event_ != nullptr) {
        CloseHandle(stop_event_);
    }
}

DWORD CALLBACK FrameDrain::ThreadProc(void* param) {
    auto* self = static_cast<FrameDrain*>(param);
    DWORD task_index = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Distribution", &task_index);
    self->Drain();
    // Deleting the swap chain when the loop ends (failure or stop) lets Windows hand out a new one if still needed.
    WdfObjectDelete(self->swapchain_);
    if (task != nullptr) {
        AvRevertMmThreadCharacteristics(task);
    }
    return 0;
}

void FrameDrain::Drain() {
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> adapter;
    if (SUCCEEDED(hr)) {
        hr = factory->EnumAdapterByLuid(render_adapter_, IID_PPV_ARGS(&adapter));
    }
    ComPtr<ID3D11Device> device;
    if (SUCCEEDED(hr)) {
        hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr);
    }
    ComPtr<IDXGIDevice> dxgi_device;
    if (SUCCEEDED(hr)) {
        hr = device.As(&dxgi_device);
    }
    if (SUCCEEDED(hr)) {
        IDARG_IN_SWAPCHAINSETDEVICE set_device = {};
        set_device.pDevice = dxgi_device.Get();
        hr = IddCxSwapChainSetDevice(swapchain_, &set_device);
    }
    if (FAILED(hr)) {
        Log("swap chain setup on render adapter %08lx:%08lx failed: 0x%08lx", render_adapter_.HighPart,
            render_adapter_.LowPart, hr);
        return;
    }

    for (;;) {
        IDARG_OUT_RELEASEANDACQUIREBUFFER buffer = {};
        hr = IddCxSwapChainReleaseAndAcquireBuffer(swapchain_, &buffer);
        if (hr == E_PENDING) {
            HANDLE events[] = {frame_event_, stop_event_};
            const DWORD wait = WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, 100);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_TIMEOUT) {
                continue;
            }
            return;  // stop requested or wait failed
        }
        if (FAILED(hr)) {
            return;  // swap chain abandoned (mode change, monitor removed, render adapter lost)
        }
        buffer.MetaData.pSurface->Release();
        if (FAILED(IddCxSwapChainFinishedProcessingFrame(swapchain_))) {
            return;
        }
    }
}

}  // namespace lumen_vdd
