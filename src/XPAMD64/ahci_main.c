#ifdef __cplusplus
extern "C" {
#endif

#include "ahcint.h"

// NOTE (x86->x64 port): the x86 build used to define its own inline-asm
// KeMemoryBarrier() here (x86 MSVC doesn't compile __asm on x64, and there
// is no portable replacement in inline asm form). A real KeMemoryBarrier()
// is normally available from ntddk.h/wdm.h; since this driver only includes
// miniport.h/scsi.h (and this DDK's compiler has no intrin.h either),
// ahcint.h declares a real x64 barrier itself via InterlockedExchange (see
// ahcint.h) so it's never left undeclared. Do NOT reintroduce an
// `#else -> #define KeMemoryBarrier()` no-op fallback here: that would
// silently strip the SMP memory barrier used in AhciResetBus instead of
// using the real one.

BOOLEAN AhciHwInitialize(IN PVOID DeviceExtension);
BOOLEAN AhciStartIo(IN PVOID DeviceExtension, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN AhciInterrupt(IN PVOID DeviceExtension);
BOOLEAN AhciResetBus(IN PVOID HwDeviceExtension, IN ULONG PathId);
ULONG AhciFindAdapter(IN PVOID DeviceExtension, IN PVOID Context, IN PVOID BusInformation, IN PCHAR ArgumentString, IN OUT PPORT_CONFIGURATION_INFORMATION ConfigInfo, OUT PBOOLEAN Again);
BOOLEAN AhciSatlProcessSrb(IN PHW_DEVICE_EXTENSION DeviceExtension, IN PSCSI_REQUEST_BLOCK Srb);

/* Multi-controller support: SCSIport calls HwFindAdapter once per PCI
   function it enumerates, so this table remembers which bus:slot each
   instance has already claimed, letting a second/third AHCI controller
   bind to its own DeviceExtension instead of re-finding the first one. */
#define AHCI_MAX_CONTROLLERS 8

typedef struct _AHCI_CLAIMED_DEVICE {
    BOOLEAN InUse;
    ULONG   Bus;
    ULONG   Slot;
} AHCI_CLAIMED_DEVICE, *PAHCI_CLAIMED_DEVICE;

static AHCI_CLAIMED_DEVICE g_AhciClaimedDevices[AHCI_MAX_CONTROLLERS];

static
BOOLEAN
AhciIsDeviceClaimed(
    IN ULONG Bus,
    IN ULONG Slot
)
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

static
VOID
AhciClaimDevice(
    IN ULONG Bus,
    IN ULONG Slot
)
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
}

/* True if this PCI function is an AHCI controller, either by the standard
   class/subclass/prog-if or by one of the known vendor-specific IDs. */
static
BOOLEAN
AhciPciConfigIsAhci(
    IN PPCI_COMMON_CONFIG PciConfig
)
{
    if (PciConfig->BaseClass == PCI_CLASS_MASS_STORAGE &&
        PciConfig->SubClass == PCI_SUBCLASS_AHCI &&
        PciConfig->ProgIf == PCI_PROGIF_AHCI) {
        return TRUE;
    }
    if (PciConfig->VendorID == 0x1022 &&
        (PciConfig->DeviceID == 0x7901 || PciConfig->DeviceID == 0x43EB || PciConfig->DeviceID == 0x43C8)) {
        return TRUE;
    }
    if (PciConfig->VendorID == 0x8086 &&
        (PciConfig->DeviceID == 0x2829 || PciConfig->DeviceID == 0x2828 ||
         PciConfig->DeviceID == 0x2922 || PciConfig->DeviceID == 0x2681)) {
        return TRUE;
    }
    return FALSE;
}

static
BOOLEAN
AhciAllocateDma(
	IN PHW_DEVICE_EXTENSION HwInit,
	IN PPORT_CONFIGURATION_INFORMATION ConfigInfo
	)
	{
    ULONG uncachedSize;
    ULONG allocLen;
    PUCHAR virtPtr;
    ULONG physPtr;
    ULONG port;
    ULONG alignOffset;

    if (HwInit->DmaArea != NULL) return TRUE;

    // Ensure uncachedSize is large enough for all ports plus worst-case alignment padding
    // (Ensure AHCI_DMA_RESOURCES struct definition accounts for MAX_SUPPORTED_PORTS)
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

    ZeroMemoryBytes(HwInit->DmaArea, uncachedSize);

    virtPtr = (PUCHAR)HwInit->DmaArea->RawBuffer;
    physPtr = HwInit->DmaAreaPhysical.LowPart;

    for (port = 0; port < MAX_SUPPORTED_PORTS; port++) {
        // Command List alignment (1024 bytes)
        alignOffset = physPtr % 1024;
        if (alignOffset != 0) {
            ULONG pad = 1024 - alignOffset;
            virtPtr += pad;
            physPtr += pad;
        }

        // Safety bounds check for ACPI memory protection.
        // (x86->x64 port: the pointer difference is ptrdiff_t, 64-bit on
        // x64 — use ULONG_PTR here instead of truncating to ULONG.)
        if ((ULONG_PTR)(virtPtr - (PUCHAR)HwInit->DmaArea) + 1024 > uncachedSize) {
            AHCI_DBG_MSG("AhciAllocateDma: DMA buffer overflow at Command List!");
            return FALSE;
        }

        HwInit->Ports[port].CommandList = (PAHCI_COMMAND_HEADER)virtPtr;
        HwInit->Ports[port].CommandListPhysical = physPtr;
        HwInit->Ports[port].CommandListPhysicalUpper = HwInit->DmaAreaPhysical.HighPart;
        virtPtr += 1024;
        physPtr += 1024;

        // Received FIS alignment (256 bytes)
        alignOffset = physPtr % 256;
        if (alignOffset != 0) {
            ULONG pad = 256 - alignOffset;
            virtPtr += pad;
            physPtr += pad;
        }

        if ((ULONG_PTR)(virtPtr - (PUCHAR)HwInit->DmaArea) + 256 > uncachedSize) {
            AHCI_DBG_MSG("AhciAllocateDma: DMA buffer overflow at Received FIS!");
            return FALSE;
        }

        HwInit->Ports[port].ReceivedFis = virtPtr;
        HwInit->Ports[port].ReceivedFisPhysical = physPtr;
        HwInit->Ports[port].ReceivedFisPhysicalUpper = HwInit->DmaAreaPhysical.HighPart;
        virtPtr += 256;
        physPtr += 256;

        // Command Table alignment (128 bytes)
        alignOffset = physPtr % 128;
        if (alignOffset != 0) {
            ULONG pad = 128 - alignOffset;
            virtPtr += pad;
            physPtr += pad;
        }

        if ((ULONG_PTR)(virtPtr - (PUCHAR)HwInit->DmaArea) + 2048 > uncachedSize) {
            AHCI_DBG_MSG("AhciAllocateDma: DMA buffer overflow at Command Table!");
            return FALSE;
        }

        HwInit->Ports[port].CommandTable = virtPtr;
        HwInit->Ports[port].CommandTablePhysical = physPtr;
        HwInit->Ports[port].CommandTablePhysicalUpper = HwInit->DmaAreaPhysical.HighPart;
        virtPtr += 2048;
        physPtr += 2048;

        // Identify DMA Buffer alignment (512 bytes)
        alignOffset = physPtr % 512;
        if (alignOffset != 0) {
            ULONG pad = 512 - alignOffset;
            virtPtr += pad;
            physPtr += pad;
        }

        if ((ULONG_PTR)(virtPtr - (PUCHAR)HwInit->DmaArea) + 512 > uncachedSize) {
            AHCI_DBG_MSG("AhciAllocateDma: DMA buffer overflow at Identify Buffer!");
            return FALSE;
        }

        HwInit->Ports[port].IdentifyDmaBuffer = virtPtr;
        HwInit->Ports[port].IdentifyDmaPhysical = physPtr;
        HwInit->Ports[port].IdentifyDmaPhysicalUpper = HwInit->DmaAreaPhysical.HighPart;
        virtPtr += 512;
        physPtr += 512;
    }

    return TRUE;
}



VOID
AhciStopPortEngines(
	IN PHW_DEVICE_EXTENSION HwInit,
	IN PUCHAR portBase
	)
	{
    ULONG cmd;
    ULONG timeout;

    cmd = AHCI_READ_REG(portBase, AHCI_PORT_CMD);
    if (cmd & AHCI_PORT_CMD_ST) {
        cmd &= ~AHCI_PORT_CMD_ST;
        AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, cmd);
        AHCI_READ_REG(portBase, AHCI_PORT_CMD); // Flush write buffer immediately for ACPI compliance

        timeout = 50000;
        while ((AHCI_READ_REG(portBase, AHCI_PORT_CMD) & AHCI_PORT_CMD_CR) && --timeout) {
            ScsiPortStallExecution(10);
        }

        // If CR fails to clear and CLO is supported, issue Command List Override
        if ((AHCI_READ_REG(portBase, AHCI_PORT_CMD) & AHCI_PORT_CMD_CR) &&
            (HwInit->HbaCapabilities & AHCI_CAP_SCLO)) {
            cmd = AHCI_READ_REG(portBase, AHCI_PORT_CMD);
            AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, cmd | AHCI_PORT_CMD_CLO);
            AHCI_READ_REG(portBase, AHCI_PORT_CMD); // Flush CLO write buffer

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
        AHCI_READ_REG(portBase, AHCI_PORT_CMD); // Flush FRE write buffer immediately

        timeout = 50000;
        while ((AHCI_READ_REG(portBase, AHCI_PORT_CMD) & AHCI_PORT_CMD_FR) && --timeout) {
            ScsiPortStallExecution(10);
        }
    }
}

