/* SPDX-License-Identifier: MIT */
/*
 * mux.c - keep the Tiger Lake TCSS mux in step with the ChromeOS EC.
 *
 * The firmware configures the mux only at boot. Afterwards the OS has to send
 * PMC USB-C messages whenever a partner attaches, detaches, flips or swaps data
 * role. CoolStar's crosectypec owns the EC's PD notifications (GOOG0003), so this
 * driver polls the EC instead and applies what it reports:
 *
 *   EC_CMD_USB_PD_CONTROL   connected?  data role (DFP = we are host)  DP pin assignment
 *   EC_CMD_USB_PD_MUX_INFO  mux flags   cable polarity  DP / HPD state
 *
 * Port numbers and orientation overrides come from \_SB.PCI0.PMC.MUX.CONx._DSD.
 * EC port n is assumed to be MUX.CONn, which matches the firmware's GOOG0014
 * CONn "mode-switch" references on Volteer boards.
 *
 * USB is CONNECT / DISCONNECT. This EC (TYPEC_REQUIRE_AP_MODE_ENTRY) enters DP alt mode
 * only when the host asks, so once the partner's discovery lists the DP SVID we send
 * EC_CMD_TYPEC_CONTROL ENTER_MODE, as Linux's cros_ec_typec does. DP alt mode on the
 * PMC side follows Linux's intel_pmc_mux:
 * SAFE, then ALT_MODE (DP pin assignment), then DP_HPD whenever the sink's hot plug
 * state changes. The IOM port status tells which of these the mux has already done.
 * Thunderbolt and USB4 modes are not handled; such partners get plain USB.
 *
 * The EC command numbers, flags and structure layouts below follow the ChromiumOS EC's
 * ec_commands.h (BSD-3-Clause, see THIRD_PARTY_NOTICES.md). The Linux drivers named above
 * served as protocol reference only; no Linux code is used.
 */
#include "driver.h"

#define EC_CMD_USB_PD_CONTROL               0x0101
#define EC_CMD_USB_PD_MUX_INFO              0x011A
#define EC_CMD_TYPEC_DISCOVERY              0x0131
#define EC_CMD_TYPEC_CONTROL                0x0132

#define TYPEC_PARTNER_SOP                   0
#define TYPEC_CONTROL_COMMAND_ENTER_MODE    2
#define TYPEC_MODE_DP                       0
#define USB_SID_DISPLAYPORT                 0xFF01
#define DISCOVERY_MAX_SVIDS                 8
#define DISCOVERY_FAST_POLLS                33      /* every poll for about 10 s after attach ... */
#define DISCOVERY_SLOW_INTERVAL             10      /* ... then every 10th poll while still attached */

#define PD_CTRL_RESP_ENABLED_CONNECTED      0x02
#define PD_CTRL_RESP_ROLE_DATA              0x02    /* 1 = DFP */

#define USB_PD_MUX_USB_ENABLED              0x01
#define USB_PD_MUX_DP_ENABLED               0x02
#define USB_PD_MUX_POLARITY_INVERTED        0x04
#define USB_PD_MUX_HPD_IRQ                  0x08
#define USB_PD_MUX_HPD_LVL                  0x10
#define USB_PD_MUX_SAFE_MODE                0x20
#define USB_PD_MUX_TBT_COMPAT_ENABLED       0x40
#define USB_PD_MUX_USB4_ENABLED             0x80
#define USB_PD_MUX_ACTIVE                   (USB_PD_MUX_USB_ENABLED | USB_PD_MUX_DP_ENABLED | \
                                             USB_PD_MUX_SAFE_MODE | USB_PD_MUX_TBT_COMPAT_ENABLED | \
                                             USB_PD_MUX_USB4_ENABLED)

#pragma pack(push, 1)
typedef struct _EC_PARAMS_USB_PD_CONTROL {
    UINT8 Port;
    UINT8 Role;                 /* 0 = no change */
    UINT8 Mux;                  /* 0 = no change */
    UINT8 Swap;                 /* 0 = none */
} EC_PARAMS_USB_PD_CONTROL;

/* Version 1 is the first 35 bytes of this. */
typedef struct _EC_RESPONSE_USB_PD_CONTROL_V2 {
    UINT8 Enabled;
    UINT8 Role;
    UINT8 Polarity;
    CHAR  State[32];
    UINT8 CcState;
    UINT8 DpMode;               /* DP pin assignment bitmask, BIT(0) = A ... BIT(5) = F */
    UINT8 Reserved;
    UINT8 ControlFlags;
    UINT8 CableSpeed;
    UINT8 CableGen;
} EC_RESPONSE_USB_PD_CONTROL_V2;

