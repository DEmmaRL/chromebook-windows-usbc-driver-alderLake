/* SPDX-License-Identifier: MIT */
/*
 * public.h - interface shared by the TigerLakeUsbcMux driver and user-mode tools.
 *
 * The IOCTLs are diagnostic: they let a tool send raw PMC USB-C mux messages,
 * raw ChromeOS EC host commands, and read the IOM port status registers.
 * The INF restricts the device object to SYSTEM and Administrators.
 */
#pragma once

/* {906165f4-9c1d-4078-ae13-b094c4b3e270} */
DEFINE_GUID(GUID_DEVINTERFACE_TGL_USBC_MUX,
    0x906165f4, 0x9c1d, 0x4078, 0xae, 0x13, 0xb0, 0x94, 0xc4, 0xb3, 0xe2, 0x70);

#define TGL_IOCTL(fn) \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800 + (fn), METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

/* In: TGL_PMC_REQUEST. Out: TGL_PMC_RESPONSE. */
#define IOCTL_TGL_PMC_USBC          TGL_IOCTL(0)
/* In: TGL_EC_REQUEST followed by OutSize bytes. Out: InSize bytes from the EC. */
#define IOCTL_TGL_EC_COMMAND        TGL_IOCTL(1)
/* Out: TGL_IOM_STATUS. */
#define IOCTL_TGL_IOM_STATUS        TGL_IOCTL(2)
/* Out: TGL_LOG. */
#define IOCTL_TGL_GET_LOG           TGL_IOCTL(3)

#define TGL_PMC_MAX_MESSAGE         16

#pragma pack(push, 1)

typedef struct _TGL_PMC_REQUEST {
    UINT8  Length;                          /* message length in bytes */
    UINT8  Message[TGL_PMC_MAX_MESSAGE];
} TGL_PMC_REQUEST, *PTGL_PMC_REQUEST;

typedef struct _TGL_PMC_RESPONSE {
    UINT32 IpcStatus;                       /* IPCS result: 0 ok, 1 error, 2 timeout */
    UINT32 ReadBuffer[4];                   /* PMC IPC read buffer (RBUF0..3) */
} TGL_PMC_RESPONSE, *PTGL_PMC_RESPONSE;

typedef struct _TGL_EC_REQUEST {
    UINT32 Command;
    UINT32 Version;
    UINT32 OutSize;                         /* bytes sent to the EC (follow this header) */
    UINT32 InSize;                          /* bytes expected back from the EC */
} TGL_EC_REQUEST, *PTGL_EC_REQUEST;

#define TGL_IOM_MAX_PORTS           4

typedef struct _TGL_IOM_STATUS {
    UINT32 PortCount;
    UINT32 PortStatus[TGL_IOM_MAX_PORTS]; /* IOM_PORT_STATUS for TCSS ports 1..N */
} TGL_IOM_STATUS, *PTGL_IOM_STATUS;

/* Activity log: what the automatic mux did, kept in a ring buffer in the driver. */
typedef enum _TGL_LOG_EVENT {
    TglLogStart = 1,            /* polling started (D0 entry); Detail = previous WDF power state */
    TglLogStop,                 /* polling stopped (D0 exit);  Detail = target WDF power state */
    TglLogAdopt,                /* found connected by firmware, kept as is */
    TglLogConnect,              /* partner attached: CONNECT sent */
    TglLogDisconnect,           /* partner detached: DISCONNECT sent */
    TglLogReconnect,            /* orientation/role changed: DISCONNECT sent before a new CONNECT */
    TglLogEcError,              /* EC unreachable (logged once until it recovers) */
    TglLogDpEnter,              /* EC entered DP alt mode: SAFE + ALT_MODE sent; pin in Flags */
    TglLogHpd,                  /* DP hot plug change: DP_HPD sent; level/IRQ in EcMuxFlags */
    TglLogDpRequest,            /* partner supports DP: EC asked to enter DP alt mode */
} TGL_LOG_EVENT;

#define TGL_LOG_FLAG_REVERSED       0x01
#define TGL_LOG_FLAG_UFP            0x02
#define TGL_LOG_FLAG_DP_PIN_SHIFT 4       /* DP pin assignment, 1 = A ... 6 = F (TglLogDpEnter) */

typedef struct _TGL_LOG_ENTRY {
    UINT32 Sequence;            /* increases by one per entry; 0 = unused slot */
    UINT8  Event;               /* TGL_LOG_EVENT */
    UINT8  Port;                /* EC port / MUX.CONx index */
    UINT8  EcMuxFlags;          /* EC_CMD_USB_PD_MUX_INFO flags at the time */
    UINT8  Flags;               /* TGL_LOG_FLAG_* */
    INT64  Time;                /* system time (UTC, 100 ns since 1601) */
    UINT32 IomStatus;           /* IOM_PORT_STATUS read before acting */
    INT32  Status;              /* NTSTATUS of the PMC command, or event detail */
} TGL_LOG_ENTRY, *PTGL_LOG_ENTRY;

#define TGL_LOG_ENTRIES             64

typedef struct _TGL_LOG {
    TGL_LOG_ENTRY Entries[TGL_LOG_ENTRIES];    /* ring buffer, unordered; sort by Sequence */
} TGL_LOG, *PTGL_LOG;

#pragma pack(pop)