static
VOID
AhciExecuteIdentify(
	IN PHW_DEVICE_EXTENSION HwInit,
	IN ULONG PortNumber
	)
	{
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

    // Clear flags with mandatory write-flushing for ACPI compliance
    AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
    AHCI_READ_REG(portBase, AHCI_PORT_SERR);
    AHCI_WRITE_REG(portBase, AHCI_PORT_IS, 0xFFFFFFFF);
    AHCI_READ_REG(portBase, AHCI_PORT_IS);
    AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_IS, (1UL << PortNumber));
    AHCI_READ_REG(HwInit->AbarMapped, AHCI_GEN_IS);

    // Issue command via CI with write-flushing and memory barrier fencing for ACPI/multi-core
    AHCI_WRITE_REG(portBase, AHCI_PORT_CI, 1);
    AHCI_READ_REG(portBase, AHCI_PORT_CI); // Forces write buffer flush and acts as hardware barrier

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

    // Clean up status registers with final write-flushing
    AHCI_WRITE_REG(portBase, AHCI_PORT_IS, 0xFFFFFFFF);
    AHCI_READ_REG(portBase, AHCI_PORT_IS);
    AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
    AHCI_READ_REG(portBase, AHCI_PORT_SERR);
    AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_IS, (1UL << PortNumber));
    AHCI_READ_REG(HwInit->AbarMapped, AHCI_GEN_IS);
}


