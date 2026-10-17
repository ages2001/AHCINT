#include "ahcint.h"

/* ahcimain.c - DriverEntry, adapter discovery, HBA/port init, ScsiPort entry points, power */

BOOLEAN AhciHwInitialize(IN PVOID DeviceExtension);
BOOLEAN AhciStartIo(IN PVOID DeviceExtension, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN AhciInterrupt(IN PVOID DeviceExtension);
BOOLEAN AhciResetBus(IN PVOID HwDeviceExtension, IN ULONG PathId);
ULONG   AhciFindAdapter(IN PVOID DeviceExtension, IN PVOID Context, IN PVOID BusInformation,
                        IN PCHAR ArgumentString, IN OUT PPORT_CONFIGURATION_INFORMATION ConfigInfo,
                        OUT PBOOLEAN Again);
VOID    AhciTimer(IN PVOID DeviceExtension);
#ifdef AHCI_W9X
BOOLEAN AhciAdapterState(IN PVOID DeviceExtension, IN PVOID Context, IN BOOLEAN SaveState);
#endif
#if !defined(AHCI_NT4)
SCSI_ADAPTER_CONTROL_STATUS AhciAdapterControl(IN PVOID DeviceExtension,
                                               IN SCSI_ADAPTER_CONTROL_TYPE ControlType,
                                               IN PVOID Parameters);
#endif

/* Crash dump / hibernation copy of the driver (DriverEntry without a
   driver object): polled, minimal memory, no registry access. */
static BOOLEAN g_AhciDumpMode;

/* ------------------------------------------------------------------ */
/* Windows 9x: VMM services                                            */
/* ------------------------------------------------------------------ */

#ifdef AHCI_W9X

/* VxD service call: int 20h, then service ordinal and VxD device ID */
#define VMM_DEVICE_ID                   0x0001
#define MapPhysToLinear_Ordinal         0x006C
#define Debug_Printf_Service_Ordinal    0x012D

#define MPL_NonCached                   0x00000000

/* KB Q169584 workaround for mapping the ABAR. VxDJmp form (0x80 flag):
   VMM returns to our caller directly, hence naked with no ret. */
__declspec(naked) PVOID __cdecl
_MapPhysToLinear(ULONG PhysAddr, ULONG nBytes, ULONG flags)
{
    __asm {
        int 0x20
        _emit ((MapPhysToLinear_Ordinal >> 0) & 0xFF)
        _emit (((MapPhysToLinear_Ordinal >> 8) & 0xFF) | 0x80)
        _emit ((VMM_DEVICE_ID >> 0) & 0xFF)
        _emit ((VMM_DEVICE_ID >> 8) & 0xFF)
    }
}

#ifdef AHCI_DBG_ENABLED
/* _Debug_Printf_Service(format, argptr): WDEB386, SoftICE or DebugView */
VOID __cdecl
AhciDbgPrint(PCHAR Format, ...)
{
    __asm lea  eax, (Format + 4)        /* va_list: first variadic argument */
    __asm push eax
    __asm push Format
    __asm int  0x20
    __asm _emit ((Debug_Printf_Service_Ordinal >> 0) & 0xFF)
    __asm _emit ((Debug_Printf_Service_Ordinal >> 8) & 0xFF)
    __asm _emit ((VMM_DEVICE_ID >> 0) & 0xFF)
    __asm _emit ((VMM_DEVICE_ID >> 8) & 0xFF)
    __asm add  esp, 2*4
}
#endif

/* Required: SCSIPORT.PDR reports "Init Failure" if HwAdapterState is NULL.
   On "restore" the BIOS may have used the HBA in between (real mode,
   APM resume): the next request checks the port registers and sets the
   HBA up again if they changed (see AhciCheckState). */
BOOLEAN
AhciAdapterState(IN PVOID DeviceExtension, IN PVOID Context, IN BOOLEAN SaveState)
{
    (VOID)Context;
    AHCI_DBG((AHCI_PFX "AdapterState: %s\n", SaveState ? "save" : "restore"));
    if (!SaveState) ((PHW_DEVICE_EXTENSION)DeviceExtension)->CheckState = TRUE;
    return TRUE;
}

#endif /* AHCI_W9X */

/* ------------------------------------------------------------------ */
/* Controller claim table (several AHCI controllers, one per extension) */
/* ------------------------------------------------------------------ */

typedef struct _AHCI_CLAIMED_DEVICE {
    BOOLEAN InUse;
    ULONG   Bus;
    ULONG   Slot;
} AHCI_CLAIMED_DEVICE;

static AHCI_CLAIMED_DEVICE g_AhciClaimedDevices[AHCI_MAX_CONTROLLERS];

static BOOLEAN
AhciIsDeviceClaimed(IN ULONG Bus, IN ULONG Slot)
{
    ULONG i;
    for (i = 0; i < AHCI_MAX_CONTROLLERS; i++) {
        if (g_AhciClaimedDevices[i].InUse &&
            g_AhciClaimedDevices[i].Bus == Bus &&
            g_AhciClaimedDevices[i].Slot == Slot) {
            return TRUE;
        }
    }
    return FALSE;
}

static VOID
AhciClaimDevice(IN ULONG Bus, IN ULONG Slot)
{
    ULONG i;
    for (i = 0; i < AHCI_MAX_CONTROLLERS; i++) {
        if (!g_AhciClaimedDevices[i].InUse) {
            g_AhciClaimedDevices[i].InUse = TRUE;
            g_AhciClaimedDevices[i].Bus = Bus;
            g_AhciClaimedDevices[i].Slot = Slot;
            return;
        }
    }
    AHCI_DBG((AHCI_PFX "ClaimDevice: table full (max %lu)\n", (ULONG)AHCI_MAX_CONTROLLERS));
}

/* ------------------------------------------------------------------ */
/* PCI discovery                                                       */
/* ------------------------------------------------------------------ */

/* Standard type-0 header: IDs, class code, header type, BARs, INTx. */
#define AHCI_PCI_HEADER_LENGTH          64
#define AHCI_PCI_MULTIFUNCTION          0x80

static BOOLEAN
AhciPciConfigIsAhci(IN PPCI_COMMON_CONFIG PciConfig)
{
    if (PciConfig->BaseClass == PCI_CLASS_MASS_STORAGE &&
        PciConfig->SubClass == PCI_SUBCLASS_AHCI &&
        PciConfig->ProgIf == PCI_PROGIF_AHCI) {
        return TRUE;
    }

    /* AHCI HBAs that report a RAID/other class code in some BIOS modes */
    if (PciConfig->VendorID == 0x1022 &&
        (PciConfig->DeviceID == 0x7901 || PciConfig->DeviceID == 0x43EB ||
         PciConfig->DeviceID == 0x43C8)) {
        return TRUE;
    }
    if (PciConfig->VendorID == 0x8086 &&
        (PciConfig->DeviceID == 0x2829 || PciConfig->DeviceID == 0x2828 ||
         PciConfig->DeviceID == 0x2922 || PciConfig->DeviceID == 0x2681)) {
        return TRUE;
    }
    return FALSE;
}

#ifdef AHCI_NT31_COMPAT
/* Configuration mechanism #1 through the I/O ports, for a HAL without PCI
   support (NT 3.1). On x86 the HAL maps I/O ports 1:1, so the port
   number is the address the ScsiPort port functions take. */
#define AHCI_PCI_CONFIG_ADDRESS         0xCF8
#define AHCI_PCI_CONFIG_DATA            0xCFC
#define AHCI_PCI_CONFIG_ENABLE          0x80000000UL

static BOOLEAN g_AhciPciProbed;
static BOOLEAN g_AhciPciViaPorts;

static VOID
AhciPciSelect(IN ULONG Bus, IN ULONG Slot, IN ULONG Offset)
{
    PCI_SLOT_NUMBER slot;

    slot.u.AsULONG = Slot;
    ScsiPortWritePortUlong((PULONG)AHCI_PCI_CONFIG_ADDRESS,
                           AHCI_PCI_CONFIG_ENABLE | ((Bus & 0xFF) << 16) |
                           ((ULONG)slot.u.bits.DeviceNumber << 11) |
                           ((ULONG)slot.u.bits.FunctionNumber << 8) |
                           (Offset & 0xFC));
}

static ULONG
AhciPciReadPortDword(IN ULONG Bus, IN ULONG Slot, IN ULONG Offset)
{
    AhciPciSelect(Bus, Slot, Offset);
    return ScsiPortReadPortUlong((PULONG)AHCI_PCI_CONFIG_DATA);
}

/* FALSE for an empty function */
static BOOLEAN
AhciPciReadPortHeader(IN ULONG Bus, IN ULONG Slot, OUT PPCI_COMMON_CONFIG PciConfig)
{
    PULONG dw = (PULONG)PciConfig;
    ULONG i;

    dw[0] = AhciPciReadPortDword(Bus, Slot, 0);
    if ((dw[0] & 0xFFFF) == 0xFFFF || (dw[0] & 0xFFFF) == 0) return FALSE;

    for (i = 1; i < AHCI_PCI_HEADER_LENGTH / sizeof(ULONG); i++) {
        dw[i] = AhciPciReadPortDword(Bus, Slot, i * sizeof(ULONG));
    }
    return TRUE;
}

/* Once per driver: does config space have to go through the ports? */
static VOID
AhciPciProbeAccess(IN PHW_DEVICE_EXTENSION HwInit)
{
    ULONG id;
    ULONG save;

    if (g_AhciPciProbed) return;
    g_AhciPciProbed = TRUE;

    /* The HAL answers for bus 0 whenever it knows PCI at all */
    if (ScsiPortGetBusData(HwInit, PCIConfiguration, 0, 0, &id, sizeof(ULONG)) != 0) return;

    save = ScsiPortReadPortUlong((PULONG)AHCI_PCI_CONFIG_ADDRESS);
    ScsiPortWritePortUlong((PULONG)AHCI_PCI_CONFIG_ADDRESS, AHCI_PCI_CONFIG_ENABLE);
    if (ScsiPortReadPortUlong((PULONG)AHCI_PCI_CONFIG_ADDRESS) == AHCI_PCI_CONFIG_ENABLE) {
        g_AhciPciViaPorts = TRUE;
    }
    ScsiPortWritePortUlong((PULONG)AHCI_PCI_CONFIG_ADDRESS, save);

    AHCI_DBG((AHCI_PFX "PCI: no HAL config access, mechanism #1 %s\n",
              g_AhciPciViaPorts ? "used" : "not found"));
}
#endif /* AHCI_NT31_COMPAT */

/* Memory space + bus master on, INTx disable off */
static VOID
AhciPciEnableDevice(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Bus, IN ULONG Slot, IN USHORT Command)
{
    USHORT pciCmd = (USHORT)((Command | 0x0006) & ~(1 << 10));

#ifdef AHCI_NT31_COMPAT
    /* No ScsiPortSetBusDataByOffset to import on NT 3.1: write the command
       register through the ports, and only when the BIOS left it off. */
    (VOID)HwInit;
    if (pciCmd == Command) return;
    AhciPciSelect(Bus, Slot, 0x04);
    ScsiPortWritePortUshort((PUSHORT)(AHCI_PCI_CONFIG_DATA + (0x04 & 2)), pciCmd);
#else
    ScsiPortSetBusDataByOffset(HwInit, PCIConfiguration, Bus, Slot, &pciCmd, 0x04, sizeof(USHORT));
#endif
}

/* FALSE for an empty slot; *BusMissing when the bus does not exist */
static BOOLEAN
AhciReadPciHeader(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN ULONG Bus,
    IN ULONG Slot,
    OUT PPCI_COMMON_CONFIG PciConfig,
    OUT PBOOLEAN BusMissing
)
{
    ULONG bytesRead;

#ifdef AHCI_NT31_COMPAT
    if (g_AhciPciViaPorts) {
        if (BusMissing != NULL) *BusMissing = FALSE;
        return AhciPciReadPortHeader(Bus, Slot, PciConfig);
    }
#endif

    bytesRead = ScsiPortGetBusData(HwInit, PCIConfiguration, Bus, Slot,
                                   PciConfig, AHCI_PCI_HEADER_LENGTH);
    if (BusMissing != NULL) {
        *BusMissing = (BOOLEAN)(bytesRead == 0);
    }
    if (bytesRead < AHCI_PCI_HEADER_LENGTH) return FALSE;
    if (PciConfig->VendorID == 0xFFFF || PciConfig->VendorID == 0x0000) return FALSE;
    return TRUE;
}

/* Fallback when the bus:slot we were called for is not an unclaimed AHCI
   HBA: walk the PCI buses for the next one. Used on every target. */
static BOOLEAN
AhciScanPciForAhci(
    IN PHW_DEVICE_EXTENSION HwInit,
    OUT PULONG FoundBus,
    OUT PULONG FoundSlot,
    OUT PPCI_COMMON_CONFIG PciConfig
)
{
    PCI_SLOT_NUMBER slot;
    ULONG bus, device, function;
    BOOLEAN busMissing;
    BOOLEAN multiFunction;

    for (bus = 0; bus < 256; bus++) {
        for (device = 0; device < 32; device++) {
            multiFunction = FALSE;

            for (function = 0; function < 8; function++) {
                if (function > 0 && !multiFunction) break;

                slot.u.AsULONG = 0;
                slot.u.bits.DeviceNumber = device;
                slot.u.bits.FunctionNumber = function;

                if (!AhciReadPciHeader(HwInit, bus, slot.u.AsULONG, PciConfig, &busMissing)) {
                    if (busMissing && device == 0 && function == 0) {
                        /* No such bus: the HAL/PCI BIOS numbers buses
                           contiguously, nothing further to find. */
                        AHCI_DBG((AHCI_PFX "PCI scan: bus %lu does not exist, stop\n", bus));
                        return FALSE;
                    }
                    if (function == 0) break;
                    continue;
                }

                if (function == 0) {
                    multiFunction = (BOOLEAN)((PciConfig->HeaderType & AHCI_PCI_MULTIFUNCTION) != 0);
                }

                if (AhciPciConfigIsAhci(PciConfig) && !AhciIsDeviceClaimed(bus, slot.u.AsULONG)) {
                    *FoundBus = bus;
                    *FoundSlot = slot.u.AsULONG;
                    return TRUE;
                }
            }
        }
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* DMA arena                                                           */
/* ------------------------------------------------------------------ */

/* Per port: command list 1 KB, received FIS 256 bytes, bounce buffer,
   then one command table per slot; plus slack for the alignment. */
#define AHCI_PORT_DMA_FIXED             (1024 + AHCI_RFIS_SIZE + AHCI_BOUNCE_SIZE + 1024)

static ULONG
AhciCountPorts(IN ULONG Pi)
{
    ULONG n = 0, p;
    for (p = 0; p < MAX_SUPPORTED_PORTS; p++) {
        if (Pi & (1UL << p)) n++;
    }
    return n;
}

static ULONG
AhciArenaLength(IN ULONG Ports, IN ULONG Slots)
{
    return Ports * (AHCI_PORT_DMA_FIXED + Slots * AHCI_CMDTBL_SIZE) + 1024;
}

static PUCHAR
AhciAlignDma(IN OUT PUCHAR *Virt, IN OUT PULONG Phys, IN ULONG Alignment)
{
    ULONG misalign = *Phys % Alignment;
    if (misalign != 0) {
        *Virt += Alignment - misalign;
        *Phys += Alignment - misalign;
    }
    return *Virt;
}

static BOOLEAN
AhciAllocateDma(IN PHW_DEVICE_EXTENSION HwInit, IN PPORT_CONFIGURATION_INFORMATION ConfigInfo)
{
    ULONG length;
    PUCHAR virt;
    ULONG phys;
    ULONG high;
    ULONG port;

    if (HwInit->DmaArea != NULL) return TRUE;

    HwInit->DmaLength = AhciArenaLength(AhciCountPorts(HwInit->PortsImplemented), HwInit->Slots);
    HwInit->DmaArea = (PUCHAR)ScsiPortGetUncachedExtension(HwInit, ConfigInfo, HwInit->DmaLength);
    if (HwInit->DmaArea == NULL) {
        AHCI_DBG((AHCI_PFX "AllocateDma: ScsiPortGetUncachedExtension(%lu) failed\n", HwInit->DmaLength));
        return FALSE;
    }

    length = HwInit->DmaLength;
    HwInit->DmaAreaPhysical = ScsiPortGetPhysicalAddress(HwInit, NULL, HwInit->DmaArea, &length);
    if (HwInit->DmaAreaPhysical.LowPart == 0 && HwInit->DmaAreaPhysical.HighPart == 0) {
        AHCI_DBG((AHCI_PFX "AllocateDma: no physical address\n"));
        return FALSE;
    }

    ZeroMemoryBytes(HwInit->DmaArea, HwInit->DmaLength);

    virt = HwInit->DmaArea;
    phys = HwInit->DmaAreaPhysical.LowPart;
    high = (ULONG)HwInit->DmaAreaPhysical.HighPart;

    for (port = 0; port < MAX_SUPPORTED_PORTS; port++) {
        PAHCI_PORT_INFO pi = &HwInit->Ports[port];

        if (!(HwInit->PortsImplemented & (1UL << port))) continue;

        pi->CommandList = (PAHCI_COMMAND_HEADER)AhciAlignDma(&virt, &phys, 1024);
        pi->CommandListPhysical = phys;
        pi->CommandListPhysicalUpper = high;
        virt += 1024; phys += 1024;

        pi->ReceivedFis = AhciAlignDma(&virt, &phys, 256);
        pi->ReceivedFisPhysical = phys;
        pi->ReceivedFisPhysicalUpper = high;
        virt += AHCI_RFIS_SIZE; phys += AHCI_RFIS_SIZE;

        pi->BounceBuffer = AhciAlignDma(&virt, &phys, 512);
        pi->BouncePhysical = phys;
        pi->BouncePhysicalUpper = high;
        virt += AHCI_BOUNCE_SIZE; phys += AHCI_BOUNCE_SIZE;

        pi->CommandTables = AhciAlignDma(&virt, &phys, 128);
        pi->CommandTablesPhysical = phys;
        pi->CommandTablesPhysicalUpper = high;
        virt += HwInit->Slots * AHCI_CMDTBL_SIZE;
        phys += HwInit->Slots * AHCI_CMDTBL_SIZE;

        pi->Implemented = TRUE;
    }

    AHCI_DBG((AHCI_PFX "AllocateDma: %lu bytes at %08lX:%08lX, %lu slot(s) per port\n",
              HwInit->DmaLength, high, HwInit->DmaAreaPhysical.LowPart, HwInit->Slots));
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Port bring-up                                                       */
/* ------------------------------------------------------------------ */

/* Pool entry for a LUN: the port's own entry for LUN 0, otherwise the
   next free one after the port entries */
static BOOLEAN
AhciAllocDevice(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN ULONG Lun)
{
    ULONG i = Port;

    if (Lun != 0) {
        for (i = MAX_SUPPORTED_PORTS; i < AHCI_MAX_DEVICES; i++) {
            if (HwInit->Devices[i].Owner == 0) break;
        }
        if (i >= AHCI_MAX_DEVICES) return FALSE;
    }
    ZeroMemoryBytes(&HwInit->Devices[i], sizeof(AHCI_DEVICE));
    HwInit->Devices[i].Owner = (UCHAR)(Port + 1);
    HwInit->Ports[Port].DeviceIndex[Lun] = (UCHAR)i;
    return TRUE;
}

/* IDENTIFY a device whose type came from a signature; when that fails,
   try the other command once (a stale or missing signature). */
static BOOLEAN
AhciProbeDevice(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN ULONG Lun, IN ULONG Signature)
{
    PAHCI_DEVICE dev = AHCI_DEVICE_OF(HwInit, Port, Lun);

    dev->Present = TRUE;
    dev->IsAtapi = (BOOLEAN)(Signature == SATA_SIG_ATAPI);
    dev->IdentifyValid = FALSE;
    dev->SensePending = FALSE;

    if (!AhciIdentifyDevice(HwInit, Port, Lun) &&
        Signature != SATA_SIG_ATA && Signature != SATA_SIG_ATAPI) {
        dev->IsAtapi = (BOOLEAN)!dev->IsAtapi;
        AhciIdentifyDevice(HwInit, Port, Lun);
    }

    AhciParseIdentify(HwInit, Port, Lun);
    AhciApplyDeviceSettings(HwInit, Port, Lun);
    return TRUE;
}

/* PM device port: COMRESET through PSCR SControl, wait for the link */
static BOOLEAN
AhciPmLinkUp(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN UCHAR PmPort)
{
    ULONG ssts = 0;
    ULONG t;

    if (!AhciPmWrite(HwInit, Port, PmPort, PM_PSCR_SCONTROL, 0x301)) return FALSE;
    ScsiPortStallExecution(1000);
    AhciPmWrite(HwInit, Port, PmPort, PM_PSCR_SCONTROL, 0x300);

    for (t = 0; t < 100; t++) {
        ScsiPortStallExecution(10000);
        if (!AhciPmRead(HwInit, Port, PmPort, PM_PSCR_SSTATUS, &ssts)) return FALSE;
        if ((ssts & AHCI_SSTS_DET_MASK) == AHCI_SSTS_DET_PHY) break;
        if ((ssts & AHCI_SSTS_DET_MASK) == AHCI_SSTS_DET_NONE && t >= 2) break;
    }
    AhciPmWrite(HwInit, Port, PmPort, PM_PSCR_SERROR, 0xFFFFFFFF);

    AHCI_DBG((AHCI_PFX "Port %lu: PM port %lu SStatus=%08lX\n", Port, (ULONG)PmPort, ssts));
    return (BOOLEAN)((ssts & AHCI_SSTS_DET_MASK) == AHCI_SSTS_DET_PHY);
}

/* Port multiplier: command-based switching, one device at a time.
   Devices become LUN 0, 1, ... in PM port order. */
static VOID
AhciPmAttach(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port)
{
    PAHCI_PORT_INFO pi = &HwInit->Ports[Port];
    PUCHAR portBase = AHCI_PORT_BASE(HwInit->AbarMapped, Port);
    ULONG info = 0, sig, t;
    UCHAR p;

    /* PMA may only change while the engine is stopped */
    AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, AHCI_READ_REG(portBase, AHCI_PORT_CMD) & ~AHCI_PORT_CMD_ST);
    t = 50000;
    while ((AHCI_READ_REG(portBase, AHCI_PORT_CMD) & AHCI_PORT_CMD_CR) && --t) {
        ScsiPortStallExecution(10);
    }
    AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, AHCI_READ_REG(portBase, AHCI_PORT_CMD) | AHCI_PORT_CMD_PMA);
    AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, AHCI_READ_REG(portBase, AHCI_PORT_CMD) | AHCI_PORT_CMD_ST);

    pi->PmAttached = TRUE;
    AhciPmRead(HwInit, Port, AHCI_PMP_CONTROL, PM_GSCR_PRODUCT, &pi->PmProduct);
    if (!AhciPmRead(HwInit, Port, AHCI_PMP_CONTROL, PM_GSCR_INFO, &info)) {
        AHCI_DBG((AHCI_PFX "Port %lu: port multiplier does not answer\n", Port));
        return;
    }
    pi->PmPorts = (UCHAR)(info & 0x0F);
    if (pi->PmPorts > AHCI_PM_MAX_PORTS) pi->PmPorts = AHCI_PM_MAX_PORTS;

    AHCI_DBG((AHCI_PFX "Port %lu: port multiplier %08lX, %lu device ports\n",
              Port, pi->PmProduct, (ULONG)pi->PmPorts));

    for (p = 0; p < pi->PmPorts && pi->DeviceCount < AHCI_MAX_LUNS; p++) {
        if (!AhciPmLinkUp(HwInit, Port, p)) continue;
        if (!AhciSoftReset(HwInit, Port, p, &sig)) {
            AHCI_DBG((AHCI_PFX "Port %lu: PM port %lu: no signature\n", Port, (ULONG)p));
            continue;
        }
        if (!AhciAllocDevice(HwInit, Port, pi->DeviceCount)) {
            AHCI_DBG((AHCI_PFX "Port %lu: device table full\n", Port));
            break;
        }
        AHCI_DEVICE_OF(HwInit, Port, pi->DeviceCount)->Pmp = p;
        AhciProbeDevice(HwInit, Port, pi->DeviceCount, sig);
        AHCI_DBG((AHCI_PFX "Port %lu: PM port %lu = LUN %lu, SIG=%08lX (%s)\n",
                  Port, (ULONG)p, (ULONG)pi->DeviceCount, sig,
                  AHCI_DEVICE_OF(HwInit, Port, pi->DeviceCount)->IsAtapi ? "ATAPI" : "ATA"));
        pi->DeviceCount++;
    }

    AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
    AHCI_WRITE_REG(portBase, AHCI_PORT_IS, 0xFFFFFFFF);
}

/* Find out what sits on a linked, running port: a port multiplier (when
   the HBA supports them) or one device. */
VOID
AhciSetupPort(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port)
{
    PAHCI_PORT_INFO pi = &HwInit->Ports[Port];
    PUCHAR portBase = AHCI_PORT_BASE(HwInit->AbarMapped, Port);
    ULONG sig = 0xFFFFFFFF;
    ULONG timeout;
    ULONG lun;

    /* Give back the devices this port had */
    for (lun = 0; lun < AHCI_MAX_DEVICES; lun++) {
        if (HwInit->Devices[lun].Owner == Port + 1 || lun == Port) {
            ZeroMemoryBytes(&HwInit->Devices[lun], sizeof(AHCI_DEVICE));
        }
    }
    pi->DeviceCount = 0;
    pi->PmAttached = FALSE;
    pi->PmPorts = 0;
    pi->NeedsSetup = FALSE;

    /* With PM support, reset the PM control port: a PM answers with its
       own signature, a plain device with the usual one. */
    if ((HwInit->HbaCapabilities & AHCI_CAP_SPM) && !HwInit->DumpMode) {
        if (AhciSoftReset(HwInit, Port, AHCI_PMP_CONTROL, &sig) && sig == SATA_SIG_PM) {
            AhciPmAttach(HwInit, Port);
            return;
        }
        /* No PM: clear PMA (engine stopped) */
        if (AHCI_READ_REG(portBase, AHCI_PORT_CMD) & AHCI_PORT_CMD_PMA) {
            AhciStopPortEngines(HwInit, portBase);
            AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, AHCI_READ_REG(portBase, AHCI_PORT_CMD) & ~AHCI_PORT_CMD_PMA);
            AhciRestartPort(HwInit, Port, FALSE);
            pi->NeedsSetup = FALSE;
        }
    }

    if (sig == 0xFFFFFFFF) {
        /* PxSIG stays FFFFFFFFh until the device's signature FIS arrived;
           ATAPI drives can take a while after COMRESET. Give it 2 s. */
        timeout = 2000;
        while ((sig = AHCI_READ_REG(portBase, AHCI_PORT_SIG)) == 0xFFFFFFFF && --timeout) {
            ScsiPortStallExecution(1000);
        }
    }

    AHCI_DBG((AHCI_PFX "Port %lu: link up, SSTS=%08lX SIG=%08lX (%s)\n",
              Port, AHCI_READ_REG(portBase, AHCI_PORT_SSTS), sig,
              (sig == SATA_SIG_ATAPI) ? "ATAPI" : "ATA"));

    AhciAllocDevice(HwInit, Port, 0);
    AHCI_DEVICE_OF(HwInit, Port, 0)->Pmp = 0;
    AhciProbeDevice(HwInit, Port, 0, sig);
    pi->DeviceCount = 1;
}

