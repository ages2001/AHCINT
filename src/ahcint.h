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

#define AHCI_DRIVER_VERSION     0x01050000  /* 1.5.0.0 */

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
#define AHCI_MAX_LUNS                   8           /* devices behind one port (port multiplier) */
#define AHCI_MAX_CONTROLLERS            8
#define AHCI_MAX_TRANSFER               0x20000     /* 128 KB */
#define AHCI_SECTOR_SIZE                512         /* default logical sector */
#define AHCI_MAX_SECTOR_SIZE            4096        /* largest logical sector taken */

/* An unaligned 128 KB buffer spans 33 pages; one PRD per page */
#define AHCI_MAX_PHYS_BREAKS            32
#define MAX_PRDT_ENTRIES                40
#define AHCI_CMDTBL_SIZE                (0x80 + MAX_PRDT_ENTRIES * 16)     /* 768 */

/* Command slots: one for the synchronous path, up to 32 with NCQ */
#define AHCI_MAX_SLOTS                  32
/* Upper bound for the whole DMA arena (all ports, all slots) */
#define AHCI_DMA_BUDGET                 0x28000     /* 160 KB */

/* Per-port driver-owned DMA buffer: IDENTIFY data, ATAPI REQUEST SENSE /
   MODE SENSE, READ LOG EXT. */
#define AHCI_BOUNCE_SIZE                512

/* Sense kept for the next REQUEST SENSE: fixed (18) or descriptor format */
#define AHCI_SENSE_MAX                  32

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

#define AHCI_CAP_NCS_SHIFT              8           /* command slots - 1 */
#define AHCI_CAP_NCS_MASK               0x1F
#define AHCI_CAP_SPM                    (1UL << 17)
#define AHCI_CAP_SCLO                   (1UL << 24)
#define AHCI_CAP_SSS                    (1UL << 27)
#define AHCI_CAP_SNCQ                   (1UL << 30)
#define AHCI_CAP_S64A                   (1UL << 31)
#define AHCI_CAP_ISS_SHIFT              20          /* interface speed: 1 Gen1, 2 Gen2, 3 Gen3 */
#define AHCI_CAP_ISS_MASK               0x0F

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
#define AHCI_PORT_CMD_PMA               (1UL << 17)

#define AHCI_PORT_IS_DHRS               (1UL << 0)
#define AHCI_PORT_IS_PSS                (1UL << 1)
#define AHCI_PORT_IS_DSS                (1UL << 2)
#define AHCI_PORT_IS_SDBS               (1UL << 3)
#define AHCI_PORT_IS_DPS                (1UL << 5)
#define AHCI_PORT_IS_OFS                (1UL << 24)
#define AHCI_PORT_IS_INFS               (1UL << 26)
#define AHCI_PORT_IS_IFS                (1UL << 27)
#define AHCI_PORT_IS_HBDS               (1UL << 28)
#define AHCI_PORT_IS_HBFS               (1UL << 29)
#define AHCI_PORT_IS_TFES               (1UL << 30)
#define AHCI_PORT_IS_FATAL              (AHCI_PORT_IS_TFES | AHCI_PORT_IS_HBFS | \
                                         AHCI_PORT_IS_HBDS | AHCI_PORT_IS_IFS)
/* Port interrupts used with NCQ: completions and errors */
#define AHCI_PORT_IE_ASYNC              (AHCI_PORT_IS_DHRS | AHCI_PORT_IS_PSS | AHCI_PORT_IS_DSS | \
                                         AHCI_PORT_IS_SDBS | AHCI_PORT_IS_DPS | AHCI_PORT_IS_OFS | \
                                         AHCI_PORT_IS_INFS | AHCI_PORT_IS_FATAL)

#define AHCI_SSTS_DET_MASK              0x0F
#define AHCI_SSTS_DET_NONE              0x00
#define AHCI_SSTS_DET_PHY               0x03
#define AHCI_SSTS_SPD_SHIFT             4           /* negotiated speed, 0 = none */
#define AHCI_SSTS_SPD_MASK              0x0F
#define AHCI_SCTL_SPD_MASK              0xF0        /* speed limit, 0 = none */

