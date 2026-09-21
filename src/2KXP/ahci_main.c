#include "ahcint.h"

BOOLEAN AhciHwInitialize(IN PVOID DeviceExtension);
BOOLEAN AhciStartIo(IN PVOID DeviceExtension, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN AhciInterrupt(IN PVOID DeviceExtension);
BOOLEAN AhciResetBus(IN PVOID HwDeviceExtension, IN ULONG PathId);
ULONG AhciFindAdapter(IN PVOID DeviceExtension, IN PVOID Context, IN PVOID BusInformation, IN PCHAR ArgumentString, IN OUT PPORT_CONFIGURATION_INFORMATION ConfigInfo, OUT PBOOLEAN Again);
BOOLEAN AhciSatlProcessSrb(IN PHW_DEVICE_EXTENSION DeviceExtension, IN PSCSI_REQUEST_BLOCK Srb);

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
    AHCI_DBG_LOG("AhciClaimDevice: claimed-device table full (max %d)",
                 AHCI_MAX_CONTROLLERS);
}

static BOOLEAN
AhciPciConfigIsAhci(IN PPCI_COMMON_CONFIG PciConfig)
{
    return (BOOLEAN)((PciConfig->BaseClass == PCI_CLASS_MASS_STORAGE &&
                       PciConfig->SubClass == PCI_SUBCLASS_AHCI &&
                       PciConfig->ProgIf == PCI_PROGIF_AHCI) ||
                      (PciConfig->VendorID == 0x1022 && (PciConfig->DeviceID == 0x7901 || PciConfig->DeviceID == 0x43EB || PciConfig->DeviceID == 0x43C8)) ||
                      (PciConfig->VendorID == 0x8086 && (PciConfig->DeviceID == 0x2829 || PciConfig->DeviceID == 0x2828 || PciConfig->DeviceID == 0x2922 || PciConfig->DeviceID == 0x2681)));
}

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
        AHCI_DBG_MSG("AhciAllocateDma: uncached extension allocation failed");
        return FALSE;
    }

    HwInit->DmaAreaPhysical = ScsiPortGetPhysicalAddress(HwInit, NULL, HwInit->DmaArea, &allocLen);
    if (HwInit->DmaAreaPhysical.LowPart == 0 && HwInit->DmaAreaPhysical.HighPart == 0) {
        AHCI_DBG_MSG("AhciAllocateDma: invalid physical address");
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
            cmd = AHCI_READ_REG(portBase, AHCI_PORT_CMD);
            AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, cmd | AHCI_PORT_CMD_CLO);
            timeout = 5000;
            while ((AHCI_READ_REG(portBase, AHCI_PORT_CMD) & AHCI_PORT_CMD_CLO) && --timeout) {
                ScsiPortStallExecution(10);
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
        AHCI_DBG_LOG("Port %d: IDENTIFY FAIL, CI=0x%08X TFD=0x%08X", PortNumber, AHCI_READ_REG(portBase, AHCI_PORT_CI), tfd);
    }

    AHCI_WRITE_REG(portBase, AHCI_PORT_IS, 0xFFFFFFFF);
    AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
    AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_IS, (1UL << PortNumber));
}

