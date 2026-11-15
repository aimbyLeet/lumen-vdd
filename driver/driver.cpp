// Driver entry, WDF/IddCx callbacks and the IOCTL interface.

#include "driver.h"

#include <winioctl.h>

using namespace lumen_vdd;

static_assert(IOCTL_LUMEN_VDD_GET_INFO == CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS),
              "contract");
static_assert(IOCTL_LUMEN_VDD_ADD_MONITOR ==
                  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA),
              "contract");
static_assert(IOCTL_LUMEN_VDD_REMOVE_MONITOR ==
                  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA),
              "contract");
static_assert(IOCTL_LUMEN_VDD_SET_RENDER_ADAPTER ==
                  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA),
              "contract");
static_assert(IOCTL_LUMEN_VDD_SET_MODES ==
                  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA),
              "contract");
static_assert(IOCTL_LUMEN_VDD_LIST_MONITORS == CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS),
              "contract");

namespace {

// EDID parsing has no device argument; there is one Lumen VDD device per driver host process.
Adapter* g_adapter = nullptr;

void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& signal, const LUMEN_VDD_MODE& mode, bool monitor_mode) {
    signal.pixelRate = static_cast<UINT64>(mode.width) * mode.height * mode.refresh_hz;
    signal.hSyncFreq.Numerator = mode.refresh_hz * mode.height;
    signal.hSyncFreq.Denominator = 1;
    signal.vSyncFreq.Numerator = mode.refresh_hz;
    signal.vSyncFreq.Denominator = 1;
    signal.activeSize.cx = signal.totalSize.cx = mode.width;
    signal.activeSize.cy = signal.totalSize.cy = mode.height;
    signal.AdditionalSignalInfo.videoStandard = 255;  // D3DKMDT_VSS_OTHER
    signal.AdditionalSignalInfo.vSyncFreqDivider = monitor_mode ? 0 : 1;
    signal.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
}

IDDCX_MONITOR_MODE MonitorMode(const LUMEN_VDD_MODE& mode, IDDCX_MONITOR_MODE_ORIGIN origin) {
    IDDCX_MONITOR_MODE result = {};
    result.Size = sizeof(result);
    result.Origin = origin;
    FillSignalInfo(result.MonitorVideoSignalInfo, mode, true);
    return result;
}

void FileCleanup(WDFOBJECT file) {
    auto* context = WdfObjectGet_FileContext(file);
    if (context->adapter != nullptr) {
        context->adapter->RemoveOwnedBy(static_cast<WDFFILEOBJECT>(file));
    }
}

// Attaches a context to the caller's file object whose cleanup (handle closed, process gone) removes the monitors
// added through it.
NTSTATUS TrackOwner(WDFFILEOBJECT file, Adapter* adapter) {
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, FileContext);
    attributes.EvtCleanupCallback = FileCleanup;
    FileContext* context = nullptr;
    const NTSTATUS status = WdfObjectAllocateContext(file, &attributes, reinterpret_cast<PVOID*>(&context));
    if (status == STATUS_OBJECT_NAME_EXISTS) {
        return STATUS_SUCCESS;
    }
    if (NT_SUCCESS(status)) {
        context->adapter = adapter;
    } else {
        Log("WdfObjectAllocateContext on file object failed: 0x%08lx", status);
    }
    return status;
}

template <typename T>
NTSTATUS InputBuffer(WDFREQUEST request, T* value) {
    void* buffer = nullptr;
    const NTSTATUS status = WdfRequestRetrieveInputBuffer(request, sizeof(T), &buffer, nullptr);
    if (NT_SUCCESS(status)) {
        memcpy(value, buffer, sizeof(T));  // METHOD_BUFFERED: input and output share one buffer
    }
    return status;
}

}  // namespace

IDDCX_TARGET_MODE lumen_vdd::TargetMode(const LUMEN_VDD_MODE& mode) {
    IDDCX_TARGET_MODE result = {};
    result.Size = sizeof(result);
    FillSignalInfo(result.TargetVideoSignalInfo.targetVideoSignalInfo, mode, false);
    return result;
}

EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY EvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT EvtDeviceD0Exit;
EVT_IDD_CX_DEVICE_IO_CONTROL EvtDeviceIoControl;
EVT_IDD_CX_ADAPTER_INIT_FINISHED EvtAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES EvtAdapterCommitModes;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION EvtParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES EvtMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES EvtMonitorQueryTargetModes;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN EvtMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN EvtMonitorUnassignSwapChain;

extern "C" BOOL WINAPI DllMain(_In_ HINSTANCE, _In_ UINT, _In_opt_ LPVOID) {
    return TRUE;
}

extern "C" DRIVER_INITIALIZE DriverEntry;

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver_object, PUNICODE_STRING registry_path) {
    Log("driver %u.%u.%u, contract %u.%u", LUMEN_VDD_VERSION_MAJOR, LUMEN_VDD_VERSION_MINOR,
        LUMEN_VDD_VERSION_PATCH, LUMEN_VDD_CONTRACT_MAJOR, LUMEN_VDD_CONTRACT_MINOR);
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
    return WdfDriverCreate(driver_object, registry_path, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

_Use_decl_annotations_
NTSTATUS EvtDeviceAdd(WDFDRIVER, PWDFDEVICE_INIT init) {
    WDF_PNPPOWER_EVENT_CALLBACKS pnp_power;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp_power);
    pnp_power.EvtDeviceD0Entry = EvtDeviceD0Entry;
    pnp_power.EvtDeviceD0Exit = EvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &pnp_power);

    IDD_CX_CLIENT_CONFIG config;
    IDD_CX_CLIENT_CONFIG_INIT(&config);
    config.EvtIddCxDeviceIoControl = EvtDeviceIoControl;
    config.EvtIddCxAdapterInitFinished = EvtAdapterInitFinished;
    config.EvtIddCxAdapterCommitModes = EvtAdapterCommitModes;
    config.EvtIddCxParseMonitorDescription = EvtParseMonitorDescription;
    config.EvtIddCxMonitorGetDefaultDescriptionModes = EvtMonitorGetDefaultModes;
    config.EvtIddCxMonitorQueryTargetModes = EvtMonitorQueryTargetModes;
    config.EvtIddCxMonitorAssignSwapChain = EvtMonitorAssignSwapChain;
    config.EvtIddCxMonitorUnassignSwapChain = EvtMonitorUnassignSwapChain;
    NTSTATUS status = IddCxDeviceInitConfig(init, &config);
    if (!NT_SUCCESS(status)) {
        Log("IddCxDeviceInitConfig failed: 0x%08lx", status);
        return status;
    }

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DeviceContext);
    attributes.EvtCleanupCallback = [](WDFOBJECT object) {
        auto* context = WdfObjectGet_DeviceContext(object);
        if (g_adapter == context->adapter) {
            g_adapter = nullptr;
        }
        delete context->adapter;
        context->adapter = nullptr;
    };
    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        Log("WdfDeviceCreate failed: 0x%08lx", status);
        return status;
    }
    auto* context = WdfObjectGet_DeviceContext(device);
    context->adapter = new Adapter(device);
    g_adapter = context->adapter;

    static const GUID kInterfaceGuid = LUMEN_VDD_INTERFACE_GUID_INIT;
    status = WdfDeviceCreateDeviceInterface(device, &kInterfaceGuid, nullptr);
    if (!NT_SUCCESS(status)) {
        Log("WdfDeviceCreateDeviceInterface failed: 0x%08lx", status);
        return status;
    }
    status = IddCxDeviceInitialize(device);
    if (!NT_SUCCESS(status)) {
        Log("IddCxDeviceInitialize failed: 0x%08lx", status);
    }
    return status;
}

