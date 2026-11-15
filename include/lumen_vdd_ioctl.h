// lumen_vdd_ioctl.h - contract between Lumen Host and the Lumen VDD driver.
//
// This file is the source of truth. Lumen mirrors it in crates/vdd and has a test that IOCTL codes and struct sizes
// match the values stated here. All structs are little-endian with natural alignment (no packing pragmas); sizes are
// pinned by static_assert at the bottom.
//
// Compatibility: a client requires the same contract major version and checks optional features with caps bits.
// Minor versions only add caps bits, IOCTLs and trailing fields guarded by caps.
//
// Usage:
//   1. Find the device interface LUMEN_VDD_INTERFACE_GUID (CM_Get_Device_Interface_List), open it with
//      CreateFile(GENERIC_READ | GENERIC_WRITE). Only SYSTEM and Administrators may open the device.
//   2. IOCTL_LUMEN_VDD_GET_INFO, check contract_major.
//   3. IOCTL_LUMEN_VDD_ADD_MONITOR / REMOVE_MONITOR / SET_MODES. Monitors belong to the handle that added them: when
//      that handle is closed (or the process dies) the driver removes them. Only the owner may remove a monitor or
//      change its modes.
//   4. IOCTL_LUMEN_VDD_LIST_MONITORS: all monitors of the driver, whoever owns them.
//
// Errors are returned as Win32 errors from DeviceIoControl (GetLastError):
//   ERROR_INVALID_PARAMETER  bad id, nonzero reserved field, mode count, mode values, EDID (or the same EDID as another monitor)
//                            or buffer size
//   ERROR_INSUFFICIENT_BUFFER  LIST_MONITORS output buffer smaller than max_monitors entries
//   ERROR_ALREADY_EXISTS     a monitor with this id exists (or is still being removed)
//   ERROR_NOT_FOUND          no monitor with this id
//   ERROR_ACCESS_DENIED      the monitor belongs to another handle
//   ERROR_NOT_READY          the display adapter is not initialized yet (retry shortly)
//   ERROR_BUSY               the monitor is being added, removed or reconnected right now (retry shortly)
//   ERROR_NOT_SUPPORTED      feature not available in this driver (see caps)

#pragma once

#include <stdint.h>

#define LUMEN_VDD_CONTRACT_MAJOR 1
#define LUMEN_VDD_CONTRACT_MINOR 1

// Device interface class: {0F76755D-FA51-4FB9-A19D-39A14E1AEA68}
#define LUMEN_VDD_INTERFACE_GUID_INIT \
    { 0x0f76755d, 0xfa51, 0x4fb9, { 0xa1, 0x9d, 0x39, 0xa1, 0x4e, 0x1a, 0xea, 0x68 } }

// Capability bits (LUMEN_VDD_INFO.caps)
#define LUMEN_VDD_CAP_ADD_REMOVE      0x00000001u  // ADD_MONITOR, REMOVE_MONITOR
#define LUMEN_VDD_CAP_RENDER_ADAPTER  0x00000002u  // SET_RENDER_ADAPTER
#define LUMEN_VDD_CAP_SET_MODES       0x00000004u  // SET_MODES (1.1)
#define LUMEN_VDD_CAP_LIST_MONITORS   0x00000008u  // LIST_MONITORS (1.1)
#define LUMEN_VDD_CAP_EDID            0x00000010u  // caller-supplied EDID in ADD_MONITOR (1.1)
#define LUMEN_VDD_CAP_DIRECT_FRAMES   0x00000020u  // reserved: frames straight from the driver swap chain

#define LUMEN_VDD_MAX_MODES 32     // per monitor
#define LUMEN_VDD_MAX_EDID  1024   // bytes

// IOCTL codes: CTL_CODE(FILE_DEVICE_UNKNOWN, function, METHOD_BUFFERED, access).
// Spelled out as numbers so other languages can copy them; the driver checks them against CTL_CODE.
#define IOCTL_LUMEN_VDD_GET_INFO            0x00222000u  // fn 0x800, FILE_ANY_ACCESS;  out LUMEN_VDD_INFO
#define IOCTL_LUMEN_VDD_ADD_MONITOR         0x0022E004u  // fn 0x801, read|write; in LUMEN_VDD_ADD_MONITOR,
                                                         //   out LUMEN_VDD_MONITOR_ADDED
#define IOCTL_LUMEN_VDD_REMOVE_MONITOR      0x0022E008u  // fn 0x802, read|write; in LUMEN_VDD_REMOVE_MONITOR
#define IOCTL_LUMEN_VDD_SET_RENDER_ADAPTER  0x0022E00Cu  // fn 0x803, read|write; in LUMEN_VDD_SET_RENDER_ADAPTER
#define IOCTL_LUMEN_VDD_SET_MODES           0x0022E010u  // fn 0x804, read|write; in LUMEN_VDD_SET_MODES,
                                                         //   out LUMEN_VDD_MONITOR
#define IOCTL_LUMEN_VDD_LIST_MONITORS       0x00222014u  // fn 0x805, FILE_ANY_ACCESS; out LUMEN_VDD_MONITOR[]

typedef struct LUMEN_VDD_INFO {
    uint16_t contract_major;
    uint16_t contract_minor;
    uint32_t caps;            // LUMEN_VDD_CAP_*
    uint32_t max_monitors;    // valid ids are 0 .. max_monitors-1
    uint32_t max_modes;       // per monitor
    uint32_t driver_version;  // (major << 16) | (minor << 8) | patch
    uint32_t iddcx_version;   // IddCxGetVersion() of this Windows, informational only (0x10C00 on 10.0.26200)
} LUMEN_VDD_INFO;

