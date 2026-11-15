// Adapter: IddCx adapter lifetime and monitor slots.

#include "driver.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace lumen_vdd {

void Log(const char* format, ...) {
    char text[512] = "LumenVdd: ";
    const size_t prefix = strlen(text);
    va_list args;
    va_start(args, format);
    _vsnprintf_s(text + prefix, sizeof(text) - prefix - 1, _TRUNCATE, format, args);
    va_end(args);
    strcat_s(text, "\n");
    OutputDebugStringA(text);
}

bool IsValidMode(const LUMEN_VDD_MODE& mode) {
    return mode.width >= 640 && mode.width <= 8192 && mode.height >= 480 && mode.height <= 8192 &&
           mode.refresh_hz >= 24 && mode.refresh_hz <= 240;
}

bool AreValidModes(const LUMEN_VDD_MODE* modes, uint32_t count) {
    if (count == 0 || count > LUMEN_VDD_MAX_MODES) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (!IsValidMode(modes[i])) {
            return false;
        }
    }
    return true;
}

namespace {

// Stable container id per monitor id (a Lumen VDD monitor is not part of the adapter device).
GUID ContainerId(uint32_t id) {
    GUID guid = {0x2728276e, 0xbc08, 0x4072, {0x98, 0x8a, 0xc6, 0x0e, 0x33, 0x8b, 0x4b, 0x00}};
    guid.Data4[7] = static_cast<uint8_t>(id);
    return guid;
}

void DestroyMonitorContext(WDFOBJECT object) {
    auto* context = WdfObjectGet_MonitorContext(object);
    delete context->cursor;
    context->cursor = nullptr;
    delete context->drain;
    context->drain = nullptr;
}

double MillisecondsSince(const LARGE_INTEGER& start) {
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return (now.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart;
}

}  // namespace

Adapter::Adapter(WDFDEVICE device) : device_(device) {}

void Adapter::Start() {
    IDDCX_ENDPOINT_VERSION version = {};
    version.Size = sizeof(version);
    version.MajorVer = LUMEN_VDD_VERSION_MAJOR;
    version.MinorVer = LUMEN_VDD_VERSION_MINOR;
    version.Build = LUMEN_VDD_VERSION_PATCH;

    IDDCX_ADAPTER_CAPS caps = {};
    caps.Size = sizeof(caps);
    caps.MaxMonitorsSupported = kMaxMonitors;
    caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
    caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;
    caps.EndPointDiagnostics.pEndPointFriendlyName = L"LumenVDA";
    caps.EndPointDiagnostics.pEndPointManufacturerName = L"Lumen";
    caps.EndPointDiagnostics.pEndPointModelName = L"LumenVDA";
    caps.EndPointDiagnostics.pFirmwareVersion = &version;
    caps.EndPointDiagnostics.pHardwareVersion = &version;

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DeviceContext);
    IDARG_IN_ADAPTER_INIT in = {};
    in.WdfDevice = device_;
    in.pCaps = &caps;
    in.ObjectAttributes = &attributes;
    IDARG_OUT_ADAPTER_INIT out = {};
    const NTSTATUS status = IddCxAdapterInitAsync(&in, &out);
    if (!NT_SUCCESS(status)) {
        Log("IddCxAdapterInitAsync failed: 0x%08lx", status);
        return;
    }
    WdfObjectGet_DeviceContext(out.AdapterObject)->adapter = this;
    std::lock_guard<std::mutex> guard(lock_);
    adapter_ = out.AdapterObject;
}

void Adapter::Stop() {
    // IddCx destroys the adapter and its monitors on D0 exit (sleep, device disable). Monitors that still have an
    // owner are recreated once the adapter is initialized again.
    std::lock_guard<std::mutex> guard(lock_);
    ready_ = false;
    adapter_ = nullptr;
    for (auto& slot : slots_) {
        if (slot.state == SlotState::Active) {
            slot.state = SlotState::Suspended;
            slot.monitor = nullptr;
        }
    }
    Log("adapter stopped");
}

void Adapter::InitFinished(NTSTATUS status) {
    if (!NT_SUCCESS(status)) {
        Log("adapter init failed: 0x%08lx", status);
        return;
    }
    std::vector<uint32_t> resume;
    LUID render = {};
    {
        std::lock_guard<std::mutex> guard(lock_);
        ready_ = true;
        render = render_adapter_;
        for (uint32_t id = 0; id < kMaxMonitors; id++) {
            if (slots_[id].state == SlotState::Suspended) {
                slots_[id].state = SlotState::Arriving;
                resume.push_back(id);
            }
        }
    }
    Log("adapter ready, resuming %zu monitors", resume.size());
    if (render.LowPart != 0 || render.HighPart != 0) {
        SetRenderAdapter(render);
    }
    for (const uint32_t id : resume) {
        const NTSTATUS arrive_status = FinishArrival(id, Arrive(id));
        if (!NT_SUCCESS(arrive_status)) {
            Log("monitor %u lost on resume: 0x%08lx", id, arrive_status);
        }
    }
}

