/* SPDX-License-Identifier: MIT */
/*
 * pmc.c - PMC IPC through the firmware method \_SB.PCI0.PMC.IPCS.
 *
 * IPCS(Cmd, SubCmd, Size, Wbuf0, Wbuf1, Wbuf2, Wbuf3) writes the IPC write
 * buffer, issues Cmd | SubCmd << 12 | Size << 16 and polls for completion. It
 * returns Package { Status, Rbuf0, Rbuf1, Rbuf2, Rbuf3 }, where Status is
 * 0 on success, 1 on an IPC error and 2 on timeout.
 *
 * IPCS writes the command without checking that the previous one has finished,
 * and gives up after about a second. A DISCONNECT of a port in DP alt mode can
 * take longer than that, so the IPC status register (the PMC's own memory
 * resource, offset 4, bit 0 = busy) is checked before every command.
 */
#include "driver.h"

#define IPCS_ARG_COUNT      7
#define IPCS_RESULT_COUNT   5

/* Method names are passed as a little-endian ULONG: 'SCPI' is "IPCS". */
#define IPCS_METHOD_NAME    ((ULONG)'SCPI')

#define PMC_IPC_STS_OFFSET  4
#define PMC_IPC_STS_BUSY    0x1
#define PMC_IPC_MAP_SIZE    8

NTSTATUS
PmcMap(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFCMRESLIST ResourcesTranslated)
{
    PCM_PARTIAL_RESOURCE_DESCRIPTOR desc;
    ULONG i;

    for (i = 0; i < WdfCmResourceListGetCount(ResourcesTranslated); i++) {
        desc = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (desc != NULL && desc->Type == CmResourceTypeMemory && desc->u.Memory.Length >= PMC_IPC_MAP_SIZE) {
            Ctx->PmcIpc = MmMapIoSpaceEx(desc->u.Memory.Start, PMC_IPC_MAP_SIZE, PAGE_READONLY | PAGE_NOCACHE);
            return Ctx->PmcIpc != NULL ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    return STATUS_RESOURCE_TYPE_NOT_FOUND;
}

VOID
PmcUnmap(_In_ PDEVICE_CONTEXT Ctx)
{
    if (Ctx->PmcIpc != NULL) {
        MmUnmapIoSpace((PVOID)Ctx->PmcIpc, PMC_IPC_MAP_SIZE);
        Ctx->PmcIpc = NULL;
    }
}

/* TRUE while the PMC is still working on an IPC command; FALSE if unknown. */
BOOLEAN
PmcIsBusy(_In_ PDEVICE_CONTEXT Ctx)
{
    if (Ctx->PmcIpc == NULL) {
        return FALSE;
    }
    return (READ_REGISTER_ULONG((volatile ULONG *)&Ctx->PmcIpc[PMC_IPC_STS_OFFSET / sizeof(ULONG)]) &
            PMC_IPC_STS_BUSY) != 0;
}

NTSTATUS
PmcIpcCommand(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ UINT32 Command,
    _In_ UINT32 SubCommand,
    _In_reads_bytes_(Length) const UINT8 *Message,
    _In_ UINT32 Length,
    _Out_ UINT32 *IpcStatus,
    _Out_writes_(4) UINT32 *ReadBuffer)
{
    UINT32 wbuf[4] = { 0 };
    UCHAR inBytes[FIELD_OFFSET(ACPI_EVAL_INPUT_BUFFER_COMPLEX, Argument) +
                  IPCS_ARG_COUNT * ACPI_METHOD_ARGUMENT_LENGTH(sizeof(ULONG))];
    UCHAR outBytes[FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument) +
                   IPCS_RESULT_COUNT * ACPI_METHOD_ARGUMENT_LENGTH(sizeof(ULONGLONG)) + 64];
    PACPI_EVAL_INPUT_BUFFER_COMPLEX in = (PACPI_EVAL_INPUT_BUFFER_COMPLEX)inBytes;
    PACPI_EVAL_OUTPUT_BUFFER out = (PACPI_EVAL_OUTPUT_BUFFER)outBytes;
    PACPI_METHOD_ARGUMENT arg;
    WDF_MEMORY_DESCRIPTOR inDesc, outDesc;
    ULONG_PTR returned = 0;
    UINT32 values[IPCS_RESULT_COUNT] = { 0 };
    ULONG i;
    NTSTATUS status;

    *IpcStatus = MAXUINT32;
    RtlZeroMemory(ReadBuffer, 4 * sizeof(UINT32));

    if (Length > sizeof(wbuf)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Length != 0) {
        RtlCopyMemory(wbuf, Message, Length);
    }

    RtlZeroMemory(inBytes, sizeof(inBytes));
    in->Signature = ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE;
    in->MethodNameAsUlong = IPCS_METHOD_NAME;
    in->Size = IPCS_ARG_COUNT * ACPI_METHOD_ARGUMENT_LENGTH(sizeof(ULONG));
    in->ArgumentCount = IPCS_ARG_COUNT;

    arg = in->Argument;
    ACPI_METHOD_SET_ARGUMENT_INTEGER(arg, Command);
    arg = ACPI_METHOD_NEXT_ARGUMENT(arg);
    ACPI_METHOD_SET_ARGUMENT_INTEGER(arg, SubCommand);
    arg = ACPI_METHOD_NEXT_ARGUMENT(arg);
    ACPI_METHOD_SET_ARGUMENT_INTEGER(arg, Length);
    for (i = 0; i < 4; i++) {
        arg = ACPI_METHOD_NEXT_ARGUMENT(arg);
        ACPI_METHOD_SET_ARGUMENT_INTEGER(arg, wbuf[i]);
    }

    RtlZeroMemory(outBytes, sizeof(outBytes));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inDesc, inBytes, sizeof(inBytes));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outDesc, outBytes, sizeof(outBytes));

    WdfWaitLockAcquire(Ctx->PmcLock, NULL);
    if (PmcIsBusy(Ctx)) {
        status = STATUS_DEVICE_BUSY;
    } else {
        status = WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(Ctx->Device), NULL,
                                                   IOCTL_ACPI_EVAL_METHOD, &inDesc, &outDesc,
                                                   NULL, &returned);
    }
    WdfWaitLockRelease(Ctx->PmcLock);

    if (!NT_SUCCESS(status)) {
        VLOG("IPCS evaluation failed 0x%08x", status);
        return status;
    }
    if (returned < FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument) ||
        out->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE ||
        out->Count < IPCS_RESULT_COUNT) {
        VLOG("IPCS returned an unexpected buffer (%Iu bytes, count %u)", returned, out->Count);
        return STATUS_ACPI_INVALID_DATA;
    }

    arg = out->Argument;
    for (i = 0; i < IPCS_RESULT_COUNT; i++) {
        if ((PUCHAR)arg + FIELD_OFFSET(ACPI_METHOD_ARGUMENT, Data) > outBytes + returned ||
            arg->Type != ACPI_METHOD_ARGUMENT_INTEGER || arg->DataLength < sizeof(ULONG)) {
            return STATUS_ACPI_INVALID_DATA;
        }
        values[i] = arg->Argument;
        arg = ACPI_METHOD_NEXT_ARGUMENT(arg);
    }

    *IpcStatus = values[0];
    RtlCopyMemory(ReadBuffer, &values[1], 4 * sizeof(UINT32));
    return STATUS_SUCCESS;
}
