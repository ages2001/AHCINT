#include "ahcint9x.h"

ULONG __cdecl DbgPrint(PCH Format, ...)
{
    if (Format != NULL) {
        AhciTrace(Format);
    }
    return 0;
}

/*
 * _MapPhysToLinear -- raw VxD service call, Win95 KB Q169584 workaround.
 *
 * ScsiPortGetDeviceBase() can fail to return a usable flat/linear
 * address for a memory-mapped PCI BAR on real Windows 95 (this is a
 * documented Microsoft bug, KB Q169584). This is the confirmed root
 * cause of the install-time hard system hang: AhciFindAdapter used to
 * map the AHCI ABAR with plain ScsiPortGetDeviceBase(), and every
 * AHCI_READ_REG/AHCI_WRITE_REG access thereafter (most of
 * AhciHwInitialize, which runs synchronously during Add New Hardware
 * detection) went through that possibly-bad pointer.
 *
 * The fix, taken from nvme2k_9x (a real, shipping Windows 9x SCSI
 * miniport by Dominik Behr & SweetLow) which has the exact same
 * Q169584 workaround for its own NVMe BAR: call the VMM's
 * _MapPhysToLinear service directly. This is a standard ring-0 VxD
 * service call -- "int 0x20" is the VxD call trap, followed by the
 * VxD service ordinal (0x006C for _MapPhysToLinear) and the target
 * VxD's device ID (0x0001 for VMM itself), each encoded as two bytes
 * per the documented Win9x VxD calling convention. This always
 * returns a valid, guaranteed-linear pointer, unlike
 * ScsiPortGetDeviceBase() on this platform.
 */
#define _MapPhysToLinear_Ordinal   0x006C
#define VMM_DEVICE_ID              0x0001

/* Naked, no explicit "ret": the VMM's VxD-call trap (int 0x20 plus the
   4 encoded ordinal/device-ID bytes) itself performs the call and
   returns control to the instruction right after those 4 bytes -- it is
   NOT a normal x86 call/ret pair. This matches nvme2k_9x's own
   _MapPhysToLinear/_PageModifyPermissions implementations exactly
   (utils.c), which are naked with no trailing ret either; the __cdecl
   caller's own "add esp,N" cleanup (emitted by the compiler at the call
   site for a __cdecl function) is what actually returns from here, via
   the return address the VMM call convention leaves on the stack. */
__declspec(naked) PVOID __cdecl _MapPhysToLinear(ULONG PhysAddr, ULONG nBytes, ULONG flags) {
    __asm {
        int 0x20
        _emit ((_MapPhysToLinear_Ordinal >> 0) & 0xFF)
        _emit (((_MapPhysToLinear_Ordinal >> 8) & 0xFF) | 0x80)
        _emit ((VMM_DEVICE_ID >> 0) & 0xFF)
        _emit ((VMM_DEVICE_ID >> 8) & 0xFF)
    }
}