NTSTATUS Adapter::GetInfo(LUMEN_VDD_INFO* info) {
    *info = {};
    info->contract_major = LUMEN_VDD_CONTRACT_MAJOR;
    info->contract_minor = LUMEN_VDD_CONTRACT_MINOR;
    info->caps = LUMEN_VDD_CAP_ADD_REMOVE | LUMEN_VDD_CAP_RENDER_ADAPTER | LUMEN_VDD_CAP_SET_MODES |
                 LUMEN_VDD_CAP_LIST_MONITORS | LUMEN_VDD_CAP_EDID;
    info->max_monitors = kMaxMonitors;
    info->max_modes = LUMEN_VDD_MAX_MODES;
    info->driver_version =
        (LUMEN_VDD_VERSION_MAJOR << 16) | (LUMEN_VDD_VERSION_MINOR << 8) | LUMEN_VDD_VERSION_PATCH;
    IDARG_OUT_GETVERSION version = {};
    if (NT_SUCCESS(IddCxGetVersion(&version))) {
        info->iddcx_version = version.IddCxVersion;
    }
    return STATUS_SUCCESS;
}

NTSTATUS Adapter::AddMonitor(WDFFILEOBJECT owner, const LUMEN_VDD_ADD_MONITOR& request,
                             LUMEN_VDD_MONITOR_ADDED* added) {
    if (request.id >= kMaxMonitors || !AreValidModes(request.modes, request.mode_count)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (request.edid_size != 0 && !IsValidEdid(request.edid, request.edid_size)) {
        return STATUS_INVALID_PARAMETER;
    }
    std::vector<uint8_t> edid = request.edid_size != 0
                                    ? std::vector<uint8_t>(request.edid, request.edid + request.edid_size)
                                    : MakeEdid(request.id);

    const uint32_t id = request.id;
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (!ready_) {
            return STATUS_DEVICE_NOT_READY;
        }
        if (slots_[id].state != SlotState::Free) {
            return STATUS_OBJECT_NAME_COLLISION;
        }
        // The EDID parsing callback finds the monitor by its EDID, so EDIDs must differ.
        for (const auto& other : slots_) {
            if (other.state != SlotState::Free && other.edid == edid) {
                return STATUS_INVALID_PARAMETER;
            }
        }
        MonitorSlot& slot = slots_[id];
        slot = MonitorSlot{};
        slot.state = SlotState::Arriving;
        slot.owner = owner;
        slot.modes.assign(request.modes, request.modes + request.mode_count);
        slot.edid = std::move(edid);
        slot.own_edid = request.edid_size != 0;
    }

    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    const NTSTATUS status = FinishArrival(id, Arrive(id));
    if (NT_SUCCESS(status)) {
        std::lock_guard<std::mutex> guard(lock_);
        added->adapter_luid_low = slots_[id].os_adapter.LowPart;
        added->adapter_luid_high = slots_[id].os_adapter.HighPart;
        added->target_id = slots_[id].os_target;
    }
    Log("add monitor %u %ux%u@%u (%u modes, %s EDID): 0x%08lx, %.1f ms", id, request.modes[0].width,
        request.modes[0].height, request.modes[0].refresh_hz, request.mode_count,
        request.edid_size != 0 ? "own" : "driver", status, MillisecondsSince(start));
    return status;
}

