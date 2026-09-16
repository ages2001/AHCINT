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
    ULONG uncachedSize;
    ULONG allocLen;
    PUCHAR virtPtr;
    ULONG physPtr;
    ULONG port;
    ULONG alignOffset;

    if (HwInit->DmaArea != NULL) return TRUE;

    uncachedSize = sizeof(AHCI_DMA_RESOURCES);
    allocLen = uncachedSize;

    HwInit->DmaArea = (PAHCI_DMA_RESOURCES)ScsiPortGetUncachedExtension(HwInit, ConfigInfo, uncachedSize);
    if (!HwInit->DmaArea) {
        AHCI_DBG_MSG("AhciAllocateDma: failed to allocate uncached extension!");
        return FALSE;
    }

    HwInit->DmaAreaPhysical = ScsiPortGetPhysicalAddress(HwInit, NULL, HwInit->DmaArea, &allocLen);
    if (HwInit->DmaAreaPhysical.LowPart == 0 && HwInit->DmaAreaPhysical.HighPart == 0) {
        AHCI_DBG_MSG("AhciAllocateDma: invalid physical address!");
        return FALSE;
    }

    ZeroMemoryBytes(HwInit->DmaArea, sizeof(AHCI_DMA_RESOURCES));

    virtPtr = (PUCHAR)HwInit->DmaArea->RawBuffer;
    physPtr = HwInit->DmaAreaPhysical.LowPart;

    for (port = 0; port < MAX_SUPPORTED_PORTS; port++) {
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
// to report idle (CR/FR clear) before returning. Falls back to Command
// List Override (CLO) if the HBA supports it and CR won't clear on its own.
VOID AhciStopPortEngines(IN PHW_DEVICE_EXTENSION HwInit, IN PUCHAR portBase) {
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

        if ((AHCI_READ_REG(portBase, AHCI_PORT_CMD) & AHCI_PORT_CMD_CR) &&
            (HwInit->HbaCapabilities & AHCI_CAP_SCLO)) {
            AHCI_DBG_MSG("AhciStopPortEngines: CR stuck, falling back to CLO");
            cmd = AHCI_READ_REG(portBase, AHCI_PORT_CMD);
            AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, cmd | AHCI_PORT_CMD_CLO);
            timeout = 5000;
            while ((AHCI_READ_REG(portBase, AHCI_PORT_CMD) & AHCI_PORT_CMD_CLO) && --timeout) {
                ScsiPortStallExecution(10);
            }
            if (timeout == 0) {
                AHCI_DBG_MSG("AhciStopPortEngines: CLO timed out");
            }
        }
    }

    cmd = AHCI_READ_REG(portBase, AHCI_PORT_CMD);
    if (cmd & AHCI_PORT_CMD_FRE) {
        cmd &= ~AHCI_PORT_CMD_FRE;
        AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, cmd);

        timeout = 50000;
        while ((AHCI_READ_REG(portBase, AHCI_PORT_CMD) & AHCI_PORT_CMD_FR) && --timeout) {
            ScsiPortStallExecution(10);
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
        AHCI_DBG_LOG("Port %d: IDENTIFY OK", PortNumber);
    } else {
        HwInit->Ports[PortNumber].IdentifyValid = FALSE;
        AHCI_DBG_LOG("Port %d: IDENTIFY failed, CI=0x%08X TFD=0x%08X", PortNumber, AHCI_READ_REG(portBase, AHCI_PORT_CI), tfd);
    }

    AHCI_WRITE_REG(portBase, AHCI_PORT_IS, 0xFFFFFFFF);
    AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
    AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_IS, (1UL << PortNumber));
}

