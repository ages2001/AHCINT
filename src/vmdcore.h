/*
 * Intel Volume Management Device (VMD) domain access, shared by AHCINT and
 * NVMENT (the same file in both). Included by one source file of a driver,
 * after the miniport headers (the types alone come with the driver's own
 * header). The driver fills VMD_DOMAIN's callbacks:
 *   Map        map physical memory (ScsiPortGetDeviceBase, or VMM on 9x)
 *   HostRead   a dword of the VMD's own configuration space
 *   HostWrite
 *   Match      TRUE for a function of the domain the driver takes
 * After Linux drivers/pci/controller/vmd.c.
 */

#ifndef VMD_CORE_TYPES
#define VMD_CORE_TYPES

#define VMD_REG_VMCAP                   0x40
#define VMD_REG_VMCONFIG                0x44
#define VMD_REG_VMLOCK                  0x70
#define VMD_MEMBAR2_SHADOW              0x2000
#define VMD_28C1_SHADOW_MEMBAR1         0x2818
#define VMD_28C1_BASE_ID                0x2840
#define VMD_MAX_BUSES                   32

#define VMD_FEAT_BUS_RESTRICT           0x01    /* VMCONFIG bits 9:8 give the first bus */
#define VMD_FEAT_SHADOW                 0x02    /* MEMBAR2 shadow registers (VMLOCK bit 1) */
#define VMD_FEAT_SHADOW_VSCAP           0x04    /* "SHDW" vendor-specific capability */
#define VMD_FEAT_BIOS_INFO              0x08    /* 28C1: bus and offsets from BAR4 */
#define VMD_FEATS_CLIENT                (VMD_FEAT_BUS_RESTRICT | VMD_FEAT_SHADOW_VSCAP)

#define VMD_CMD_IO                      0x0001
#define VMD_CMD_MEMORY                  0x0002
#define VMD_CMD_MASTER                  0x0004

typedef struct _VMD_DOMAIN {
    PVOID Context;                      /* the driver's device extension */
    PVOID (*Map)(PVOID Context, ULONGLONG Phys, ULONG Length);
    ULONG (*HostRead)(PVOID Context, ULONG Offset);
    VOID (*HostWrite)(PVOID Context, ULONG Offset, ULONG Value);
    BOOLEAN (*Match)(PVOID Context, PUCHAR Cfg);
    ULONG BusStart;                     /* first bus of the domain */
    ULONG EcamBase;                     /* bus at CFGBAR offset 0 */
    ULONG BusCount;                     /* buses the CFGBAR covers */
    ULONGLONG CfgPhys;
    ULONGLONG Mem1Phys;                 /* MEMBAR1 (CPU address) and its length */
    ULONGLONG Mem1Length;
    LONGLONG Mem1Offset;                /* CPU address - bus address */
    PUCHAR Cfg[VMD_MAX_BUSES];          /* 1 MB of configuration space per bus, mapped when first used */
} VMD_DOMAIN, *PVMD_DOMAIN;

#endif /* VMD_CORE_TYPES */

/* The functions: in the one source file that defines VMD_CORE_IMPLEMENTATION */
#if defined(VMD_CORE_IMPLEMENTATION) && !defined(VMD_CORE_IMPLEMENTED)
#define VMD_CORE_IMPLEMENTED

static const struct {
    USHORT DeviceId;
    UCHAR Features;
} g_VmdIds[] = {
    { 0x201D, VMD_FEAT_SHADOW_VSCAP },
    { 0x28C0, VMD_FEAT_SHADOW | VMD_FEAT_BUS_RESTRICT },
    { 0x28C1, VMD_FEAT_SHADOW | VMD_FEAT_BIOS_INFO },
    { 0x467F, VMD_FEATS_CLIENT },
    { 0x4C3D, VMD_FEATS_CLIENT },
    { 0xA77F, VMD_FEATS_CLIENT },
    { 0x7D0B, VMD_FEATS_CLIENT },
    { 0xAD0B, VMD_FEATS_CLIENT },
    { 0x9A0B, VMD_FEATS_CLIENT },
    { 0xB60B, VMD_FEATS_CLIENT },
    { 0xB06F, VMD_FEATS_CLIENT },
    { 0xB07F, VMD_FEATS_CLIENT },
    { 0xD70B, VMD_FEATS_CLIENT },
    { 0xD73B, VMD_FEATS_CLIENT },
};