static BOOLEAN AhciInitializePort(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG PortNumber) {
    PUCHAR portBase;
    ULONG ssts, sig, cmd, timeout, retry;

    portBase = AHCI_PORT_BASE(HwInit->AbarMapped, PortNumber);
    AhciStopPortEngines(HwInit, portBase);

    cmd = AHCI_READ_REG(portBase, AHCI_PORT_CMD);
    cmd |= AHCI_PORT_CMD_POD | AHCI_PORT_CMD_SUD;
    AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, cmd);
    ScsiPortStallExecution(1000);

    /* DET == 0 means no cable/device present; skip the retry loop. */
    ssts = AHCI_READ_REG(portBase, AHCI_PORT_SSTS);
    if ((ssts & 0x0F) == 0x00) {
        HwInit->Ports[PortNumber].Present = FALSE;
        return FALSE;
    }

    /* Short retry loop (3x) while the link trains to DET == 3. */
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
                ScsiPortStallExecution(5000);
            }
            if ((ssts & 0x0F) == 0x03) break;
        }
    }

    if ((ssts & 0x0F) != 0x03) {
        HwInit->Ports[PortNumber].Present = FALSE;
        return FALSE;
    }

    AHCI_WRITE_REG(portBase, AHCI_PORT_CLB, HwInit->Ports[PortNumber].CommandListPhysical);
    AHCI_WRITE_REG(portBase, AHCI_PORT_CLBU, HwInit->Ports[PortNumber].CommandListPhysicalUpper);
    AHCI_WRITE_REG(portBase, AHCI_PORT_FB, HwInit->Ports[PortNumber].ReceivedFisPhysical);
    AHCI_WRITE_REG(portBase, AHCI_PORT_FBU, HwInit->Ports[PortNumber].ReceivedFisPhysicalUpper);

    AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
    AHCI_WRITE_REG(portBase, AHCI_PORT_IS, 0xFFFFFFFF);

    AHCI_WRITE_REG(portBase, AHCI_PORT_IE, AHCI_PORT_IE_DEFAULT);

    cmd = AHCI_READ_REG(portBase, AHCI_PORT_CMD);
    cmd |= AHCI_PORT_CMD_FRE;
    AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, cmd);

    timeout = 15000;
    while ((AHCI_READ_REG(portBase, AHCI_PORT_TFD) & 0x88) && --timeout) {
        ScsiPortStallExecution(100);
    }

    cmd = AHCI_READ_REG(portBase, AHCI_PORT_CMD);
    cmd |= AHCI_PORT_CMD_ST;
    AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, cmd);

    sig = AHCI_READ_REG(portBase, AHCI_PORT_SIG);
    HwInit->Ports[PortNumber].Present = TRUE;
    HwInit->Ports[PortNumber].IsAtapi = (sig == SATA_SIG_ATAPI);
    HwInit->Ports[PortNumber].IdentifyValid = FALSE;

    AhciExecuteIdentify(HwInit, PortNumber);
    return TRUE;
}

