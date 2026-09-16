#include "ahcint.h"

BOOLEAN AhciHwInitialize(IN PVOID DeviceExtension);
BOOLEAN AhciStartIo(IN PVOID DeviceExtension, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN AhciInterrupt(IN PVOID DeviceExtension);
BOOLEAN AhciResetBus(IN PVOID HwDeviceExtension, IN ULONG PathId);
ULONG AhciFindAdapter(IN PVOID DeviceExtension, IN PVOID Context, IN PVOID BusInformation, IN PCHAR ArgumentString, IN OUT PPORT_CONFIGURATION_INFORMATION ConfigInfo, OUT PBOOLEAN Again);
BOOLEAN AhciSatlProcessSrb(IN PHW_DEVICE_EXTENSION DeviceExtension, IN PSCSI_REQUEST_BLOCK Srb);

// Carve up the uncached DMA extension into per-port regions (command list,
// received-FIS, command table, IDENTIFY buffer), each aligned to its
// AHCI-mandated boundary.
static BOOLEAN AhciAllocateDma(IN PHW_DEVICE_EXTENSION HwInit, IN PPORT_CONFIGURATION_INFORMATION ConfigInfo) {
    ULONG uncachedSize = sizeof(AHCI_DMA_RESOURCES);
    ULONG allocLen = uncachedSize;
    PUCHAR virtPtr;
    ULONG physPtr;
    ULONG port;
    ULONG alignOffset;

    if (HwInit->DmaArea != NULL) return TRUE;

    HwInit->DmaArea = (PAHCI_DMA_RESOURCES)ScsiPortGetUncachedExtension(HwInit, ConfigInfo, uncachedSize);
    if (!HwInit->DmaArea) {
        AHCI_DBG_MSG("AhciAllocateDma: ScsiPortGetUncachedExtension failed");
        return FALSE;
    }

    HwInit->DmaAreaPhysical = ScsiPortGetPhysicalAddress(HwInit, NULL, HwInit->DmaArea, &allocLen);
    if (HwInit->DmaAreaPhysical.LowPart == 0 && HwInit->DmaAreaPhysical.HighPart == 0) {
        AHCI_DBG_MSG("AhciAllocateDma: failed to resolve physical address of DMA area");
        return FALSE;
    }

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

// Clear ST (start) and FRE (FIS receive enable), waiting for the engines
// to report idle (CR/FR clear) before returning. Required before touching
// CLB/FB or re-initializing a port.
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
        if (timeout == 0) {
            AHCI_DBG_MSG("AhciStopPortEngines: timeout waiting for CR to clear");
        }
    }

    if (cmd & AHCI_PORT_CMD_FRE) {
        cmd &= ~AHCI_PORT_CMD_FRE;
        AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, cmd);

        timeout = 50000;
        while ((AHCI_READ_REG(portBase, AHCI_PORT_CMD) & AHCI_PORT_CMD_FR) && --timeout) {
            ScsiPortStallExecution(10);
        }
        if (timeout == 0) {
            AHCI_DBG_MSG("AhciStopPortEngines: timeout waiting for FR to clear");
        }
    }
}

// Issue IDENTIFY DEVICE (or IDENTIFY PACKET DEVICE for ATAPI) on the given
// port and copy the 512-byte result into Ports[PortNumber].IdentifyData.
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

    while (--timeout) {
        if (!(AHCI_READ_REG(portBase, AHCI_PORT_CI) & 1)) break;
        tfd = AHCI_READ_REG(portBase, AHCI_PORT_TFD);
        if (tfd & 0x01) break;
        ScsiPortStallExecution(10);
    }

    tfd = AHCI_READ_REG(portBase, AHCI_PORT_TFD);
    id = (PUSHORT)HwInit->Ports[PortNumber].IdentifyDmaBuffer;

    if (!(AHCI_READ_REG(portBase, AHCI_PORT_CI) & 1) && !(tfd & 0x01) && (id[27] != 0 || id[0] != 0)) {
        HwInit->Ports[PortNumber].IdentifyValid = TRUE;
        for (i = 0; i < 512; i++) {
            HwInit->Ports[PortNumber].IdentifyData[i] = HwInit->Ports[PortNumber].IdentifyDmaBuffer[i];
        }
        AHCI_DBG_LOG("[AHCINT] AhciExecuteIdentify: port %u IDENTIFY OK\n", PortNumber);
    } else {
        HwInit->Ports[PortNumber].IdentifyValid = FALSE;
        AHCI_DBG_LOG("[AHCINT] AhciExecuteIdentify: port %u IDENTIFY failed (TFD=0x%x)\n", PortNumber, tfd);
    }

    AHCI_WRITE_REG(portBase, AHCI_PORT_IS, 0xFFFFFFFF);
    AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
    AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_IS, (1UL << PortNumber));
}

