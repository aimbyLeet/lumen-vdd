// lumen-vdd-setup: installs and removes the Lumen VDD driver package. Run as Administrator (the Lumen installer runs
// it elevated). Lives in the driver package folder next to lumen_vdd.inf, lumen_vdd.cat, lumen_vdd.dll and
// lumen-vdd.cer.
//
//   lumen-vdd-setup install     trust the package certificate (LocalMachine Root + TrustedPublisher), create the root
//                               device Root\LumenVdd if missing, install this package on it (also over a newer or
//                               older Lumen VDD), remove older Lumen VDD packages from the driver store
//   lumen-vdd-setup update      as install, but only where Lumen VDD is installed and older than this package
//                               (Lumen's silent update); else nothing, exit 0
//   lumen-vdd-setup uninstall   remove the device, every Lumen VDD package and the certificates with the package
//                               certificate's subject
//   lumen-vdd-setup status      print what is installed
//
// Exit codes: 0 done (status: installed and working), 1 error, 2 status: not installed, 3010 done, reboot needed.

#define NOMINMAX
#include <windows.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <initguid.h>  // define the DEVPKEY_* used below
#include <devpkey.h>
#include <newdev.h>
#include <setupapi.h>
#include <wincrypt.h>

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kHardwareId[] = L"Root\\LumenVdd";
constexpr wchar_t kInfName[] = L"lumen_vdd.inf";
constexpr wchar_t kCertName[] = L"lumen-vdd.cer";
constexpr wchar_t kDeviceDescription[] = L"Lumen Virtual Display Adapter";
constexpr const wchar_t* kCertStores[] = {L"Root", L"TrustedPublisher"};

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitNotInstalled = 2;
constexpr int kExitReboot = 3010;

int Fail(const char* what, DWORD error = GetLastError()) {
    char message[256] = "";
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, message,
                   sizeof(message), nullptr);
    for (char* p = message; *p; p++) {
        if (*p == '\r' || *p == '\n') {
            *p = '\0';
        }
    }
    printf("ERROR: %s: 0x%08lX (%s)\n", what, error, message);
    return kExitError;
}

bool IsAdministrator() {
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID admins = nullptr;
    BOOL member = FALSE;
    if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0,
                                 0, &admins)) {
        CheckTokenMembership(nullptr, admins, &member);
        FreeSid(admins);
    }
    return member != FALSE;
}

std::wstring PackageDir() {
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path, length);
    return dir.substr(0, dir.find_last_of(L'\\'));
}

std::vector<BYTE> ReadFileBytes(const std::wstring& path) {
    std::vector<BYTE> data;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return data;
    }
    LARGE_INTEGER size;
    DWORD read = 0;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 20)) {
        data.resize(static_cast<size_t>(size.QuadPart));
        if (!ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) || read != data.size()) {
            data.clear();
        }
    }
    CloseHandle(file);
    return data;
}

// The package certificate, or nullptr (free with CertFreeCertificateContext).
PCCERT_CONTEXT LoadPackageCert() {
    const auto data = ReadFileBytes(PackageDir() + L"\\" + kCertName);
    if (data.empty()) {
        return nullptr;
    }
    return CertCreateCertificateContext(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, data.data(),
                                        static_cast<DWORD>(data.size()));
}

HCERTSTORE OpenMachineStore(const wchar_t* name, DWORD flags = 0) {
    return CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE | flags, name);
}

std::string CertSubject(PCCERT_CONTEXT cert) {
    char name[256] = "";
    CertGetNameStringA(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, sizeof(name));
    return name;
}

bool HasHardwareId(HDEVINFO devices, SP_DEVINFO_DATA* device) {
    wchar_t ids[1024] = {};
    if (!SetupDiGetDeviceRegistryPropertyW(devices, device, SPDRP_HARDWAREID, nullptr, reinterpret_cast<BYTE*>(ids),
                                           sizeof(ids) - 2 * sizeof(wchar_t), nullptr)) {
        return false;
    }
    for (const wchar_t* id = ids; *id; id += wcslen(id) + 1) {
        if (_wcsicmp(id, kHardwareId) == 0) {
            return true;
        }
    }
    return false;
}