ULONG DriverEntry(IN PVOID DriverObject, IN PVOID Argument2) {
    HW_INITIALIZATION_DATA initData;
    PUCHAR ptr;
    ULONG i;

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

    /* Step 1: try the device PnP already points us at via ConfigInfo -
       the correct way to pick the right controller when there are 2+. */
    bytesRead = ScsiPortGetBusData(hwInit, PCIConfiguration,
                                   ConfigInfo->SystemIoBusNumber, ConfigInfo->SlotNumber,
                                   &pciConfig, sizeof(PCI_COMMON_CONFIG));
    if (bytesRead == sizeof(PCI_COMMON_CONFIG) &&
        pciConfig.VendorID != 0xFFFF && pciConfig.VendorID != 0x0000 &&
        AhciPciConfigIsAhci(&pciConfig) &&
        !AhciIsDeviceClaimed(ConfigInfo->SystemIoBusNumber, ConfigInfo->SlotNumber)) {

        targetBus = ConfigInfo->SystemIoBusNumber;
        targetSlot = ConfigInfo->SlotNumber;
        basePhys.LowPart = pciConfig.u.type0.BaseAddresses[barId] & 0xFFFFFFF0;
        basePhys.HighPart = 0;
        found = TRUE;
    }

    /* Step 2: fallback scan for the next unclaimed AHCI device on the
       bus, in case ConfigInfo's slot was empty or already claimed. */
    if (!found) {
        for (b = 0; b < 255 && !found; b++) {
            for (d = 0; d < 32 && !found; d++) {
                for (f = 0; f < 8; f++) {
                    ULONG s = (d << 3) | (f & 0x07);

                    bytesRead = ScsiPortGetBusData(hwInit, PCIConfiguration, b, s, &pciConfig, sizeof(PCI_COMMON_CONFIG));
                    if (bytesRead != sizeof(PCI_COMMON_CONFIG) || pciConfig.VendorID == 0xFFFF || pciConfig.VendorID == 0x0000) {
                        continue;
                    }

                    if (AhciPciConfigIsAhci(&pciConfig) && !AhciIsDeviceClaimed(b, s)) {
                        targetBus = b;
                        targetSlot = s;
                        basePhys.LowPart = pciConfig.u.type0.BaseAddresses[barId] & 0xFFFFFFF0;
                        basePhys.HighPart = 0;
                        found = TRUE;
                        break;
                    }
                }
            }
        }
    }

    if (!found || basePhys.LowPart == 0) {
        if (ConfigInfo->NumberOfAccessRanges > 5 && accessRanges != NULL) {
            basePhys = accessRanges[5].RangeStart;
            targetBus = ConfigInfo->SystemIoBusNumber;
            targetSlot = ConfigInfo->SlotNumber;
            found = TRUE;
        } else if (ConfigInfo->NumberOfAccessRanges > 0 && accessRanges != NULL) {
            basePhys = accessRanges[0].RangeStart;
            targetBus = ConfigInfo->SystemIoBusNumber;
            targetSlot = ConfigInfo->SlotNumber;
            found = TRUE;
        }
    }

    if (!found || basePhys.LowPart == 0) return SP_RETURN_NOT_FOUND;

    /* Claim this bus:slot now so a later FindAdapter call can't pick it. */
    AhciClaimDevice(targetBus, targetSlot);

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

    if (!hwInit->AbarMapped) return SP_RETURN_ERROR;

    if (ConfigInfo->NumberOfAccessRanges > barId && accessRanges != NULL) {
        accessRanges[barId].RangeStart = basePhys;
        accessRanges[barId].RangeLength = 0x10000;
        accessRanges[barId].RangeInMemory = TRUE;
    }

    ConfigInfo->SystemIoBusNumber = targetBus;
    ConfigInfo->SlotNumber = targetSlot;

    /* No manual IRQ assignment: ConfigInfo->BusInterruptLevel/Vector are
       already filled in by the HAL/PnP before this call. We only read
       them; Level/Vector == 0 (e.g. text-mode Setup) means no real IRQ. */
    hwInit->ActualIrq = (UCHAR)ConfigInfo->BusInterruptLevel;
    hwInit->UseInterrupt =
        (ConfigInfo->BusInterruptLevel != 0 && ConfigInfo->BusInterruptLevel != 0xFFFFFFFF) ? TRUE : FALSE;

    AHCI_DBG_LOG("FindAdapter: HAL IRQ level=%lu vector=%lu -> mode=%s",
                 ConfigInfo->BusInterruptLevel, ConfigInfo->BusInterruptVector,
                 hwInit->UseInterrupt ? "IRQ" : "FALLBACK-POLL");

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

BOOLEAN AhciHwInitialize(IN PVOID DeviceExtension) {
    PHW_DEVICE_EXTENSION hwInit;
    ULONG ghc, pi, port, timeout;
    ULONG cap2;
    ULONG i;

    hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;

    cap2 = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_CAP2);
    if (cap2 & 0x01) {
        ULONG bohc = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_BOHC);
        bohc |= AHCI_BOHC_OOS;
        AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_BOHC, bohc);

        timeout = 50000;
        while ((AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_BOHC) & AHCI_BOHC_BB) && --timeout) {
            ScsiPortStallExecution(10);
        }
    }

    ghc = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_GHC);
    AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_GHC, (ghc | AHCI_GHC_AE) & ~AHCI_GHC_IE);
    ScsiPortStallExecution(100);

    ghc = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_GHC);
    AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_GHC, ghc | AHCI_GHC_HR);
    timeout = 1000;
    while ((AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_GHC) & AHCI_GHC_HR) && --timeout) {
        ScsiPortStallExecution(1000);
    }

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
            /* No real IRQ (UseInterrupt == FALSE): leave port IE off,
               AhciFallbackTimer polls CI/TFD/IS instead. */
            AHCI_WRITE_REG(pb, AHCI_PORT_IE, hwInit->UseInterrupt ? AHCI_PORT_IE_DEFAULT : 0);
        }
    }
    AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_IS, 0xFFFFFFFF);

    /* Only enable the HBA-wide interrupt line if the HAL gave us a real
       IRQ; otherwise the fallback timer polls for all completions. */
    ghc = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_GHC);
    if (hwInit->UseInterrupt) {
        ghc |= AHCI_GHC_IE;
    } else {
        ghc &= ~AHCI_GHC_IE;
    }
    AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_GHC, ghc);

    hwInit->ActivePort = 0;
    hwInit->ActiveBytes = 0;

    AHCI_DBG_LOG("HwInitialize: mode=%s PI=0x%08X CAP=0x%08X",
                 hwInit->UseInterrupt ? "IRQ" : "FALLBACK-POLL", pi, hwInit->HbaCapabilities);

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

    /* Not pending means it completed synchronously; finish it now. */
    if (!pending) {
        hwInit->ActiveSrb = NULL;
        ScsiPortNotification(RequestComplete, hwInit, Srb);
        ScsiPortNotification(NextRequest, hwInit, NULL);
    }
    return TRUE;
}