_Use_decl_annotations_
NTSTATUS EvtDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE) {
    WdfObjectGet_DeviceContext(device)->adapter->Start();
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS EvtDeviceD0Exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE) {
    WdfObjectGet_DeviceContext(device)->adapter->Stop();
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID EvtDeviceIoControl(WDFDEVICE device, WDFREQUEST request, size_t, size_t, ULONG code) {
    Adapter* adapter = WdfObjectGet_DeviceContext(device)->adapter;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    size_t written = 0;

    switch (code) {
    case IOCTL_LUMEN_VDD_GET_INFO: {
        LUMEN_VDD_INFO* info = nullptr;
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(*info), reinterpret_cast<PVOID*>(&info), nullptr);
        if (NT_SUCCESS(status)) {
            status = adapter->GetInfo(info);
            written = sizeof(*info);
        }
        break;
    }
    case IOCTL_LUMEN_VDD_ADD_MONITOR: {
        LUMEN_VDD_ADD_MONITOR in;
        LUMEN_VDD_MONITOR_ADDED* out = nullptr;
        status = InputBuffer(request, &in);
        if (NT_SUCCESS(status)) {
            status = WdfRequestRetrieveOutputBuffer(request, sizeof(*out), reinterpret_cast<PVOID*>(&out), nullptr);
        }
        WDFFILEOBJECT file = WdfRequestGetFileObject(request);
        if (NT_SUCCESS(status) && file == nullptr) {
            Log("request has no file object, cannot track the monitor owner");
            status = STATUS_NOT_SUPPORTED;
        }
        if (NT_SUCCESS(status)) {
            status = TrackOwner(file, adapter);
        }
        if (NT_SUCCESS(status)) {
            LUMEN_VDD_MONITOR_ADDED added = {};
            status = adapter->AddMonitor(file, in, &added);
            *out = added;
            written = sizeof(added);
        }
        break;
    }
    case IOCTL_LUMEN_VDD_REMOVE_MONITOR: {
        LUMEN_VDD_REMOVE_MONITOR in;
        status = InputBuffer(request, &in);
        if (NT_SUCCESS(status)) {
            status = adapter->RemoveMonitor(WdfRequestGetFileObject(request), in.id);
        }
        break;
    }
    case IOCTL_LUMEN_VDD_SET_RENDER_ADAPTER: {
        LUMEN_VDD_SET_RENDER_ADAPTER in;
        status = InputBuffer(request, &in);
        if (NT_SUCCESS(status)) {
            LUID luid;
            luid.LowPart = in.luid_low;
            luid.HighPart = in.luid_high;
            status = adapter->SetRenderAdapter(luid);
        }
        break;
    }
    case IOCTL_LUMEN_VDD_SET_MODES: {
        LUMEN_VDD_SET_MODES in;
        LUMEN_VDD_MONITOR* out = nullptr;
        status = InputBuffer(request, &in);
        if (NT_SUCCESS(status)) {
            status = WdfRequestRetrieveOutputBuffer(request, sizeof(*out), reinterpret_cast<PVOID*>(&out), nullptr);
        }
        if (NT_SUCCESS(status)) {
            LUMEN_VDD_MONITOR monitor = {};
            status = adapter->SetModes(WdfRequestGetFileObject(request), in, &monitor);
            *out = monitor;
            written = sizeof(monitor);
        }
        break;
    }
    case IOCTL_LUMEN_VDD_LIST_MONITORS: {
        LUMEN_VDD_MONITOR* out = nullptr;
        size_t size = 0;
        status = WdfRequestRetrieveOutputBuffer(request, kMaxMonitors * sizeof(*out), reinterpret_cast<PVOID*>(&out),
                                                &size);
        if (NT_SUCCESS(status)) {
            written = adapter->ListMonitors(WdfRequestGetFileObject(request), out, size / sizeof(*out)) *
                      sizeof(*out);
        }
        break;
    }
    default:
        break;
    }
    WdfRequestCompleteWithInformation(request, status, NT_SUCCESS(status) ? written : 0);
}