// Bring one port online: PHY power-up/spin-up, wait for device detection
// (with a bounded COMRESET retry loop), program CLB/FB, enable FRE/ST,
// then run IDENTIFY.
static BOOLEAN AhciInitializePort(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG PortNumber) {
    PUCHAR portBase;
    ULONG ssts, sig, cmd, timeout, retry;

    portBase = AHCI_PORT_BASE(HwInit->AbarMapped, PortNumber);
    AhciStopPortEngines(HwInit, portBase);

    cmd = AHCI_READ_REG(portBase, AHCI_PORT_CMD);
    cmd |= AHCI_PORT_CMD_POD | AHCI_PORT_CMD_SUD;
    AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, cmd);
    ScsiPortStallExecution(1000);

    /* Quick check: if DET is completely zero, no device/cable is present at all */
    ssts = AHCI_READ_REG(portBase, AHCI_PORT_SSTS);
    if ((ssts & 0x0F) == 0x00) {
        /* Port is empty, bail out immediately without waiting */
        AHCI_DBG_LOG("Port %d: no device detected (SSTS=0x%X)", PortNumber, ssts);
        HwInit->Ports[PortNumber].Present = FALSE;
        return FALSE;
    }

    /* A link is being negotiated: retry with a short, bounded loop
       (3 retries instead of 10, shorter per-iteration stall) */
    if ((ssts & 0x0F) != 0x03) {
        for (retry = 0; retry < 3; retry++) {
            AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
            AHCI_WRITE_REG(portBase, AHCI_PORT_SCTL, 0x301);
            ScsiPortStallExecution(1000);
            AHCI_WRITE_REG(portBase, AHCI_PORT_SCTL, 0x300);

            timeout = 30;
            while (--timeout) {
                ssts = AHCI_READ_REG(portBase, AHCI_PORT_SSTS);
                if ((ssts & 0x0F) == 0x03) break;
                ScsiPortStallExecution(5000); /* 5ms instead of 10ms */
            }
            if ((ssts & 0x0F) == 0x03) break;
        }
    }

    if ((ssts & 0x0F) != 0x03) {
        AHCI_DBG_LOG("Port %d: link not established after retries (SSTS=0x%X)", PortNumber, ssts);
        HwInit->Ports[PortNumber].Present = FALSE;
        return FALSE;
    }

    AHCI_WRITE_REG(portBase, AHCI_PORT_CLB, HwInit->Ports[PortNumber].CommandListPhysical);
    AHCI_WRITE_REG(portBase, AHCI_PORT_CLBU, HwInit->Ports[PortNumber].CommandListPhysicalUpper);
    AHCI_WRITE_REG(portBase, AHCI_PORT_FB, HwInit->Ports[PortNumber].ReceivedFisPhysical);
    AHCI_WRITE_REG(portBase, AHCI_PORT_FBU, HwInit->Ports[PortNumber].ReceivedFisPhysicalUpper);

    AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
    AHCI_WRITE_REG(portBase, AHCI_PORT_IS, 0xFFFFFFFF);

    /* Enable port interrupts */
    AHCI_WRITE_REG(portBase, AHCI_PORT_IE, AHCI_PORT_IE_DEFAULT);

    cmd = AHCI_READ_REG(portBase, AHCI_PORT_CMD);
    cmd |= AHCI_PORT_CMD_FRE;
    AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, cmd);

    timeout = 15000;
    while ((AHCI_READ_REG(portBase, AHCI_PORT_TFD) & 0x88) && --timeout) {
        ScsiPortStallExecution(100);
    }
    if (timeout == 0) {
        AHCI_DBG_LOG("Port %d: timeout waiting for BSY/DRQ to clear", PortNumber);
    }

    cmd = AHCI_READ_REG(portBase, AHCI_PORT_CMD);
    cmd |= AHCI_PORT_CMD_ST;
    AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, cmd);

    sig = AHCI_READ_REG(portBase, AHCI_PORT_SIG);
    HwInit->Ports[PortNumber].Present = TRUE;
    HwInit->Ports[PortNumber].IsAtapi = (sig == SATA_SIG_ATAPI);
    HwInit->Ports[PortNumber].IdentifyValid = FALSE;

    AHCI_DBG_LOG("Port %d: present, sig=0x%08X, atapi=%d", PortNumber, sig, (int)HwInit->Ports[PortNumber].IsAtapi);

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