#define TFD_STS_ERR                     0x01
#define TFD_STS_DRQ                     0x08
#define TFD_STS_DF                      0x20
#define TFD_STS_BSY                     0x80
#define TFD_ERR_SHIFT                   8       /* PxTFD[15:8] = ATA Error register */

#define SATA_SIG_ATA                    0x00000101
#define SATA_SIG_ATAPI                  0xEB140101
#define SATA_SIG_PM                     0x96690101

#define AHCI_READ_REG(base, off)        ScsiPortReadRegisterUlong((PULONG)((PUCHAR)(base) + (off)))
#define AHCI_WRITE_REG(base, off, val)  ScsiPortWriteRegisterUlong((PULONG)((PUCHAR)(base) + (off)), (ULONG)(val))
#define AHCI_PORT_BASE(abar, port)      ((PUCHAR)(abar) + 0x100 + ((port) * 0x80))

/* Command header DW0 */
#define AHCI_CMDHDR_CFL_H2D             5           /* FIS length in DWORDs */
#define AHCI_CMDHDR_ATAPI               (1 << 5)
#define AHCI_CMDHDR_WRITE               (1 << 6)
#define AHCI_CMDHDR_RESET               (1 << 8)
#define AHCI_CMDHDR_CLEAR_BUSY          (1 << 10)
#define AHCI_CMDHDR_PMP_SHIFT           12

/* Command table layout */
#define AHCI_CMDTBL_ACMD_OFFSET         0x40
#define AHCI_CMDTBL_PRDT_OFFSET         0x80

/* Received FIS area */
#define AHCI_RFIS_PIO_SETUP             0x20
#define AHCI_RFIS_D2H                   0x40
#define AHCI_RFIS_SIZE                  256

/* Polling (Fast Polling: every non-queued command is completed inside HwStartIo) */
#define AHCI_POLL_INTERVAL_USEC         10

/* Per-command limit, from the SRB's TimeOutValue and ending one second
   before ScsiPort's own request timer would expire: a single write can
   take seconds (spin-up, a growing dynamic VHD, host cache flushes), and
   on MP systems the port driver's timer runs on another CPU while
   HwStartIo still polls. */
#define AHCI_POLL_TIMEOUT_DEFAULT_SEC   10      /* SRB without TimeOutValue */
#define AHCI_POLL_TIMEOUT_INTERNAL_SEC  3       /* driver's own commands */
#define AHCI_POLL_TIMEOUT_MAX_SEC       60
#define AHCI_POLL_TIMEOUT_POWER_SEC     30      /* flush / standby */

/* NCQ watchdog: the timer completes what the interrupt missed; after this
   many misses in a row the adapter falls back to Fast Polling. */
#define AHCI_TIMER_NCQ_USEC             100000
#define AHCI_MISSED_IRQ_LIMIT           5
#define AHCI_NCQ_ERROR_LIMIT            3

/* ------------------------------------------------------------------ */
/* ATA                                                                 */
/* ------------------------------------------------------------------ */

#define FIS_TYPE_REG_H2D                0x27
#define FIS_H2D_COMMAND                 0x80        /* C bit */
#define FIS_CONTROL_SRST                0x04
#define ATA_DEV_LBA                     0x40
#define ATA_DEV_FUA                     0x80        /* FPDMA */
#define AHCI_PMP_CONTROL                0x0F        /* port multiplier control port */

