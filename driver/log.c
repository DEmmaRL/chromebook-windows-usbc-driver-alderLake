/* SPDX-License-Identifier: MIT */
/*
 * log.c - small ring buffer of mux activity, read by `usbcctl log`.
 *
 * Kernel debug output needs a debugger attached; this keeps the last few
 * decisions available on any machine.
 */
#include "driver.h"

VOID
LogEvent(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ TGL_LOG_EVENT Event,
    _In_ UINT8 Port,
    _In_ UINT8 EcMuxFlags,
    _In_ UINT8 Flags,
    _In_ UINT32 IomStatus,
    _In_ INT32 Status)
{
    PTGL_LOG_ENTRY entry;
    LARGE_INTEGER now;
    KIRQL irql;

    KeQuerySystemTimePrecise(&now);

    KeAcquireSpinLock(&Ctx->LogLock, &irql);
    Ctx->LogSequence++;
    entry = &Ctx->Log.Entries[Ctx->LogSequence % TGL_LOG_ENTRIES];
    entry->Sequence = Ctx->LogSequence;
    entry->Event = (UINT8)Event;
    entry->Port = Port;
    entry->EcMuxFlags = EcMuxFlags;
    entry->Flags = Flags;
    entry->Time = now.QuadPart;
    entry->IomStatus = IomStatus;
    entry->Status = Status;
    KeReleaseSpinLock(&Ctx->LogLock, irql);
}

VOID
LogCopy(_In_ PDEVICE_CONTEXT Ctx, _Out_ PTGL_LOG Log)
{
    KIRQL irql;

    KeAcquireSpinLock(&Ctx->LogLock, &irql);
    RtlCopyMemory(Log, &Ctx->Log, sizeof(*Log));
    KeReleaseSpinLock(&Ctx->LogLock, irql);
}