static
BOOLEAN
AhciInitializePort(
	IN PHW_DEVICE_EXTENSION HwInit,
	IN ULONG PortNumber
	)
	{
    PUCHAR portBase;
    ULONG ssts, sig, cmd, timeout, retry;

    portBase = AHCI_PORT_BASE(HwInit->AbarMapped, PortNumber);
    AhciStopPortEngines(HwInit, portBase);

    // Assert Power On and Spin-Up with write-posting for ACPI compatibility
    cmd = AHCI_READ_REG(portBase, AHCI_PORT_CMD);
    cmd |= AHCI_PORT_CMD_POD | AHCI_PORT_CMD_SUD;
    AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, cmd);
    AHCI_READ_REG(portBase, AHCI_PORT_CMD); // Flush write buffer

    // Extended power-stabilization delay for fast I/O APIC systems (50ms)
    ScsiPortStallExecution(50000);

    // Poll for initial PHY presence (DET != 0) to accommodate faster MP HAL execution
    timeout = 50;
    do {
        ssts = AHCI_READ_REG(portBase, AHCI_PORT_SSTS);
        if ((ssts & 0x0F) != 0x00) {
            break;
        }
        ScsiPortStallExecution(5000);
    } while (--timeout);

    if ((ssts & 0x0F) == 0x00) {
        HwInit->Ports[PortNumber].Present = FALSE;
        return FALSE;
    }

    if ((ssts & 0x0F) != 0x03) {
        for (retry = 0; retry < 3; retry++) {
            AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
            AHCI_READ_REG(portBase, AHCI_PORT_SERR); // Flush SERR

            AHCI_WRITE_REG(portBase, AHCI_PORT_SCTL, 0x301); // COMRESET
            AHCI_READ_REG(portBase, AHCI_PORT_SCTL); // Flush SCTL
            ScsiPortStallExecution(2000);

            AHCI_WRITE_REG(portBase, AHCI_PORT_SCTL, 0x300); // Clear COMRESET
            AHCI_READ_REG(portBase, AHCI_PORT_SCTL); // Flush SCTL

            timeout = 40;
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

    // Set up command list and FIS base addresses with mandatory write-flushing
    AHCI_WRITE_REG(portBase, AHCI_PORT_CLB, HwInit->Ports[PortNumber].CommandListPhysical);
    AHCI_READ_REG(portBase, AHCI_PORT_CLB);
    AHCI_WRITE_REG(portBase, AHCI_PORT_CLBU, HwInit->Ports[PortNumber].CommandListPhysicalUpper);
    AHCI_READ_REG(portBase, AHCI_PORT_CLBU);

    AHCI_WRITE_REG(portBase, AHCI_PORT_FB, HwInit->Ports[PortNumber].ReceivedFisPhysical);
    AHCI_READ_REG(portBase, AHCI_PORT_FB);
    AHCI_WRITE_REG(portBase, AHCI_PORT_FBU, HwInit->Ports[PortNumber].ReceivedFisPhysicalUpper);
    AHCI_READ_REG(portBase, AHCI_PORT_FBU);

    // Clear pending errors and interrupt status before starting engines
    AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
    AHCI_READ_REG(portBase, AHCI_PORT_SERR);
    AHCI_WRITE_REG(portBase, AHCI_PORT_IS, 0xFFFFFFFF);
    AHCI_READ_REG(portBase, AHCI_PORT_IS);

    // Enable Frame Reader Engine (FRE)
    cmd = AHCI_READ_REG(portBase, AHCI_PORT_CMD);
    cmd |= AHCI_PORT_CMD_FRE;
    AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, cmd);
    AHCI_READ_REG(portBase, AHCI_PORT_CMD); // Flush FRE write

    timeout = 15000;
    while ((AHCI_READ_REG(portBase, AHCI_PORT_TFD) & 0x88) && --timeout) {
        ScsiPortStallExecution(100);
    }

    // Enable Command List Runner (ST)
    cmd = AHCI_READ_REG(portBase, AHCI_PORT_CMD);
    cmd |= AHCI_PORT_CMD_ST;
    AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, cmd);
    AHCI_READ_REG(portBase, AHCI_PORT_CMD); // Flush ST write

    // Enable interrupts ONLY AFTER engines are running (prevents I/O APIC level-triggered locks),
    // and only if we're actually running in interrupt mode rather than fast-poll mode.
    if (HwInit->UseInterrupt) {
        AHCI_WRITE_REG(portBase, AHCI_PORT_IE, AHCI_PORT_IE_DEFAULT);
        AHCI_READ_REG(portBase, AHCI_PORT_IE);
    } else {
        AHCI_WRITE_REG(portBase, AHCI_PORT_IE, 0);
        AHCI_READ_REG(portBase, AHCI_PORT_IE);
    }

    // Post-start stabilization delay
    ScsiPortStallExecution(2000);

    sig = AHCI_READ_REG(portBase, AHCI_PORT_SIG);
    HwInit->Ports[PortNumber].Present = TRUE;
    HwInit->Ports[PortNumber].IsAtapi = (sig == SATA_SIG_ATAPI);
    HwInit->Ports[PortNumber].IdentifyValid = FALSE;

    AhciExecuteIdentify(HwInit, PortNumber);
    return TRUE;
}


