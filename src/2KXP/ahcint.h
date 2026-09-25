#ifndef _AHCINT_H_
#define _AHCINT_H_

#include <miniport.h>
#include <scsi.h>

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

#define AHCI_GHC_AE                     (1UL << 31)
#define AHCI_GHC_IE                     (1UL << 1)
#define AHCI_GHC_HR                     (1UL << 0)

#define AHCI_CAP_SCLO                   (1UL << 24)
#define AHCI_CAP_S64A                   (1UL << 31)

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

#define AHCI_PORT_CMD_ST                (1UL << 0)
#define AHCI_PORT_CMD_SUD               (1UL << 1)
#define AHCI_PORT_CMD_POD               (1UL << 2)
#define AHCI_PORT_CMD_CLO               (1UL << 3)
#define AHCI_PORT_CMD_FRE               (1UL << 4)
#define AHCI_PORT_CMD_FR                (1UL << 14)
#define AHCI_PORT_CMD_CR                (1UL << 15)
#define AHCI_PORT_CMD_ATAPI             (1UL << 24)

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

/* UniATA default port interrupt-enable mask */
#define AHCI_PORT_IE_DEFAULT            (AHCI_PORT_IS_DHRS | AHCI_PORT_IS_PSS | AHCI_PORT_IS_DSS | \
                                         AHCI_PORT_IS_SDBS | AHCI_PORT_IS_DPS | AHCI_PORT_IS_FATAL)

/* nvme2k-style safety net: if the real IRQ never arrives, this timer
   polls CI/TFD/IS and completes the command anyway. */
#define AHCI_FALLBACK_TIMER_USEC        10000

/* FAST-POLL mode (no HAL-assigned IRQ, e.g. text-mode Setup): tight
   synchronous poll loop instead of the ~10ms system timer. */
#define AHCI_FAST_POLL_INTERVAL_USEC    10
/* Upper bound for one command's tight-poll wait, in microseconds. */
#define AHCI_FAST_POLL_TIMEOUT_USEC     3000000

#define TFD_STS_ERR                     0x01
#define TFD_STS_DF                      0x20

/* PxTFD bits 15:8 mirror the real ATA/ATAPI Error register whenever
   PxTFD.STS.ERR (bit 0) is set -- AHCI 1.3 spec, PxTFD layout. */
#define TFD_ERR_SHIFT                   8

/* Plain ATA (hard disk) Error register bit flags -- independent bits,
   used only when the target is NOT an ATAPI device. */
#define IDE_ERROR_MEDIA_CHANGE          0x20
#define IDE_ERROR_MEDIA_CHANGE_REQ      0x08

/* ATAPI Error register: the high nibble (>> 4) is the real SCSI sense
   key, not a bitmask -- matches SCSI_SENSE_* in scsi.h (NO_SENSE=0x00,
   RECOVERED_ERROR=0x01, NOT_READY=0x02, MEDIUM_ERROR=0x03,
   HARDWARE_ERROR=0x04, ILLEGAL_REQUEST=0x05, UNIT_ATTENTION=0x06,
   DATA_PROTECT=0x07, BLANK_CHECK=0x08, ABORTED_COMMAND=0x0B). */
#define ATAPI_ERROR_SENSE_KEY(errByte)  (((errByte) >> 4) & 0x0F)

#define SATA_SIG_ATA                    0x00000101
#define SATA_SIG_ATAPI                  0xEB140101

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

#define AHCI_READ_REG(base, off)        ScsiPortReadRegisterUlong((PULONG)((PUCHAR)(base) + (off)))
#define AHCI_WRITE_REG(base, off, val)  ScsiPortWriteRegisterUlong((PULONG)((PUCHAR)(base) + (off)), (ULONG)(val))
#define AHCI_PORT_BASE(abar, port)      ((PUCHAR)(abar) + 0x100 + ((port) * 0x80))

ULONG __cdecl DbgPrint(PCH Format, ...);
#define AHCI_DBG_LOG(fmt, ...) DbgPrint("[AHCINT] " fmt "\n", __VA_ARGS__)
#define AHCI_DBG_MSG(msg)      DbgPrint("[AHCINT] " msg "\n")

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
    UCHAR Control;
    UCHAR Reserved;
} FIS_REG_H2D, *PFIS_REG_H2D;