typedef struct _EC_PARAMS_TYPEC_DISCOVERY {
    UINT8 Port;
    UINT8 PartnerType;
} EC_PARAMS_TYPEC_DISCOVERY;

typedef struct _EC_SVID_MODE_INFO {
    UINT16 Svid;
    UINT16 ModeCount;
    UINT32 ModeVdo[6];
} EC_SVID_MODE_INFO;

typedef struct _EC_RESPONSE_TYPEC_DISCOVERY {
    UINT8  IdentityCount;
    UINT8  SvidCount;
    UINT16 Reserved;
    UINT32 DiscoveryVdo[6];
    EC_SVID_MODE_INFO Svids[DISCOVERY_MAX_SVIDS];
} EC_RESPONSE_TYPEC_DISCOVERY;

/* Only the fields used here; the EC accepts the short form. */
typedef struct _EC_PARAMS_TYPEC_CONTROL {
    UINT8  Port;
    UINT8  Command;
    UINT16 Reserved;
    UINT8  ModeToEnter;
    UINT8  Padding[3];
} EC_PARAMS_TYPEC_CONTROL;
#pragma pack(pop)

/* PMC USB-C message usage (low nibble of byte 0; high nibble is the USB3 port). */
#define PMC_USB_CONNECT                     0
#define PMC_USB_DISCONNECT                  1
#define PMC_USB_SAFE_MODE                   2
#define PMC_USB_ALT_MODE                    3
#define PMC_USB_DP_HPD                      4

#define PMC_USB_MSG_UFP_SHIFT               4
#define PMC_USB_MSG_ORI_HSL_SHIFT           5
#define PMC_USB_MSG_ORI_AUX_SHIFT           6

#define PMC_USB_MODE_TYPE_DP                1
#define PMC_USB_MODE_TYPE_SHIFT             4
#define PMC_USB_ALTMODE_ORI_SHIFT           1
#define PMC_USB_ALTMODE_UFP_SHIFT           3
#define PMC_USB_ALTMODE_DP_MODE_SHIFT       8

#define PMC_USB_DP_HPD_LVL                  0x10
#define PMC_USB_DP_HPD_IRQ                  0x20

#define PMC_USB_RESP_FAILURE                0x01
#define PMC_USB_RESP_FATAL                  0x02
#define PMC_USB_ATTEMPTS                    3

#define IOM_PORT_STATUS_CONNECTED           0x80000000UL
#define IOM_PORT_STATUS_HPD                 0x00001000UL
#define IOM_PORT_ACTIVITY(_s)               (((_s) >> 6) & 0xF)
#define IOM_ACTIVITY_SAFE_MODE              0x4
#define IOM_ACTIVITY_DP                     0x5
#define IOM_ACTIVITY_DP_MFD                 0x6

#define MUX_DEFAULT_POLL_MS                 300

static INT8
OrientationProperty(_In_reads_bytes_(Length) PACPI_EVAL_OUTPUT_BUFFER Dsd, _In_ ULONG Length,
                    _In_z_ PCSTR Name)
{
    const ACPI_METHOD_ARGUMENT *value = AcpiDsdFindProperty(Dsd, Length, Name);

    if (value == NULL || value->Type != ACPI_METHOD_ARGUMENT_STRING) {
        return MUX_ORIENTATION_FOLLOW_CABLE;
    }
    if (value->DataLength >= sizeof("reverse") && strcmp((PCSTR)value->Data, "reverse") == 0) {
        return 1;
    }
    return 0;   /* "normal" */
}

static BOOLEAN
IntegerProperty(_In_reads_bytes_(Length) PACPI_EVAL_OUTPUT_BUFFER Dsd, _In_ ULONG Length,
                _In_z_ PCSTR Name, _Out_ ULONG *Value)
{
    const ACPI_METHOD_ARGUMENT *value = AcpiDsdFindProperty(Dsd, Length, Name);

    *Value = 0;
    if (value == NULL || value->Type != ACPI_METHOD_ARGUMENT_INTEGER || value->DataLength < sizeof(ULONG)) {
        return FALSE;
    }
    *Value = value->Argument;
    return TRUE;
}

