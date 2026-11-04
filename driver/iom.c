/* SPDX-License-Identifier: MIT */
/*
 * iom.c - read-only access to the Tiger Lake IOM port status registers.
 *
 * The IOM is a separate ACPI device (\_SB.IOM, INTC1072) whose _CRS on this
 * firmware is Memory32Fixed(0xFBC10000, 0x1600). Each TCSS port has a 32-bit
 * IOM_PORT_STATUS register at offset 0x560 + 4 * (port - 1).
 */
#include "driver.h"

NTSTATUS
IomMap(_In_ PDEVICE_CONTEXT Ctx)
{
    PHYSICAL_ADDRESS pa;

    pa.QuadPart = IOM_BASE_ADDRESS + IOM_PORT_STATUS_OFFSET;
    Ctx->IomPortStatus = MmMapIoSpaceEx(pa, IOM_PORT_COUNT * sizeof(ULONG),
                                        PAGE_READONLY | PAGE_NOCACHE);
    return Ctx->IomPortStatus != NULL ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

VOID
IomUnmap(_In_ PDEVICE_CONTEXT Ctx)
{
    if (Ctx->IomPortStatus != NULL) {
        MmUnmapIoSpace((PVOID)Ctx->IomPortStatus, IOM_PORT_COUNT * sizeof(ULONG));
        Ctx->IomPortStatus = NULL;
    }
}

/* Reads one port's status; FALSE if unmapped or the TCSS is powered down (all ones). */
BOOLEAN
IomReadPort(_In_ PDEVICE_CONTEXT Ctx, _In_ UINT8 Usb3Port, _Out_ UINT32 *Value)
{
    *Value = 0;
    if (Ctx->IomPortStatus == NULL || Usb3Port == 0 || Usb3Port > IOM_PORT_COUNT) {
        return FALSE;
    }
    *Value = READ_REGISTER_ULONG((volatile ULONG *)&Ctx->IomPortStatus[Usb3Port - 1]);
    return *Value != MAXULONG;
}

NTSTATUS
IomReadPortStatus(_In_ PDEVICE_CONTEXT Ctx, _Out_ PTGL_IOM_STATUS Status)
{
    ULONG i;

    RtlZeroMemory(Status, sizeof(*Status));
    if (Ctx->IomPortStatus == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    Status->PortCount = IOM_PORT_COUNT;
    for (i = 0; i < IOM_PORT_COUNT; i++) {
        Status->PortStatus[i] = READ_REGISTER_ULONG((volatile ULONG *)&Ctx->IomPortStatus[i]);
    }
    return STATUS_SUCCESS;
}
