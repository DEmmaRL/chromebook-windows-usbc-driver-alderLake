/* SPDX-License-Identifier: MIT */
/*
 * acpi.c - evaluate ACPI objects below the PMC device and read _DSD properties.
 */
#include "driver.h"

/* Device Properties UUID daffd814-6eba-4d8c-8a91-bc9bbf4aa301 in ACPI ToUUID byte order. */
static const UCHAR DsdPropertiesUuid[16] = {
    0x14, 0xd8, 0xff, 0xda, 0xba, 0x6e, 0x8c, 0x4d, 0x8a, 0x91, 0xbc, 0x9b, 0xbf, 0x4a, 0xa3, 0x01,
};

NTSTATUS
AcpiEvaluateChild(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_z_ PCSTR Path,
    _Outptr_result_bytebuffer_(*OutLength) PACPI_EVAL_OUTPUT_BUFFER *Out,
    _Out_ ULONG *OutLength)
{
    ACPI_EVAL_INPUT_BUFFER_EX in;
    WDF_MEMORY_DESCRIPTOR inDesc, outDesc;
    PACPI_EVAL_OUTPUT_BUFFER buffer;
    ULONG length = 512;
    ULONG_PTR returned;
    NTSTATUS status;
    int attempt;

    *Out = NULL;
    *OutLength = 0;

    RtlZeroMemory(&in, sizeof(in));
    in.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE_EX;
    status = RtlStringCchCopyA(in.MethodName, sizeof(in.MethodName), Path);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    for (attempt = 0; attempt < 2; attempt++) {
        buffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, length, TGL_POOL_TAG);
        if (buffer == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&inDesc, &in, sizeof(in));
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&outDesc, buffer, length);
        returned = 0;
        status = WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(Ctx->Device), NULL,
                                                   IOCTL_ACPI_EVAL_METHOD_EX, &inDesc, &outDesc,
                                                   NULL, &returned);
        if (status == STATUS_BUFFER_OVERFLOW && returned >= sizeof(ACPI_EVAL_OUTPUT_BUFFER) &&
            buffer->Length > length) {
            length = buffer->Length;
            ExFreePoolWithTag(buffer, TGL_POOL_TAG);
            continue;
        }
        if (!NT_SUCCESS(status) || returned < FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument) ||
            buffer->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE) {
            ExFreePoolWithTag(buffer, TGL_POOL_TAG);
            return NT_SUCCESS(status) ? STATUS_ACPI_INVALID_DATA : status;
        }

        *Out = buffer;
        *OutLength = (ULONG)returned;
        return STATUS_SUCCESS;
    }
    return STATUS_BUFFER_OVERFLOW;
}

/* Returns the argument following Arg, or NULL if it would run past End. */
static const ACPI_METHOD_ARGUMENT *
NextArgument(_In_ const ACPI_METHOD_ARGUMENT *Arg, _In_ const UCHAR *End)
{
    const UCHAR *next = (const UCHAR *)ACPI_METHOD_NEXT_ARGUMENT(Arg);
    return next + FIELD_OFFSET(ACPI_METHOD_ARGUMENT, Data) <= End ? (const ACPI_METHOD_ARGUMENT *)next : NULL;
}

static BOOLEAN
ArgumentFits(_In_ const ACPI_METHOD_ARGUMENT *Arg, _In_ const UCHAR *End)
{
    return (const UCHAR *)Arg + FIELD_OFFSET(ACPI_METHOD_ARGUMENT, Data) <= End &&
           Arg->Data + Arg->DataLength <= End;
}

const ACPI_METHOD_ARGUMENT *
AcpiDsdFindProperty(
    _In_reads_bytes_(Length) PACPI_EVAL_OUTPUT_BUFFER Dsd,
    _In_ ULONG Length,
    _In_z_ PCSTR Name)
{
    const UCHAR *end = (const UCHAR *)Dsd + Length;
    const ACPI_METHOD_ARGUMENT *uuid, *props, *entry, *key, *value;
    const UCHAR *propsEnd;
    size_t nameLength = strlen(Name) + 1;

    /* _DSD = Package { UUID buffer, Package { Package { "name", value }, ... }, ... } */
    if (Dsd->Count < 2) {
        return NULL;
    }
    uuid = (const ACPI_METHOD_ARGUMENT *)Dsd->Argument;
    if (!ArgumentFits(uuid, end) || uuid->Type != ACPI_METHOD_ARGUMENT_BUFFER ||
        uuid->DataLength != sizeof(DsdPropertiesUuid) ||
        RtlCompareMemory(uuid->Data, DsdPropertiesUuid, sizeof(DsdPropertiesUuid)) != sizeof(DsdPropertiesUuid)) {
        return NULL;
    }
    props = NextArgument(uuid, end);
    if (props == NULL || !ArgumentFits(props, end) || props->Type != ACPI_METHOD_ARGUMENT_PACKAGE) {
        return NULL;
    }

    propsEnd = props->Data + props->DataLength;
    for (entry = (const ACPI_METHOD_ARGUMENT *)props->Data;
         entry != NULL && ArgumentFits(entry, propsEnd);
         entry = NextArgument(entry, propsEnd)) {
        const UCHAR *entryEnd = entry->Data + entry->DataLength;

        if (entry->Type != ACPI_METHOD_ARGUMENT_PACKAGE) {
            continue;
        }
        key = (const ACPI_METHOD_ARGUMENT *)entry->Data;
        if (!ArgumentFits(key, entryEnd) || key->Type != ACPI_METHOD_ARGUMENT_STRING ||
            key->DataLength != nameLength || RtlCompareMemory(key->Data, Name, nameLength) != nameLength) {
            continue;
        }
        value = NextArgument(key, entryEnd);
        return value != NULL && ArgumentFits(value, entryEnd) ? value : NULL;
    }
    return NULL;
}
