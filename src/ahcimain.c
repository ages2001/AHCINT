#include "ahcint.h"

/* ahcimain.c - DriverEntry, adapter discovery, HBA/port init, ScsiPort entry points */

BOOLEAN AhciHwInitialize(IN PVOID DeviceExtension);
BOOLEAN AhciStartIo(IN PVOID DeviceExtension, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN AhciInterrupt(IN PVOID DeviceExtension);
BOOLEAN AhciResetBus(IN PVOID HwDeviceExtension, IN ULONG PathId);
ULONG   AhciFindAdapter(IN PVOID DeviceExtension, IN PVOID Context, IN PVOID BusInformation,
                        IN PCHAR ArgumentString, IN OUT PPORT_CONFIGURATION_INFORMATION ConfigInfo,
                        OUT PBOOLEAN Again);
#ifdef AHCI_W9X
BOOLEAN AhciAdapterState(IN PVOID DeviceExtension, IN PVOID Context, IN BOOLEAN SaveState);
#endif

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

/* Required: SCSIPORT.PDR reports "Init Failure" if HwAdapterState is NULL */
BOOLEAN
AhciAdapterState(IN PVOID DeviceExtension, IN PVOID Context, IN BOOLEAN SaveState)
{
    (VOID)DeviceExtension;
    (VOID)Context;
    AHCI_DBG((AHCI_PFX "AdapterState: %s\n", SaveState ? "save" : "restore"));
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

    HwInit->DmaArea = (PAHCI_DMA_RESOURCES)ScsiPortGetUncachedExtension(
        HwInit, ConfigInfo, sizeof(AHCI_DMA_RESOURCES));
    if (HwInit->DmaArea == NULL) {
        AHCI_DBG((AHCI_PFX "AllocateDma: ScsiPortGetUncachedExtension failed\n"));
        return FALSE;
    }

    length = sizeof(AHCI_DMA_RESOURCES);
    HwInit->DmaAreaPhysical = ScsiPortGetPhysicalAddress(HwInit, NULL, HwInit->DmaArea, &length);
    if (HwInit->DmaAreaPhysical.LowPart == 0 && HwInit->DmaAreaPhysical.HighPart == 0) {
        AHCI_DBG((AHCI_PFX "AllocateDma: no physical address\n"));
        return FALSE;
    }

    ZeroMemoryBytes(HwInit->DmaArea, sizeof(AHCI_DMA_RESOURCES));

    virt = HwInit->DmaArea->RawBuffer;
    phys = HwInit->DmaAreaPhysical.LowPart;
    high = (ULONG)HwInit->DmaAreaPhysical.HighPart;

    for (port = 0; port < MAX_SUPPORTED_PORTS; port++) {
        PAHCI_PORT_INFO pi = &HwInit->Ports[port];

        pi->CommandList = (PAHCI_COMMAND_HEADER)AhciAlignDma(&virt, &phys, 1024);
        pi->CommandListPhysical = phys;
        pi->CommandListPhysicalUpper = high;
        virt += 1024; phys += 1024;

        pi->ReceivedFis = AhciAlignDma(&virt, &phys, 256);
        pi->ReceivedFisPhysical = phys;
        pi->ReceivedFisPhysicalUpper = high;
        virt += 256; phys += 256;

        pi->CommandTable = AhciAlignDma(&virt, &phys, 128);
        pi->CommandTablePhysical = phys;
        pi->CommandTablePhysicalUpper = high;
        virt += 2048; phys += 2048;

        pi->BounceBuffer = AhciAlignDma(&virt, &phys, 512);
        pi->BouncePhysical = phys;
        pi->BouncePhysicalUpper = high;
        virt += AHCI_BOUNCE_SIZE; phys += AHCI_BOUNCE_SIZE;
    }

    AHCI_DBG((AHCI_PFX "AllocateDma: arena phys=%08lX:%08lX\n", high, HwInit->DmaAreaPhysical.LowPart));
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Port engine control                                                 */
/* ------------------------------------------------------------------ */

VOID
AhciStopPortEngines(IN PHW_DEVICE_EXTENSION HwInit, IN PUCHAR PortBase)
{
    ULONG cmd;
    ULONG timeout;

    cmd = AHCI_READ_REG(PortBase, AHCI_PORT_CMD);
    if (cmd & AHCI_PORT_CMD_ST) {
        AHCI_WRITE_REG(PortBase, AHCI_PORT_CMD, cmd & ~AHCI_PORT_CMD_ST);

        timeout = 50000;
        while ((AHCI_READ_REG(PortBase, AHCI_PORT_CMD) & AHCI_PORT_CMD_CR) && --timeout) {
            ScsiPortStallExecution(10);
        }

        if ((AHCI_READ_REG(PortBase, AHCI_PORT_CMD) & AHCI_PORT_CMD_CR) &&
            (HwInit->HbaCapabilities & AHCI_CAP_SCLO)) {
            cmd = AHCI_READ_REG(PortBase, AHCI_PORT_CMD);
            AHCI_WRITE_REG(PortBase, AHCI_PORT_CMD, cmd | AHCI_PORT_CMD_CLO);
            timeout = 5000;
            while ((AHCI_READ_REG(PortBase, AHCI_PORT_CMD) & AHCI_PORT_CMD_CLO) && --timeout) {
                ScsiPortStallExecution(10);
            }
        }
    }

    cmd = AHCI_READ_REG(PortBase, AHCI_PORT_CMD);
    if (cmd & AHCI_PORT_CMD_FRE) {
        AHCI_WRITE_REG(PortBase, AHCI_PORT_CMD, cmd & ~AHCI_PORT_CMD_FRE);

        timeout = 50000;
        while ((AHCI_READ_REG(PortBase, AHCI_PORT_CMD) & AHCI_PORT_CMD_FR) && --timeout) {
            ScsiPortStallExecution(10);
        }
    }
}

/* Error recovery after a failed command: stop the engine (clears PxCI),
   clear status, start FIS receive and the command engine again. */
VOID
AhciRestartPort(IN PHW_DEVICE_EXTENSION HwInit, IN PUCHAR PortBase)
{
    AhciStopPortEngines(HwInit, PortBase);
    AHCI_WRITE_REG(PortBase, AHCI_PORT_SERR, 0xFFFFFFFF);
    AHCI_WRITE_REG(PortBase, AHCI_PORT_IS, 0xFFFFFFFF);
    AHCI_WRITE_REG(PortBase, AHCI_PORT_CMD, AHCI_READ_REG(PortBase, AHCI_PORT_CMD) | AHCI_PORT_CMD_FRE);
    AHCI_WRITE_REG(PortBase, AHCI_PORT_CMD, AHCI_READ_REG(PortBase, AHCI_PORT_CMD) | AHCI_PORT_CMD_ST);
}

/* ------------------------------------------------------------------ */
/* Port bring-up                                                       */
/* ------------------------------------------------------------------ */

static VOID
AhciExecuteIdentify(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG PortNumber)
{
    PAHCI_PORT_INFO pi;
    PUCHAR portBase;
    PAHCI_COMMAND_HEADER cmdHeader;
    PFIS_REG_H2D fis;
    PAHCI_PRDT_ENTRY prdt;
    ULONG timeout;
    ULONG tfd;
    ULONG ci;
    PUSHORT id;

    pi = &HwInit->Ports[PortNumber];
    portBase = AHCI_PORT_BASE(HwInit->AbarMapped, PortNumber);
    cmdHeader = &pi->CommandList[0];
    fis = (PFIS_REG_H2D)pi->CommandTable;
    prdt = (PAHCI_PRDT_ENTRY)(pi->CommandTable + AHCI_CMDTBL_PRDT_OFFSET);

    ZeroMemoryBytes(pi->BounceBuffer, 512);
    ZeroMemoryBytes(pi->IdentifyData, 512);

    prdt[0].DataBaseAddress = pi->BouncePhysical;
    prdt[0].DataBaseAddressUpper = pi->BouncePhysicalUpper;
    prdt[0].Reserved = 0;
    prdt[0].ByteCountInterrupt = 512 - 1;

    cmdHeader->Flags = AHCI_CMDHDR_CFL_H2D;
    cmdHeader->PrdtLength = 1;
    cmdHeader->PrdByteCount = 0;
    cmdHeader->CommandTableBase = pi->CommandTablePhysical;
    cmdHeader->CommandTableBaseUpper = pi->CommandTablePhysicalUpper;

    ZeroMemoryBytes(fis, sizeof(FIS_REG_H2D));
    fis->FisType = FIS_TYPE_REG_H2D;
    fis->PmPortControl = FIS_H2D_COMMAND;
    fis->Command = pi->IsAtapi ? IDE_COMMAND_IDENTIFY_PACKET : IDE_COMMAND_IDENTIFY_DEVICE;

    AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
    AHCI_WRITE_REG(portBase, AHCI_PORT_IS, 0xFFFFFFFF);
    AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_IS, (1UL << PortNumber));

    AHCI_WRITE_REG(portBase, AHCI_PORT_CI, 1);

    timeout = 150000;
    while (--timeout) {
        if (!(AHCI_READ_REG(portBase, AHCI_PORT_CI) & 1)) break;
        if (AHCI_READ_REG(portBase, AHCI_PORT_TFD) & TFD_STS_ERR) break;
        ScsiPortStallExecution(10);
    }

    ci = AHCI_READ_REG(portBase, AHCI_PORT_CI);
    tfd = AHCI_READ_REG(portBase, AHCI_PORT_TFD);
    id = (PUSHORT)pi->BounceBuffer;

    if (!(ci & 1) && !(tfd & TFD_STS_ERR) && (id[IDW_MODEL] != 0 || id[IDW_GENERAL_CONFIG] != 0)) {
        pi->IdentifyValid = TRUE;
        CopyMemoryBytes(pi->IdentifyData, pi->BounceBuffer, 512);
    } else {
        pi->IdentifyValid = FALSE;
        AHCI_DBG((AHCI_PFX "Port %lu: IDENTIFY failed, CI=%08lX TFD=%08lX\n", PortNumber, ci, tfd));
        if (ci & 1) {
            AhciRestartPort(HwInit, portBase);
        }
    }

    AHCI_WRITE_REG(portBase, AHCI_PORT_IS, 0xFFFFFFFF);
    AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
    AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_IS, (1UL << PortNumber));
}