typedef struct LUMEN_VDD_MODE {
    uint32_t width;
    uint32_t height;
    uint32_t refresh_hz;
} LUMEN_VDD_MODE;

typedef struct LUMEN_VDD_ADD_MONITOR {
    // Stable monitor id, 0 .. max_monitors-1. The same id always gives Windows the same monitor (connector and EDID
    // identity), so Windows keeps its scale and position across re-adds, service restarts and reboots.
    uint32_t id;
    uint32_t mode_count;                         // 1 .. LUMEN_VDD_MAX_MODES; modes[0] is the preferred mode
    LUMEN_VDD_MODE modes[LUMEN_VDD_MAX_MODES];
    // 0 = driver-generated EDID ("LumenVDA" since 0.2.3, the same bytes for the same id whatever the modes).
    // Otherwise (LUMEN_VDD_CAP_EDID) the caller's EDID, used as given: 128 .. LUMEN_VDD_MAX_EDID bytes, a multiple
    // of 128, valid header and base block checksum. Windows then sees the caller's monitor (name, color data), and
    // keeps scale and position for that EDID on this id. Two monitors cannot have the same EDID. The modes still come
    // from `modes`, not from the EDID.
    uint32_t edid_size;
    uint8_t edid[LUMEN_VDD_MAX_EDID];
} LUMEN_VDD_ADD_MONITOR;

typedef struct LUMEN_VDD_MONITOR_ADDED {
    // Where Windows exposed the monitor: the adapter LUID and target id of its DISPLAYCONFIG path
    // (QueryDisplayConfig / SetDisplayConfig).
    uint32_t adapter_luid_low;
    int32_t adapter_luid_high;
    uint32_t target_id;
} LUMEN_VDD_MONITOR_ADDED;

typedef struct LUMEN_VDD_REMOVE_MONITOR {
    uint32_t id;
} LUMEN_VDD_REMOVE_MONITOR;

typedef struct LUMEN_VDD_SET_RENDER_ADAPTER {
    // GPU that should render the desktop for all Lumen VDD monitors; {0, 0} = no preference. A preference only:
    // Windows may pick another GPU (for example if this one is stopped).
    uint32_t luid_low;
    int32_t luid_high;
} LUMEN_VDD_SET_RENDER_ADAPTER;

// SET_MODES replaces the modes of a monitor by unplugging it and plugging it back in (same id, EDID and owner, so
// Windows keeps its scale and position; the screen blinks as on any monitor change). Changing the modes in place
// (IddCxMonitorUpdateModes) does not work: Windows keeps offering the modes the monitor arrived with.
typedef struct LUMEN_VDD_SET_MODES {
    uint32_t id;
    uint32_t flags;                              // reserved, 0
    uint32_t mode_count;                         // 1 .. LUMEN_VDD_MAX_MODES; modes[0] is the preferred mode
    LUMEN_VDD_MODE modes[LUMEN_VDD_MAX_MODES];
} LUMEN_VDD_SET_MODES;

// LUMEN_VDD_MONITOR.state
#define LUMEN_VDD_MONITOR_ACTIVE     1  // connected to Windows
#define LUMEN_VDD_MONITOR_SUSPENDED  2  // the display adapter is down (sleep); comes back when it is up again
#define LUMEN_VDD_MONITOR_CHANGING   3  // being added, removed or reconnected right now

// LUMEN_VDD_MONITOR.flags
#define LUMEN_VDD_MONITOR_OWNED      0x00000001u  // added through the handle that asks
#define LUMEN_VDD_MONITOR_OWN_EDID   0x00000002u  // EDID supplied by the caller of ADD_MONITOR

// One monitor, as returned by SET_MODES and LIST_MONITORS. LIST_MONITORS fills an array of these: pass a buffer of
// max_monitors entries; the number of bytes returned / sizeof(LUMEN_VDD_MONITOR) is the number of monitors.
typedef struct LUMEN_VDD_MONITOR {
    uint32_t id;
    uint32_t state;                              // LUMEN_VDD_MONITOR_*
    uint32_t flags;                              // LUMEN_VDD_MONITOR_*
    uint32_t adapter_luid_low;                   // where Windows exposed the monitor (as in LUMEN_VDD_MONITOR_ADDED);
    int32_t adapter_luid_high;                   //   zero while not active
    uint32_t target_id;
    uint32_t mode_count;
    LUMEN_VDD_MODE modes[LUMEN_VDD_MAX_MODES];
} LUMEN_VDD_MONITOR;

#ifdef __cplusplus
static_assert(sizeof(LUMEN_VDD_INFO) == 24, "contract");
static_assert(sizeof(LUMEN_VDD_MODE) == 12, "contract");
static_assert(sizeof(LUMEN_VDD_ADD_MONITOR) == 1420, "contract");
static_assert(sizeof(LUMEN_VDD_MONITOR_ADDED) == 12, "contract");
static_assert(sizeof(LUMEN_VDD_REMOVE_MONITOR) == 4, "contract");
static_assert(sizeof(LUMEN_VDD_SET_RENDER_ADAPTER) == 8, "contract");
static_assert(sizeof(LUMEN_VDD_SET_MODES) == 396, "contract");
static_assert(sizeof(LUMEN_VDD_MONITOR) == 412, "contract");
#endif