NTSTATUS
MuxLoadPortConfig(_In_ PDEVICE_CONTEXT Ctx)
{
    CHAR path[] = "MUX_.CON0._DSD";
    PACPI_EVAL_OUTPUT_BUFFER dsd;
    ULONG length, usb2, usb3, i;
    PMUX_PORT port;
    NTSTATUS status;

    Ctx->PortCount = 0;
    for (i = 0; i < MUX_MAX_PORTS; i++) {
        path[8] = (CHAR)('0' + i);
        status = AcpiEvaluateChild(Ctx, path, &dsd, &length);
        if (!NT_SUCCESS(status)) {
            break;
        }

        port = &Ctx->Ports[i];
        RtlZeroMemory(port, sizeof(*port));
        if (!IntegerProperty(dsd, length, "usb2-port-number", &usb2) ||
            !IntegerProperty(dsd, length, "usb3-port-number", &usb3) ||
            usb2 == 0 || usb2 > 15 || usb3 == 0 || usb3 > IOM_PORT_COUNT) {
            VLOG("CON%u: missing or invalid port numbers", i);
            ExFreePoolWithTag(dsd, TGL_POOL_TAG);
            break;
        }
        port->Usb2Port = (UINT8)usb2;
        port->Usb3Port = (UINT8)usb3;
        port->SbuOrientation = OrientationProperty(dsd, length, "sbu-orientation");
        port->HslOrientation = OrientationProperty(dsd, length, "hsl-orientation");
        port->State = MuxStateUnknown;
        ExFreePoolWithTag(dsd, TGL_POOL_TAG);

        VLOG("CON%u: usb2 %u usb3 %u sbu %d hsl %d", i, port->Usb2Port, port->Usb3Port,
             port->SbuOrientation, port->HslOrientation);
        Ctx->PortCount = i + 1;
    }

    return Ctx->PortCount != 0 ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}

static NTSTATUS
MuxSendMessage(_In_ PDEVICE_CONTEXT Ctx, _In_reads_bytes_(Length) const UINT8 *Message, _In_ UINT32 Length)
{
    UINT32 ipcStatus, rbuf[4];
    UINT8 usage = Message[0] & 0xF;
    UINT8 result;
    NTSTATUS status;
    int attempt;

    for (attempt = 0; attempt < PMC_USB_ATTEMPTS; attempt++) {
        status = PmcIpcCommand(Ctx, PMC_IPC_USBC_CMD, 0, Message, Length, &ipcStatus, rbuf);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        if (ipcStatus == 2) {
            /* Still running (e.g. DP disconnect); the IOM status shows when it is done. */
            VLOG("PMC IPC timeout for message %02x", Message[0]);
            return STATUS_IO_TIMEOUT;
        }
        if (ipcStatus != 0) {
            VLOG("PMC IPC error %u for message %02x", ipcStatus, Message[0]);
            return STATUS_IO_DEVICE_ERROR;
        }

        /* Connect and disconnect report their result in byte 2, other messages in byte 1. */
        result = (UINT8)(rbuf[0] >> (usage <= PMC_USB_DISCONNECT ? 16 : 8));
        if (!(result & PMC_USB_RESP_FAILURE)) {
            return STATUS_SUCCESS;
        }
        if (result & PMC_USB_RESP_FATAL) {
            VLOG("PMC rejected message %02x (fatal)", Message[0]);
            return STATUS_IO_DEVICE_ERROR;
        }
    }
    VLOG("PMC busy for message %02x", Message[0]);
    return STATUS_DEVICE_BUSY;
}

static NTSTATUS
MuxConnect(_In_ PDEVICE_CONTEXT Ctx, _In_ PMUX_PORT Port, _In_ BOOLEAN Reversed, _In_ BOOLEAN Ufp)
{
    UINT8 hsl = Port->HslOrientation == MUX_ORIENTATION_FOLLOW_CABLE ? Reversed : (UINT8)Port->HslOrientation;
    UINT8 sbu = Port->SbuOrientation == MUX_ORIENTATION_FOLLOW_CABLE ? Reversed : (UINT8)Port->SbuOrientation;
    UINT8 msg[2];

    msg[0] = (UINT8)(PMC_USB_CONNECT | Port->Usb3Port << 4);
    msg[1] = (UINT8)(Port->Usb2Port | Ufp << PMC_USB_MSG_UFP_SHIFT | hsl << PMC_USB_MSG_ORI_HSL_SHIFT |
                     sbu << PMC_USB_MSG_ORI_AUX_SHIFT);
    return MuxSendMessage(Ctx, msg, sizeof(msg));
}