static BOOLEAN
AhciInitializePort(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG PortNumber)
{
    PAHCI_PORT_INFO pi;
    PUCHAR portBase;
    ULONG ssts, sig, cmd, timeout, retry;

    pi = &HwInit->Ports[PortNumber];
    portBase = AHCI_PORT_BASE(HwInit->AbarMapped, PortNumber);
    pi->Present = FALSE;

    AhciStopPortEngines(HwInit, portBase);

    cmd = AHCI_READ_REG(portBase, AHCI_PORT_CMD);
    AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, cmd | AHCI_PORT_CMD_POD | AHCI_PORT_CMD_SUD);

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

    /* Device detected but no PHY communication yet: COMRESET, 3 tries */
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
    AHCI_WRITE_REG(portBase, AHCI_PORT_IE, 0);          /* Fast Polling: no port interrupts */

    cmd = AHCI_READ_REG(portBase, AHCI_PORT_CMD);
    AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, cmd | AHCI_PORT_CMD_FRE);

    /* Wait for the device's first D2H FIS (BSY/DRQ clear) */
    timeout = 15000;
    while ((AHCI_READ_REG(portBase, AHCI_PORT_TFD) & (TFD_STS_BSY | TFD_STS_DRQ)) && --timeout) {
        ScsiPortStallExecution(100);
    }

    cmd = AHCI_READ_REG(portBase, AHCI_PORT_CMD);
    AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, cmd | AHCI_PORT_CMD_ST);

    /* PxSIG stays FFFFFFFFh until the device's signature FIS arrived;
       ATAPI drives can take a while after COMRESET. Give it up to 2 s. */
    timeout = 2000;
    while ((sig = AHCI_READ_REG(portBase, AHCI_PORT_SIG)) == 0xFFFFFFFF && --timeout) {
        ScsiPortStallExecution(1000);
    }

    pi->Present = TRUE;
    pi->IsAtapi = (BOOLEAN)(sig == SATA_SIG_ATAPI);
    pi->IdentifyValid = FALSE;
    pi->SensePending = FALSE;

    AHCI_DBG((AHCI_PFX "Port %lu: link up, SSTS=%08lX SIG=%08lX (%s)\n",
              PortNumber, ssts, sig, pi->IsAtapi ? "ATAPI" : "ATA"));

    AhciExecuteIdentify(HwInit, PortNumber);

    /* IDENTIFY failed: re-read the signature, retry if the type changed */
    if (!pi->IdentifyValid) {
        sig = AHCI_READ_REG(portBase, AHCI_PORT_SIG);
        if ((BOOLEAN)(sig == SATA_SIG_ATAPI) != pi->IsAtapi) {
            pi->IsAtapi = (BOOLEAN)(sig == SATA_SIG_ATAPI);
            AHCI_DBG((AHCI_PFX "Port %lu: SIG now %08lX, retrying IDENTIFY as %s\n",
                      PortNumber, sig, pi->IsAtapi ? "ATAPI" : "ATA"));
            AhciExecuteIdentify(HwInit, PortNumber);
        }
    }

    AhciParseIdentify(HwInit, PortNumber);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* DriverEntry                                                         */