#define IDE_COMMAND_IDENTIFY_DEVICE     0xEC
#define IDE_COMMAND_IDENTIFY_PACKET     0xA1
#define IDE_COMMAND_PACKET              0xA0
#define IDE_COMMAND_READ_DMA            0xC8
#define IDE_COMMAND_WRITE_DMA           0xCA
#define IDE_COMMAND_READ_DMA_EXT        0x25
#define IDE_COMMAND_WRITE_DMA_EXT       0x35
#define IDE_COMMAND_WRITE_DMA_FUA_EXT   0x3D
#define IDE_COMMAND_READ_VERIFY         0x40
#define IDE_COMMAND_READ_VERIFY_EXT     0x42
#define IDE_COMMAND_READ_LOG_EXT        0x2F
#define IDE_COMMAND_READ_FPDMA          0x60
#define IDE_COMMAND_WRITE_FPDMA         0x61
#define IDE_COMMAND_SMART               0xB0
#define IDE_COMMAND_DATA_SET_MANAGEMENT 0x06
#define IDE_DSM_TRIM                    0x0001
#define IDE_DSM_RANGES_PER_BLOCK        64
#define IDE_DSM_MAX_RANGE               0xFFFF
#define IDE_SMART_RETURN_STATUS_FEATURE 0xDA
#define IDE_SMART_READ_DATA_FEATURE     0xD0
#define IDE_SMART_LBA_SIGNATURE         0xC24F00
#define IDE_COMMAND_STANDBY_IMMEDIATE   0xE0
#define IDE_COMMAND_IDLE_IMMEDIATE      0xE1
#define IDE_COMMAND_FLUSH_CACHE         0xE7
#define IDE_COMMAND_READ_PM             0xE4
#define IDE_COMMAND_WRITE_PM            0xE8
#define IDE_COMMAND_FLUSH_CACHE_EXT     0xEA
#define IDE_COMMAND_SET_FEATURES        0xEF

#define IDE_FEATURE_ENABLE_WRITE_CACHE  0x02
#define IDE_FEATURE_DISABLE_LOOKAHEAD   0x55
#define IDE_FEATURE_DISABLE_WRITE_CACHE 0x82
#define IDE_FEATURE_ENABLE_LOOKAHEAD    0xAA

#define ATA_LOG_NCQ_ERROR               0x10

/* Port multiplier registers (READ/WRITE PORT MULTIPLIER) */
#define PM_GSCR_PRODUCT                 0
#define PM_GSCR_REVISION                1
#define PM_GSCR_INFO                    2           /* [3:0] device ports */
#define PM_GSCR_ERROR                   32
#define PM_PSCR_SSTATUS                 0
#define PM_PSCR_SERROR                  1
#define PM_PSCR_SCONTROL                2
#define AHCI_PM_MAX_PORTS               15

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
#define IDW_QUEUE_DEPTH                 75
#define IDW_SATA_CAPABILITIES           76
#define IDW_CMDSET_SUPPORTED_1          82
#define IDW_CMDSET_SUPPORTED_2          83
#define IDW_CMDSET_SUPPORTED_EXT        84
#define IDW_CMDSET_ENABLED_1            85
#define IDW_CMDSET_ENABLED_2            86
#define IDW_LBA48_SECTORS               100         /* 100..103 */
#define IDW_SECTOR_SIZE_INFO            106
#define IDW_ADDITIONAL_SUPPORTED        69
#define IDW_FORM_FACTOR                 168
#define IDW_DSM_SUPPORT                 169
#define IDW_ROTATION_RATE               217
#define IDW69_DETERMINISTIC_TRIM        0x4000
#define IDW69_ZERO_AFTER_TRIM           0x0020
#define IDW169_TRIM                     0x0001
#define IDW_LOGICAL_SECTOR_WORDS        117         /* 117..118 */

#define IDW49_LBA_SUPPORTED             0x0200
#define IDW76_NCQ                       0x0100
#define IDW82_SMART                     0x0001
#define IDW82_WRITE_CACHE               0x0020
#define IDW82_LOOKAHEAD                 0x0040
#define IDW83_VALID_MASK                0xC000
#define IDW83_VALID                     0x4000
#define IDW83_LBA48                     0x0400
#define IDW83_FLUSH                     0x1000
#define IDW83_FLUSH_EXT                 0x2000
#define IDW84_FUA                       0x0040
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

/* Own names, so no clash with newer SCSI.H */
#define AHCI_SCSIOP_START_STOP_UNIT     0x1B
#define AHCI_SCSIOP_MODE_SELECT6        0x15
#define AHCI_SCSIOP_MODE_SELECT10       0x55
#define AHCI_SCSIOP_WRITE_VERIFY        0x2E
#define AHCI_SCSIOP_ATA_PASSTHROUGH12   0xA1
#define AHCI_SCSIOP_REPORT_LUNS         0xA0
#define AHCI_SCSIOP_READ12              0xA8
#define AHCI_SCSIOP_WRITE12             0xAA
#define AHCI_SCSIOP_VERIFY12            0xAF
#define AHCI_SCSIOP_ATA_PASSTHROUGH16   0x85
#define AHCI_SCSIOP_READ16              0x88
#define AHCI_SCSIOP_WRITE16             0x8A
#define AHCI_SCSIOP_VERIFY16            0x8F
#define AHCI_SCSIOP_SYNC_CACHE16        0x91
#define AHCI_SCSIOP_SERVICE_ACTION_IN16 0x9E
#define AHCI_SCSIOP_UNMAP               0x42
#define AHCI_SCSIOP_LOG_SENSE           0x4D
#define AHCI_SA_READ_CAPACITY16         0x10
#define AHCI_READ_CAPACITY16_DATA_LEN   32