static NTSTATUS
MuxDpHpd(_In_ PDEVICE_CONTEXT Ctx, _In_ PMUX_PORT Port, _In_ BOOLEAN Level, _In_ BOOLEAN Irq)
{
    UINT8 msg[2];

    msg[0] = (UINT8)(PMC_USB_DP_HPD | Port->Usb3Port << 4);
    msg[1] = (UINT8)((Level ? PMC_USB_DP_HPD_LVL : 0) | (Irq ? PMC_USB_DP_HPD_IRQ : 0));
    return MuxSendMessage(Ctx, msg, sizeof(msg));
}

static NTSTATUS
MuxDisconnect(_In_ PDEVICE_CONTEXT Ctx, _In_ PMUX_PORT Port, _In_ UINT32 Iom)
{
    UINT8 msg[2];

    /* Like Linux, drop a still asserted HPD before leaving DP mode. */
    if (Iom & IOM_PORT_STATUS_HPD) {
        (VOID)MuxDpHpd(Ctx, Port, FALSE, FALSE);
    }

    msg[0] = (UINT8)(PMC_USB_DISCONNECT | Port->Usb3Port << 4);
    msg[1] = Port->Usb2Port;
    return MuxSendMessage(Ctx, msg, sizeof(msg));
}

static NTSTATUS
MuxSafeMode(_In_ PDEVICE_CONTEXT Ctx, _In_ PMUX_PORT Port)
{
    UINT8 msg = (UINT8)(PMC_USB_SAFE_MODE | Port->Usb3Port << 4);

    return MuxSendMessage(Ctx, &msg, sizeof(msg));
}

static NTSTATUS
MuxDpAltMode(_In_ PDEVICE_CONTEXT Ctx, _In_ PMUX_PORT Port, _In_ BOOLEAN Reversed, _In_ BOOLEAN Ufp,
             _In_ UINT8 Pin)
{
    UINT8 msg[8] = { 0 };
    UINT32 modeData;

    modeData = (UINT32)Reversed << PMC_USB_ALTMODE_ORI_SHIFT | (UINT32)Ufp << PMC_USB_ALTMODE_UFP_SHIFT |
               (UINT32)Pin << PMC_USB_ALTMODE_DP_MODE_SHIFT;
    msg[0] = (UINT8)(PMC_USB_ALT_MODE | Port->Usb3Port << 4);
    msg[1] = PMC_USB_MODE_TYPE_DP << PMC_USB_MODE_TYPE_SHIFT;
    RtlCopyMemory(&msg[4], &modeData, sizeof(modeData));
    return MuxSendMessage(Ctx, msg, sizeof(msg));
}

/* 1-based index of the lowest set bit (A = 1 ... F = 6), 0 if none: what the PMC calls the DP mode. */
static UINT8
DpPinAssignment(_In_ UINT8 DpMode)
{
    UINT8 pin;

    for (pin = 1; pin <= 8; pin++) {
        if (DpMode & (1 << (pin - 1))) {
            return pin;
        }
    }
    return 0;
}

/*
 * Bring a connected port into DP alt mode and keep its HPD in step with the EC.
 * Iom is the port status before this poll's messages.
 */
