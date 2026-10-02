/*
 * AHCINT - SATA AHCI SCSI miniport driver
 *
 *   AHCI_W9X  Windows 95/98/Me (implies AHCI_NT4)
 *   AHCI_NT4  Windows NT 3.1/3.50/3.51/4.0
 *   (neither) Windows 2000/XP/2003, x86 and x64
 */

#ifndef _AHCINT_H_
#define _AHCINT_H_

#include <miniport.h>
#include <scsi.h>

/* ------------------------------------------------------------------ */
/* Target configuration                                                */
/* ------------------------------------------------------------------ */

#if defined(AHCI_W9X) && !defined(AHCI_NT4)
#define AHCI_NT4
#endif

#ifdef AHCI_NT4
/* NT4/9x: no Dma64BitAddresses field; class drivers send REQUEST SENSE */
#define AHCI_HAS_DMA64          0
#define AHCI_AUTOSENSE          0
#else
#define AHCI_HAS_DMA64          1
#define AHCI_AUTOSENSE          1
#endif

/* The NT build also runs on NT 3.1, whose ScsiPort and HAL differ:
   HW_INITIALIZATION_DATA must be exactly 40h bytes, the ConfigInfo is
   58h bytes (no SlotNumber / MaximumNumberOfTargets), the HAL has no PCI
   configuration access and ScsiPortSetBusDataByOffset does not exist. */
#if defined(AHCI_NT4) && !defined(AHCI_W9X)
#define AHCI_NT31_COMPAT
#endif

#ifndef FIELD_OFFSET
#define FIELD_OFFSET(type, field)       ((LONG)&(((type *)0)->field))
#endif

#ifdef AHCI_NT31_COMPAT
/* NT 3.1 HW_INITIALIZATION_DATA ends where VendorId starts; NT 3.50 to
   4.0 accept the structure up to DeviceId. */
#define AHCI_HWINIT_SIZE_NT31           ((ULONG)FIELD_OFFSET(HW_INITIALIZATION_DATA, VendorId))
#define AHCI_HWINIT_SIZE_NT35           ((ULONG)FIELD_OFFSET(HW_INITIALIZATION_DATA, DeviceId) + sizeof(PVOID))

/* ConfigInfo field present in the structure ScsiPort handed us? */
#define AHCI_CONFIG_HAS(ci, field)      ((ci)->Length >= (ULONG)FIELD_OFFSET(PORT_CONFIGURATION_INFORMATION, field) + sizeof((ci)->field))
#endif

/* NT family: always; 9x: DEBUG builds only */
#if !defined(AHCI_W9X) || defined(DEBUG)
#define AHCI_DBG_ENABLED
#endif

#ifdef AHCI_W9X
#define AHCI_DRIVER_NAME        "ahcint9x"
#else
#define AHCI_DRIVER_NAME        "AHCINT"
#endif

/* ------------------------------------------------------------------ */
/* Debug output                                                        */
/* ------------------------------------------------------------------ */

/* AHCI_DBG((AHCI_PFX "Port %lu\n", port)); -- double parentheses, since
   VC++ 2.0 / MSVC 4.0 have no variadic macros. Use %lu %lX %s %c only. */
#define AHCI_PFX                "[" AHCI_DRIVER_NAME "] "

#ifdef AHCI_DBG_ENABLED
#ifdef AHCI_W9X
VOID __cdecl AhciDbgPrint(PCHAR Format, ...);
#define AHCI_DBG(args)          AhciDbgPrint args
#else
ULONG __cdecl DbgPrint(PCH Format, ...);
#define AHCI_DBG(args)          DbgPrint args
#endif
#else
#define AHCI_DBG(args)
#endif

/* ------------------------------------------------------------------ */
/* Limits                                                              */
/* ------------------------------------------------------------------ */