// Calls `fn(devices, device)` for every Root\LumenVdd device, present or not; returns how many there are. Any
// class: a failed install leaves the device with the NULL driver and no class (seen on Windows 10, 2026-10-30), and
// such a device was missed, then a second one created next to it.
template <typename Fn>
int ForEachDevice(Fn fn) {
    HDEVINFO devices = SetupDiGetClassDevsW(nullptr, L"ROOT", nullptr, DIGCF_ALLCLASSES);
    if (devices == INVALID_HANDLE_VALUE) {
        return 0;
    }
    int count = 0;
    SP_DEVINFO_DATA device = {sizeof(device)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devices, i, &device); i++) {
        if (HasHardwareId(devices, &device)) {
            count++;
            fn(devices, &device);
        }
    }
    SetupDiDestroyDeviceInfoList(devices);
    return count;
}

std::wstring StringProperty(HDEVINFO devices, SP_DEVINFO_DATA* device, const DEVPROPKEY& key) {
    wchar_t value[512] = L"";
    DEVPROPTYPE type = 0;
    if (!SetupDiGetDevicePropertyW(devices, device, &key, &type, reinterpret_cast<BYTE*>(value), sizeof(value) - 2,
                                   nullptr, 0) ||
        type != DEVPROP_TYPE_STRING) {
        return L"";
    }
    return value;
}

std::wstring InstanceId(HDEVINFO devices, SP_DEVINFO_DATA* device) {
    wchar_t id[MAX_DEVICE_ID_LEN] = L"";
    SetupDiGetDeviceInstanceIdW(devices, device, id, MAX_DEVICE_ID_LEN, nullptr);
    return id;
}

// Published names (oemNN.inf) of every Lumen VDD package in the driver store.
std::vector<std::wstring> LumenPackages() {
    std::vector<std::wstring> result;
    wchar_t windows[MAX_PATH];
    GetWindowsDirectoryW(windows, MAX_PATH);
    const std::wstring inf_dir = std::wstring(windows) + L"\\INF\\";
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((inf_dir + L"oem*.inf").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) {
        return result;
    }
    do {
        const std::wstring path = inf_dir + found.cFileName;
        DWORD size = 0;
        if (!SetupGetInfInformationW(path.c_str(), INFINFO_INF_NAME_IS_ABSOLUTE, nullptr, 0, &size)) {
            continue;
        }
        std::vector<BYTE> buffer(size);
        auto* info = reinterpret_cast<PSP_INF_INFORMATION>(buffer.data());
        SP_ORIGINAL_FILE_INFO_W original = {sizeof(original)};
        if (SetupGetInfInformationW(path.c_str(), INFINFO_INF_NAME_IS_ABSOLUTE, info, size, nullptr) &&
            SetupQueryInfOriginalFileInformationW(info, 0, nullptr, &original) &&
            _wcsicmp(original.OriginalInfName, kInfName) == 0) {
            // Same file name could be someone else's: check the provider too.
            HINF inf = SetupOpenInfFileW(path.c_str(), nullptr, INF_STYLE_WIN4, nullptr);
            wchar_t provider[64] = L"";
            if (inf != INVALID_HANDLE_VALUE) {
                SetupGetLineTextW(nullptr, inf, L"Version", L"Provider", provider, 64, nullptr);
                SetupCloseInfFile(inf);
            }
            if (wcscmp(provider, L"Lumen") == 0) {
                result.push_back(found.cFileName);
            }
        }
    } while (FindNextFileW(search, &found));
    FindClose(search);
    return result;
}

int TrustCertificate() {
    PCCERT_CONTEXT cert = LoadPackageCert();
    if (cert == nullptr) {
        return Fail("cannot read the package certificate lumen-vdd.cer");
    }
    for (const wchar_t* name : kCertStores) {
        HCERTSTORE store = OpenMachineStore(name);
        if (store == nullptr || !CertAddCertificateContextToStore(store, cert, CERT_STORE_ADD_USE_EXISTING, nullptr)) {
            const DWORD error = GetLastError();
            if (store != nullptr) {
                CertCloseStore(store, 0);
            }
            CertFreeCertificateContext(cert);
            return Fail("cannot add the certificate to LocalMachine", error);
        }
        CertCloseStore(store, 0);
    }
    printf("certificate '%s' trusted (LocalMachine Root, TrustedPublisher)\n", CertSubject(cert).c_str());
    CertFreeCertificateContext(cert);
    return kExitOk;
}