BOOLEAN AhciHwInitialize(IN PVOID DeviceExtension);
BOOLEAN AhciStartIo(IN PVOID DeviceExtension, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN AhciInterrupt(IN PVOID DeviceExtension);
BOOLEAN AhciResetBus(IN PVOID HwDeviceExtension, IN ULONG PathId);
ULONG AhciFindAdapter(IN PVOID DeviceExtension, IN PVOID Context, IN PVOID BusInformation, IN PCHAR ArgumentString, IN OUT PPORT_CONFIGURATION_INFORMATION ConfigInfo, OUT PBOOLEAN Again);
BOOLEAN AhciSatlProcessSrb(IN PHW_DEVICE_EXTENSION DeviceExtension, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN AhciAdapterState(IN PVOID DeviceExtension, IN PVOID Context, IN BOOLEAN SaveState);

BOOLEAN AhciAdapterState(IN PVOID DeviceExtension, IN PVOID Context, IN BOOLEAN SaveState) {
    /* DeviceExtension/Context/SaveState are intentionally unused now --
       see the comment below for why this deliberately does nothing. */
    (VOID)DeviceExtension;
    (VOID)Context;
    (VOID)SaveState;

    AHCI_TRACE("AhciAdapterState: enter");

    AHCI_TRACE("AhciAdapterState: exit, return TRUE");
    return TRUE;
}

/* Multi-controller support: tracks which PCI bus:slot each
   DeviceExtension already claimed, so two instances can't grab
   the same physical AHCI controller. */
#define AHCI_MAX_CONTROLLERS 8

typedef struct _AHCI_CLAIMED_DEVICE {
    BOOLEAN InUse;
    ULONG   Bus;
    ULONG   Slot;
} AHCI_CLAIMED_DEVICE, *PAHCI_CLAIMED_DEVICE;

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
    AHCI_DBG_MSG("AhciClaimDevice: claimed-device table full");
}

static BOOLEAN
AhciPciConfigIsAhci(IN PPCI_COMMON_CONFIG PciConfig)
{
    return (BOOLEAN)(PciConfig->BaseClass == PCI_CLASS_MASS_STORAGE &&
                      PciConfig->SubClass == PCI_SUBCLASS_AHCI &&
                      PciConfig->ProgIf == PCI_PROGIF_AHCI);
}

static BOOLEAN AhciAllocateDma(IN PHW_DEVICE_EXTENSION HwInit, IN PPORT_CONFIGURATION_INFORMATION ConfigInfo) {
    ULONG uncachedSize = sizeof(AHCI_DMA_RESOURCES);
    ULONG allocLen = uncachedSize;
    PUCHAR virtPtr;
    ULONG physPtr;
    ULONG port;
    ULONG alignOffset;

    if (HwInit->DmaArea != NULL) return TRUE;

    HwInit->DmaArea = (PAHCI_DMA_RESOURCES)ScsiPortGetUncachedExtension(HwInit, ConfigInfo, uncachedSize);
    if (!HwInit->DmaArea) return FALSE;

    HwInit->DmaAreaPhysical = ScsiPortGetPhysicalAddress(HwInit, NULL, HwInit->DmaArea, &allocLen);
    if (HwInit->DmaAreaPhysical.LowPart == 0 && HwInit->DmaAreaPhysical.HighPart == 0) return FALSE;

    ZeroMemoryBytes(HwInit->DmaArea, sizeof(AHCI_DMA_RESOURCES));

    virtPtr = (PUCHAR)HwInit->DmaArea->RawBuffer;
    physPtr = HwInit->DmaAreaPhysical.LowPart;

    for (port = 0; port < MAX_AHCI_PORTS; port++) {
        alignOffset = physPtr % 1024;
        if (alignOffset != 0) {
            ULONG pad = 1024 - alignOffset;
            virtPtr += pad;
            physPtr += pad;
        }
        HwInit->Ports[port].CommandList = (PAHCI_COMMAND_HEADER)virtPtr;
        HwInit->Ports[port].CommandListPhysical = physPtr;
        HwInit->Ports[port].CommandListPhysicalUpper = HwInit->DmaAreaPhysical.HighPart;
        virtPtr += 1024;
        physPtr += 1024;

        alignOffset = physPtr % 256;
        if (alignOffset != 0) {
            ULONG pad = 256 - alignOffset;
            virtPtr += pad;
            physPtr += pad;
        }
        HwInit->Ports[port].ReceivedFis = virtPtr;
        HwInit->Ports[port].ReceivedFisPhysical = physPtr;
        HwInit->Ports[port].ReceivedFisPhysicalUpper = HwInit->DmaAreaPhysical.HighPart;
        virtPtr += 256;
        physPtr += 256;

        alignOffset = physPtr % 128;
        if (alignOffset != 0) {
            ULONG pad = 128 - alignOffset;
            virtPtr += pad;
            physPtr += pad;
        }
        HwInit->Ports[port].CommandTable = virtPtr;
        HwInit->Ports[port].CommandTablePhysical = physPtr;
        HwInit->Ports[port].CommandTablePhysicalUpper = HwInit->DmaAreaPhysical.HighPart;
        virtPtr += 2048;
        physPtr += 2048;

        alignOffset = physPtr % 512;
        if (alignOffset != 0) {
            ULONG pad = 512 - alignOffset;
            virtPtr += pad;
            physPtr += pad;
        }
        HwInit->Ports[port].IdentifyDmaBuffer = virtPtr;
        HwInit->Ports[port].IdentifyDmaPhysical = physPtr;
        HwInit->Ports[port].IdentifyDmaPhysicalUpper = HwInit->DmaAreaPhysical.HighPart;
        virtPtr += 512;
        physPtr += 512;
    }

    return TRUE;
}

VOID AhciStopPortEngines(IN PUCHAR portBase) {
    ULONG cmd;
    ULONG timeout;

    cmd = AHCI_READ_REG(portBase, AHCI_PORT_CMD);

    if (cmd & AHCI_PORT_CMD_ST) {
        cmd &= ~AHCI_PORT_CMD_ST;
        AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, cmd);

        timeout = 50000;
        while ((AHCI_READ_REG(portBase, AHCI_PORT_CMD) & AHCI_PORT_CMD_CR) && --timeout) {
            ScsiPortStallExecution(10);
        }
    }

    if (cmd & AHCI_PORT_CMD_FRE) {
        cmd &= ~AHCI_PORT_CMD_FRE;
        AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, cmd);

        timeout = 50000;
        while ((AHCI_READ_REG(portBase, AHCI_PORT_CMD) & AHCI_PORT_CMD_FR) && --timeout) {
            ScsiPortStallExecution(10);
        }
    }
}