static BOOLEAN
AhciInitializePort(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG PortNumber)
{
    PAHCI_PORT_INFO pi;
    PUCHAR portBase;
    ULONG ssts, cmd, timeout, retry;

    pi = &HwInit->Ports[PortNumber];
    portBase = AHCI_PORT_BASE(HwInit->AbarMapped, PortNumber);
    pi->DeviceCount = 0;
    pi->PmAttached = FALSE;
    pi->Outstanding = 0;
    pi->LuBlocked = FALSE;

    AhciStopPortEngines(HwInit, portBase);

    cmd = AHCI_READ_REG(portBase, AHCI_PORT_CMD);
    AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, (cmd | AHCI_PORT_CMD_POD | AHCI_PORT_CMD_SUD) & ~AHCI_PORT_CMD_PMA);

    /* After the HBA reset the PHY needs a few ms before DET reports a
       device at all; give it up to 20 ms before calling the port empty. */
    timeout = 20;
    do {
        ScsiPortStallExecution(1000);
        ssts = AHCI_READ_REG(portBase, AHCI_PORT_SSTS);
    } while ((ssts & AHCI_SSTS_DET_MASK) == AHCI_SSTS_DET_NONE && --timeout);

    if ((ssts & AHCI_SSTS_DET_MASK) == AHCI_SSTS_DET_NONE) {
        AHCI_DBG((AHCI_PFX "Port %lu: no device (SSTS=%08lX)\n", PortNumber, ssts));
        return FALSE;
    }

    /* COMRESET, 3 tries, when there is no PHY communication yet, or when the
       link is up but slower than it could be: a speed limit left in
       PxSCTL.SPD by the firmware or a previous driver, or a link below the
       HBA's interface speed. The COMRESET below sets no limit (SPD = 0). */
    if ((ssts & AHCI_SSTS_DET_MASK) == AHCI_SSTS_DET_PHY && !pi->SpeedChecked) {
        ULONG iss = (HwInit->HbaCapabilities >> AHCI_CAP_ISS_SHIFT) & AHCI_CAP_ISS_MASK;
        ULONG spd = (ssts >> AHCI_SSTS_SPD_SHIFT) & AHCI_SSTS_SPD_MASK;
        ULONG sctl = AHCI_READ_REG(portBase, AHCI_PORT_SCTL);

        pi->SpeedChecked = TRUE;
        if ((sctl & AHCI_SCTL_SPD_MASK) || (spd != 0 && spd < iss)) {
            AHCI_DBG((AHCI_PFX "Port %lu: link Gen%lu, HBA Gen%lu, SCTL=%08lX - renegotiating\n",
                      PortNumber, spd, iss, sctl));
            ssts = 0;
        }
    }
    if ((ssts & AHCI_SSTS_DET_MASK) != AHCI_SSTS_DET_PHY) {
        for (retry = 0; retry < 3; retry++) {
            AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
            AHCI_WRITE_REG(portBase, AHCI_PORT_SCTL, 0x301);
            ScsiPortStallExecution(1000);
            AHCI_WRITE_REG(portBase, AHCI_PORT_SCTL, 0x300);

            timeout = 30;
            while (--timeout) {
                ssts = AHCI_READ_REG(portBase, AHCI_PORT_SSTS);
                if ((ssts & AHCI_SSTS_DET_MASK) == AHCI_SSTS_DET_PHY) break;
                ScsiPortStallExecution(5000);
            }
            if ((ssts & AHCI_SSTS_DET_MASK) == AHCI_SSTS_DET_PHY) break;
        }
    }

    if ((ssts & AHCI_SSTS_DET_MASK) != AHCI_SSTS_DET_PHY) {
        AHCI_DBG((AHCI_PFX "Port %lu: link not established (SSTS=%08lX)\n", PortNumber, ssts));
        return FALSE;
    }

    AHCI_WRITE_REG(portBase, AHCI_PORT_CLB, pi->CommandListPhysical);
    AHCI_WRITE_REG(portBase, AHCI_PORT_CLBU, pi->CommandListPhysicalUpper);
    AHCI_WRITE_REG(portBase, AHCI_PORT_FB, pi->ReceivedFisPhysical);
    AHCI_WRITE_REG(portBase, AHCI_PORT_FBU, pi->ReceivedFisPhysicalUpper);

    AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
    AHCI_WRITE_REG(portBase, AHCI_PORT_IS, 0xFFFFFFFF);
    AHCI_WRITE_REG(portBase, AHCI_PORT_IE, 0);          /* set later, see AhciSetInterrupts */

    cmd = AHCI_READ_REG(portBase, AHCI_PORT_CMD);
    AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, cmd | AHCI_PORT_CMD_FRE);

    /* Wait for the device's first D2H FIS (BSY/DRQ clear) */
    timeout = 15000;
    while ((AHCI_READ_REG(portBase, AHCI_PORT_TFD) & (TFD_STS_BSY | TFD_STS_DRQ)) && --timeout) {
        ScsiPortStallExecution(100);
    }

    cmd = AHCI_READ_REG(portBase, AHCI_PORT_CMD);
    AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, cmd | AHCI_PORT_CMD_ST);

    AhciSetupPort(HwInit, PortNumber);
    return TRUE;
}