static UCHAR
VmdFeatures(IN USHORT VendorId, IN USHORT DeviceId)
{
    ULONG i;
    if (VendorId != 0x8086) return 0xFF;
    for (i = 0; i < sizeof(g_VmdIds) / sizeof(g_VmdIds[0]); i++) {
        if (g_VmdIds[i].DeviceId == DeviceId) return g_VmdIds[i].Features;
    }
    return 0xFF;
}

/* An Intel VMD: its ID and a mass storage class (RAID) */
static BOOLEAN
VmdIsVmd(IN USHORT VendorId, IN USHORT DeviceId, IN UCHAR BaseClass)
{
    return (BOOLEAN)(VmdFeatures(VendorId, DeviceId) != 0xFF && BaseClass == 0x01);
}

/* The 4 KB configuration space of Bus:Dev.Fn in the domain, NULL outside it */
static PUCHAR
VmdCfgSpace(IN PVMD_DOMAIN D, IN ULONG Bus, IN ULONG DevFn)
{
    ULONG ecam, idx;
    if (Bus < D->EcamBase || Bus < D->BusStart) return NULL;
    ecam = Bus - D->EcamBase;
    idx = Bus - D->BusStart;
    if (ecam >= D->BusCount || idx >= VMD_MAX_BUSES) return NULL;
    if (D->Cfg[idx] == NULL) {
        D->Cfg[idx] = (PUCHAR)D->Map(D->Context, D->CfgPhys + ((ULONGLONG)ecam << 20), 1UL << 20);
        if (D->Cfg[idx] == NULL) return NULL;
    }
    return D->Cfg[idx] + (DevFn << 12);
}

static ULONG
VmdCfgRead(IN PUCHAR Cfg, IN ULONG Offset)
{
    return ScsiPortReadRegisterUlong((PULONG)(Cfg + (Offset & ~3UL)));
}

static VOID
VmdCfgWrite(IN PUCHAR Cfg, IN ULONG Offset, IN ULONG Value)
{
    ScsiPortWriteRegisterUlong((PULONG)(Cfg + (Offset & ~3UL)), Value);
}

/* A memory BAR of the VMD: its address and size */
static ULONGLONG
VmdHostBar(IN PVMD_DOMAIN D, IN ULONG Index, OUT PULONGLONG Length)
{
    ULONG off = 0x10 + Index * 4;
    ULONG lo = D->HostRead(D->Context, off), hi = 0, szlo, szhi = 0xFFFFFFFF;
    BOOLEAN is64 = (BOOLEAN)((lo & 6) == 4);
    ULONGLONG size;

    if (is64) hi = D->HostRead(D->Context, off + 4);
    D->HostWrite(D->Context, off, 0xFFFFFFFF);
    szlo = D->HostRead(D->Context, off);
    D->HostWrite(D->Context, off, lo);
    if (is64) {
        D->HostWrite(D->Context, off + 4, 0xFFFFFFFF);
        szhi = D->HostRead(D->Context, off + 4);
        D->HostWrite(D->Context, off + 4, hi);
    }
    size = ~(((ULONGLONG)szhi << 32) | (szlo & ~0xFUL)) + 1;
    if (!is64) size &= (ULONGLONG)0xFFFFFFFF;
    *Length = size;
    return ((ULONGLONG)hi << 32) | (lo & ~0xFUL);
}

typedef struct _VMD_WALK {
    ULONG NextBus;                      /* last bus number given out */
    ULONGLONG Cursor;                   /* next free bus address in MEMBAR1 */
    ULONGLONG End;
    BOOLEAN Assign;                     /* program buses and BARs */
    BOOLEAN Failed;
    ULONG Found;                        /* matching functions seen */
    ULONG Want;                         /* the one asked for */
    ULONG ChildBus, ChildDevFn;
} VMD_WALK, *PVMD_WALK;

#define VMD_ALIGN(v, a)                 (((v) + (a) - 1) & ~((ULONGLONG)(a) - 1))

/* The memory BARs of an endpoint: given addresses from the cursor (Assign),
   or checked to lie inside MEMBAR1 */