#define MAX_SUPPORTED_PORTS             8           /* 1..32; also the initiator ID */
#define AHCI_MAX_CONTROLLERS            8
#define AHCI_MAX_TRANSFER               0x20000     /* 128 KB */
#define AHCI_SECTOR_SIZE                512         /* default logical sector */
#define AHCI_MAX_SECTOR_SIZE            4096        /* largest logical sector taken */

/* An unaligned 128 KB buffer spans 33 pages; the command table fits 120 */
#define AHCI_MAX_PHYS_BREAKS            32
#define MAX_PRDT_ENTRIES                64

/* Per-port driver-owned DMA buffer: IDENTIFY data, and ATAPI REQUEST
   SENSE / MODE SENSE issued by the driver itself. */
#define AHCI_BOUNCE_SIZE                512

/* ------------------------------------------------------------------ */
/* PCI                                                                 */
/* ------------------------------------------------------------------ */

#define PCI_CLASS_MASS_STORAGE          0x01
#define PCI_SUBCLASS_AHCI               0x06
#define PCI_PROGIF_AHCI                 0x01
#define AHCI_ABAR_INDEX                 5           /* BAR5 = ABAR */
/* Generic host registers plus the port blocks the driver uses. Claimed
   and mapped as is: HBAs often sit only 4 KB apart, so a larger range
   would collide with the next controller's ABAR. */
#define AHCI_ABAR_MAP_SIZE              (0x100 + 0x80 * MAX_SUPPORTED_PORTS)

/* ------------------------------------------------------------------ */
/* HBA registers                                                       */
/* ------------------------------------------------------------------ */

#define AHCI_GEN_CAP                    0x00
#define AHCI_GEN_GHC                    0x04
#define AHCI_GEN_IS                     0x08
#define AHCI_GEN_PI                     0x0C
#define AHCI_GEN_VS                     0x10
#define AHCI_GEN_CAP2                   0x24
#define AHCI_GEN_BOHC                   0x28

#define AHCI_GHC_HR                     (1UL << 0)
#define AHCI_GHC_IE                     (1UL << 1)
#define AHCI_GHC_AE                     (1UL << 31)

#define AHCI_CAP_SCLO                   (1UL << 24)
#define AHCI_CAP_S64A                   (1UL << 31)

#define AHCI_CAP2_BOH                   (1UL << 0)

#define AHCI_BOHC_BOS                   (1UL << 0)
#define AHCI_BOHC_OOS                   (1UL << 1)

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

#define AHCI_PORT_IS_TFES               (1UL << 30)
#define AHCI_PORT_IS_HBFS               (1UL << 29)
#define AHCI_PORT_IS_HBDS               (1UL << 28)
#define AHCI_PORT_IS_IFS                (1UL << 27)
#define AHCI_PORT_IS_FATAL              (AHCI_PORT_IS_TFES | AHCI_PORT_IS_HBFS | \
                                         AHCI_PORT_IS_HBDS | AHCI_PORT_IS_IFS)

#define AHCI_SSTS_DET_MASK              0x0F
#define AHCI_SSTS_DET_NONE              0x00
#define AHCI_SSTS_DET_PHY               0x03

#define TFD_STS_ERR                     0x01
#define TFD_STS_DRQ                     0x08
#define TFD_STS_DF                      0x20
#define TFD_STS_BSY                     0x80
#define TFD_ERR_SHIFT                   8       /* PxTFD[15:8] = ATA Error register */

#define SATA_SIG_ATA                    0x00000101
#define SATA_SIG_ATAPI                  0xEB140101

#define AHCI_READ_REG(base, off)        ScsiPortReadRegisterUlong((PULONG)((PUCHAR)(base) + (off)))
#define AHCI_WRITE_REG(base, off, val)  ScsiPortWriteRegisterUlong((PULONG)((PUCHAR)(base) + (off)), (ULONG)(val))
#define AHCI_PORT_BASE(abar, port)      ((PUCHAR)(abar) + 0x100 + ((port) * 0x80))