/* Interrupts only with NCQ; Fast Polling keeps them off */
static VOID
AhciSetInterrupts(IN PHW_DEVICE_EXTENSION HwInit)
{
    ULONG port, ghc;

    for (port = 0; port < MAX_SUPPORTED_PORTS; port++) {
        PUCHAR pb;
        if (!HwInit->Ports[port].Implemented) continue;
        pb = AHCI_PORT_BASE(HwInit->AbarMapped, port);
        AHCI_WRITE_REG(pb, AHCI_PORT_IS, 0xFFFFFFFF);
        AHCI_WRITE_REG(pb, AHCI_PORT_SERR, 0xFFFFFFFF);
        AHCI_WRITE_REG(pb, AHCI_PORT_IE,
                       (HwInit->AsyncMode && HwInit->Ports[port].DeviceCount != 0) ? AHCI_PORT_IE_ASYNC : 0);
    }
    AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_IS, 0xFFFFFFFF);

    ghc = AHCI_READ_REG(HwInit->AbarMapped, AHCI_GEN_GHC);
    if (HwInit->AsyncMode) {
        AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_GHC, ghc | AHCI_GHC_IE);
    } else {
        AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_GHC, ghc & ~AHCI_GHC_IE);
    }
}

/* BIOS handoff, AHCI mode, HBA reset (2000/XP/x64), all ports */
static VOID
AhciInitializeHba(IN PHW_DEVICE_EXTENSION HwInit)
{
    ULONG ghc, pi, port, timeout;
#ifndef AHCI_NT4
    ULONG i;
#endif

    AHCI_DBG((AHCI_PFX "HwInitialize: CAP=%08lX CAP2=%08lX VS=%08lX\n",
              AHCI_READ_REG(HwInit->AbarMapped, AHCI_GEN_CAP),
              AHCI_READ_REG(HwInit->AbarMapped, AHCI_GEN_CAP2),
              AHCI_READ_REG(HwInit->AbarMapped, AHCI_GEN_VS)));

    /* BIOS/OS handoff: request ownership, wait for BIOS to release it */
    if (AHCI_READ_REG(HwInit->AbarMapped, AHCI_GEN_CAP2) & AHCI_CAP2_BOH) {
        ULONG bohc = AHCI_READ_REG(HwInit->AbarMapped, AHCI_GEN_BOHC);
        AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_BOHC, bohc | AHCI_BOHC_OOS);

        timeout = 50000;
        while ((AHCI_READ_REG(HwInit->AbarMapped, AHCI_GEN_BOHC) & AHCI_BOHC_BOS) && --timeout) {
            ScsiPortStallExecution(10);
        }
        AHCI_DBG((AHCI_PFX "HwInitialize: BOHC=%08lX after handoff\n",
                  AHCI_READ_REG(HwInit->AbarMapped, AHCI_GEN_BOHC)));
    }

    /* AHCI mode, interrupts off */
    ghc = AHCI_READ_REG(HwInit->AbarMapped, AHCI_GEN_GHC);
    AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_GHC, (ghc | AHCI_GHC_AE) & ~AHCI_GHC_IE);
    ScsiPortStallExecution(100);

