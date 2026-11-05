/*
 * AHCINT - IRP_MJ_DEVICE_CONTROL front end for Windows 2000/XP
 *
 * Built into the 2000/XP driver only.
 */

#include <ntddk.h>
#include <srb.h>
#include <ntddscsi.h>

#define AHCI_PDO_POOL_TAG ((ULONG)'A' | ((ULONG)'H' << 8) | ((ULONG)'C' << 16) | ((ULONG)'I' << 24))

/* disk.sys sends its SMART requests (SMART_GET_VERSION, SMART_RCV_DRIVE_DATA,
   SMART_SEND_DRIVE_COMMAND and its own failure prediction) as
   IOCTL_SCSI_MINIPORT to the disk's PDO. ScsiPort of Server 2003 / XP x64
   answers that IOCTL on a PDO with STATUS_INVALID_DEVICE_REQUEST (only the
   adapter's device object takes it). The front end turns it into the
   SRB_FUNCTION_IO_CONTROL request ScsiPort builds for the adapter, sent
   through the PDO, which puts the disk's address into it. Same on every
   2000/XP version: the miniport sees the request as before. */
static PDRIVER_DISPATCH g_AhciOrigDeviceControl;

/* No CRT */
static VOID
AhciPdoZero(OUT PVOID Dst, IN ULONG Bytes)
{
    PUCHAR d = (PUCHAR)Dst;
    while (Bytes--) *d++ = 0;
}

static NTSTATUS NTAPI
AhciPdoIoctlDone(IN PDEVICE_OBJECT Device, IN PIRP Irp, IN PVOID Context)
{
    (VOID)Device;
    (VOID)Irp;
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS
AhciPdoMiniportIoctl(IN PDEVICE_OBJECT Device, IN PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp), next;
    PSRB_IO_CONTROL ctl = (PSRB_IO_CONTROL)Irp->AssociatedIrp.SystemBuffer;
    ULONG inLen = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = stack->Parameters.DeviceIoControl.OutputBufferLength;
    PSCSI_REQUEST_BLOCK srb;
    ULONG length;
    KEVENT event;
    PMDL mdl;
    NTSTATUS status;
    PIRP irp;

    if (ctl == NULL || inLen < sizeof(SRB_IO_CONTROL)) {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }
    if (ctl->HeaderLength != sizeof(SRB_IO_CONTROL)) {
        status = STATUS_REVISION_MISMATCH;
        goto done;
    }
    length = ctl->HeaderLength + ctl->Length;
    if (length < ctl->Length) {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }
    if (inLen < length && outLen < length) {
        status = STATUS_BUFFER_TOO_SMALL;
        goto done;
    }
    srb = (PSCSI_REQUEST_BLOCK)ExAllocatePoolWithTag(NonPagedPool, sizeof(SCSI_REQUEST_BLOCK), AHCI_PDO_POOL_TAG);
    irp = IoAllocateIrp(Device->StackSize, FALSE);
    /* ScsiPort maps the data of a request through the IRP's MDL: one for
       the buffered IOCTL's system buffer (nonpaged pool) */
    mdl = (irp != NULL) ? IoAllocateMdl(ctl, length, FALSE, FALSE, irp) : NULL;
    if (srb == NULL || irp == NULL || mdl == NULL) {
        if (srb != NULL) ExFreePool(srb);
        if (mdl != NULL) IoFreeMdl(mdl);
        if (irp != NULL) { irp->MdlAddress = NULL; IoFreeIrp(irp); }
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }
    MmBuildMdlForNonPagedPool(mdl);
    AhciPdoZero(srb, sizeof(*srb));
    srb->Length = sizeof(SCSI_REQUEST_BLOCK);
    srb->Function = SRB_FUNCTION_IO_CONTROL;
    srb->SrbFlags = SRB_FLAGS_DATA_IN | SRB_FLAGS_DATA_OUT | SRB_FLAGS_NO_QUEUE_FREEZE;
    srb->DataBuffer = ctl;
    srb->DataTransferLength = length;
    srb->TimeOutValue = ctl->Timeout;
    srb->OriginalRequest = irp;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    next = IoGetNextIrpStackLocation(irp);
    next->MajorFunction = IRP_MJ_SCSI;
    next->Parameters.Scsi.Srb = srb;
    IoSetCompletionRoutine(irp, AhciPdoIoctlDone, &event, TRUE, TRUE, TRUE);
    if (IoCallDriver(Device, irp) == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
    }
    status = irp->IoStatus.Status;
    irp->MdlAddress = NULL;
    IoFreeMdl(mdl);
    IoFreeIrp(irp);
    ExFreePool(srb);
    Irp->IoStatus.Information = length < outLen ? length : outLen;
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;

done:
    Irp->IoStatus.Information = 0;
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

/* ScsiPort creates the adapter's device object as FILE_DEVICE_CONTROLLER,
   its PDOs with the type of the device (disk, CD-ROM, mass storage) */
static NTSTATUS NTAPI
AhciDeviceControlDispatch(IN PDEVICE_OBJECT Device, IN PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);

    if (Device->DeviceType != FILE_DEVICE_CONTROLLER &&
        stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_SCSI_MINIPORT &&
        KeGetCurrentIrql() == PASSIVE_LEVEL) {
        return AhciPdoMiniportIoctl(Device, Irp);
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