// Bring one port online: PHY power-up/spin-up, wait for device detection,
// program CLB/FB, enable FRE/ST, then run IDENTIFY.
static BOOLEAN AhciInitializePort(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG PortNumber) {
    PUCHAR portBase;
    ULONG ssts, sig, cmd, timeout;

    portBase = AHCI_PORT_BASE(HwInit->AbarMapped, PortNumber);
    AhciStopPortEngines(portBase);

    // Power-on and spin-up the PHY (POD/SUD) before checking device detection
    cmd = AHCI_READ_REG(portBase, AHCI_PORT_CMD);
    cmd |= AHCI_PORT_CMD_POD | AHCI_PORT_CMD_SUD;
    AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, cmd);
    ScsiPortStallExecution(1000);

    ssts = AHCI_READ_REG(portBase, AHCI_PORT_SSTS);
    if ((ssts & 0x0F) != 0x03) {
        // No device detected yet — force a COMRESET via SCTL.DET, then release it
        AHCI_WRITE_REG(portBase, AHCI_PORT_SCTL, 0x301);
        ScsiPortStallExecution(2000);
        AHCI_WRITE_REG(portBase, AHCI_PORT_SCTL, 0x300);

        timeout = 30000;
        while (--timeout) {
            ssts = AHCI_READ_REG(portBase, AHCI_PORT_SSTS);
            if ((ssts & 0x0F) == 0x03) break;
            ScsiPortStallExecution(10);
        }
    }

    if ((ssts & 0x0F) != 0x03) {
        AHCI_DBG_LOG("[AHCINT] AhciInitializePort: port %u no device detected (SSTS=0x%x)\n", PortNumber, ssts);
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

    timeout = 50000;
    while ((AHCI_READ_REG(portBase, AHCI_PORT_TFD) & 0x88) && --timeout) {
        ScsiPortStallExecution(10);
    }
    if (timeout == 0) {
        AHCI_DBG_LOG("[AHCINT] AhciInitializePort: port %u timeout waiting for BSY/DRQ to clear\n", PortNumber);
    }

    cmd = AHCI_READ_REG(portBase, AHCI_PORT_CMD);
    cmd |= AHCI_PORT_CMD_ST;
    AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, cmd);

    sig = AHCI_READ_REG(portBase, AHCI_PORT_SIG);
    HwInit->Ports[PortNumber].Present = TRUE;
    HwInit->Ports[PortNumber].IsAtapi = (sig == SATA_SIG_ATAPI);
    HwInit->Ports[PortNumber].IdentifyValid = FALSE;

    AHCI_DBG_LOG("[AHCINT] AhciInitializePort: port %u present, sig=0x%x, atapi=%u\n",
                 PortNumber, sig, (ULONG)HwInit->Ports[PortNumber].IsAtapi);

    AhciExecuteIdentify(HwInit, PortNumber);
    return TRUE;
}

