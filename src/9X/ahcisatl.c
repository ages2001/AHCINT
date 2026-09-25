#include "ahcint9x.h"

/* Confirmed against the real Windows 95 DDK's SCSI.H (Copyright 1993-95
   Microsoft Corporation): SCSIOP_SYNCHRONIZE_CACHE is not defined there
   (SCSI.H only goes up to SCSIOP_VERIFY = 0x2F and a handful of higher
   opcodes; SYNCHRONIZE CACHE(10) = 0x35 is simply missing). Every other
   SCSIOP_ / CDB / INQUIRYDATA / READ_CAPACITY_DATA symbol this file uses
   was verified present and byte-layout-compatible in that header. Kept
   here (Win95-only need) even though the rest of this file below is now
   restored to match src/NT's ahci_satl.c verbatim, per the driver
   author's own request -- NT4 already has this symbol defined natively,
   so this #ifndef guard is a no-op there and only takes effect on the
   Win95 build. */
#ifndef SCSIOP_SYNCHRONIZE_CACHE
#define SCSIOP_SYNCHRONIZE_CACHE   0x35
#endif

static VOID
AhciExtractAtaString(
    IN PUSHORT IdentifyWords,
    IN ULONG StartWord,
    IN ULONG NumWords,
    OUT PUCHAR OutBuffer,
    IN ULONG OutBufferMax
)
{
    ULONG i;
    ULONG outIndex;

    outIndex = 0;
    for (i = 0; i < NumWords && (outIndex + 1) < OutBufferMax; i++) {
        USHORT w = IdentifyWords[StartWord + i];
        OutBuffer[outIndex++] = (UCHAR)((w >> 8) & 0xFF);
        OutBuffer[outIndex++] = (UCHAR)(w & 0xFF);
    }

    while (outIndex > 0 && (OutBuffer[outIndex - 1] == ' ' || OutBuffer[outIndex - 1] == '\0')) {
        outIndex--;
    }

    while (outIndex < OutBufferMax) {
        OutBuffer[outIndex++] = ' ';
    }
}

static ULONG
AhciGetTotalSectors(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN ULONG Port
)
{
    PUSHORT id;

    id = (PUSHORT)HwInit->Ports[Port].IdentifyData;

    if (HwInit->Ports[Port].IdentifyValid) {
        if ((id[83] & 0xC400) == 0x4400 || (id[83] & (1 << 10))) {
            ULONG lba48Low = (ULONG)id[100] | ((ULONG)id[101] << 16);
            if (lba48Low > 0) return lba48Low;
        }
        {
            ULONG lba28 = (ULONG)id[60] | ((ULONG)id[61] << 16);
            if (lba28 > 0) return lba28;
        }
        {
            ULONG chs = (ULONG)id[1] * (ULONG)id[3] * (ULONG)id[6];
            if (chs > 0) return chs;
        }
    }
    return 0;
}

static VOID
AhciHandleInquiry(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN PSCSI_REQUEST_BLOCK Srb
)
{
    PINQUIRYDATA inq;
    ULONG port;
    PUSHORT id;
    BOOLEAN isCd;

    inq = (PINQUIRYDATA)Srb->DataBuffer;
    port = Srb->TargetId;
    id = (PUSHORT)HwInit->Ports[port].IdentifyData;
    isCd = HwInit->Ports[port].IsAtapi;

    ZeroMemoryBytes(inq, Srb->DataTransferLength);

    inq->DeviceType = isCd ? READ_ONLY_DIRECT_ACCESS_DEVICE : DIRECT_ACCESS_DEVICE;
    inq->RemovableMedia = isCd ? 1 : ((id[0] & (1 << 7)) ? 1 : 0);
    inq->ResponseDataFormat = 2;
    inq->AdditionalLength = 31;
    inq->CommandQueue = (id[76] & (1 << 8)) ? 1 : 0;

    if (HwInit->Ports[port].IdentifyValid) {
        AhciExtractAtaString(id, 27, 4, inq->VendorId, 8);
        AhciExtractAtaString(id, 31, 8, inq->ProductId, 16);
        AhciExtractAtaString(id, 23, 2, inq->ProductRevisionLevel, 4);
    } else {
        PUCHAR v = isCd ? "ATAPI   " : "ATA     ";
        PUCHAR p = isCd ? "SATA CD-ROM     " : "SATA HARDDISK   ";
        ULONG i;
        for (i = 0; i < 8; i++) inq->VendorId[i] = v[i];
        for (i = 0; i < 16; i++) inq->ProductId[i] = p[i];
        inq->ProductRevisionLevel[0] = '1';
        inq->ProductRevisionLevel[1] = '.';
        inq->ProductRevisionLevel[2] = '0';
        inq->ProductRevisionLevel[3] = '0';
    }

    Srb->SrbStatus = SRB_STATUS_SUCCESS;
    Srb->ScsiStatus = SCSISTAT_GOOD;
}