#ifndef AHCI_NT4
    /* HBA reset on 2000/XP/x64 only; NT4/9x keep the links as the BIOS left them */
    ghc = AHCI_READ_REG(HwInit->AbarMapped, AHCI_GEN_GHC);
    AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_GHC, ghc | AHCI_GHC_HR);
    timeout = 1000;
    while ((AHCI_READ_REG(HwInit->AbarMapped, AHCI_GEN_GHC) & AHCI_GHC_HR) && --timeout) {
        ScsiPortStallExecution(1000);
    }

    /* HR clears AE on HBAs that are not AHCI-only */
    for (i = 0; i < 5; i++) {
        ghc = AHCI_READ_REG(HwInit->AbarMapped, AHCI_GEN_GHC);
        if (ghc & AHCI_GHC_AE) break;
        AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_GHC, ghc | AHCI_GHC_AE);
        ScsiPortStallExecution(1000);
    }
#endif

    pi = AHCI_READ_REG(HwInit->AbarMapped, AHCI_GEN_PI);
    AHCI_DBG((AHCI_PFX "HwInitialize: GHC=%08lX PI=%08lX\n", ghc, pi));

    for (port = 0; port < MAX_SUPPORTED_PORTS; port++) {
        if (HwInit->Ports[port].Implemented && (pi & (1UL << port))) {
            AhciInitializePort(HwInit, port);
        }
    }

    AhciSetInterrupts(HwInit);
    HwInit->Stopped = FALSE;
    HwInit->CheckState = FALSE;
}