/* ------------------------------------------------------------------ */

ULONG
DriverEntry(IN PVOID DriverObject, IN PVOID Argument2)
{
    HW_INITIALIZATION_DATA initData;
    ULONG status;

    AHCI_DBG((AHCI_PFX "DriverEntry\n"));

    ZeroMemoryBytes(&initData, sizeof(HW_INITIALIZATION_DATA));

    initData.HwInitializationDataSize = sizeof(HW_INITIALIZATION_DATA);
    initData.HwInitialize = AhciHwInitialize;
    initData.HwStartIo = AhciStartIo;
    initData.HwInterrupt = AhciInterrupt;
    initData.HwResetBus = AhciResetBus;
    initData.HwFindAdapter = AhciFindAdapter;
#ifdef AHCI_W9X
    initData.HwAdapterState = AhciAdapterState;
#endif
    initData.DeviceExtensionSize = sizeof(HW_DEVICE_EXTENSION);
    initData.AdapterInterfaceType = PCIBus;
    initData.NumberOfAccessRanges = 1;
    initData.MapBuffers = TRUE;
    initData.NeedPhysicalAddresses = TRUE;
    /* The HBA has no firmware auto-sense; AHCI_AUTOSENSE decides whether
       the SATL fills SenseInfoBuffer itself (see ahcisatl.c). */
    initData.AutoRequestSense = FALSE;
    initData.MultipleRequestPerLu = FALSE;

    status = ScsiPortInitialize(DriverObject, Argument2, &initData, NULL);

    AHCI_DBG((AHCI_PFX "DriverEntry: ScsiPortInitialize returned %08lX\n", status));
    return status;
}