/* INQUIRY VERSION for ATA disks: SPC-3, advertises 16-byte CDB support */
#define AHCI_INQUIRY_VERSION_SPC3       0x05

/* VPD pages */
#define AHCI_VPD_SUPPORTED_PAGES        0x00
#define AHCI_VPD_SERIAL_NUMBER          0x80
#define AHCI_VPD_BLOCK_LIMITS           0xB0
#define AHCI_VPD_BLOCK_CHARACTERISTICS  0xB1
#define AHCI_VPD_PROVISIONING           0xB2

#define AHCI_LOG_PAGE_SUPPORTED         0x00
#define AHCI_LOG_PAGE_TEMPERATURE       0x0D
#define AHCI_LOG_PAGE_INFO_EXCEPTIONS   0x2F
#define AHCI_ASC_FAILURE_PREDICTION     0x5D
#define AHCI_UNMAP_MAX_DESCRIPTORS      64
#define AHCI_UNMAP_MAX_BLOCKS           (IDE_DSM_RANGES_PER_BLOCK * IDE_DSM_MAX_RANGE)

/* Mode pages */
#define AHCI_MODE_PAGE_CACHING          0x08
#define AHCI_MODE_PAGE_GEOMETRY         0x04
#define AHCI_MODE_PAGE_CONTROL          0x0A
#define AHCI_MODE_PAGE_ALL              0x3F

/* Sense codes used by the SATL */
#define AHCI_ASC_NONE                   0x00
#define AHCI_ASC_UNRECOVERED_READ       0x11
#define AHCI_ASC_PARAM_LIST_LENGTH      0x1A
#define AHCI_ASC_INVALID_OPCODE         0x20
#define AHCI_ASC_LBA_OUT_OF_RANGE       0x21
#define AHCI_ASC_INVALID_CDB_FIELD      0x24
#define AHCI_ASC_INVALID_PARAM_FIELD    0x26
#define AHCI_ASC_MEDIUM_CHANGED         0x28
#define AHCI_ASC_POWER_ON_RESET         0x29
#define AHCI_ASC_NO_MEDIA               0x3A
#define AHCI_ASCQ_ATA_INFO_AVAILABLE    0x1D        /* with ASC 00h */

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

/* ATA registers as the device returned them (D2H Register or PIO Setup
   FIS): status, error, LBA, count, device. */
typedef struct _AHCI_ATA_RESULT {
    UCHAR Status;
    UCHAR Error;
    UCHAR Lba0;
    UCHAR Lba1;
    UCHAR Lba2;
    UCHAR Device;
    UCHAR Lba3;
    UCHAR Lba4;
    UCHAR Lba5;
    UCHAR CountLow;
    UCHAR CountHigh;
} AHCI_ATA_RESULT, *PAHCI_ATA_RESULT;

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
/* Miniport IOCTLs (SRB_FUNCTION_IO_CONTROL)                           */
/* ------------------------------------------------------------------ */

/* Same layout as SRB_IO_CONTROL (NTDDSCSI.H), declared here so the 9x and
   NT 3.x DDKs need no extra header. */
typedef struct _AHCI_SRB_IO_CONTROL {
    ULONG HeaderLength;
    UCHAR Signature[8];
    ULONG Timeout;
    ULONG ControlCode;
    ULONG ReturnCode;
    ULONG Length;
} AHCI_SRB_IO_CONTROL, *PAHCI_SRB_IO_CONTROL;

/* "AHCINT  ": power control for an ACPI driver on NT 3.1-4.0, and info.
   Send through IOCTL_SCSI_MINIPORT to \\.\ScsiN:. */