// Scan PCI configuration space for an AHCI-class controller (class
// 01/06/01) or a known AMD/Intel AHCI device ID, map its ABAR (BAR5), and
// fill in ConfigInfo for ScsiPort. Falls back to a pre-reported access
// range if the manual scan doesn't find a usable BAR.
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
    ULONG bytesRead;
    USHORT pciCmd;
    SCSI_PHYSICAL_ADDRESS basePhys;
    PACCESS_RANGE accessRanges;
    ULONG b, d, f;
    ULONG targetBus, targetSlot;
    BOOLEAN found;
    ULONG cap;
    ULONG barId;

    hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;
    accessRanges = *ConfigInfo->AccessRanges;
    found = FALSE;
    targetBus = 0;
    targetSlot = 0;
    barId = 5;

    *Again = FALSE;

    if (hwInit->AbarMapped != NULL) return SP_RETURN_FOUND;

    AHCI_DBG_MSG("AhciFindAdapter: scanning PCI bus for AHCI controller");

    for (b = 0; b < 255; b++) {
        for (d = 0; d < 32; d++) {
            for (f = 0; f < 8; f++) {
                ULONG s = (d << 3) | (f & 0x07);

                bytesRead = ScsiPortGetBusData(hwInit, PCIConfiguration, b, s, &pciConfig, sizeof(PCI_COMMON_CONFIG));
                if (bytesRead != sizeof(PCI_COMMON_CONFIG) || pciConfig.VendorID == 0xFFFF || pciConfig.VendorID == 0x0000) {
                    continue;
                }

                if ((pciConfig.BaseClass == PCI_CLASS_MASS_STORAGE &&
                     pciConfig.SubClass == PCI_SUBCLASS_AHCI &&
                     pciConfig.ProgIf == PCI_PROGIF_AHCI) ||
                    (pciConfig.VendorID == 0x1022 && (pciConfig.DeviceID == 0x7901 || pciConfig.DeviceID == 0x43EB || pciConfig.DeviceID == 0x43C8)) ||
                    (pciConfig.VendorID == 0x8086 && (pciConfig.DeviceID == 0x2829 || pciConfig.DeviceID == 0x2828 || pciConfig.DeviceID == 0x2922 || pciConfig.DeviceID == 0x2681))) {
                    
                    targetBus = b;
                    targetSlot = s;
                    basePhys.LowPart = pciConfig.u.type0.BaseAddresses[barId] & 0xFFFFFFF0;
                    basePhys.HighPart = 0;
                    found = TRUE;
                    break;
                }
            }
            if (found) break;
        }
        if (found) break;
    }

    if (!found || basePhys.LowPart == 0) {
        AHCI_DBG_MSG("AhciFindAdapter: manual scan found no BAR, trying reported access ranges");
        if (ConfigInfo->NumberOfAccessRanges > 5 && accessRanges != NULL) {
            basePhys = accessRanges[5].RangeStart;
            found = TRUE;
        } else if (ConfigInfo->NumberOfAccessRanges > 0 && accessRanges != NULL) {
            basePhys = accessRanges[0].RangeStart;
            found = TRUE;
        }
    }

    if (!found || basePhys.LowPart == 0) {
        AHCI_DBG_MSG("AhciFindAdapter: no AHCI controller found");
        return SP_RETURN_NOT_FOUND;
    }

    AHCI_DBG_LOG("AhciFindAdapter: AHCI controller found at bus %d, slot 0x%X", targetBus, targetSlot);

    hwInit->PciBus = targetBus;
    hwInit->PciSlot = targetSlot;

    bytesRead = ScsiPortGetBusData(hwInit, PCIConfiguration, targetBus, targetSlot, &pciConfig, sizeof(PCI_COMMON_CONFIG));
    pciCmd = (pciConfig.Command | 0x0006) & ~(1 << 10);
    ScsiPortSetBusDataByOffset(hwInit, PCIConfiguration, targetBus, targetSlot, &pciCmd, 0x04, sizeof(USHORT));

    hwInit->AbarMapped = (PUCHAR)ScsiPortGetDeviceBase(
        hwInit, 
        ConfigInfo->AdapterInterfaceType, 
        targetBus, 
        basePhys, 
        0x10000, 
        FALSE
    );

    if (!hwInit->AbarMapped) {
        hwInit->AbarMapped = (PUCHAR)ScsiPortGetDeviceBase(
            hwInit, 
            PCIBus, 
            targetBus, 
            basePhys, 
            0x10000, 
            FALSE
        );
    }

    if (!hwInit->AbarMapped) {
        AHCI_DBG_MSG("AhciFindAdapter: ScsiPortGetDeviceBase failed to map ABAR");
        return SP_RETURN_ERROR;
    }

    if (ConfigInfo->NumberOfAccessRanges > barId && accessRanges != NULL) {
        accessRanges[barId].RangeStart = basePhys;
        accessRanges[barId].RangeLength = 0x10000;
        accessRanges[barId].RangeInMemory = TRUE;
    }

    ConfigInfo->SystemIoBusNumber = targetBus;
    ConfigInfo->SlotNumber = targetSlot;

    if (ConfigInfo->BusInterruptLevel == 0 || ConfigInfo->BusInterruptLevel == 0xFFFFFFFF) {
        ConfigInfo->BusInterruptLevel = pciConfig.u.type0.InterruptLine;
        ConfigInfo->BusInterruptVector = pciConfig.u.type0.InterruptLine;
    }
    hwInit->ActualIrq = (UCHAR)ConfigInfo->BusInterruptLevel;

    ConfigInfo->InterruptMode = LevelSensitive;
    ConfigInfo->Master = TRUE;
    ConfigInfo->Dma32BitAddresses = TRUE;

    cap = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_CAP);
    hwInit->HbaCapabilities = cap;

    if (cap & AHCI_CAP_S64A) {
        ConfigInfo->Dma64BitAddresses = TRUE;
    } else {
        ConfigInfo->Dma64BitAddresses = FALSE;
    }

    ConfigInfo->ScatterGather = TRUE;
    ConfigInfo->MaximumTransferLength = 0x20000;
    ConfigInfo->NumberOfPhysicalBreaks = MAX_PRDT_ENTRIES;
    ConfigInfo->MaximumNumberOfTargets = MAX_SUPPORTED_PORTS;
    ConfigInfo->NumberOfBuses = 1;
    ConfigInfo->InitiatorBusId[0] = 7;

    if (!AhciAllocateDma(hwInit, ConfigInfo)) return SP_RETURN_ERROR;

    return SP_RETURN_FOUND;
}