/* After a power transition or a BIOS that took the HBA over: everything
   in flight is lost, set the HBA up again. Settings made through MODE
   SELECT are kept per port and applied again. */
BOOLEAN
AhciReinitialize(IN PHW_DEVICE_EXTENSION HwInit)
{
    ULONG port;

    AHCI_DBG((AHCI_PFX "Reinitialize\n"));
    for (port = 0; port < MAX_SUPPORTED_PORTS; port++) {
        if (HwInit->Ports[port].Outstanding != 0) {
            AhciFailOutstanding(HwInit, port, SRB_STATUS_BUS_RESET);
        }
    }
    AhciInitializeHba(HwInit);
    return TRUE;
}

/* Port registers no longer what the driver programmed (the HBA was reset
   by a sleep state, the BIOS or an ACPI driver)? Then set it up again. */
VOID
AhciCheckState(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port)
{
    PAHCI_PORT_INFO pi;
    PUCHAR pb;
    ULONG cmd;

    if (HwInit->Stopped) {
        AhciReinitialize(HwInit);
        return;
    }
    if (Port >= MAX_SUPPORTED_PORTS) return;
    pi = &HwInit->Ports[Port];
    if (!pi->Implemented || pi->DeviceCount == 0) {
        if (HwInit->CheckState) AhciReinitialize(HwInit);
        return;
    }

    pb = AHCI_PORT_BASE(HwInit->AbarMapped, Port);
    cmd = AHCI_READ_REG(pb, AHCI_PORT_CMD);
    if ((cmd & (AHCI_PORT_CMD_ST | AHCI_PORT_CMD_FRE)) != (AHCI_PORT_CMD_ST | AHCI_PORT_CMD_FRE) ||
        AHCI_READ_REG(pb, AHCI_PORT_CLB) != pi->CommandListPhysical ||
        (HwInit->AsyncMode && !(AHCI_READ_REG(HwInit->AbarMapped, AHCI_GEN_GHC) & AHCI_GHC_IE))) {
        AHCI_DBG((AHCI_PFX "Port %lu: HBA state lost (CMD=%08lX CLB=%08lX)\n",
                  Port, cmd, AHCI_READ_REG(pb, AHCI_PORT_CLB)));
        AhciReinitialize(HwInit);
        return;
    }
    HwInit->CheckState = FALSE;
}

/* Flush every disk, put it in standby, stop the ports. Used for
   AHCI_IOCTL_POWER_DOWN; the next request or POWER_UP brings the HBA back. */
BOOLEAN
AhciPowerDown(IN PHW_DEVICE_EXTENSION HwInit)
{
    ULONG port, lun;

    AHCI_DBG((AHCI_PFX "PowerDown\n"));
    for (port = 0; port < MAX_SUPPORTED_PORTS; port++) {
        PAHCI_PORT_INFO pi = &HwInit->Ports[port];
        if (!pi->Implemented || pi->DeviceCount == 0) continue;
        AhciDrainPort(HwInit, port);
        for (lun = 0; lun < pi->DeviceCount; lun++) {
            if (!AHCI_DEVICE_OF(HwInit, port, lun)->Present || AHCI_DEVICE_OF(HwInit, port, lun)->IsAtapi) continue;
            AhciFlushDevice(HwInit, port, lun);
            AhciRunSimple(HwInit, port, lun, IDE_COMMAND_STANDBY_IMMEDIATE, 0, AHCI_POLL_TIMEOUT_POWER_SEC);
        }
    }
    for (port = 0; port < MAX_SUPPORTED_PORTS; port++) {
        if (!HwInit->Ports[port].Implemented) continue;
        AhciStopPortEngines(HwInit, AHCI_PORT_BASE(HwInit->AbarMapped, port));
        AHCI_WRITE_REG(AHCI_PORT_BASE(HwInit->AbarMapped, port), AHCI_PORT_IE, 0);
    }
    AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_GHC,
                   AHCI_READ_REG(HwInit->AbarMapped, AHCI_GEN_GHC) & ~AHCI_GHC_IE);
    HwInit->Stopped = TRUE;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* NCQ: interrupt watchdog                                             */
/* ------------------------------------------------------------------ */

VOID
AhciArmTimer(IN PHW_DEVICE_EXTENSION HwInit)
{
    if (HwInit->TimerArmed || !HwInit->AsyncMode) return;
    HwInit->TimerArmed = TRUE;
    ScsiPortNotification(RequestTimerCall, HwInit, AhciTimer, AHCI_TIMER_NCQ_USEC);
}

/* Back to Fast Polling for good */
static VOID
AhciDisableAsync(IN PHW_DEVICE_EXTENSION HwInit)
{
    ULONG port;

    AHCI_DBG((AHCI_PFX "NCQ: interrupts not delivered, back to Fast Polling\n"));
    for (port = 0; port < MAX_SUPPORTED_PORTS; port++) {
        if (HwInit->Ports[port].Implemented) AhciDrainPort(HwInit, port);
    }
    HwInit->AsyncMode = FALSE;
    AhciSetInterrupts(HwInit);
}

/* Commands completed by the timer were missed by the interrupt */
VOID
AhciTimer(IN PVOID DeviceExtension)
{
    PHW_DEVICE_EXTENSION hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;
    ULONG port, missed = 0;
    BOOLEAN pending = FALSE;

    hwInit->TimerArmed = FALSE;
    if (!hwInit->AsyncMode || hwInit->Stopped) return;

    for (port = 0; port < MAX_SUPPORTED_PORTS; port++) {
        if (hwInit->Ports[port].Outstanding == 0) continue;
        missed += AhciServicePort(hwInit, port);
        if (hwInit->Ports[port].Outstanding != 0) pending = TRUE;
    }

    if (missed != 0 && ++hwInit->MissedInterrupts >= AHCI_MISSED_IRQ_LIMIT) {
        AhciDisableAsync(hwInit);
        return;
    }
    if (pending) AhciArmTimer(hwInit);
}

/* ------------------------------------------------------------------ */
/* DriverEntry                                                         */
/* ------------------------------------------------------------------ */

#ifdef AHCI_NT31_COMPAT
static BOOLEAN g_AhciFindAdapterCalled;
#endif

#if !defined(AHCI_NT4)
/* Text-mode setup boots with a bare service key: the [Config] values of
   txtsetup.oem reach only the installed system. Without
   Parameters\PnpInterface the x64 ScsiPort fails IRP_MN_START_DEVICE
   before HwFindAdapter (STOP 0x7B), so the driver adds the entry the INF
   would have made. Declared here: miniport.h has no registry API. */
typedef struct _AHCI_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    USHORT *Buffer;
} AHCI_UNICODE_STRING, *PAHCI_UNICODE_STRING;

typedef struct _AHCI_OBJECT_ATTRIBUTES {
    ULONG Length;
    PVOID RootDirectory;
    PAHCI_UNICODE_STRING ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
} AHCI_OBJECT_ATTRIBUTES;

LONG __stdcall ZwOpenKey(PVOID *KeyHandle, ULONG DesiredAccess, PVOID ObjectAttributes);
LONG __stdcall ZwCreateKey(PVOID *KeyHandle, ULONG DesiredAccess, PVOID ObjectAttributes,
                           ULONG TitleIndex, PVOID Class, ULONG CreateOptions, PULONG Disposition);
