/* trimnt <drive letter> : TRIM the free space of a volume on an AHCINT,
   IDENT, MMCSDNT or NVMENT disk (Windows NT 3.1 - XP x64).

   Windows before 7 sends no TRIM/UNMAP. The drivers have a TRIM mode: while
   it is on, a write whose data is a given pattern, repeated, is not written
   but trimmed (DATA SET MANAGEMENT TRIM / Dataset Management deallocate /
   SD-eMMC discard or erase). trimnt finds the disk of the volume, switches
   the mode on with a random pattern on its adapter, fills the free space
   of the volume with files of that pattern (unbuffered, so every write
   reaches the disk), asks the driver how much it trimmed, deletes the
   files and switches the mode off again.

   The pattern is 512 random bytes repeated: a file system splits a write
   where the free space is fragmented, and every piece that starts on a
   sector boundary is still the pattern from its first byte.

   A full volume ends the fill: "disk full" (error 112) is the normal end,
   not an error. Build: no C runtime, kernel32 and user32 (wsprintfA). */
#include <windows.h>

#define IOCTL_SCSI_MINIPORT     0x4D008
#define IOCTL_SCSI_GET_ADDRESS  0x41018
#define CHUNK                   (1024 * 1024)
#define FILE_LIMIT              1024            /* MB per file (FAT32: < 4 GB) */
#define MAX_FILES               65536
#define TRIM_DIR                "TRIMNT.TMP"

typedef struct { ULONG HeaderLength; UCHAR Signature[8]; ULONG Timeout, ControlCode, ReturnCode, Length; } SIC;
typedef struct { ULONG Length; UCHAR PortNumber, PathId, TargetId, Lun; } SCSIADDR;
/* TRIM_STATUS of all four drivers */
typedef struct {
    UCHAR PathId, TargetId, Lun, Flags;
    ULONG BlockSize;
    ULONG TrimmedLow, TrimmedHigh;
    ULONG WrittenLow, WrittenHigh;
} TRIMST;
#define ST_PRESENT  0x01
#define ST_CAN_TRIM 0x02

typedef struct { const char *Sig; ULONG On, Off, Status; } DRV;
static const DRV g_Drivers[] = {
    { "AHCINT  ", 0x20, 0x21, 0x22 }, { "IDENT   ", 0x20, 0x21, 0x22 },
    { "MMCSDNT ", 0x20, 0x21, 0x22 }, { "NVMENT  ", 0x1001, 0x1002, 0x1004 },
};
#define NDRV (int)(sizeof(g_Drivers) / sizeof(g_Drivers[0]))

static HANDLE g_Out;
static int g_Console;                   /* progress only on a console, not into a file */
static HANDLE g_Port[32];
static int g_Drv[32];
static int g_Ports;
static volatile LONG g_Stop;
static char g_Root[4];

/* Ctrl+C / Ctrl+Break: stop filling; the mode is switched off and the
   files deleted as after a full volume */
static BOOL WINAPI CtrlHandler(DWORD Type)
{
    (void)Type;
    g_Stop = 1;
    return TRUE;
}

static void w(const char *s) { DWORD n; WriteFile(g_Out, s, lstrlenA(s), &n, 0); }
static void wd(DWORD v) { char b[16]; wsprintfA(b, "%lu", v); w(b); }
/* A driver signature without its padding */
static void ws(const char *Sig) { char b[9]; int i; for (i = 0; i < 8 && Sig[i] != ' '; i++) b[i] = Sig[i]; b[i] = 0; w(b); }
/* A 64-bit byte count as MB */
static DWORD Mb(ULONG Low, ULONG High) { return (High << 12) | (Low >> 20); }