/* ------------------------------------------------------------------ */
/* HwFindAdapter                                                       */
/* ------------------------------------------------------------------ */

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
    ULONG bar;
    ULONG i;
    USHORT pciCmd;
    BOOLEAN found;
    BOOLEAN ownSlot;

    (VOID)Context;
    (VOID)BusInformation;
    (VOID)ArgumentString;

    hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;
    accessRanges = *ConfigInfo->AccessRanges;
    *Again = FALSE;

    if (hwInit->AbarMapped != NULL) return SP_RETURN_FOUND;

    AHCI_DBG((AHCI_PFX "FindAdapter: called for bus %lu slot %08lX, %lu access range(s)\n",
              ConfigInfo->SystemIoBusNumber, ConfigInfo->SlotNumber, ConfigInfo->NumberOfAccessRanges));

    /* Step 1: the function ScsiPort / PnP / SCSIPORT.PDR called us for */
    found = FALSE;
    ownSlot = FALSE;
    targetBus = ConfigInfo->SystemIoBusNumber;
    targetSlot = ConfigInfo->SlotNumber;

    if (AhciReadPciHeader(hwInit, targetBus, targetSlot, &pciConfig, NULL) &&
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

    /* Memory space + bus master on, INTx disable off */
    pciCmd = (USHORT)((pciConfig.Command | 0x0006) & ~(1 << 10));
    ScsiPortSetBusDataByOffset(hwInit, PCIConfiguration, targetBus, targetSlot,
                               &pciCmd, 0x04, sizeof(USHORT));

#ifdef AHCI_NT4
    /* Non-PnP ScsiPort: report the range we use so it gets claimed */
    if (accessRanges != NULL && ConfigInfo->NumberOfAccessRanges > 0) {
        accessRanges[0].RangeStart = basePhys;
        accessRanges[0].RangeLength = AHCI_ABAR_MAP_SIZE;
        accessRanges[0].RangeInMemory = TRUE;
    }
#endif

#ifdef AHCI_W9X
    hwInit->AbarMapped = (PUCHAR)_MapPhysToLinear(basePhys.LowPart, AHCI_ABAR_MAP_SIZE, MPL_NonCached);
    if (hwInit->AbarMapped == (PUCHAR)0xFFFFFFFF) {
        hwInit->AbarMapped = NULL;
    }
#else
    hwInit->AbarMapped = (PUCHAR)ScsiPortGetDeviceBase(hwInit, ConfigInfo->AdapterInterfaceType,
                                                       targetBus, basePhys, AHCI_ABAR_MAP_SIZE, FALSE);
    if (hwInit->AbarMapped == NULL && ConfigInfo->AdapterInterfaceType != PCIBus) {
        hwInit->AbarMapped = (PUCHAR)ScsiPortGetDeviceBase(hwInit, PCIBus, targetBus, basePhys,
                                                           AHCI_ABAR_MAP_SIZE, FALSE);
    }
#endif

    if (hwInit->AbarMapped == NULL) {
        AHCI_DBG((AHCI_PFX "FindAdapter: mapping ABAR %08lX failed\n", basePhys.LowPart));
        return SP_RETURN_ERROR;
    }

    ConfigInfo->SystemIoBusNumber = targetBus;
    ConfigInfo->SlotNumber = targetSlot;

#ifdef AHCI_NT4
    /* When the miniport picks the slot itself, NT4 ScsiPort does not look
       up the interrupt for it -- take INTx from config space. */
    ConfigInfo->BusInterruptLevel = pciConfig.u.type0.InterruptLine;
    ConfigInfo->BusInterruptVector = pciConfig.u.type0.InterruptLine;
#endif
    ConfigInfo->InterruptMode = LevelSensitive;

    hwInit->HbaCapabilities = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_CAP);

    ConfigInfo->Master = TRUE;
    ConfigInfo->Dma32BitAddresses = TRUE;