static VOID AhciExecuteIdentify(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG PortNumber) {
    PUCHAR portBase;
    PAHCI_COMMAND_HEADER cmdHeader;
    PFIS_REG_H2D fis;
    PAHCI_PRDT_ENTRY prdt;
    ULONG timeout;
    BOOLEAN isCd;
    ULONG tfd;
    PUSHORT id;
    ULONG i;

    portBase = AHCI_PORT_BASE(HwInit->AbarMapped, PortNumber);
    cmdHeader = &HwInit->Ports[PortNumber].CommandList[0];
    fis = (PFIS_REG_H2D)HwInit->Ports[PortNumber].CommandTable;
    prdt = (PAHCI_PRDT_ENTRY)(HwInit->Ports[PortNumber].CommandTable + 0x80);
    timeout = 150000;
    isCd = HwInit->Ports[PortNumber].IsAtapi;

    ZeroMemoryBytes(HwInit->Ports[PortNumber].IdentifyDmaBuffer, 512);
    ZeroMemoryBytes(HwInit->Ports[PortNumber].IdentifyData, 512);

    prdt[0].DataBaseAddress = HwInit->Ports[PortNumber].IdentifyDmaPhysical;
    prdt[0].DataBaseAddressUpper = HwInit->Ports[PortNumber].IdentifyDmaPhysicalUpper;
    prdt[0].Reserved = 0;
    prdt[0].ByteCountInterrupt = (512 - 1);

    cmdHeader->Flags = 5;
    cmdHeader->PrdtLength = 1;
    cmdHeader->PrdByteCount = 0;
    cmdHeader->CommandTableBase = HwInit->Ports[PortNumber].CommandTablePhysical;
    cmdHeader->CommandTableBaseUpper = HwInit->Ports[PortNumber].CommandTablePhysicalUpper;

    ZeroMemoryBytes(fis, sizeof(FIS_REG_H2D));
    fis->FisType = 0x27;
    fis->PmPortControl = 0x80;
    fis->Command = isCd ? IDE_COMMAND_IDENTIFY_PACKET : IDE_COMMAND_IDENTIFY_DEVICE;
    fis->Device = 0;

    AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
    AHCI_WRITE_REG(portBase, AHCI_PORT_IS, 0xFFFFFFFF);
    AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_IS, (1UL << PortNumber));

    AHCI_WRITE_REG(portBase, AHCI_PORT_CI, 1);

    AHCI_TRACE("AhciExecuteIdentify: entering CI poll loop");
    while (--timeout) {
        if (!(AHCI_READ_REG(portBase, AHCI_PORT_CI) & 1)) break;
        tfd = AHCI_READ_REG(portBase, AHCI_PORT_TFD);
        if (tfd & 0x01) break;
        ScsiPortStallExecution(10);
    }
    AHCI_TRACE("AhciExecuteIdentify: CI poll loop exited");

    tfd = AHCI_READ_REG(portBase, AHCI_PORT_TFD);
    id = (PUSHORT)HwInit->Ports[PortNumber].IdentifyDmaBuffer;

    if (!(AHCI_READ_REG(portBase, AHCI_PORT_CI) & 1) && !(tfd & 0x01) && (id[27] != 0 || id[0] != 0)) {
        HwInit->Ports[PortNumber].IdentifyValid = TRUE;
        for (i = 0; i < 512; i++) {
            HwInit->Ports[PortNumber].IdentifyData[i] = HwInit->Ports[PortNumber].IdentifyDmaBuffer[i];
        }
    } else {
        HwInit->Ports[PortNumber].IdentifyValid = FALSE;
    }

    AHCI_WRITE_REG(portBase, AHCI_PORT_IS, 0xFFFFFFFF);
    AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
    AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_IS, (1UL << PortNumber));
}