static BOOLEAN
VmdEndpointBars(IN PVMD_DOMAIN D, IN PUCHAR Cfg, IN OUT PVMD_WALK W)
{
    ULONG i;
    ULONGLONG lo1 = D->Mem1Phys - (ULONGLONG)D->Mem1Offset;
    ULONGLONG hi1 = lo1 + D->Mem1Length;

    for (i = 0; i < 6; i++) {
        ULONG off = 0x10 + i * 4, v = VmdCfgRead(Cfg, off), sz, szhi = 0xFFFFFFFF, vhi = 0;
        BOOLEAN is64;
        ULONGLONG size, addr;
        if (v & 1) {                                /* I/O: no I/O window in the domain */
            if (W->Assign) VmdCfgWrite(Cfg, off, 0);
            continue;
        }
        is64 = (BOOLEAN)((v & 6) == 4);
        if (is64) vhi = VmdCfgRead(Cfg, off + 4);
        VmdCfgWrite(Cfg, off, 0xFFFFFFFF);
        sz = VmdCfgRead(Cfg, off);
        VmdCfgWrite(Cfg, off, v);
        if (is64) {
            VmdCfgWrite(Cfg, off + 4, 0xFFFFFFFF);
            szhi = VmdCfgRead(Cfg, off + 4);
            VmdCfgWrite(Cfg, off + 4, vhi);
        }
        if ((sz & ~0xFUL) == 0 && (!is64 || szhi == 0)) {
            if (is64) i++;
            continue;                               /* not implemented */
        }
        size = ~(((ULONGLONG)szhi << 32) | (sz & ~0xFUL)) + 1;
        if (!is64) size &= (ULONGLONG)0xFFFFFFFF;
        if (size < 16) size = 16;
        if (W->Assign) {
            addr = VMD_ALIGN(W->Cursor, size);
            if (addr + size > W->End) {
                W->Failed = TRUE;
                return FALSE;
            }
            W->Cursor = addr + size;
            VmdCfgWrite(Cfg, off, (ULONG)addr | (v & 0xF));
            if (is64) VmdCfgWrite(Cfg, off + 4, (ULONG)(addr >> 32));
        } else {
            addr = ((ULONGLONG)vhi << 32) | (v & ~0xFUL);
            if (addr < lo1 || addr + size > hi1) return FALSE;
        }
        if (is64) i++;
    }
    return TRUE;
}

/* Depth-first walk of one bus of the domain. Assign: buses and windows
   given out (to every device, so that AHCINT and NVMENT, whichever comes
   first, lay the domain out the same way); otherwise the set-up found is
   checked for the functions the driver takes. */