LONG __stdcall ZwSetValueKey(PVOID KeyHandle, PVOID ValueName, ULONG TitleIndex, ULONG Type,
                             PVOID Data, ULONG DataSize);
LONG __stdcall ZwClose(PVOID Handle);

#define AHCI_OBJ_CASE_INSENSITIVE       0x00000040
#define AHCI_OBJ_KERNEL_HANDLE          0x00000200
#define AHCI_KEY_READ_WRITE             0x0002001F  /* KEY_READ | KEY_WRITE */
#define AHCI_REG_CREATED_NEW_KEY        1
#define AHCI_REG_DWORD                  4

static USHORT g_AhciParametersName[] = L"Parameters";
static USHORT g_AhciPnpInterfaceName[] = L"PnpInterface";
static USHORT g_AhciPciBusValueName[] = L"5";              /* PCIBus */

static VOID
AhciInitString(OUT PAHCI_UNICODE_STRING String, IN USHORT *Text, IN ULONG Bytes)
{
    String->Buffer = Text;
    String->Length = (USHORT)(Bytes - sizeof(USHORT));
    String->MaximumLength = (USHORT)Bytes;
}

static VOID
AhciInitAttributes(OUT AHCI_OBJECT_ATTRIBUTES *Oa, IN PAHCI_UNICODE_STRING Name, IN PVOID Root)
{
    Oa->Length = sizeof(AHCI_OBJECT_ATTRIBUTES);
    Oa->RootDirectory = Root;
    Oa->ObjectName = Name;
    Oa->Attributes = AHCI_OBJ_CASE_INSENSITIVE | AHCI_OBJ_KERNEL_HANDLE;
    Oa->SecurityDescriptor = NULL;
    Oa->SecurityQualityOfService = NULL;
}

static VOID
AhciEnsurePnpInterface(IN PVOID RegistryPath)
{
    AHCI_OBJECT_ATTRIBUTES oa;
    AHCI_UNICODE_STRING name;
    PVOID service, params, pnp;
    ULONG disposition = 0;
    ULONG one = 1;
    LONG status;

    if (RegistryPath == NULL) return;

    AhciInitAttributes(&oa, (PAHCI_UNICODE_STRING)RegistryPath, NULL);
    if (ZwOpenKey(&service, AHCI_KEY_READ_WRITE, &oa) < 0) return;

    AhciInitString(&name, g_AhciParametersName, sizeof(g_AhciParametersName));
    AhciInitAttributes(&oa, &name, service);
    status = ZwCreateKey(&params, AHCI_KEY_READ_WRITE, &oa, 0, NULL, 0, &disposition);
    if (status >= 0) {
        AhciInitString(&name, g_AhciPnpInterfaceName, sizeof(g_AhciPnpInterfaceName));
        AhciInitAttributes(&oa, &name, params);
        status = ZwCreateKey(&pnp, AHCI_KEY_READ_WRITE, &oa, 0, NULL, 0, &disposition);
        if (status >= 0) {
            if (disposition == AHCI_REG_CREATED_NEW_KEY) {
                AhciInitString(&name, g_AhciPciBusValueName, sizeof(g_AhciPciBusValueName));
                status = ZwSetValueKey(pnp, &name, 0, AHCI_REG_DWORD, &one, sizeof(one));
                AHCI_DBG((AHCI_PFX "DriverEntry: Parameters\\PnpInterface\\5 added (%08lX)\n", (ULONG)status));
            }
            ZwClose(pnp);
        }
        ZwClose(params);
    }
    ZwClose(service);
}
#endif

ULONG
DriverEntry(IN PVOID DriverObject, IN PVOID Argument2)
{
    HW_INITIALIZATION_DATA initData;
    ULONG status;

    /* The crash dump / hibernation stack calls the miniport without a
       driver object */
    g_AhciDumpMode = (BOOLEAN)(DriverObject == NULL);

    AHCI_DBG((AHCI_PFX "DriverEntry%s\n", g_AhciDumpMode ? " (dump)" : ""));

    ZeroMemoryBytes(&initData, sizeof(HW_INITIALIZATION_DATA));

#ifdef AHCI_NT31_COMPAT
    initData.HwInitializationDataSize = AHCI_HWINIT_SIZE_NT35;
#else
    initData.HwInitializationDataSize = sizeof(HW_INITIALIZATION_DATA);
#endif
    initData.HwInitialize = AhciHwInitialize;
    initData.HwStartIo = AhciStartIo;
    initData.HwInterrupt = AhciInterrupt;
    initData.HwResetBus = AhciResetBus;
    initData.HwFindAdapter = AhciFindAdapter;
#ifdef AHCI_W9X
    initData.HwAdapterState = AhciAdapterState;
#endif
#if !defined(AHCI_NT4)
    initData.HwAdapterControl = AhciAdapterControl;
#endif
    initData.DeviceExtensionSize = sizeof(HW_DEVICE_EXTENSION);
    initData.AdapterInterfaceType = PCIBus;
    initData.NumberOfAccessRanges = 1;
    initData.MapBuffers = TRUE;
    initData.NeedPhysicalAddresses = TRUE;
    /* The HBA has no firmware auto-sense; AHCI_AUTOSENSE decides whether
       the SATL fills SenseInfoBuffer itself (see ahcisatl.c). */
    initData.AutoRequestSense = FALSE;
#ifdef AHCI_W9X
    initData.TaggedQueuing = FALSE;
    initData.MultipleRequestPerLu = FALSE;
#else
    /* NCQ: queued requests per LU; INQUIRY reports CmdQue only for disks
       that really get them (see AhciHandleInquiry). Not in the dump stack. */
    initData.TaggedQueuing = (BOOLEAN)!g_AhciDumpMode;
    initData.MultipleRequestPerLu = (BOOLEAN)!g_AhciDumpMode;
#endif

#if !defined(AHCI_NT4)
    if (!g_AhciDumpMode) AhciEnsurePnpInterface(Argument2);
#endif

    status = ScsiPortInitialize(DriverObject, Argument2, &initData, NULL);

#ifdef AHCI_NT31_COMPAT
    /* NT 3.1 refuses any other structure size before HwFindAdapter is ever
       called, and has no PCI bus to scan: retry with its 40h-byte layout on
       the ISA bus. The PCI functions are then found through the ports. */
    if (status != 0 && !g_AhciFindAdapterCalled) {
        AHCI_DBG((AHCI_PFX "DriverEntry: %08lX, retrying with the NT 3.1 layout\n", status));
        initData.HwInitializationDataSize = AHCI_HWINIT_SIZE_NT31;
        initData.AdapterInterfaceType = Isa;
        status = ScsiPortInitialize(DriverObject, Argument2, &initData, NULL);
    }
#endif

    AHCI_DBG((AHCI_PFX "DriverEntry: ScsiPortInitialize returned %08lX\n", status));
    return status;
}

/* ------------------------------------------------------------------ */
/* HwFindAdapter                                                       */
/* ------------------------------------------------------------------ */

/* "key" in the argument string (Parameters\Device\DriverParameters on
   NT), case-insensitive */
static BOOLEAN
AhciArgPresent(IN PCHAR Args, IN PCHAR Key)
{
    ULONG i, j;
    CHAR a, b;

    if (Args == NULL) return FALSE;
    for (i = 0; Args[i] != 0; i++) {
        for (j = 0; Key[j] != 0; j++) {
            a = Args[i + j];
            b = Key[j];
            if (a >= 'a' && a <= 'z') a = (CHAR)(a - 'a' + 'A');
            if (a != b) break;
        }
        if (Key[j] == 0) return TRUE;
    }
    return FALSE;
}