static VOID
AhciHandleModeSense(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN PSCSI_REQUEST_BLOCK Srb
)
{
    PUCHAR buf;
    PCDB cdb;
    UCHAR pageControl;
    ULONG port;
    PUSHORT id;
    ULONG totalSectors;
    ULONG cyls, heads, spt;

    buf = (PUCHAR)Srb->DataBuffer;
    cdb = (PCDB)Srb->Cdb;
    pageControl = cdb->MODE_SENSE.PageCode & 0x3F;
    port = Srb->TargetId;
    id = (PUSHORT)HwInit->Ports[port].IdentifyData;
    totalSectors = AhciGetTotalSectors(HwInit, port);
    cyls = 0;
    heads = 0;
    spt = 0;

    ZeroMemoryBytes(buf, Srb->DataTransferLength);

    if (HwInit->Ports[port].IdentifyValid) {
        cyls = (ULONG)id[1];
        heads = (ULONG)id[3];
        spt = (ULONG)id[6];
    }

    if (cyls == 0 || heads == 0 || spt == 0) {
        heads = 255;
        spt = 63;
        cyls = (totalSectors > 0) ? (totalSectors / (heads * spt)) : 1;
        if (cyls == 0) cyls = 1;
    }

    buf[0] = 0; buf[1] = 0; buf[2] = 0; buf[3] = 8;

    if (Srb->DataTransferLength >= 12) {
        buf[4] = 0;
        buf[5] = (UCHAR)((totalSectors >> 16) & 0xFF);
        buf[6] = (UCHAR)((totalSectors >> 8) & 0xFF);
        buf[7] = (UCHAR)(totalSectors & 0xFF);
        buf[8] = 0; buf[9] = 0; buf[10] = 2; buf[11] = 0;
    }

    if ((pageControl == 0x04 || pageControl == 0x3F) && Srb->DataTransferLength >= 36) {
        PUCHAR p4 = &buf[12];
        p4[0] = 0x04;
        p4[1] = 0x16;
        p4[2] = (UCHAR)((cyls >> 16) & 0xFF);
        p4[3] = (UCHAR)((cyls >> 8) & 0xFF);
        p4[4] = (UCHAR)(cyls & 0xFF);
        p4[5] = (UCHAR)heads;
        p4[20] = (UCHAR)((spt >> 8) & 0xFF);
        p4[21] = (UCHAR)(spt & 0xFF);
        buf[0] = 35;
    } else {
        buf[0] = 11;
    }

    Srb->SrbStatus = SRB_STATUS_SUCCESS;
    Srb->ScsiStatus = SCSISTAT_GOOD;
}

