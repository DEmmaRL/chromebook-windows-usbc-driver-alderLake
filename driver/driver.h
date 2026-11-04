/* SPDX-License-Identifier: MIT */
/*
 * driver.h - TigerLakeUsbcMux: USB-C mux driver for Tiger Lake Chromebooks.
 *
 * Binds to the PMC IPC device (ACPI\INTC1026). Sends USB-C mux messages to the
 * PMC through the firmware's \_SB.PCI0.PMC.IPCS method, and talks to the
 * ChromeOS EC through the crosecbus interface.
 */
#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <wdmguid.h>
#include <acpiioct.h>
#include <ntstrsafe.h>

#include "crosec.h"
#include "public.h"

#define TGL_POOL_TAG            'cUgT'

#define VLOG(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "TigerLakeUsbcMux: " fmt "\n", ##__VA_ARGS__)

/* PMC IPC command for USB-C mux messages. */
#define PMC_IPC_USBC_CMD        0xA7

/* Tiger Lake IOM (ACPI\INTC1072, \_SB.IOM) port status registers. */
#define IOM_BASE_ADDRESS        0xFBC10000ULL
#define IOM_PORT_STATUS_OFFSET  0x560
#define IOM_PORT_COUNT          4

#define MUX_MAX_PORTS           4
#define MUX_ORIENTATION_FOLLOW_CABLE (-1)

typedef enum _MUX_STATE {
    MuxStateUnknown,            /* not yet observed (may have been connected by firmware) */
    MuxStateDisconnected,
    MuxStateConnected,
} MUX_STATE;

typedef struct _MUX_PORT {
    UINT8       Usb2Port;       /* PCH xHCI USB2 port */
    UINT8       Usb3Port;       /* TCSS xHCI USB3 port, 1-based */
    INT8        SbuOrientation; /* fixed 0/1, or MUX_ORIENTATION_FOLLOW_CABLE */
    INT8        HslOrientation;
    MUX_STATE   State;          /* what we last applied */
    BOOLEAN     Reversed;
    BOOLEAN     Ufp;
    BOOLEAN     DpRequestDone;  /* DP entry asked of the EC (or not possible) since attach */
    ULONG       DiscoveryPolls; /* polls spent waiting for the partner's PD discovery */
} MUX_PORT, *PMUX_PORT;

typedef struct _DEVICE_CONTEXT {
    WDFDEVICE                   Device;

    WDFWAITLOCK                 PmcLock;        /* serialises PMC IPC */
    volatile ULONG             *PmcIpc;         /* mapped PMC IPC registers (busy status) */

    WDFWAITLOCK                 EcLock;         /* guards the EC target, interface and commands */
    WDFIOTARGET                 EcTarget;       /* crosecbus, open while connected (removal notices) */
    BOOLEAN                     EcConnected;
    BOOLEAN                     EcRemovePending; /* crosecbus is being removed; don't reconnect */
    CROSEC_INTERFACE_STANDARD   Ec;

    volatile ULONG             *IomPortStatus;  /* mapped IOM_PORT_STATUS array */

    MUX_PORT                    Ports[MUX_MAX_PORTS];
    ULONG                       PortCount;
    ULONG                       PollIntervalMs;
    WDFTIMER                    PollTimer;
    volatile LONG               PollActive;     /* timer re-arms itself while set */
    BOOLEAN                     EcErrorLogged;

    KSPIN_LOCK                  LogLock;        /* guards Log and LogSequence */
    ULONG                       LogSequence;
    TGL_LOG                     Log;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

/* pmc.c */
NTSTATUS PmcIpcCommand(_In_ PDEVICE_CONTEXT Ctx, _In_ UINT32 Command, _In_ UINT32 SubCommand,
                       _In_reads_bytes_(Length) const UINT8 *Message, _In_ UINT32 Length,
                       _Out_ UINT32 *IpcStatus, _Out_writes_(4) UINT32 *ReadBuffer);
NTSTATUS PmcMap(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFCMRESLIST ResourcesTranslated);
VOID     PmcUnmap(_In_ PDEVICE_CONTEXT Ctx);
BOOLEAN  PmcIsBusy(_In_ PDEVICE_CONTEXT Ctx);

/* acpi.c */
NTSTATUS AcpiEvaluateChild(_In_ PDEVICE_CONTEXT Ctx, _In_z_ PCSTR Path,
                           _Outptr_result_bytebuffer_(*OutLength) PACPI_EVAL_OUTPUT_BUFFER *Out,
                           _Out_ ULONG *OutLength);
const ACPI_METHOD_ARGUMENT *AcpiDsdFindProperty(_In_reads_bytes_(Length) PACPI_EVAL_OUTPUT_BUFFER Dsd,
                                                _In_ ULONG Length, _In_z_ PCSTR Name);

/* ec.c */
NTSTATUS EcCommand(_In_ PDEVICE_CONTEXT Ctx, _In_ UINT32 Command, _In_ UINT32 Version,
                   _In_reads_bytes_opt_(OutSize) const VOID *Out, _In_ UINT32 OutSize,
                   _Out_writes_bytes_opt_(InSize) VOID *In, _In_ UINT32 InSize);
VOID     EcRelease(_In_ PDEVICE_CONTEXT Ctx);

/* iom.c */
NTSTATUS IomMap(_In_ PDEVICE_CONTEXT Ctx);
VOID     IomUnmap(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS IomReadPortStatus(_In_ PDEVICE_CONTEXT Ctx, _Out_ PTGL_IOM_STATUS Status);
BOOLEAN  IomReadPort(_In_ PDEVICE_CONTEXT Ctx, _In_ UINT8 Usb3Port, _Out_ UINT32 *Value);

/* mux.c */
NTSTATUS MuxLoadPortConfig(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS MuxInitialize(_In_ PDEVICE_CONTEXT Ctx);
VOID     MuxStart(_In_ PDEVICE_CONTEXT Ctx, _In_ INT32 PreviousState);
VOID     MuxStop(_In_ PDEVICE_CONTEXT Ctx, _In_ INT32 TargetState);

/* log.c */
VOID     LogEvent(_In_ PDEVICE_CONTEXT Ctx, _In_ TGL_LOG_EVENT Event, _In_ UINT8 Port, _In_ UINT8 EcMuxFlags,
                  _In_ UINT8 Flags, _In_ UINT32 IomStatus, _In_ INT32 Status);
VOID     LogCopy(_In_ PDEVICE_CONTEXT Ctx, _Out_ PTGL_LOG Log);