static BOOLEAN AhciInitializePort(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG PortNumber) {
    PUCHAR portBase;
    ULONG ssts, sig, cmd, timeout;

    AHCI_TRACE("AhciInitializePort: enter");

    portBase = AHCI_PORT_BASE(HwInit->AbarMapped, PortNumber);
    AhciStopPortEngines(portBase);
    AHCI_TRACE("AhciInitializePort: AhciStopPortEngines done");

    /* AMD Promontory PHY power-on & spin-up. */
    cmd = AHCI_READ_REG(portBase, AHCI_PORT_CMD);
    cmd |= AHCI_PORT_CMD_POD | AHCI_PORT_CMD_SUD;
    AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, cmd);
    ScsiPortStallExecution(1000);

    AHCI_TRACE("AhciInitializePort: checking SSTS (link)");
    ssts = AHCI_READ_REG(portBase, AHCI_PORT_SSTS);
    if ((ssts & 0x0F) != 0x03) {
        AHCI_WRITE_REG(portBase, AHCI_PORT_SCTL, 0x301);
        ScsiPortStallExecution(2000);
        AHCI_WRITE_REG(portBase, AHCI_PORT_SCTL, 0x300);

        AHCI_TRACE("AhciInitializePort: entering SSTS poll loop");
        timeout = 30000;
        while (--timeout) {
            ssts = AHCI_READ_REG(portBase, AHCI_PORT_SSTS);
            if ((ssts & 0x0F) == 0x03) break;
            ScsiPortStallExecution(10);
        }
        AHCI_TRACE("AhciInitializePort: SSTS poll loop exited");
    }

    if ((ssts & 0x0F) != 0x03) {
        AHCI_TRACE("AhciInitializePort: no link, port not present, return FALSE");
        HwInit->Ports[PortNumber].Present = FALSE;
        return FALSE;
    }

    ScsiPortStallExecution(1000);

    AHCI_WRITE_REG(portBase, AHCI_PORT_CLB, HwInit->Ports[PortNumber].CommandListPhysical);
    AHCI_WRITE_REG(portBase, AHCI_PORT_CLBU, HwInit->Ports[PortNumber].CommandListPhysicalUpper);
    AHCI_WRITE_REG(portBase, AHCI_PORT_FB, HwInit->Ports[PortNumber].ReceivedFisPhysical);
    AHCI_WRITE_REG(portBase, AHCI_PORT_FBU, HwInit->Ports[PortNumber].ReceivedFisPhysicalUpper);

    AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
    AHCI_WRITE_REG(portBase, AHCI_PORT_IS, 0xFFFFFFFF);
    AHCI_WRITE_REG(portBase, AHCI_PORT_IE, 0x00000000);

    cmd = AHCI_READ_REG(portBase, AHCI_PORT_CMD);
    cmd |= AHCI_PORT_CMD_FRE;
    AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, cmd);

    AHCI_TRACE("AhciInitializePort: entering TFD poll loop (FRE)");
    timeout = 50000;
    while ((AHCI_READ_REG(portBase, AHCI_PORT_TFD) & 0x88) && --timeout) {
        ScsiPortStallExecution(10);
    }
    AHCI_TRACE("AhciInitializePort: TFD poll loop exited");

    cmd = AHCI_READ_REG(portBase, AHCI_PORT_CMD);
    cmd |= AHCI_PORT_CMD_ST;
    AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, cmd);

    sig = AHCI_READ_REG(portBase, AHCI_PORT_SIG);
    HwInit->Ports[PortNumber].Present = TRUE;
    HwInit->Ports[PortNumber].IsAtapi = (sig == SATA_SIG_ATAPI);
    HwInit->Ports[PortNumber].IdentifyValid = FALSE;

    AHCI_TRACE("AhciInitializePort: calling AhciExecuteIdentify");
    AhciExecuteIdentify(HwInit, PortNumber);
    AHCI_TRACE("AhciInitializePort: exit, return TRUE");
    return TRUE;
}

