/*
 * portpage.c - Device Manager property page for AHCINT, IDENT, NVMENT and
 * MMCSDNT (Windows 2000 / XP / Server 2003, x86 and x64)
 *
 * The page lists the ports (AHCINT), channels and devices (IDENT),
 * namespaces (NVMENT) or slots (MMCSDNT) of one controller with a check
 * box each. A cleared box becomes an OFF= item for that controller in the
 * DriverParameters value of the driver's service; the driver reads it
 * when the controller starts. IDENT also lists each device's transfer
 * mode: "PIO only" becomes a PIO= item. Built once per driver:
 *   PP_AHCI -> ahcipp.dll, PP_IDE -> identpp.dll,
 *   PP_NVME -> nvmentpp.dll, PP_MMC -> mmcsdpp.dll
 * INF: HKR,,EnumPropPages32,,"<dll>,PortPageProvider"
 *
 * Also exported for scripted tests: PortPageTest (rundll32 entry,
 * "<controller index> <out file> [apply items]"), see TESTS.TXT.
 *
 * With PP_EXE the same list is a program of its own for the systems
 * without that page: Windows NT 3.1 to 4.0 (controllers found as
 * \\.\ScsiN:, DriverParameters as above) and Windows 95/98/Me (the
 * AdapterSettings value of each controller's driver key; no live device
 * list there). It runs on 2000/XP as well:
 *   PP_AHCI -> ahcicfg.exe, PP_IDE -> identcfg.exe,
 *   PP_NVME -> nvmecfg.exe, PP_MMC -> mmccfg.exe
 * "<exe> /t <controller index> <out file> [apply items]" is the scripted
 * form, as PortPageTest. nvmecfg also takes "/ns <controller index> <out
 * file> <action>" for namespace administration (see PpExeNs).
 *
 * Copyright (c) 2026 ages2001
 */

#include <windows.h>
#ifndef PP_EXE
#include <commctrl.h>
#include <prsht.h>
#include <setupapi.h>
#endif
#include <winioctl.h>
#include <ntddscsi.h>

#if defined(PP_AHCI)
#define PP_SERVICE      "AHCINT"
#define PP_SIGNATURE    "AHCINT  "
#define PP_TITLE        "Ports"
#define PP_COLUMN       "Port"
#elif defined(PP_IDE)
#define PP_SERVICE      "IDENT"
#define PP_SIGNATURE    "IDENT   "
#define PP_TITLE        "Channels"
#define PP_COLUMN       "Channel / device"
#elif defined(PP_NVME)
#define PP_SERVICE      "NVMENT"
#define PP_SIGNATURE    "NvmeMini"
#define PP_TITLE        "Namespaces"
#define PP_COLUMN       "Namespace"
#elif defined(PP_MMC)
#define PP_SERVICE      "MMCSDNT"
#define PP_SIGNATURE    "MMCSDNT "
#define PP_TITLE        "Slots"
#define PP_COLUMN       "Slot"
#else
#error PP_AHCI, PP_IDE, PP_NVME or PP_MMC
#endif

/* The program (PP_EXE): what identifies a controller of the driver */
#if defined(PP_AHCI)
#define PP_MPD          "AHCINT9X.MPD"
#define PP_LOC_SIG      PP_SIGNATURE
#define PP_LOC_CODE     2
#elif defined(PP_IDE)
#define PP_MPD          "IDENT9X.MPD"
#define PP_LOC_SIG      PP_SIGNATURE
#define PP_LOC_CODE     2
#elif defined(PP_NVME)
#define PP_MPD          "NVMENT9X.MPD"
#define PP_LOC_SIG      "NVMENT  "
#define PP_LOC_CODE     0x1003
#else
#define PP_MPD          "MMCSD9X.MPD"
#define PP_LOC_SIG      PP_SIGNATURE
#define PP_LOC_CODE     2
#endif

#define PP_MAX_ROWS     96
#define PP_MAX_ITEMS    128
#define PP_MAX_OTHERS   32
#define PP_PARAM_LEN    1024

#define IDC_NOTE        100
#define IDC_LIST        101
#define IDC_PLABEL      102
#define IDC_PARAMS      103
#define IDC_PIO         104

#ifndef PP_EXE
/* {2ACCFE60-C130-11D2-B082-00A0C91EFB8B}: ScsiPort registers it for every adapter */
static const GUID PpStoragePortGuid =
    { 0x2accfe60, 0xc130, 0x11d2, { 0xb0, 0x82, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b } };
#endif

static HINSTANCE PpInstance;

/* ------------------------------------------------------------------ */
/* No C runtime                                                        */
/* ------------------------------------------------------------------ */

#ifdef _MSC_VER
#pragma function(memset, memcpy)
#endif

void *memset(void *d, int c, size_t n)
{
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = (unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;
    while (n--) *p++ = *q++;
    return d;
}

static int PpIsSpace(char c) { return c == ' ' || c == '\t' || c == ';' || c == '\r' || c == '\n'; }
static int PpIsDigit(char c) { return c >= '0' && c <= '9'; }
static char PpUpper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }

static void PpCopyN(char *d, const char *s, int n)
{
    while (n-- > 0 && *s) *d++ = *s++;
    *d = 0;
}

static void PpAppend(char *d, const char *s, int size)
{
    int n = lstrlenA(d);
    while (*s && n < size - 1) d[n++] = *s++;
    d[n] = 0;
}

static ULONG PpGet32(const UCHAR *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((ULONG)p[3] << 24); }

#if defined(PP_NVME) || defined(PP_MMC)
/* "12.3 GB" and the like */
static void PpFormatSize(char *out, ULONGLONG bytes)
{
    static const char *units[] = { "bytes", "KB", "MB", "GB", "TB", "PB" };
    ULONGLONG whole = bytes, rest = 0;
    int u = 0;
    while (whole >= 1024 && u < 5) {
        rest = whole & 1023;
        whole >>= 10;
        u++;
    }
    if (u == 0) wsprintfA(out, "%lu bytes", (ULONG)whole);
    else wsprintfA(out, "%lu.%lu %s", (ULONG)whole, (ULONG)(rest * 10 / 1024), units[u]);
}
#endif

/* ------------------------------------------------------------------ */
/* OFF= items                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    BOOL  Pci;          /* "bus.device.function:" in front */
    ULONG Bus, Dev, Fn;
    char  Body[24];     /* "3", "1.0": decimal numbers joined by dots */
} PP_ITEM;

typedef struct {
    BOOL  Pci;
    ULONG Bus, Dev, Fn;
} PP_LOC;

/* One item from p (after "OFF=" or a comma); returns the end or NULL */
static const char *PpParseItem(const char *p, PP_ITEM *it)
{
    ULONG v[6];
    int count = 0, colon = 0, i;
    char tmp[16];
    for (;;) {
        ULONG n = 0;
        if (!PpIsDigit(*p)) return NULL;
        while (PpIsDigit(*p)) n = n * 10 + (ULONG)(*p++ - '0');
        if (count < 6) v[count++] = n;
        if (*p == ':' && count == 3 && !colon) { colon = 3; p++; continue; }
        if (*p == '.') { p++; continue; }
        break;
    }
    if (count <= colon) return NULL;
    it->Pci = colon != 0;
    it->Bus = colon ? v[0] : 0;
    it->Dev = colon ? v[1] : 0;
    it->Fn = colon ? v[2] : 0;
    it->Body[0] = 0;
    for (i = colon; i < count; i++) {
        wsprintfA(tmp, i > colon ? ".%lu" : "%lu", v[i]);
        PpAppend(it->Body, tmp, sizeof(it->Body));
    }
    return p;
}

/* Splits the parameter string into the items of Key ("OFF=", "PIO=") and the rest */
static int PpSplitKey(const char *params, const char *key, PP_ITEM *items, int max, char *rest, int restSize)
{
    const char *p = params;
    int n = 0;
    rest[0] = 0;
    while (*p) {
        const char *start;
        while (*p && PpIsSpace(*p)) p++;
        if (!*p) break;
        start = p;
        if (PpUpper(p[0]) == key[0] && PpUpper(p[1]) == key[1] && PpUpper(p[2]) == key[2] && p[3] == '=') {
            p += 4;
            for (;;) {
                PP_ITEM it;
                const char *e = PpParseItem(p, &it);
                if (e == NULL) break;
                if (n < max) items[n++] = it;
                p = e;
                if (*p != ',') break;
                p++;
            }
            while (*p && !PpIsSpace(*p)) p++;       /* anything unparsable after the key is dropped */
        } else {
            char tok[256];
            while (*p && !PpIsSpace(*p)) p++;
            PpCopyN(tok, start, (int)(p - start) < 255 ? (int)(p - start) : 255);
            if (rest[0]) PpAppend(rest, " ", restSize);
            PpAppend(rest, tok, restSize);
        }
    }
    return n;
}

static int PpSplit(const char *params, PP_ITEM *items, int max, char *rest, int restSize)
{
    return PpSplitKey(params, "OFF=", items, max, rest, restSize);
}

static BOOL PpSameLoc(const PP_ITEM *it, const PP_LOC *l)
{
    return it->Pci && l->Pci && it->Bus == l->Bus && it->Dev == l->Dev && it->Fn == l->Fn;
}

/* Is "body" turned off for this controller? */
static BOOL PpIsOff(const PP_ITEM *items, int n, const PP_LOC *self, const char *body)
{
    int i;
    for (i = 0; i < n; i++) {
        if (lstrcmpA(items[i].Body, body) != 0) continue;
        if (!items[i].Pci || PpSameLoc(&items[i], self)) return TRUE;
    }
    return FALSE;
}

static void PpItemText(char *out, const PP_ITEM *it)
{
    if (it->Pci) wsprintfA(out, "%lu.%lu.%lu:%s", it->Bus, it->Dev, it->Fn, it->Body);
    else lstrcpyA(out, it->Body);
}