NTSTATUS Adapter::Arrive(uint32_t id) {
    IDDCX_ADAPTER adapter = nullptr;
    const uint8_t* edid = nullptr;
    UINT edid_size = 0;
    {
        std::lock_guard<std::mutex> guard(lock_);
        adapter = adapter_;
        edid = slots_[id].edid.data();
        edid_size = static_cast<UINT>(slots_[id].edid.size());
    }
    if (adapter == nullptr) {
        return STATUS_DEVICE_NOT_READY;
    }

    IDDCX_MONITOR_INFO info = {};
    info.Size = sizeof(info);
    info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    info.ConnectorIndex = id;
    info.MonitorDescription.Size = sizeof(info.MonitorDescription);
    info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    info.MonitorDescription.DataSize = edid_size;
    info.MonitorDescription.pData = const_cast<uint8_t*>(edid);
    info.MonitorContainerId = ContainerId(id);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, MonitorContext);
    attributes.EvtCleanupCallback = DestroyMonitorContext;
    IDARG_IN_MONITORCREATE create = {};
    create.ObjectAttributes = &attributes;
    create.pMonitorInfo = &info;
    IDARG_OUT_MONITORCREATE created = {};
    NTSTATUS status = IddCxMonitorCreate(adapter, &create, &created);
    if (!NT_SUCCESS(status)) {
        Log("IddCxMonitorCreate %u failed: 0x%08lx", id, status);
        return status;
    }
    auto* context = WdfObjectGet_MonitorContext(created.MonitorObject);
    context->id = id;
    context->drain = nullptr;
    context->cursor = nullptr;
    {
        std::lock_guard<std::mutex> guard(lock_);
        slots_[id].monitor = created.MonitorObject;
    }

    IDARG_OUT_MONITORARRIVAL arrival = {};
    status = IddCxMonitorArrival(created.MonitorObject, &arrival);
    if (!NT_SUCCESS(status)) {
        Log("IddCxMonitorArrival %u failed: 0x%08lx", id, status);
        WdfObjectDelete(created.MonitorObject);
        std::lock_guard<std::mutex> guard(lock_);
        slots_[id].monitor = nullptr;
        return status;
    }
    std::lock_guard<std::mutex> guard(lock_);
    slots_[id].os_adapter = arrival.OsAdapterLuid;
    slots_[id].os_target = arrival.OsTargetId;
    return STATUS_SUCCESS;
}

NTSTATUS Adapter::FinishArrival(uint32_t id, NTSTATUS status) {
    {
        std::lock_guard<std::mutex> guard(lock_);
        MonitorSlot& slot = slots_[id];
        if (!NT_SUCCESS(status)) {
            slot = MonitorSlot{};
            return status;
        }
        if (!slot.owner_gone) {
            slot.state = SlotState::Active;
            return status;
        }
        slot.state = SlotState::Departing;
    }
    Log("monitor %u: owner closed its handle while the monitor was arriving", id);
    Depart(id);
    return STATUS_CANCELLED;
}

NTSTATUS Adapter::RemoveMonitor(WDFFILEOBJECT owner, uint32_t id) {
    if (id >= kMaxMonitors) {
        return STATUS_INVALID_PARAMETER;
    }
    {
        std::lock_guard<std::mutex> guard(lock_);
        MonitorSlot& slot = slots_[id];
        if (slot.state != SlotState::Active && slot.state != SlotState::Suspended) {
            return slot.state == SlotState::Free ? STATUS_NOT_FOUND : STATUS_DEVICE_BUSY;
        }
        if (slot.owner != owner) {
            return STATUS_ACCESS_DENIED;
        }
        if (slot.state == SlotState::Suspended) {
            slot = MonitorSlot{};
            return STATUS_SUCCESS;
        }
        slot.state = SlotState::Departing;
    }
    Depart(id);
    return STATUS_SUCCESS;
}

void Adapter::RemoveOwnedBy(WDFFILEOBJECT owner) {
    std::vector<uint32_t> depart;
    {
        std::lock_guard<std::mutex> guard(lock_);
        for (uint32_t id = 0; id < kMaxMonitors; id++) {
            MonitorSlot& slot = slots_[id];
            if (slot.owner != owner) {
                continue;
            }
            if (slot.state == SlotState::Active) {
                slot.state = SlotState::Departing;
                depart.push_back(id);
            } else if (slot.state == SlotState::Suspended) {
                slot = MonitorSlot{};
            } else if (slot.state == SlotState::Arriving) {
                slot.owner_gone = true;  // FinishArrival removes it
            }
        }
    }
    for (const uint32_t id : depart) {
        Depart(id);
    }
    if (!depart.empty()) {
        Log("handle closed, removed %zu monitors", depart.size());
    }
}

void Adapter::Depart(uint32_t id) {
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    IDDCX_MONITOR monitor = nullptr;
    {
        std::lock_guard<std::mutex> guard(lock_);
        monitor = slots_[id].monitor;
    }
    NTSTATUS status = STATUS_SUCCESS;
    if (monitor != nullptr) {
        status = IddCxMonitorDeparture(monitor);
    }
    {
        std::lock_guard<std::mutex> guard(lock_);
        slots_[id] = MonitorSlot{};
    }
    Log("remove monitor %u: 0x%08lx, %.1f ms", id, status, MillisecondsSince(start));
}

