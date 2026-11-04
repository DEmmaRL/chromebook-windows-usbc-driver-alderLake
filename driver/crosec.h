/* SPDX-License-Identifier: MIT */
/*
 * crosec.h - client side of the ChromeOS EC bus interface published by
 * CoolStar's crosecbus driver (https://github.com/coolstar/crosecbus, Apache-2.0).
 *
 * These declarations are written to be binary compatible with that interface;
 * they are not copied from it. The bus creates \DosDevices\GOOG0004 and answers
 * IRP_MN_QUERY_INTERFACE for GUID_CROSEC_INTERFACE_STANDARD (version 1).
 * Attribution and the Apache-2.0 text are in THIRD_PARTY_NOTICES.md.
 */
#pragma once

#define CROSEC_BUS_DEVICE_NAME  L"\\DosDevices\\GOOG0004"

/* {d7062676-e3a4-11ec-a6c4-244bfe9946d0} */
DEFINE_GUID(GUID_CROSEC_INTERFACE_STANDARD,
    0xd7062676, 0xe3a4, 0x11ec, 0xa6, 0xc4, 0x24, 0x4b, 0xfe, 0x99, 0x46, 0xd0);

/*
 * One EC host command. Data is used for both directions: OutSize bytes are
 * sent and up to InSize bytes are returned in place, so it must hold
 * max(OutSize, InSize) bytes. Result is not filled in by the bus.
 */
typedef struct _CROSEC_COMMAND {
    UINT32 Version;
    UINT32 Command;
    UINT32 OutSize;
    UINT32 InSize;
    UINT32 Result;
    UINT8  Data[1];
} CROSEC_COMMAND, *PCROSEC_COMMAND;

/* Must be called at PASSIVE_LEVEL; returns STATUS_INTERNAL_ERROR on an EC error code. */
typedef NTSTATUS (*PCROSEC_CMD_XFER_STATUS)(_In_ PVOID Context, _Inout_ PCROSEC_COMMAND Msg);
typedef BOOLEAN  (*PCROSEC_CHECK_FEATURES)(_In_ PVOID Context, _In_ INT Feature);

typedef struct _CROSEC_INTERFACE_STANDARD {
    INTERFACE               InterfaceHeader;
    PCROSEC_CMD_XFER_STATUS CmdXferStatus;
    PCROSEC_CHECK_FEATURES  CheckFeatures;
} CROSEC_INTERFACE_STANDARD, *PCROSEC_INTERFACE_STANDARD;
