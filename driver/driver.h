// Lumen VDD: IddCx virtual display driver.
//
// The display adapter lives as long as the device; monitors are added and removed on request through IOCTLs
// (include/lumen_vdd_ioctl.h) and belong to the file handle that added them.

#pragma once

#define NOMINMAX
#include <windows.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <iddcx.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

#include "lumen_vdd_ioctl.h"

// LUMEN_VDD_VERSION_MAJOR/MINOR/PATCH come from lumen_vdd.vcxproj (LumenVddVersion), which also stamps the INF
// DriverVer.
#if !defined(LUMEN_VDD_VERSION_MAJOR) || !defined(LUMEN_VDD_VERSION_MINOR) || !defined(LUMEN_VDD_VERSION_PATCH)
#error "driver version is set by the project"
#endif

namespace lumen_vdd {

constexpr uint32_t kMaxMonitors = 4;
constexpr size_t kEdidBlock = 128;

// Debug output (OutputDebugString, prefixed "LumenVdd: "); view with DebugView, "Capture Global Win32".
void Log(_Printf_format_string_ const char* format, ...);

bool IsValidMode(const LUMEN_VDD_MODE& mode);
bool AreValidModes(const LUMEN_VDD_MODE* modes, uint32_t count);  // 1 .. LUMEN_VDD_MAX_MODES valid modes

IDDCX_TARGET_MODE TargetMode(const LUMEN_VDD_MODE& mode);

// EDID of Lumen VDD monitor `id`: manufacturer "LMN", product code and serial number derived from `id`. It depends on
// `id` only (not on the modes), so the same id is always the same monitor for Windows, which keys the saved scale and
// position on the EDID. The mode list comes from the driver, not from the EDID.
std::vector<uint8_t> MakeEdid(uint32_t id);

// Caller-supplied EDID check: size, header and base block checksum.
bool IsValidEdid(const uint8_t* edid, size_t size);

// Keeps acquiring and releasing the frames of one swap chain on its own thread. Windows requires the driver to
// consume frames; Lumen captures the monitor with Desktop Duplication, so the frames themselves are dropped.
class FrameDrain {
public:
    FrameDrain(IDDCX_SWAPCHAIN swapchain, LUID render_adapter, HANDLE frame_event);
    ~FrameDrain();
    FrameDrain(const FrameDrain&) = delete;
    FrameDrain& operator=(const FrameDrain&) = delete;

private:
    static DWORD CALLBACK ThreadProc(void* self);
    void Drain();

    IDDCX_SWAPCHAIN swapchain_;
    LUID render_adapter_;
    HANDLE frame_event_;
    HANDLE stop_event_ = nullptr;
    HANDLE thread_ = nullptr;
};

// The monitor's hardware cursor (set up with each swap chain): Windows no longer draws the pointer into the image
// and signals the driver instead; a thread of its own takes each update (Lumen reads the pointer through Desktop
// Duplication, which reports it for this monitor only with a hardware cursor). The latest state is kept for logging.
class CursorWatch {
public:
    struct State {
        bool visible = false;
        int32_t x = 0;  // top-left of the shape, monitor coordinates
        int32_t y = 0;
        uint32_t shape_id = 0;  // 0: none yet
        bool alpha = false;     // else masked color
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t pitch = 0;
        uint32_t hot_x = 0;
        uint32_t hot_y = 0;
        uint64_t updates = 0;
    };

    CursorWatch(IDDCX_MONITOR monitor, uint32_t id);
    ~CursorWatch();
    CursorWatch(const CursorWatch&) = delete;
    CursorWatch& operator=(const CursorWatch&) = delete;

private:
    static DWORD CALLBACK ThreadProc(void* self);
    void Watch();

