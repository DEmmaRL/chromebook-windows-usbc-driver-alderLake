/* SPDX-License-Identifier: MIT */
/*
 * driver.c - driver entry, device setup and the diagnostic IOCTL interface.
 */
#include "driver.h"

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD TglEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE TglEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE TglEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY TglEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT TglEvtD0Exit;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL TglEvtIoDeviceControl;

NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;

    WDF_DRIVER_CONFIG_INIT(&config, TglEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

NTSTATUS
TglEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    PDEVICE_CONTEXT ctx;
    WDFDEVICE device;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = TglEvtPrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware = TglEvtReleaseHardware;
    pnpCallbacks.EvtDeviceD0Entry = TglEvtD0Entry;
    pnpCallbacks.EvtDeviceD0Exit = TglEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    ctx = GetDeviceContext(device);
    ctx->Device = device;
    KeInitializeSpinLock(&ctx->LogLock);

    status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &ctx->PmcLock);
    if (NT_SUCCESS(status)) {
        status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &ctx->EcLock);
    }
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = TglEvtIoDeviceControl;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_TGL_USBC_MUX, NULL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = MuxInitialize(ctx);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    VLOG("device added");
    return STATUS_SUCCESS;
}

NTSTATUS
TglEvtPrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST ResourcesRaw,
                        _In_ WDFCMRESLIST ResourcesTranslated)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);
    NTSTATUS status;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    /* Without the busy bit, commands are sent blind as before. */
    status = PmcMap(ctx, ResourcesTranslated);
    if (!NT_SUCCESS(status)) {
        VLOG("PMC IPC map failed 0x%08x", status);
    }

    /* IOM status is diagnostic only; the device works without it. */
    status = IomMap(ctx);
    if (!NT_SUCCESS(status)) {
        VLOG("IOM map failed 0x%08x", status);
    }

    /* Without port descriptions the device still serves the diagnostic IOCTLs. */
    status = MuxLoadPortConfig(ctx);
    if (!NT_SUCCESS(status)) {
        VLOG("no USB-C mux ports described in ACPI (0x%08x)", status);
    }
    return STATUS_SUCCESS;
}

NTSTATUS
TglEvtD0Entry(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    MuxStart(GetDeviceContext(Device), (INT32)PreviousState);
    return STATUS_SUCCESS;
}

NTSTATUS
TglEvtD0Exit(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    MuxStop(GetDeviceContext(Device), (INT32)TargetState);
    return STATUS_SUCCESS;
}

NTSTATUS
TglEvtReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST ResourcesTranslated)
{
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    EcRelease(GetDeviceContext(Device));
    IomUnmap(GetDeviceContext(Device));
    PmcUnmap(GetDeviceContext(Device));
    return STATUS_SUCCESS;
}

static NTSTATUS
TglIoctlPmcUsbc(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST Request, _Out_ size_t *Information)
{
    PTGL_PMC_REQUEST in = NULL;
    PTGL_PMC_RESPONSE out = NULL;
    NTSTATUS status;

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, NULL);
    if (NT_SUCCESS(status)) {
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), (PVOID *)&out, NULL);
    }
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (in->Length == 0 || in->Length > TGL_PMC_MAX_MESSAGE) {
        return STATUS_INVALID_PARAMETER;
    }

    TGL_PMC_REQUEST req = *in;    /* in and out share the system buffer */
    status = PmcIpcCommand(Ctx, PMC_IPC_USBC_CMD, 0, req.Message, req.Length,
                           &out->IpcStatus, out->ReadBuffer);
    if (NT_SUCCESS(status)) {
        VLOG("PMC usbc len %u msg %02x %02x -> ipc %u rbuf0 0x%08x", req.Length, req.Message[0],
             req.Message[1], out->IpcStatus, out->ReadBuffer[0]);
        *Information = sizeof(*out);
    }
    return status;
}

static NTSTATUS
TglIoctlEcCommand(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST Request, _Out_ size_t *Information)
{
    PTGL_EC_REQUEST in;
    PVOID out = NULL;
    size_t inLength;
    TGL_EC_REQUEST hdr;
    PVOID payload = NULL;
    NTSTATUS status;

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, &inLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    hdr = *in;
    if (hdr.OutSize > 0x1000 || hdr.InSize > 0x1000 || inLength < sizeof(hdr) + hdr.OutSize) {
        return STATUS_INVALID_PARAMETER;
    }
    if (hdr.InSize != 0) {
        status = WdfRequestRetrieveOutputBuffer(Request, hdr.InSize, &out, NULL);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    /* Input and output share the system buffer, so copy the payload first. */
    if (hdr.OutSize != 0) {
        payload = ExAllocatePool2(POOL_FLAG_NON_PAGED, hdr.OutSize, TGL_POOL_TAG);
        if (payload == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlCopyMemory(payload, in + 1, hdr.OutSize);
    }

    status = EcCommand(Ctx, hdr.Command, hdr.Version, payload, hdr.OutSize, out, hdr.InSize);
    if (NT_SUCCESS(status)) {
        *Information = hdr.InSize;
    }

    if (payload != NULL) {
        ExFreePoolWithTag(payload, TGL_POOL_TAG);
    }
    return status;
}

static NTSTATUS
TglIoctlIomStatus(_In_ PDEVICE_CONTEXT Ctx, _In_ WDFREQUEST Request, _Out_ size_t *Information)
{
    PTGL_IOM_STATUS out;
    NTSTATUS status;

    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), (PVOID *)&out, NULL);
    if (NT_SUCCESS(status)) {
        status = IomReadPortStatus(Ctx, out);
    }
    if (NT_SUCCESS(status)) {
        *Information = sizeof(*out);
    }
    return status;
}

VOID
TglEvtIoDeviceControl(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request, _In_ size_t OutputBufferLength,
                        _In_ size_t InputBufferLength, _In_ ULONG IoControlCode)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(WdfIoQueueGetDevice(Queue));
    size_t information = 0;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {
    case IOCTL_TGL_PMC_USBC:
        status = TglIoctlPmcUsbc(ctx, Request, &information);
        break;
    case IOCTL_TGL_EC_COMMAND:
        status = TglIoctlEcCommand(ctx, Request, &information);
        break;
    case IOCTL_TGL_IOM_STATUS:
        status = TglIoctlIomStatus(ctx, Request, &information);
        break;
    case IOCTL_TGL_GET_LOG: {
        PTGL_LOG log;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*log), (PVOID *)&log, NULL);
        if (NT_SUCCESS(status)) {
            LogCopy(ctx, log);
            information = sizeof(*log);
        }
        break;
    }
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, information);
}