#define AHCI_IOCTL_SIGNATURE            "AHCINT  "
#define AHCI_IOCTL_GET_INFO             0x00000001
#define AHCI_IOCTL_POWER_DOWN           0x00000010  /* flush, standby, stop ports */
#define AHCI_IOCTL_POWER_UP             0x00000011  /* reinitialize the HBA */
#define AHCI_IOCTL_TRIM_MODE_ON         0x00000020  /* 4 KB pattern after the header */
#define AHCI_IOCTL_TRIM_MODE_OFF        0x00000021
#define AHCI_TRIM_PATTERN_SIZE          4096

#define AHCI_INFO_ASYNC                 0x00000001  /* NCQ with interrupts active */
#define AHCI_INFO_POWERED_DOWN          0x00000002
#define AHCI_INFO_TRIM_MODE             0x00000008  /* writes of the TRIM pattern become TRIMs */

typedef struct _AHCI_IOCTL_INFO {
    ULONG Version;                      /* AHCI_DRIVER_VERSION */
    ULONG Capabilities;                 /* CAP */
    ULONG PortsImplemented;
    ULONG Flags;                        /* AHCI_INFO_xxx */
    ULONG SlotsPerPort;
    UCHAR DeviceCount[MAX_SUPPORTED_PORTS];     /* per port; > 1 behind a PM */
} AHCI_IOCTL_INFO, *PAHCI_IOCTL_INFO;

/* "SCSIDISK": SMART, as IOCTL_SCSI_MINIPORT_SMART_xxx (NTDDSCSI.H) */
#define AHCI_SMART_SIGNATURE            "SCSIDISK"
#define AHCI_SMART_BASE                 0x001B0500
#define AHCI_SMART_VERSION              (AHCI_SMART_BASE + 0x00)
#define AHCI_SMART_IDENTIFY             (AHCI_SMART_BASE + 0x01)
#define AHCI_SMART_READ_ATTRIBS         (AHCI_SMART_BASE + 0x02)
#define AHCI_SMART_READ_THRESHOLDS      (AHCI_SMART_BASE + 0x03)
#define AHCI_SMART_ENABLE               (AHCI_SMART_BASE + 0x04)
#define AHCI_SMART_DISABLE              (AHCI_SMART_BASE + 0x05)
#define AHCI_SMART_RETURN_STATUS        (AHCI_SMART_BASE + 0x06)
#define AHCI_SMART_AUTOSAVE             (AHCI_SMART_BASE + 0x07)
#define AHCI_SMART_SAVE_ATTRIBS         (AHCI_SMART_BASE + 0x08)
#define AHCI_SMART_OFFLINE_DIAGS        (AHCI_SMART_BASE + 0x09)
#define AHCI_SMART_AUTO_OFFLINE         (AHCI_SMART_BASE + 0x0A)
#define AHCI_SMART_READ_LOG             (AHCI_SMART_BASE + 0x0B)
#define AHCI_SMART_WRITE_LOG            (AHCI_SMART_BASE + 0x0C)

#pragma pack(push, 1)

/* IDEREGS / SENDCMDINPARAMS / SENDCMDOUTPARAMS / GETVERSIONINPARAMS (NTDDDISK.H) */
typedef struct _AHCI_IDEREGS {
    UCHAR Features;
    UCHAR SectorCount;
    UCHAR SectorNumber;
    UCHAR CylLow;
    UCHAR CylHigh;
    UCHAR DriveHead;
    UCHAR Command;
    UCHAR Reserved;
} AHCI_IDEREGS, *PAHCI_IDEREGS;

typedef struct _AHCI_SENDCMD_IN {
    ULONG        BufferSize;
    AHCI_IDEREGS Regs;
    UCHAR        DriveNumber;
    UCHAR        Reserved[3];
    ULONG        Reserved2[4];
    UCHAR        Buffer[1];
} AHCI_SENDCMD_IN, *PAHCI_SENDCMD_IN;

typedef struct _AHCI_SENDCMD_OUT {
    ULONG BufferSize;
    UCHAR DriverError;
    UCHAR IdeError;
    UCHAR Reserved[2];
    ULONG Reserved2[2];
    UCHAR Buffer[1];
} AHCI_SENDCMD_OUT, *PAHCI_SENDCMD_OUT;