    IDDCX_MONITOR monitor_;
    uint32_t id_;
    HANDLE data_event_ = nullptr;
    HANDLE stop_event_ = nullptr;
    HANDLE thread_ = nullptr;
    std::mutex lock_;
    State state_;
    std::vector<uint8_t> shape_;  // the latest shape's pixels (pitch × height bytes)
};

enum class SlotState {
    Free,
    Arriving,   // being reported to Windows
    Active,
    Departing,  // being removed from Windows
    Suspended,  // the adapter went away (D0 exit); the monitor comes back when the adapter is ready again
};

struct MonitorSlot {
    SlotState state = SlotState::Free;
    WDFFILEOBJECT owner = nullptr;
    bool owner_gone = false;  // the owner's handle closed while the slot was Arriving: remove once it has arrived
    IDDCX_MONITOR monitor = nullptr;
    std::vector<LUMEN_VDD_MODE> modes;
    std::vector<uint8_t> edid;  // IddCx may keep pointing at it: not changed while Arriving or Active
    bool own_edid = false;
    LUID os_adapter = {};       // where Windows exposed the monitor (valid while Active)
    uint32_t os_target = 0;
};

// One per device: the IddCx adapter and its monitor slots (slot index = monitor id = IddCx connector index).
// IddCx calls are never made under lock_: they may call back into the driver on other threads.
class Adapter {
public:
    explicit Adapter(WDFDEVICE device);

    void Start();  // D0 entry
    void Stop();   // D0 exit
    void InitFinished(NTSTATUS status);

    NTSTATUS GetInfo(LUMEN_VDD_INFO* info);
    NTSTATUS AddMonitor(WDFFILEOBJECT owner, const LUMEN_VDD_ADD_MONITOR& request, LUMEN_VDD_MONITOR_ADDED* added);
    NTSTATUS RemoveMonitor(WDFFILEOBJECT owner, uint32_t id);
    void RemoveOwnedBy(WDFFILEOBJECT owner);
    NTSTATUS SetModes(WDFFILEOBJECT owner, const LUMEN_VDD_SET_MODES& request, LUMEN_VDD_MONITOR* monitor);
    // Fills up to `capacity` entries, returns the number of monitors.
    size_t ListMonitors(WDFFILEOBJECT caller, LUMEN_VDD_MONITOR* monitors, size_t capacity);
    NTSTATUS SetRenderAdapter(LUID luid);

    std::vector<LUMEN_VDD_MODE> Modes(uint32_t id);
    // Modes of the monitor whose EDID is `edid` (EDID parsing callback); empty if there is none.
    std::vector<LUMEN_VDD_MODE> ModesForEdid(const uint8_t* edid, size_t size);

private:
    // Reports slot `id` (Arriving) to Windows and records where it appeared.
    NTSTATUS Arrive(uint32_t id);
    // Ends an Arriving slot: Active on success (or departs right away if the owner went meanwhile), Free on failure.
    NTSTATUS FinishArrival(uint32_t id, NTSTATUS status);
    void Depart(uint32_t id);
    void Describe(uint32_t id, WDFFILEOBJECT caller, LUMEN_VDD_MONITOR* monitor);  // under lock_

    WDFDEVICE device_;
    std::mutex lock_;
    IDDCX_ADAPTER adapter_ = nullptr;
    bool ready_ = false;
    LUID render_adapter_ = {};
    std::array<MonitorSlot, kMaxMonitors> slots_;
};

}  // namespace lumen_vdd

// WDF object contexts
struct DeviceContext {
    lumen_vdd::Adapter* adapter;  // owned; on the WDFDEVICE and the IDDCX_ADAPTER
};
struct MonitorContext {
    uint32_t id;
    lumen_vdd::FrameDrain* drain;    // owned
    lumen_vdd::CursorWatch* cursor;  // owned
};
struct FileContext {
    lumen_vdd::Adapter* adapter;
};
WDF_DECLARE_CONTEXT_TYPE(DeviceContext);
WDF_DECLARE_CONTEXT_TYPE(MonitorContext);
WDF_DECLARE_CONTEXT_TYPE(FileContext);