ULONG
DriverEntry(
	IN PVOID DriverObject,
	IN PVOID Argument2)
	{
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
    ULONG bytesRead;
    USHORT pciCmd;
    SCSI_PHYSICAL_ADDRESS basePhys;
    PACCESS_RANGE accessRanges;
    ULONG targetBus = 0;
    ULONG targetSlot = 0;
    BOOLEAN found = FALSE;
    ULONG barId = 5; // Typically BAR5 for AHCI (ABAR)
    ULONG cap;

    hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;
    accessRanges = (ConfigInfo->AccessRanges != NULL) ? *ConfigInfo->AccessRanges : NULL;

    *Again = FALSE;

    if (hwInit->AbarMapped != NULL) return SP_RETURN_FOUND;

    basePhys.QuadPart = 0;

    // 1. Check if the OS (PnP Manager / ACPI HAL) provided Bus and Slot numbers,
    // and that this particular device hasn't already been claimed by another
    // instance of this driver (multi-controller support).
    if (ConfigInfo->SystemIoBusNumber != (ULONG)-1 &&
        ConfigInfo->SlotNumber != (ULONG)-1 &&
        !AhciIsDeviceClaimed(ConfigInfo->SystemIoBusNumber, ConfigInfo->SlotNumber)) {

        targetBus = ConfigInfo->SystemIoBusNumber;
        targetSlot = ConfigInfo->SlotNumber;

        bytesRead = ScsiPortGetBusData(hwInit, PCIConfiguration, targetBus, targetSlot, &pciConfig, sizeof(PCI_COMMON_CONFIG));
        if (bytesRead == sizeof(PCI_COMMON_CONFIG) && pciConfig.VendorID != 0xFFFF && pciConfig.VendorID != 0x0000) {
            basePhys.LowPart = pciConfig.u.type0.BaseAddresses[barId] & 0xFFFFFFF0;

            // Check if BAR is 64-bit memory (type bits 1-2 == 10b, represented by mask 0x06 == 0x04)
            if ((pciConfig.u.type0.BaseAddresses[barId] & 0x06) == 0x04) {
                basePhys.HighPart = pciConfig.u.type0.BaseAddresses[barId + 1];
            } else {
                basePhys.HighPart = 0;
            }
            found = TRUE;
        }
    }

    // 2. If not found via OS bus/slot, check AccessRanges provided by PnP
    if (!found && ConfigInfo->NumberOfAccessRanges > 0 && accessRanges != NULL) {
        ULONG accBus = (ConfigInfo->SystemIoBusNumber != (ULONG)-1) ? ConfigInfo->SystemIoBusNumber : 0;
        ULONG accSlot = (ConfigInfo->SlotNumber != (ULONG)-1) ? ConfigInfo->SlotNumber : 0;

        if (!AhciIsDeviceClaimed(accBus, accSlot)) {
            if (ConfigInfo->NumberOfAccessRanges > barId && accessRanges[barId].RangeStart.LowPart != 0) {
                basePhys = accessRanges[barId].RangeStart;
                found = TRUE;
            } else if (accessRanges[0].RangeStart.LowPart != 0) {
                basePhys = accessRanges[0].RangeStart;
                barId = 0; // Fallback to BAR0 if BAR5 isn't populated in range
                found = TRUE;
            }

            targetBus = accBus;
            targetSlot = accSlot;
        }
    }

    // 3. Last Resort: Manual full PCI bus scan (primarily for legacy Non-ACPI /
    // Standard PC systems, and to pick up any remaining unclaimed AHCI
    // controller when 2+ are present in the system).
    if (!found) {
        ULONG b, d, f;
        for (b = 0; b < 255; b++) {
            for (d = 0; d < 32; d++) {
                for (f = 0; f < 8; f++) {
                    ULONG s = (d << 3) | (f & 0x07);

                    if (AhciIsDeviceClaimed(b, s)) {
                        continue;
                    }

                    bytesRead = ScsiPortGetBusData(hwInit, PCIConfiguration, b, s, &pciConfig, sizeof(PCI_COMMON_CONFIG));
                    if (bytesRead != sizeof(PCI_COMMON_CONFIG) || pciConfig.VendorID == 0xFFFF || pciConfig.VendorID == 0x0000) {
                        continue;
                    }

                    if (AhciPciConfigIsAhci(&pciConfig)) {
                        targetBus = b;
                        targetSlot = s;
                        basePhys.LowPart = pciConfig.u.type0.BaseAddresses[barId] & 0xFFFFFFF0;

                        if ((pciConfig.u.type0.BaseAddresses[barId] & 0x06) == 0x04) {
                            basePhys.HighPart = pciConfig.u.type0.BaseAddresses[barId + 1];
                        } else {
                            basePhys.HighPart = 0;
                        }

                        found = TRUE;
                        break;
                    }
                }
                if (found) break;
            }
            if (found) break;
        }
    }

    if (!found || basePhys.QuadPart == 0) {
        return SP_RETURN_NOT_FOUND;
    }

    hwInit->PciBus = targetBus;
    hwInit->PciSlot = targetSlot;
    AhciClaimDevice(targetBus, targetSlot);

    // Safely configure PCI command register with read-back barrier for ACPI synchronization
    bytesRead = ScsiPortGetBusData(hwInit, PCIConfiguration, targetBus, targetSlot, &pciConfig, sizeof(PCI_COMMON_CONFIG));
    if (bytesRead == sizeof(PCI_COMMON_CONFIG)) {
        pciCmd = (pciConfig.Command | 0x0006) & ~(1 << 10);
        ScsiPortSetBusDataByOffset(hwInit, PCIConfiguration, targetBus, targetSlot, &pciCmd, 0x04, sizeof(USHORT));
        ScsiPortGetBusData(hwInit, PCIConfiguration, targetBus, targetSlot, &pciConfig, sizeof(PCI_COMMON_CONFIG)); // Barrier
    }

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

    // IRQ assignment is left entirely to the HAL/PnP manager: ConfigInfo's
    // BusInterruptLevel/Vector are already populated by the time we get here
    // on a PnP-safe miniport, so we only read them back and decide whether
    // we have a usable interrupt at all.
    hwInit->ActualIrq = (UCHAR)ConfigInfo->BusInterruptLevel;
    hwInit->UseInterrupt =
        (ConfigInfo->BusInterruptLevel != 0 && ConfigInfo->BusInterruptLevel != 0xFFFFFFFF) ? TRUE : FALSE;

    ConfigInfo->InterruptMode = LevelSensitive;
    ConfigInfo->Master = TRUE;
    ConfigInfo->Dma32BitAddresses = TRUE;

    // Read HBA capabilities with immediate read barrier for ACPI memory ordering
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


BOOLEAN
AhciHwInitialize(
	IN PVOID DeviceExtension
	)
	{
    PHW_DEVICE_EXTENSION hwInit;
    ULONG ghc, pi, port, timeout;
    ULONG cap2;
    ULONG i;
    ULONG bohc;

    hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;

    // 1. Handle BIOS/OS Handoff (BOHC) with proper write posting for ACPI compliance
    cap2 = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_CAP2);
    if (cap2 & 0x01) {
        bohc = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_BOHC);
        bohc |= AHCI_BOHC_OOS;
        AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_BOHC, bohc);

        // Posting read to flush write buffers to the PCI device
        bohc = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_BOHC);

        timeout = 50000;
        while ((AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_BOHC) & AHCI_BOHC_BB) && --timeout) {
            ScsiPortStallExecution(10);
        }

        // Fallback: If ACPI BIOS holds BB too long, force clear it to prevent hanging
        if (timeout == 0) {
            bohc = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_BOHC);
            bohc &= ~AHCI_BOHC_BB;
            AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_BOHC, bohc);
            AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_BOHC);
        }
    }

    // 2. Enable AHCI Mode and clear Interrupt Enables with write flushing
    ghc = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_GHC);
    AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_GHC, (ghc | AHCI_GHC_AE) & ~AHCI_GHC_IE);
    AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_GHC); // Flush write
    ScsiPortStallExecution(100);

    // 3. Issue HBA Reset (HR)
    ghc = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_GHC);
    AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_GHC, ghc | AHCI_GHC_HR);
    AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_GHC); // Flush write

    timeout = 1000;
    while ((AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_GHC) & AHCI_GHC_HR) && --timeout) {
        ScsiPortStallExecution(1000);
    }

    // 4. Ensure AHCI Enable (AE) stays set post-reset with robust retries
    for (i = 0; i < 10; i++) {
        ghc = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_GHC);
        if (!(ghc & AHCI_GHC_AE)) {
            AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_GHC, ghc | AHCI_GHC_AE);
            AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_GHC); // Flush write
            ScsiPortStallExecution(1000);
        } else {
            break;
        }
    }

    // Give the controller a moment to settle after reset on ACPI platforms
    ScsiPortStallExecution(1000);

    pi = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_PI);

    // Safety fallback if PI reads 0 due to ACPI power timing
    if (pi == 0) {
        ULONG cap = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_CAP);
        ULONG numPorts = (cap & 0x1F) + 1;
        pi = (1 << numPorts) - 1;
    }

    hwInit->PortsImplemented = pi;

    // 5. Initialize active ports
    for (port = 0; port < MAX_SUPPORTED_PORTS; port++) {
        if (pi & (1 << port)) {
            AhciInitializePort(hwInit, port);
        }
    }

    // 6. Clear pending status and set up port interrupts (only meaningful
    // when we have a real HAL-assigned IRQ; fast-poll mode leaves PxIE=0).
    for (port = 0; port < MAX_SUPPORTED_PORTS; port++) {
        if (pi & (1 << port)) {
            PUCHAR pb = AHCI_PORT_BASE(hwInit->AbarMapped, port);
            AHCI_WRITE_REG(pb, AHCI_PORT_IS, 0xFFFFFFFF);
            AHCI_READ_REG(pb, AHCI_PORT_IS); // Flush
            AHCI_WRITE_REG(pb, AHCI_PORT_SERR, 0xFFFFFFFF);
            AHCI_READ_REG(pb, AHCI_PORT_SERR); // Flush
            AHCI_WRITE_REG(pb, AHCI_PORT_IE, hwInit->UseInterrupt ? AHCI_PORT_IE_DEFAULT : 0);
            AHCI_READ_REG(pb, AHCI_PORT_IE); // Flush
        }
    }

    AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_IS, 0xFFFFFFFF);
    AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_IS); // Flush

    // 7. Enable Global Interrupts only if we actually have a usable HAL IRQ;
    // otherwise leave GHC.IE clear and rely on the StartIo fast-poll path.
    if (hwInit->UseInterrupt) {
        ghc = AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_GHC);
        ghc |= AHCI_GHC_IE;
        AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_GHC, ghc);
        AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_GHC); // Final flush
    }

    return TRUE;
}



