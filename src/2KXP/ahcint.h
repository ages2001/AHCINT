// AHCI SCSI miniport driver — shared definitions for Windows 2000 / XP (x86)
#ifndef _AHCINT_H_
#define _AHCINT_H_

#include <miniport.h>
#include <scsi.h>

// PCI class/subclass/prog-if identifying an AHCI HBA
#define PCI_CLASS_MASS_STORAGE          0x01
#define PCI_SUBCLASS_AHCI               0x06
#define PCI_PROGIF_AHCI                 0x01

#define MAX_SUPPORTED_PORTS             8
#define MAX_PRDT_ENTRIES                32

/* Global HBA Registers */
#define AHCI_GEN_CAP                    0x00
#define AHCI_GEN_GHC                    0x04
#define AHCI_GEN_IS                     0x08
#define AHCI_GEN_PI                     0x0C
#define AHCI_GEN_VS                     0x10
#define AHCI_GEN_CAP2                   0x24
#define AHCI_GEN_BOHC                   0x28

// GHC bits
#define AHCI_GHC_AE                     (1UL << 31)
#define AHCI_GHC_IE                     (1UL << 1)
#define AHCI_GHC_HR                     (1UL << 0)

// CAP bits
#define AHCI_CAP_SCLO                   (1UL << 24)
#define AHCI_CAP_S64A                   (1UL << 31)

// BOHC (BIOS/OS Handoff Control) bits
#define AHCI_BOHC_BB                    0x00000001
#define AHCI_BOHC_OOS                   0x00000002
#define AHCI_BOHC_SOOE                  0x00000004
#define AHCI_BOHC_BOS                   0x00000010

/* Port Registers */
#define AHCI_PORT_CLB                   0x00
#define AHCI_PORT_CLBU                  0x04
#define AHCI_PORT_FB                    0x08
#define AHCI_PORT_FBU                   0x0C
#define AHCI_PORT_IS                    0x10
#define AHCI_PORT_IE                    0x14
#define AHCI_PORT_CMD                   0x18
#define AHCI_PORT_TFD                   0x20
#define AHCI_PORT_SIG                   0x24
#define AHCI_PORT_SSTS                  0x28
#define AHCI_PORT_SCTL                  0x2C
#define AHCI_PORT_SERR                  0x30
#define AHCI_PORT_SACT                  0x34
#define AHCI_PORT_CI                    0x38

// PxCMD bits
#define AHCI_PORT_CMD_ST                (1UL << 0)
#define AHCI_PORT_CMD_SUD               (1UL << 1)
#define AHCI_PORT_CMD_POD               (1UL << 2)
#define AHCI_PORT_CMD_CLO               (1UL << 3)
#define AHCI_PORT_CMD_FRE               (1UL << 4)
#define AHCI_PORT_CMD_FR                (1UL << 14)
#define AHCI_PORT_CMD_CR                (1UL << 15)
#define AHCI_PORT_CMD_ATAPI             (1UL << 24)

// PxIS bits
#define AHCI_PORT_IS_DHRS               (1UL << 0)
#define AHCI_PORT_IS_PSS                (1UL << 1)
#define AHCI_PORT_IS_DSS                (1UL << 2)
#define AHCI_PORT_IS_SDBS               (1UL << 3)
#define AHCI_PORT_IS_DPS                (1UL << 5)
#define AHCI_PORT_IS_TFES               (1UL << 30)
#define AHCI_PORT_IS_HBFS               (1UL << 29)
#define AHCI_PORT_IS_HBDS               (1UL << 28)
#define AHCI_PORT_IS_IFS                (1UL << 27)
#define AHCI_PORT_IS_FATAL              (AHCI_PORT_IS_TFES | AHCI_PORT_IS_HBFS | AHCI_PORT_IS_HBDS | AHCI_PORT_IS_IFS)

/* Default port interrupt-enable mask */
#define AHCI_PORT_IE_DEFAULT            (AHCI_PORT_IS_DHRS | AHCI_PORT_IS_PSS | AHCI_PORT_IS_DSS | \
                                         AHCI_PORT_IS_SDBS | AHCI_PORT_IS_DPS | AHCI_PORT_IS_FATAL)

// PxSIG values identifying device type after COMRESET
#define SATA_SIG_ATA                    0x00000101
#define SATA_SIG_ATAPI                  0xEB140101