ULONG
AhciFindAdapter(
    IN PVOID DeviceExtension,
    IN PVOID Context,
    IN PVOID BusInformation,
    IN PCHAR ArgumentString,
    IN OUT PPORT_CONFIGURATION_INFORMATION ConfigInfo,
    OUT PBOOLEAN Again
)
{
    PHW_DEVICE_EXTENSION hwInit;
    PCI_COMMON_CONFIG pciConfig;
    PACCESS_RANGE accessRanges;
    SCSI_PHYSICAL_ADDRESS basePhys;
    ULONG targetBus, targetSlot;
    ULONG mapBus;
    ULONG bar;
    ULONG i, ports, slots;
    BOOLEAN found;
    BOOLEAN ownSlot;
    BOOLEAN hasSlot;
    BOOLEAN interrupt;

    (VOID)Context;
    (VOID)BusInformation;

    hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;
    accessRanges = *ConfigInfo->AccessRanges;
    *Again = FALSE;

#ifdef AHCI_NT31_COMPAT
    g_AhciFindAdapterCalled = TRUE;
    AhciPciProbeAccess(hwInit);
    /* NT 3.1: no SlotNumber, called for the ISA bus rather than a PCI slot */
    hasSlot = (BOOLEAN)AHCI_CONFIG_HAS(ConfigInfo, SlotNumber);
#else
    hasSlot = TRUE;
#endif

    if (hwInit->AbarMapped != NULL) return SP_RETURN_FOUND;

    hwInit->DumpMode = (BOOLEAN)(g_AhciDumpMode || AhciArgPresent(ArgumentString, "DUMP="));
    hwInit->TrimAllowed = (BOOLEAN)!AhciArgPresent(ArgumentString, "TRIM=0");

    AHCI_DBG((AHCI_PFX "FindAdapter: called for bus %lu slot %08lX, %lu access range(s)%s\n",
              ConfigInfo->SystemIoBusNumber, hasSlot ? ConfigInfo->SlotNumber : 0xFFFFFFFF,
              ConfigInfo->NumberOfAccessRanges, hwInit->DumpMode ? ", dump" : ""));

    /* Step 1: the function ScsiPort / PnP / SCSIPORT.PDR called us for */
    found = FALSE;
    ownSlot = FALSE;
    targetBus = ConfigInfo->SystemIoBusNumber;
    targetSlot = hasSlot ? ConfigInfo->SlotNumber : 0;

    if (hasSlot &&
        AhciReadPciHeader(hwInit, targetBus, targetSlot, &pciConfig, NULL) &&
        AhciPciConfigIsAhci(&pciConfig) &&
        !AhciIsDeviceClaimed(targetBus, targetSlot)) {
        found = TRUE;
        ownSlot = TRUE;
    }

    /* Step 2: fallback scan for the next unclaimed AHCI function */
    if (!found) {
        AHCI_DBG((AHCI_PFX "FindAdapter: not an unclaimed AHCI HBA there, scanning PCI\n"));
        found = AhciScanPciForAhci(hwInit, &targetBus, &targetSlot, &pciConfig);
    }

    if (!found) {
        AHCI_DBG((AHCI_PFX "FindAdapter: no AHCI HBA found\n"));
        return SP_RETURN_NOT_FOUND;
    }

    AHCI_DBG((AHCI_PFX "FindAdapter: HBA %04X:%04X at bus %lu slot %08lX\n",
              pciConfig.VendorID, pciConfig.DeviceID, targetBus, targetSlot));

    /* ABAR = BAR5, a 32-bit memory BAR */
    bar = pciConfig.u.type0.BaseAddresses[AHCI_ABAR_INDEX];
    basePhys.LowPart = (bar & 0x1) ? 0 : (bar & 0xFFFFFFF0);
    basePhys.HighPart = 0;

    /* Config space unreadable/unassigned but PnP handed us resources for
       our own function: use the first memory range from ConfigInfo. */
    if (basePhys.LowPart == 0 && ownSlot && accessRanges != NULL) {
        for (i = 0; i < ConfigInfo->NumberOfAccessRanges; i++) {
            if (accessRanges[i].RangeInMemory &&
                (accessRanges[i].RangeStart.LowPart != 0 || accessRanges[i].RangeStart.HighPart != 0)) {
                basePhys = accessRanges[i].RangeStart;
                break;
            }
        }
    }

    if (basePhys.LowPart == 0 && basePhys.HighPart == 0) {
        AHCI_DBG((AHCI_PFX "FindAdapter: ABAR not assigned (BAR5=%08lX)\n", bar));
        return SP_RETURN_ERROR;
    }

    AhciClaimDevice(targetBus, targetSlot);
    hwInit->PciBus = targetBus;
    hwInit->PciSlot = targetSlot;

    AhciPciEnableDevice(hwInit, targetBus, targetSlot, pciConfig.Command);

#ifdef AHCI_NT4
    /* Non-PnP ScsiPort: report the range we use so it gets claimed */
    if (accessRanges != NULL && ConfigInfo->NumberOfAccessRanges > 0) {
        accessRanges[0].RangeStart = basePhys;
        accessRanges[0].RangeLength = AHCI_ABAR_MAP_SIZE;
        accessRanges[0].RangeInMemory = TRUE;
    }
#endif

    /* Without a slot (NT 3.1) the ABAR is mapped on the bus ScsiPort
       called us for: there is no PCI bus to translate on. */
    mapBus = hasSlot ? targetBus : ConfigInfo->SystemIoBusNumber;

#ifdef AHCI_W9X
    (VOID)mapBus;
    hwInit->AbarMapped = (PUCHAR)_MapPhysToLinear(basePhys.LowPart, AHCI_ABAR_MAP_SIZE, MPL_NonCached);
    if (hwInit->AbarMapped == (PUCHAR)0xFFFFFFFF) {
        hwInit->AbarMapped = NULL;
    }
#else
    hwInit->AbarMapped = (PUCHAR)ScsiPortGetDeviceBase(hwInit, ConfigInfo->AdapterInterfaceType,
                                                       mapBus, basePhys, AHCI_ABAR_MAP_SIZE, FALSE);
    if (hwInit->AbarMapped == NULL && ConfigInfo->AdapterInterfaceType != PCIBus) {
        hwInit->AbarMapped = (PUCHAR)ScsiPortGetDeviceBase(hwInit, PCIBus, targetBus, basePhys,
                                                           AHCI_ABAR_MAP_SIZE, FALSE);
    }
#endif

    if (hwInit->AbarMapped == NULL) {
        AHCI_DBG((AHCI_PFX "FindAdapter: mapping ABAR %08lX failed\n", basePhys.LowPart));
        return SP_RETURN_ERROR;
    }

    if (hasSlot) {
        ConfigInfo->SystemIoBusNumber = targetBus;
        ConfigInfo->SlotNumber = targetSlot;
        ConfigInfo->MaximumNumberOfTargets = MAX_SUPPORTED_PORTS;
#ifdef AHCI_NT4
        /* When the miniport picks the slot itself, ScsiPort does not look
           up the interrupt for it -- take INTx from config space. */
        ConfigInfo->BusInterruptLevel = (pciConfig.u.type0.InterruptLine == 0xFF) ? 0 : pciConfig.u.type0.InterruptLine;
        ConfigInfo->BusInterruptVector = ConfigInfo->BusInterruptLevel;
#endif
    } else {
        /* NT 3.1: no PCI bus driver, so ScsiPort connects no interrupt on
           its own; level 0 connects none and Fast Polling needs none.
           With "NCQ=1" the INTx line from config space is connected on
           the EISA bus (level-sensitive) for NCQ. Ask to be called again
           for the next HBA. */
        ConfigInfo->BusInterruptLevel = 0;
        if (AhciArgPresent(ArgumentString, "NCQ=1") && pciConfig.u.type0.InterruptLine != 0xFF) {
            ConfigInfo->BusInterruptLevel = pciConfig.u.type0.InterruptLine;
        }
        ConfigInfo->BusInterruptVector = ConfigInfo->BusInterruptLevel;
        *Again = TRUE;
        /* NT 3.1 HAL: an ISA bus master always gets map registers below
           16 MB, and the copy-back from them overwrites data the SATL wrote
           itself. A scatter/gather master on EISA gets real physical
           addresses. Must be set before the uncached extension is taken. */
        ConfigInfo->AdapterInterfaceType = Eisa;
    }
    ConfigInfo->InterruptMode = LevelSensitive;

    hwInit->HbaCapabilities = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_CAP);
    hwInit->PortsImplemented = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_PI) &
                               ((1UL << MAX_SUPPORTED_PORTS) - 1);

    ConfigInfo->Master = TRUE;
    ConfigInfo->Dma32BitAddresses = TRUE;
#if AHCI_HAS_DMA64
    ConfigInfo->Dma64BitAddresses = (hwInit->HbaCapabilities & AHCI_CAP_S64A) ? TRUE : FALSE;
#endif
    ConfigInfo->ScatterGather = TRUE;
    ConfigInfo->MaximumTransferLength = AHCI_MAX_TRANSFER;
    ConfigInfo->CachesData = TRUE;             /* ScsiPort passes SRB_FUNCTION_SHUTDOWN and FLUSH only then */
    ConfigInfo->NumberOfPhysicalBreaks = AHCI_MAX_PHYS_BREAKS;
    ConfigInfo->NumberOfBuses = 1;
    /* ScsiPort skips the initiator ID when scanning: keep it past the last port */
    ConfigInfo->InitiatorBusId[0] = (UCHAR)MAX_SUPPORTED_PORTS;

    /* NCQ needs the interrupt: queued commands complete in HwInterrupt.
       Off in the dump stack, with "NCQ=0", on 9x unless "NCQ=1". */
    interrupt = (BOOLEAN)(ConfigInfo->BusInterruptLevel != 0 || ConfigInfo->BusInterruptVector != 0);
    hwInit->NcqAllowed = (BOOLEAN)((hwInit->HbaCapabilities & AHCI_CAP_SNCQ) &&
                                   !hwInit->DumpMode &&
                                   !AhciArgPresent(ArgumentString, "NCQ=0"));
#ifdef AHCI_W9X
    if (!AhciArgPresent(ArgumentString, "NCQ=1")) hwInit->NcqAllowed = FALSE;