// Miniport entry point — registers our callbacks with ScsiPort and lets it
// drive AhciFindAdapter for enumeration.
ULONG DriverEntry(IN PVOID DriverObject, IN PVOID Argument2) {
    HW_INITIALIZATION_DATA initData;
    PUCHAR ptr;
    ULONG i;

    AHCI_DBG_MSG("DriverEntry: loading AHCI miniport");

    ptr = (PUCHAR)&initData;
    for (i = 0; i < sizeof(HW_INITIALIZATION_DATA); i++) ptr[i] = 0;

    initData.HwInitializationDataSize = sizeof(HW_INITIALIZATION_DATA);
    initData.HwInitialize = AhciHwInitialize;
    initData.HwStartIo = AhciStartIo;
    initData.HwInterrupt = AhciInterrupt;
    initData.HwResetBus = AhciResetBus;
    initData.HwFindAdapter = AhciFindAdapter;
    initData.DeviceExtensionSize = sizeof(HW_DEVICE_EXTENSION);
    initData.AdapterInterfaceType = PCIBus;
    initData.NumberOfAccessRanges = 1;
    initData.MapBuffers = TRUE;
    initData.NeedPhysicalAddresses = TRUE;
    initData.AutoRequestSense = TRUE;
    initData.MultipleRequestPerLu = FALSE;

    return ScsiPortInitialize(DriverObject, Argument2, &initData, NULL);
}

// Scan PCI configuration space for the first AHCI-class controller (class
// 01/06/01), map its ABAR (BAR5), and fill in ConfigInfo for ScsiPort.
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
    ULONG bytesRead, busNumber, deviceNumber, funcNumber;
    PCI_SLOT_NUMBER pciSlot;
    USHORT pciCmd;
    SCSI_PHYSICAL_ADDRESS basePhys;
    PACCESS_RANGE accessRanges;
    BOOLEAN found;

    hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;
    accessRanges = *ConfigInfo->AccessRanges;
    found = FALSE;
    *Again = FALSE;

    if (hwInit->AbarMapped != NULL) return SP_RETURN_FOUND;

    AHCI_DBG_MSG("AhciFindAdapter: scanning PCI bus for AHCI controller");

    // Match on standard PCI class code only: 01 (Mass Storage) / 06 (AHCI) / 01 (Programming Interface)
    for (busNumber = 0; busNumber < 256; busNumber++) {
        for (deviceNumber = 0; deviceNumber < 32; deviceNumber++) {
            for (funcNumber = 0; funcNumber < 8; funcNumber++) {
                pciSlot.u.AsULONG = 0;
                pciSlot.u.bits.DeviceNumber = deviceNumber;
                pciSlot.u.bits.FunctionNumber = funcNumber;

                bytesRead = ScsiPortGetBusData(hwInit, PCIConfiguration, busNumber, pciSlot.u.AsULONG, &pciConfig, sizeof(PCI_COMMON_CONFIG));
                if (bytesRead != sizeof(PCI_COMMON_CONFIG) || pciConfig.VendorID == 0xFFFF || pciConfig.VendorID == 0x0000) {
                    if (funcNumber == 0) break;
                    continue;
                }

                if (pciConfig.BaseClass == PCI_CLASS_MASS_STORAGE &&
                    pciConfig.SubClass == PCI_SUBCLASS_AHCI &&
                    pciConfig.ProgIf == PCI_PROGIF_AHCI) {
                    
                    hwInit->PciBus = busNumber;
                    hwInit->PciSlot = pciSlot.u.AsULONG;
                    found = TRUE;
                    break;
                }
            }
            if (found) break;
        }
        if (found) break;
    }

    if (!found) {
        AHCI_DBG_MSG("AhciFindAdapter: no AHCI controller found on the PCI bus");
        return SP_RETURN_NOT_FOUND;
    }

    AHCI_DBG_LOG("[AHCINT] AhciFindAdapter: AHCI controller found at bus %u, slot 0x%x\n",
                 hwInit->PciBus, hwInit->PciSlot);

    pciCmd = (pciConfig.Command | 0x0006) & ~(1 << 10);
    ScsiPortSetBusDataByOffset(hwInit, PCIConfiguration, hwInit->PciBus, hwInit->PciSlot, &pciCmd, 0x04, sizeof(USHORT));

    basePhys.LowPart = pciConfig.u.type0.BaseAddresses[5] & 0xFFFFFFF0;
    basePhys.HighPart = 0;
    if (basePhys.LowPart == 0) {
        AHCI_DBG_MSG("AhciFindAdapter: BAR5 (ABAR) is zero");
        return SP_RETURN_ERROR;
    }

    // Map a fixed 64 KB (0x10000) MMIO window for the ABAR
    accessRanges[0].RangeStart = basePhys;
    accessRanges[0].RangeLength = 0x10000;
    accessRanges[0].RangeInMemory = TRUE;

    hwInit->AbarMapped = (PUCHAR)ScsiPortGetDeviceBase(hwInit, PCIBus, hwInit->PciBus, basePhys, 0x10000, FALSE);
    if (!hwInit->AbarMapped) {
        AHCI_DBG_MSG("AhciFindAdapter: ScsiPortGetDeviceBase failed to map ABAR");
        return SP_RETURN_ERROR;
    }

    // Report bus/slot/IRQ back to ScsiPort/HAL
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

    if (!AhciAllocateDma(hwInit, ConfigInfo)) return SP_RETURN_ERROR;

    return SP_RETURN_FOUND;
}