// ATA/ATAPI command opcodes used by this driver
#define IDE_COMMAND_IDENTIFY_DEVICE     0xEC
#define IDE_COMMAND_IDENTIFY_PACKET     0xA1
#define IDE_COMMAND_READ_DMA_EXT        0x25
#define IDE_COMMAND_WRITE_DMA_EXT       0x35
#define IDE_COMMAND_READ_SECTORS_EXT    0x24
#define IDE_COMMAND_WRITE_SECTORS_EXT   0x34
#define IDE_COMMAND_READ_SECTORS        0x20
#define IDE_COMMAND_WRITE_SECTORS       0x30
#define IDE_COMMAND_READ_DMA            0xC8
#define IDE_COMMAND_WRITE_DMA           0xCA
#define IDE_COMMAND_PACKET              0xA0

// MMIO register access helpers and per-port base address
#define AHCI_READ_REG(base, off)        ScsiPortReadRegisterUlong((PULONG)((PUCHAR)(base) + (off)))
#define AHCI_WRITE_REG(base, off, val)  ScsiPortWriteRegisterUlong((PULONG)((PUCHAR)(base) + (off)), (ULONG)(val))
#define AHCI_PORT_BASE(abar, port)      ((PUCHAR)(abar) + 0x100 + ((port) * 0x80))

// Debug print helpers. AHCI_DBG_LOG is a printf-style variadic macro (C99);
// AHCI_DBG_MSG takes a plain string literal with no format arguments.
ULONG __cdecl DbgPrint(PCH Format, ...);
#define AHCI_DBG_LOG(fmt, ...) DbgPrint("[AHCINT] " fmt "\n", __VA_ARGS__)
#define AHCI_DBG_MSG(msg)      DbgPrint("[AHCINT] " msg "\n")

// AHCI on-adapter DMA structures must be byte-packed (no compiler padding)
#pragma pack(push, 1)

// One entry in a port's command list (32 entries per port in AHCI, but this
// driver only ever uses slot 0 at a time)
typedef struct _AHCI_COMMAND_HEADER {
    USHORT Flags;
    USHORT PrdtLength;
    ULONG  PrdByteCount;
    ULONG  CommandTableBase;
    ULONG  CommandTableBaseUpper;
    ULONG  Reserved[4];
} AHCI_COMMAND_HEADER, *PAHCI_COMMAND_HEADER;

// Physical Region Descriptor Table entry — one scatter-gather segment
typedef struct _AHCI_PRDT_ENTRY {
    ULONG DataBaseAddress;
    ULONG DataBaseAddressUpper;
    ULONG Reserved;
    ULONG ByteCountInterrupt;
} AHCI_PRDT_ENTRY, *PAHCI_PRDT_ENTRY;

// Host-to-Device Register FIS, as written into the command table
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
    UCHAR Control;
    UCHAR Reserved;
} FIS_REG_H2D, *PFIS_REG_H2D;

#pragma pack(pop)

// Single uncached DMA allocation backing all ports' command lists, FIS
// receive areas, command tables and IDENTIFY buffers (see AhciAllocateDma)
typedef struct _AHCI_DMA_RESOURCES {
    UCHAR RawBuffer[65536];
} AHCI_DMA_RESOURCES, *PAHCI_DMA_RESOURCES;

// Per-port state: device presence/type, cached IDENTIFY data, and the
// virtual/physical addresses of that port's DMA regions
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

// ScsiPort miniport device extension for this adapter instance
typedef struct _HW_DEVICE_EXTENSION {
    PUCHAR                  AbarMapped;
    ULONG                   PciBus;
    ULONG                   PciSlot;
    UCHAR                   ActualIrq;
    ULONG                   HbaCapabilities;
    ULONG                   PortsImplemented;
    AHCI_PORT_INFO          Ports[MAX_SUPPORTED_PORTS];
    PSCSI_REQUEST_BLOCK     ActiveSrb;

    PAHCI_DMA_RESOURCES     DmaArea;
    SCSI_PHYSICAL_ADDRESS   DmaAreaPhysical;
} HW_DEVICE_EXTENSION, *PHW_DEVICE_EXTENSION;

// Normalized request passed from the SATL layer to the transfer engine.
// ForcePio selects the legacy PIO opcode family instead of DMA.
typedef struct _ATA_REQUEST {
    PVOID     DataBuffer;
    ULONG     DataBufferLen;
    BOOLEAN   IsWrite;
    BOOLEAN   IsAtapi;
    BOOLEAN   ForcePio;
    ULONGLONG Lba;
    USHORT    SectorCount;
} ATA_REQUEST, *PATA_REQUEST;

// Freestanding-environment memset (no CRT available in a kernel miniport)
#pragma function(memset)
static __inline void * __cdecl memset(void *dst, int val, size_t count) {
    char *p = (char *)dst;
    while (count--) *p++ = (char)val;
    return dst;
}

#define ZeroMemoryBytes(dst, len) memset((dst), 0, (len))

VOID AhciStopPortEngines(IN PHW_DEVICE_EXTENSION HwInit, IN PUCHAR portBase);

#endif