/*
 * AHCINT - IRP_MJ_DEVICE_CONTROL front end for Windows 2000/XP
 *
 * Built into the 2000/XP driver only.
 */

#include <ntddk.h>
#include <srb.h>
#include <ntddscsi.h>

#define AHCI_PDO_POOL_TAG ((ULONG)'A' | ((ULONG)'H' << 8) | ((ULONG)'C' << 16) | ((ULONG)'I' << 24))

/* IOCTLs of the disk class driver that ScsiPort does not handle on a PDO */
#define AHCI_IOCTL_IDE_PASS_THROUGH      0x0004D028
#define AHCI_IOCTL_ATA_PASS_THROUGH      0x0004D02C

/* ATA_PASS_THROUGH_EX.AtaFlags */
#define AHCI_ATA_FLAGS_DATA_IN           0x0002
#define AHCI_ATA_FLAGS_DATA_OUT          0x0004
#define AHCI_ATA_FLAGS_48BIT_COMMAND     0x0008
#define AHCI_ATA_FLAGS_USE_DMA           0x0010

/* ATA_PASS_THROUGH_EX: DataBufferOffset, a ULONG_PTR (at 14h for a 32-bit
   caller, at 18h for a 64-bit one), the two task files after it */
#define AHCI_APT_OFFSET_FIELD(ptr)       ((ptr) == 8 ? 0x18 : 0x14)
#define AHCI_APT_HEADER(ptr)             (AHCI_APT_OFFSET_FIELD(ptr) + (ptr) + 16)
/* IOCTL_IDE_PASS_THROUGH: IDEREGS, ULONG DataBufferSize, data */
#define AHCI_IPT_HEADER                  12

#define AHCI_PT_MAX_DATA                 0x20000
#define AHCI_PT_SENSE                    32
#define AHCI_SCSIOP_ATA_PASSTHROUGH16    0x85

static PDRIVER_DISPATCH g_AhciOrigDeviceControl;

/* No CRT */
static VOID
AhciPdoZero(OUT PVOID Dst, IN ULONG Bytes)
{
    PUCHAR d = (PUCHAR)Dst;
    while (Bytes--) *d++ = 0;
}

static VOID
AhciPdoCopy(OUT PVOID Dst, IN CONST VOID *Src, IN ULONG Bytes)
{
    PUCHAR d = (PUCHAR)Dst;
    CONST UCHAR *s = (CONST UCHAR *)Src;
    while (Bytes--) *d++ = *s++;
}

