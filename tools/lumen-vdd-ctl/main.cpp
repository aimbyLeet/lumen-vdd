// lumen-vdd-ctl: manual control of the Lumen VDD driver, without Lumen. Run as Administrator.
//
// Each argument is one command; with no arguments (or after commands that added monitors) the tool reads commands
// from the console. Monitors belong to this process's device handle and disappear when the tool exits.

#define NOMINMAX
#include <windows.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <dxgi1_6.h>
#include <setupapi.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "lumen_vdd_ioctl.h"

using Microsoft::WRL::ComPtr;

namespace {

const GUID kInterfaceGuid = LUMEN_VDD_INTERFACE_GUID_INIT;

HANDLE g_device = INVALID_HANDLE_VALUE;
int g_added = 0;

DWORD PrintLastError(const char* what) {
    const DWORD error = GetLastError();
    char message[256] = "";
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, message,
                   sizeof(message), nullptr);
    for (char* p = message; *p; p++) {
        if (*p == '\r' || *p == '\n') {
            *p = '\0';
        }
    }
    printf("%s failed: error %lu (%s)\n", what, error, message);
    return error;
}

double Milliseconds(const LARGE_INTEGER& start, const LARGE_INTEGER& end) {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return (end.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart;
}

bool OpenDevice() {
    if (g_device != INVALID_HANDLE_VALUE) {
        return true;
    }
    ULONG size = 0;
    CONFIGRET cr = CM_Get_Device_Interface_List_SizeW(&size, const_cast<GUID*>(&kInterfaceGuid), nullptr,
                                                      CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    std::vector<wchar_t> list(size > 1 ? size : 2, L'\0');
    if (cr == CR_SUCCESS) {
        cr = CM_Get_Device_Interface_ListW(const_cast<GUID*>(&kInterfaceGuid), nullptr, list.data(), size,
                                           CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    }
    if (cr != CR_SUCCESS || list[0] == L'\0') {
        printf("Lumen VDD device not found (is the driver installed and the device Root\\LumenVdd present?)\n");
        return false;
    }
    g_device = CreateFileW(list.data(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (g_device == INVALID_HANDLE_VALUE) {
        if (PrintLastError("open device") == ERROR_ACCESS_DENIED) {
            printf("only Administrators and SYSTEM may open the device: run from an elevated console\n");
        }
        return false;
    }
    return true;
}

bool Ioctl(DWORD code, const void* in, DWORD in_size, void* out, DWORD out_size, const char* what,
           double* ms = nullptr) {
    if (!OpenDevice()) {
        return false;
    }
    DWORD returned = 0;
    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);
    const BOOL ok = DeviceIoControl(g_device, code, const_cast<void*>(in), in_size, out, out_size, &returned, nullptr);
    QueryPerformanceCounter(&end);
    if (!ok) {
        SetLastError(PrintLastError(what));  // keep the IOCTL error for the caller, printf may change it
        return false;
    }
    if (ms != nullptr) {
        *ms = Milliseconds(start, end);
    }
    return true;
}

bool ParseMode(const std::string& text, LUMEN_VDD_MODE* mode) {
    char tail = 0;
    return sscanf_s(text.c_str(), "%ux%u@%u%c", &mode->width, &mode->height, &mode->refresh_hz, &tail, 1) == 3;
}

// "2560x1440@60,1920x1080@60" -> modes; false (with a message) on a bad list.
bool ParseModes(const std::string& text, LUMEN_VDD_MODE* modes, uint32_t* count) {
    std::istringstream list(text);
    std::string mode;
    *count = 0;
    while (std::getline(list, mode, ',')) {
        if (*count == LUMEN_VDD_MAX_MODES || !ParseMode(mode, &modes[*count])) {
            printf("bad mode '%s' (expected e.g. 2560x1440@60, at most %u modes)\n", mode.c_str(),
                   LUMEN_VDD_MAX_MODES);
            return false;
        }
        (*count)++;
    }
    return *count > 0;
}

// Monitor name from an EDID's display name descriptor (0xFC), or "?".
std::string EdidName(const std::vector<uint8_t>& edid) {
    for (size_t d = 54; d + 18 <= 126 && edid.size() >= 128; d += 18) {
        if (edid[d] == 0 && edid[d + 1] == 0 && edid[d + 3] == 0xFC) {
            std::string name(reinterpret_cast<const char*>(&edid[d + 5]), 13);
            return name.substr(0, name.find_first_of("\n"));
        }
    }
    return "?";
}

// EDID of the first present monitor that is not a Lumen VDD monitor (manufacturer "LMN").
std::vector<uint8_t> HostMonitorEdid() {
    HDEVINFO devices = SetupDiGetClassDevsW(&GUID_DEVCLASS_MONITOR, nullptr, nullptr, DIGCF_PRESENT);
    if (devices == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::vector<uint8_t> result;
    SP_DEVINFO_DATA device = {sizeof(device)};
    for (DWORD i = 0; result.empty() && SetupDiEnumDeviceInfo(devices, i, &device); i++) {
        HKEY key = SetupDiOpenDevRegKey(devices, &device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) {
            continue;
        }
        std::vector<uint8_t> edid(LUMEN_VDD_MAX_EDID);
        DWORD size = static_cast<DWORD>(edid.size());
        if (RegQueryValueExW(key, L"EDID", nullptr, nullptr, edid.data(), &size) == ERROR_SUCCESS && size >= 128 &&
            !(edid[8] == 0x31 && edid[9] == 0xAE)) {
            edid.resize(size);
            result = std::move(edid);
        }
        RegCloseKey(key);
    }
    SetupDiDestroyDeviceInfoList(devices);
    return result;
}

std::vector<uint8_t> LoadEdid(const std::string& source) {
    if (source == "host") {
        auto edid = HostMonitorEdid();
        if (edid.empty()) {
            printf("no physical monitor EDID found\n");
        }
        return edid;
    }
    std::ifstream file(source, std::ios::binary);
    std::vector<uint8_t> edid((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (edid.empty()) {
        printf("cannot read EDID file '%s'\n", source.c_str());
    }
    return edid;
}

const char* StateName(uint32_t state) {
    switch (state) {
    case LUMEN_VDD_MONITOR_ACTIVE:
        return "active";
    case LUMEN_VDD_MONITOR_SUSPENDED:
        return "suspended";
    case LUMEN_VDD_MONITOR_CHANGING:
        return "changing";
    default:
        return "?";
    }
}

void PrintMonitor(const LUMEN_VDD_MONITOR& monitor) {
    printf("monitor %u: %s, adapter LUID %08X:%08X, target id %u%s%s\n  modes:", monitor.id,
           StateName(monitor.state), static_cast<uint32_t>(monitor.adapter_luid_high), monitor.adapter_luid_low,
           monitor.target_id, monitor.flags & LUMEN_VDD_MONITOR_OWNED ? ", ours" : ", another handle's",
           monitor.flags & LUMEN_VDD_MONITOR_OWN_EDID ? ", own EDID" : "");
    for (uint32_t i = 0; i < monitor.mode_count && i < LUMEN_VDD_MAX_MODES; i++) {
        printf(" %ux%u@%u", monitor.modes[i].width, monitor.modes[i].height, monitor.modes[i].refresh_hz);
    }
    printf("\n");
}

void CmdInfo() {
    LUMEN_VDD_INFO info = {};
    if (!Ioctl(IOCTL_LUMEN_VDD_GET_INFO, nullptr, 0, &info, sizeof(info), "GET_INFO")) {
        return;
    }
    printf("contract %u.%u (this tool: %u.%u)\n", info.contract_major, info.contract_minor,
           LUMEN_VDD_CONTRACT_MAJOR, LUMEN_VDD_CONTRACT_MINOR);
    printf("driver %u.%u.%u, IddCx 0x%04X\n", info.driver_version >> 16, (info.driver_version >> 8) & 0xFF,
           info.driver_version & 0xFF, info.iddcx_version);
    printf("max monitors %u, max modes %u\n", info.max_monitors, info.max_modes);
    printf("caps 0x%08X:", info.caps);
    const struct {
        uint32_t bit;
        const char* name;
    } kCaps[] = {
        {LUMEN_VDD_CAP_ADD_REMOVE, "add-remove"},     {LUMEN_VDD_CAP_RENDER_ADAPTER, "render-adapter"},
        {LUMEN_VDD_CAP_SET_MODES, "set-modes"},       {LUMEN_VDD_CAP_LIST_MONITORS, "list-monitors"},
        {LUMEN_VDD_CAP_EDID, "edid"},                 {LUMEN_VDD_CAP_DIRECT_FRAMES, "direct-frames"},
    };
    for (const auto& cap : kCaps) {
        if (info.caps & cap.bit) {
            printf(" %s", cap.name);
        }
    }
    printf("\n");
    if (info.contract_major != LUMEN_VDD_CONTRACT_MAJOR) {
        printf("WARNING: contract major differs, this tool cannot talk to this driver\n");
    }
}

void CmdAdd(std::istringstream& args) {
    static LUMEN_VDD_ADD_MONITOR request;  // 1.4 KB, keep it off the stack
    request = {};
    std::string modes, option;
    if (!(args >> request.id >> modes)) {
        printf("usage: add <id> <W>x<H>@<Hz>[,<W>x<H>@<Hz>...] [edid=host|<file>]\n");
        return;
    }
    if (!ParseModes(modes, request.modes, &request.mode_count)) {
        return;
    }
    if (args >> option) {
        if (option.rfind("edid=", 0) != 0) {
            printf("unknown option '%s'\n", option.c_str());
            return;
        }
        const auto edid = LoadEdid(option.substr(5));
        if (edid.empty()) {
            return;
        }
        if (edid.size() > LUMEN_VDD_MAX_EDID) {
            printf("EDID is %zu bytes, at most %u\n", edid.size(), LUMEN_VDD_MAX_EDID);
            return;
        }
        memcpy(request.edid, edid.data(), edid.size());
        request.edid_size = static_cast<uint32_t>(edid.size());
        printf("EDID: %u bytes, monitor name '%s'\n", request.edid_size, EdidName(edid).c_str());
    }
    LUMEN_VDD_MONITOR_ADDED added = {};
    double ms = 0;
    if (!Ioctl(IOCTL_LUMEN_VDD_ADD_MONITOR, &request, sizeof(request), &added, sizeof(added), "ADD_MONITOR", &ms)) {
        return;
    }
    g_added++;
    printf("monitor %u added in %.1f ms: adapter LUID %08X:%08X, target id %u\n", request.id, ms,
           static_cast<uint32_t>(added.adapter_luid_high), added.adapter_luid_low, added.target_id);
}

void CmdRemove(std::istringstream& args) {
    LUMEN_VDD_REMOVE_MONITOR request = {};
    if (!(args >> request.id)) {
        printf("usage: remove <id>\n");
        return;
    }
    double ms = 0;
    if (Ioctl(IOCTL_LUMEN_VDD_REMOVE_MONITOR, &request, sizeof(request), nullptr, 0, "REMOVE_MONITOR", &ms)) {
        printf("monitor %u removed in %.1f ms\n", request.id, ms);
    } else if (GetLastError() == ERROR_ACCESS_DENIED) {
        printf("monitor %u was added through another handle (another lumen-vdd-ctl); only its owner removes it\n",
               request.id);
    }
}

void CmdSet(std::istringstream& args) {
    LUMEN_VDD_SET_MODES request = {};
    std::string modes;
    if (!(args >> request.id >> modes)) {
        printf("usage: set <id> <W>x<H>@<Hz>[,<W>x<H>@<Hz>...]\n");
        return;
    }
    if (!ParseModes(modes, request.modes, &request.mode_count)) {
        return;
    }
    LUMEN_VDD_MONITOR monitor = {};
    double ms = 0;
    if (Ioctl(IOCTL_LUMEN_VDD_SET_MODES, &request, sizeof(request), &monitor, sizeof(monitor), "SET_MODES", &ms)) {
        printf("modes set in %.1f ms\n", ms);
        PrintMonitor(monitor);
    } else if (GetLastError() == ERROR_ACCESS_DENIED) {
        printf("monitor %u was added through another handle; only its owner changes it\n", request.id);
    }
}

void CmdList() {
    LUMEN_VDD_INFO info = {};
    if (!Ioctl(IOCTL_LUMEN_VDD_GET_INFO, nullptr, 0, &info, sizeof(info), "GET_INFO")) {
        return;
    }
    if (!(info.caps & LUMEN_VDD_CAP_LIST_MONITORS)) {
        printf("this driver has no LIST_MONITORS\n");
        return;
    }
    std::vector<LUMEN_VDD_MONITOR> monitors(info.max_monitors);
    DWORD returned = 0;
    if (!DeviceIoControl(g_device, IOCTL_LUMEN_VDD_LIST_MONITORS, nullptr, 0, monitors.data(),
                         static_cast<DWORD>(monitors.size() * sizeof(LUMEN_VDD_MONITOR)), &returned, nullptr)) {
        PrintLastError("LIST_MONITORS");
        return;
    }
    const size_t count = returned / sizeof(LUMEN_VDD_MONITOR);
    printf("%zu monitors\n", count);
    for (size_t i = 0; i < count; i++) {
        PrintMonitor(monitors[i]);
    }
}

std::vector<std::pair<DXGI_ADAPTER_DESC1, LUID>> Adapters() {
    std::vector<std::pair<DXGI_ADAPTER_DESC1, LUID>> result;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return result;
    }
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        result.emplace_back(desc, desc.AdapterLuid);
    }
    return result;
}

void CmdAdapters() {
    for (const auto& [desc, luid] : Adapters()) {
        printf("%08X:%08X  vendor %04X  %ls\n", static_cast<uint32_t>(luid.HighPart), luid.LowPart, desc.VendorId,
               desc.Description);
    }
}

void CmdRender(std::istringstream& args) {
    std::string target;
    if (!(args >> target)) {
        printf("usage: render nvidia | none | <high>:<low> (LUID in hex, see 'adapters')\n");
        return;
    }
    LUMEN_VDD_SET_RENDER_ADAPTER request = {};
    if (target == "nvidia") {
        bool found = false;
        for (const auto& [desc, luid] : Adapters()) {
            if (desc.VendorId == 0x10DE) {
                request.luid_low = luid.LowPart;
                request.luid_high = luid.HighPart;
                found = true;
                break;
            }
        }
        if (!found) {
            printf("no NVIDIA adapter\n");
            return;
        }
    } else if (target != "none") {
        unsigned high = 0, low = 0;
        if (sscanf_s(target.c_str(), "%x:%x", &high, &low) != 2) {
            printf("bad LUID '%s'\n", target.c_str());
            return;
        }
        request.luid_low = low;
        request.luid_high = static_cast<int32_t>(high);
    }
    if (Ioctl(IOCTL_LUMEN_VDD_SET_RENDER_ADAPTER, &request, sizeof(request), nullptr, 0, "SET_RENDER_ADAPTER")) {
        printf("render adapter set to %08X:%08X\n", static_cast<uint32_t>(request.luid_high), request.luid_low);
    }
}

void PrintHelp() {
    printf("commands:\n"
           "  info                                   driver version, contract, capabilities\n"
           "  add <id> <W>x<H>@<Hz>[,...] [edid=host|<file>]\n"
           "                                         add monitor <id> (0..3), first mode preferred;\n"
           "                                         edid=host: EDID of the physical monitor\n"
           "  set <id> <W>x<H>@<Hz>[,...]            change the modes of monitor <id> (it is replugged)\n"
           "  list                                   all monitors of the driver\n"
           "  remove <id>                            remove monitor <id>\n"
           "  adapters                               list GPUs with their LUIDs\n"
           "  render nvidia | none | <high>:<low>    preferred render GPU for the monitors\n"
           "  quit                                   exit (monitors added here disappear)\n");
}

// Returns false on quit.
bool Run(const std::string& line) {
    std::istringstream args(line);
    std::string command;
    if (!(args >> command)) {
        return true;
    }
    if (command == "quit" || command == "exit") {
        return false;
    } else if (command == "info") {
        CmdInfo();
    } else if (command == "add") {
        CmdAdd(args);
    } else if (command == "remove") {
        CmdRemove(args);
    } else if (command == "set") {
        CmdSet(args);
    } else if (command == "list") {
        CmdList();
    } else if (command == "adapters") {
        CmdAdapters();
    } else if (command == "render") {
        CmdRender(args);
    } else {
        PrintHelp();
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        printf("> %s\n", argv[i]);
        if (!Run(argv[i])) {
            return 0;
        }
    }
    if (argc > 1 && g_added == 0) {
        return 0;
    }
    if (argc == 1) {
        PrintHelp();
    } else {
        printf("monitors stay while this tool runs; 'quit' to exit\n");
    }
    std::string line;
    while (printf("lumen-vdd> "), std::getline(std::cin, line)) {
        if (!Run(line)) {
            break;
        }
    }
    return 0;
}