/* Normal completion path when UseInterrupt == TRUE. AhciFallbackTimer
   also runs as a safety net; we don't cancel it here since a stale
   fallback call is harmless (it checks ActiveSrb/port itself). */
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

            AHCI_WRITE_REG(portBase, AHCI_PORT_IS, portIs);
            AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
            AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_IS, (1UL << p));

            handled = TRUE;

            srb = hwInit->ActiveSrb;
            if (srb != NULL && srb->TargetId == (UCHAR)p) {
                ci = AHCI_READ_REG(portBase, AHCI_PORT_CI);
                tfd = AHCI_READ_REG(portBase, AHCI_PORT_TFD);

                /* Slot done (CI bit clear) or an error flag is set. */
                if (!(ci & 1) || (portIs & AHCI_PORT_IS_FATAL) || (tfd & TFD_STS_ERR)) {
                    if ((portIs & AHCI_PORT_IS_FATAL) || (tfd & TFD_STS_ERR)) {
                        srb->SrbStatus = SRB_STATUS_ERROR;
                        srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
                        AhciStopPortEngines(hwInit, portBase);
                        AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, AHCI_READ_REG(portBase, AHCI_PORT_CMD) | AHCI_PORT_CMD_FRE);
                        AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, AHCI_READ_REG(portBase, AHCI_PORT_CMD) | AHCI_PORT_CMD_ST);
                    } else {
                        srb->SrbStatus = SRB_STATUS_SUCCESS;
                        srb->ScsiStatus = SCSISTAT_GOOD;
                        if (hwInit->ActiveBytes != 0) {
                            srb->DataTransferLength = hwInit->ActiveBytes;
                        }
                    }

                    hwInit->ActiveSrb = NULL;
                    hwInit->ActiveBytes = 0;
                    ScsiPortNotification(RequestComplete, hwInit, srb);
                    ScsiPortNotification(NextRequest, hwInit, NULL);
                }
            }
        }
    }

    return handled;
}

/* Fires AHCI_FALLBACK_TIMER_USEC after a command starts. If AhciInterrupt
   already completed it, ActiveSrb is NULL/different and we just return.
   Otherwise poll CI/TFD/IS and complete the command ourselves. */