static VOID
MuxUpdateDp(_In_ PDEVICE_CONTEXT Ctx, _In_ UINT8 Index, _In_ UINT8 Flags, _In_ UINT8 LogFlags,
            _In_ UINT32 Iom, _In_ BOOLEAN Reversed, _In_ BOOLEAN Ufp, _In_ UINT8 Pin)
{
    PMUX_PORT port = &Ctx->Ports[Index];
    UINT32 activity = IOM_PORT_ACTIVITY(Iom);
    BOOLEAN level = (Flags & USB_PD_MUX_HPD_LVL) != 0;
    BOOLEAN irq = (Flags & USB_PD_MUX_HPD_IRQ) != 0;
    BOOLEAN asserted = (Iom & IOM_PORT_STATUS_HPD) != 0;
    NTSTATUS status;

    if (activity != IOM_ACTIVITY_DP && activity != IOM_ACTIVITY_DP_MFD) {
        status = STATUS_SUCCESS;
        if (activity != IOM_ACTIVITY_SAFE_MODE) {
            status = MuxSafeMode(Ctx, port);
        }
        if (NT_SUCCESS(status)) {
            status = MuxDpAltMode(Ctx, port, Reversed, Ufp, Pin);
        }
        VLOG("CON%u: DP pin %u -> alt mode 0x%08x", Index, Pin, status);
        LogEvent(Ctx, TglLogDpEnter, Index, Flags, (UINT8)(LogFlags | Pin << TGL_LOG_FLAG_DP_PIN_SHIFT),
                 Iom, status);
        if (!NT_SUCCESS(status)) {
            return;
        }
        asserted = FALSE;
    } else if (level == asserted && !irq) {
        return;
    }

    if (!level && !asserted && !irq) {
        return;
    }
    /* An IRQ that arrives together with the rising edge needs the level set first. */
    if (irq && level && !asserted) {
        (VOID)MuxDpHpd(Ctx, port, TRUE, FALSE);
    }
    status = MuxDpHpd(Ctx, port, level, irq);
    VLOG("CON%u: HPD level %u irq %u -> 0x%08x", Index, level, irq, status);
    LogEvent(Ctx, TglLogHpd, Index, Flags, LogFlags, Iom, status);
}

/*
 * Ask the EC to enter DP alt mode once the partner's PD discovery shows the DP SVID.
 * Called while we are host on a connected port that is not in DP yet. Discovery can
 * finish late (PD may start after the Type-C attach, e.g. after `pd resume`), and plain
 * USB devices never have any, so after DISCOVERY_FAST_POLLS it is only rechecked every
 * DISCOVERY_SLOW_INTERVAL polls. Done for good once requested, or once discovery lists
 * SVIDs without DP.
 */
static VOID
MuxRequestDp(_In_ PDEVICE_CONTEXT Ctx, _In_ UINT8 Index, _In_ UINT8 Flags, _In_ UINT8 LogFlags, _In_ UINT32 Iom)
{
    PMUX_PORT port = &Ctx->Ports[Index];
    EC_PARAMS_TYPEC_DISCOVERY discParams = { Index, TYPEC_PARTNER_SOP };
    EC_PARAMS_TYPEC_CONTROL control = { 0 };
    EC_RESPONSE_TYPEC_DISCOVERY *disc;
    BOOLEAN hasDp = FALSE;
    UINT8 i;
    NTSTATUS status;

    if (port->DpRequestDone) {
        return;
    }
    if (++port->DiscoveryPolls > DISCOVERY_FAST_POLLS &&
        (port->DiscoveryPolls - DISCOVERY_FAST_POLLS) % DISCOVERY_SLOW_INTERVAL != 0) {
        return;
    }

    disc = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*disc), TGL_POOL_TAG);
    if (disc == NULL) {
        return;
    }
    /* An error (e.g. PD not running yet) or an empty list means: not known yet. */
    status = EcCommand(Ctx, EC_CMD_TYPEC_DISCOVERY, 0, &discParams, sizeof(discParams), disc, sizeof(*disc));
    if (NT_SUCCESS(status) && disc->SvidCount != 0) {
        for (i = 0; i < disc->SvidCount && i < DISCOVERY_MAX_SVIDS; i++) {
            hasDp |= disc->Svids[i].Svid == USB_SID_DISPLAYPORT;
        }
        port->DpRequestDone = TRUE;
    }
    ExFreePoolWithTag(disc, TGL_POOL_TAG);
    if (!hasDp) {
        return;
    }

    control.Port = Index;
    control.Command = TYPEC_CONTROL_COMMAND_ENTER_MODE;
    control.ModeToEnter = TYPEC_MODE_DP;
    status = EcCommand(Ctx, EC_CMD_TYPEC_CONTROL, 0, &control, sizeof(control), NULL, 0);
    VLOG("CON%u: partner supports DP -> enter mode 0x%08x", Index, status);
    LogEvent(Ctx, TglLogDpRequest, Index, Flags, LogFlags, Iom, status);
    if (!NT_SUCCESS(status)) {
        /* Often only busy (another request in flight); ask again at the slow rate unless DP shows up. */
        port->DpRequestDone = FALSE;
        port->DiscoveryPolls = max(port->DiscoveryPolls, DISCOVERY_FAST_POLLS);
    }
}