/* Asynchronous StartIo model */
// HwStartIo callback: reject re-entrant requests while one SRB is active,
// otherwise hand the SRB to the SCSI-to-ATA translation layer. Completion
// happens later from AhciInterrupt (or inline via fast-poll) unless the
// SATL layer already finished the request synchronously.
BOOLEAN
AhciStartIo(
	IN PVOID DeviceExtension,
	IN PSCSI_REQUEST_BLOCK Srb
	)
	{
    PHW_DEVICE_EXTENSION hwInit;
    BOOLEAN pending;

    hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;

    // Use volatile pointer access to prevent compiler caching and ensure fresh reads from memory
    if (*(volatile PVOID*)&hwInit->ActiveSrb != NULL) {
        Srb->SrbStatus = SRB_STATUS_BUSY;
        ScsiPortNotification(NextRequest, hwInit, NULL);
        return TRUE;
    }

    // Atomically store the active SRB using volatile casting
    *(volatile PVOID*)&hwInit->ActiveSrb = (PVOID)Srb;

    pending = AhciSatlProcessSrb(hwInit, Srb);

    /* If the request wasn't left pending (i.e. it completed synchronously), complete it now */
    if (!pending) {
        *(volatile PVOID*)&hwInit->ActiveSrb = NULL;
        ScsiPortNotification(RequestComplete, hwInit, Srb);
        ScsiPortNotification(NextRequest, hwInit, NULL);
    }
    return TRUE;
}


