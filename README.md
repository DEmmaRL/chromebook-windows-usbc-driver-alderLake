# Tiger Lake Chromebook USB-C Driver for Windows

USB-C hotplug and DisplayPort for Tiger Lake Chromebooks on Windows.

An open-source Windows driver (`TigerLakeUsbcMux`) that makes the USB-C ports of Tiger Lake Chromebooks running
[MrChromebox UEFI firmware](https://mrchromebox.tech/) work like on ChromeOS and Linux. Devices plugged in after boot
are detected, cables work either way round, and USB-C hubs and adapters with HDMI or DisplayPort outputs drive
external screens. Built with the help of Claude Opus 5.5.

Without it, Windows only sees what was plugged into a USB-C port before power-on. Unplugging and plugging again does
nothing, and USB-C video output does not work.

Repository: https://github.com/missingtools-dev/chromebook-windows-usbc-driver

> **Status:** test-signed only. Windows loads the driver only with test signing enabled (see [Installation](#installation)).

## Supported hardware

| | |
|---|---|
| Tested | Google **Volta** (Acer Chromebook 514, CB514-1WT), Intel Tiger Lake, MrChromebox UEFI firmware |
| Windows | Windows 11 25H2 and Windows 10 21H2 (USB and DisplayPort) |
| Probably works | Other Tiger Lake ("Volteer" family) Chromebooks with MrChromebox UEFI. Untested; reports welcome |
| Not for | Regular Tiger Lake Windows laptops: their firmware and embedded controller work differently |

## What works

| Function | Status |
|---|---|
| USB hotplug on both ports, normal and flipped cable | Works |
| Devices plugged in after a cold boot | Works |
| Sleep (S3), hibernate and restart, including plugging in or unplugging while asleep | Works |
| USB 3.2 Gen 2 (10 Gbps) devices | Works |
| DisplayPort alt mode: USB-C hubs/adapters with HDMI or DP out, with USB 3 alongside (pin assignment D) | Works |
| Multiple monitors through a DP MST adapter, and both ports driving displays at once (3 external + internal) | Works |
| Charging through a hub's USB-PD passthrough while it drives a display | Works |
| HDMI/DP audio through USB-C | Works with an audio driver that supports display audio |
| 4-lane DisplayPort (pin assignment C/E, display-only adapters) | Untested (no device available) |
| USB4 / Thunderbolt devices | Win11 only, not supported yet (they work as plain USB where they fall back to it) |

Release builds are also tested under Driver Verifier (standard checks and the KMDF verifier) through hotplug,
DisplayPort, sleep, hibernate, driver upgrade, and removal and return of CoolStar's EC bus driver.

## Requirements

1. A supported Chromebook running **MrChromebox UEFI** firmware.
2. **CoolStar's Chromebook drivers** for Windows, in particular the **ChromeOS EC bus driver (`crosecbus`)**. TigerLakeUsbcMux talks to
   the embedded controller through it. It was tested together with CoolStar's `crosectypec` (USB-C PD notification)
   driver. Both come with CoolStar's Chromebook driver installer.
3. **Test signing enabled**, which requires **Secure Boot to be disabled** in the firmware settings.
4. An administrator account.

## Installation

1. Disable Secure Boot in the firmware setup, if it is on.
2. Enable test signing from an elevated command prompt, then restart:

   ```
   bcdedit /set testsigning on
   ```

   Windows shows a "Test Mode" watermark on the desktop from now on. That is expected.
3. Download `TigerLakeUsbcMux-<version>-x64.zip` from the [releases](https://github.com/missingtools-dev/chromebook-windows-usbc-driver/releases) and extract it.
4. Run **`install.cmd`**. It asks for administrator rights and then:
   - adds the release's test certificate (`TigerLakeUsbcMuxTest.cer`) to the machine's trusted root and trusted publisher
     certificate stores;
   - installs the driver with `pnputil`;
   - lists the three devices it covers. All three should show `OK`.

A restart is normally not needed. Plug something in to try it.

The driver installs on three firmware devices, which show up under "System devices":

| Device | Hardware ID | Role |
|---|---|---|
| Tiger Lake PMC USB-C Mux (Chromebook) | `ACPI\INTC1026` | The driver itself |
| Tiger Lake PMC USB-C Mux Agent (Chromebook) | `ACPI\INTC105C` | No function driver needed. Installed so it no longer shows as unknown |
| Tiger Lake Type-C IO Manager (Chromebook) | `ACPI\INTC1072` | Same |

The firmware marks the PMC device as hidden, so Device Manager shows "Tiger Lake PMC USB-C Mux (Chromebook)" only
with View → Show hidden devices.

### About the test certificate

A test-signed driver is only trusted because `install.cmd` adds its signing certificate to the machine's trusted roots.
Whoever holds that certificate's private key could sign other code that this machine would then trust. Only install
releases from this repository. If you prefer, [build the driver yourself](#building-from-source): the build then
creates and uses your own certificate.

## Uninstall

Run **`uninstall.cmd`** from the release folder, then restart. To also remove the test certificate:

```
uninstall.cmd -RemoveCertificate
```

To turn test signing off again: `bcdedit /set testsigning off`, then restart.

## Diagnostics

`usbcctl.exe` is a developer tool. It is not part of the release, but `build.ps1` builds it (see
[Building from source](#building-from-source)). Run it elevated to see what the driver and the hardware are doing:

| Command | Purpose |
|---|---|
| `usbcctl log` | The driver's last 64 actions: start/stop, connect, disconnect, dp-request, dp-enter, hpd, ... |
| `usbcctl muxinfo` | USB-C mux state per port, as reported by the embedded controller (EC) |
| `usbcctl iom` | What the SoC's USB-C mux is actually doing (IOM port status) |
| `usbcctl pd suspend\|resume <port>` | Make the EC detach / re-attach a port (simulated unplug) |
| `usbcctl pmc <hex bytes>` | Send a raw PMC USB-C mux message (developers only) |
| `usbcctl ec <cmd> <ver> <insize> [bytes]` | Send a raw EC host command (developers only) |

Reading `usbcctl muxinfo` clears a pending DisplayPort HPD IRQ in the EC, so avoid running it in a loop while a
display is connected.

When reporting a problem, describe what was plugged in where and what happened. If you can build usbcctl, also
include the output of `usbcctl log`, `usbcctl muxinfo` and `usbcctl iom`, captured right after the problem happens.

To stop the driver from changing the mux, for manual testing, set the DWORD value
`HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\INTC1026\<instance>\Device Parameters\PollIntervalMs` to `0` and restart.

## Building from source

Requirements:

- Visual Studio 2022 or Build Tools for Visual Studio 2022, with "Desktop development with C++" (MSVC x64 tools).
- Windows SDK and Windows Driver Kit (WDK) 10.0.26100 or newer. Tested with WDK 10.0.28000 and MSVC 14.44.

From PowerShell in the repository folder:

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1      # -> out\: TigerLakeUsbcMux.sys/.inf/.cat/.pdb, TigerLakeUsbcMuxTest.cer, usbcctl.exe
powershell -ExecutionPolicy Bypass -File .\package.ps1    # build + dist\TigerLakeUsbcMux-<version>-x64.zip and SHA256SUMS.txt
```

- `build.ps1` compiles with the command-line tools; no Visual Studio project is needed.
- On first use it creates a self-signed code-signing certificate, `CN=Open Source Driver`, in your user certificate store,
  and signs the driver and catalog with it. `build.ps1 -NoSign` skips signing.
- To install your own build, run `package.ps1` and use the zip it creates, or copy `TigerLakeUsbcMux.sys/.inf/.cat` and
  `TigerLakeUsbcMuxTest.cer` from `out\` together with the files in `install\` into one folder and run `install.cmd`.
- The release zip contains only the driver, its test certificate, the install/uninstall scripts and the license
  files. `usbcctl.exe` and the debug symbols (`TigerLakeUsbcMux.pdb`) stay in `out\`. Keep the `.pdb` of each published
  release if you want to analyse crash dumps of it later.

The version shown in Device Manager and used for the release name is `DriverVer` in `driver\TigerLakeUsbcMux.inf`.

## How it works

On Tiger Lake, the SoC side of each USB-C port (the Type-C subsystem mux) is controlled by the Power Management
Controller (PMC). The Chromebook's embedded controller (EC) negotiates everything on the cable side. The firmware programs
the mux once at boot. After that, the operating system has to tell the PMC about every change. On Windows nothing did,
so only devices present at power-on worked.

TigerLakeUsbcMux binds to the PMC (`ACPI\INTC1026`). Every 300 ms it asks the EC (through CoolStar's `crosecbus`) about each
port's state, and on a change sends PMC messages through the firmware's `\_SB.PCI0.PMC.IPCS` method:

- **USB:** CONNECT and DISCONNECT, with cable orientation and data role.
- **DisplayPort:** this EC enters DisplayPort mode only when the host asks. When a partner advertises DisplayPort, the
  driver requests the mode from the EC. It then sends the PMC SAFE, ALT_MODE (pin assignment) and HPD (hot plug)
  messages, so the Intel graphics driver sees the display.

Port numbers come from the firmware's `\_SB.PCI0.PMC.MUX.CONx._DSD`. More detail: [docs/TECHNICAL.md](docs/TECHNICAL.md).

## Known limitations

- Test-signed only (see above). Secure Boot must stay off.
- 4-lane DisplayPort (pin C/E) is untested. Adapters with a USB hub always use pin D.
- USB4 / Thunderbolt modes are not implemented. On Windows 11 this could later build on Windows' inbox USB4 driver.
- The driver polls the EC every 300 ms instead of reacting to EC events, because CoolStar's `crosectypec` handles
  those events. Plugging in therefore takes up to about a third of a second longer to notice.

## License

MIT, see [LICENSE](LICENSE). Credits and license texts for the interfaces and definitions this project is built to
match (CoolStar crosecbus, ChromiumOS EC) are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

This project is not affiliated with Google, Intel, Acer, CoolStar or MrChromebox. Trademarks belong to their owners;
see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md#trademarks).