NTSTATUS Adapter::SetModes(WDFFILEOBJECT owner, const LUMEN_VDD_SET_MODES& request, LUMEN_VDD_MONITOR* monitor) {
    if (request.id >= kMaxMonitors || !AreValidModes(request.modes, request.mode_count) || request.flags != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    const uint32_t id = request.id;
    IDDCX_MONITOR old_monitor = nullptr;
    {
        std::lock_guard<std::mutex> guard(lock_);
        MonitorSlot& slot = slots_[id];
        if (slot.state != SlotState::Active && slot.state != SlotState::Suspended) {
            return slot.state == SlotState::Free ? STATUS_NOT_FOUND : STATUS_DEVICE_BUSY;
        }
        if (slot.owner != owner) {
            return STATUS_ACCESS_DENIED;
        }
        slot.modes.assign(request.modes, request.modes + request.mode_count);
        if (slot.state == SlotState::Suspended) {
            Describe(id, owner, monitor);
            return STATUS_SUCCESS;  // the monitor comes back with the new modes
        }
        old_monitor = slot.monitor;
        slot.state = SlotState::Arriving;
        slot.monitor = nullptr;
    }

    // Reconnect: same slot, EDID and owner, so Windows sees the same monitor unplugged and plugged back in.
    // IddCxMonitorUpdateModes is not enough: checked on Windows 11 26200, it succeeds but Windows keeps offering the
    // modes the monitor arrived with.
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    const NTSTATUS departure = IddCxMonitorDeparture(old_monitor);
    if (!NT_SUCCESS(departure)) {
        Log("IddCxMonitorDeparture %u failed: 0x%08lx", id, departure);
    }
    const NTSTATUS status = FinishArrival(id, Arrive(id));
    Log("set modes %u %ux%u@%u (%u modes): 0x%08lx, %.1f ms", id, request.modes[0].width, request.modes[0].height,
        request.modes[0].refresh_hz, request.mode_count, status, MillisecondsSince(start));
    if (NT_SUCCESS(status)) {
        std::lock_guard<std::mutex> guard(lock_);
        Describe(id, owner, monitor);
    }
    return status;
}

size_t Adapter::ListMonitors(WDFFILEOBJECT caller, LUMEN_VDD_MONITOR* monitors, size_t capacity) {
    std::lock_guard<std::mutex> guard(lock_);
    size_t count = 0;
    for (uint32_t id = 0; id < kMaxMonitors && count < capacity; id++) {
        if (slots_[id].state != SlotState::Free) {
            Describe(id, caller, &monitors[count++]);
        }
    }
    return count;
}

void Adapter::Describe(uint32_t id, WDFFILEOBJECT caller, LUMEN_VDD_MONITOR* monitor) {
    const MonitorSlot& slot = slots_[id];
    *monitor = {};
    monitor->id = id;
    switch (slot.state) {
    case SlotState::Active:
        monitor->state = LUMEN_VDD_MONITOR_ACTIVE;
        monitor->adapter_luid_low = slot.os_adapter.LowPart;
        monitor->adapter_luid_high = slot.os_adapter.HighPart;
        monitor->target_id = slot.os_target;
        break;
    case SlotState::Suspended:
        monitor->state = LUMEN_VDD_MONITOR_SUSPENDED;
        break;
    default:
        monitor->state = LUMEN_VDD_MONITOR_CHANGING;
        break;
    }
    monitor->flags = (slot.owner == caller ? LUMEN_VDD_MONITOR_OWNED : 0u) |
                     (slot.own_edid ? LUMEN_VDD_MONITOR_OWN_EDID : 0u);
    monitor->mode_count = static_cast<uint32_t>(slot.modes.size());
    for (size_t i = 0; i < slot.modes.size(); i++) {
        monitor->modes[i] = slot.modes[i];
    }
}

NTSTATUS Adapter::SetRenderAdapter(LUID luid) {
    IDDCX_ADAPTER adapter = nullptr;
    {
        std::lock_guard<std::mutex> guard(lock_);
        render_adapter_ = luid;
        if (!ready_) {
            return STATUS_SUCCESS;  // applied in InitFinished
        }
        adapter = adapter_;
    }
    IDARG_IN_ADAPTERSETRENDERADAPTER in = {};
    in.PreferredRenderAdapter = luid;
    IddCxAdapterSetRenderAdapter(adapter, &in);
    Log("render adapter %08lx:%08lx", luid.HighPart, luid.LowPart);
    return STATUS_SUCCESS;
}

std::vector<LUMEN_VDD_MODE> Adapter::Modes(uint32_t id) {
    std::lock_guard<std::mutex> guard(lock_);
    if (id >= kMaxMonitors || slots_[id].state == SlotState::Free) {
        return {};
    }
    return slots_[id].modes;
}

std::vector<LUMEN_VDD_MODE> Adapter::ModesForEdid(const uint8_t* edid, size_t size) {
    std::lock_guard<std::mutex> guard(lock_);
    for (const auto& slot : slots_) {
        if (slot.state != SlotState::Free && slot.edid.size() == size && memcmp(slot.edid.data(), edid, size) == 0) {
            return slot.modes;
        }
    }
    return {};
}

}  // namespace lumen_vdd
