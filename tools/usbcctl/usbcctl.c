/* SPDX-License-Identifier: MIT */
/*
 * usbcctl - diagnostic CLI for the TigerLakeUsbcMux driver.
 *
 *   usbcctl pmc <hex bytes...>          send a raw PMC USB-C mux message (IPC command 0xA7)
 *   usbcctl ec <cmd> <ver> <insize> [hex bytes...]
 *                                        send a raw EC host command, dump the response
 *   usbcctl muxinfo                     EC_CMD_USB_PD_MUX_INFO for every port
 *   usbcctl pd suspend|resume <port>    EC_CMD_PD_CONTROL: simulate unplug / replug
 *   usbcctl iom                         IOM port status registers
 *   usbcctl log                         recent automatic mux activity
 *
 * Must run elevated.
 */
#include <windows.h>
#include <initguid.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../driver/public.h"

#define EC_CMD_USB_PD_PORTS     0x0102
#define EC_CMD_PD_CONTROL       0x0119
#define EC_CMD_USB_PD_MUX_INFO  0x011A

static HANDLE
OpenMuxDevice(void)
{
    HDEVINFO set;
    SP_DEVICE_INTERFACE_DATA ifData = { sizeof(ifData) };
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail;
    DWORD needed = 0;
    HANDLE h = INVALID_HANDLE_VALUE;

    set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_TGL_USBC_MUX, NULL, NULL,
                               DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        return h;
    }
    if (SetupDiEnumDeviceInterfaces(set, NULL, &GUID_DEVINTERFACE_TGL_USBC_MUX, 0, &ifData)) {
        SetupDiGetDeviceInterfaceDetailW(set, &ifData, NULL, 0, &needed, NULL);
        detail = malloc(needed);
        if (detail != NULL) {
            detail->cbSize = sizeof(*detail);
            if (SetupDiGetDeviceInterfaceDetailW(set, &ifData, detail, needed, NULL, NULL)) {
                h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                OPEN_EXISTING, 0, NULL);
            }
            free(detail);
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return h;
}

static int
EcCommand(HANDLE h, UINT32 cmd, UINT32 ver, const void *out, UINT32 outSize, void *in, UINT32 inSize)
{
    BYTE buf[sizeof(TGL_EC_REQUEST) + 256];
    PTGL_EC_REQUEST req = (PTGL_EC_REQUEST)buf;
    DWORD returned = 0;

    if (outSize > 256) {
        return 0;
    }
    req->Command = cmd;
    req->Version = ver;
    req->OutSize = outSize;
    req->InSize = inSize;
    if (outSize != 0) {
        memcpy(req + 1, out, outSize);
    }
    if (!DeviceIoControl(h, IOCTL_TGL_EC_COMMAND, buf, sizeof(*req) + outSize, in, inSize,
                         &returned, NULL)) {
        fprintf(stderr, "EC command 0x%x failed: error %lu\n", cmd, GetLastError());
        return 0;
    }
    return 1;
}

static int
CmdPmc(HANDLE h, int argc, char **argv)
{
    TGL_PMC_REQUEST req = { 0 };
    TGL_PMC_RESPONSE resp = { 0 };
    DWORD returned = 0;
    int i;

    if (argc < 1 || argc > TGL_PMC_MAX_MESSAGE) {
        fprintf(stderr, "usage: usbcctl pmc <hex bytes...>\n");
        return 2;
    }
    for (i = 0; i < argc; i++) {
        req.Message[i] = (UINT8)strtoul(argv[i], NULL, 16);
    }
    req.Length = (UINT8)argc;

    if (!DeviceIoControl(h, IOCTL_TGL_PMC_USBC, &req, sizeof(req), &resp, sizeof(resp),
                         &returned, NULL)) {
        fprintf(stderr, "PMC IPC failed: error %lu\n", GetLastError());
        return 1;
    }

    /* Linux intel_pmc_mux: connect/disconnect report status in byte 2, others in byte 1. */
    UINT8 usage = req.Message[0] & 0xF;
    UINT8 res = (UINT8)(resp.ReadBuffer[0] >> (usage < 2 ? 16 : 8));
    printf("ipc status %u  rbuf %08x %08x %08x %08x  -> %s%s\n", resp.IpcStatus,
           resp.ReadBuffer[0], resp.ReadBuffer[1], resp.ReadBuffer[2], resp.ReadBuffer[3],
           (res & 1) ? "FAILED" : "ok", (res & 2) ? " (fatal)" : "");
    return resp.IpcStatus == 0 && !(res & 1) ? 0 : 1;
}