int CreateRootDevice() {
    HDEVINFO devices = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_DISPLAY, nullptr);
    if (devices == INVALID_HANDLE_VALUE) {
        return Fail("SetupDiCreateDeviceInfoList");
    }
    SP_DEVINFO_DATA device = {sizeof(device)};
    // Hardware id list: REG_MULTI_SZ, double-terminated.
    std::vector<wchar_t> ids(kHardwareId, kHardwareId + wcslen(kHardwareId));
    ids.push_back(L'\0');
    ids.push_back(L'\0');
    int result = kExitOk;
    if (!SetupDiCreateDeviceInfoW(devices, L"DISPLAY", &GUID_DEVCLASS_DISPLAY, kDeviceDescription, nullptr,
                                  DICD_GENERATE_ID, &device)) {
        result = Fail("SetupDiCreateDeviceInfo");
    } else if (!SetupDiSetDeviceRegistryPropertyW(devices, &device, SPDRP_HARDWAREID,
                                                  reinterpret_cast<const BYTE*>(ids.data()),
                                                  static_cast<DWORD>(ids.size() * sizeof(wchar_t)))) {
        result = Fail("SetupDiSetDeviceRegistryProperty");
    } else if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, devices, &device)) {
        result = Fail("DIF_REGISTERDEVICE");
    } else {
        printf("device %ls created\n", InstanceId(devices, &device).c_str());
    }
    SetupDiDestroyDeviceInfoList(devices);
    return result;
}

void RemoveDevices(bool* reboot) {
    ForEachDevice([reboot](HDEVINFO devices, SP_DEVINFO_DATA* device) {
        const std::wstring id = InstanceId(devices, device);
        BOOL need_reboot = FALSE;
        if (DiUninstallDevice(nullptr, devices, device, 0, &need_reboot)) {
            printf("device %ls removed\n", id.c_str());
            *reboot = *reboot || need_reboot;
        } else {
            Fail("DiUninstallDevice");
        }
    });
}

int Install() {
    const std::wstring inf = PackageDir() + L"\\" + kInfName;
    if (GetFileAttributesW(inf.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return Fail("lumen_vdd.inf not found next to lumen-vdd-setup.exe");
    }
    if (TrustCertificate() != kExitOk) {
        return kExitError;
    }

    bool created = false;
    if (ForEachDevice([](HDEVINFO, SP_DEVINFO_DATA*) {}) == 0) {
        if (CreateRootDevice() != kExitOk) {
            return kExitError;
        }
        created = true;
    }
    // INSTALLFLAG_FORCE: this package even if the device has a Lumen VDD driver that Windows ranks higher (newer
    // date or version), so that installing an older Lumen also installs its driver.
    BOOL reboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, kHardwareId, inf.c_str(), INSTALLFLAG_FORCE, &reboot)) {
        const DWORD error = GetLastError();
        if (created) {
            bool ignored = false;
            RemoveDevices(&ignored);
        }
        return Fail("UpdateDriverForPlugAndPlayDevices", error);
    }

    // One adapter: devices left by failed installs of earlier versions go.
    int seen = 0;
    ForEachDevice([&seen](HDEVINFO devices, SP_DEVINFO_DATA* device) {
        if (seen++ == 0) {
            return;
        }
        const std::wstring id = InstanceId(devices, device);
        BOOL need_reboot = FALSE;
        if (DiUninstallDevice(nullptr, devices, device, 0, &need_reboot)) {
            printf("extra device %ls removed\n", id.c_str());
        } else {
            Fail("DiUninstallDevice");
        }
    });

    // Older Lumen VDD packages stay in the driver store after an update: remove all but the one in use.
    std::wstring in_use;
    ForEachDevice([&in_use](HDEVINFO devices, SP_DEVINFO_DATA* device) {
        in_use = StringProperty(devices, device, DEVPKEY_Device_DriverInfPath);
    });
    for (const auto& package : LumenPackages()) {
        if (_wcsicmp(package.c_str(), in_use.c_str()) != 0 && SetupUninstallOEMInfW(package.c_str(), 0, nullptr)) {
            printf("old package %ls removed from the driver store\n", package.c_str());
        }
    }
    printf("driver installed (%ls)%s\n", in_use.c_str(), reboot ? ", reboot needed" : "");
    return reboot ? kExitReboot : kExitOk;
}