ULONG DriverEntry(IN PVOID DriverObject, IN PVOID Argument2) {
    HW_INITIALIZATION_DATA initData;
    PUCHAR ptr;
    ULONG i;
    ULONG rc;

    /* Raw, unconditional marker: the literal first thing this function
       does, before any C statement, any function call, any struct
       access, and NOT going through AhciTrace/AhciTraceChar (in case
       that wait-for-THRE loop is itself somehow the problem). Writes
       the single byte '!' straight to COM1's transmit register with
       no readiness check at all -- if this never shows up on the host
       log, DriverEntry is provably never being reached; if it DOES
       show up but nothing else from AHCI_TRACE follows, the problem is
       in AhciTraceChar's wait loop or later. */
    __asm {
        mov dx, 3F8h
        mov al, '!'
        out dx, al
    }

    AHCI_TRACE("DriverEntry: enter");

    ptr = (PUCHAR)&initData;
    for (i = 0; i < sizeof(HW_INITIALIZATION_DATA); i++) ptr[i] = 0;

    initData.HwInitializationDataSize = sizeof(HW_INITIALIZATION_DATA);
    initData.HwInitialize = AhciHwInitialize;
    initData.HwStartIo = AhciStartIo;
    initData.HwInterrupt = AhciInterrupt;
    initData.HwResetBus = AhciResetBus;
    initData.HwFindAdapter = AhciFindAdapter;
    initData.HwAdapterState = AhciAdapterState;
    initData.DeviceExtensionSize = sizeof(HW_DEVICE_EXTENSION);
    initData.AdapterInterfaceType = PCIBus;
    initData.NumberOfAccessRanges = 1;
    initData.MapBuffers = TRUE;
    initData.NeedPhysicalAddresses = TRUE;
    initData.AutoRequestSense = FALSE;
    initData.MultipleRequestPerLu = FALSE;

    AHCI_TRACE("DriverEntry: calling ScsiPortInitialize");
    rc = ScsiPortInitialize(DriverObject, Argument2, &initData, NULL);
    AHCI_TRACE("DriverEntry: ScsiPortInitialize returned, exiting");
    return rc;
}