/* Command header DW0 */
#define AHCI_CMDHDR_CFL_H2D             5           /* FIS length in DWORDs */
#define AHCI_CMDHDR_ATAPI               (1 << 5)
#define AHCI_CMDHDR_WRITE               (1 << 6)

/* Command table layout */
#define AHCI_CMDTBL_ACMD_OFFSET         0x40
#define AHCI_CMDTBL_PRDT_OFFSET         0x80

/* Polling (Fast Polling: every command is completed inside HwStartIo) */
#define AHCI_POLL_INTERVAL_USEC         10

/* Per-command limit, from the SRB's TimeOutValue and ending one second
   before ScsiPort's own request timer would expire: a single write can
   take seconds (spin-up, a growing dynamic VHD, host cache flushes), and
   on MP systems the port driver's timer runs on another CPU while
   HwStartIo still polls. */
#define AHCI_POLL_TIMEOUT_DEFAULT_SEC   10      /* SRB without TimeOutValue */
#define AHCI_POLL_TIMEOUT_INTERNAL_SEC  3       /* driver's own commands */
#define AHCI_POLL_TIMEOUT_MAX_SEC       60

/* ------------------------------------------------------------------ */
/* ATA                                                                 */
/* ------------------------------------------------------------------ */

#define FIS_TYPE_REG_H2D                0x27
#define FIS_H2D_COMMAND                 0x80        /* C bit */
#define ATA_DEV_LBA                     0x40

#define IDE_COMMAND_IDENTIFY_DEVICE     0xEC
#define IDE_COMMAND_IDENTIFY_PACKET     0xA1
#define IDE_COMMAND_PACKET              0xA0
#define IDE_COMMAND_READ_DMA            0xC8
#define IDE_COMMAND_WRITE_DMA           0xCA
#define IDE_COMMAND_READ_DMA_EXT        0x25
#define IDE_COMMAND_WRITE_DMA_EXT       0x35

/* ATA Error register */
#define IDE_ERROR_ABORT                 0x04
#define IDE_ERROR_MEDIA_CHANGE_REQ      0x08
#define IDE_ERROR_ID_NOT_FOUND          0x10
#define IDE_ERROR_MEDIA_CHANGE          0x20
#define IDE_ERROR_UNCORRECTABLE         0x40

/* ATAPI: Error register [7:4] is the SCSI sense key */
#define ATAPI_ERROR_SENSE_KEY(err)      (((err) >> 4) & 0x0F)

/* IDENTIFY DEVICE words */
#define IDW_GENERAL_CONFIG              0
#define IDW_CYLINDERS                   1
#define IDW_HEADS                       3
#define IDW_SECTORS_PER_TRACK           6
#define IDW_SERIAL                      10          /* 10..19 */
#define IDW_FIRMWARE                    23
#define IDW_MODEL                       27
#define IDW_CAPABILITIES                49
#define IDW_LBA28_SECTORS               60          /* 60..61 */
#define IDW_CMDSET_SUPPORTED_2          83
#define IDW_CMDSET_ENABLED_2            86
#define IDW_LBA48_SECTORS               100         /* 100..103 */
#define IDW_SECTOR_SIZE_INFO            106
#define IDW_LOGICAL_SECTOR_WORDS        117         /* 117..118 */

#define IDW49_LBA_SUPPORTED             0x0200
#define IDW83_VALID_MASK                0xC000
#define IDW83_VALID                     0x4000
#define IDW83_LBA48                     0x0400
#define IDW86_LBA48                     0x0400
#define IDW106_VALID_MASK               0xC000
#define IDW106_VALID                    0x4000
#define IDW106_MULTI_LOGICAL            0x2000      /* several logical per physical */
#define IDW106_LONG_LOGICAL             0x1000      /* logical sector > 256 words */
#define IDW106_PHYS_EXP_MASK            0x000F

/* 28-bit commands: LBA 0..0x0FFFFFFF, 1..256 sectors (count 0 = 256).
   48-bit commands: 1..65536 sectors (count 0 = 65536). */