// "a.b.c.d" → four numbers (missing or not numbers: 0).
std::vector<unsigned> VersionParts(const std::wstring& text) {
    std::vector<unsigned> parts;
    size_t at = 0;
    while (parts.size() < 4) {
        const size_t dot = text.find(L'.', at);
        const std::wstring part = text.substr(at, dot == std::wstring::npos ? std::wstring::npos : dot - at);
        parts.push_back(static_cast<unsigned>(wcstoul(part.c_str(), nullptr, 10)));
        if (dot == std::wstring::npos) {
            break;
        }
        at = dot + 1;
    }
    parts.resize(4, 0);
    return parts;
}

// The version in this package's lumen_vdd.inf (`DriverVer = date,a.b.c.d`), or "" if not found. The INF is ASCII
// (stampinf); UTF-16 LE with a BOM is read too.
std::wstring PackageVersion() {
    const auto data = ReadFileBytes(PackageDir() + L"\\" + kInfName);
    std::wstring text;
    if (data.size() >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
        text.assign(reinterpret_cast<const wchar_t*>(data.data() + 2), (data.size() - 2) / 2);
    } else {
        text.assign(data.begin(), data.end());
    }
    size_t at = 0;
    while (at < text.size()) {
        const size_t end = text.find(L'\n', at);
        const std::wstring line = text.substr(at, end == std::wstring::npos ? std::wstring::npos : end - at);
        at = end == std::wstring::npos ? text.size() : end + 1;
        if (_wcsnicmp(line.c_str(), L"DriverVer", 9) != 0) {
            continue;
        }
        const size_t comma = line.find(L',');
        if (comma == std::wstring::npos) {
            return L"";
        }
        const std::wstring version = line.substr(comma + 1);
        const size_t first = version.find_first_not_of(L" \t");
        const size_t last = version.find_last_not_of(L" \t\r");
        return first == std::wstring::npos ? L"" : version.substr(first, last - first + 1);
    }
    return L"";
}

// Lumen's update (no "virtual monitors" task): this package only where Lumen VDD is installed and older. Nothing
// where it is not (the owner chooses it per machine) or is this version or newer.
int Update() {
    bool any = false;
    std::wstring installed;
    ForEachDevice([&any, &installed](HDEVINFO devices, SP_DEVINFO_DATA* device) {
        any = true;
        const std::wstring v = StringProperty(devices, device, DEVPKEY_Device_DriverVersion);
        if (!v.empty() && (installed.empty() || VersionParts(v) > VersionParts(installed))) {
            installed = v;
        }
    });
    if (!any) {
        printf("Lumen VDD is not installed here: nothing to update\n");
        return kExitOk;
    }
    const std::wstring package = PackageVersion();
    if (package.empty()) {
        return Fail("lumen_vdd.inf: no DriverVer", ERROR_INVALID_DATA);
    }
    if (!installed.empty() && VersionParts(installed) >= VersionParts(package)) {
        printf("Lumen VDD %ls installed, this package %ls: nothing to update\n", installed.c_str(), package.c_str());
        return kExitOk;
    }
    printf("Lumen VDD %ls installed, this package %ls: updating\n",
           installed.empty() ? L"(no driver)" : installed.c_str(), package.c_str());
    return Install();
}