#if AHCI_HAS_DMA64
    ConfigInfo->Dma64BitAddresses = (hwInit->HbaCapabilities & AHCI_CAP_S64A) ? TRUE : FALSE;
#endif
    ConfigInfo->ScatterGather = TRUE;
    ConfigInfo->MaximumTransferLength = AHCI_MAX_TRANSFER;
    ConfigInfo->NumberOfPhysicalBreaks = AHCI_MAX_PHYS_BREAKS;
    ConfigInfo->MaximumNumberOfTargets = MAX_SUPPORTED_PORTS;
    ConfigInfo->NumberOfBuses = 1;
    /* ScsiPort skips the initiator ID when scanning: keep it past the last port */
    ConfigInfo->InitiatorBusId[0] = (UCHAR)MAX_SUPPORTED_PORTS;

    AHCI_DBG((AHCI_PFX "FindAdapter: ABAR %08lX mapped, CAP=%08lX, IRQ level %lu\n",
              basePhys.LowPart, hwInit->HbaCapabilities, ConfigInfo->BusInterruptLevel));

    if (!AhciAllocateDma(hwInit, ConfigInfo)) return SP_RETURN_ERROR;

    return SP_RETURN_FOUND;
}

/* ------------------------------------------------------------------ */
/* HwInitialize                                                        */
/* ------------------------------------------------------------------ */

BOOLEAN
AhciHwInitialize(IN PVOID DeviceExtension)
{
    PHW_DEVICE_EXTENSION hwInit;
    ULONG ghc, pi, port, timeout;
#ifndef AHCI_NT4
    ULONG i;
#endif

    hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;

    AHCI_DBG((AHCI_PFX "HwInitialize: CAP=%08lX CAP2=%08lX VS=%08lX\n",
              AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_CAP),
              AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_CAP2),
              AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_VS)));

    /* BIOS/OS handoff: request ownership, wait for BIOS to release it */
    if (AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_CAP2) & AHCI_CAP2_BOH) {
        ULONG bohc = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_BOHC);
        AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_BOHC, bohc | AHCI_BOHC_OOS);

        timeout = 50000;
        while ((AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_BOHC) & AHCI_BOHC_BOS) && --timeout) {
            ScsiPortStallExecution(10);
        }
        AHCI_DBG((AHCI_PFX "HwInitialize: BOHC=%08lX after handoff\n",
                  AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_BOHC)));
    }

    /* AHCI mode, interrupts off */
    ghc = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_GHC);
    AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_GHC, (ghc | AHCI_GHC_AE) & ~AHCI_GHC_IE);
    ScsiPortStallExecution(100);

#ifndef AHCI_NT4
    /* HBA reset on 2000/XP/x64 only; NT4/9x keep the links as the BIOS left them */
    ghc = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_GHC);
    AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_GHC, ghc | AHCI_GHC_HR);
    timeout = 1000;
    while ((AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_GHC) & AHCI_GHC_HR) && --timeout) {
        ScsiPortStallExecution(1000);
    }

    /* HR clears AE on HBAs that are not AHCI-only */
    for (i = 0; i < 5; i++) {
        ghc = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_GHC);
        if (ghc & AHCI_GHC_AE) break;
        AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_GHC, ghc | AHCI_GHC_AE);
        ScsiPortStallExecution(1000);
    }