// HwInitialize callback: BIOS/OS handoff, HBA reset, enable AHCI mode,
// bring up each implemented port, clear pending interrupts, then enable
// HBA interrupts.
BOOLEAN AhciHwInitialize(IN PVOID DeviceExtension) {
    PHW_DEVICE_EXTENSION hwInit;
    ULONG ghc, pi, port, timeout;
    ULONG cap2;
    ULONG i;

    hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;

    AHCI_DBG_MSG("AhciHwInitialize: starting HBA initialization");

    // BIOS/OS handoff control (BOHC) — request OS ownership if supported
    cap2 = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_CAP2);
    if (cap2 & 0x01) {
        ULONG bohc = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_BOHC);
        bohc |= AHCI_BOHC_OOS;
        AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_BOHC, bohc);

        timeout = 50000;
        while ((AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_BOHC) & AHCI_BOHC_BB) && --timeout) {
            ScsiPortStallExecution(10);
        }
        if (timeout == 0) {
            AHCI_DBG_MSG("AhciHwInitialize: BIOS/OS handoff timed out");
        }
    }

    ghc = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_GHC);
    AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_GHC, (ghc | AHCI_GHC_AE) & ~AHCI_GHC_IE);
    ScsiPortStallExecution(100);

    // Full HBA reset (HR) — wait for the HBA to self-clear the bit
    ghc = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_GHC);
    AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_GHC, ghc | AHCI_GHC_HR);
    timeout = 1000;
    while ((AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_GHC) & AHCI_GHC_HR) && --timeout) {
        ScsiPortStallExecution(1000);
    }
    if (timeout == 0) {
        AHCI_DBG_MSG("AhciHwInitialize: HBA reset (HR) timed out");
    }

    // HBA reset clears AE — re-enable AHCI mode, retrying a few times
    for (i = 0; i < 5; i++) {
        ghc = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_GHC);
        if (!(ghc & AHCI_GHC_AE)) {
            AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_GHC, ghc | AHCI_GHC_AE);
            ScsiPortStallExecution(1000);
        } else {
            break;
        }
    }

    pi = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_PI);
    hwInit->PortsImplemented = pi;

    AHCI_DBG_LOG("AhciHwInitialize: PortsImplemented=0x%08X", pi);

    for (port = 0; port < MAX_SUPPORTED_PORTS; port++) {
        if (pi & (1 << port)) {
            AhciInitializePort(hwInit, port);
        }
    }

    for (port = 0; port < MAX_SUPPORTED_PORTS; port++) {
        if (pi & (1 << port)) {
            PUCHAR pb = AHCI_PORT_BASE(hwInit->AbarMapped, port);
            AHCI_WRITE_REG(pb, AHCI_PORT_IS, 0xFFFFFFFF);
            AHCI_WRITE_REG(pb, AHCI_PORT_SERR, 0xFFFFFFFF);
            AHCI_WRITE_REG(pb, AHCI_PORT_IE, AHCI_PORT_IE_DEFAULT);
        }
    }
    AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_IS, 0xFFFFFFFF);

    /* Enable the HBA's global interrupt line */
    ghc = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_GHC);
    ghc |= AHCI_GHC_IE;
    AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_GHC, ghc);

    AHCI_DBG_MSG("AhciHwInitialize: HBA initialization complete, interrupts enabled");

    return TRUE;
}

