# Technical notes

Hardware and protocol details behind TigerLakeUsbcMux, gathered on the Google Volta, the Tiger Lake Chromebook it was
developed and tested on (MrChromebox UEFI firmware, EC firmware `voxel_v2.0.10247`). Values are from that machine. Other Volteer boards may differ, especially in port
numbers.

To inspect the ACPI tables of your own machine, dump and disassemble them with `acpidump` and `iasl -d` (ACPICA).

## Devices involved

| ACPI device | Path | Role | Linux driver | Windows |
|---|---|---|---|---|
| `GOOG0004` | `\_SB.CREC` | ChromeOS EC | `cros_ec_lpcs` | CoolStar `crosecbus` |
| `GOOG0014` | `\_SB.CREC.USBC` | EC Type-C ports (CON0/CON1) | `cros_ec_typec` | CoolStar `crosectypec` |
| `GOOG0003` | `\_SB.CREC.ECPD` | EC PD event notification | `cros-usbpd-notify-acpi` | CoolStar `crosectypec` |
| `INTC1026` | `\_SB.PCI0.PMC` | PMC IPC mailbox | `intel_scu` | **TigerLakeUsbcMux** |
| `INTC105C` | `\_SB.PCI0.PMC.MUX` | Type-C mux agent (port descriptions) | `intel_pmc_mux` | TigerLakeUsbcMux INF (no function driver) |
| `INTC1072` | `\_SB.IOM` | Tiger Lake IOM (port status registers) | read by `intel_pmc_mux` | TigerLakeUsbcMux INF (no function driver) |
| `INTC1079` | `\_SB.IOM` | Alder Lake / Alder Lake-N IOM (port status registers) | read by `intel_pmc_mux` | TigerLakeUsbcMux INF (no function driver) |

The firmware configures the Tiger Lake Type-C subsystem (TCSS) mux only at boot. After that, the OS must send PMC IPC
messages on every attach, detach and mode change.

## Port mapping (`\_SB.PCI0.PMC.MUX.CONx._DSD`)

| Connector | EC port | `usb3-port-number` (TCSS xHCI) | `usb2-port-number` (PCH xHCI) | `sbu-orientation` |
|---|---|---|---|---|
| CON0 | 0 | 1 | 9 | normal (fixed) |
| CON1 | 1 | 2 | 4 | normal (fixed) |

`hsl-orientation` is not given, so it follows the cable orientation.

## PMC IPC

- Mailbox registers at the PMC's memory resource (`0xFE000000`). The IPC status register at offset 4 has bit 0 = busy.
- Firmware method `\_SB.PCI0.PMC.IPCS(cmd, subcmd, size, wbuf0..3)` writes `cmd | subcmd << 12 | size << 16` and polls
  for about 1000 ms (1000 x `Sleep(1)`; about 1.8 s observed on Windows). It returns
  `Package { status (0 ok, 1 error, 2 timeout), rbuf0..3 }`.
- **IPCS does not check the busy bit before writing a command.** TigerLakeUsbcMux checks it itself and skips a poll while the
  PMC is busy.
- USB-C mux command: `cmd = 0xA7`, `subcmd = 0`.

### USB-C mux messages

Byte 0: low nibble = usage, high nibble = USB3 (TCSS) port number.

| Usage | Message | Response status byte |
|---|---|---|
| 0 CONNECT | `[0x?0, usb2 \| ufp << 4 \| hsl_ori << 5 \| sbu_ori << 6]` | rbuf0 byte 2 |
| 1 DISCONNECT | `[0x?1, usb2]` | rbuf0 byte 2 |
| 2 SAFE_MODE | `[0x?2]` | rbuf0 byte 1 |
| 3 ALT_MODE | `[0x?3, mode_type << 4, 0, 0, mode_data (u32 LE)]`. DP: `mode_type = 1`, `mode_data = ori << 1 \| ufp << 3 \| pin << 8` | rbuf0 byte 1 |
| 4 DP_HPD | `[0x?4, lvl << 4 \| irq << 5]` | rbuf0 byte 1 |

Status bit 0 = failed (retry, up to 3 attempts), bit 1 = fatal. The DP `pin` is `ffs(pin mask)`: A = 1 ... D = 4.

Examples (CON1, usb3 = 2, usb2 = 4):

| Event | Message |
|---|---|
| Attach, normal orientation, we are host | `20 04` |
| Attach, flipped cable | `20 24` |
| Detach | `21 04` |
| Enter DP, pin D, normal orientation | `22`, then `23 10 00 00 00 04 00 00`, then `24 10` |

### IOM port status

`IOM_PORT_STATUS` registers at `0xFBC10000 + offset + 4 * (usb3_port - 1)` (`_CRS` of `\_SB.IOM`):
- **Tiger Lake (`INTC1072`):** `offset = 0x560`
- **Alder Lake / Alder Lake-N (`INTC1079`):** `offset = 0x160`