ULONG AhciFindAdapter(
    IN PVOID DeviceExtension,
    IN PVOID Context,
    IN PVOID BusInformation,
    IN PCHAR ArgumentString,
    IN OUT PPORT_CONFIGURATION_INFORMATION ConfigInfo,
    OUT PBOOLEAN Again
) {
    PHW_DEVICE_EXTENSION hwInit;
    PCI_COMMON_CONFIG pciConfig;
    ULONG bytesRead, busNumber;
    PCI_SLOT_NUMBER pciSlot;
    USHORT pciCmd;
    SCSI_PHYSICAL_ADDRESS basePhys;
    PACCESS_RANGE accessRanges;
    BOOLEAN found;

    AHCI_TRACE("AhciFindAdapter: enter");

    hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;
    accessRanges = *ConfigInfo->AccessRanges;
    found = FALSE;
    *Again = FALSE;
    busNumber = 0;

    if (hwInit->AbarMapped != NULL) {
        AHCI_TRACE("AhciFindAdapter: already mapped, SP_RETURN_FOUND");
        return SP_RETURN_FOUND;
    }

    /* Windows 95's SCSIPORT.PDR already enumerates the PCI bus itself for
       an AdapterInterfaceType=PCIBus miniport and calls HwFindAdapter once
       per physical PCI function, with ConfigInfo->SystemIoBusNumber/
       SlotNumber already pointing at that exact device -- this matches how
       the DDK's own MINIPORT.H documents HalGetBusData/ScsiPortGetBusData
       being used (query the given bus:slot, not sweep the whole bus).
       Never do our own brute-force 256 bus x 32 device x 8 function PCI
       config-space sweep here: on Win95's V86/PCI-BIOS-backed
       ScsiPortGetBusData path this produced a full hard system hang during
       driver install (confirmed on real hardware/VM by the driver author),
       almost certainly because querying bus numbers or slots that don't
       exist on Win95 is far less forgiving than it is on the NT PCI bus
       driver this code was originally written against (see src/NT). If the
       device this HwFindAdapter call was invoked for isn't a claimed AHCI
       controller, just fail this call -- SCSIPORT will call again for the
       next PCI function on its own. */
    AHCI_TRACE("AhciFindAdapter: calling ScsiPortGetBusData");
    pciSlot.u.AsULONG = ConfigInfo->SlotNumber;
    bytesRead = ScsiPortGetBusData(hwInit, PCIConfiguration, ConfigInfo->SystemIoBusNumber,
                                   pciSlot.u.AsULONG, &pciConfig, sizeof(PCI_COMMON_CONFIG));
    AHCI_TRACE("AhciFindAdapter: ScsiPortGetBusData returned");
    if (bytesRead == sizeof(PCI_COMMON_CONFIG) &&
        pciConfig.VendorID != 0xFFFF && pciConfig.VendorID != 0x0000 &&
        AhciPciConfigIsAhci(&pciConfig) &&
        !AhciIsDeviceClaimed(ConfigInfo->SystemIoBusNumber, pciSlot.u.AsULONG)) {
        busNumber = ConfigInfo->SystemIoBusNumber;
        hwInit->PciBus = busNumber;
        hwInit->PciSlot = pciSlot.u.AsULONG;
        found = TRUE;
    }

    if (!found) {
        AHCI_TRACE("AhciFindAdapter: not our device, SP_RETURN_NOT_FOUND");
        return SP_RETURN_NOT_FOUND;
    }

    AHCI_TRACE("AhciFindAdapter: AHCI device matched, claiming");
    AhciClaimDevice(hwInit->PciBus, hwInit->PciSlot);

    pciCmd = (pciConfig.Command | 0x0006) & ~(1 << 10);
    ScsiPortSetBusDataByOffset(hwInit, PCIConfiguration, hwInit->PciBus, hwInit->PciSlot, &pciCmd, 0x04, sizeof(USHORT));
    AHCI_TRACE("AhciFindAdapter: PCI command register updated");

    basePhys.LowPart = pciConfig.u.type0.BaseAddresses[5] & 0xFFFFFFF0;
    basePhys.HighPart = 0;
    if (basePhys.LowPart == 0) {
        AHCI_TRACE("AhciFindAdapter: BAR5 is zero, SP_RETURN_ERROR");
        return SP_RETURN_ERROR;
    }

    /* 64 KB (0x10000) mapping, matching the AHCI MMIO BAR size. */
    accessRanges[0].RangeStart = basePhys;
    accessRanges[0].RangeLength = 0x10000;
    accessRanges[0].RangeInMemory = TRUE;

    /* Win95 KB Q169584 workaround -- see _MapPhysToLinear() above and
       ahcint9x.h for the full explanation. Plain ScsiPortGetDeviceBase()
       is what caused the install-time hard hang; _MapPhysToLinear is
       the proven-working replacement used by nvme2k_9x for the same
       reason (mapping a PCI MMIO BAR on real Windows 95).
       IMPORTANT: MPL_NonCached must be the real VMM value (0x0) -- see
       the fixed #define and its comment in ahcint9x.h. An earlier version
       of this fix had MPL_NonCached wrongly defined as 0x1, which is
       actually MPL_HardwareCoherentCached: that mapped the ABAR as
       CACHED, so AHCI_READ_REG on port status registers (SSTS/TFD/CMD)
       could return stale cached values forever, making every polling
       loop in AhciInitializePort() spin to its full timeout -- which is
       exactly what still looked like a hang/lockup, and is almost
       certainly why the first attempt at this fix did not resolve the
       problem. */
    AHCI_TRACE("AhciFindAdapter: calling _MapPhysToLinear");
    hwInit->AbarMapped = (PUCHAR)_MapPhysToLinear(basePhys.LowPart, 0x10000, MPL_NonCached);
    AHCI_TRACE("AhciFindAdapter: _MapPhysToLinear returned");
    if (hwInit->AbarMapped == NULL || hwInit->AbarMapped == (PUCHAR)0xFFFFFFFF) {
        AHCI_TRACE("AhciFindAdapter: ABAR map failed, SP_RETURN_ERROR");
        hwInit->AbarMapped = NULL;
        return SP_RETURN_ERROR;
    }

    ConfigInfo->SystemIoBusNumber = hwInit->PciBus;
    ConfigInfo->SlotNumber = hwInit->PciSlot;
    hwInit->ActualIrq = pciConfig.u.type0.InterruptLine;
    ConfigInfo->BusInterruptLevel = hwInit->ActualIrq;
    ConfigInfo->BusInterruptVector = hwInit->ActualIrq;
    ConfigInfo->InterruptMode = LevelSensitive;
    ConfigInfo->Master = TRUE;
    ConfigInfo->Dma32BitAddresses = TRUE;

    ConfigInfo->ScatterGather = TRUE;
    ConfigInfo->MaximumTransferLength = 0x20000;
    ConfigInfo->NumberOfPhysicalBreaks = MAX_PRDT_ENTRIES;
    ConfigInfo->MaximumNumberOfTargets = MAX_AHCI_PORTS;
    ConfigInfo->NumberOfBuses = 1;
    ConfigInfo->InitiatorBusId[0] = 7;

    AHCI_TRACE("AhciFindAdapter: calling AhciAllocateDma");
    if (!AhciAllocateDma(hwInit, ConfigInfo)) {
        AHCI_TRACE("AhciFindAdapter: AhciAllocateDma failed, SP_RETURN_ERROR");
        return SP_RETURN_ERROR;
    }

    AHCI_TRACE("AhciFindAdapter: exit, SP_RETURN_FOUND");
    return SP_RETURN_FOUND;
}

