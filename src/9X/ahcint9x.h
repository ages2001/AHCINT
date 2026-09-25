/* ahcint9x - Windows 95 SCSI miniport target */

#ifndef _ahcint9x_H_
#define _ahcint9x_H_

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
#define AHCI_DBG_MSG(msg) DbgPrint("[ahcint9x] " msg "\n")
#define AHCI_DBG_LOG      DbgPrint

/*
 * Raw COM1 (16550 UART) debug checkpoint logger.
 *
 * DbgPrint (above) needs a kernel debugger attached over serial to show
 * anything on real/VirtualBox Windows 95 -- with no debugger attached
 * it goes nowhere, which is why every fix so far has effectively been
 * a blind guess: there has been no way to see where the driver
 * actually gets to before the hang. This writes single bytes directly
 * to the COM1 UART's transmit register (I/O port 0x3F8) using plain
 * "out dx,al" -- no VxD call, no DDK function, nothing that depends on
 * an interrupt controller, an OS subsystem, or anything else that
 * could itself be broken. It works as long as COM1 exists as a real
 * (possibly emulated) 16550-compatible UART, which VirtualBox provides
 * once its "Serial Ports" setting for COM1 is enabled and redirected
 * to a host file (VBoxManage modifyvm <vm> --uart1 on 0x3F8 4
 * --uartmode1 file <path-on-host>, VM powered off first). Every
 * AHCI_TRACE() call below appends one line to that host file, live, as
 * the driver runs -- including the moment right before whatever hangs.
 */
#define COM1_PORT       0x3F8
#define COM1_LSR        (COM1_PORT + 5)   /* Line Status Register */
#define COM1_LSR_THRE   0x20              /* Transmit Holding Register Empty */

static __inline UCHAR AhciInPortB(USHORT port) {
    UCHAR value;
    __asm {
        mov dx, port
        in  al, dx
        mov value, al
    }
    return value;
}

static __inline void AhciOutPortB(USHORT port, UCHAR value) {
    __asm {
        mov dx, port
        mov al, value
        out dx, al
    }
}

static __inline void AhciTraceChar(UCHAR ch) {
    ULONG spin;
    /* Bounded wait for THRE so this can never itself hang if the UART
       is missing/unresponsive -- worst case this becomes a harmless
       no-op, it never blocks driver execution. */
    spin = 100000;
    while (!(AhciInPortB(COM1_LSR) & COM1_LSR_THRE) && --spin) { }
    AhciOutPortB(COM1_PORT, ch);
}

static __inline void AhciTrace(PCH msg) {
    while (*msg) {
        if (*msg == '\n') AhciTraceChar('\r');
        AhciTraceChar((UCHAR)*msg);
        msg++;
    }
}

#define AHCI_TRACE(msg) AhciTrace("[ahcint9x] " msg "\n")

/* Win95 KB Q169584 workaround: _MapPhysToLinear VxD service call.
   See ahcimain.c for the implementation and full explanation. */
#define MPL_NonCached                0x00000000
#define MPL_HardwareCoherentCached   0x00000001
#define MPL_FrameBufferCached        0x00000002
#define MPL_Cached                   0x00000004

PVOID __cdecl _MapPhysToLinear(ULONG PhysAddr, ULONG nBytes, ULONG flags);

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

/* Real ATAPI MODE SENSE(10) parameter header, confirmed against
   Microsoft's own real, shipped NT4 ATAPI miniport source
   (private/ntos/miniport/atapi/atapi.c / atapi.h) -- NOT present in this
   DDK's own SCSI.H, which only has the 4-byte SCSI-6 MODE_PARAMETER_HEADER
   (ModeDataLength, MediumType, DeviceSpecificParameter,
   BlockDescriptorLength). All-UCHAR, so no packing pragma is needed:
   natural alignment already matches the on-the-wire byte layout. Used by
   ahcisatl.c's AhciSatlProcessSrb to convert a real ATAPI drive's MODE
   SENSE(10) response back into the SCSI-6 MODE_PARAMETER_HEADER format
   ScsiPort/the CD-ROM class driver above us actually expects, the same
   way Microsoft's own driver's reverse-conversion code does. */
typedef struct _MODE_PARAMETER_HEADER_10 {
    UCHAR ModeDataLengthMsb;
    UCHAR ModeDataLengthLsb;
    UCHAR MediumType;
    UCHAR Reserved[5];
} MODE_PARAMETER_HEADER_10, *PMODE_PARAMETER_HEADER_10;

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

    /* Real ATA/ATAPI Error register (Task File Data, bits 8-15) latched
       from the last failing command on this port, plus whether it's
       actually new/unreported yet. This is what real REQUEST_SENSE
       support is built on: the AHCI TFD register's error byte IS the
       same real IDE_ERROR_* byte a native IDE controller would expose
       (MEDIA_CHANGE, MEDIA_CHANGE_REQUESTED, END_OF_MEDIA, ABRT, etc,
       per the ATA/ATAPI-4+ spec), we were just reading it and throwing
       it away. Latching it here lets AhciSatlProcessSrb's REQUEST_SENSE
       handler and its auto-sense-on-error path turn it into a real
       SCSI SENSE_DATA response instead of never reporting media changes
       to the CD-ROM class driver at all. */
    UCHAR                   LastAtaError;
    BOOLEAN                 LastErrorPending;
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