int Uninstall() {
    bool reboot = false;
    RemoveDevices(&reboot);
    for (const auto& package : LumenPackages()) {
        if (SetupUninstallOEMInfW(package.c_str(), SUOI_FORCEDELETE, nullptr)) {
            printf("package %ls removed from the driver store\n", package.c_str());
        } else {
            Fail("SetupUninstallOEMInf");
        }
    }

    // Every certificate with the package certificate's subject: also those of earlier Lumen VDD versions.
    PCCERT_CONTEXT cert = LoadPackageCert();
    if (cert == nullptr) {
        printf("no lumen-vdd.cer next to lumen-vdd-setup.exe, certificates left as they are\n");
    } else {
        for (const wchar_t* name : kCertStores) {
            HCERTSTORE store = OpenMachineStore(name);
            if (store == nullptr) {
                continue;
            }
            PCCERT_CONTEXT found = nullptr;
            while ((found = CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                                                       CERT_FIND_SUBJECT_NAME, &cert->pCertInfo->Subject,
                                                       nullptr)) != nullptr) {
                // CertDeleteCertificateFromStore frees the context it is given.
                if (!CertDeleteCertificateFromStore(found)) {
                    Fail("CertDeleteCertificateFromStore");
                    break;
                }
                printf("certificate '%s' removed from LocalMachine\\%ls\n", CertSubject(cert).c_str(), name);
            }
            CertCloseStore(store, 0);
        }
        CertFreeCertificateContext(cert);
    }
    printf("uninstalled%s\n", reboot ? ", reboot needed" : "");
    return reboot ? kExitReboot : kExitOk;
}

int Status() {
    int result = kExitNotInstalled;
    const int count = ForEachDevice([&result](HDEVINFO devices, SP_DEVINFO_DATA* device) {
        ULONG status = 0, problem = 0;
        const CONFIGRET cr = CM_Get_DevNode_Status(&status, &problem, device->DevInst, 0);
        const char* state = cr == CR_NO_SUCH_DEVINST ? "not present"
                            : cr != CR_SUCCESS       ? "unknown"
                            : (status & DN_HAS_PROBLEM) ? "problem"
                            : (status & DN_STARTED)     ? "working"
                                                        : "not started";
        printf("device %ls: %s", InstanceId(devices, device).c_str(), state);
        if (cr == CR_SUCCESS && (status & DN_HAS_PROBLEM)) {
            printf(" (code %lu)", problem);
        }
        printf(", driver %ls (%ls)\n", StringProperty(devices, device, DEVPKEY_Device_DriverVersion).c_str(),
               StringProperty(devices, device, DEVPKEY_Device_DriverInfPath).c_str());
        if (strcmp(state, "working") == 0) {
            result = kExitOk;
        } else if (result != kExitOk) {
            result = kExitError;
        }
    });
    if (count == 0) {
        printf("device %ls: not installed\n", kHardwareId);
    }
    const auto packages = LumenPackages();
    printf("driver store: %zu Lumen VDD package(s)", packages.size());
    for (const auto& package : packages) {
        printf(" %ls", package.c_str());
    }
    printf("\n");
    PCCERT_CONTEXT cert = LoadPackageCert();
    if (cert != nullptr) {
        for (const wchar_t* name : kCertStores) {
            HCERTSTORE store = OpenMachineStore(name, CERT_STORE_READONLY_FLAG);  // status runs without admin too
            PCCERT_CONTEXT found =
                store != nullptr ? CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                                                              CERT_FIND_EXISTING, cert, nullptr)
                                 : nullptr;
            printf("certificate '%s' in LocalMachine\\%ls: %s\n", CertSubject(cert).c_str(), name,
                   found != nullptr ? "yes" : "no");
            if (found != nullptr) {
                CertFreeCertificateContext(found);
            }
            if (store != nullptr) {
                CertCloseStore(store, 0);
            }
        }
        CertFreeCertificateContext(cert);
    }
    return result;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring command = argc == 2 ? argv[1] : L"";
    if (command != L"install" && command != L"update" && command != L"uninstall" && command != L"status") {
        printf("usage: lumen-vdd-setup install | update | uninstall | status   (as Administrator)\n");
        return kExitError;
    }
    if (command == L"status") {
        return Status();
    }
    if (!IsAdministrator()) {
        printf("ERROR: run as Administrator\n");
        return kExitError;
    }
    if (command == L"update") {
        return Update();
    }
    return command == L"install" ? Install() : Uninstall();
}