/* Fallback safety-net timer: only armed when UseInterrupt is TRUE, as a
   backstop in case the real IRQ is lost or coalesced. Polls the active
   port's CI/TFD exactly like AhciInterrupt would and completes the SRB
   if the command has already finished. */
VOID
AhciFallbackTimer(
    IN PVOID DeviceExtension
)
{
    PHW_DEVICE_EXTENSION hwInit;
    PSCSI_REQUEST_BLOCK srb;
    PUCHAR portBase;
    ULONG ci, tfd, portIs;

    hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;

    srb = (PSCSI_REQUEST_BLOCK)*(volatile PVOID*)&hwInit->ActiveSrb;
    if (srb == NULL) {
        return;
    }

    portBase = AHCI_PORT_BASE(hwInit->AbarMapped, hwInit->ActivePort);
    ci = AHCI_READ_REG(portBase, AHCI_PORT_CI);
    tfd = AHCI_READ_REG(portBase, AHCI_PORT_TFD);
    portIs = AHCI_READ_REG(portBase, AHCI_PORT_IS);

    if ((ci & 1) && !(portIs & AHCI_PORT_IS_FATAL) && !(tfd & TFD_STS_ERR)) {
        // Command is still running normally; re-arm and check again later.
        ScsiPortNotification(RequestTimerCall, hwInit, AhciFallbackTimer, AHCI_FALLBACK_TIMER_USEC);
        return;
    }

    AHCI_WRITE_REG(portBase, AHCI_PORT_IS, portIs);
    AHCI_READ_REG(portBase, AHCI_PORT_IS);
    AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
    AHCI_READ_REG(portBase, AHCI_PORT_SERR);

    if ((portIs & AHCI_PORT_IS_FATAL) || (tfd & TFD_STS_ERR)) {
        srb->SrbStatus = SRB_STATUS_ERROR;
        srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
        AhciStopPortEngines(hwInit, portBase);

        AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, AHCI_READ_REG(portBase, AHCI_PORT_CMD) | AHCI_PORT_CMD_FRE);
        AHCI_READ_REG(portBase, AHCI_PORT_CMD);
        AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, AHCI_READ_REG(portBase, AHCI_PORT_CMD) | AHCI_PORT_CMD_ST);
        AHCI_READ_REG(portBase, AHCI_PORT_CMD);
    } else {
        srb->SrbStatus = SRB_STATUS_SUCCESS;
        srb->ScsiStatus = SCSISTAT_GOOD;
        if (srb->DataTransferLength > hwInit->ActiveBytes) {
            srb->DataTransferLength = hwInit->ActiveBytes;
        }
    }

    *(volatile PVOID*)&hwInit->ActiveSrb = NULL;
    ScsiPortNotification(RequestComplete, hwInit, srb);
    ScsiPortNotification(NextRequest, hwInit, NULL);
}