static int PpAddItem(PP_ITEM *items, int n, int max, const PP_ITEM *it)
{
    int i;
    char a[48], b[48];
    PpItemText(a, it);
    for (i = 0; i < n; i++) {
        PpItemText(b, &items[i]);
        if (lstrcmpA(a, b) == 0) return n;
    }
    if (n < max) items[n++] = *it;
    return n;
}

/*
 * New parameter string: "off" lists the items this controller should
 * have turned off (bodies). Items of other controllers are kept; an item
 * for all controllers that this one should no longer follow is given to
 * every other controller of the driver by its PCI location.
 */
static void PpRebuildKey(const char *key, const char *params, const PP_LOC *self, const PP_LOC *others, int nOthers,
                         char offBodies[][24], int nOff, char *out, int outSize)
{
    static PP_ITEM items[PP_MAX_ITEMS], result[PP_MAX_ITEMS];
    char rest[PP_PARAM_LEN], text[48];
    int n, r = 0, i, j, k;
    BOOL used[PP_MAX_ROWS];

    n = PpSplitKey(params, key, items, PP_MAX_ITEMS, rest, sizeof(rest));
    for (k = 0; k < nOff && k < PP_MAX_ROWS; k++) used[k] = FALSE;

    for (i = 0; i < n; i++) {
        PP_ITEM *it = &items[i];
        BOOL wanted = FALSE;
        for (k = 0; k < nOff; k++) {
            if (lstrcmpA(offBodies[k], it->Body) == 0) { wanted = TRUE; break; }
        }
        if (it->Pci) {
            if (PpSameLoc(it, self)) continue;              /* rebuilt below */
            r = PpAddItem(result, r, PP_MAX_ITEMS, it);
        } else if (wanted || !self->Pci) {
            if (wanted) {
                r = PpAddItem(result, r, PP_MAX_ITEMS, it);
                used[k] = TRUE;
            }
            /* this controller has no PCI location: only items for all
               controllers reach it, so one it no longer wants goes */
        } else {
            for (j = 0; j < nOthers; j++) {
                PP_ITEM q = *it;
                if (!others[j].Pci) continue;
                q.Pci = TRUE;
                q.Bus = others[j].Bus;
                q.Dev = others[j].Dev;
                q.Fn = others[j].Fn;
                r = PpAddItem(result, r, PP_MAX_ITEMS, &q);
            }
        }
    }
    for (k = 0; k < nOff; k++) {
        PP_ITEM q;
        if (used[k]) continue;
        q.Pci = self->Pci;
        q.Bus = self->Bus;
        q.Dev = self->Dev;
        q.Fn = self->Fn;
        lstrcpyA(q.Body, offBodies[k]);
        r = PpAddItem(result, r, PP_MAX_ITEMS, &q);
    }

    out[0] = 0;
    PpAppend(out, rest, outSize);
    if (r > 0) {
        if (out[0]) PpAppend(out, " ", outSize);
        PpAppend(out, key, outSize);
        for (i = 0; i < r; i++) {
            PpItemText(text, &result[i]);
            if (i) PpAppend(out, ",", outSize);
            PpAppend(out, text, outSize);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Controller                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    char Item[24];      /* OFF= body */
    char Label[48];
    char Desc[128];
    BOOL On;
    BOOL Pio;           /* IDENT device: PIO only (PIO= item) */
} PP_ROW;

typedef struct {
#ifdef PP_EXE
    int             Port;           /* \\.\ScsiN:, -1 on 9x */
#else
    HDEVINFO        Set;
    SP_DEVINFO_DATA Dev;
#endif
    PP_LOC          Self;
    PP_LOC          Others[PP_MAX_OTHERS];
    int             NumOthers;
    PP_ROW          Rows[PP_MAX_ROWS];
    int             NumRows;
    BOOL            Opened;         /* the adapter answered */
    char            Params[PP_PARAM_LEN];
    HWND            List, Edit;
    BOOL            Updating;
} PP_PAGE;

static UCHAR PpBuf[16384];

#ifndef PP_EXE
static BOOL PpLocation(HDEVINFO set, PSP_DEVINFO_DATA dev, PP_LOC *loc)
{
    DWORD bus = 0, addr = 0, type, n;
    char enumr[32];
    loc->Pci = FALSE;
    if (!SetupDiGetDeviceRegistryPropertyA(set, dev, SPDRP_ENUMERATOR_NAME, &type, (PBYTE)enumr, sizeof(enumr), &n) ||
        lstrcmpiA(enumr, "PCI") != 0) {
        return FALSE;
    }
    if (!SetupDiGetDeviceRegistryPropertyA(set, dev, SPDRP_BUSNUMBER, &type, (PBYTE)&bus, sizeof(bus), &n)) return FALSE;
    if (!SetupDiGetDeviceRegistryPropertyA(set, dev, SPDRP_ADDRESS, &type, (PBYTE)&addr, sizeof(addr), &n)) return FALSE;
    loc->Pci = TRUE;
    loc->Bus = bus;
    loc->Dev = addr >> 16;
    loc->Fn = addr & 0xFFFF;
    return TRUE;
}

static BOOL PpIsOurs(HDEVINFO set, PSP_DEVINFO_DATA dev)
{
    char svc[64];
    DWORD type, n;
    return SetupDiGetDeviceRegistryPropertyA(set, dev, SPDRP_SERVICE, &type, (PBYTE)svc, sizeof(svc), &n) &&
           lstrcmpiA(svc, PP_SERVICE) == 0;
}

/* The other controllers of the driver, by PCI location */
static void PpFindOthers(PP_PAGE *pg)
{
    HDEVINFO set = SetupDiGetClassDevsA(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    SP_DEVINFO_DATA d;
    DWORD i;
    pg->NumOthers = 0;
    if (set == INVALID_HANDLE_VALUE) return;
    for (i = 0; ; i++) {
        d.cbSize = sizeof(d);
        if (!SetupDiEnumDeviceInfo(set, i, &d)) break;
        if (d.DevInst == pg->Dev.DevInst || !PpIsOurs(set, &d)) continue;
        if (pg->NumOthers < PP_MAX_OTHERS) {
            PpLocation(set, &d, &pg->Others[pg->NumOthers]);
            pg->NumOthers++;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
}

/* The adapter's ScsiPort device, through its storage port interface */
static HANDLE PpOpenAdapter(PP_PAGE *pg)
{
    HDEVINFO set = SetupDiGetClassDevsA(&PpStoragePortGuid, NULL, NULL, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    SP_DEVICE_INTERFACE_DATA ifd;
    SP_DEVINFO_DATA d;
    PSP_DEVICE_INTERFACE_DETAIL_DATA_A det = (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)PpBuf;
    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD i;
    if (set == INVALID_HANDLE_VALUE) return h;
    for (i = 0; h == INVALID_HANDLE_VALUE; i++) {
        ifd.cbSize = sizeof(ifd);
        if (!SetupDiEnumDeviceInterfaces(set, NULL, &PpStoragePortGuid, i, &ifd)) break;
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        d.cbSize = sizeof(d);
        if (!SetupDiGetDeviceInterfaceDetailA(set, &ifd, det, sizeof(PpBuf), NULL, &d)) continue;
        if (d.DevInst != pg->Dev.DevInst) continue;
        h = CreateFileA(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
    }
    SetupDiDestroyDeviceInfoList(set);
    return h;
}

#endif

#ifdef PP_EXE
/* The controllers of the driver: NT family by the ScsiPort device map,
   9x by the PortDriver of the driver keys */
typedef struct {
    int    Port;            /* \\.\ScsiN:, -1 on 9x */
    char   Key[160];        /* 9x: driver key under HKLM */
    PP_LOC Loc;
    char   Name[96];
} PP_CTL;

static PP_CTL PpCtl[PP_MAX_OTHERS];
static int PpCtlCount;
static BOOL Pp9x;

static HANDLE PpOpenPort(int port)
{
    char name[32];
    wsprintfA(name, "\\\\.\\Scsi%d:", port);
    return CreateFileA(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, 0, NULL);
}

static HANDLE PpOpenAdapter(PP_PAGE *pg)
{
    if (pg->Port < 0) return INVALID_HANDLE_VALUE;
    return PpOpenPort(pg->Port);
}

static BOOL PpMiniportSig(HANDLE h, const char *sig, ULONG code, ULONG length);

static void PpFindControllers(void)
{
    char key[160], value[64];
    HKEY k;
    DWORD type, n;
    int port, i;

    PpCtlCount = 0;
    Pp9x = (GetVersion() & 0x80000000) != 0;
    if (!Pp9x) {
        for (port = 0; port < 64 && PpCtlCount < PP_MAX_OTHERS; port++) {
            PP_CTL *c = &PpCtl[PpCtlCount];
            HANDLE h;
            wsprintfA(key, "HARDWARE\\DEVICEMAP\\Scsi\\Scsi Port %d", port);
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
            n = sizeof(value);
            value[0] = 0;
            if (RegQueryValueExA(k, "Driver", NULL, &type, (LPBYTE)value, &n) != ERROR_SUCCESS || type != REG_SZ) value[0] = 0;
            RegCloseKey(k);
            if (lstrcmpiA(value, PP_SERVICE) != 0) continue;
            memset(c, 0, sizeof(*c));
            c->Port = port;
            h = PpOpenPort(port);
            if (h != INVALID_HANDLE_VALUE) {
                memset(PpBuf, 0, sizeof(SRB_IO_CONTROL) + 4);
                if (PpMiniportSig(h, PP_LOC_SIG, PP_LOC_CODE, 4)) {
                    ULONG v = PpGet32(PpBuf + sizeof(SRB_IO_CONTROL));
                    if (v != 0xFFFFFFFF) {
                        c->Loc.Pci = TRUE;
                        c->Loc.Bus = v >> 16;
                        c->Loc.Dev = (v >> 8) & 0xFF;
                        c->Loc.Fn = v & 0xFF;
                    }
                }
                CloseHandle(h);
            }
            if (c->Loc.Pci) wsprintfA(c->Name, "SCSI port %d, PCI %lu.%lu.%lu", port, c->Loc.Bus, c->Loc.Dev, c->Loc.Fn);
            else wsprintfA(c->Name, "SCSI port %d", port);
            PpCtlCount++;
        }
        return;
    }
    {
        static const char *classes[] = { "SCSIAdapter", "hdc" };
        int cl;
        for (cl = 0; cl < 2; cl++) {
            for (i = 0; i < 100 && PpCtlCount < PP_MAX_OTHERS; i++) {
                PP_CTL *c = &PpCtl[PpCtlCount];
                wsprintfA(key, "System\\CurrentControlSet\\Services\\Class\\%s\\%04d", classes[cl], i);
                if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
                n = sizeof(value);
                value[0] = 0;
                if (RegQueryValueExA(k, "PortDriver", NULL, &type, (LPBYTE)value, &n) != ERROR_SUCCESS) value[0] = 0;
                if (lstrcmpiA(value, PP_MPD) == 0) {
                    memset(c, 0, sizeof(*c));
                    c->Port = -1;
                    lstrcpyA(c->Key, key);
                    n = sizeof(c->Name) - 16;
                    if (RegQueryValueExA(k, "DriverDesc", NULL, &type, (LPBYTE)c->Name, &n) != ERROR_SUCCESS) c->Name[0] = 0;
                    wsprintfA(value, " (%s\\%04d)", classes[cl], i);
                    PpAppend(c->Name, value, sizeof(c->Name));
                    PpCtlCount++;
                }
                RegCloseKey(k);
            }
        }
    }
}
#endif

/* IOCTL_SCSI_MINIPORT; the data follows the header in PpBuf (prepared by the caller) */
static BOOL PpMiniportSig(HANDLE h, const char *sig, ULONG code, ULONG length)
{
    PSRB_IO_CONTROL c = (PSRB_IO_CONTROL)PpBuf;
    DWORD n;
    int i;
    c->HeaderLength = sizeof(SRB_IO_CONTROL);
    for (i = 0; i < 8; i++) c->Signature[i] = (UCHAR)sig[i];
    c->Timeout = 10;
    c->ControlCode = code;
    c->Length = length;
    if (!DeviceIoControl(h, IOCTL_SCSI_MINIPORT, PpBuf, sizeof(SRB_IO_CONTROL) + length,
                         PpBuf, sizeof(SRB_IO_CONTROL) + length, &n, NULL)) {
        return FALSE;
    }
    return c->ReturnCode == 0;
}

static BOOL PpMiniport(HANDLE h, ULONG code, ULONG length)
{
    return PpMiniportSig(h, PP_SIGNATURE, code, length);
}

#if defined(PP_AHCI) || defined(PP_IDE)
/* Vendor and product of the device at path/target/LUN 0, from ScsiPort's inquiry data */
static UCHAR PpInquiry[8192];
static ULONG PpInquiryLength;

static void PpReadInquiry(HANDLE h)
{
    DWORD n = 0;
    PpInquiryLength = 0;
    if (DeviceIoControl(h, IOCTL_SCSI_GET_INQUIRY_DATA, NULL, 0, PpInquiry, sizeof(PpInquiry), &n, NULL)) {
        PpInquiryLength = n;
    }
}

static BOOL PpDeviceName(ULONG path, ULONG target, char *out)
{
    PSCSI_ADAPTER_BUS_INFO info = (PSCSI_ADAPTER_BUS_INFO)PpInquiry;
    ULONG b;
    out[0] = 0;
    if (PpInquiryLength < sizeof(SCSI_ADAPTER_BUS_INFO)) return FALSE;
    for (b = 0; b < info->NumberOfBuses; b++) {
        ULONG off = info->BusData[b].InquiryDataOffset;
        while (off != 0 && off + sizeof(SCSI_INQUIRY_DATA) <= PpInquiryLength) {
            PSCSI_INQUIRY_DATA d = (PSCSI_INQUIRY_DATA)(PpInquiry + off);
            if (d->PathId == path && d->TargetId == target && d->Lun == 0 && d->InquiryDataLength >= 32) {
                /* vendor and product as one string: an ATA model split into
                   the two fields reads as it was */
                int i, n = 0;
                for (i = 8; i < 32; i++) {
                    char c = (char)d->InquiryData[i];
                    if (c < ' ' || c > '~') c = ' ';
                    if (c == ' ' && (n == 0 || out[n - 1] == ' ')) continue;
                    out[n++] = c;
                }
                while (n > 0 && out[n - 1] == ' ') n--;
                out[n] = 0;
                if ((d->InquiryData[0] & 0x1F) == 5) lstrcpyA(out + n, " (CD/DVD)");
                return TRUE;
            }
            off = d->NextInquiryDataOffset;
        }
    }
    return FALSE;
}
#endif

/* IDENT: the PIO= items for this controller onto the device rows (a
   channel item covers both of its devices) */
static void PpPioFromText(PP_PAGE *pg, const char *params)
{
#if defined(PP_IDE)
    static PP_ITEM items[PP_MAX_ITEMS];
    char rest[PP_PARAM_LEN], channel[24];
    int n, i, k;
    n = PpSplitKey(params, "PIO=", items, PP_MAX_ITEMS, rest, sizeof(rest));
    for (i = 0; i < pg->NumRows; i++) {
        PP_ROW *r = &pg->Rows[i];
        r->Pio = FALSE;
        for (k = 0; r->Item[k] && r->Item[k] != '.'; k++) channel[k] = r->Item[k];
        channel[k] = 0;
        if (r->Item[k] != '.') continue;                    /* channel rows show no mode */
        r->Pio = PpIsOff(items, n, &pg->Self, r->Item) || PpIsOff(items, n, &pg->Self, channel);
    }
#else
    (void)pg; (void)params;
#endif
}

#if defined(PP_IDE) || defined(PP_EXE)
static BOOL PpIsDeviceRow(const PP_ROW *r)
{
    int k;
    for (k = 0; r->Item[k]; k++) if (r->Item[k] == '.') return TRUE;
    return FALSE;
}
#endif

static PP_ROW *PpRow(PP_PAGE *pg, const char *item, const char *label)
{
    PP_ROW *r;
    int i;
    for (i = 0; i < pg->NumRows; i++) {
        if (lstrcmpA(pg->Rows[i].Item, item) == 0) return &pg->Rows[i];
    }
    if (pg->NumRows >= PP_MAX_ROWS) return NULL;
    r = &pg->Rows[pg->NumRows++];
    lstrcpyA(r->Item, item);
    lstrcpyA(r->Label, label);
    r->Desc[0] = 0;
    r->On = TRUE;
    r->Pio = FALSE;
    return r;
}

static ULONG PpNumber(const char *s, const char **end)
{
    ULONG n = 0;
    while (PpIsDigit(*s)) n = n * 10 + (ULONG)(*s++ - '0');
    if (end) *end = s;
    return n;
}

/* Sorts the rows by their item numbers ("1" < "1.0" < "2") */
static int PpCompare(const char *a, const char *b)
{
    for (;;) {
        ULONG x, y;
        if (!*a || !*b) return (*a != 0) - (*b != 0);
        x = PpNumber(a, &a);
        y = PpNumber(b, &b);
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
}

static void PpSortRows(PP_PAGE *pg)
{
    int i, j;
    for (i = 1; i < pg->NumRows; i++) {
        PP_ROW t = pg->Rows[i];
        for (j = i; j > 0 && PpCompare(pg->Rows[j - 1].Item, t.Item) > 0; j--) pg->Rows[j] = pg->Rows[j - 1];
        pg->Rows[j] = t;
    }
}

/* What the controller has, and the rows for what OFF= names */
static void PpCollect(PP_PAGE *pg)
{
    static PP_ITEM items[PP_MAX_ITEMS];
    char rest[PP_PARAM_LEN], item[24], label[48];
    int n, i;
    HANDLE h = PpOpenAdapter(pg);

    pg->NumRows = 0;
    pg->Opened = h != INVALID_HANDLE_VALUE;
    n = PpSplit(pg->Params, items, PP_MAX_ITEMS, rest, sizeof(rest));

#if defined(PP_AHCI)
    if (pg->Opened) {
        ULONG port, pi;
        UCHAR *info = PpBuf + sizeof(SRB_IO_CONTROL);
        PpReadInquiry(h);
        memset(PpBuf, 0, sizeof(PpBuf));
        if (PpMiniport(h, 1, 20 + 32)) {
            pi = PpGet32(info + 8);
            for (port = 0; port < 32; port++) {
                UCHAR count = port < 8 ? info[20 + port] : info[28 + port - 8];
                PP_ROW *r;
                if (!(pi & (1u << port))) continue;
                wsprintfA(item, "%lu", port);
                wsprintfA(label, "Port %lu", port);
                r = PpRow(pg, item, label);
                if (r == NULL) break;
                if (count > 1) wsprintfA(r->Desc, "Port multiplier, %u devices", count);
                else if (!PpDeviceName(port / 8, port % 8, r->Desc)) lstrcpyA(r->Desc, "(no device)");
            }
        }
    }
    for (i = 0; i < n; i++) {
        const char *e;
        ULONG p;
        if (!(!items[i].Pci || PpSameLoc(&items[i], &pg->Self))) continue;
        p = PpNumber(items[i].Body, &e);
        if (*e == '.') wsprintfA(label, "Port %lu, PM port %lu", p, PpNumber(e + 1, NULL));
        else wsprintfA(label, "Port %lu", p);
        PpRow(pg, items[i].Body, label);
    }
#elif defined(PP_IDE)
    if (pg->Opened) {
        UCHAR *info = PpBuf + sizeof(SRB_IO_CONTROL);
        ULONG c, u, channels = 2;
        PpReadInquiry(h);
        memset(PpBuf, 0, sizeof(PpBuf));
        /* without a device on the controller ScsiPort may not pass the
           request on: then both channels, as on every PCI IDE function */
        if (PpMiniport(h, 1, 64)) channels = PpGet32(info + 8);
        else memset(info, 0, 64);
        {
            if (channels > 2) channels = 2;
            for (c = 0; c < channels; c++) {
                ULONG cmd = info[16 + c * 2] | (info[17 + c * 2] << 8);
                PP_ROW *r;
                wsprintfA(item, "%lu", c);
                wsprintfA(label, "%s channel", c == 0 ? "Primary" : "Secondary");
                r = PpRow(pg, item, label);
                if (r == NULL) break;
                if (cmd) wsprintfA(r->Desc, "Ports %lXh", cmd);
                for (u = 0; u < 2; u++) {
                    wsprintfA(item, "%lu.%lu", c, u);
                    wsprintfA(label, "    %s", u == 0 ? "Master" : "Slave");
                    r = PpRow(pg, item, label);
                    if (r == NULL) break;
                    if (!PpDeviceName(0, c * 2 + u, r->Desc)) lstrcpyA(r->Desc, "(no device)");
                }
            }
        }
    }
    for (i = 0; i < n; i++) {
        const char *e;
        ULONG c;
        if (!(!items[i].Pci || PpSameLoc(&items[i], &pg->Self))) continue;
        c = PpNumber(items[i].Body, &e);
        if (*e == '.') wsprintfA(label, "    %s (channel %lu)", PpNumber(e + 1, NULL) ? "Slave" : "Master", c);
        else wsprintfA(label, "%s channel", c == 0 ? "Primary" : c == 1 ? "Secondary" : "Other");
        PpRow(pg, items[i].Body, label);
    }
#elif defined(PP_NVME)
    if (pg->Opened) {
        /* NvmeMini pass-through: Identify Controller (NN), the active
           namespace list (CNS 02h, NVMe 1.1+) and every namespace */
        UCHAR *pt = PpBuf + sizeof(SRB_IO_CONTROL);
        UCHAR *data = pt + 124;
        ULONG *cmd = (ULONG *)(pt + 24);
        static ULONG nsids[PP_MAX_ROWS];
        ULONG nn = 0, count = 0, k;
#define PP_NVME_IDENTIFY(nsid, cns) \
        (memset(PpBuf, 0, sizeof(PpBuf)), cmd[0] = 0x06, cmd[1] = (nsid), cmd[10] = (cns), \
         *(ULONG *)(pt + 104) = 2, *(ULONG *)(pt + 112) = 4096, *(ULONG *)(pt + 120) = 124 + 4096, \
         PpMiniport(h, 0, 124 + 4096))
        if (PP_NVME_IDENTIFY(0, 1)) nn = PpGet32(data + 516);
        if (PP_NVME_IDENTIFY(0, 2)) {
            for (k = 0; k < 1024 && count < PP_MAX_ROWS; k++) {
                ULONG id = PpGet32(data + k * 4);
                if (id == 0) break;
                nsids[count++] = id;
            }
        }
        if (count == 0) {
            for (k = 1; k <= nn && count < 32; k++) nsids[count++] = k;
        }
        for (k = 0; k < count; k++) {
            PP_ROW *r;
            wsprintfA(item, "%lu", nsids[k]);
            wsprintfA(label, "Namespace %lu", nsids[k]);
            r = PpRow(pg, item, label);
            if (r == NULL) break;
            if (PP_NVME_IDENTIFY(nsids[k], 0)) {
                ULONGLONG blocks = (ULONGLONG)PpGet32(data) | ((ULONGLONG)PpGet32(data + 4) << 32);
                UCHAR fmt = (UCHAR)(data[26] & 0x0F);
                UCHAR lbads = data[128 + fmt * 4 + 2];
                char size[32];
                if (blocks == 0 || lbads < 9 || lbads > 16) {
                    lstrcpyA(r->Desc, "(not usable)");
                } else {
                    PpFormatSize(size, blocks << lbads);
                    wsprintfA(r->Desc, "%s, %lu-byte blocks", size, 1ul << lbads);
                }
            }
        }
    }
    for (i = 0; i < n; i++) {
        if (!(!items[i].Pci || PpSameLoc(&items[i], &pg->Self))) continue;
        wsprintfA(label, "Namespace %s", items[i].Body);
        PpRow(pg, items[i].Body, label);
    }
#elif defined(PP_MMC)
    if (pg->Opened) {
        UCHAR *info = PpBuf + sizeof(SRB_IO_CONTROL);
        ULONG s, slots = 1;
        memset(PpBuf, 0, sizeof(PpBuf));
        /* without a card ScsiPort may not pass the request on: then slot 0 */
        if (PpMiniport(h, 1, 28 + 6 * 76)) slots = PpGet32(info + 8);
        else memset(info, 0, 28 + 6 * 76);
        {
            static const char *types[] = { "", "SD card", "SDHC/SDXC card", "MMC card", "MMC/eMMC" };
            if (slots > 6) slots = 6;
            for (s = 0; s < slots; s++) {
                UCHAR *sl = info + 28 + s * 76;
                PP_ROW *r;
                wsprintfA(item, "%lu", s);
                wsprintfA(label, "Slot %lu", s);
                r = PpRow(pg, item, label);
                if (r == NULL) break;
                if (sl[0] >= 1 && sl[0] <= 4) {
                    char size[32];
                    ULONGLONG sectors = (ULONGLONG)PpGet32(sl + 12) | ((ULONGLONG)PpGet32(sl + 16) << 32);
                    PpFormatSize(size, sectors * 512);
                    wsprintfA(r->Desc, "%s, %s", types[sl[0]], size);
                } else {
                    lstrcpyA(r->Desc, "(no card)");
                }
            }
        }
    }
    for (i = 0; i < n; i++) {
        if (!(!items[i].Pci || PpSameLoc(&items[i], &pg->Self))) continue;
        wsprintfA(label, "Slot %s", items[i].Body);
        PpRow(pg, items[i].Body, label);
    }
#endif

#ifdef PP_EXE
    /* 9x (or a controller that does not answer): the usual items */
    if (!pg->Opened) {
#if defined(PP_AHCI)
        ULONG k;
        for (k = 0; k < 6; k++) {
            wsprintfA(item, "%lu", k);
            wsprintfA(label, "Port %lu", k);
            PpRow(pg, item, label);
        }
#elif defined(PP_IDE)
        PpRow(pg, "0", "Primary channel");
        PpRow(pg, "0.0", "    Master");
        PpRow(pg, "0.1", "    Slave");
        PpRow(pg, "1", "Secondary channel");
        PpRow(pg, "1.0", "    Master");
        PpRow(pg, "1.1", "    Slave");
#elif defined(PP_NVME)
        PpRow(pg, "1", "Namespace 1");
#else
        PpRow(pg, "0", "Slot 0");
        PpRow(pg, "1", "Slot 1");
#endif
    }
#endif

    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    PpSortRows(pg);
    PpPioFromText(pg, pg->Params);
    for (i = 0; i < pg->NumRows; i++) {
        pg->Rows[i].On = !PpIsOff(items, n, &pg->Self, pg->Rows[i].Item);
        if (!pg->Rows[i].On && (pg->Rows[i].Desc[0] == 0 || lstrcmpA(pg->Rows[i].Desc, "(no device)") == 0 ||
                                  lstrcmpA(pg->Rows[i].Desc, "(no card)") == 0)) {
            lstrcpyA(pg->Rows[i].Desc, "(turned off)");
        }
    }
}

/* ------------------------------------------------------------------ */
/* Registry                                                            */
/* ------------------------------------------------------------------ */

#define PP_KEY "SYSTEM\\CurrentControlSet\\Services\\" PP_SERVICE "\\Parameters\\Device"

/* Where the parameter string is: DriverParameters of the service (NT
   family), or AdapterSettings of the controller's driver key (9x) */
static char PpKeyPath[160] = PP_KEY;
static const char *PpValueName = "DriverParameters";

static void PpReadParams(char *out, int size)
{
    HKEY k;
    DWORD type, n = (DWORD)size - 1;
    out[0] = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, PpKeyPath, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    if (RegQueryValueExA(k, PpValueName, NULL, &type, (LPBYTE)out, &n) != ERROR_SUCCESS || type != REG_SZ) n = 0;
    out[n < (DWORD)size ? n : (DWORD)size - 1] = 0;
    RegCloseKey(k);
}

static LONG PpWriteParams(const char *params)
{
    HKEY k;
    LONG r = RegCreateKeyExA(HKEY_LOCAL_MACHINE, PpKeyPath, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &k, NULL);
    if (r != ERROR_SUCCESS) return r;
    if (params[0]) r = RegSetValueExA(k, PpValueName, 0, REG_SZ, (const BYTE *)params, lstrlenA(params) + 1);
    else {
        r = RegDeleteValueA(k, PpValueName);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    }
    RegCloseKey(k);
    return r;
}

/* The parameter string for the rows as they are now */
static void PpParamsFromRows(PP_PAGE *pg, const char *current, char *out, int size)
{
    static char off[PP_MAX_ROWS][24];
    int i, n = 0;
    for (i = 0; i < pg->NumRows; i++) {
        if (!pg->Rows[i].On) lstrcpyA(off[n++], pg->Rows[i].Item);
    }
    PpRebuildKey("OFF=", current, &pg->Self, pg->Others, pg->NumOthers, off, n, out, size);
#if defined(PP_IDE)
    {
        static char pio[PP_MAX_ROWS][24];
        char tmp[PP_PARAM_LEN];
        lstrcpyA(tmp, out);
        n = 0;
        for (i = 0; i < pg->NumRows; i++) {
            if (pg->Rows[i].Pio && PpIsDeviceRow(&pg->Rows[i])) lstrcpyA(pio[n++], pg->Rows[i].Item);
        }
        PpRebuildKey("PIO=", tmp, &pg->Self, pg->Others, pg->NumOthers, pio, n, out, size);
    }
#endif
}

/* Scripted tests: "item,item,..." turned off, "pitem" PIO only, "-" none */
static void PpApplyTestItems(PP_PAGE *pg, const char *p)
{
    int k;
    for (k = 0; k < pg->NumRows; k++) {
        pg->Rows[k].On = TRUE;
        pg->Rows[k].Pio = FALSE;
    }
    while (*p && *p != '-') {
        char body[24];
        int n = 0;
        BOOL pio = FALSE;
        if (*p == 'p' || *p == 'P') { pio = TRUE; p++; }
        while (*p && *p != ',' && *p != '+' && *p != ' ' && n < 23) body[n++] = *p++;
        body[n] = 0;
        for (k = 0; k < pg->NumRows; k++) {
            if (lstrcmpA(pg->Rows[k].Item, body) != 0) continue;
            if (pio) pg->Rows[k].Pio = TRUE;
            else pg->Rows[k].On = FALSE;
        }
        if (*p == ',' || *p == '+') p++;
        else break;
    }
}

static const char *PpModeText(const PP_ROW *r)
{
#if defined(PP_IDE)
    if (PpIsDeviceRow(r)) return r->Pio ? "PIO only" : "DMA";
#else
    (void)r;
#endif
    return "";
}

#ifndef PP_EXE
static void PpInit(PP_PAGE *pg)
{
    PpLocation(pg->Set, &pg->Dev, &pg->Self);
    PpFindOthers(pg);
    PpReadParams(pg->Params, sizeof(pg->Params));
    PpCollect(pg);
}
#endif

/* ------------------------------------------------------------------ */
/* Page                                                                */
/* ------------------------------------------------------------------ */

#ifndef PP_EXE
static void PpFillList(PP_PAGE *pg)
{
    LVITEMA it;
    int i;
    pg->Updating = TRUE;
    SendMessageA(pg->List, LVM_DELETEALLITEMS, 0, 0);
    for (i = 0; i < pg->NumRows; i++) {
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = pg->Rows[i].Label;
        SendMessageA(pg->List, LVM_INSERTITEMA, 0, (LPARAM)&it);
        it.iSubItem = 1;
        it.pszText = pg->Rows[i].Desc;
        SendMessageA(pg->List, LVM_SETITEMTEXTA, i, (LPARAM)&it);
#if defined(PP_IDE)
        it.iSubItem = 2;
        it.pszText = (LPSTR)PpModeText(&pg->Rows[i]);
        SendMessageA(pg->List, LVM_SETITEMTEXTA, i, (LPARAM)&it);
#endif
        it.mask = LVIF_STATE;
        it.iSubItem = 0;
        it.stateMask = LVIS_STATEIMAGEMASK;
        it.state = INDEXTOSTATEIMAGEMASK(pg->Rows[i].On ? 2 : 1);
        SendMessageA(pg->List, LVM_SETITEMSTATE, i, (LPARAM)&it);
    }
    pg->Updating = FALSE;
}

/* Check boxes from the text in the edit box */
static void PpRowsFromText(PP_PAGE *pg)
{
    static PP_ITEM items[PP_MAX_ITEMS];
    char rest[PP_PARAM_LEN];
    int n, i;
    GetWindowTextA(pg->Edit, pg->Params, sizeof(pg->Params));
    n = PpSplit(pg->Params, items, PP_MAX_ITEMS, rest, sizeof(rest));
    pg->Updating = TRUE;
    PpPioFromText(pg, pg->Params);
    for (i = 0; i < pg->NumRows; i++) {
        LVITEMA it;
        BOOL on = !PpIsOff(items, n, &pg->Self, pg->Rows[i].Item);
#if defined(PP_IDE)
        memset(&it, 0, sizeof(it));
        it.iSubItem = 2;
        it.pszText = (LPSTR)PpModeText(&pg->Rows[i]);
        SendMessageA(pg->List, LVM_SETITEMTEXTA, i, (LPARAM)&it);
#endif
        if (on == pg->Rows[i].On) continue;
        pg->Rows[i].On = on;
        memset(&it, 0, sizeof(it));
        it.stateMask = LVIS_STATEIMAGEMASK;
        it.state = INDEXTOSTATEIMAGEMASK(on ? 2 : 1);
        SendMessageA(pg->List, LVM_SETITEMSTATE, i, (LPARAM)&it);
    }
    pg->Updating = FALSE;
}

#endif

static HWND PpControl(HWND dlg, const char *cls, const char *text, DWORD style, DWORD exStyle,
                      int x, int y, int cx, int cy, int id)
{
    RECT rc;
    HWND w;
    rc.left = x; rc.top = y; rc.right = x + cx; rc.bottom = y + cy;
    MapDialogRect(dlg, &rc);
    w = CreateWindowExA(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, rc.left, rc.top,
                        rc.right - rc.left, rc.bottom - rc.top, dlg, (HMENU)(INT_PTR)id, PpInstance, NULL);
    SendMessageA(w, WM_SETFONT, SendMessageA(dlg, WM_GETFONT, 0, 0), 0);
    SetWindowPos(w, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);   /* tab order = creation order */
    return w;
}

static const char PpNote[] =
#ifdef PP_EXE
    "Select a line and press Turn on / off (or double-click it); [X] is on. "
#endif
#if defined(PP_AHCI)
    "Clear a port to turn it off: AHCINT then leaves it alone (no reset, no commands, "
    "its interrupts disabled) and its disk disappears."
#elif defined(PP_IDE)
    "Clear a channel or a device to turn it off: IDENT then leaves it alone (no reset, "
    "no commands) and its disk or drive disappears. DMA / PIO switches a device to PIO "
    "only (PIO=)."
#elif defined(PP_NVME)
    "Clear a namespace to leave it out: NVMENT then presents no disk for it; the other "
    "namespaces move up."
#else
    "Clear a slot to turn it off: MMCSDNT then sets up no card in it and disables its "
    "interrupts."
#endif
#ifdef PP_EXE
    " The change is stored as OFF= in the driver parameters below and takes effect when "
    "Windows is started the next time.";
#else
    " The change is stored as OFF= in the driver parameters below and takes effect when "
    "the controller restarts (for the controller of the system disk: at the next boot).";
#endif

#ifndef PP_EXE
static INT_PTR CALLBACK PpDialog(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    PP_PAGE *pg = (PP_PAGE *)GetWindowLongPtrA(dlg, DWLP_USER);

    switch (msg) {
    case WM_INITDIALOG: {
        LVCOLUMNA col;
        RECT rc;
        pg = (PP_PAGE *)((LPPROPSHEETPAGEA)lp)->lParam;
        SetWindowLongPtrA(dlg, DWLP_USER, (LONG_PTR)pg);
        pg->Updating = TRUE;        /* the edit box reports EN_CHANGE while it is created */
        PpControl(dlg, "STATIC", PpNote, 0, 0, 7, 7, 238, 40, IDC_NOTE);
        pg->List = PpControl(dlg, WC_LISTVIEWA, "", LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER |
                             LVS_SHOWSELALWAYS | WS_TABSTOP, WS_EX_CLIENTEDGE, 7, 50, 238, 118, IDC_LIST);
        SendMessageA(pg->List, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
        GetClientRect(pg->List, &rc);
        memset(&col, 0, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = (LPSTR)PP_COLUMN;
        col.cx = (rc.right - rc.left) * 2 / 5;
        SendMessageA(pg->List, LVM_INSERTCOLUMNA, 0, (LPARAM)&col);
        col.pszText = (LPSTR)(pg->Opened ? "Device" : "Device (controller not running)");
        col.cx = (rc.right - rc.left) - col.cx - GetSystemMetrics(SM_CXVSCROLL);
#if defined(PP_IDE)
        col.cx -= (rc.right - rc.left) / 6;
#endif
        SendMessageA(pg->List, LVM_INSERTCOLUMNA, 1, (LPARAM)&col);
#if defined(PP_IDE)
        col.pszText = (LPSTR)"Mode";
        col.cx = (rc.right - rc.left) / 6;
        SendMessageA(pg->List, LVM_INSERTCOLUMNA, 2, (LPARAM)&col);
        PpControl(dlg, "BUTTON", "&DMA / PIO", BS_PUSHBUTTON | WS_TABSTOP, 0, 7, 171, 60, 14, IDC_PIO);
#define PP_PARAMS_Y 190
#else
#define PP_PARAMS_Y 174
#endif
        PpControl(dlg, "STATIC", "Driver parameters (" PP_SERVICE ", all controllers):", 0, 0,
                  7, PP_PARAMS_Y, 238, 9, IDC_PLABEL);
        pg->Edit = PpControl(dlg, "EDIT", pg->Params, ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE,
                             7, PP_PARAMS_Y + 11, 238, 13, IDC_PARAMS);
        SendMessageA(pg->Edit, EM_LIMITTEXT, PP_PARAM_LEN - 1, 0);
        PpFillList(pg);
        return TRUE;
    }
    case WM_COMMAND:
        if (pg && LOWORD(wp) == IDC_PARAMS && HIWORD(wp) == EN_CHANGE && !pg->Updating) {
            PpRowsFromText(pg);
            PropSheet_Changed(GetParent(dlg), dlg);
        }
#if defined(PP_IDE)
        if (pg && LOWORD(wp) == IDC_PIO && HIWORD(wp) == BN_CLICKED) {
            int i = (int)SendMessageA(pg->List, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
            if (i >= 0 && i < pg->NumRows && PpIsDeviceRow(&pg->Rows[i])) {
                char text[PP_PARAM_LEN];
                LVITEMA it;
                pg->Rows[i].Pio = !pg->Rows[i].Pio;
                memset(&it, 0, sizeof(it));
                it.iSubItem = 2;
                it.pszText = (LPSTR)PpModeText(&pg->Rows[i]);
                SendMessageA(pg->List, LVM_SETITEMTEXTA, i, (LPARAM)&it);
                GetWindowTextA(pg->Edit, pg->Params, sizeof(pg->Params));
                PpParamsFromRows(pg, pg->Params, text, sizeof(text));
                pg->Updating = TRUE;
                SetWindowTextA(pg->Edit, text);
                pg->Updating = FALSE;
                PropSheet_Changed(GetParent(dlg), dlg);
            }
        }
#endif
        break;
    case WM_NOTIFY: {
        LPNMHDR h = (LPNMHDR)lp;
        if (pg == NULL) break;
        if (h->idFrom == IDC_LIST && h->code == LVN_ITEMCHANGED && !pg->Updating) {
            LPNMLISTVIEW lv = (LPNMLISTVIEW)lp;
            if ((lv->uChanged & LVIF_STATE) && ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK) &&
                lv->iItem >= 0 && lv->iItem < pg->NumRows) {
                char text[PP_PARAM_LEN];
                pg->Rows[lv->iItem].On = ((lv->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2;
                GetWindowTextA(pg->Edit, pg->Params, sizeof(pg->Params));
                PpParamsFromRows(pg, pg->Params, text, sizeof(text));
                pg->Updating = TRUE;
                SetWindowTextA(pg->Edit, text);
                pg->Updating = FALSE;
                PropSheet_Changed(GetParent(dlg), dlg);
            }
        } else if (h->code == PSN_APPLY) {
            char text[PP_PARAM_LEN], old[PP_PARAM_LEN];
            LONG r;
            GetWindowTextA(pg->Edit, text, sizeof(text));
            PpReadParams(old, sizeof(old));
            if (lstrcmpA(old, text) != 0) {
                r = PpWriteParams(text);
                if (r != ERROR_SUCCESS) {
                    char m[160];
                    wsprintfA(m, "The driver parameters could not be written (error %ld).", r);
                    MessageBoxA(dlg, m, PP_SERVICE, MB_OK | MB_ICONERROR);
                    SetWindowLongPtrA(dlg, DWLP_MSGRESULT, PSNRET_INVALID_NOCHANGEPAGE);
                    return TRUE;
                }
                {
                    SP_DEVINSTALL_PARAMS_A ip;
                    ip.cbSize = sizeof(ip);
                    if (SetupDiGetDeviceInstallParamsA(pg->Set, &pg->Dev, &ip)) {
                        ip.FlagsEx |= DI_FLAGSEX_PROPCHANGE_PENDING;
                        SetupDiSetDeviceInstallParamsA(pg->Set, &pg->Dev, &ip);
                    }
                }
            }
            SetWindowLongPtrA(dlg, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

static UINT CALLBACK PpPageCallback(HWND w, UINT msg, LPPROPSHEETPAGEA psp)
{
    (void)w;
    if (msg == PSPCB_RELEASE && psp->lParam) HeapFree(GetProcessHeap(), 0, (PVOID)psp->lParam);
    return 1;
}

/* Empty page template; the controls are created in WM_INITDIALOG. Not
   const: comctl32 adjusts the style of the template in place */
#ifdef _MSC_VER
#define PP_ALIGN4 __declspec(align(4))
#else
#define PP_ALIGN4 __attribute__((aligned(4)))
#endif
static PP_ALIGN4 WORD PpTemplate[] = {
    /* DLGTEMPLATE: style (DS_SETFONT | DS_3DLOOK | WS_CHILD | WS_CAPTION | WS_DISABLED), exstyle */
    0x0044, 0x48C0, 0x0000, 0x0000,
    0,                      /* cdit */
    0, 0, 252, 218,         /* x, y, cx, cy */
    0, 0, 0,                /* menu, class, title */
    8,                      /* point size */
    'M', 'S', ' ', 'S', 'h', 'e', 'l', 'l', ' ', 'D', 'l', 'g', 0
};

/* Device Manager: advanced property pages of the controller */
__declspec(dllexport) BOOL WINAPI PortPageProvider(PSP_PROPSHEETPAGE_REQUEST req, LPFNADDPROPSHEETPAGE add, LPARAM lp)
{
    PROPSHEETPAGEA psp;
    HPROPSHEETPAGE hp;
    PP_PAGE *pg;
    INITCOMMONCONTROLSEX icc;

    if (req == NULL || req->cbSize < sizeof(SP_PROPSHEETPAGE_REQUEST) ||
        req->PageRequested != SPPSR_ENUM_ADV_DEVICE_PROPERTIES || req->DeviceInfoData == NULL) {
        return FALSE;
    }
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    pg = (PP_PAGE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PP_PAGE));
    if (pg == NULL) return FALSE;
    pg->Set = req->DeviceInfoSet;
    pg->Dev = *req->DeviceInfoData;
    PpInit(pg);

    memset(&psp, 0, sizeof(psp));
    psp.dwSize = PROPSHEETPAGEA_V1_SIZE;
    psp.dwFlags = PSP_DLGINDIRECT | PSP_USETITLE | PSP_USECALLBACK;
    psp.hInstance = PpInstance;
    psp.pResource = (LPCDLGTEMPLATE)PpTemplate;
    psp.pszTitle = PP_TITLE;
    psp.pfnDlgProc = PpDialog;
    psp.lParam = (LPARAM)pg;
    psp.pfnCallback = PpPageCallback;
    hp = CreatePropertySheetPageA(&psp);
    if (hp == NULL) {
        HeapFree(GetProcessHeap(), 0, pg);
        return FALSE;
    }
    if (!add(hp, lp)) {
        DestroyPropertySheetPage(hp);
        return FALSE;
    }
    return TRUE;
}

/*
 * rundll32 <dll>,PortPageTest <n> <out file> [items]
 * Controller n of the driver (0 = first present one): writes the rows and
 * the parameter string to the file. With items (OFF= bodies separated
 * by commas or plus signs, a "p" in front for PIO only (IDENT), or "-"
 * for none) the rows are set to exactly those and the resulting string is
 * written to the registry.
 */
static void PpOut(HANDLE f, const char *s)
{
    DWORD n;
    WriteFile(f, s, lstrlenA(s), &n, NULL);
}

__declspec(dllexport) void CALLBACK PortPageTest(HWND w, HINSTANCE inst, LPSTR cmd, int show)
{
    static PP_PAGE pg;
    HDEVINFO set;
    SP_DEVINFO_DATA d;
    DWORD i;
    ULONG index;
    const char *p = cmd, *e;
    char file[MAX_PATH], line[512];
    HANDLE f;
    int found = -1, k;

    (void)w; (void)inst; (void)show;
    while (*p == ' ') p++;
    index = PpNumber(p, &e);
    p = e;
    while (*p == ' ') p++;
    for (k = 0; *p && *p != ' ' && k < MAX_PATH - 1; k++) file[k] = *p++;
    file[k] = 0;
    while (*p == ' ') p++;

    f = CreateFileA(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    set = SetupDiGetClassDevsA(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    for (i = 0; set != INVALID_HANDLE_VALUE; i++) {
        d.cbSize = sizeof(d);
        if (!SetupDiEnumDeviceInfo(set, i, &d)) break;
        if (!PpIsOurs(set, &d)) continue;
        if (++found == (int)index) break;
    }
    if (set == INVALID_HANDLE_VALUE || found != (int)index) {
        PpOut(f, "no controller\r\n");
        CloseHandle(f);
        return;
    }
    memset(&pg, 0, sizeof(pg));
    pg.Set = set;
    pg.Dev = d;
    PpInit(&pg);
    wsprintfA(line, "controller %lu: %s %lu.%lu.%lu, adapter %s, %d other(s)\r\nparams [%s]\r\n", index,
              pg.Self.Pci ? "PCI" : "no PCI location", pg.Self.Bus, pg.Self.Dev, pg.Self.Fn,
              pg.Opened ? "open" : "NOT OPEN", pg.NumOthers, pg.Params);
    PpOut(f, line);
    for (k = 0; k < pg.NumRows; k++) {
        wsprintfA(line, "[%c] %-6s %s | %s %s\r\n", pg.Rows[k].On ? 'x' : ' ', pg.Rows[k].Item,
                  pg.Rows[k].Label, pg.Rows[k].Desc, PpModeText(&pg.Rows[k]));
        PpOut(f, line);
    }
    if (*p) {
        char text[PP_PARAM_LEN];
        PpApplyTestItems(&pg, p);
        PpParamsFromRows(&pg, pg.Params, text, sizeof(text));
        wsprintfA(line, "new [%s] write %ld\r\n", text, PpWriteParams(text));
        PpOut(f, line);
    }
    CloseHandle(f);
    SetupDiDestroyDeviceInfoList(set);
}

BOOL WINAPI DllMainCRTStartup(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        PpInstance = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}
#endif

#ifdef PP_EXE
/* ------------------------------------------------------------------ */
/* Program (PP_EXE)                                                    */
/* ------------------------------------------------------------------ */

#define IDC_CTL         110
#define IDC_TOGGLE      111
#define IDC_SAVE        112

static PP_PAGE PpPg;
static int PpSel = -1;
static BOOL PpDirty;

static void PpInitExe(PP_PAGE *pg, int index)
{
    HWND list = pg->List, edit = pg->Edit;
    int i;
    memset(pg, 0, sizeof(*pg));
    pg->List = list;
    pg->Edit = edit;
    pg->Port = PpCtl[index].Port;
    pg->Self = PpCtl[index].Loc;
    for (i = 0; i < PpCtlCount; i++) {
        if (i != index && pg->NumOthers < PP_MAX_OTHERS) pg->Others[pg->NumOthers++] = PpCtl[i].Loc;
    }
    if (Pp9x) {
        lstrcpyA(PpKeyPath, PpCtl[index].Key);
        PpValueName = "AdapterSettings";
    }
    PpReadParams(pg->Params, sizeof(pg->Params));
    PpCollect(pg);
}

static void PpListText(const PP_ROW *r, char *out)
{
    const char *mode = PpModeText(r);
    wsprintfA(out, mode[0] ? "[%c]  %s\t%s  (%s)" : "[%c]  %s\t%s", r->On ? 'X' : ' ', r->Label, r->Desc, mode);
}

static void PpExeFill(HWND dlg)
{
    char line[256];
    int i, sel = (int)SendMessageA(PpPg.List, LB_GETCURSEL, 0, 0);
    SendMessageA(PpPg.List, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < PpPg.NumRows; i++) {
        PpListText(&PpPg.Rows[i], line);
        SendMessageA(PpPg.List, LB_ADDSTRING, 0, (LPARAM)line);
    }
    if (sel >= 0 && sel < PpPg.NumRows) SendMessageA(PpPg.List, LB_SETCURSEL, sel, 0);
    (void)dlg;
}

static void PpExeSelect(HWND dlg, int index)
{
    PpSel = index;
    PpInitExe(&PpPg, index);
    PpPg.Updating = TRUE;
    SetWindowTextA(PpPg.Edit, PpPg.Params);
    PpPg.Updating = FALSE;
    PpExeFill(dlg);
    PpDirty = FALSE;
}

/* Check marks from the text in the edit box */
static void PpExeRowsFromText(HWND dlg)
{
    static PP_ITEM items[PP_MAX_ITEMS];
    char rest[PP_PARAM_LEN];
    int n, i;
    GetWindowTextA(PpPg.Edit, PpPg.Params, sizeof(PpPg.Params));
    n = PpSplit(PpPg.Params, items, PP_MAX_ITEMS, rest, sizeof(rest));
    for (i = 0; i < PpPg.NumRows; i++) PpPg.Rows[i].On = !PpIsOff(items, n, &PpPg.Self, PpPg.Rows[i].Item);
    PpPioFromText(&PpPg, PpPg.Params);
    PpExeFill(dlg);
}

static void PpExeToggle(HWND dlg, BOOL Pio)
{
    char text[PP_PARAM_LEN];
    int i = (int)SendMessageA(PpPg.List, LB_GETCURSEL, 0, 0);
    if (i < 0 || i >= PpPg.NumRows) return;
    if (Pio) {
        if (!PpIsDeviceRow(&PpPg.Rows[i])) return;
        PpPg.Rows[i].Pio = !PpPg.Rows[i].Pio;
    } else {
        PpPg.Rows[i].On = !PpPg.Rows[i].On;
    }
    GetWindowTextA(PpPg.Edit, PpPg.Params, sizeof(PpPg.Params));
    PpParamsFromRows(&PpPg, PpPg.Params, text, sizeof(text));
    PpPg.Updating = TRUE;
    SetWindowTextA(PpPg.Edit, text);
    PpPg.Updating = FALSE;
    PpExeFill(dlg);
    PpDirty = TRUE;
}

static BOOL PpExeSave(HWND dlg)
{
    char text[PP_PARAM_LEN], m[200];
    LONG r;
    GetWindowTextA(PpPg.Edit, text, sizeof(text));
    r = PpWriteParams(text);
    if (r != ERROR_SUCCESS) {
        wsprintfA(m, "The driver parameters could not be written (error %ld).", r);
        MessageBoxA(dlg, m, PP_SERVICE, MB_OK | MB_ICONSTOP);
        return FALSE;
    }
    PpDirty = FALSE;
    MessageBoxA(dlg, "Stored. The change takes effect when Windows is restarted.", PP_SERVICE, MB_OK | MB_ICONINFORMATION);
    return TRUE;
}

static BOOL PpExeKeep(HWND dlg)
{
    int r;
    if (!PpDirty) return TRUE;
    r = MessageBoxA(dlg, "Store the changed driver parameters?", PP_SERVICE, MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return FALSE;
    if (r == IDYES) return PpExeSave(dlg);
    return TRUE;
}

static BOOL CALLBACK PpExeDialog(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        HWND combo;
        int tabs[1], i;
        char label[96];
        PpPg.Updating = TRUE;
        PpControl(dlg, "STATIC", PpNote, 0, 0, 7, 7, 270, 44, IDC_NOTE);
        PpControl(dlg, "STATIC", "Controller:", 0, 0, 7, 54, 44, 9, -1);
        combo = PpControl(dlg, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, 52, 52, 225, 80, IDC_CTL);
        for (i = 0; i < PpCtlCount; i++) SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)PpCtl[i].Name);
        PpPg.List = PpControl(dlg, "LISTBOX", "", LBS_NOTIFY | LBS_USETABSTOPS | WS_VSCROLL | WS_BORDER | WS_TABSTOP,
                              0, 7, 70, 270, 100, IDC_LIST);
        tabs[0] = 110;
        SendMessageA(PpPg.List, LB_SETTABSTOPS, 1, (LPARAM)tabs);
        PpControl(dlg, "BUTTON", "&Turn on / off", BS_PUSHBUTTON | WS_TABSTOP, 0, 7, 172, 70, 14, IDC_TOGGLE);
#if defined(PP_IDE)
        PpControl(dlg, "BUTTON", "&DMA / PIO", BS_PUSHBUTTON | WS_TABSTOP, 0, 82, 172, 70, 14, IDC_PIO);
#endif
        wsprintfA(label, "Driver parameters (%s):", Pp9x ? "AdapterSettings of this controller"
                                                         : PP_SERVICE ", all controllers");
        PpControl(dlg, "STATIC", label, 0, 0, 7, 192, 270, 9, IDC_PLABEL);
        PpPg.Edit = PpControl(dlg, "EDIT", "", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 0, 7, 203, 270, 13, IDC_PARAMS);
        SendMessageA(PpPg.Edit, EM_LIMITTEXT, PP_PARAM_LEN - 1, 0);
        PpControl(dlg, "BUTTON", "&Store", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, 166, 222, 52, 14, IDC_SAVE);
        PpControl(dlg, "BUTTON", "Close", BS_PUSHBUTTON | WS_TABSTOP, 0, 225, 222, 52, 14, IDCANCEL);
        PpPg.Updating = FALSE;
        if (PpCtlCount > 0) {
            SendMessageA(combo, CB_SETCURSEL, 0, 0);
            PpExeSelect(dlg, 0);
        } else {
            SendMessageA(PpPg.List, LB_ADDSTRING, 0, (LPARAM)"No " PP_SERVICE " controller was found.");
            EnableWindow(PpPg.Edit, FALSE);
        }
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_CTL:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int i = (int)SendMessageA((HWND)lp, CB_GETCURSEL, 0, 0);
                if (i != PpSel && i >= 0) {
                    if (!PpExeKeep(dlg)) SendMessageA((HWND)lp, CB_SETCURSEL, PpSel, 0);
                    else PpExeSelect(dlg, i);
                }
            }
            break;
        case IDC_LIST:
            if (HIWORD(wp) == LBN_DBLCLK && PpSel >= 0) PpExeToggle(dlg, FALSE);
            break;
        case IDC_TOGGLE:
            if (PpSel >= 0) PpExeToggle(dlg, FALSE);
            break;
        case IDC_PIO:
            if (PpSel >= 0) PpExeToggle(dlg, TRUE);
            break;
        case IDC_PARAMS:
            if (HIWORD(wp) == EN_CHANGE && !PpPg.Updating && PpSel >= 0) {
                PpExeRowsFromText(dlg);
                PpDirty = TRUE;
            }
            break;
        case IDC_SAVE:
            if (PpSel >= 0) PpExeSave(dlg);
            break;
        case IDCANCEL:
            if (PpSel < 0 || PpExeKeep(dlg)) EndDialog(dlg, 0);
            break;
        }
        return TRUE;
    }
    return FALSE;
}

/* The dialog: no controls in the template, MS Sans Serif (every system
   from NT 3.1 and 95 has it) */
#ifdef _MSC_VER
#define PP_ALIGN4 __declspec(align(4))
#else
#define PP_ALIGN4 __attribute__((aligned(4)))
#endif
static PP_ALIGN4 WORD PpExeTemplate[] = {
    /* DS_SETFONT | DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE */
    0x00C0, 0x90C8, 0x0000, 0x0000,
    0,
    0, 0, 284, 244,
    0, 0,
    'P', 'o', 'r', 't', 's', 0,
    8,
    'M', 'S', ' ', 'S', 'a', 'n', 's', ' ', 'S', 'e', 'r', 'i', 'f', 0
};

static void PpOut(HANDLE f, const char *s)
{
    DWORD n;
    WriteFile(f, s, lstrlenA(s), &n, NULL);
}

#if defined(PP_NVME)
/* One admin command through NvmeMini: Command[] as given, data of Len
   bytes after the structure, Dir 0 none, 1 to the controller, 2 from it.
   The NVMe status (0 = success) is returned, FFFFh when the driver did
   not take the command. */
static ULONG PpNvmeAdmin(HANDLE h, ULONG opc, ULONG nsid, ULONG cdw10, ULONG cdw11, ULONG dir, ULONG len, ULONG *dw0)
{
    UCHAR *pt = PpBuf + sizeof(SRB_IO_CONTROL);
    ULONG *cmd = (ULONG *)(pt + 24);
    ULONG st;
    cmd[0] = opc;
    cmd[1] = nsid;
    cmd[10] = cdw10;
    cmd[11] = cdw11;
    *(ULONG *)(pt + 104) = dir;
    *(ULONG *)(pt + 112) = len;
    *(ULONG *)(pt + 120) = 124 + len;
    *(ULONG *)(pt + 100) = 0;
    st = PpMiniport(h, 0, 124 + 4096) ? 0 : 0xFFFF;
    if (PpGet32(pt + 88 + 12) != 0) st = (PpGet32(pt + 88 + 12) >> 17) & 0x7FFF;
    if (dw0 != NULL) *dw0 = PpGet32(pt + 88);
    return st;
}

static void PpNvmeClear(void)
{
    memset(PpBuf, 0, sizeof(SRB_IO_CONTROL) + 124 + 4096);
}

/* <exe> /ns <n> <out file> list | create <MB> [format] | delete <nsid> |
   attach <nsid> | detach <nsid> | format <nsid> [format]
   Namespace administration of NVMe controller n (the driver needs
   ADMIN=1 for all but list) */
static void PpExeNs(const char *p)
{
    const char *e;
    char file[MAX_PATH], line[256], act[16], size[32];
    UCHAR *data = PpBuf + sizeof(SRB_IO_CONTROL) + 124;
    HANDLE f, h;
    ULONG index, arg1 = 0, arg2 = 0, st, dw0 = 0, k, cntlid, oacs, nn;
    int n;
    BOOL has2;

    while (*p == ' ') p++;
    index = PpNumber(p, &e);
    p = e;
    while (*p == ' ') p++;
    for (n = 0; *p && *p != ' ' && n < MAX_PATH - 1; n++) file[n] = *p++;
    file[n] = 0;
    while (*p == ' ') p++;
    for (n = 0; *p && *p != ' ' && n < 15; n++) act[n] = *p++;
    act[n] = 0;
    while (*p == ' ') p++;
    arg1 = PpNumber(p, &e);
    p = e;
    while (*p == ' ') p++;
    has2 = (BOOL)(*p >= '0' && *p <= '9');
    if (has2) arg2 = PpNumber(p, NULL);
    f = CreateFileA(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    if ((int)index >= PpCtlCount) {
        PpOut(f, "no controller\r\n");
        CloseHandle(f);
        return;
    }
    PpInitExe(&PpPg, (int)index);
    h = PpOpenAdapter(&PpPg);
    if (h == INVALID_HANDLE_VALUE) {
        PpOut(f, "controller not open\r\n");
        CloseHandle(f);
        return;
    }
    PpNvmeClear();
    if (PpNvmeAdmin(h, 0x06, 0, 1, 0, 2, 4096, NULL) != 0) {
        PpOut(f, "Identify Controller failed\r\n");
        CloseHandle(h);
        CloseHandle(f);
        return;
    }
    cntlid = data[78] | (data[79] << 8);
    oacs = data[256] | (data[257] << 8);
    nn = PpGet32(data + 516);
    if (lstrcmpiA(act, "list") == 0) {
        static ULONG ids[1024];
        ULONG count = 0, active[64], na = 0;
        PpFormatSize(size, (ULONGLONG)PpGet32(data + 280) | ((ULONGLONG)PpGet32(data + 284) << 32));
        wsprintfA(line, "controller %lu, %lu namespace(s) at most, namespace management %s, capacity %s",
                  cntlid, nn, (oacs & 8) ? "yes" : "no", size);
        PpOut(f, line);
        PpFormatSize(size, (ULONGLONG)PpGet32(data + 296) | ((ULONGLONG)PpGet32(data + 300) << 32));
        wsprintfA(line, ", unallocated %s\r\n", size);
        PpOut(f, line);
        PpNvmeClear();
        if (PpNvmeAdmin(h, 0x06, 0, 2, 0, 2, 4096, NULL) == 0) {
            for (k = 0; k < 64 && PpGet32(data + k * 4) != 0; k++) active[na++] = PpGet32(data + k * 4);
        }
        PpNvmeClear();
        if ((oacs & 8) && PpNvmeAdmin(h, 0x06, 0, 0x10, 0, 2, 4096, NULL) == 0) {
            for (k = 0; k < 1024 && PpGet32(data + k * 4) != 0; k++) ids[count++] = PpGet32(data + k * 4);
        } else {
            for (k = 0; k < na; k++) ids[count++] = active[k];
        }
        for (k = 0; k < count; k++) {
            ULONG a, j;
            UCHAR fmt, lbads;
            ULONGLONG blocks;
            for (a = 0, j = 0; j < na; j++) if (active[j] == ids[k]) a = 1;
            PpNvmeClear();
            if (PpNvmeAdmin(h, 0x06, ids[k], a ? 0 : 0x11, 0, 2, 4096, NULL) != 0) {
                wsprintfA(line, "namespace %lu: no data\r\n", ids[k]);
            } else {
                blocks = (ULONGLONG)PpGet32(data) | ((ULONGLONG)PpGet32(data + 4) << 32);
                fmt = (UCHAR)(data[26] & 0x0F);
                lbads = data[128 + fmt * 4 + 2];
                PpFormatSize(size, blocks << lbads);
                wsprintfA(line, "namespace %lu: %s, format %lu (%lu-byte blocks, %lu metadata), %s\r\n", ids[k], size,
                          (ULONG)fmt, 1ul << lbads, (ULONG)(data[128 + fmt * 4] | (data[129 + fmt * 4] << 8)),
                          a ? "attached" : "not attached");
            }
            PpOut(f, line);
        }
    } else if (lstrcmpiA(act, "create") == 0) {
        UCHAR fmt = (UCHAR)(has2 ? arg2 : 0), lbads;
        ULONGLONG blocks;
        PpNvmeClear();
        /* block size of the format: from the common namespace data, or a namespace's */
        if (PpNvmeAdmin(h, 0x06, 0xFFFFFFFF, 0, 0, 2, 4096, NULL) != 0) {
            PpNvmeClear();
            PpNvmeAdmin(h, 0x06, 1, 0, 0, 2, 4096, NULL);
        }
        lbads = data[128 + fmt * 4 + 2];
        if (lbads < 9 || lbads > 16) lbads = 9;
        blocks = ((ULONGLONG)arg1 << 20) >> lbads;
        PpNvmeClear();
        *(ULONG *)(data + 0) = (ULONG)blocks;
        *(ULONG *)(data + 4) = (ULONG)(blocks >> 32);
        *(ULONG *)(data + 8) = (ULONG)blocks;
        *(ULONG *)(data + 12) = (ULONG)(blocks >> 32);
        data[26] = fmt;
        st = PpNvmeAdmin(h, 0x0D, 0, 0, 0, 1, 4096, &dw0);
        if (st == 0) wsprintfA(line, "created namespace %lu (%lu MB, format %lu)\r\n", dw0, arg1, (ULONG)fmt);
        else wsprintfA(line, "create failed, status %04lX\r\n", st);
        PpOut(f, line);
    } else if (lstrcmpiA(act, "delete") == 0 || lstrcmpiA(act, "attach") == 0 || lstrcmpiA(act, "detach") == 0 ||
               lstrcmpiA(act, "format") == 0) {
        PpNvmeClear();
        if (lstrcmpiA(act, "delete") == 0) {
            st = PpNvmeAdmin(h, 0x0D, arg1, 1, 0, 0, 0, NULL);
        } else if (lstrcmpiA(act, "format") == 0) {
            st = PpNvmeAdmin(h, 0x80, arg1, has2 ? (arg2 & 0x0F) : 0, 0, 0, 0, NULL);
        } else {
            data[0] = 1;
            data[2] = (UCHAR)cntlid;
            data[3] = (UCHAR)(cntlid >> 8);
            st = PpNvmeAdmin(h, 0x15, arg1, lstrcmpiA(act, "detach") == 0 ? 1 : 0, 0, 1, 4096, NULL);
        }
        if (st == 0) wsprintfA(line, "%s namespace %lu: done\r\n", act, arg1);
        else if (st == 0xFFFF) wsprintfA(line, "%s namespace %lu: not taken by the driver (ADMIN=1 needed)\r\n", act, arg1);
        else wsprintfA(line, "%s namespace %lu: failed, status %04lX\r\n", act, arg1, st);
        PpOut(f, line);
    } else {
        PpOut(f, "list | create <MB> [format] | delete <nsid> | attach <nsid> | detach <nsid> | format <nsid> [format]\r\n");
    }
    CloseHandle(h);
    CloseHandle(f);
}
#endif

/* <exe> /t <n> <out file> [items]: as PortPageTest */
static void PpExeTest(const char *p)
{
    const char *e;
    char file[MAX_PATH], line[512];
    HANDLE f;
    ULONG index;
    int k;

    while (*p == ' ') p++;
    index = PpNumber(p, &e);
    p = e;
    while (*p == ' ') p++;
    for (k = 0; *p && *p != ' ' && k < MAX_PATH - 1; k++) file[k] = *p++;
    file[k] = 0;
    while (*p == ' ') p++;
    f = CreateFileA(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    for (k = 0; k < PpCtlCount; k++) {
        wsprintfA(line, "%d: %s\r\n", k, PpCtl[k].Name);
        PpOut(f, line);
    }
    if ((int)index >= PpCtlCount) {
        PpOut(f, "no controller\r\n");
        CloseHandle(f);
        return;
    }
    PpInitExe(&PpPg, (int)index);
    wsprintfA(line, "controller %lu: %s %lu.%lu.%lu, adapter %s, %d other(s)\r\nparams [%s]\r\n", index,
              PpPg.Self.Pci ? "PCI" : "no PCI location", PpPg.Self.Bus, PpPg.Self.Dev, PpPg.Self.Fn,
              PpPg.Opened ? "open" : "NOT OPEN", PpPg.NumOthers, PpPg.Params);
    PpOut(f, line);
    for (k = 0; k < PpPg.NumRows; k++) {
        wsprintfA(line, "[%c] %-6s %s | %s %s\r\n", PpPg.Rows[k].On ? 'x' : ' ', PpPg.Rows[k].Item,
                  PpPg.Rows[k].Label, PpPg.Rows[k].Desc, PpModeText(&PpPg.Rows[k]));
        PpOut(f, line);
    }
    if (*p) {
        char text[PP_PARAM_LEN];
        PpApplyTestItems(&PpPg, p);
        PpParamsFromRows(&PpPg, PpPg.Params, text, sizeof(text));
        wsprintfA(line, "new [%s] write %ld\r\n", text, PpWriteParams(text));
        PpOut(f, line);
    }
    CloseHandle(f);
}

void PpStart(void)
{
    const char *cl = GetCommandLineA();
    PpInstance = GetModuleHandleA(NULL);
    if (*cl == '"') {
        cl++;
        while (*cl && *cl != '"') cl++;
        if (*cl) cl++;
    } else {
        while (*cl && *cl != ' ') cl++;
    }
    while (*cl == ' ') cl++;
    PpFindControllers();
#if defined(PP_NVME)
    if ((cl[0] == '/' || cl[0] == '-') && (cl[1] == 'n' || cl[1] == 'N') && (cl[2] == 's' || cl[2] == 'S')) {
        PpExeNs(cl + 3);
    } else
#endif
    if ((cl[0] == '/' || cl[0] == '-') && (cl[1] == 't' || cl[1] == 'T')) {
        PpExeTest(cl + 2);
    } else {
        DialogBoxIndirectParamA(PpInstance, (LPCDLGTEMPLATEA)PpExeTemplate, NULL, (DLGPROC)PpExeDialog, 0);
    }
    ExitProcess(0);
}
#endif
