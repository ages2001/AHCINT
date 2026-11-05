/* trimnt <drive letter> : TRIM the free space of a volume on an AHCINT,
   IDENT, MMCSDNT or NVMENT disk (Windows NT 3.1 - XP x64).

   Windows before 7 sends no TRIM/UNMAP. The drivers have a TRIM mode: while
   it is on, a write whose data is a given 4 KB pattern, repeated, is not
   written but trimmed (UNMAP / DATA SET MANAGEMENT / eMMC DISCARD). trimnt
   switches the mode on with a random pattern on every adapter of these
   drivers, fills the free space of the volume with files of that pattern
   (unbuffered, so every write reaches the disk), deletes the files and
   switches the mode off again. Build: no C runtime, kernel32 and user32. */
#include <windows.h>

#define IOCTL_SCSI_MINIPORT 0x4D008
#define CHUNK               (1024 * 1024)
#define FILE_LIMIT          1024            /* MB per file (FAT32: < 4 GB) */
#define MAX_FILES           4096

typedef struct { ULONG HeaderLength; UCHAR Signature[8]; ULONG Timeout, ControlCode, ReturnCode, Length; } SIC;
typedef struct { const char *Sig; ULONG On, Off; } DRV;
static const DRV g_Drivers[] = {
    { "AHCINT  ", 0x20, 0x21 }, { "IDENT   ", 0x20, 0x21 },
    { "MMCSDNT ", 0x20, 0x21 }, { "NVMENT  ", 0x1001, 0x1002 },
};
static HANDLE g_Out;
static HANDLE g_Port[32];
static int g_Drv[32];
static int g_Ports;
static volatile LONG g_Stop;

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
    return h->ReturnCode == 0;
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

void start(void)
{
    char *cl = GetCommandLineA(), root[4], name[32], path[MAX_PATH];
    static char files[MAX_FILES][16];
    BYTE *pattern, *chunk;
    ULONG seed, i, written, mb, nfiles = 0, tail = 0;
    HANDLE f, port;
    DWORD n;
    int d, ok;

    g_Out = GetStdHandle(STD_OUTPUT_HANDLE);
    while (*cl && *cl != ' ') cl++;
    while (*cl == ' ') cl++;
    if (!((*cl >= 'A' && *cl <= 'Z') || (*cl >= 'a' && *cl <= 'z'))) {
        w("usage: trimnt <drive letter>   (trims the free space of the volume)\r\n");
        ExitProcess(2);
    }
    root[0] = *cl; root[1] = ':'; root[2] = '\\'; root[3] = 0;

    pattern = (BYTE *)VirtualAlloc(0, 4096, MEM_COMMIT, PAGE_READWRITE);
    chunk = (BYTE *)VirtualAlloc(0, CHUNK, MEM_COMMIT, PAGE_READWRITE);
    if (pattern == NULL || chunk == NULL) ExitProcess(3);
    seed = GetTickCount() ^ (ULONG)(ULONG_PTR)pattern;
    for (i = 0; i < 4096; i++) {
        seed = seed * 1103515245UL + 12345UL;
        pattern[i] = (BYTE)(seed >> 16);
    }
    for (i = 0; i < CHUNK; i++) chunk[i] = pattern[i & 4095];

    /* TRIM mode on, on every adapter of these drivers */
    for (i = 0; i < 32; i++) {
        wsprintfA(name, "\\\\.\\Scsi%lu:", i);
        port = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
        if (port == INVALID_HANDLE_VALUE) continue;
        ok = 0;
        for (d = 0; d < (int)(sizeof(g_Drivers) / sizeof(g_Drivers[0])) && !ok; d++) {
            if (Miniport(port, &g_Drivers[d], g_Drivers[d].On, pattern, 4096)) {
                g_Port[g_Ports] = port;
                g_Drv[g_Ports] = d;
                g_Ports++;
                ok = 1;
                w("Scsi"); wd(i); w(": "); w(g_Drivers[d].Sig); w(" TRIM mode on\r\n");
            }
        }
        if (!ok) CloseHandle(port);
    }
    if (g_Ports == 0) {
        w("no AHCINT / IDENT / MMCSDNT / NVMENT adapter with TRIM found\r\n");
        ExitProcess(1);
    }

    /* Fill the free space, unbuffered: every write reaches the driver */
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    mb = 0;
    for (;;) {
        if (nfiles >= MAX_FILES || g_Stop) break;
        wsprintfA(files[nfiles], "TRIM%04lu.TMP", nfiles);
        lstrcpyA(path, root);
        lstrcatA(path, files[nfiles]);
        f = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, 0);
        if (f == INVALID_HANDLE_VALUE) break;
        nfiles++;
        for (written = 0; written < FILE_LIMIT && !g_Stop; written++) {
            if (!WriteFile(f, chunk, CHUNK, &n, 0) || n != CHUNK) break;
            mb++;
            if ((mb & 63) == 0) { w("  "); wd(mb); w(" MB\r\n"); }
        }
        if (written < FILE_LIMIT && !g_Stop) {
            /* The rest of the volume in 4 KB pieces (whole clusters) */
            SetFilePointer(f, (LONG)(written * CHUNK), NULL, FILE_BEGIN);
            SetEndOfFile(f);
            while (WriteFile(f, chunk, 4096, &n, 0) && n == 4096) tail += 4096;
            w("  stopped: error "); wd(GetLastError()); w("\r\n");
        }
        CloseHandle(f);
        if (written < FILE_LIMIT) break;    /* volume full */
    }
    w("trimmed "); wd(mb + tail / CHUNK); w(" MB in "); wd(nfiles); w(" file(s)\r\n");

    ModeOff();
    for (i = 0; i < nfiles; i++) {
        lstrcpyA(path, root);
        lstrcatA(path, files[i]);
        DeleteFileA(path);
    }
    w("TRIM mode off, files deleted\r\n");
    ExitProcess(0);
}