/* Asynchronous Interrupt Service Routine (ISR) */
// HwInterrupt callback: identify which ports raised IS, clear their status
// bits, and complete the active SRB if this port's command slot finished
// or reported an error.
BOOLEAN
AhciInterrupt(
	IN PVOID DeviceExtension
	)
	{
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

            // If no actual bits are set in PORT_IS, skip this port
            if (portIs == 0) {
                continue;
            }

            /* Clear the interrupt status bits and immediately read back (flush) for level-triggered ACPI interrupts */
            AHCI_WRITE_REG(portBase, AHCI_PORT_IS, portIs);
            AHCI_READ_REG(portBase, AHCI_PORT_IS); // Flush write buffer

            AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
            AHCI_READ_REG(portBase, AHCI_PORT_SERR); // Flush write buffer

            handled = TRUE;

            // Use volatile access to ensure fresh read across multi-core ACPI environments
            srb = (PSCSI_REQUEST_BLOCK)*(volatile PVOID*)&hwInit->ActiveSrb;
            if (srb != NULL && srb->TargetId == (UCHAR)p) {
                ci = AHCI_READ_REG(portBase, AHCI_PORT_CI);
                tfd = AHCI_READ_REG(portBase, AHCI_PORT_TFD);

                /*
                 * Under SMP I/O APIC environments, portIs firing combined with
                 * a fatal error or task file error is absolute proof of completion/failure.
                 * For normal completions, if portIs indicates a valid event (like D2H FIS or
                 * standard completion) but CI has a minor hardware delay, we rely on portIs
                 * or allow a slightly relaxed check to prevent dropping completions.
                 */
                if ((portIs & AHCI_PORT_IS_FATAL) || (tfd & 0x01) || !(ci & 1)) {
                    if ((portIs & AHCI_PORT_IS_FATAL) || (tfd & 0x01)) {
                        srb->SrbStatus = SRB_STATUS_ERROR;
                        srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
                        AhciStopPortEngines(hwInit, portBase);

                        AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, AHCI_READ_REG(portBase, AHCI_PORT_CMD) | AHCI_PORT_CMD_FRE);
                        AHCI_READ_REG(portBase, AHCI_PORT_CMD);

                        AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, AHCI_READ_REG(portBase, AHCI_PORT_CMD) | AHCI_PORT_CMD_ST);
                        AHCI_READ_REG(portBase, AHCI_PORT_CMD);
                    } else {
                        srb->SrbStatus = SRB_STATUS_SUCCESS;
                        srb->ScsiStatus = SCSISTAT_GOOD;
                        if (srb->DataTransferLength > hwInit->ActiveBytes) {
                            srb->DataTransferLength = hwInit->ActiveBytes;
                        }
                    }

                    // Safely clear ActiveSrb using volatile casting
                    *(volatile PVOID*)&hwInit->ActiveSrb = NULL;

                    ScsiPortNotification(RequestComplete, hwInit, srb);
                    ScsiPortNotification(NextRequest, hwInit, NULL);
                }
            }
        }
    }

    // Clear global HBA interrupt status for the handled ports
    AHCI_WRITE_REG(hwInit->AbarMapped, AHCI_GEN_IS, hbaIs);
    AHCI_READ_REG(hwInit->AbarMapped, AHCI_GEN_IS);

    return handled;
}


