/* SPDX-License-Identifier: MIT */
/*
 * iom.c - read-only access to the Tiger Lake / Alder Lake IOM port status registers.
 *
 * The IOM is a separate ACPI device (\_SB.IOM, INTC1072 on Tiger Lake or INTC1079
 * on Alder Lake / Alder Lake-N) whose _CRS on these Chromebooks is
 * Memory32Fixed(0xFBC10000, 0x1600). Each TCSS port has a 32-bit IOM_PORT_STATUS
 * register at offset:
 *   - 0x560 + 4 * (port - 1) on Tiger Lake (ACPI\INTC1072)
 *   - 0x160 + 4 * (port - 1) on Alder Lake / Alder Lake-N (ACPI\INTC1079)
 */
#include "driver.h"
#include <intrin.h>

#define IOM_ACPI_ADL_ENUM_KEY \
    L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Enum\\ACPI\\INTC1079"

static BOOLEAN
IomHasAcpiDevice(_In_z_ PCWSTR EnumSubKey)
{
    UNICODE_STRING path;
    OBJECT_ATTRIBUTES oa;
    HANDLE key = NULL;
    NTSTATUS status;

    RtlInitUnicodeString(&path, EnumSubKey);
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    status = ZwOpenKey(&key, KEY_READ, &oa);
    if (NT_SUCCESS(status)) {
        ZwClose(key);
        return TRUE;
    }
    return FALSE;
}

static BOOLEAN
IomIsAlderLakeCpu(_Out_ UINT32 *FamilyOut, _Out_ UINT32 *ModelOut)
{
    int regs[4] = { 0 };
    UINT32 eax, baseFamily, baseModel, extFamily, extModel, family, model;

    __cpuid(regs, 1);
    eax = (UINT32)regs[0];
    baseFamily = (eax >> 8) & 0xFU;
    baseModel  = (eax >> 4) & 0xFU;
    extFamily  = (eax >> 20) & 0xFFU;
    extModel   = (eax >> 16) & 0xFU;

    family = (baseFamily == 0xFU) ? (baseFamily + extFamily) : baseFamily;
    model  = (baseFamily == 0x6U || baseFamily == 0xFU) ? ((extModel << 4) | baseModel) : baseModel;

    *FamilyOut = family;
    *ModelOut = model;

    if (family != 0x6U) {
        return FALSE;
    }

    switch (model) {
    case 0x97U: /* Alder Lake-S */
    case 0x9AU: /* Alder Lake-P / U */
    case 0xBEU: /* Alder Lake-N (e.g. Intel N100 / N200 / i3-N305) */
    case 0xB7U: /* Raptor Lake-S */
    case 0xBAU: /* Raptor Lake-P / U */
    case 0xBFU: /* Raptor Lake-S refresh */
        return TRUE;
    default:
        return FALSE;
    }
}

static ULONG
IomDetectPortStatusOffset(_In_ PDEVICE_CONTEXT Ctx)
{
    WDFKEY key;
    DECLARE_CONST_UNICODE_STRING(valueName, L"IomPortStatusOffset");
    ULONG offset = 0;
    UINT32 family = 0, model = 0;

    /* Optional registry override: Device Parameters\IomPortStatusOffset (DWORD). */
    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(Ctx->Device, PLUGPLAY_REGKEY_DEVICE, KEY_READ,
                                            WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        NTSTATUS status = WdfRegistryQueryULong(key, &valueName, &offset);
        WdfRegistryClose(key);
        if (NT_SUCCESS(status)) {
            if ((offset & 3UL) == 0 && offset <= IOM_MMIO_SIZE - IOM_PORT_COUNT * sizeof(ULONG)) {
                VLOG("IOM port status offset 0x%x (registry override)", offset);
                return offset;
            }
            VLOG("ignoring invalid IomPortStatusOffset registry override 0x%x", offset);
        }
    }

    if (IomHasAcpiDevice(IOM_ACPI_ADL_ENUM_KEY)) {
        VLOG("IOM port status offset 0x%x (ACPI\\INTC1079 detected)", IOM_PORT_STATUS_OFFSET_ADL);
        return IOM_PORT_STATUS_OFFSET_ADL;
    }

    if (IomIsAlderLakeCpu(&family, &model)) {
        VLOG("IOM port status offset 0x%x (CPUID family 0x%x model 0x%x)",
             IOM_PORT_STATUS_OFFSET_ADL, family, model);
        return IOM_PORT_STATUS_OFFSET_ADL;
    }

    VLOG("IOM port status offset 0x%x (default Tiger Lake / INTC1072, CPUID family 0x%x model 0x%x)",
         IOM_PORT_STATUS_OFFSET_TGL, family, model);
    return IOM_PORT_STATUS_OFFSET_TGL;
}

NTSTATUS
IomMap(_In_ PDEVICE_CONTEXT Ctx)
{
    PHYSICAL_ADDRESS pa;

    Ctx->IomPortStatusOffset = IomDetectPortStatusOffset(Ctx);
    pa.QuadPart = (LONGLONG)(IOM_BASE_ADDRESS + Ctx->IomPortStatusOffset);
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