#endif

    pi = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_PI);
    hwInit->PortsImplemented = pi;
    AHCI_DBG((AHCI_PFX "HwInitialize: GHC=%08lX PI=%08lX\n", ghc, pi));

    for (port = 0; port < MAX_SUPPORTED_PORTS; port++) {
        if (pi & (1UL << port)) {
            AhciInitializePort(hwInit, port);
        }
    }

    for (port = 0; port < MAX_SUPPORTED_PORTS; port++) {
        if (pi & (1UL << port)) {
            PUCHAR pb = AHCI_PORT_BASE(hwInit->AbarMapped, port);
            AHCI_WRITE_REG(pb, AHCI_PORT_IS, 0xFFFFFFFF);
            AHCI_WRITE_REG(pb, AHCI_PORT_SERR, 0xFFFFFFFF);
            AHCI_WRITE_REG(pb, AHCI_PORT_IE, 0);
        }
    }
    AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_IS, 0xFFFFFFFF);

    /* Fast Polling: HBA interrupt stays off */
    ghc = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_GHC);
    AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_GHC, ghc & ~AHCI_GHC_IE);

    AHCI_DBG((AHCI_PFX "HwInitialize: done\n"));
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* HwStartIo                                                           */
/* ------------------------------------------------------------------ */

BOOLEAN
AhciStartIo(IN PVOID DeviceExtension, IN PSCSI_REQUEST_BLOCK Srb)
{
    PHW_DEVICE_EXTENSION hwInit;

    hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;

    switch (Srb->Function) {
    case SRB_FUNCTION_EXECUTE_SCSI:
        AhciSatlProcessSrb(hwInit, Srb);
        break;

    case SRB_FUNCTION_RESET_BUS:
        AhciResetBus(hwInit, Srb->PathId);
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        break;

    case SRB_FUNCTION_ABORT_COMMAND:
        /* Commands never outlive HwStartIo, nothing left to abort */
        Srb->SrbStatus = SRB_STATUS_ABORT_FAILED;
        break;

    case SRB_FUNCTION_RESET_DEVICE:
    case SRB_FUNCTION_SHUTDOWN:
    case SRB_FUNCTION_FLUSH:
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        break;

    default:
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        break;
    }

    ScsiPortNotification(RequestComplete, hwInit, Srb);
    ScsiPortNotification(NextRequest, hwInit, NULL);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* HwInterrupt                                                         */
/* ------------------------------------------------------------------ */

/* PxIE is 0 and GHC.IE is off, so this only runs for a shared line or a
   stray status bit: acknowledge whatever is pending, report ownership. */
BOOLEAN
AhciInterrupt(IN PVOID DeviceExtension)
{
    PHW_DEVICE_EXTENSION hwInit;
    ULONG hbaIs, portIs, p;
    PUCHAR portBase;

    hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;
    if (hwInit->AbarMapped == NULL) return FALSE;

    hbaIs = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_IS);
    if (hbaIs == 0) return FALSE;

    for (p = 0; p < MAX_SUPPORTED_PORTS; p++) {
        if (hbaIs & (1UL << p)) {
            portBase = AHCI_PORT_BASE(hwInit->AbarMapped, p);
            portIs = AHCI_READ_REG(portBase, AHCI_PORT_IS);
            AHCI_WRITE_REG(portBase, AHCI_PORT_IS, portIs);
            AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
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
    ULONG port;

    (VOID)PathId;
    hwInit = (PHW_DEVICE_EXTENSION)HwDeviceExtension;

    AHCI_DBG((AHCI_PFX "ResetBus\n"));

    for (port = 0; port < MAX_SUPPORTED_PORTS; port++) {
        if (hwInit->Ports[port].Present) {
            PUCHAR portBase = AHCI_PORT_BASE(hwInit->AbarMapped, port);
            AhciRestartPort(hwInit, portBase);
            AHCI_WRITE_REG(portBase, AHCI_PORT_IE, 0);
            hwInit->Ports[port].SensePending = FALSE;
        }
    }
    AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_IS, 0xFFFFFFFF);

    return TRUE;
}
