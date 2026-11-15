# Lumen VDD

A virtual display driver for Windows. It adds and removes monitors on demand: a program opens the driver, asks for
a monitor with a set of resolutions, and Windows sees a new display (named **LumenVDA**). When the program closes
its handle or exits, its monitors disappear.

Lumen VDD is part of [Lumen](https://github.com/aimbyLeet/lumen), open source remote access to work machines. The Lumen
host uses it to give a remote client extra screens with the client's resolutions. The driver
does not depend on Lumen, so other programs can use it through the same small IOCTL interface.

- **Indirect Display driver** (IddCx, user-mode UMDF 2). No kernel code.
- **Monitors on demand**, up to 4, each with its own list of up to 32 modes.
- **Stable identity**: the same monitor id always looks like the same monitor to Windows, so Windows remembers its
  scale and position across restarts and reboots.
- **Owned by a handle**: monitors belong to the process that added them and go away with it. No heartbeat needed.
- **Hardware cursor**: Windows does not draw the cursor into the image; capture tools get the cursor separately.
- **Custom EDID** (optional): a monitor can pretend to be another one, for example the physical monitor of the
  machine.
- Only **SYSTEM and Administrators** may open the driver.

## Requirements

- Windows 10 version 1903 or later, or Windows 11; x64.
- Tested on Windows 11 25H2 (build 26200, Secure Boot and HVCI on) and Windows 10 22H2 (build 19045).

## A note on signing and trust

Release packages are signed with the project's own self-signed certificate (`CN=Lumen VDD Dev`). It is not a
Microsoft signature. `lumen-vdd-setup install` adds this certificate to the machine's **Trusted Root** and
**Trusted Publishers** stores. Windows then installs the driver without warnings, with Secure Boot on and no test
signing mode. Signatures carry a timestamp, so a released package keeps installing after the certificate expires.

Doing this means you trust the holder of that certificate's private key to sign code for your machine. The
certificate can only sign code (Code Signing usage only), and the private key never leaves the developer's machine.
`lumen-vdd-setup uninstall` removes the certificate again. If you would rather not trust it, build the driver
yourself (see below): your build is signed with a certificate created on your own machine.

## Install a release package

1. Download `lumen-vdd-<version>.zip` from [Releases](https://github.com/aimbyLeet/lumen-vdd/releases) and check its
   SHA-256 against the one on the release page:

   ```powershell
   Get-FileHash .\lumen-vdd-0.2.3.zip -Algorithm SHA256
   ```

2. Unpack it. The folder contains the driver (`lumen_vdd.inf`, `lumen_vdd.dll`, `lumen_vdd.cat`), the certificate
   (`lumen-vdd.cer`) and two tools (`lumen-vdd-setup.exe`, `lumen-vdd-ctl.exe`).
3. Open a console **as Administrator** in that folder and run:

   ```powershell
   .\lumen-vdd-setup.exe install
   .\lumen-vdd-setup.exe status
   ```

   `status` should report the device as working. Device Manager now shows **Lumen Virtual Display Adapter** under
   Display adapters. It has no monitors yet: monitors appear only when a program asks for them.

Lumen users do not need to do this: the Lumen installer includes the driver (the "Virtual monitors" option).

`lumen-vdd-setup` commands (run as Administrator, except `status`):

| Command     | What it does |
|-------------|--------------|
| `install`   | Trusts the package certificate, creates the device `Root\LumenVdd` if needed, installs this package (over any older or newer Lumen VDD) and removes older Lumen VDD packages from the driver store. |
| `update`    | Same as `install`, but only if Lumen VDD is already installed and older than this package; otherwise does nothing. |
| `uninstall` | Removes the device, every Lumen VDD package and the certificate. |
| `status`    | Prints what is installed. |

Exit codes: `0` done (for `status`: installed and working), `1` error, `2` (`status`) not installed, `3010` done,
reboot needed.

## Try it by hand

`lumen-vdd-ctl` talks to the driver directly. Run it as Administrator from the package folder:

```text
> lumen-vdd-ctl.exe
lumen-vdd> info
lumen-vdd> add 0 2560x1440@60,1920x1080@60
lumen-vdd> list
lumen-vdd> set 0 3440x1440@60
lumen-vdd> remove 0
lumen-vdd> quit
```

| Command | What it does |
|---------|--------------|
| `info` | Driver version, interface version, capabilities. |
| `add <id> <W>x<H>@<Hz>[,...] [edid=host\|<file>]` | Adds monitor `<id>` (0..3); the first mode is the preferred one. `edid=host` copies the EDID of the physical monitor. |
| `set <id> <W>x<H>@<Hz>[,...]` | Replaces the modes of a monitor (it is unplugged and plugged back in). |
| `list` | All monitors of the driver, including those of other programs. |
| `remove <id>` | Removes a monitor. |
| `adapters` | Lists GPUs with their LUIDs. |
| `render nvidia \| none \| <high>:<low>` | Preferred GPU to render the virtual monitors. |
| `quit` | Exits; the monitors added by this tool disappear. |

Commands can also be passed as arguments, one per argument: `lumen-vdd-ctl.exe info "add 0 1920x1080@60"`.

## Build from source

You need:

- Visual Studio 2022 or later with **Desktop development with C++**;
- **Windows Driver Kit 10.0.28000** with its Visual Studio extension, and Windows SDK 10.0.28000. The projects
  target this version (`WindowsTargetPlatformVersion`). This only sets the build tools: the driver still runs on
  Windows 10 1903 and later.

Steps:

1. Clone the repository.
2. Build and sign (as a regular user, no Administrator needed):

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1
   ```

   The script builds Release x64 with the 64-bit MSBuild, then runs `scripts\sign.ps1`. The first run creates a
   self-signed certificate `CN=Lumen VDD Dev` in your `CurrentUser\My` store. It is valid for one year and its
   private key cannot be exported. Every file of the package is signed with it and timestamped by
   `timestamp.digicert.com` (only file hashes are sent). Without internet access, sign with
   `scripts\sign.ps1 -TimestampUrl ''`: such a package stops installing when the certificate expires.
3. The package is in `build\package`. Install it as described above (`lumen-vdd-setup.exe install`, as
   Administrator).
4. Optional: `scripts\package.ps1` packs it into `build\release\lumen-vdd-<version>.zip` with a `.sha256` file, the
   same way as a release.

Driver log: `OutputDebugString` with the prefix `LumenVdd:`. View it with
[DebugView](https://learn.microsoft.com/sysinternals/downloads/debugview) ("Capture Global Win32").

## Use it from your program

The whole interface is one header, [`include/lumen_vdd_ioctl.h`](include/lumen_vdd_ioctl.h). It lists the IOCTL
codes as plain numbers and fixes the struct sizes, so any language can use it.

1. Find the device interface `{0F76755D-FA51-4FB9-A19D-39A14E1AEA68}` (`CM_Get_Device_Interface_List`) and open it
   with `CreateFile(GENERIC_READ | GENERIC_WRITE)`. This needs SYSTEM or Administrator rights.
2. `IOCTL_LUMEN_VDD_GET_INFO`: check `contract_major` (currently 1). Check optional features with the `caps` bits.
3. `IOCTL_LUMEN_VDD_ADD_MONITOR` with a monitor id and modes. The reply gives the adapter LUID and target id, which you
   can use with `QueryDisplayConfig` / `SetDisplayConfig` to place the monitor.
4. `SET_MODES`, `LIST_MONITORS`, `REMOVE_MONITOR`, `SET_RENDER_ADAPTER` as needed.
5. Close the handle: your monitors are removed.

Errors come back as Win32 errors from `DeviceIoControl`. They are listed at the top of the header.

The interface has its own version, separate from the driver's. A minor version only adds features, behind new
`caps` bits. The driver version is set in one place, `driver/lumen_vdd.vcxproj` (`LumenVddVersion*`).

## Repository layout

| Path | Contents |
|------|----------|
| `include/lumen_vdd_ioctl.h` | The interface between the driver and programs. |
| `driver/` | The driver (`lumen_vdd.dll`) and its INF. |
| `tools/lumen-vdd-setup/` | Installs, updates and removes the driver package. |
| `tools/lumen-vdd-ctl/` | Manual control for testing. |
| `scripts/` | `build.ps1` (build + sign), `sign.ps1` (signing only), `package.ps1` (release archive). |

## License

MIT, see [LICENSE-MIT](LICENSE-MIT).

The driver is written from the IddCx documentation. It is not derived from Microsoft's `IddSampleDriver`, which is
under a different license (MS-PL).