// HwInitialize callback: BIOS/OS handoff, enable AHCI mode, bring up each
// implemented port, clear pending interrupts, then enable HBA interrupts.
BOOLEAN AhciHwInitialize(IN PVOID DeviceExtension) {
    PHW_DEVICE_EXTENSION hwInit;
    ULONG ghc, pi, port, timeout;
    ULONG cap2;

    hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;

    AHCI_DBG_MSG("AhciHwInitialize: starting HBA initialization");

    // BIOS/OS handoff control (BOHC) — request OS ownership if supported
    cap2 = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_CAP2);
    if (cap2 & 0x01) {
        ULONG bohc = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_BOHC);
        bohc |= 0x02; // OOS (OS Ownership Set)
        AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_BOHC, bohc);

        timeout = 50000;
        while ((AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_BOHC) & 0x01) && --timeout) {
            ScsiPortStallExecution(10);
        }
        if (timeout == 0) {
            AHCI_DBG_MSG("AhciHwInitialize: BIOS/OS handoff timed out");
        }
    }

    ghc = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_GHC);
    ghc |= AHCI_GHC_AE;
    AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_GHC, ghc);

    pi = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_PI);
    hwInit->PortsImplemented = pi;

    AHCI_DBG_LOG("[AHCINT] AhciHwInitialize: PortsImplemented=0x%x\n", pi);

    for (port = 0; port < MAX_AHCI_PORTS; port++) {
        if (pi & (1 << port)) {
            AhciInitializePort(hwInit, port);
        }
    }

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

    AHCI_DBG_MSG("AhciHwInitialize: HBA initialization complete, interrupts enabled");

    return TRUE;
}

// HwStartIo callback: reject re-entrant requests while one SRB is active,
// otherwise hand the SRB to the SCSI-to-ATA translation layer.
BOOLEAN AhciStartIo(IN PVOID DeviceExtension, IN PSCSI_REQUEST_BLOCK Srb) {
    PHW_DEVICE_EXTENSION hwInit;
    BOOLEAN pending;

    hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;

    if (hwInit->ActiveSrb != NULL) {
        AHCI_DBG_MSG("AhciStartIo: adapter busy, returning SRB_STATUS_BUSY");
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

// HwInterrupt callback: identify which ports raised IS, clear their status
// bits, and acknowledge at the HBA level.
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

// HwResetBus callback: stop the command/FIS-receive engines on every
// present port. Does not attempt a full COMRESET/re-enumeration.
BOOLEAN AhciResetBus(IN PVOID HwDeviceExtension, IN ULONG PathId) {
    PHW_DEVICE_EXTENSION hwInit;
    ULONG port;

    hwInit = (PHW_DEVICE_EXTENSION)HwDeviceExtension;

    AHCI_DBG_LOG("[AHCINT] AhciResetBus: resetting PathId %u\n", PathId);

    for (port = 0; port < MAX_AHCI_PORTS; port++) {
        if (hwInit->Ports[port].Present) {
            PUCHAR portBase = AHCI_PORT_BASE(hwInit->AbarMapped, port);
            AhciStopPortEngines(portBase);
        }
    }
    return TRUE;
}