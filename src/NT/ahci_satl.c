#include "ahcint.h"

// Convert an ATA IDENTIFY string field (byte-swapped 16-bit words) into a
// left-justified, space-padded ASCII buffer of OutBufferMax bytes.
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

// Return total addressable sector count from IDENTIFY data, preferring
// 48-bit LBA, then 28-bit LBA, then legacy CHS as a fallback.
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

// Build SCSI INQUIRY response data from cached IDENTIFY data, or a generic
// fallback string set if IDENTIFY never completed successfully.
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

        // No valid IDENTIFY data cached — report generic strings instead
        AHCI_DBG_LOG("[AHCINT] AhciHandleInquiry: port %u has no valid IDENTIFY data, using generic strings\n", port);

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

// Build a minimal MODE SENSE(6) response: block descriptor plus, when
// requested, the Rigid Disk Geometry (page 0x04) page derived from
// IDENTIFY data or a synthesized 255/63 geometry.
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

// Walk the SRB's data buffer and fill the port's PRDT, splitting each
// scatter-gather segment at 4 KB physical page boundaries. Fails if more
// than MAX_PRDT_ENTRIES entries would be needed.
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

    if (bytesLeft > 0) {
        AHCI_DBG_LOG("[AHCINT] AhciBuildPrdt: ran out of PRDT entries (%u left)\n", bytesLeft);
        return FALSE;
    }

    *PrdtEntriesCount = entryIndex;
    return TRUE;
}

// Build and issue a single H2D FIS (ATA read/write DMA EXT, or an ATAPI
// PACKET command) on the port, then poll CI until completion or timeout.
// Always returns FALSE (command runs to completion synchronously; no
// asynchronous/interrupt-driven completion is used here).
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
    ULONG spinCount;
    ULONG portIs;
    ULONG waitLimit;
    ULONG tfd;
    ULONG i;
    PUCHAR cdb;
    ULONG cdbLen;

    if (Req->DataBufferLen > 0) {
        if (!AhciBuildPrdt(HwInit, portNumber, Req, &prdtEntries)) {
            AHCI_DBG_LOG("[AHCINT] AhciExecuteTransferEngine: port %u PRDT build failed\n", portNumber);
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

    for (spinCount = 0; spinCount < 5000; spinCount++) {
        if (!(AHCI_READ_REG(portBase, AHCI_PORT_CI) & 1)) break;
    }

    waitLimit = Req->IsAtapi ? 100000 : 25000;
    while ((AHCI_READ_REG(portBase, AHCI_PORT_CI) & 1) && --waitLimit) {
        ScsiPortStallExecution(20);
    }

    portIs = AHCI_READ_REG(portBase, AHCI_PORT_IS);
    tfd = AHCI_READ_REG(portBase, AHCI_PORT_TFD);

    AHCI_WRITE_REG(portBase, AHCI_PORT_IS, portIs);
    AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
    AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_IS, (1UL << portNumber));

    if ((AHCI_READ_REG(portBase, AHCI_PORT_CI) & 1) || (portIs & AHCI_PORT_IS_FATAL) || (tfd & 0x01)) {
        AHCI_DBG_LOG("[AHCINT] AhciExecuteTransferEngine: port %u command failed (IS=0x%x, TFD=0x%x)\n",
                     portNumber, portIs, tfd);

        if (Req->IsAtapi) {
            // Fatal error on an ATAPI command — restart the engines so the
            // next request has a clean port to work with
            AhciStopPortEngines(portBase);
            AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
            AHCI_WRITE_REG(portBase, AHCI_PORT_IS, 0xFFFFFFFF);
            AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, AHCI_READ_REG(portBase, AHCI_PORT_CMD) | AHCI_PORT_CMD_FRE);
            AHCI_WRITE_REG(portBase, AHCI_PORT_CMD, AHCI_READ_REG(portBase, AHCI_PORT_CMD) | AHCI_PORT_CMD_ST);
        }
        if (HwInit->ActiveSrb) {
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

// SCSI-to-ATA translation layer (SATL) entry point: validates the target,
// then dispatches the SRB's CDB opcode to the matching handler.
BOOLEAN
AhciSatlProcessSrb(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN PSCSI_REQUEST_BLOCK Srb
)
{
    PCDB cdb;
    ATA_REQUEST ataReq;
    ULONG port;

    cdb = (PCDB)Srb->Cdb;
    port = Srb->TargetId;

    if (port >= MAX_AHCI_PORTS || !HwInit->Ports[port].Present) {
        AHCI_DBG_LOG("[AHCINT] AhciSatlProcessSrb: target %u not present\n", port);
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

    case SCSIOP_TEST_UNIT_READY:
    case SCSIOP_SYNCHRONIZE_CACHE:
    case SCSIOP_VERIFY:
    case SCSIOP_MEDIUM_REMOVAL:
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
            // Pass any other CDB straight through as an ATAPI PACKET command
            ataReq.DataBuffer = Srb->DataBuffer;
            ataReq.DataBufferLen = Srb->DataTransferLength;
            ataReq.IsAtapi = TRUE;
            ataReq.IsWrite = FALSE;
            return AhciExecuteTransferEngine(HwInit, &ataReq);
        }

        AHCI_DBG_LOG("[AHCINT] AhciSatlProcessSrb: unsupported opcode 0x%x on port %u\n",
                     cdb->CDB6GENERIC.OperationCode, port);
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        return FALSE;
    }
}