typedef struct _AHCI_GETVERSION {
    UCHAR Version;
    UCHAR Revision;
    UCHAR Reserved;
    UCHAR IdeDeviceMap;
    ULONG Capabilities;
    ULONG Reserved2[4];
} AHCI_GETVERSION, *PAHCI_GETVERSION;

#pragma pack(pop)

#define AHCI_SENDCMD_IN_HEADER          ((ULONG)FIELD_OFFSET(AHCI_SENDCMD_IN, Buffer))
#define AHCI_SENDCMD_OUT_HEADER         ((ULONG)FIELD_OFFSET(AHCI_SENDCMD_OUT, Buffer))
#define AHCI_SMART_CAP_ATA_ID           0x00000001
#define AHCI_SMART_CAP_ATAPI_ID         0x00000002
#define AHCI_SMART_CAP_SMART            0x00000004
#define AHCI_SMART_DRV_ERROR            1           /* DriverError: device error */
#define AHCI_SMART_INVALID_DRIVE        5
#define AHCI_SMART_INVALID_BUFFER       4

/* ------------------------------------------------------------------ */
/* Driver state                                                        */
/* ------------------------------------------------------------------ */

/* IDENTIFY words kept per device (indexes into AHCI_DEVICE.Id) */
#define AHCI_ID_CONFIG                  0           /* word 0 */
#define AHCI_ID_CYLINDERS               1           /* word 1 */
#define AHCI_ID_HEADS                   2           /* word 3 */
#define AHCI_ID_SPT                     3           /* word 6 */
#define AHCI_ID_CMDSET_1                4           /* word 82 */
#define AHCI_ID_CMDSET_EXT              5           /* word 84 */
#define AHCI_ID_ENABLED_1               6           /* word 85 */
#define AHCI_ID_SERIAL                  7           /* words 10..19 */
#define AHCI_ID_FIRMWARE                17          /* words 23..26 */
#define AHCI_ID_MODEL                   21          /* words 27..46 */
#define AHCI_ID_WORDS                   41

/* Device pool: entry n is the device directly on port n (or LUN 0 behind
   its PM); further PM devices take the entries after the ports. */
#define AHCI_MAX_DEVICES                (MAX_SUPPORTED_PORTS + 8)

/* One ATA/ATAPI device: directly on a port (LUN 0), or behind a port
   multiplier (LUN = n-th device found behind it). */
typedef struct _AHCI_DEVICE {
    BOOLEAN                 Present;
    BOOLEAN                 IsAtapi;
    BOOLEAN                 IdentifyValid;
    UCHAR                   Pmp;            /* PM device port; 0 when direct */

    /* Parsed from IDENTIFY DEVICE (ATA only), see AhciParseIdentify */
    BOOLEAN                 LbaSupported;
    BOOLEAN                 Lba48;
    BOOLEAN                 Ncq;            /* queued commands used for R/W */
    BOOLEAN                 Fua;            /* WRITE DMA FUA EXT */
    BOOLEAN                 FlushExt;
    UCHAR                   NcqDepth;
    UCHAR                   NcqErrors;
    UCHAR                   PhysExponent;   /* log2(logical sectors per physical) */
    BOOLEAN                 Trim;           /* DATA SET MANAGEMENT TRIM */
    BOOLEAN                 TrimZeroes;     /* deterministic zeroes after TRIM */
    BOOLEAN                 Smart;          /* SMART supported and enabled */
    UCHAR                   FormFactor;
    USHORT                  RotationRate;   /* 1 = non-rotating */
    AHCI_LBA                TotalSectors;   /* in logical sectors */
    ULONG                   SectorSize;     /* logical sector, bytes */

    /* Sense data for the next REQUEST SENSE (ATAPI devices answer it
       themselves unless the driver produced the error) */
    BOOLEAN                 SensePending;
    UCHAR                   SenseLength;
    UCHAR                   Sense[AHCI_SENSE_MAX];

    /* PxTFD of the last completed command */
    ULONG                   LastTfd;

    UCHAR                   Owner;          /* port + 1; 0 = free pool entry */

    /* IDENTIFY words kept after parsing, see AHCI_ID_xxx */
    USHORT                  Id[AHCI_ID_WORDS];
} AHCI_DEVICE, *PAHCI_DEVICE;