static BOOLEAN
VmdWalkBus(IN PVMD_DOMAIN D, IN ULONG Bus, IN OUT PVMD_WALK W, IN ULONG Depth)
{
    ULONG dev, fn;

    if (Depth > 8) return FALSE;
    for (dev = 0; dev < 32; dev++) {
        PUCHAR cfg0 = VmdCfgSpace(D, Bus, dev << 3);
        ULONG fns;
        if (cfg0 == NULL) return FALSE;
        if ((VmdCfgRead(cfg0, 0) & 0xFFFF) == 0xFFFF) continue;
        fns = (VmdCfgRead(cfg0, 0x0C) & 0x00800000) ? 8 : 1;
        for (fn = 0; fn < fns; fn++) {
            PUCHAR cfg = VmdCfgSpace(D, Bus, (dev << 3) | fn);
            ULONG type, cmd;
            if ((VmdCfgRead(cfg, 0) & 0xFFFF) == 0xFFFF) continue;
            type = (VmdCfgRead(cfg, 0x0C) >> 16) & 0x7F;
            cmd = VmdCfgRead(cfg, 4) & 0xFFFF;
            if (type == 1) {
                ULONG buses = VmdCfgRead(cfg, 0x18), sec, sub;
                if (W->Assign) {
                    ULONGLONG start;
                    if (W->NextBus + 1 >= D->BusStart + D->BusCount) {
                        W->Failed = TRUE;
                        return FALSE;
                    }
                    sec = ++W->NextBus;
                    VmdCfgWrite(cfg, 0x18, (buses & 0xFF000000) | (0xFFUL << 16) | (sec << 8) | Bus);
                    W->Cursor = VMD_ALIGN(W->Cursor, 0x100000);
                    start = W->Cursor;
                    if (!VmdWalkBus(D, sec, W, Depth + 1) && W->Failed) return FALSE;
                    VmdCfgWrite(cfg, 0x18, (buses & 0xFF000000) | (W->NextBus << 16) | (sec << 8) | Bus);
                    W->Cursor = VMD_ALIGN(W->Cursor, 0x100000);
                    if (W->Cursor > start) {
                        VmdCfgWrite(cfg, 0x20, (ULONG)(((W->Cursor - 1) >> 16) & 0xFFF0) << 16 |
                                               (ULONG)((start >> 16) & 0xFFF0));
                    } else {
                        VmdCfgWrite(cfg, 0x20, 0x0000FFF0);
                    }
                    VmdCfgWrite(cfg, 0x24, 0x0000FFF0);     /* prefetchable window closed */
                    VmdCfgWrite(cfg, 0x28, 0);
                    VmdCfgWrite(cfg, 0x2C, 0);
                    VmdCfgWrite(cfg, 0x30, 0);              /* I/O window closed */
                    VmdCfgWrite(cfg, 0x1C, 0x000000F0);
                    VmdCfgWrite(cfg, 4, (cmd & ~VMD_CMD_IO) | VMD_CMD_MEMORY | VMD_CMD_MASTER);
                } else {
                    ULONG win = VmdCfgRead(cfg, 0x20);
                    ULONG wb = (win & 0xFFF0) << 16, wl = (win & 0xFFF00000) | 0xFFFFF;
                    sec = (buses >> 8) & 0xFF;
                    sub = (buses >> 16) & 0xFF;
                    if (sec <= Bus || sub < sec || sec < D->BusStart || sub >= D->BusStart + D->BusCount) {
                        return FALSE;
                    }
                    if (!(cmd & VMD_CMD_MEMORY) || wl < wb) {
                        continue;                           /* nothing reachable behind it */
                    }
                    (VOID)VmdWalkBus(D, sec, W, Depth + 1);
                }
            } else if (type == 0) {
                BOOLEAN match = D->Match(D->Context, cfg);
                if (W->Assign) {
                    if (!VmdEndpointBars(D, cfg, W)) return FALSE;
                    VmdCfgWrite(cfg, 4, (cmd & ~VMD_CMD_IO) | VMD_CMD_MEMORY | VMD_CMD_MASTER);
                } else if (match) {
                    if (!(cmd & VMD_CMD_MEMORY) || !VmdEndpointBars(D, cfg, W)) continue;
                }
                if (match) {
                    if (W->Found == W->Want) {
                        W->ChildBus = Bus;
                        W->ChildDevFn = (dev << 3) | fn;
                    }
                    W->Found++;
                }
            }
        }
    }
    return TRUE;
}

/* Function number Index of those the driver takes: the set-up found when
   they are reachable through it, else the whole domain assigned again */
static BOOLEAN
VmdFindFunction(IN PVMD_DOMAIN D, IN ULONG Index, OUT PULONG Bus, OUT PULONG DevFn, OUT PULONG Count,
                OUT PBOOLEAN Assigned)
{
    VMD_WALK w;
    ULONG pass;

    for (pass = 0; pass < 2; pass++) {
        w.NextBus = D->BusStart;
        w.Cursor = D->Mem1Phys - (ULONGLONG)D->Mem1Offset;
        w.End = w.Cursor + D->Mem1Length;
        w.Assign = (BOOLEAN)(pass == 1);
        w.Failed = FALSE;
        w.Found = 0;
        w.Want = Index;
        if (VmdWalkBus(D, D->BusStart, &w, 0) && !w.Failed && w.Found > 0) {
            *Count = w.Found;
            *Assigned = (BOOLEAN)(pass == 1);
            if (w.Found <= Index) return FALSE;
            *Bus = w.ChildBus;
            *DevFn = w.ChildDevFn;
            return TRUE;
        }
    }
    *Count = 0;
    return FALSE;
}

/* The domain's location: BARs, bus numbers, shadow offsets (what Linux's
   vmd_prepare_offsets_and_bus finds). Memory decoding and bus mastering
   are turned on at the VMD. */