VOID AhciFallbackTimer(IN PVOID DeviceExtension) {
    PHW_DEVICE_EXTENSION hwInit;
    PSCSI_REQUEST_BLOCK srb;
    PUCHAR portBase;
    ULONG p;
    ULONG portIs, tfd, ci;
    BOOLEAN error;

    hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;
    if (hwInit == NULL || hwInit->AbarMapped == NULL) return;
    if (hwInit->ActiveSrb == NULL) return;

    p = hwInit->ActivePort;
    if (p >= MAX_SUPPORTED_PORTS) return;

    portBase = AHCI_PORT_BASE(hwInit->AbarMapped, p);
    portIs = AHCI_READ_REG(portBase, AHCI_PORT_IS);
    tfd = AHCI_READ_REG(portBase, AHCI_PORT_TFD);
    ci = AHCI_READ_REG(portBase, AHCI_PORT_CI);

    error = (BOOLEAN)(((portIs & AHCI_PORT_IS_FATAL) != 0) || ((tfd & (TFD_STS_ERR | TFD_STS_DF)) != 0));

    if (!error && (ci & 1) != 0) {
        /* Still running; re-arm the timer for another look. */
        ScsiPortNotification(RequestTimerCall, hwInit, AhciFallbackTimer,
                             AHCI_FALLBACK_TIMER_USEC);
        return;
    }

    if (portIs != 0) {
        AHCI_WRITE_REG(portBase, AHCI_PORT_IS, portIs);
    }
    if (portIs & AHCI_PORT_IS_FATAL) {
        AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
    }
    AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_IS, (1UL << p));

    srb = hwInit->ActiveSrb;
    if (srb == NULL) return;
    hwInit->ActiveSrb = NULL;

    if (error) {
        srb->SrbStatus = SRB_STATUS_ERROR;
        srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
        AhciStopPortEngines(hwInit, portBase);
        AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, AHCI_READ_REG(portBase, AHCI_PORT_CMD) | AHCI_PORT_CMD_FRE);
        AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, AHCI_READ_REG(portBase, AHCI_PORT_CMD) | AHCI_PORT_CMD_ST);
        AHCI_DBG_LOG("FallbackTimer: port %lu command error (TFD=0x%08X IS=0x%08X)", p, tfd, portIs);
    } else {
        srb->SrbStatus = SRB_STATUS_SUCCESS;
        srb->ScsiStatus = SCSISTAT_GOOD;
        if (hwInit->ActiveBytes != 0) {
            srb->DataTransferLength = hwInit->ActiveBytes;
        }
    }

    hwInit->ActiveBytes = 0;
    ScsiPortNotification(RequestComplete, hwInit, srb);
    ScsiPortNotification(NextRequest, hwInit, NULL);
}

BOOLEAN AhciResetBus(IN PVOID HwDeviceExtension, IN ULONG PathId) {
    PHW_DEVICE_EXTENSION hwInit;
    ULONG port;

    hwInit = (PHW_DEVICE_EXTENSION)HwDeviceExtension;

    for (port = 0; port < MAX_SUPPORTED_PORTS; port++) {
        if (hwInit->Ports[port].Present) {
            PUCHAR portBase = AHCI_PORT_BASE(hwInit->AbarMapped, port);
            AhciStopPortEngines(hwInit, portBase);
            AHCI_WRITE_REG(portBase, AHCI_PORT_IS, 0xFFFFFFFF);
            AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
            AHCI_WRITE_REG(portBase, AHCI_PORT_IE, hwInit->UseInterrupt ? AHCI_PORT_IE_DEFAULT : 0);
            AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, AHCI_READ_REG(portBase, AHCI_PORT_CMD) | AHCI_PORT_CMD_FRE);
            AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, AHCI_READ_REG(portBase, AHCI_PORT_CMD) | AHCI_PORT_CMD_ST);
        }
    }
    AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_IS, 0xFFFFFFFF);

    if (hwInit->ActiveSrb != NULL) {
        PSCSI_REQUEST_BLOCK srb = hwInit->ActiveSrb;
        hwInit->ActiveSrb = NULL;
        hwInit->ActiveBytes = 0;
        srb->SrbStatus = SRB_STATUS_BUS_RESET;
        ScsiPortNotification(RequestComplete, hwInit, srb);
        ScsiPortNotification(NextRequest, hwInit, NULL);
    }

    return TRUE;
}