#pragma pack(pop)

/* Real ATAPI MODE SENSE(10) parameter header, confirmed against
   Microsoft's own real, shipped NT4 ATAPI miniport source
   (private/ntos/miniport/atapi/atapi.c / atapi.h) -- NOT the same as
   this DDK's own SCSI.H MODE_PARAMETER_HEADER, which is the 4-byte
   SCSI-6 form (ModeDataLength, MediumType, DeviceSpecificParameter,
   BlockDescriptorLength). All-UCHAR, so no packing pragma is needed:
   natural alignment already matches the on-the-wire byte layout. Used
   by ahci_satl.c's AhciSatlProcessSrb to convert a real ATAPI drive's
   MODE SENSE(10) response back into the SCSI-6 MODE_PARAMETER_HEADER
   format ScsiPort/the CD-ROM class driver above us actually expects,
   the same way Microsoft's own driver's reverse-conversion code does. */
typedef struct _MODE_PARAMETER_HEADER_10 {
    UCHAR ModeDataLengthMsb;
    UCHAR ModeDataLengthLsb;
    UCHAR MediumType;
    UCHAR Reserved[5];
} MODE_PARAMETER_HEADER_10, *PMODE_PARAMETER_HEADER_10;

typedef struct _AHCI_DMA_RESOURCES {
    UCHAR RawBuffer[65536];
} AHCI_DMA_RESOURCES, *PAHCI_DMA_RESOURCES;

typedef struct _AHCI_PORT_INFO {
    BOOLEAN                 Present;
    BOOLEAN                 IsAtapi;
    BOOLEAN                 IdentifyValid;
    UCHAR                   IdentifyData[512];

    /* Raw PxTFD value from the last completed command on this port; used
       by AhciFillAutoSenseFromTfd in ahci_satl.c to fill real sense data
       into an SRB the instant a command fails. */
    ULONG                    LastTfd;

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
    PUCHAR                  AbarMapped;
    ULONG                   PciBus;
    ULONG                   PciSlot;
    UCHAR                   ActualIrq;
    ULONG                   HbaCapabilities;
    ULONG                   PortsImplemented;
    AHCI_PORT_INFO          Ports[MAX_SUPPORTED_PORTS];
    PSCSI_REQUEST_BLOCK     ActiveSrb;

    /* TRUE if the HAL assigned a real IRQ (see AhciFindAdapter);
       FALSE means FAST-POLL/fallback-timer is the only completion path. */
    BOOLEAN                 UseInterrupt;
    ULONG                   ActivePort;
    ULONG                   ActiveBytes;

    /* Generation counter: bumped on every new command dispatch (async or
       synchronous admin path). AhciFallbackTimer captures the generation
       it was armed for and no-ops if a newer command has since started,
       so a stale fallback callback can never grab an unrelated SRB. */
    ULONG                   CommandGeneration;
    ULONG                   FallbackArmedGeneration;

    PAHCI_DMA_RESOURCES     DmaArea;
    SCSI_PHYSICAL_ADDRESS   DmaAreaPhysical;
} HW_DEVICE_EXTENSION, *PHW_DEVICE_EXTENSION;

typedef struct _ATA_REQUEST {
    PVOID     DataBuffer;
    ULONG     DataBufferLen;
    BOOLEAN   IsWrite;
    BOOLEAN   IsAtapi;
    BOOLEAN   ForcePio;
    ULONGLONG Lba;
    USHORT    SectorCount;
} ATA_REQUEST, *PATA_REQUEST;

#pragma function(memset)
static __inline void * __cdecl memset(void *dst, int val, size_t count) {
    char *p = (char *)dst;
    while (count--) *p++ = (char)val;
    return dst;
}

#define ZeroMemoryBytes(dst, len) memset((dst), 0, (len))

VOID AhciStopPortEngines(IN PHW_DEVICE_EXTENSION HwInit, IN PUCHAR portBase);
VOID AhciFallbackTimer(IN PVOID DeviceExtension);

#endif