BOOLEAN AhciHwInitialize(IN PVOID DeviceExtension) {
    PHW_DEVICE_EXTENSION hwInit;
    ULONG ghc, pi, port, timeout;
    ULONG cap2;

    AHCI_TRACE("AhciHwInitialize: enter");

    hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;

    /* BIOS/OS handoff: request OS ownership (OOS bit). */
    AHCI_TRACE("AhciHwInitialize: reading CAP2");
    cap2 = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_CAP2);
    if (cap2 & 0x01) {
        ULONG bohc = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_BOHC);
        bohc |= 0x02;
        AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_BOHC, bohc);

        AHCI_TRACE("AhciHwInitialize: entering BOHC poll loop");
        timeout = 50000;
        while ((AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_BOHC) & 0x01) && --timeout) {
            ScsiPortStallExecution(10);
        }
        AHCI_TRACE("AhciHwInitialize: BOHC poll loop exited");
    }

    ghc = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_GHC);
    ghc |= AHCI_GHC_AE;
    AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_GHC, ghc);
    AHCI_TRACE("AhciHwInitialize: AE bit set");

    pi = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_PI);
    hwInit->PortsImplemented = pi;
    AHCI_TRACE("AhciHwInitialize: PI read, starting port loop");

    for (port = 0; port < MAX_AHCI_PORTS; port++) {
        if (pi & (1 << port)) {
            AHCI_TRACE("AhciHwInitialize: calling AhciInitializePort");
            AhciInitializePort(hwInit, port);
            AHCI_TRACE("AhciHwInitialize: AhciInitializePort returned");
        }
    }
    AHCI_TRACE("AhciHwInitialize: port loop done");

    for (port = 0; port < MAX_AHCI_PORTS; port++) {
        if (pi & (1 << port)) {
            PUCHAR pb = AHCI_PORT_BASE(hwInit->AbarMapped, port);
            AHCI_WRITE_REG(pb, AHCI_PORT_IS, 0xFFFFFFFF);
            AHCI_WRITE_REG(pb, AHCI_PORT_SERR, 0xFFFFFFFF);
            AHCI_WRITE_REG(pb, AHCI_PORT_IE, 0x00000000);
        }
    }
    AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_IS, 0xFFFFFFFF);

    ghc = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_GHC);
    ghc |= AHCI_GHC_IE;
    AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_GHC, ghc);
    AHCI_TRACE("AhciHwInitialize: exit, return TRUE");

    return TRUE;
}