typedef struct _AHCI_PORT_INFO {
    BOOLEAN                 Implemented;
    BOOLEAN                 PmAttached;
    BOOLEAN                 NeedsSetup;     /* after COMRESET: PM links, device settings */
    UCHAR                   DeviceCount;    /* devices[0..DeviceCount-1] */
    UCHAR                   PmPorts;
    ULONG                   PmProduct;

    UCHAR                   DeviceIndex[AHCI_MAX_LUNS];     /* LUN -> HwInit->Devices[] */

    /* Settings changed through MODE SELECT, by PM port, applied again
       after a reset: 0 = as the drive has it, 1 = on, 2 = off */
    UCHAR                   WantWriteCache[16];
    UCHAR                   WantLookAhead[16];

    PAHCI_COMMAND_HEADER    CommandList;
    ULONG                   CommandListPhysical;
    ULONG                   CommandListPhysicalUpper;

    PUCHAR                  ReceivedFis;
    ULONG                   ReceivedFisPhysical;
    ULONG                   ReceivedFisPhysicalUpper;

    PUCHAR                  CommandTables;  /* slot n at n * AHCI_CMDTBL_SIZE */
    ULONG                   CommandTablesPhysical;
    ULONG                   CommandTablesPhysicalUpper;

    PUCHAR                  BounceBuffer;
    ULONG                   BouncePhysical;
    ULONG                   BouncePhysicalUpper;

    /* NCQ: slots in flight and their SRBs */
    ULONG                   Outstanding;
    PSCSI_REQUEST_BLOCK     SlotSrb[AHCI_MAX_SLOTS];
    ULONG                   NextSlot;
    BOOLEAN                 LuBlocked;      /* NextLuRequest held back: slots full */
    UCHAR                   MaxQueued;      /* deepest queue seen (debug output) */
    BOOLEAN                 SpeedChecked;   /* link speed checked once (see AhciInitializePort) */

    /* Registers of the last completed command (pass-through, PM access) */
    AHCI_ATA_RESULT         Result;
} AHCI_PORT_INFO, *PAHCI_PORT_INFO;

typedef struct _HW_DEVICE_EXTENSION {
    PUCHAR                  AbarMapped;
    ULONG                   PciBus;
    ULONG                   PciSlot;
    ULONG                   HbaCapabilities;
    ULONG                   PortsImplemented;
    ULONG                   Slots;          /* command tables per port */

    BOOLEAN                 AsyncMode;      /* NCQ with interrupt completion */
    BOOLEAN                 NcqAllowed;     /* not disabled by NCQ=0 */
    BOOLEAN                 Stopped;        /* ScsiStopAdapter / POWER_DOWN */
    BOOLEAN                 TimerArmed;
    BOOLEAN                 DumpMode;       /* crash dump / hibernation */
    BOOLEAN                 CheckState;     /* verify the HBA before the next request */
    ULONG                   MissedInterrupts;

    AHCI_PORT_INFO          Ports[MAX_SUPPORTED_PORTS];
    AHCI_DEVICE             Devices[AHCI_MAX_DEVICES];

    PUCHAR                  DmaArea;
    ULONG                   DmaLength;
    SCSI_PHYSICAL_ADDRESS   DmaAreaPhysical;

    BOOLEAN                 TrimAllowed;    /* TRIM=0 turns UNMAP off */
    BOOLEAN                 TrimMode;       /* TRIM_MODE_ON: pattern writes become TRIMs */
    UCHAR                   TrimPattern[AHCI_TRIM_PATTERN_SIZE];
} HW_DEVICE_EXTENSION, *PHW_DEVICE_EXTENSION;

/* One ATA/ATAPI command to put on the wire */
#define AHCI_REQ_DATA_NONE      0
#define AHCI_REQ_DATA_IN        1
#define AHCI_REQ_DATA_OUT       2