The driver selects `0x160` automatically when `ACPI\INTC1079` or an Alder Lake / Raptor Lake CPU is detected (and supports an optional `Device Parameters\IomPortStatusOffset` DWORD override).

| Bits | Meaning |
|---|---|
| 31 | connected |
| 13:12 | DP HPD state (1 = asserted; reads 2 right after ALT_MODE or HPD low) |
| 10 | UFP |
| 9:6 | activity: 3 USB, 4 SAFE, 5 DP, 6 DP + USB (DP_MFD), 7 TBT, 0xC ALT_USB, 0xD ALT_TBT_USB |

Examples: `0x800004e9` charger attached (USB, UFP), `0x804011a9` hub in DP mode with USB 3, HPD high, `0x00000029` idle.

## ChromeOS EC

EC host commands used, through `crosecbus`:

| Command | Use |
|---|---|
| `0x101` `USB_PD_CONTROL` v2 | attach state (`enabled & 0x02`), data role (`role & 0x02` = we are DFP), DP pin mask (`dp_mode`) |
| `0x11A` `USB_PD_MUX_INFO` | mux flags: USB 0x01, DP 0x02, POLARITY_INVERTED 0x04, HPD_IRQ 0x08, HPD_LVL 0x10, SAFE 0x20, TBT 0x40, USB4 0x80 |
| `0x131` `TYPEC_DISCOVERY` | the partner's SVIDs (DisplayPort = `0xFF01`) |
| `0x132` `TYPEC_CONTROL` | `{port, 2 = ENTER_MODE, 0, mode 0 = DP}` |
| `0x119` `PD_CONTROL` | `{port, 0 = suspend / 1 = resume}`, used by `usbcctl pd` |

Notes:

- TigerLakeUsbcMux is not a child of the EC bus, so the bus can go away while it runs (disabled, updated, restarted). The
  driver keeps the bus open as a KMDF I/O target to receive its removal notifications, releases the interface on
  query-remove (waiting for a command in flight), and reconnects on the next poll once the bus is back. CoolStar's
  `crosecservice.exe` keeps the bus open too and may veto the removal; Windows then disables it at the next restart.
- `USB_PD_CONTROL` attach state is the reliable attach signal. During a PD suspend `MUX_INFO` can still report USB.
- **This EC requires the host to request alt mode entry** (feature `TYPEC_REQUIRE_AP_MODE_ENTRY`, bit 42 of
  `GET_FEATURES`). Without `TYPEC_CONTROL ENTER_MODE`, DisplayPort partners stay in USB mode. The same applies on
  Linux: kernels without host-driven alt mode entry in `cros_ec_typec` get no USB-C video.
- The EC also advertises `TYPEC_MUX_REQUIRE_AP_ACK` (bit 43). TigerLakeUsbcMux does not send `USB_PD_MUX_ACK`, and nothing has
  needed it so far.
- Reading `MUX_INFO` clears a pending HPD IRQ. Every reader of the flags (the driver, `usbcctl`, other drivers) competes for it.
- `ENTER_MODE` may fail with an EC error while the EC is already switching modes (flags show SAFE). The driver retries.
- When the Chromebook is the power source, `pd suspend` only pauses PD. The partner stays attached as plain Type-C, and
  `TYPEC_DISCOVERY` fails until `pd resume`.

## DisplayPort sequence in TigerLakeUsbcMux

1. The EC reports attach: send CONNECT (USB).
2. Discovery lists `0xFF01` and the EC is not in DP: send `TYPEC_CONTROL ENTER_MODE(DP)`. If discovery is not ready
   yet, check again every poll for 10 s, then every 3 s while the partner stays attached.
3. The EC reports DP: send SAFE (unless the IOM is already in SAFE), then ALT_MODE with the pin from `dp_mode`, then
   DP_HPD if the HPD level or an IRQ is set.
4. While in DP: forward HPD level changes and IRQs (`DP_HPD`). If an IRQ arrives together with a rising level, the
   level is sent first.
5. Detach: drop HPD if asserted, then DISCONNECT. In DP mode the DISCONNECT often outlasts the IPCS timeout and
   finishes in the background. The busy check keeps later commands from colliding with it.
6. A port in DP mode stays there until detach, even if the EC later reports USB only, as on Linux.

## Measured behaviour

| Scenario | Result |
|---|---|
| USB attach (polling at 300 ms) | mux connected about 1.0 s after the EC attach, USB devices enumerated by about 1.7 s |
| Detach | mux disconnected within about 0.7 s |
| DP hub with TV, attach | TV active about 3.9 s after attach (connect, request +0.8 s, alt mode +0.4 s, HPD +0.4 s, Windows mode set) |
| DP MST adapter, two monitors | about 14 HPD IRQs in the first 10 s; on a monitor replug the adapter drops HPD for about 2 s, then sends a burst of IRQs |
| S3, hibernate, restart | the firmware/EC restore the port. The driver adopts the existing state and forwards HPD |