BOOLEAN AhciStartIo(IN PVOID DeviceExtension, IN PSCSI_REQUEST_BLOCK Srb) {
    PHW_DEVICE_EXTENSION hwInit;
    BOOLEAN pending;

    hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;

    if (hwInit->ActiveSrb != NULL) {
        Srb->SrbStatus = SRB_STATUS_BUSY;
        ScsiPortNotification(NextRequest, hwInit, NULL);
        return TRUE;
    }

    hwInit->ActiveSrb = Srb;
    pending = AhciSatlProcessSrb(hwInit, Srb);

    if (!pending) {
        hwInit->ActiveSrb = NULL;
        ScsiPortNotification(RequestComplete, hwInit, Srb);
        ScsiPortNotification(NextRequest, hwInit, NULL);
    }

    return TRUE;
}

BOOLEAN AhciInterrupt(IN PVOID DeviceExtension) {
    PHW_DEVICE_EXTENSION hwInit;
    ULONG hbaIs, portIs, p;
    PUCHAR portBase;
    BOOLEAN handled;

    hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;
    handled = FALSE;

    hbaIs = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_IS);
    if (hbaIs == 0) return FALSE;

    for (p = 0; p < MAX_AHCI_PORTS; p++) {
        if (hbaIs & (1 << p)) {
            portBase = AHCI_PORT_BASE(hwInit->AbarMapped, p);
            portIs = AHCI_READ_REG(portBase, AHCI_PORT_IS);

            AHCI_WRITE_REG(portBase, AHCI_PORT_IS, portIs);
            AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
            AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_IS, (1UL << p));

            handled = TRUE;
        }
    }

    return handled;
}

BOOLEAN AhciResetBus(IN PVOID HwDeviceExtension, IN ULONG PathId) {
    PHW_DEVICE_EXTENSION hwInit;
    ULONG port;

    hwInit = (PHW_DEVICE_EXTENSION)HwDeviceExtension;
    for (port = 0; port < MAX_AHCI_PORTS; port++) {
        if (hwInit->Ports[port].Present) {
            PUCHAR portBase = AHCI_PORT_BASE(hwInit->AbarMapped, port);
            AhciStopPortEngines(portBase);
        }
    }
    return TRUE;
}