static VOID
MuxUpdatePort(_In_ PDEVICE_CONTEXT Ctx, _In_ UINT8 Index)
{
    PMUX_PORT port = &Ctx->Ports[Index];
    EC_PARAMS_USB_PD_CONTROL pdParams = { Index, 0, 0, 0 };
    EC_RESPONSE_USB_PD_CONTROL_V2 pd;
    UINT8 flags = 0, logFlags, pin;
    UINT32 iom = 0;
    BOOLEAN want, reversed, ufp, connected, iomValid;
    NTSTATUS status;

    /*
     * The PMC is still finishing an earlier command; decide again on the next poll. Checked
     * before reading the EC, because reading the mux flags clears a pending HPD IRQ.
     */
    if (PmcIsBusy(Ctx)) {
        return;
    }

    RtlZeroMemory(&pd, sizeof(pd));
    status = EcCommand(Ctx, EC_CMD_USB_PD_CONTROL, 2, &pdParams, sizeof(pdParams), &pd, sizeof(pd));
    if (!NT_SUCCESS(status)) {
        /* Older EC: no DP pin assignment, so USB only. */
        status = EcCommand(Ctx, EC_CMD_USB_PD_CONTROL, 1, &pdParams, sizeof(pdParams), &pd,
                           FIELD_OFFSET(EC_RESPONSE_USB_PD_CONTROL_V2, CcState));
        pd.DpMode = 0;
    }
    if (NT_SUCCESS(status)) {
        status = EcCommand(Ctx, EC_CMD_USB_PD_MUX_INFO, 0, &Index, sizeof(Index), &flags, sizeof(flags));
    }
    if (!NT_SUCCESS(status)) {
        /* EC not reachable yet; try again next poll. */
        if (!Ctx->EcErrorLogged) {
            Ctx->EcErrorLogged = TRUE;
            LogEvent(Ctx, TglLogEcError, Index, 0, 0, 0, status);
        }
        return;
    }
    Ctx->EcErrorLogged = FALSE;

    want = (pd.Enabled & PD_CTRL_RESP_ENABLED_CONNECTED) && (flags & USB_PD_MUX_ACTIVE);
    reversed = (flags & USB_PD_MUX_POLARITY_INVERTED) != 0;
    ufp = !(pd.Role & PD_CTRL_RESP_ROLE_DATA);
    logFlags = (UINT8)((reversed ? TGL_LOG_FLAG_REVERSED : 0) | (ufp ? TGL_LOG_FLAG_UFP : 0));

    /* The IOM is the source of truth for what the mux is doing; fall back to our own record. */
    iomValid = IomReadPort(Ctx, port->Usb3Port, &iom);
    if (iomValid) {
        connected = (iom & IOM_PORT_STATUS_CONNECTED) != 0;
    } else {
        iom = 0;
        connected = port->State == MuxStateConnected;
    }

    /* DP needs the IOM status to know which alt mode steps are done. */
    pin = (flags & USB_PD_MUX_DP_ENABLED) && iomValid ? DpPinAssignment(pd.DpMode) : 0;

    if (!want) {
        if (connected) {
            status = MuxDisconnect(Ctx, port, iom);
            VLOG("CON%u: detached -> disconnect 0x%08x", Index, status);
            LogEvent(Ctx, TglLogDisconnect, Index, flags, logFlags, iom, status);
        }
        port->State = MuxStateDisconnected;
        port->DpRequestDone = FALSE;
        port->DiscoveryPolls = 0;
        return;
    }

    if (connected) {
        if (port->State == MuxStateUnknown) {
            /* Connected by the firmware at boot: adopt it rather than re-enumerating the device. */
            VLOG("CON%u: already connected (reversed %u ufp %u)", Index, reversed, ufp);
            LogEvent(Ctx, TglLogAdopt, Index, flags, logFlags, iom, STATUS_SUCCESS);
            port->State = MuxStateConnected;
            port->Reversed = reversed;
            port->Ufp = ufp;
        }
        if (port->State == MuxStateConnected && port->Reversed == reversed && port->Ufp == ufp) {
            /*
             * Like Linux, a port in DP mode stays there when the EC drops back to plain USB;
             * it is reset on detach. Only DP entry and HPD changes need messages.
             */
            if (pin != 0) {
                MuxUpdateDp(Ctx, Index, flags, logFlags, iom, reversed, ufp, pin);
            } else if (!(flags & USB_PD_MUX_DP_ENABLED) && !ufp) {
                MuxRequestDp(Ctx, Index, flags, logFlags, iom);
            }
            return;
        }
        status = MuxDisconnect(Ctx, port, iom);
        VLOG("CON%u: orientation/role changed -> disconnect 0x%08x", Index, status);
        LogEvent(Ctx, TglLogReconnect, Index, flags, logFlags, iom, status);
        iom = 0;
    }

    status = MuxConnect(Ctx, port, reversed, ufp);
    VLOG("CON%u: attached (flags 0x%02x reversed %u ufp %u) -> connect 0x%08x", Index, flags, reversed, ufp, status);
    LogEvent(Ctx, TglLogConnect, Index, flags, logFlags, iom, status);
    if (NT_SUCCESS(status)) {
        port->State = MuxStateConnected;
        port->Reversed = reversed;
        port->Ufp = ufp;
        /* A fresh connect is plain USB; DP follows on top of it, as in the manual test. */
        if (pin != 0) {
            MuxUpdateDp(Ctx, Index, flags, logFlags, IOM_PORT_STATUS_CONNECTED, reversed, ufp, pin);
        } else if (!(flags & USB_PD_MUX_DP_ENABLED) && !ufp) {
            MuxRequestDp(Ctx, Index, flags, logFlags, IOM_PORT_STATUS_CONNECTED);
        }
    } else {
        port->State = MuxStateDisconnected;
    }
}