_Use_decl_annotations_
NTSTATUS EvtAdapterInitFinished(IDDCX_ADAPTER, const IDARG_IN_ADAPTER_INIT_FINISHED* in) {
    // g_adapter rather than the adapter object's context: this may run before IddCxAdapterInitAsync returns and the
    // context is filled.
    if (g_adapter != nullptr) {
        g_adapter->InitFinished(in->AdapterInitStatus);
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS EvtAdapterCommitModes(IDDCX_ADAPTER, const IDARG_IN_COMMITMODES*) {
    return STATUS_SUCCESS;  // nothing to reconfigure: there is no hardware behind the monitors
}

_Use_decl_annotations_
NTSTATUS EvtParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* in,
                                    IDARG_OUT_PARSEMONITORDESCRIPTION* out) {
    if (g_adapter == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    const auto modes = g_adapter->ModesForEdid(static_cast<const uint8_t*>(in->MonitorDescription.pData),
                                               in->MonitorDescription.DataSize);
    if (modes.empty()) {
        return STATUS_INVALID_PARAMETER;
    }
    out->MonitorModeBufferOutputCount = static_cast<UINT>(modes.size());
    if (in->MonitorModeBufferInputCount == 0) {
        return STATUS_SUCCESS;  // count query
    }
    if (in->MonitorModeBufferInputCount < modes.size()) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (size_t i = 0; i < modes.size(); i++) {
        in->pMonitorModes[i] = MonitorMode(modes[i], IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR);
    }
    out->PreferredMonitorModeIdx = 0;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS EvtMonitorGetDefaultModes(IDDCX_MONITOR monitor, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* in,
                                   IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* out) {
    // Only called for monitors without a description; ours always have an EDID, but answer with the same modes.
    const auto modes = g_adapter != nullptr ? g_adapter->Modes(WdfObjectGet_MonitorContext(monitor)->id)
                                            : std::vector<LUMEN_VDD_MODE>{};
    out->DefaultMonitorModeBufferOutputCount = static_cast<UINT>(modes.size());
    if (in->DefaultMonitorModeBufferInputCount == 0) {
        return STATUS_SUCCESS;
    }
    if (in->DefaultMonitorModeBufferInputCount < modes.size()) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (size_t i = 0; i < modes.size(); i++) {
        in->pDefaultMonitorModes[i] = MonitorMode(modes[i], IDDCX_MONITOR_MODE_ORIGIN_DRIVER);
    }
    out->PreferredMonitorModeIdx = 0;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS EvtMonitorQueryTargetModes(IDDCX_MONITOR monitor, const IDARG_IN_QUERYTARGETMODES* in,
                                    IDARG_OUT_QUERYTARGETMODES* out) {
    // Windows offers the intersection of monitor modes and target modes; both are the requested list.
    const auto modes = g_adapter != nullptr ? g_adapter->Modes(WdfObjectGet_MonitorContext(monitor)->id)
                                            : std::vector<LUMEN_VDD_MODE>{};
    out->TargetModeBufferOutputCount = static_cast<UINT>(modes.size());
    if (in->TargetModeBufferInputCount >= modes.size()) {
        for (size_t i = 0; i < modes.size(); i++) {
            in->pTargetModes[i] = TargetMode(modes[i]);
        }
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS EvtMonitorAssignSwapChain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN* in) {
    auto* context = WdfObjectGet_MonitorContext(monitor);
    delete context->cursor;
    context->cursor = nullptr;
    delete context->drain;
    context->drain = new FrameDrain(in->hSwapChain, in->RenderAdapterLuid, in->hNextSurfaceAvailable);
    // Set up again with every swap chain (IddCx forgets it with the old one).
    context->cursor = new CursorWatch(monitor, context->id);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS EvtMonitorUnassignSwapChain(IDDCX_MONITOR monitor) {
    auto* context = WdfObjectGet_MonitorContext(monitor);
    delete context->cursor;
    context->cursor = nullptr;
    delete context->drain;
    context->drain = nullptr;
    return STATUS_SUCCESS;
}
