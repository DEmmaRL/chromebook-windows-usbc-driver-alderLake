/* SPDX-License-Identifier: MIT */
/*
 * ec.c - ChromeOS EC host commands through the crosecbus interface.
 *
 * The bus may start after us, so the interface is acquired on first use. We are not a child
 * of the bus, so the bus can be removed while we run (disabled, updated, restarted): the I/O
 * target stays open to get its removal notifications, and the interface is dropped before
 * the bus goes away. EcLock is held across each command so removal waits for it to finish.
 */
#include "driver.h"

static EVT_WDF_IO_TARGET_QUERY_REMOVE EcEvtTargetQueryRemove;
static EVT_WDF_IO_TARGET_REMOVE_CANCELED EcEvtTargetRemoveCanceled;
static EVT_WDF_IO_TARGET_REMOVE_COMPLETE EcEvtTargetRemoveComplete;

/* Caller holds EcLock. */
static VOID
EcDropInterface(_In_ PDEVICE_CONTEXT Ctx)
{
    if (Ctx->EcConnected) {
        if (Ctx->Ec.InterfaceHeader.InterfaceDereference != NULL) {
            Ctx->Ec.InterfaceHeader.InterfaceDereference(Ctx->Ec.InterfaceHeader.Context);
        }
        RtlZeroMemory(&Ctx->Ec, sizeof(Ctx->Ec));
        Ctx->EcConnected = FALSE;
    }
}

/* Caller holds EcLock. */
static VOID
EcDeleteTarget(_In_ PDEVICE_CONTEXT Ctx)
{
    EcDropInterface(Ctx);
    if (Ctx->EcTarget != NULL) {
        WdfObjectDelete(Ctx->EcTarget);
        Ctx->EcTarget = NULL;
    }
}

/* Caller holds EcLock. */
static NTSTATUS
EcAcquireInterface(_In_ PDEVICE_CONTEXT Ctx)
{
    DECLARE_CONST_UNICODE_STRING(busName, CROSEC_BUS_DEVICE_NAME);
    WDF_IO_TARGET_OPEN_PARAMS openParams;
    NTSTATUS status;

    /* A target left over from a cancelled removal: start again. */
    EcDeleteTarget(Ctx);

    status = WdfIoTargetCreate(Ctx->Device, WDF_NO_OBJECT_ATTRIBUTES, &Ctx->EcTarget);
    if (!NT_SUCCESS(status)) {
        Ctx->EcTarget = NULL;
        return status;
    }

    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&openParams, &busName, STANDARD_RIGHTS_ALL);
    openParams.EvtIoTargetQueryRemove = EcEvtTargetQueryRemove;
    openParams.EvtIoTargetRemoveCanceled = EcEvtTargetRemoveCanceled;
    openParams.EvtIoTargetRemoveComplete = EcEvtTargetRemoveComplete;
    status = WdfIoTargetOpen(Ctx->EcTarget, &openParams);
    if (NT_SUCCESS(status)) {
        status = WdfIoTargetQueryForInterface(Ctx->EcTarget, &GUID_CROSEC_INTERFACE_STANDARD,
                                              (PINTERFACE)&Ctx->Ec, sizeof(Ctx->Ec), 1, NULL);
        if (NT_SUCCESS(status)) {
            Ctx->EcConnected = TRUE;
            if (Ctx->Ec.CmdXferStatus == NULL) {
                status = STATUS_NOINTERFACE;
            }
        }
    }

    if (!NT_SUCCESS(status)) {
        VLOG("cannot reach crosecbus 0x%08x", status);
        EcDeleteTarget(Ctx);
        return status;
    }

    VLOG("connected to crosecbus");
    return STATUS_SUCCESS;
}

static NTSTATUS
EcEvtTargetQueryRemove(_In_ WDFIOTARGET Target)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(WdfIoTargetGetDevice(Target));

    WdfWaitLockAcquire(ctx->EcLock, NULL);
    EcDropInterface(ctx);
    ctx->EcRemovePending = TRUE;
    WdfIoTargetCloseForQueryRemove(Target);
    WdfWaitLockRelease(ctx->EcLock);
    VLOG("crosecbus removal pending, EC released");
    return STATUS_SUCCESS;
}

static VOID
EcEvtTargetRemoveCanceled(_In_ WDFIOTARGET Target)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(WdfIoTargetGetDevice(Target));

    /* The closed target is replaced on the next command. */
    WdfWaitLockAcquire(ctx->EcLock, NULL);
    ctx->EcRemovePending = FALSE;
    WdfWaitLockRelease(ctx->EcLock);
    VLOG("crosecbus removal cancelled");
}

static VOID
EcEvtTargetRemoveComplete(_In_ WDFIOTARGET Target)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(WdfIoTargetGetDevice(Target));

    WdfWaitLockAcquire(ctx->EcLock, NULL);
    if (ctx->EcTarget == Target) {
        EcDeleteTarget(ctx);
    }
    ctx->EcRemovePending = FALSE;
    WdfWaitLockRelease(ctx->EcLock);
    VLOG("crosecbus removed");
}

VOID
EcRelease(_In_ PDEVICE_CONTEXT Ctx)
{
    WdfWaitLockAcquire(Ctx->EcLock, NULL);
    EcDeleteTarget(Ctx);
    WdfWaitLockRelease(Ctx->EcLock);
}

NTSTATUS
EcCommand(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ UINT32 Command,
    _In_ UINT32 Version,
    _In_reads_bytes_opt_(OutSize) const VOID *Out,
    _In_ UINT32 OutSize,
    _Out_writes_bytes_opt_(InSize) VOID *In,
    _In_ UINT32 InSize)
{
    UINT32 dataSize = max(OutSize, InSize);
    PCROSEC_COMMAND msg;
    NTSTATUS status = STATUS_SUCCESS;

    PAGED_CODE();

    if (dataSize > 0x1000) {
        return STATUS_INVALID_PARAMETER;
    }

    msg = ExAllocatePool2(POOL_FLAG_NON_PAGED, FIELD_OFFSET(CROSEC_COMMAND, Data) + max(dataSize, 1),
                          TGL_POOL_TAG);
    if (msg == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    msg->Version = Version;
    msg->Command = Command;
    msg->OutSize = OutSize;
    msg->InSize = InSize;
    if (OutSize != 0) {
        RtlCopyMemory(msg->Data, Out, OutSize);
    }

    WdfWaitLockAcquire(Ctx->EcLock, NULL);
    if (Ctx->EcRemovePending) {
        status = STATUS_DEVICE_NOT_CONNECTED;
    } else if (!Ctx->EcConnected) {
        status = EcAcquireInterface(Ctx);
    }
    if (NT_SUCCESS(status)) {
        status = Ctx->Ec.CmdXferStatus(Ctx->Ec.InterfaceHeader.Context, msg);
    }
    WdfWaitLockRelease(Ctx->EcLock);

    if (NT_SUCCESS(status) && InSize != 0) {
        RtlCopyMemory(In, msg->Data, InSize);
    }

    ExFreePoolWithTag(msg, TGL_POOL_TAG);
    return status;
}
