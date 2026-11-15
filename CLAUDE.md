# CLAUDE.md — Lumen VDD

Virtual display driver (IddCx, Windows) for **Lumen**, open source remote access to work machines (the Lumen
repository, its `CLAUDE.md` and `docs/multimonitor-plan.md`). User-facing description: `README.md`.
Talk to the project owner in **Russian**. Code, identifiers and comments are in English.

## Why a separate project (owner's decision, 2026-10-29)

- The driver is a **separate repository** with its own version, build (C++/WDK) and releases. The only link to Lumen
  is the **contract** `include/lumen_vdd_ioctl.h`.
- Lumen ships the built, signed package inside its installer (the "Virtual monitors" option); there is no separate
  end-user installer. Without the driver Lumen Host works as before.
- Lumen works with any driver of the same contract major version; new features are checked by `caps` bits.
- Lumen's auto-update runs `lumen-vdd-setup update` (only replaces an older installed driver); the Lumen installer
  with the option ticked runs `install`.

## Split of duties

The driver is "dumb": **plug / unplug / change modes** of a monitor. All policy lives in Lumen Host: when to create
monitors, which resolutions, where they stand (the host computes positions and calls `SetDisplayConfig`), scale, what
the user sees. Monitor settings are stored by the Lumen Host service, not by the driver.

## Contract (`include/lumen_vdd_ioctl.h`, currently 1.1)

The header is the source of truth. Lumen mirrors it in `crates/vdd` with a test that IOCTL codes and struct sizes
match (it reads this header when the repositories sit side by side, or from `LUMEN_VDD_HEADER`). Any contract change:
header first, then Lumen's mirror.

- `GET_INFO` → contract major.minor, caps, max monitors (4), max modes (32), driver version, IddCx version (info only).
- `ADD_MONITOR { id, modes[], edid? }` → adapter LUID + target id. `id` 0..3 is **stable**: connector index plus a
  generated EDID that depends only on the id (manufacturer `LMN`, product `0x4C00 + id`, serial `0x4C560000 + id`,
  name "LumenVDA", timing always 1920×1080@60, physical size unknown). Windows keys scale and position on it, so
  changing modes must not change the EDID. A caller EDID (128..1024 bytes) is used as given; two monitors cannot share
  an EDID. Modes always come from `modes`, never from the EDID.
- `REMOVE_MONITOR`, `LIST_MONITORS` (all monitors, with an "owned by this handle" flag), `SET_MODES` (unplug + plug
  back, same id/EDID/owner), `SET_RENDER_ADAPTER { luid }` (a preference only).
- **Monitors belong to the handle** that added them (`EvtCleanupCallback` on the `WDFFILEOBJECT`): handle closed or
  process died → its monitors go. Only the owner may remove or change a monitor (`ERROR_ACCESS_DENIED`). No heartbeat.
- Device access: SYSTEM and Administrators only (SDDL `D:P(A;;GA;;;SY)(A;;GA;;;BA)` in the INF).
- Sleep (D0 exit): IddCx destroys the adapter and monitors; the driver remembers monitors with a live owner and
  recreates them when the adapter is back (`SUSPENDED` state meanwhile).
- `DIRECT_FRAMES` cap is reserved; capture is Desktop Duplication in Lumen, as for a physical monitor.

## Verified on hardware (do not "fix" these)

- **Signing:** a self-signed UMDF package installs and loads with Secure Boot on, test signing off, HVCI on, no
  watermark, once its certificate is in `LocalMachine\Root` + `TrustedPublisher` (Windows 11 26200, 2026-10-29).
- **Mode change in place does not work:** `IddCxMonitorUpdateModes` succeeds, but Windows keeps offering the modes the
  monitor arrived with. `SET_MODES` therefore reconnects the monitor; `flags` is reserved.
- **Handle ownership cleanup works** (`quit`, killing the process in Task Manager).
- **Windows 10 without `WUDFRD.inf`** (0.2.1): `UpdateDriverForPlugAndPlayDevices` failed with `0xE0000219`. The INF
  declares the `WUDFRd` service itself instead of `Include/Needs`. InfVerif warning 2084 is expected.
