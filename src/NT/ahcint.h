#ifndef _AHCINT_H_
#define _AHCINT_H_

#include <miniport.h>
#include <scsi.h>

#define MAX_AHCI_PORTS              8

#define AHCI_READ_REG(base, off)        ScsiPortReadRegisterUlong((PULONG)((PUCHAR)(base) + (off)))
#define AHCI_WRITE_REG(base, off, val)  ScsiPortWriteRegisterUlong((PULONG)((PUCHAR)(base) + (off)), (ULONG)(val))
#define AHCI_PORT_BASE(abar, port)      ((PUCHAR)(abar) + 0x100 + ((port) * 0x80))

#define AHCI_PORT_CLB               0x00
#define AHCI_PORT_CLBU              0x04
#define AHCI_PORT_FB                0x08
#define AHCI_PORT_FBU               0x0C
#define AHCI_PORT_IS                0x10
#define AHCI_PORT_IE                0x14
#define AHCI_PORT_CMD               0x18
#define AHCI_PORT_TFD               0x20
#define AHCI_PORT_SIG               0x24
#define AHCI_PORT_SSTS              0x28
#define AHCI_PORT_SCTL              0x2C
#define AHCI_PORT_SERR              0x30
#define AHCI_PORT_CI                0x38

#define AHCI_PORT_CMD_ST            0x00000001
#define AHCI_PORT_CMD_SUD           0x00000002
#define AHCI_PORT_CMD_POD           0x00000004
#define AHCI_PORT_CMD_FRE           0x00000010
#define AHCI_PORT_CMD_FR            0x00004000
#define AHCI_PORT_CMD_CR            0x00008000
#define AHCI_PORT_CMD_ATAPI         0x01000000

#define AHCI_PORT_IS_DHRS           0x00000001
#define AHCI_PORT_IS_PSS            0x00000002
#define AHCI_PORT_IS_DPS            0x00000020
#define AHCI_PORT_IS_TFES           0x40000000
#define AHCI_PORT_IS_FATAL          0x78000000

#define AHCI_GEN_CAP                0x00
#define AHCI_GEN_GHC                0x04
#define AHCI_GEN_IS                 0x08
#define AHCI_GEN_PI                 0x0C
#define AHCI_GEN_VS                 0x10
#define AHCI_GEN_CAP2               0x24
#define AHCI_GEN_BOHC               0x28

#define AHCI_GHC_HR                 (1UL << 0)
#define AHCI_GHC_IE                 (1UL << 1)
#define AHCI_GHC_AE                 (1UL << 31)

#define SATA_SIG_ATA                0x00000101
#define SATA_SIG_ATAPI              0xEB140101

#define IDE_COMMAND_READ_DMA_EXT    0x25
#define IDE_COMMAND_WRITE_DMA_EXT   0x35
#define IDE_COMMAND_IDENTIFY_DEVICE 0xEC
#define IDE_COMMAND_IDENTIFY_PACKET 0xA1
#define IDE_COMMAND_PACKET          0xA0

#define PCI_CLASS_MASS_STORAGE      0x01
#define PCI_SUBCLASS_AHCI           0x06
#define PCI_PROGIF_AHCI             0x01

ULONG __cdecl DbgPrint(PCH Format, ...);
#define AHCI_DBG_MSG(msg) DbgPrint("[AHCINT] " msg "\n")
#define AHCI_DBG_LOG      DbgPrint

#define MAX_PRDT_ENTRIES            32

#pragma pack(push, 1)

typedef struct _AHCI_COMMAND_HEADER {
    USHORT Flags;
    USHORT PrdtLength;
    ULONG  PrdByteCount;
    ULONG  CommandTableBase;
    ULONG  CommandTableBaseUpper;
    ULONG  Reserved[4];
} AHCI_COMMAND_HEADER, *PAHCI_COMMAND_HEADER;

typedef struct _AHCI_PRDT_ENTRY {
    ULONG DataBaseAddress;
    ULONG DataBaseAddressUpper;
    ULONG Reserved;
    ULONG ByteCountInterrupt;
} AHCI_PRDT_ENTRY, *PAHCI_PRDT_ENTRY;

typedef struct _FIS_REG_H2D {
    UCHAR FisType;
    UCHAR PmPortControl;
    UCHAR Command;
    UCHAR FeaturesLow;
    UCHAR Lba0;
    UCHAR Lba1;
    UCHAR Lba2;
    UCHAR Device;
    UCHAR Lba3;
    UCHAR Lba4;
    UCHAR Lba5;
    UCHAR FeaturesHigh;
    UCHAR SectorCountLow;
    UCHAR SectorCountHigh;
    UCHAR Icc;
    UCHAR Control;
    ULONG Reserved;
} FIS_REG_H2D, *PFIS_REG_H2D;

typedef struct _AHCI_DMA_RESOURCES {
    UCHAR RawBuffer[65536];
} AHCI_DMA_RESOURCES, *PAHCI_DMA_RESOURCES;

#pragma pack(pop)

typedef struct _ATA_REQUEST {
    PVOID     DataBuffer;
    ULONG     DataBufferLen;
    ULONG     LbaLow;
    ULONG     LbaHigh;
    USHORT    SectorCount;
    BOOLEAN   IsWrite;
    BOOLEAN   IsAtapi;
} ATA_REQUEST, *PATA_REQUEST;

typedef struct _AHCI_PORT_INFO {
    BOOLEAN                 Present;
    BOOLEAN                 IsAtapi;
    BOOLEAN                 IdentifyValid;
    UCHAR                   IdentifyData[512];

    PAHCI_COMMAND_HEADER    CommandList;
    ULONG                   CommandListPhysical;
    ULONG                   CommandListPhysicalUpper;

    PUCHAR                  ReceivedFis;
    ULONG                   ReceivedFisPhysical;
    ULONG                   ReceivedFisPhysicalUpper;

    PUCHAR                  CommandTable;
    ULONG                   CommandTablePhysical;
    ULONG                   CommandTablePhysicalUpper;

    PUCHAR                  IdentifyDmaBuffer;
    ULONG                   IdentifyDmaPhysical;
    ULONG                   IdentifyDmaPhysicalUpper;
} AHCI_PORT_INFO, *PAHCI_PORT_INFO;

typedef struct _HW_DEVICE_EXTENSION {
    ULONG                 PciBus;
    ULONG                 PciSlot;
    UCHAR                 ActualIrq;
    PUCHAR                AbarMapped;
    ULONG                 PortsImplemented;
    AHCI_PORT_INFO        Ports[MAX_AHCI_PORTS];
    PAHCI_DMA_RESOURCES   DmaArea;
    SCSI_PHYSICAL_ADDRESS DmaAreaPhysical;
    PSCSI_REQUEST_BLOCK   ActiveSrb;
} HW_DEVICE_EXTENSION, *PHW_DEVICE_EXTENSION;

static __inline void ZeroMemoryBytes(PVOID dst, ULONG len) {
    PUCHAR _d = (PUCHAR)dst;
    while (len--) *_d++ = 0;
}

VOID AhciStopPortEngines(IN PUCHAR portBase);

#endif