static int
CmdEc(HANDLE h, int argc, char **argv)
{
    BYTE out[256], in[256];
    UINT32 cmd, ver, inSize, outSize = 0;
    int i;

    if (argc < 3) {
        fprintf(stderr, "usage: usbcctl ec <cmd> <ver> <insize> [hex bytes...]\n");
        return 2;
    }
    cmd = strtoul(argv[0], NULL, 0);
    ver = strtoul(argv[1], NULL, 0);
    inSize = strtoul(argv[2], NULL, 0);
    if (inSize > sizeof(in) || argc - 3 > (int)sizeof(out)) {
        return 2;
    }
    for (i = 3; i < argc; i++) {
        out[outSize++] = (BYTE)strtoul(argv[i], NULL, 16);
    }
    if (!EcCommand(h, cmd, ver, out, outSize, in, inSize)) {
        return 1;
    }
    for (i = 0; i < (int)inSize; i++) {
        printf("%02x%s", in[i], (i % 16 == 15 || i == (int)inSize - 1) ? "\n" : " ");
    }
    return 0;
}

static int
CmdMuxInfo(HANDLE h)
{
    BYTE ports = 0;
    BYTE port, flags;

    if (!EcCommand(h, EC_CMD_USB_PD_PORTS, 0, NULL, 0, &ports, 1)) {
        return 1;
    }
    for (port = 0; port < ports; port++) {
        if (!EcCommand(h, EC_CMD_USB_PD_MUX_INFO, 0, &port, 1, &flags, 1)) {
            return 1;
        }
        printf("port %u: 0x%02x USB=%d DP=%d POLARITY=%s HPD_IRQ=%d HPD_LVL=%d SAFE=%d TBT=%d USB4=%d\n",
               port, flags, !!(flags & 0x01), !!(flags & 0x02), (flags & 0x04) ? "INVERTED" : "NORMAL",
               !!(flags & 0x08), !!(flags & 0x10), !!(flags & 0x20), !!(flags & 0x40), !!(flags & 0x80));
    }
    return 0;
}

static int
CmdPd(HANDLE h, int argc, char **argv)
{
    BYTE params[2];

    if (argc != 2 || (strcmp(argv[0], "suspend") != 0 && strcmp(argv[0], "resume") != 0)) {
        fprintf(stderr, "usage: usbcctl pd suspend|resume <port>\n");
        return 2;
    }
    params[0] = (BYTE)strtoul(argv[1], NULL, 0);            /* chip (port) */
    params[1] = strcmp(argv[0], "suspend") == 0 ? 0 : 1;    /* PD_SUSPEND / PD_RESUME */
    return EcCommand(h, EC_CMD_PD_CONTROL, 0, params, sizeof(params), NULL, 0) ? 0 : 1;
}

static int
CompareLogEntries(const void *a, const void *b)
{
    UINT32 sa = ((const TGL_LOG_ENTRY *)a)->Sequence, sb = ((const TGL_LOG_ENTRY *)b)->Sequence;
    return sa < sb ? -1 : sa > sb;
}