#endif
    hwInit->AsyncMode = (BOOLEAN)(hwInit->NcqAllowed && interrupt);

    /* Command tables: one for Fast Polling; with NCQ as many tags as the
       HBA has slots, within the DMA budget */
    slots = 1;
    if (hwInit->AsyncMode) {
        ports = AhciCountPorts(hwInit->PortsImplemented);
        slots = ((hwInit->HbaCapabilities >> AHCI_CAP_NCS_SHIFT) & AHCI_CAP_NCS_MASK) + 1;
        if (ports != 0) {
            ULONG fit = ((AHCI_DMA_BUDGET - 1024) / ports - AHCI_PORT_DMA_FIXED) / AHCI_CMDTBL_SIZE;
            if (slots > fit) slots = fit;
        }
        if (slots > AHCI_MAX_SLOTS) slots = AHCI_MAX_SLOTS;
        if (slots < 2) {
            slots = 1;
            hwInit->AsyncMode = FALSE;
        }
    }
    hwInit->Slots = slots;

    AHCI_DBG((AHCI_PFX "FindAdapter: ABAR %08lX mapped, CAP=%08lX PI=%08lX, IRQ level %lu, %s\n",
              basePhys.LowPart, hwInit->HbaCapabilities, hwInit->PortsImplemented,
              ConfigInfo->BusInterruptLevel, hwInit->AsyncMode ? "NCQ" : "Fast Polling"));

    if (!AhciAllocateDma(hwInit, ConfigInfo)) return SP_RETURN_ERROR;

    return SP_RETURN_FOUND;
}

/* ------------------------------------------------------------------ */
/* HwInitialize                                                        */
/* ------------------------------------------------------------------ */

BOOLEAN
AhciHwInitialize(IN PVOID DeviceExtension)
{
    AhciInitializeHba((PHW_DEVICE_EXTENSION)DeviceExtension);
    AHCI_DBG((AHCI_PFX "HwInitialize: done\n"));
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* HwStartIo                                                           */
/* ------------------------------------------------------------------ */

static VOID
AhciResetTarget(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port)
{
    if (Port >= MAX_SUPPORTED_PORTS || !HwInit->Ports[Port].Implemented ||
        HwInit->Ports[Port].DeviceCount == 0) {
        return;
    }
    AhciFailOutstanding(HwInit, Port, SRB_STATUS_BUS_RESET);
    AhciRestartPort(HwInit, Port, FALSE);
}

BOOLEAN
AhciStartIo(IN PVOID DeviceExtension, IN PSCSI_REQUEST_BLOCK Srb)
{
    PHW_DEVICE_EXTENSION hwInit;
    ULONG lun;

    hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;
    Srb->SrbStatus = SRB_STATUS_PENDING;

    switch (Srb->Function) {
    case SRB_FUNCTION_EXECUTE_SCSI:
        AhciCheckState(hwInit, Srb->TargetId);
        AhciSatlProcessSrb(hwInit, Srb);
        break;

    case SRB_FUNCTION_IO_CONTROL:
        AhciProcessIoControl(hwInit, Srb);
        break;

    case SRB_FUNCTION_RESET_BUS:
        AhciResetBus(hwInit, Srb->PathId);
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        break;

    case SRB_FUNCTION_RESET_DEVICE:
        AhciResetTarget(hwInit, Srb->TargetId);
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        break;

    case SRB_FUNCTION_ABORT_COMMAND:
        /* Queued commands cannot be taken back one by one; ScsiPort goes
           on with a reset */
        Srb->SrbStatus = SRB_STATUS_ABORT_FAILED;
        break;

    case SRB_FUNCTION_SHUTDOWN:
    case SRB_FUNCTION_FLUSH:
        /* Write cache to the media; for every device on the target */
        AhciCheckState(hwInit, Srb->TargetId);
        if (Srb->TargetId < MAX_SUPPORTED_PORTS && hwInit->Ports[Srb->TargetId].Implemented) {
            for (lun = 0; lun < hwInit->Ports[Srb->TargetId].DeviceCount; lun++) {
                if (Srb->Function == SRB_FUNCTION_SHUTDOWN) {
                    AhciShutdownDevice(hwInit, Srb->TargetId, lun);
                } else if (lun == Srb->Lun) {
                    AhciFlushDevice(hwInit, Srb->TargetId, lun);
                }
            }
        }
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        break;

    default:
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        break;
    }

    /* Queued (NCQ): IssueNcq already told ScsiPort what it may send next */
    if (Srb->SrbStatus != SRB_STATUS_PENDING) {
        ScsiPortNotification(RequestComplete, hwInit, Srb);
        ScsiPortNotification(NextRequest, hwInit, NULL);
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* HwInterrupt                                                         */
/* ------------------------------------------------------------------ */

/* NCQ completions and errors. With Fast Polling PxIE and GHC.IE are off,
   so this only runs for a shared line or a stray status bit. */
BOOLEAN
AhciInterrupt(IN PVOID DeviceExtension)
{
    PHW_DEVICE_EXTENSION hwInit;
    ULONG hbaIs, portIs, p;
    PUCHAR portBase;

    hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;
    if (hwInit->AbarMapped == NULL) return FALSE;

    hbaIs = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_IS);
    if (hbaIs == 0 || hbaIs == 0xFFFFFFFF) return FALSE;

    for (p = 0; p < MAX_SUPPORTED_PORTS; p++) {
        if (!(hbaIs & (1UL << p))) continue;
        if (hwInit->AsyncMode && hwInit->Ports[p].Outstanding != 0) {
            if (AhciServicePort(hwInit, p) != 0) hwInit->MissedInterrupts = 0;
        } else {
            portBase = AHCI_PORT_BASE(hwInit->AbarMapped, p);
            portIs = AHCI_READ_REG(portBase, AHCI_PORT_IS);
            AHCI_WRITE_REG(portBase, AHCI_PORT_IS, portIs);
            if (portIs & AHCI_PORT_IS_FATAL) AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
        }
    }
    AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_IS, hbaIs);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* HwResetBus                                                          */
/* ------------------------------------------------------------------ */

BOOLEAN
AhciResetBus(IN PVOID HwDeviceExtension, IN ULONG PathId)
{
    PHW_DEVICE_EXTENSION hwInit;
    ULONG port, lun;

    (VOID)PathId;
    hwInit = (PHW_DEVICE_EXTENSION)HwDeviceExtension;

    AHCI_DBG((AHCI_PFX "ResetBus\n"));

    for (port = 0; port < MAX_SUPPORTED_PORTS; port++) {
        PAHCI_PORT_INFO pi = &hwInit->Ports[port];
        if (!pi->Implemented || pi->DeviceCount == 0) continue;
        AhciFailOutstanding(hwInit, port, SRB_STATUS_BUS_RESET);
        AhciRestartPort(hwInit, port, FALSE);
        for (lun = 0; lun < pi->DeviceCount; lun++) AHCI_DEVICE_OF(hwInit, port, lun)->SensePending = FALSE;
    }
    AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_IS, 0xFFFFFFFF);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* HwAdapterControl (2000/XP/x64): stop and restart for power changes  */
/* ------------------------------------------------------------------ */

#if !defined(AHCI_NT4)
SCSI_ADAPTER_CONTROL_STATUS
AhciAdapterControl(
    IN PVOID DeviceExtension,
    IN SCSI_ADAPTER_CONTROL_TYPE ControlType,
    IN PVOID Parameters
)
{
    PHW_DEVICE_EXTENSION hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;
    PSCSI_SUPPORTED_CONTROL_TYPE_LIST list;
    ULONG i, port;

    switch (ControlType) {
    case ScsiQuerySupportedControlTypes:
        list = (PSCSI_SUPPORTED_CONTROL_TYPE_LIST)Parameters;
        for (i = 0; i < list->MaxControlType; i++) {
            list->SupportedTypeList[i] = (BOOLEAN)(i == ScsiQuerySupportedControlTypes ||
                                                  i == ScsiStopAdapter ||
                                                  i == ScsiRestartAdapter);
        }
        return ScsiAdapterControlSuccess;

    case ScsiStopAdapter:
        /* The class drivers have flushed and stopped the disks already */
        AHCI_DBG((AHCI_PFX "AdapterControl: stop\n"));
        for (port = 0; port < MAX_SUPPORTED_PORTS; port++) {
            if (!hwInit->Ports[port].Implemented) continue;
            AhciDrainPort(hwInit, port);
            AHCI_WRITE_REG(AHCI_PORT_BASE(hwInit->AbarMapped, port), AHCI_PORT_IE, 0);
            AhciStopPortEngines(hwInit, AHCI_PORT_BASE(hwInit->AbarMapped, port));
        }
        AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_GHC,
                       AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_GHC) & ~AHCI_GHC_IE);
        hwInit->Stopped = TRUE;
        return ScsiAdapterControlSuccess;

    case ScsiRestartAdapter:
        AHCI_DBG((AHCI_PFX "AdapterControl: restart\n"));
        AhciReinitialize(hwInit);
        return ScsiAdapterControlSuccess;

    default:
        return ScsiAdapterControlUnsuccessful;
    }
}
#endif