static BOOLEAN
VmdOpen(IN PVMD_DOMAIN D, IN USHORT VendorId, IN USHORT DeviceId)
{
    UCHAR features = VmdFeatures(VendorId, DeviceId);
    ULONGLONG cfgLen, mem2, mem2Len, phys1 = 0;
    ULONG i;

    if (features == 0xFF) return FALSE;
    for (i = 0; i < VMD_MAX_BUSES; i++) D->Cfg[i] = NULL;
    D->HostWrite(D->Context, 4, (D->HostRead(D->Context, 4) & 0xFFFF) | VMD_CMD_MEMORY | VMD_CMD_MASTER);
    D->CfgPhys = VmdHostBar(D, 0, &cfgLen);
    D->Mem1Phys = VmdHostBar(D, 2, &D->Mem1Length);
    mem2 = VmdHostBar(D, 4, &mem2Len);
    D->BusCount = (ULONG)(cfgLen >> 20);
    if (D->CfgPhys == 0 || D->Mem1Phys == 0 || D->BusCount == 0) return FALSE;
    D->BusStart = 0;
    D->EcamBase = 0;
    D->Mem1Offset = 0;
    if (features & VMD_FEAT_BIOS_INFO) {
        PUCHAR m2 = (PUCHAR)D->Map(D->Context, mem2, 0x3000);
        if (m2 == NULL) return FALSE;
        phys1 = ScsiPortReadRegisterUlong((PULONG)(m2 + VMD_28C1_SHADOW_MEMBAR1)) |
                ((ULONGLONG)ScsiPortReadRegisterUlong((PULONG)(m2 + VMD_28C1_SHADOW_MEMBAR1 + 4)) << 32);
        D->BusStart = (ScsiPortReadRegisterUlong((PULONG)(m2 + VMD_28C1_BASE_ID)) >> 8) & 0xFF;
        D->EcamBase = 0;                            /* bus numbers index the CFGBAR as they are */
    } else {
        if ((features & VMD_FEAT_SHADOW) && (D->HostRead(D->Context, VMD_REG_VMLOCK) & 2)) {
            PUCHAR m2 = (PUCHAR)D->Map(D->Context, mem2, 0x3000);
            if (m2 == NULL) return FALSE;
            phys1 = ScsiPortReadRegisterUlong((PULONG)(m2 + VMD_MEMBAR2_SHADOW)) |
                    ((ULONGLONG)ScsiPortReadRegisterUlong((PULONG)(m2 + VMD_MEMBAR2_SHADOW + 4)) << 32);
        } else if (features & VMD_FEAT_SHADOW_VSCAP) {
            /* hypervisor-emulated vendor-specific capability "SHDW" */
            ULONG cap = (D->HostRead(D->Context, 0x04) & 0x00100000) ? (D->HostRead(D->Context, 0x34) & 0xFC) : 0;
            ULONG guard = 48;
            while (cap >= 0x40 && guard--) {
                ULONG hdr = D->HostRead(D->Context, cap);
                if ((hdr & 0xFF) == 0x09) {
                    if (D->HostRead(D->Context, cap + 4) == 0x53484457) {
                        phys1 = D->HostRead(D->Context, cap + 8) | ((ULONGLONG)D->HostRead(D->Context, cap + 12) << 32);
                    }
                    break;
                }
                cap = (hdr >> 8) & 0xFC;
            }
        }
        if ((features & VMD_FEAT_BUS_RESTRICT) && (D->HostRead(D->Context, VMD_REG_VMCAP) & 1)) {
            switch ((D->HostRead(D->Context, VMD_REG_VMCONFIG) >> 8) & 3) {
            case 0: D->BusStart = 0; break;
            case 1: D->BusStart = 128; break;
            default: D->BusStart = 224; break;
            }
        }
        D->EcamBase = D->BusStart;
    }
    if (phys1 != 0) D->Mem1Offset = (LONGLONG)(D->Mem1Phys - (phys1 & ~(ULONGLONG)0xF));
    if (D->BusCount > 256 - D->EcamBase) D->BusCount = 256 - D->EcamBase;
    return TRUE;
}

/* A memory BAR of a function in the domain as a CPU address; *Length its size */
static ULONGLONG
VmdFunctionBar(IN PVMD_DOMAIN D, IN PUCHAR Cfg, IN ULONG Index, OUT PULONG Length)
{
    ULONG off = 0x10 + Index * 4, lo = VmdCfgRead(Cfg, off), hi = 0;
    if ((lo & 6) == 4 && Index < 5) hi = VmdCfgRead(Cfg, off + 4);
    VmdCfgWrite(Cfg, off, 0xFFFFFFFF);
    *Length = ~(VmdCfgRead(Cfg, off) & ~0xFUL) + 1;
    VmdCfgWrite(Cfg, off, lo);
    return (((ULONGLONG)hi << 32) | (lo & ~0xFUL)) + (ULONGLONG)D->Mem1Offset;
}

#endif /* VMD_CORE_IMPLEMENTATION */