#define ATA_LBA28_LIMIT                 0x10000000UL
#define ATA_LBA28_MAX_SECTORS           256
#define ATA_LBA48_MAX_SECTORS           65536

/* ------------------------------------------------------------------ */
/* SCSI opcodes missing from older SCSI.H versions                     */
/* ------------------------------------------------------------------ */

#ifndef SCSIOP_SYNCHRONIZE_CACHE
#define SCSIOP_SYNCHRONIZE_CACHE        0x35        /* missing from 95 DDK */
#endif
#ifndef SCSIOP_MODE_SENSE10
#define SCSIOP_MODE_SENSE10             0x5A
#endif

/* 16-byte CDBs (SBC-2). Own names, so no clash with newer SCSI.H. */
#define AHCI_SCSIOP_READ16              0x88
#define AHCI_SCSIOP_WRITE16             0x8A
#define AHCI_SCSIOP_VERIFY16            0x8F
#define AHCI_SCSIOP_SYNC_CACHE16        0x91
#define AHCI_SCSIOP_SERVICE_ACTION_IN16 0x9E
#define AHCI_SA_READ_CAPACITY16         0x10
#define AHCI_READ_CAPACITY16_DATA_LEN   32

/* INQUIRY VERSION for ATA disks: SPC-3, advertises 16-byte CDB support */
#define AHCI_INQUIRY_VERSION_SPC3       0x05

/* VPD pages */
#define AHCI_VPD_SUPPORTED_PAGES        0x00
#define AHCI_VPD_SERIAL_NUMBER          0x80

/* Sense codes used by the SATL */
#define AHCI_ASC_NONE                   0x00
#define AHCI_ASC_UNRECOVERED_READ       0x11
#define AHCI_ASC_INVALID_OPCODE         0x20
#define AHCI_ASC_LBA_OUT_OF_RANGE       0x21
#define AHCI_ASC_INVALID_CDB_FIELD      0x24
#define AHCI_ASC_MEDIUM_CHANGED         0x28
#define AHCI_ASC_NO_MEDIA               0x3A

/* ------------------------------------------------------------------ */
/* 64-bit block numbers without compiler 64-bit types                  */
/* ------------------------------------------------------------------ */

/* Two ULONGs instead of __int64: no CRT helpers (_aulldiv, ...) needed,
   the 9x build links no CRT. */
typedef struct _AHCI_LBA {
    ULONG Low;
    ULONG High;
} AHCI_LBA, *PAHCI_LBA;

/* ------------------------------------------------------------------ */
/* Hardware structures                                                 */
/* ------------------------------------------------------------------ */

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

/* ATAPI MODE SENSE(10) parameter header (8 bytes; SCSI.H only has the
   4-byte MODE SENSE(6) MODE_PARAMETER_HEADER). All UCHAR, no packing. */
typedef struct _MODE_PARAMETER_HEADER_10 {
    UCHAR ModeDataLengthMsb;
    UCHAR ModeDataLengthLsb;
    UCHAR MediumType;
    UCHAR Reserved[5];
} MODE_PARAMETER_HEADER_10, *PMODE_PARAMETER_HEADER_10;

/* ------------------------------------------------------------------ */
/* Driver state                                                        */
/* ------------------------------------------------------------------ */