static VOID
MuxEvtTimer(_In_ WDFTIMER Timer)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(WdfTimerGetParentObject(Timer));
    UINT8 i;

    for (i = 0; i < ctx->PortCount; i++) {
        MuxUpdatePort(ctx, i);
    }

    /* Passive-level KMDF timers cannot be periodic, so re-arm while polling is active. */
    if (InterlockedCompareExchange(&ctx->PollActive, 0, 0)) {
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(ctx->PollIntervalMs));
    }
}

NTSTATUS
MuxInitialize(_In_ PDEVICE_CONTEXT Ctx)
{
    WDF_TIMER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFKEY key;
    DECLARE_CONST_UNICODE_STRING(valueName, L"PollIntervalMs");
    ULONG interval = MUX_DEFAULT_POLL_MS;

    /* Device Parameters\PollIntervalMs: 0 disables the automatic mux (manual testing with usbcctl). */
    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(Ctx->Device, PLUGPLAY_REGKEY_DEVICE, KEY_READ,
                                            WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        (VOID)WdfRegistryQueryULong(key, &valueName, &interval);
        WdfRegistryClose(key);
    }
    Ctx->PollIntervalMs = interval;
    if (interval == 0) {
        VLOG("automatic mux disabled");
        return STATUS_SUCCESS;
    }

    WDF_TIMER_CONFIG_INIT(&config, MuxEvtTimer);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Ctx->Device;
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    return WdfTimerCreate(&config, &attributes, &Ctx->PollTimer);
}

VOID
MuxStart(_In_ PDEVICE_CONTEXT Ctx, _In_ INT32 PreviousState)
{
    UINT8 i;

    LogEvent(Ctx, TglLogStart, 0, 0, 0, 0, PreviousState);

    /* The EC may have left DP mode while we were away (sleep, hibernate). */
    for (i = 0; i < Ctx->PortCount; i++) {
        Ctx->Ports[i].DpRequestDone = FALSE;
        Ctx->Ports[i].DiscoveryPolls = 0;
    }
    if (Ctx->PollTimer != NULL && Ctx->PortCount != 0) {
        InterlockedExchange(&Ctx->PollActive, 1);
        WdfTimerStart(Ctx->PollTimer, WDF_REL_TIMEOUT_IN_MS(Ctx->PollIntervalMs));
    }
}

VOID
MuxStop(_In_ PDEVICE_CONTEXT Ctx, _In_ INT32 TargetState)
{
    LogEvent(Ctx, TglLogStop, 0, 0, 0, 0, TargetState);
    if (Ctx->PollTimer != NULL) {
        InterlockedExchange(&Ctx->PollActive, 0);
        /* The first stop waits for a running callback, which may have re-armed; the second cancels that. */
        WdfTimerStop(Ctx->PollTimer, TRUE);
        WdfTimerStop(Ctx->PollTimer, TRUE);
    }
}