static int Miniport(HANDLE Port, const DRV *Drv, ULONG Code, BYTE *Data, ULONG Length)
{
    static BYTE buf[sizeof(SIC) + 4096];
    SIC *h = (SIC *)buf;
    DWORD n;
    ULONG i;

    for (i = 0; i < sizeof(buf); i++) buf[i] = 0;
    h->HeaderLength = sizeof(SIC);
    for (i = 0; i < 8; i++) h->Signature[i] = (UCHAR)Drv->Sig[i];
    h->Timeout = 30;
    h->ControlCode = Code;
    h->Length = Length;
    for (i = 0; i < Length; i++) buf[sizeof(SIC) + i] = Data[i];
    if (!DeviceIoControl(Port, IOCTL_SCSI_MINIPORT, buf, sizeof(SIC) + Length, buf, sizeof(SIC) + Length, &n, 0)) return 0;
    if (h->ReturnCode != 0) return 0;
    for (i = 0; i < Length; i++) Data[i] = buf[sizeof(SIC) + i];
    return 1;
}

/* TRIM_STATUS of the disk at Addr; 0 when the driver has none (before
   AHCINT 1.8 / IDENT 1.3 / MMCSDNT 1.3 / NVMENT 3.3) */
static int Status(HANDLE Port, const DRV *Drv, const SCSIADDR *Addr, TRIMST *St)
{
    St->PathId = Addr->PathId;
    St->TargetId = Addr->TargetId;
    St->Lun = Addr->Lun;
    St->Flags = 0;
    return Miniport(Port, Drv, Drv->Status, (BYTE *)St, sizeof(TRIMST));
}

static void ModeOff(void)
{
    int i;
    for (i = 0; i < g_Ports; i++) {
        Miniport(g_Port[i], &g_Drivers[g_Drv[i]], g_Drivers[g_Drv[i]].Off, NULL, 0);
        CloseHandle(g_Port[i]);
    }
    g_Ports = 0;
}

