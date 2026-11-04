# Third-party notices

This project is licensed under the MIT License (see [LICENSE](LICENSE)). It contains no source code copied from other
projects, but parts of it are written to match interfaces and protocol definitions published by others. Those projects
are credited here, and their license texts are included in [licenses/](licenses/).

## CoolStar crosecbus: ChromeOS EC bus interface

- Project: <https://github.com/coolstar/crosecbus>
- Copyright 2022 CoolStar
- License: Apache License, Version 2.0, [licenses/Apache-2.0.txt](licenses/Apache-2.0.txt)
- Used in: [driver/crosec.h](driver/crosec.h)

`crosec.h` declares the device name, interface GUID and structure layouts needed to call the EC bus interface that
crosecbus publishes to other drivers. These declarations were written for this project to be binary compatible with
that interface. They are not a copy of crosecbus source.

crosecbus itself is not part of this project or its releases. It must be installed separately (see the README).

## ChromiumOS Embedded Controller: host command definitions

- Project: <https://chromium.googlesource.com/chromiumos/platform/ec>
- Copyright 2010 The ChromiumOS Authors
- License: BSD 3-Clause, [licenses/BSD-3-Clause-ChromiumOS-EC.txt](licenses/BSD-3-Clause-ChromiumOS-EC.txt)
- Used in: [driver/mux.c](driver/mux.c)

The EC host command numbers, flag values and request/response structure layouts the driver uses (for example
`EC_CMD_USB_PD_CONTROL`, `EC_CMD_USB_PD_MUX_INFO`, `EC_CMD_TYPEC_DISCOVERY` and `EC_CMD_TYPEC_CONTROL`) follow the
definitions in the ChromiumOS EC's `include/ec_commands.h`.

## Reference only (no code used)

- Linux kernel `drivers/platform/chrome/cros_ec_typec.c` and `drivers/usb/typec/mux/intel_pmc_mux.c` (GPL-2.0) were
  used to understand the order of the EC and PMC messages for USB-C and DisplayPort alt mode. No Linux code is part of
  this project.

## Build tools

- The driver is built with the Microsoft Windows Driver Kit (WDK). The compiled driver links the KMDF stub libraries
  (`WdfDriverEntry.lib`, `WdfLdr.lib`) and `BufferOverflowFastFailK.lib` from the WDK, as every KMDF driver does. Those
  parts are distributed only in compiled form, under the WDK license terms. No WDK files are included in this repository.

## Not included: runtime dependencies

- CoolStar crosecbus (Apache-2.0) and CoolStar crosectypec (closed source) are required or used at runtime on the
  target machine. Neither is distributed by this project.

## Trademarks

Chromebook and ChromeOS are trademarks of Google LLC. Intel, Tiger Lake and Thunderbolt are trademarks of Intel
Corporation. USB4 and USB Type-C are trademarks of the USB Implementers Forum. DisplayPort is a trademark of VESA. HDMI is
a trademark of HDMI Licensing Administrator, Inc. Acer is a trademark of Acer Inc. Windows is a trademark of Microsoft
Corporation. All other names are the property of their respective owners. This project is not affiliated with or
endorsed by any of them.