static int
CmdLog(HANDLE h)
{
    static const char *events[] = {
        "?", "start", "stop", "adopt", "connect", "disconnect", "reconnect", "ec-error", "dp-enter", "hpd", "dp-request",
    };
    static TGL_LOG log;
    DWORD returned = 0;
    UINT32 i;

    if (!DeviceIoControl(h, IOCTL_TGL_GET_LOG, NULL, 0, &log, sizeof(log), &returned, NULL)) {
        fprintf(stderr, "reading log failed: error %lu\n", GetLastError());
        return 1;
    }
    qsort(log.Entries, TGL_LOG_ENTRIES, sizeof(log.Entries[0]), CompareLogEntries);

    for (i = 0; i < TGL_LOG_ENTRIES; i++) {
        const TGL_LOG_ENTRY *e = &log.Entries[i];
        FILETIME utc, local;
        SYSTEMTIME st;
        const char *name;

        if (e->Sequence == 0) {
            continue;
        }
        utc.dwLowDateTime = (DWORD)e->Time;
        utc.dwHighDateTime = (DWORD)(e->Time >> 32);
        FileTimeToLocalFileTime(&utc, &local);
        FileTimeToSystemTime(&local, &st);
        name = e->Event < ARRAYSIZE(events) ? events[e->Event] : "?";

        printf("%5u %02u:%02u:%02u.%03u  %-10s", e->Sequence, st.wHour, st.wMinute, st.wSecond,
               st.wMilliseconds, name);
        if (e->Event == TglLogStart || e->Event == TglLogStop) {
            /* WDF_POWER_DEVICE_STATE: 1 = D0 ... 4 = D3, 5 = D3Final (first start / removal). */
            printf(" %s D%d\n", e->Event == TglLogStart ? "from" : "to", e->Status == 5 ? 3 : e->Status - 1);
        } else if (e->Event == TglLogEcError) {
            printf(" port %u status 0x%08lx\n", e->Port, (unsigned long)e->Status);
        } else {
            unsigned pin = e->Flags >> TGL_LOG_FLAG_DP_PIN_SHIFT;

            printf(" port %u ec-mux 0x%02x %s %s iom-before 0x%08x status 0x%08lx", e->Port, e->EcMuxFlags,
                   (e->Flags & TGL_LOG_FLAG_REVERSED) ? "reversed" : "normal  ",
                   (e->Flags & TGL_LOG_FLAG_UFP) ? "ufp" : "dfp", e->IomStatus, (unsigned long)e->Status);
            if (e->Event == TglLogDpEnter) {
                printf(" pin %c", pin >= 1 && pin <= 6 ? 'A' + pin - 1 : '?');
            } else if (e->Event == TglLogHpd) {
                printf(" hpd %s%s", (e->EcMuxFlags & 0x10) ? "high" : "low", (e->EcMuxFlags & 0x08) ? " irq" : "");
            }
            printf("\n");
        }
    }
    return 0;
}

static int
CmdIom(HANDLE h)
{
    static const char *activity[16] = {
        "none", "1", "2", "USB", "SAFE", "DP", "DP_MFD", "TBT",
        "8", "9", "10", "11", "ALT_USB", "ALT_TBT_USB", "14", "15",
    };
    TGL_IOM_STATUS st = { 0 };
    DWORD returned = 0;
    UINT32 i;

    if (!DeviceIoControl(h, IOCTL_TGL_IOM_STATUS, NULL, 0, &st, sizeof(st), &returned, NULL)) {
        fprintf(stderr, "IOM status failed: error %lu\n", GetLastError());
        return 1;
    }
    for (i = 0; i < st.PortCount && i < TGL_IOM_MAX_PORTS; i++) {
        UINT32 v = st.PortStatus[i];
        printf("tcss port %u: 0x%08x connected=%u activity=%s ufp=%u hpd=%u\n", i + 1, v,
               v >> 31, activity[(v >> 6) & 0xF], (v >> 10) & 1, (v >> 12) & 3);
    }
    return 0;
}

int
main(int argc, char **argv)
{
    HANDLE h;
    int rc;

    if (argc < 2) {
        fprintf(stderr, "usage: usbcctl pmc|ec|muxinfo|pd|iom ...\n");
        return 2;
    }

    h = OpenMuxDevice();
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "cannot open TigerLakeUsbcMux device: error %lu\n", GetLastError());
        return 1;
    }

    if (strcmp(argv[1], "pmc") == 0) {
        rc = CmdPmc(h, argc - 2, argv + 2);
    } else if (strcmp(argv[1], "ec") == 0) {
        rc = CmdEc(h, argc - 2, argv + 2);
    } else if (strcmp(argv[1], "muxinfo") == 0) {
        rc = CmdMuxInfo(h);
    } else if (strcmp(argv[1], "pd") == 0) {
        rc = CmdPd(h, argc - 2, argv + 2);
    } else if (strcmp(argv[1], "iom") == 0) {
        rc = CmdIom(h);
    } else if (strcmp(argv[1], "log") == 0) {
        rc = CmdLog(h);
    } else {
        fprintf(stderr, "unknown command %s\n", argv[1]);
        rc = 2;
    }

    CloseHandle(h);
    return rc;
}