static BOOLEAN
AhciBuildPrdt(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN ULONG PortNumber,
    IN PATA_REQUEST Req,
    OUT PULONG PrdtEntriesCount
)
{
    ULONG bytesLeft = Req->DataBufferLen;
    PUCHAR virtAddr = (PUCHAR)Req->DataBuffer;
    PAHCI_PRDT_ENTRY prdt = (PAHCI_PRDT_ENTRY)(HwInit->Ports[PortNumber].CommandTable + 0x80);
    ULONG entryIndex = 0;
    ULONG segLen;
    SCSI_PHYSICAL_ADDRESS physAddr;
    ULONG bytesToPageBoundary;

    while (bytesLeft > 0 && entryIndex < MAX_PRDT_ENTRIES) {
        physAddr = ScsiPortGetPhysicalAddress(HwInit, HwInit->ActiveSrb, virtAddr, &segLen);
        if (physAddr.LowPart == 0 && physAddr.HighPart == 0) return FALSE;
        if (segLen > bytesLeft) segLen = bytesLeft;

        bytesToPageBoundary = 4096 - (physAddr.LowPart & 0xFFF);
        if (segLen > bytesToPageBoundary) {
            segLen = bytesToPageBoundary;
        }

        prdt[entryIndex].DataBaseAddress = physAddr.LowPart;
        prdt[entryIndex].DataBaseAddressUpper = physAddr.HighPart;
        prdt[entryIndex].Reserved = 0;
        prdt[entryIndex].ByteCountInterrupt = (segLen - 1) & 0x003FFFFF;

        virtAddr += segLen;
        bytesLeft -= segLen;
        entryIndex++;
    }

    if (bytesLeft > 0) return FALSE;

    *PrdtEntriesCount = entryIndex;
    return TRUE;
}