typedef struct _AHCI_PORT_INFO {
    BOOLEAN                 Present;
    BOOLEAN                 IsAtapi;
    BOOLEAN                 IdentifyValid;

    /* Parsed from IDENTIFY DEVICE (ATA only), see AhciParseIdentify */
    BOOLEAN                 LbaSupported;
    BOOLEAN                 Lba48;
    AHCI_LBA                TotalSectors;   /* in logical sectors */
    ULONG                   SectorSize;     /* logical sector, bytes */
    UCHAR                   PhysExponent;   /* log2(logical sectors per physical) */

    /* Sense data for the next REQUEST SENSE on an ATA target (ATAPI
       targets answer REQUEST SENSE from the drive itself). */
    BOOLEAN                 SensePending;
    UCHAR                   SenseKey;
    UCHAR                   SenseAsc;
    UCHAR                   SenseAscq;

    UCHAR                   IdentifyData[512];

    /* PxTFD of the last completed command */
    ULONG                   LastTfd;

    PAHCI_COMMAND_HEADER    CommandList;
    ULONG                   CommandListPhysical;
    ULONG                   CommandListPhysicalUpper;

    PUCHAR                  ReceivedFis;
    ULONG                   ReceivedFisPhysical;
    ULONG                   ReceivedFisPhysicalUpper;

    PUCHAR                  CommandTable;
    ULONG                   CommandTablePhysical;
    ULONG                   CommandTablePhysicalUpper;

    PUCHAR                  BounceBuffer;
    ULONG                   BouncePhysical;
    ULONG                   BouncePhysicalUpper;
} AHCI_PORT_INFO, *PAHCI_PORT_INFO;

typedef struct _HW_DEVICE_EXTENSION {
    PUCHAR                  AbarMapped;
    ULONG                   PciBus;
    ULONG                   PciSlot;
    ULONG                   HbaCapabilities;
    ULONG                   PortsImplemented;
    AHCI_PORT_INFO          Ports[MAX_SUPPORTED_PORTS];
    PAHCI_DMA_RESOURCES     DmaArea;
    SCSI_PHYSICAL_ADDRESS   DmaAreaPhysical;
} HW_DEVICE_EXTENSION, *PHW_DEVICE_EXTENSION;

/* One ATA/ATAPI command to put on the wire */
typedef struct _ATA_REQUEST {
    PVOID     DataBuffer;       /* ignored with UseBounce */
    ULONG     DataBufferLen;
    BOOLEAN   IsWrite;
    BOOLEAN   IsAtapi;
    BOOLEAN   UseBounce;        /* data in the port's BounceBuffer */
    BOOLEAN   Is48Bit;          /* ATA only */
    UCHAR     Command;          /* ATA only */
    AHCI_LBA  Lba;              /* ATA only */
    ULONG     SectorCount;      /* ATA only, 1..65536 */
    PUCHAR    Cdb;              /* ATAPI only */
    ULONG     CdbLength;
} ATA_REQUEST, *PATA_REQUEST;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

#if !defined(AHCI_NT4)
/* The 2000/XP/x64 compilers may turn a zeroing loop into a memset call;
   provide it inline so no CRT is needed. */
#pragma function(memset)
static __inline void * __cdecl memset(void *dst, int val, size_t count) {
    char *p = (char *)dst;
    while (count--) *p++ = (char)val;
    return dst;
}
#define ZeroMemoryBytes(dst, len)       memset((dst), 0, (len))
#else
static __inline void ZeroMemoryBytes(PVOID dst, ULONG len) {
    PUCHAR d = (PUCHAR)dst;
    while (len--) *d++ = 0;
}
#endif

static __inline void CopyMemoryBytes(PVOID dst, PVOID src, ULONG len) {
    PUCHAR d = (PUCHAR)dst;
    PUCHAR s = (PUCHAR)src;
    while (len--) *d++ = *s++;
}

/* ------------------------------------------------------------------ */
/* Cross-file prototypes                                               */
/* ------------------------------------------------------------------ */

/* ahcimain.c */
VOID    AhciStopPortEngines(IN PHW_DEVICE_EXTENSION HwInit, IN PUCHAR PortBase);
VOID    AhciRestartPort(IN PHW_DEVICE_EXTENSION HwInit, IN PUCHAR PortBase);

/* ahcisatl.c */
VOID    AhciParseIdentify(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port);
VOID    AhciSatlProcessSrb(IN PHW_DEVICE_EXTENSION HwInit, IN PSCSI_REQUEST_BLOCK Srb);

#endif /* _AHCINT_H_ */