static NTSTATUS NTAPI
AhciPdoIoctlDone(IN PDEVICE_OBJECT Device, IN PIRP Irp, IN PVOID Context)
{
    (VOID)Device;
    (VOID)Irp;
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* One SRB through the PDO, which puts the disk's address into it; the data
   (nonpaged pool) is mapped through an MDL, as ScsiPort maps the data of
   every request with DATA_IN or DATA_OUT through the IRP's MDL */
static NTSTATUS
AhciPdoSendSrb(IN PDEVICE_OBJECT Device, IN PSCSI_REQUEST_BLOCK Srb, IN PVOID Data, IN ULONG Length)
{
    PIO_STACK_LOCATION next;
    KEVENT event;
    NTSTATUS status;
    PMDL mdl = NULL;
    PIRP irp;

    irp = IoAllocateIrp(Device->StackSize, FALSE);
    if (irp == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    if (Length != 0) {
        mdl = IoAllocateMdl(Data, Length, FALSE, FALSE, irp);
        if (mdl == NULL) {
            IoFreeIrp(irp);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        MmBuildMdlForNonPagedPool(mdl);
    }
    Srb->Length = sizeof(SCSI_REQUEST_BLOCK);
    Srb->DataBuffer = Data;
    Srb->DataTransferLength = Length;
    Srb->OriginalRequest = irp;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    next = IoGetNextIrpStackLocation(irp);
    next->MajorFunction = IRP_MJ_SCSI;
    next->Parameters.Scsi.Srb = Srb;
    IoSetCompletionRoutine(irp, AhciPdoIoctlDone, &event, TRUE, TRUE, TRUE);
    if (IoCallDriver(Device, irp) == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
    }
    status = irp->IoStatus.Status;
    irp->MdlAddress = NULL;
    if (mdl != NULL) IoFreeMdl(mdl);
    IoFreeIrp(irp);
    return status;
}

static NTSTATUS
AhciPdoComplete(IN PIRP Irp, IN NTSTATUS Status, IN ULONG_PTR Information)
{
    Irp->IoStatus.Information = Information;
    Irp->IoStatus.Status = Status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

/* ------------------------------------------------------------------ */
/* IOCTL_SCSI_MINIPORT                                                 */
/* ------------------------------------------------------------------ */

/* disk.sys sends its SMART requests (SMART_GET_VERSION, SMART_RCV_DRIVE_DATA,
   SMART_SEND_DRIVE_COMMAND and its own failure prediction) as
   IOCTL_SCSI_MINIPORT to the disk's PDO. ScsiPort of Server 2003 / XP x64
   answers that IOCTL on a PDO with STATUS_INVALID_DEVICE_REQUEST (only the
   adapter's device object takes it). The front end turns it into the
   SRB_FUNCTION_IO_CONTROL request ScsiPort builds for the adapter, sent
   through the PDO, which puts the disk's address into it. Same on every
   2000/XP version: the miniport sees the request as before. */
static NTSTATUS
AhciPdoMiniportIoctl(IN PDEVICE_OBJECT Device, IN PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PSRB_IO_CONTROL ctl = (PSRB_IO_CONTROL)Irp->AssociatedIrp.SystemBuffer;
    ULONG inLen = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = stack->Parameters.DeviceIoControl.OutputBufferLength;
    PSCSI_REQUEST_BLOCK srb;
    ULONG length;
    NTSTATUS status;

    if (ctl == NULL || inLen < sizeof(SRB_IO_CONTROL)) {
        return AhciPdoComplete(Irp, STATUS_INVALID_PARAMETER, 0);
    }
    if (ctl->HeaderLength != sizeof(SRB_IO_CONTROL)) {
        return AhciPdoComplete(Irp, STATUS_REVISION_MISMATCH, 0);
    }
    length = ctl->HeaderLength + ctl->Length;
    if (length < ctl->Length) {
        return AhciPdoComplete(Irp, STATUS_INVALID_PARAMETER, 0);
    }
    if (inLen < length && outLen < length) {
        return AhciPdoComplete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
    }
    srb = (PSCSI_REQUEST_BLOCK)ExAllocatePoolWithTag(NonPagedPool, sizeof(SCSI_REQUEST_BLOCK), AHCI_PDO_POOL_TAG);
    if (srb == NULL) return AhciPdoComplete(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    AhciPdoZero(srb, sizeof(*srb));
    srb->Function = SRB_FUNCTION_IO_CONTROL;
    srb->SrbFlags = SRB_FLAGS_DATA_IN | SRB_FLAGS_DATA_OUT | SRB_FLAGS_NO_QUEUE_FREEZE;
    srb->TimeOutValue = ctl->Timeout;
    status = AhciPdoSendSrb(Device, srb, ctl, length);
    ExFreePool(srb);
    return AhciPdoComplete(Irp, status, length < outLen ? length : outLen);
}

/* ------------------------------------------------------------------ */
/* IOCTL_ATA_PASS_THROUGH and IOCTL_IDE_PASS_THROUGH                    */
/* ------------------------------------------------------------------ */

/* The disk class driver passes both on to the PDO unchanged; ScsiPort
   knows neither (Windows XP sends IOCTL_ATA_PASS_THROUGH to an ATA port
   driver only). The front end sends the command as SCSI ATA PASS-THROUGH
   (16) with CK_COND, which the miniport carries out as its SAT layer
   does, and returns the ATA registers of the result from the sense data
   (ATA Status Return descriptor, or the fixed format of SAT). An ATA error
   completes the IOCTL with success and ERR in the status register, as the
   ATA port driver does. IOCTL_ATA_PASS_THROUGH_DIRECT (a buffer of the
   caller, not copied) stays unsupported. */

/* Commands that write data (IOCTL_IDE_PASS_THROUGH has no direction) */
static BOOLEAN
AhciPdoIdeWrites(IN CONST UCHAR *Regs)
{
    static CONST UCHAR writes[] = {
        0x30, 0x31, 0x32, 0x33, 0x34, 0x39, 0x3A, 0x3B, 0x3D, 0x3E, 0x3F, 0x57,
        0x92, 0x93, 0xC5, 0xCA, 0xCB, 0xCC, 0xCE, 0xE8, 0xF1, 0xF2, 0xF4, 0xF6
    };
    ULONG i;

    if (Regs[6] == 0xB0) return (BOOLEAN)(Regs[0] == 0xD6);      /* SMART WRITE LOG */
    for (i = 0; i < sizeof(writes); i++) if (writes[i] == Regs[6]) return TRUE;
    return FALSE;
}

/* DMA and 48-bit commands among those IOCTL_IDE_PASS_THROUGH may carry */
static BOOLEAN
AhciPdoIdeDma(IN UCHAR Command)
{
    return (BOOLEAN)(Command == 0xC8 || Command == 0xCA || Command == 0x25 || Command == 0x35 ||
                     Command == 0x3D || Command == 0x47 || Command == 0x57 || Command == 0x92 ||
                     Command == 0x93 || Command == 0x06 || Command == 0x07);
}

static BOOLEAN
AhciPdoIde48(IN UCHAR Command)
{
    return (BOOLEAN)(Command == 0x24 || Command == 0x25 || Command == 0x27 || Command == 0x29 ||
                     Command == 0x2F || Command == 0x34 || Command == 0x35 || Command == 0x37 ||
                     Command == 0x39 || Command == 0x3D || Command == 0x3F || Command == 0x42 ||
                     Command == 0x47 || Command == 0x57);
}

/* The registers of the result from the sense data: Cur/Prev in task file
   order (features/error, count, LBA low, mid, high, device, command/status) */
static BOOLEAN
AhciPdoSenseRegs(IN CONST UCHAR *Sense, IN ULONG Length, OUT PUCHAR Cur, OUT PUCHAR Prev)
{
    ULONG pos, end;

    if (Length < 8) return FALSE;
    if ((Sense[0] & 0x7F) == 0x72 || (Sense[0] & 0x7F) == 0x73) {
        end = 8 + (ULONG)Sense[7];
        if (end > Length) end = Length;
        for (pos = 8; pos + 2 <= end; pos += 2 + (ULONG)Sense[pos + 1]) {
            if (Sense[pos] == 0x09 && Sense[pos + 1] >= 0x0C && pos + 14 <= end) {
                Cur[0] = Sense[pos + 3];
                Prev[1] = Sense[pos + 4];
                Cur[1] = Sense[pos + 5];
                Prev[2] = Sense[pos + 6];
                Cur[2] = Sense[pos + 7];
                Prev[3] = Sense[pos + 8];
                Cur[3] = Sense[pos + 9];
                Prev[4] = Sense[pos + 10];
                Cur[4] = Sense[pos + 11];
                Cur[5] = Sense[pos + 12];
                Cur[6] = Sense[pos + 13];
                return TRUE;
            }
        }
        return FALSE;
    }
    if ((Sense[0] & 0x7F) == 0x70 || (Sense[0] & 0x7F) == 0x71) {
        /* SAT fixed format: INFORMATION = error, status, device, count;
           COMMAND-SPECIFIC INFORMATION = flags, LBA low, mid, high */
        if (Length < 12 || Sense[12] != 0x00 || Sense[13] != 0x1D) return FALSE;
        Cur[0] = Sense[3];
        Cur[6] = Sense[4];
        Cur[5] = Sense[5];
        Cur[1] = Sense[6];
        Cur[2] = Sense[9];
        Cur[3] = Sense[10];
        Cur[4] = Sense[11];
        return TRUE;
    }
    return FALSE;
}

static NTSTATUS
AhciPdoAtaPassThrough(IN PDEVICE_OBJECT Device, IN PIRP Irp, IN BOOLEAN Ide)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    PUCHAR buf = (PUCHAR)Irp->AssociatedIrp.SystemBuffer;
    ULONG inLen = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = stack->Parameters.DeviceIoControl.OutputBufferLength;
    ULONG bufLen = inLen > outLen ? inLen : outLen;
    ULONG header, size, offset, timeout, ptr = sizeof(PVOID), done;
    USHORT flags;
    UCHAR cur[8], prev[8], res[8], resPrev[8];
    UCHAR protocol;
    BOOLEAN dataIn, dataOut, extend;
    PSCSI_REQUEST_BLOCK srb;
    PUCHAR sense, data = NULL;
    NTSTATUS status;
    UCHAR srbStatus;

    if (buf == NULL) return AhciPdoComplete(Irp, STATUS_INVALID_PARAMETER, 0);
#if defined(_WIN64)
    if (!Ide && IoIs32bitProcess(Irp)) ptr = 4;
#endif
    AhciPdoZero(prev, sizeof(prev));
    if (Ide) {
        header = AHCI_IPT_HEADER;
        if (inLen < header) return AhciPdoComplete(Irp, STATUS_INVALID_PARAMETER, 0);
        AhciPdoCopy(cur, buf, 8);
        size = *(ULONG UNALIGNED *)(buf + 8);
        offset = header;
        timeout = 10;
        dataOut = (BOOLEAN)(size != 0 && AhciPdoIdeWrites(cur));
        dataIn = (BOOLEAN)(size != 0 && !dataOut);
        extend = AhciPdoIde48(cur[6]);
        flags = (USHORT)(AhciPdoIdeDma(cur[6]) ? AHCI_ATA_FLAGS_USE_DMA : 0);
        if (cur[6] == 0xB0) {                   /* SMART: the signature, as SMART_SEND_DRIVE_COMMAND */
            cur[3] = 0x4F;
            cur[4] = 0xC2;
        }
    } else {
        header = AHCI_APT_HEADER(ptr);
        if (inLen < header || *(USHORT UNALIGNED *)buf != header) {
            return AhciPdoComplete(Irp, STATUS_REVISION_MISMATCH, 0);
        }
        flags = *(USHORT UNALIGNED *)(buf + 2);
        size = *(ULONG UNALIGNED *)(buf + 8);
        timeout = *(ULONG UNALIGNED *)(buf + 12);
        offset = (ptr == 8) ? (ULONG)*(ULONGLONG UNALIGNED *)(buf + AHCI_APT_OFFSET_FIELD(ptr))
                            : *(ULONG UNALIGNED *)(buf + AHCI_APT_OFFSET_FIELD(ptr));
        if (ptr == 8 && *(ULONGLONG UNALIGNED *)(buf + AHCI_APT_OFFSET_FIELD(ptr)) > 0xFFFFFFFF) {
            return AhciPdoComplete(Irp, STATUS_INVALID_PARAMETER, 0);
        }
        AhciPdoCopy(prev, buf + AHCI_APT_OFFSET_FIELD(ptr) + ptr, 8);
        AhciPdoCopy(cur, buf + AHCI_APT_OFFSET_FIELD(ptr) + ptr + 8, 8);
        dataIn = (BOOLEAN)((flags & AHCI_ATA_FLAGS_DATA_IN) != 0);
        dataOut = (BOOLEAN)((flags & AHCI_ATA_FLAGS_DATA_OUT) != 0);
        extend = (BOOLEAN)((flags & AHCI_ATA_FLAGS_48BIT_COMMAND) != 0);
        if (dataIn && dataOut) return AhciPdoComplete(Irp, STATUS_INVALID_PARAMETER, 0);
        if (!dataIn && !dataOut) size = 0;
    }
    if (size > AHCI_PT_MAX_DATA) return AhciPdoComplete(Irp, STATUS_INVALID_PARAMETER, 0);
    if (size != 0) {
        if (offset < header || offset > bufLen || size > bufLen - offset ||
            (dataIn && offset + size > outLen) || (dataOut && offset + size > inLen)) {
            return AhciPdoComplete(Irp, STATUS_INVALID_PARAMETER, 0);
        }
    } else {
        dataIn = dataOut = FALSE;
    }
    if (timeout == 0) timeout = 10;

    /* SRB, sense buffer and a copy of the data in one nonpaged block */
    srb = (PSCSI_REQUEST_BLOCK)ExAllocatePoolWithTag(NonPagedPool,
              sizeof(SCSI_REQUEST_BLOCK) + AHCI_PT_SENSE + size, AHCI_PDO_POOL_TAG);
    if (srb == NULL) return AhciPdoComplete(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    AhciPdoZero(srb, sizeof(SCSI_REQUEST_BLOCK) + AHCI_PT_SENSE);
    sense = (PUCHAR)(srb + 1);
    if (size != 0) {
        data = sense + AHCI_PT_SENSE;
        if (dataOut) AhciPdoCopy(data, buf + offset, size); else AhciPdoZero(data, size);
    }

    if (!dataIn && !dataOut) protocol = 3;                         /* non-data */
    else if (flags & AHCI_ATA_FLAGS_USE_DMA) protocol = 6;          /* DMA */
    else protocol = (UCHAR)(dataIn ? 4 : 5);                        /* PIO in / out */
    srb->Function = SRB_FUNCTION_EXECUTE_SCSI;
    srb->CdbLength = 16;
    srb->SrbFlags = SRB_FLAGS_NO_QUEUE_FREEZE | (dataIn ? SRB_FLAGS_DATA_IN : 0) | (dataOut ? SRB_FLAGS_DATA_OUT : 0);
    if (!dataIn && !dataOut) srb->SrbFlags |= SRB_FLAGS_NO_DATA_TRANSFER;
    srb->TimeOutValue = timeout;
    srb->SenseInfoBuffer = sense;
    srb->SenseInfoBufferLength = AHCI_PT_SENSE;
    srb->Cdb[0] = AHCI_SCSIOP_ATA_PASSTHROUGH16;
    srb->Cdb[1] = (UCHAR)((protocol << 1) | (extend ? 1 : 0));
    /* CK_COND; T_DIR, BYT_BLOK, T_LENGTH in the sector count */
    srb->Cdb[2] = (UCHAR)(0x20 | (size != 0 ? (0x04 | 0x02 | (dataIn ? 0x08 : 0)) : 0));
    srb->Cdb[3] = extend ? prev[0] : 0;
    srb->Cdb[4] = cur[0];
    srb->Cdb[5] = extend ? prev[1] : 0;
    srb->Cdb[6] = cur[1];
    srb->Cdb[7] = extend ? prev[2] : 0;
    srb->Cdb[8] = cur[2];
    srb->Cdb[9] = extend ? prev[3] : 0;
    srb->Cdb[10] = cur[3];
    srb->Cdb[11] = extend ? prev[4] : 0;
    srb->Cdb[12] = cur[4];
    srb->Cdb[13] = cur[5];
    srb->Cdb[14] = cur[6];

    status = AhciPdoSendSrb(Device, srb, data, size);
    srbStatus = SRB_STATUS(srb->SrbStatus);
    AhciPdoZero(res, sizeof(res));
    AhciPdoZero(resPrev, sizeof(resPrev));
    if ((srb->SrbStatus & SRB_STATUS_AUTOSENSE_VALID) &&
        AhciPdoSenseRegs(sense, AHCI_PT_SENSE, res, resPrev)) {
        status = STATUS_SUCCESS;
    } else if (srbStatus == SRB_STATUS_SUCCESS || srbStatus == SRB_STATUS_DATA_OVERRUN) {
        res[6] = 0x50;                          /* DRDY DSC: done, no registers returned */
        status = STATUS_SUCCESS;
    } else if (NT_SUCCESS(status)) {
        status = STATUS_IO_DEVICE_ERROR;
    }
    if (!NT_SUCCESS(status)) {
        ExFreePool(srb);
        return AhciPdoComplete(Irp, status, 0);
    }

    done = (dataIn && srb->DataTransferLength < size) ? srb->DataTransferLength : size;
    if (dataIn) AhciPdoCopy(buf + offset, data, done);
    ExFreePool(srb);
    res[7] = 0;
    if (Ide) {
        AhciPdoCopy(buf, res, 8);
        return AhciPdoComplete(Irp, STATUS_SUCCESS, dataIn ? header + done : header);
    }
    *(ULONG UNALIGNED *)(buf + 8) = done;
    if (extend) AhciPdoCopy(buf + AHCI_APT_OFFSET_FIELD(ptr) + ptr, resPrev, 8);
    AhciPdoCopy(buf + AHCI_APT_OFFSET_FIELD(ptr) + ptr + 8, res, 8);
    return AhciPdoComplete(Irp, STATUS_SUCCESS, dataIn ? offset + done : (outLen < header ? outLen : header));
}

/* ScsiPort creates the adapter's device object as FILE_DEVICE_CONTROLLER,
   its PDOs with the type of the device (disk, CD-ROM, mass storage) */
static NTSTATUS NTAPI
AhciDeviceControlDispatch(IN PDEVICE_OBJECT Device, IN PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;

    if (Device->DeviceType != FILE_DEVICE_CONTROLLER && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        if (code == IOCTL_SCSI_MINIPORT) return AhciPdoMiniportIoctl(Device, Irp);
        if (code == AHCI_IOCTL_ATA_PASS_THROUGH) return AhciPdoAtaPassThrough(Device, Irp, FALSE);
        if (code == AHCI_IOCTL_IDE_PASS_THROUGH) return AhciPdoAtaPassThrough(Device, Irp, TRUE);
    }
    return g_AhciOrigDeviceControl(Device, Irp);
}

/* After ScsiPortInitialize: ScsiPort's IRP_MJ_DEVICE_CONTROL entry gets a
   front end */
VOID
AhciHookDeviceControl(IN PVOID DriverObject)
{
    PDRIVER_OBJECT driver = (PDRIVER_OBJECT)DriverObject;

    if (driver == NULL || driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] == NULL ||
        driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] == AhciDeviceControlDispatch) {
        return;
    }
    g_AhciOrigDeviceControl = driver->MajorFunction[IRP_MJ_DEVICE_CONTROL];
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = AhciDeviceControlDispatch;
}