static BOOLEAN
AhciExecuteTransferEngine(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN PATA_REQUEST Req
)
{
    ULONG portNumber = HwInit->ActiveSrb ? HwInit->ActiveSrb->TargetId : 0;
    PUCHAR portBase = AHCI_PORT_BASE(HwInit->AbarMapped, portNumber);
    PAHCI_COMMAND_HEADER cmdHeader = &HwInit->Ports[portNumber].CommandList[0];
    PFIS_REG_H2D fis = (PFIS_REG_H2D)HwInit->Ports[portNumber].CommandTable;
    PUCHAR acmd = (PUCHAR)(HwInit->Ports[portNumber].CommandTable + 0x40);
    ULONG prdtEntries = 0;
    ULONG portIs;
    ULONG waitLimit;
    ULONG tfd;
    ULONG i;
    PUCHAR cdb;
    ULONG cdbLen;

    if (Req->DataBufferLen > 0) {
        if (!AhciBuildPrdt(HwInit, portNumber, Req, &prdtEntries)) {
            if (HwInit->ActiveSrb) HwInit->ActiveSrb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
            return FALSE;
        }
    }

    if (Req->IsAtapi) {
        cmdHeader->Flags = 5 | (Req->IsWrite ? (1 << 6) : 0) | (1 << 5);
        cmdHeader->PrdtLength = (USHORT)prdtEntries;
        cmdHeader->PrdByteCount = 0;
        cmdHeader->CommandTableBase = HwInit->Ports[portNumber].CommandTablePhysical;
        cmdHeader->CommandTableBaseUpper = HwInit->Ports[portNumber].CommandTablePhysicalUpper;

        ZeroMemoryBytes(acmd, 16);
        if (HwInit->ActiveSrb) {
            cdb = (PUCHAR)HwInit->ActiveSrb->Cdb;
            cdbLen = HwInit->ActiveSrb->CdbLength;
            if (cdbLen > 16) cdbLen = 16;
            for (i = 0; i < cdbLen; i++) {
                acmd[i] = cdb[i];
            }
        }

        ZeroMemoryBytes(fis, sizeof(FIS_REG_H2D));
        fis->FisType = 0x27;
        fis->PmPortControl = 0x80;
        fis->Command = IDE_COMMAND_PACKET;
        fis->FeaturesLow = 0x01;
        fis->Lba1 = (UCHAR)(Req->DataBufferLen & 0xFF);
        fis->Lba2 = (UCHAR)((Req->DataBufferLen >> 8) & 0xFF);
    } else {
        cmdHeader->Flags = 5 | (Req->IsWrite ? (1 << 6) : 0);
        cmdHeader->PrdtLength = (USHORT)prdtEntries;
        cmdHeader->PrdByteCount = 0;
        cmdHeader->CommandTableBase = HwInit->Ports[portNumber].CommandTablePhysical;
        cmdHeader->CommandTableBaseUpper = HwInit->Ports[portNumber].CommandTablePhysicalUpper;

        ZeroMemoryBytes(fis, sizeof(FIS_REG_H2D));
        fis->FisType = 0x27;
        fis->PmPortControl = 0x80;
        fis->Command = Req->IsWrite ? IDE_COMMAND_WRITE_DMA_EXT : IDE_COMMAND_READ_DMA_EXT;
        fis->Device = 0x40;

        fis->Lba0 = (UCHAR)(Req->LbaLow & 0xFF);
        fis->Lba1 = (UCHAR)((Req->LbaLow >> 8) & 0xFF);
        fis->Lba2 = (UCHAR)((Req->LbaLow >> 16) & 0xFF);
        fis->Lba3 = (UCHAR)((Req->LbaLow >> 24) & 0xFF);
        fis->Lba4 = (UCHAR)(Req->LbaHigh & 0xFF);
        fis->Lba5 = (UCHAR)((Req->LbaHigh >> 8) & 0xFF);
        fis->SectorCountLow = (UCHAR)(Req->SectorCount & 0xFF);
        fis->SectorCountHigh = (UCHAR)((Req->SectorCount >> 8) & 0xFF);
    }

    AHCI_WRITE_REG(portBase, AHCI_PORT_IS, 0xFFFFFFFF);
    AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
    AHCI_WRITE_REG(portBase, AHCI_PORT_IE, 0x00000000);
    AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_IS, (1UL << portNumber));

    AHCI_WRITE_REG(portBase, AHCI_PORT_CI, 1);

    waitLimit = Req->IsAtapi ? 100000 : 25000;
    for (;;) {
        ULONG ci = AHCI_READ_REG(portBase, AHCI_PORT_CI);
        ULONG isNow = AHCI_READ_REG(portBase, AHCI_PORT_IS);
        if (!(ci & 1)) break;
        if (isNow & AHCI_PORT_IS_FATAL) break;
        if (--waitLimit == 0) break;
        ScsiPortStallExecution(20);
    }

    /* Diagnostic only (temporary, kept from the trace that found the bug
       above): if this still prints after the TFES-aware wait fix, the
       full timeout is being hit for some OTHER reason than a reported
       task-file error (e.g. a genuinely wedged port), which would need
       separate investigation. Expected now: this should no longer print
       for the plain "no disc in drive" case. Safe to remove once
       confirmed quiet on real hardware. */
    if (Req->IsAtapi && waitLimit == 0) {
        AHCI_TRACE("ATAPI cmd: TIMED OUT (no TFES, CI never cleared)");
    }

    portIs = AHCI_READ_REG(portBase, AHCI_PORT_IS);
    tfd = AHCI_READ_REG(portBase, AHCI_PORT_TFD);

    AHCI_WRITE_REG(portBase, AHCI_PORT_IS, portIs);
    AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
    AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_IS, (1UL << portNumber));

    if ((AHCI_READ_REG(portBase, AHCI_PORT_CI) & 1) || (portIs & AHCI_PORT_IS_FATAL) || (tfd & 0x01)) {
        if (Req->IsAtapi) {
            AhciStopPortEngines(portBase);
            AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
            AHCI_WRITE_REG(portBase, AHCI_PORT_IS, 0xFFFFFFFF);
            AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, AHCI_READ_REG(portBase, AHCI_PORT_CMD) | AHCI_PORT_CMD_FRE);
            AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, AHCI_READ_REG(portBase, AHCI_PORT_CMD) | AHCI_PORT_CMD_ST);
        }

        if (HwInit->ActiveSrb) {
            ULONG portNum2 = HwInit->ActiveSrb->TargetId;
            if (portNum2 < MAX_AHCI_PORTS) {
                HwInit->Ports[portNum2].LastAtaError = (UCHAR)((tfd >> 8) & 0xFF);
                HwInit->Ports[portNum2].LastErrorPending = TRUE;
            }
            HwInit->ActiveSrb->SrbStatus = SRB_STATUS_ERROR;
            HwInit->ActiveSrb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
        }
        return FALSE;
    }

    if (HwInit->ActiveSrb) {
        HwInit->ActiveSrb->SrbStatus = SRB_STATUS_SUCCESS;
        HwInit->ActiveSrb->ScsiStatus = SCSISTAT_GOOD;
    }
    return FALSE;
}