/* Asynchronous StartIo model */
// HwStartIo callback: reject re-entrant requests while one SRB is active,
// otherwise hand the SRB to the SCSI-to-ATA translation layer. Completion
// happens later from AhciInterrupt unless the SATL layer already finished
// the request synchronously.
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

    /* If the request wasn't left pending (i.e. it completed synchronously), complete it now */
    if (!pending) {
        hwInit->ActiveSrb = NULL;
        ScsiPortNotification(RequestComplete, hwInit, Srb);
        ScsiPortNotification(NextRequest, hwInit, NULL);
    }
    return TRUE;
}

/* Asynchronous Interrupt Service Routine (ISR) */
// HwInterrupt callback: identify which ports raised IS, clear their status
// bits, and complete the active SRB if this port's command slot finished
// or reported an error.
BOOLEAN AhciInterrupt(IN PVOID DeviceExtension) {
    PHW_DEVICE_EXTENSION hwInit;
    PSCSI_REQUEST_BLOCK srb;
    ULONG hbaIs, portIs, tfd, ci;
    ULONG p;
    PUCHAR portBase;
    BOOLEAN handled;

    hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;
    handled = FALSE;

    hbaIs = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_IS);
    if (hbaIs == 0) return FALSE;

    for (p = 0; p < MAX_SUPPORTED_PORTS; p++) {
        if (hbaIs & (1 << p)) {
            portBase = AHCI_PORT_BASE(hwInit->AbarMapped, p);
            portIs = AHCI_READ_REG(portBase, AHCI_PORT_IS);

            /* Clear the interrupt status bits */
            AHCI_WRITE_REG(portBase, AHCI_PORT_IS, portIs);
            AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
            AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_IS, (1UL << p));

            handled = TRUE;

            srb = hwInit->ActiveSrb;
            if (srb != NULL && srb->TargetId == (UCHAR)p) {
                ci = AHCI_READ_REG(portBase, AHCI_PORT_CI);
                tfd = AHCI_READ_REG(portBase, AHCI_PORT_TFD);

                /* Complete the request if slot 0 finished (CI bit cleared) or an error occurred */
                if (!(ci & 1) || (portIs & AHCI_PORT_IS_FATAL) || (tfd & 0x01)) {
                    if ((portIs & AHCI_PORT_IS_FATAL) || (tfd & 0x01)) {
                        AHCI_DBG_LOG("AhciInterrupt: port %d command failed (IS=0x%08X, TFD=0x%08X)", p, portIs, tfd);
                        srb->SrbStatus = SRB_STATUS_ERROR;
                        srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
                        AhciStopPortEngines(hwInit, portBase);
                        AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, AHCI_READ_REG(portBase, AHCI_PORT_CMD) | AHCI_PORT_CMD_FRE);
                        AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, AHCI_READ_REG(portBase, AHCI_PORT_CMD) | AHCI_PORT_CMD_ST);
                    } else {
                        srb->SrbStatus = SRB_STATUS_SUCCESS;
                        srb->ScsiStatus = SCSISTAT_GOOD;
                    }

                    hwInit->ActiveSrb = NULL;
                    ScsiPortNotification(RequestComplete, hwInit, srb);
                    ScsiPortNotification(NextRequest, hwInit, NULL);
                }
            }
        }
    }

    return handled;
}

// HwResetBus callback: stop and restart the command/FIS-receive engines on
// every present port, then fail the active SRB (if any) with
// SRB_STATUS_BUS_RESET.
BOOLEAN AhciResetBus(IN PVOID HwDeviceExtension, IN ULONG PathId) {
    PHW_DEVICE_EXTENSION hwInit;
    ULONG port;

    hwInit = (PHW_DEVICE_EXTENSION)HwDeviceExtension;

    AHCI_DBG_LOG("AhciResetBus: resetting PathId %d", PathId);

    for (port = 0; port < MAX_SUPPORTED_PORTS; port++) {
        if (hwInit->Ports[port].Present) {
            PUCHAR portBase = AHCI_PORT_BASE(hwInit->AbarMapped, port);
            AhciStopPortEngines(hwInit, portBase);
            AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, AHCI_READ_REG(portBase, AHCI_PORT_CMD) | AHCI_PORT_CMD_FRE);
            AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, AHCI_READ_REG(portBase, AHCI_PORT_CMD) | AHCI_PORT_CMD_ST);
        }
    }

    if (hwInit->ActiveSrb != NULL) {
        PSCSI_REQUEST_BLOCK srb = hwInit->ActiveSrb;
        hwInit->ActiveSrb = NULL;
        srb->SrbStatus = SRB_STATUS_BUS_RESET;
        ScsiPortNotification(RequestComplete, hwInit, srb);
        ScsiPortNotification(NextRequest, hwInit, NULL);
    }

    return TRUE;
}