BOOLEAN
AhciResetBus(
    IN PVOID HwDeviceExtension,
    IN ULONG PathId
    )
{
    PHW_DEVICE_EXTENSION hwInit;
    ULONG port;
    PSCSI_REQUEST_BLOCK srb;
    ULONG portCmd;
    ULONG timeout;

    hwInit = (PHW_DEVICE_EXTENSION)HwDeviceExtension;

    for (port = 0; port < MAX_SUPPORTED_PORTS; port++) {
        if (hwInit->Ports[port].Present) {
            PUCHAR portBase = AHCI_PORT_BASE(hwInit->AbarMapped, port);

            // 1. Stop port engines securely
            AhciStopPortEngines(hwInit, portBase);

            // 2. Clear any pending port interrupt status flags by writing 1s to them
            ScsiPortWriteRegisterUlong((PULONG)(portBase + AHCI_PORT_IS), 0xFFFFFFFF);

            // 3. Clear any SATA error (SERR) status flags by writing 1s
            ScsiPortWriteRegisterUlong((PULONG)(portBase + AHCI_PORT_SERR), 0xFFFFFFFF);

            // Read back to flush the write buffer across SMP/I/O APIC buses
            ScsiPortReadRegisterUlong((PULONG)(portBase + AHCI_PORT_SERR));

            // 4. Enable FIS Receive Enable (FRE) first per AHCI specification
            portCmd = ScsiPortReadRegisterUlong((PULONG)(portBase + AHCI_PORT_CMD));
            portCmd |= AHCI_PORT_CMD_FRE;
            ScsiPortWriteRegisterUlong((PULONG)(portBase + AHCI_PORT_CMD), portCmd);
            ScsiPortReadRegisterUlong((PULONG)(portBase + AHCI_PORT_CMD)); // Flush

            // 5. Wait a brief moment or poll for FR (FIS Receive Running) to assert
            timeout = 1000;
            while (!(ScsiPortReadRegisterUlong((PULONG)(portBase + AHCI_PORT_CMD)) & AHCI_PORT_CMD_FR) && --timeout) {
                // Micro-delay if needed, or simple spin loop
            }

            // 6. Enable Command List Start (ST)
            portCmd = ScsiPortReadRegisterUlong((PULONG)(portBase + AHCI_PORT_CMD));
            portCmd |= AHCI_PORT_CMD_ST;
            ScsiPortWriteRegisterUlong((PULONG)(portBase + AHCI_PORT_CMD), portCmd);
            ScsiPortReadRegisterUlong((PULONG)(portBase + AHCI_PORT_CMD)); // Flush
        }
    }

    // 7. Safely check and clear ActiveSrb with full compiler/hardware memory barriers for SMP safety
    KeMemoryBarrier();
    srb = (PSCSI_REQUEST_BLOCK)*(volatile PVOID*)&hwInit->ActiveSrb;
    if (srb != NULL) {
        *(volatile PVOID*)&hwInit->ActiveSrb = NULL;
        KeMemoryBarrier();

        srb->SrbStatus = SRB_STATUS_BUS_RESET;
        ScsiPortNotification(RequestComplete, hwInit, srb);
        ScsiPortNotification(NextRequest, hwInit, NULL);
    }

    return TRUE;
}


#ifdef __cplusplus
}
#endif