static HANDLE OpenPort(ULONG Number)
{
    char name[32];
    wsprintfA(name, "\\\\.\\Scsi%lu:", Number);
    return CreateFileA(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
}

/* The SCSI address of the disk the volume is on (disk class driver, through
   the file system); 0 when it cannot be found (e.g. a fault-tolerant set) */
static int VolumeAddress(SCSIADDR *Addr)
{
    char name[8];
    HANDLE v;
    DWORD n;
    int ok;

    wsprintfA(name, "\\\\.\\%c:", g_Root[0]);
    v = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
    if (v == INVALID_HANDLE_VALUE) v = CreateFileA(name, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
    if (v == INVALID_HANDLE_VALUE) return 0;
    Addr->Length = sizeof(SCSIADDR);
    ok = DeviceIoControl(v, IOCTL_SCSI_GET_ADDRESS, NULL, 0, Addr, sizeof(SCSIADDR), &n, 0) && n >= 8;
    CloseHandle(v);
    return ok;
}

static void FileName(char *Path, ULONG Index, int InDir)
{
    lstrcpyA(Path, g_Root);
    if (InDir) lstrcatA(Path, TRIM_DIR "\\");
    /* 8.3 names: FAT on NT 3.1 has no long names (0 to FFFF covers MAX_FILES) */
    wsprintfA(Path + lstrlenA(Path), "TRIM%04lX.TMP", Index);
}

void start(void)
{
    char *cl = GetCommandLineA(), path[MAX_PATH], fs[32];
    BYTE *pattern, *chunk;
    ULONG seed, i, written, mb = 0, nfiles = 0, tail = 0, piece;
    DWORD spc, bps, freeClusters, totalClusters, n, err = 0;
    SCSIADDR addr;
    TRIMST st;
    HANDLE f, port;
    int d, ok, haveAddr, haveStatus = 0, inDir, full = 0;

    g_Out = GetStdHandle(STD_OUTPUT_HANDLE);
    g_Console = (GetFileType(g_Out) == FILE_TYPE_CHAR);
    while (*cl && *cl != ' ') cl++;
    while (*cl == ' ') cl++;
    if (!((*cl >= 'A' && *cl <= 'Z') || (*cl >= 'a' && *cl <= 'z'))) {
        w("usage: trimnt <drive letter>   (trims the free space of the volume)\r\n");
        ExitProcess(2);
    }
    g_Root[0] = (char)(*cl & ~0x20); g_Root[1] = ':'; g_Root[2] = '\\'; g_Root[3] = 0;

    if (!GetDiskFreeSpaceA(g_Root, &spc, &bps, &freeClusters, &totalClusters)) {
        w(g_Root); w(" cannot be read: error "); wd(GetLastError()); w("\r\n");
        ExitProcess(1);
    }
    fs[0] = 0;
    GetVolumeInformationA(g_Root, NULL, 0, NULL, NULL, NULL, fs, sizeof(fs));
    piece = spc * bps;                  /* the tail is filled cluster by cluster */
    if (piece == 0 || piece > CHUNK || (piece & (bps - 1)) != 0) piece = 4096;
    w(g_Root); w(" "); w(fs); w(", ");
    wd((DWORD)(((ULONGLONG)freeClusters * piece) >> 20)); w(" MB free, cluster "); wd(piece); w(" bytes\r\n");

    pattern = (BYTE *)VirtualAlloc(0, 4096, MEM_COMMIT, PAGE_READWRITE);
    chunk = (BYTE *)VirtualAlloc(0, CHUNK, MEM_COMMIT, PAGE_READWRITE);
    if (pattern == NULL || chunk == NULL) ExitProcess(3);
    seed = GetTickCount() ^ (ULONG)(ULONG_PTR)pattern ^ (GetCurrentProcessId() << 16);
    for (i = 0; i < 512; i++) {
        seed = seed * 1103515245UL + 12345UL;
        pattern[i] = (BYTE)(seed >> 16);
    }
    for (i = 512; i < 4096; i++) pattern[i] = pattern[i & 511];
    for (i = 0; i < CHUNK; i++) chunk[i] = pattern[i & 4095];

    /* TRIM mode on: on the adapter of the volume's disk, or on every
       adapter of these drivers when the disk cannot be found */
    haveAddr = VolumeAddress(&addr);
    if (haveAddr) {
        port = OpenPort(addr.PortNumber);
        if (port == INVALID_HANDLE_VALUE) haveAddr = 0;
    }
    if (haveAddr) {
        w(g_Root); w(" is on Scsi"); wd(addr.PortNumber); w(": path "); wd(addr.PathId);
        w(" target "); wd(addr.TargetId); w(" lun "); wd(addr.Lun); w("\r\n");
        for (d = 0; d < NDRV; d++) {
            if (Status(port, &g_Drivers[d], &addr, &st)) {
                haveStatus = 1;
                break;
            }
        }
        if (haveStatus) {
            if (!(st.Flags & ST_PRESENT)) {
                w("no disk of "); ws(g_Drivers[d].Sig); w(" at that address\r\n");
                ExitProcess(1);
            }
            if (!(st.Flags & ST_CAN_TRIM)) {
                w("the disk takes no TRIM through "); ws(g_Drivers[d].Sig);
                w(" (no TRIM support, TRIM=0, or IDENT in PIO mode): nothing to do\r\n");
                ExitProcess(1);
            }
        } else {
            for (d = 0; d < NDRV; d++) {
                if (Miniport(port, &g_Drivers[d], g_Drivers[d].On, pattern, 4096)) break;
            }
            if (d == NDRV) {
                w("Scsi"); wd(addr.PortNumber); w(": is no AHCINT / IDENT / MMCSDNT / NVMENT adapter with TRIM\r\n");
                ExitProcess(1);
            }
            Miniport(port, &g_Drivers[d], g_Drivers[d].Off, NULL, 0);
        }
        if (!Miniport(port, &g_Drivers[d], g_Drivers[d].On, pattern, 4096)) {
            w("TRIM mode could not be switched on\r\n");
            ExitProcess(1);
        }
        g_Port[0] = port;
        g_Drv[0] = d;
        g_Ports = 1;
        w("Scsi"); wd(addr.PortNumber); w(": "); ws(g_Drivers[d].Sig); w(" TRIM mode on\r\n");
    } else {
        w("the disk of "); w(g_Root); w(" was not found: TRIM mode on every adapter\r\n");
        for (i = 0; i < 32; i++) {
            port = OpenPort(i);
            if (port == INVALID_HANDLE_VALUE) continue;
            ok = 0;
            for (d = 0; d < NDRV && !ok; d++) {
                if (Miniport(port, &g_Drivers[d], g_Drivers[d].On, pattern, 4096)) {
                    g_Port[g_Ports] = port;
                    g_Drv[g_Ports] = d;
                    g_Ports++;
                    ok = 1;
                    w("Scsi"); wd(i); w(": "); ws(g_Drivers[d].Sig); w(" TRIM mode on\r\n");
                }
            }
            if (!ok) CloseHandle(port);
        }
        if (g_Ports == 0) {
            w("no AHCINT / IDENT / MMCSDNT / NVMENT adapter with TRIM found\r\n");
            ExitProcess(1);
        }
    }

    /* Fill the free space, unbuffered: every write reaches the driver. The
       files go in a directory (the FAT12/16 root has room for few). */
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    lstrcpyA(path, g_Root);
    lstrcatA(path, TRIM_DIR);
    inDir = CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
    for (;;) {
        if (nfiles >= MAX_FILES || g_Stop) break;
        FileName(path, nfiles, inDir);
        f = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, 0);
        if (f == INVALID_HANDLE_VALUE) {
            err = GetLastError();
            full = (err == ERROR_DISK_FULL || err == ERROR_HANDLE_DISK_FULL);
            break;
        }
        nfiles++;
        for (written = 0; written < FILE_LIMIT && !g_Stop; written++) {
            if (!WriteFile(f, chunk, CHUNK, &n, 0) || n != CHUNK) break;
            mb++;
            if ((mb & 255) == 0 && g_Console) { w("  "); wd(mb); w(" MB\r"); }
        }
        if (written < FILE_LIMIT && !g_Stop) {
            /* The rest of the volume cluster by cluster */
            SetFilePointer(f, (LONG)(written * CHUNK), NULL, FILE_BEGIN);
            SetEndOfFile(f);
            while (!g_Stop && WriteFile(f, chunk, piece, &n, 0) && n == piece) tail += piece;
            err = GetLastError();
            full = (err == ERROR_DISK_FULL || err == ERROR_HANDLE_DISK_FULL);
        }
        CloseHandle(f);
        if (written < FILE_LIMIT) break;
    }
    mb += tail >> 20;
    if (mb >= 256 && g_Console) w("                \r");
    if (g_Stop) w("stopped by Ctrl+C\r\n");
    else if (full) w("volume full\r\n");
    else if (nfiles >= MAX_FILES) w("file limit reached\r\n");
    else if (err != 0) { w("write error "); wd(err); w("\r\n"); }
    w("filled "); wd(mb); w(" MB in "); wd(nfiles); w(" file(s)\r\n");

    /* What the driver did with it */
    if (haveStatus && Status(g_Port[0], &g_Drivers[g_Drv[0]], &addr, &st)) {
        w("trimmed "); wd(Mb(st.TrimmedLow, st.TrimmedHigh)); w(" MB");
        if (st.WrittenLow != 0 || st.WrittenHigh != 0) {
            w(", "); wd(Mb(st.WrittenLow, st.WrittenHigh)); w(" MB written as data");
        }
        w("\r\n");
    }

    ModeOff();
    for (i = 0; i < nfiles; i++) {
        FileName(path, i, inDir);
        DeleteFileA(path);
    }
    if (inDir) {
        lstrcpyA(path, g_Root);
        lstrcatA(path, TRIM_DIR);
        RemoveDirectoryA(path);
    }
    w("TRIM mode off, files deleted\r\n");
    ExitProcess((full || g_Stop || err == 0) ? 0 : 1);
}