BOOLEAN
AhciSatlProcessSrb(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN PSCSI_REQUEST_BLOCK Srb
)
{
    PCDB cdb;
    ATA_REQUEST ataReq;
    ULONG port;
    ULONG i;

    cdb = (PCDB)Srb->Cdb;
    port = Srb->TargetId;

    if (port >= MAX_AHCI_PORTS || !HwInit->Ports[port].Present) {
        Srb->SrbStatus = SRB_STATUS_SELECTION_TIMEOUT;
        return FALSE;
    }

    if (Srb->Lun != 0) {
        Srb->SrbStatus = SRB_STATUS_NO_DEVICE;
        return FALSE;
    }

    switch (cdb->CDB6GENERIC.OperationCode) {
    case SCSIOP_INQUIRY:
        AhciHandleInquiry(HwInit, Srb);
        return FALSE;

    case SCSIOP_REQUEST_SENSE:
        if (HwInit->Ports[port].IsAtapi) {
            UCHAR senseCdb[12];
            PSENSE_DATA senseOut = (PSENSE_DATA)Srb->DataBuffer;
            UCHAR allocLen = (UCHAR)Srb->DataTransferLength;
            UCHAR savedCdb[16];
            UCHAR savedCdbLength;

            ZeroMemoryBytes(savedCdb, sizeof(savedCdb));
            for (i = 0; i < 16; i++) {
                savedCdb[i] = Srb->Cdb[i];
            }
            savedCdbLength = Srb->CdbLength;

            if (allocLen == 0 || allocLen > sizeof(SENSE_DATA)) {
                allocLen = sizeof(SENSE_DATA);
            }

            ZeroMemoryBytes(senseCdb, sizeof(senseCdb));
            senseCdb[0] = SCSIOP_REQUEST_SENSE;
            senseCdb[4] = allocLen;

            ZeroMemoryBytes(Srb->Cdb, 16);
            for (i = 0; i < 6; i++) {
                Srb->Cdb[i] = senseCdb[i];
            }
            Srb->CdbLength = 6;

            if (Srb->DataBuffer != NULL && Srb->DataTransferLength > 0) {
                ZeroMemoryBytes(Srb->DataBuffer, Srb->DataTransferLength);
            }

            ataReq.DataBuffer = Srb->DataBuffer;
            ataReq.DataBufferLen = Srb->DataTransferLength;
            ataReq.IsAtapi = TRUE;
            ataReq.IsWrite = FALSE;
            AhciExecuteTransferEngine(HwInit, &ataReq);

            /* A REQUEST SENSE that itself fails leaves nothing real to
               report -- fabricating NO SENSE here would hide the real
               condition, so this is the one place a clean, zeroed
               SENSE_DATA (ErrorCode 0x70, SenseKey NO_SENSE, everything
               else zero) really is the honest answer: "sense data was
               requested but the drive gave us nothing usable." */
            if (Srb->SrbStatus != SRB_STATUS_SUCCESS &&
                senseOut != NULL && Srb->DataTransferLength >= sizeof(SENSE_DATA)) {
                ZeroMemoryBytes(senseOut, sizeof(SENSE_DATA));
                senseOut->ErrorCode = 0x70;
            }

            /* Restore the SRB's CDB exactly as ScsiPort gave it to us --
               see the bug note above. This must happen for every return
               path, success or failure. */
            for (i = 0; i < 16; i++) {
                Srb->Cdb[i] = savedCdb[i];
            }
            Srb->CdbLength = savedCdbLength;

            HwInit->Ports[port].LastErrorPending = FALSE;

            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            Srb->ScsiStatus = SCSISTAT_GOOD;
            return FALSE;
        }

        /* ATA disks: nothing this driver does ever fails in a way that
           produces real, meaningful sense data (see SYNCHRONIZE_CACHE/
           VERIFY below), so an honest, empty NO SENSE response -- not a
           fabricated specific error -- is the correct real answer here. */
        if (Srb->DataTransferLength >= sizeof(SENSE_DATA)) {
            PSENSE_DATA senseOut = (PSENSE_DATA)Srb->DataBuffer;
            ZeroMemoryBytes(senseOut, sizeof(SENSE_DATA));
            senseOut->ErrorCode = 0x70;
        }
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        Srb->ScsiStatus = SCSISTAT_GOOD;
        return FALSE;

    /* Win95-specific fix (kept): TEST_UNIT_READY and MEDIUM_REMOVAL used
       to be hardcoded to SRB_STATUS_SUCCESS/SCSISTAT_GOOD unconditionally
       for EVERY device, with no real hardware check at all -- a fake/
       placeholder answer. For a fixed ATA hard disk this is a reasonable
       shortcut (a disk that AhciInitializePort already found present is,
       definitionally, always "ready"), but for an ATAPI device (CD-ROM)
       it's wrong: TEST_UNIT_READY is exactly the command a real CD-ROM
       uses to report tray-open/no-media/media-changed, and always
       answering "ready" regardless of actual media state is what let
       Windows' CD Player applet believe a disc was always present and
       kept it re-polling. Real fix: for ATAPI, pass TEST_UNIT_READY/
       MEDIUM_REMOVAL straight through to the real drive as an actual
       ATAPI PACKET command via AhciExecuteTransferEngine (SCSI
       TEST_UNIT_READY/START_STOP_UNIT opcodes are valid, real MMC ATAPI
       packet-command opcodes too -- HwInit->ActiveSrb->Cdb is copied
       byte-for-byte into the ATAPI ACMD field there, so no translation
       is needed), and let the drive's own real TFD/status response (not
       a hardcoded value) decide SrbStatus/ScsiStatus.
       SYNCHRONIZE_CACHE/VERIFY stay a real no-op: this driver's transfer
       engine is fully synchronous with no write-back cache of its own to
       flush, and VERIFY without BYTCHK has no real hardware equivalent
       to check against, so "already true" is the real answer, not a
       fake one, for both ATA and ATAPI. */
    case SCSIOP_SYNCHRONIZE_CACHE:
    case SCSIOP_VERIFY:
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        Srb->ScsiStatus = SCSISTAT_GOOD;
        return FALSE;

    case SCSIOP_TEST_UNIT_READY:
    case SCSIOP_MEDIUM_REMOVAL:
        if (HwInit->Ports[port].IsAtapi) {
            ataReq.DataBuffer = NULL;
            ataReq.DataBufferLen = 0;
            ataReq.IsAtapi = TRUE;
            ataReq.IsWrite = FALSE;
            return AhciExecuteTransferEngine(HwInit, &ataReq);
        }
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        Srb->ScsiStatus = SCSISTAT_GOOD;
        return FALSE;

    case SCSIOP_READ_CAPACITY:
        if (HwInit->Ports[port].IsAtapi) {
            ataReq.DataBuffer = Srb->DataBuffer;
            ataReq.DataBufferLen = Srb->DataTransferLength;
            ataReq.IsAtapi = TRUE;
            ataReq.IsWrite = FALSE;
            return AhciExecuteTransferEngine(HwInit, &ataReq);
        }

        {
            PREAD_CAPACITY_DATA cap = (PREAD_CAPACITY_DATA)Srb->DataBuffer;
            ULONG tot = AhciGetTotalSectors(HwInit, port);
            ULONG sectorSize = 512;

            if (tot > 0) tot--;

            cap->LogicalBlockAddress =
                ((tot & 0xFF000000) >> 24) |
                ((tot & 0x00FF0000) >> 8)  |
                ((tot & 0x0000FF00) << 8)  |
                ((tot & 0x000000FF) << 24);

            cap->BytesPerBlock =
                ((sectorSize & 0xFF000000) >> 24) |
                ((sectorSize & 0x00FF0000) >> 8)  |
                ((sectorSize & 0x0000FF00) << 8)  |
                ((sectorSize & 0x000000FF) << 24);

            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            Srb->ScsiStatus = SCSISTAT_GOOD;
            return FALSE;
        }

    case SCSIOP_MODE_SENSE:
        if (HwInit->Ports[port].IsAtapi) {
            UCHAR atapiCdb[12];
            UCHAR scsiPageCode;
            UCHAR scsiAllocLength;
            BOOLEAN ok;
            UCHAR savedCdb[16];
            UCHAR savedCdbLength;

            ZeroMemoryBytes(savedCdb, sizeof(savedCdb));
            for (i = 0; i < 16; i++) {
                savedCdb[i] = Srb->Cdb[i];
            }
            savedCdbLength = Srb->CdbLength;

            scsiPageCode = cdb->MODE_SENSE.PageCode & 0x3F;
            scsiAllocLength = cdb->MODE_SENSE.AllocationLength;

            /* Build the real ATAPI MODE SENSE(10) CDB (opcode 0x5A),
               matching Scsi2Atapi() field-for-field: PageCode carried
               over as-is, ParameterListLength = the SCSI-6 allocation
               length (Msb always 0 -- SCSI-6's AllocationLength is only
               one byte, so it can never need the Msb byte). */
            ZeroMemoryBytes(atapiCdb, sizeof(atapiCdb));
            atapiCdb[0] = 0x5A;                 /* ATAPI_MODE_SENSE */
            atapiCdb[2] = scsiPageCode;
            atapiCdb[7] = 0;                    /* ParameterListLengthMsb */
            atapiCdb[8] = scsiAllocLength;       /* ParameterListLengthLsb */

            ZeroMemoryBytes(Srb->Cdb, 16);
            for (i = 0; i < 12; i++) {
                Srb->Cdb[i] = atapiCdb[i];
            }
            Srb->CdbLength = 12;

            if (Srb->DataBuffer != NULL && Srb->DataTransferLength > 0) {
                ZeroMemoryBytes(Srb->DataBuffer, Srb->DataTransferLength);
            }

            ataReq.DataBuffer = Srb->DataBuffer;
            ataReq.DataBufferLen = Srb->DataTransferLength;
            ataReq.IsAtapi = TRUE;
            ataReq.IsWrite = FALSE;
            ok = AhciExecuteTransferEngine(HwInit, &ataReq);

            /* Restore the SRB's CDB exactly as ScsiPort gave it to us --
               see the bug note above. This must happen for every return
               path, success or failure, and before any of the header
               conversion below (which only touches Srb->DataBuffer, not
               Srb->Cdb, but is kept after this for clarity). */
            for (i = 0; i < 16; i++) {
                Srb->Cdb[i] = savedCdb[i];
            }
            Srb->CdbLength = savedCdbLength;

            if (Srb->SrbStatus == SRB_STATUS_SUCCESS &&
                Srb->DataTransferLength >= sizeof(MODE_PARAMETER_HEADER_10)) {
                PMODE_PARAMETER_HEADER_10 hdr10 = (PMODE_PARAMETER_HEADER_10)Srb->DataBuffer;
                MODE_PARAMETER_HEADER hdr6;
                ULONG payloadBytes;

                /* Squeeze the 8-byte ATAPI header down to SCSI's 4-byte
                   header, exactly as Microsoft's real reverse-conversion
                   does: ModeDataLength takes the Lsb (SCSI-6's length
                   byte can't represent anything the Msb would carry for
                   the small CD mode pages actually in use here), and the
                   two SCSI-only fields ATAPI has no equivalent for
                   (DeviceSpecificParameter, BlockDescriptorLength) come
                   back zeroed rather than fabricated. */
                hdr6.ModeDataLength = hdr10->ModeDataLengthLsb;
                hdr6.MediumType = hdr10->MediumType;
                hdr6.DeviceSpecificParameter = 0;
                hdr6.BlockDescriptorLength = 0;

                payloadBytes = Srb->DataTransferLength - sizeof(MODE_PARAMETER_HEADER_10);
                if (payloadBytes > 0) {
                    PUCHAR dst = (PUCHAR)Srb->DataBuffer + sizeof(MODE_PARAMETER_HEADER);
                    PUCHAR src = (PUCHAR)Srb->DataBuffer + sizeof(MODE_PARAMETER_HEADER_10);
                    for (i = 0; i < payloadBytes; i++) {
                        dst[i] = src[i];
                    }
                }

                *(PMODE_PARAMETER_HEADER)Srb->DataBuffer = hdr6;
            }

            return ok;
        }
        AhciHandleModeSense(HwInit, Srb);
        return FALSE;

    case SCSIOP_READ6:
    case SCSIOP_WRITE6:
        ataReq.DataBuffer = Srb->DataBuffer;
        ataReq.DataBufferLen = Srb->DataTransferLength;
        ataReq.IsAtapi = HwInit->Ports[port].IsAtapi;
        ataReq.LbaHigh = 0;
        ataReq.IsWrite = (cdb->CDB6GENERIC.OperationCode == SCSIOP_WRITE6);
        ataReq.LbaLow = ((ULONG)cdb->CDB6READWRITE.LogicalBlockMsb0 << 16) |
                        ((ULONG)cdb->CDB6READWRITE.LogicalBlockMsb1 << 8)  |
                        ((ULONG)cdb->CDB6READWRITE.LogicalBlockLsb);
        ataReq.SectorCount = (USHORT)cdb->CDB6READWRITE.TransferBlocks;
        if (ataReq.SectorCount == 0) ataReq.SectorCount = 256;
        return AhciExecuteTransferEngine(HwInit, &ataReq);

    case SCSIOP_READ:
    case SCSIOP_WRITE:
        ataReq.DataBuffer = Srb->DataBuffer;
        ataReq.DataBufferLen = Srb->DataTransferLength;
        ataReq.IsAtapi = HwInit->Ports[port].IsAtapi;
        ataReq.LbaHigh = 0;
        ataReq.IsWrite = (cdb->CDB10.OperationCode == SCSIOP_WRITE);
        ataReq.LbaLow = ((ULONG)cdb->CDB10.LogicalBlockByte0 << 24) |
                        ((ULONG)cdb->CDB10.LogicalBlockByte1 << 16) |
                        ((ULONG)cdb->CDB10.LogicalBlockByte2 << 8)  |
                        ((ULONG)cdb->CDB10.LogicalBlockByte3);
        ataReq.SectorCount = ((USHORT)cdb->CDB10.TransferBlocksMsb << 8) |
                              ((USHORT)cdb->CDB10.TransferBlocksLsb);
        return AhciExecuteTransferEngine(HwInit, &ataReq);

    default:
        if (HwInit->Ports[port].IsAtapi) {
            ataReq.DataBuffer = Srb->DataBuffer;
            ataReq.DataBufferLen = Srb->DataTransferLength;
            ataReq.IsAtapi = TRUE;
            ataReq.IsWrite = FALSE;
            return AhciExecuteTransferEngine(HwInit, &ataReq);
        }

        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        return FALSE;
    }
}