- **Device lookup across all classes** (0.2.1): a failed install can leave a root device without class; setup looks
  through all `ROOT` devices of any class and keeps exactly one.
- **Hardware cursor** (0.2.2, `driver/cursor_watch.cpp`): `IddCxMonitorSetupHardwareCursor` on every swapchain.
  Windows then stops drawing the cursor into the image and Lumen draws it. Driver and Lumen Host come from one
  installer.
- **Name "LumenVDA"** (0.2.3) in the EDID descriptor and `EndPointDiagnostics`; manufacturer, product and serial are
  unchanged, so Windows keeps recognizing existing monitors and Lumen finds its own by `LMN`.
- Default scale without a physical size in the EDID is 100 %, also at 3840×2160.
- The adapter LUID changes on every reinstall and device restart: always take it from the reply, never store it.

Not verified: monitors across sleep, the sign-in and lock screens, an NVIDIA driver update with a live service,
`render`.

## Build (pitfalls)

- `scripts/build.ps1`: Release x64 with the **64-bit MSBuild** (`MSBuild\Current\Bin\amd64`); the 32-bit one looks for
  `x86\InfVerif.dll`, which WDK 10.0.28000 does not ship. `/W4`, warnings are errors. `build/` is not in git.
- Headers for IddCx are only in WDK `Include\10.0.28000.0` → `WindowsTargetPlatformVersion = 10.0.28000.0`. Runtime
  targets are set by IddCx 1.4 (Windows 10 1903+) and UMDF 2.25, not by the WDK version.
- The driver version lives in one place, `driver/lumen_vdd.vcxproj` (`LumenVddVersion*`); it goes into the code and
  into `DriverVer` of the INF (stampinf `TimeStamp`).
- `scripts/sign.ps1`: certificate `CN=Lumen VDD Dev` in `CurrentUser\My` (Code Signing EKU only, RSA 3072, one year,
  non-exportable key). Signs the DLL, the catalog and both tools; the catalog holds the PE hash without the embedded
  signature, so signing the DLL after `Inf2Cat` keeps the catalog valid. Signatures are timestamped
  (`timestamp.digicert.com`, 3 attempts; `-TimestampUrl ''` for offline builds), so released packages keep installing
  after the certificate expires (2027-10-29). Private keys never go into the repository.
- Release: `scripts/build.ps1`, then `scripts/package.ps1` → `build\release\lumen-vdd-<version>.zip` + `.sha256`
  (checks that the package's `DriverVer` matches the project version); tag `v<version>`.
- Package (`build\package`): `lumen_vdd.inf`, `.cat`, `.dll`, `lumen-vdd.cer`, `lumen-vdd-setup.exe`,
  `lumen-vdd-ctl.exe` (manual checks only; the Lumen installer does not ship it).
- `lumen-vdd-setup.exe` is our own (SetupAPI + newdev + crypt32), because `devcon` from the WDK may not be
  redistributed.
- Driver log: `OutputDebugString` with the prefix `LumenVdd:` (DebugView, "Capture Global Win32").

## License

MIT (`LICENSE-MIT`). `microsoft/Windows-driver-samples` (including `IddSampleDriver`) is **MS-PL**, so the driver is
written from the IddCx documentation: the sample and its forks (SudoVDA, Virtual-Display-Driver) are a reference for
how to call the API only. **Never copy code from them.**

## Process

As in Lumen: step by step; before a step say what is done and how it is checked (a measurable criterion); a step is
done only when checked; after a step, a **local commit**. `push` — only when the owner says so. Installing the driver
or the certificate (anything elevated) is done by the **owner**; warn before anything that touches the owner's
windows or monitors. Say honestly what was checked on hardware and what was only built.

## Current state (2026-11-15)

Driver **0.2.3**, contract **1.1**, shipped with Lumen 0.7.x–0.8.x. Preparing the first public release on GitHub.