typedef struct _ATA_REQUEST {
    PVOID     DataBuffer;       /* ignored with UseBounce */
    ULONG     DataBufferLen;
    BOOLEAN   IsWrite;
    BOOLEAN   IsAtapi;
    BOOLEAN   UseBounce;        /* data in the port's BounceBuffer */
    BOOLEAN   Is48Bit;          /* ATA only */
    BOOLEAN   RawDevice;        /* Device register as given (pass-through) */
    UCHAR     Pmp;
    UCHAR     Command;          /* ATA only */
    UCHAR     Device;           /* with RawDevice */
    USHORT    Features;
    AHCI_LBA  Lba;              /* ATA only */
    ULONG     SectorCount;      /* ATA only, 1..65536 (also the raw count) */
    ULONG     TimeoutSec;       /* 0: from the SRB */
    PUCHAR    Cdb;              /* ATAPI only */
    ULONG     CdbLength;
} ATA_REQUEST, *PATA_REQUEST;

/* AhciRunCommand results */
#define AHCI_CMD_OK             0
#define AHCI_CMD_DEVICE_ERROR   1   /* PxTFD.ERR: device rejected/failed it */
#define AHCI_CMD_HBA_ERROR      2   /* fatal PxIS without ERR, or timeout */
#define AHCI_CMD_BAD_BUFFER     3   /* buffer could not be described */

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

#define AHCI_SLOT_TABLE(pi, slot)       ((pi)->CommandTables + (slot) * AHCI_CMDTBL_SIZE)
#define AHCI_DEVICE_OF(hw, port, lun)   (&(hw)->Devices[(hw)->Ports[port].DeviceIndex[lun]])

/* ------------------------------------------------------------------ */
/* Cross-file prototypes                                               */
/* ------------------------------------------------------------------ */

/* ahcimain.c */
BOOLEAN AhciReinitialize(IN PHW_DEVICE_EXTENSION HwInit);
VOID    AhciCheckState(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port);
VOID    AhciSetupPort(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port);
VOID    AhciArmTimer(IN PHW_DEVICE_EXTENSION HwInit);
BOOLEAN AhciPowerDown(IN PHW_DEVICE_EXTENSION HwInit);

/* ahcicmd.c */
VOID    AhciStopPortEngines(IN PHW_DEVICE_EXTENSION HwInit, IN PUCHAR PortBase);
BOOLEAN AhciRestartPort(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN BOOLEAN ForceComReset);
ULONG   AhciRunCommand(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN PSCSI_REQUEST_BLOCK Srb, IN PATA_REQUEST Req);
ULONG   AhciRunSimple(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN ULONG Lun,
                      IN UCHAR Command, IN USHORT Features, IN ULONG TimeoutSec);
BOOLEAN AhciIdentifyDevice(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN ULONG Lun);
VOID    AhciApplyDeviceSettings(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN ULONG Lun);
BOOLEAN AhciSoftReset(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN UCHAR Pmp, OUT PULONG Signature);
BOOLEAN AhciPmRead(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN UCHAR PmPort, IN ULONG Reg, OUT PULONG Value);
BOOLEAN AhciPmWrite(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN UCHAR PmPort, IN ULONG Reg, IN ULONG Value);
UCHAR   AhciIssueNcq(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN PSCSI_REQUEST_BLOCK Srb,
                     IN AHCI_LBA Lba, IN ULONG Count, IN BOOLEAN IsWrite, IN BOOLEAN Fua);
ULONG   AhciServicePort(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port);
VOID    AhciDrainPort(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port);
VOID    AhciFailOutstanding(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN UCHAR SrbStatus);
VOID    AhciReadResult(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN BOOLEAN PioSetup);

/* ahcisatl.c */
VOID    AhciParseIdentify(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN ULONG Lun);
VOID    AhciSatlProcessSrb(IN PHW_DEVICE_EXTENSION HwInit, IN PSCSI_REQUEST_BLOCK Srb);
VOID    AhciProcessIoControl(IN PHW_DEVICE_EXTENSION HwInit, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN AhciFlushDevice(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN ULONG Lun);
VOID    AhciShutdownDevice(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN ULONG Lun);
VOID    AhciSatlCompleteAtaError(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN PSCSI_REQUEST_BLOCK Srb);

#endif /* _AHCINT_H_ */
