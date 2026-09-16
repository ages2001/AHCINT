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
    USHORT w;

    outIndex = 0;

    for (i = 0; i < NumWords && (outIndex + 1) < OutBufferMax; i++) {
        w = IdentifyWords[StartWord + i];
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
static ULONGLONG
AhciGetTotalSectors64(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN ULONG Port
)
{
    PUSHORT id;
    ULONGLONG lba48;
    ULONG lba28;
    ULONG chs;

    id = (PUSHORT)HwInit->Ports[Port].IdentifyData;

    if (HwInit->Ports[Port].IdentifyValid) {
        if ((id[83] & 0xC400) == 0x4400 || (id[83] & (1 << 10)) || (id[86] & (1 << 10))) {
            lba48 = (ULONGLONG)id[100] |
                    ((ULONGLONG)id[101] << 16) |
                    ((ULONGLONG)id[102] << 32) |
                    ((ULONGLONG)id[103] << 48);
            if (lba48 > 0) return lba48;
        }

        lba28 = (ULONG)id[60] | ((ULONG)id[61] << 16);
        if (lba28 > 0) return (ULONGLONG)lba28;

        chs = (ULONG)id[1] * (ULONG)id[3] * (ULONG)id[6];
        if (chs > 0) return (ULONGLONG)chs;
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
    PUCHAR v;
    PUCHAR p;
    ULONG i;

    inq = (PINQUIRYDATA)Srb->DataBuffer;
    port = Srb->TargetId;
    id = (PUSHORT)HwInit->Ports[port].IdentifyData;
    isCd = HwInit->Ports[port].IsAtapi;

    ZeroMemoryBytes(inq, Srb->DataTransferLength);

    inq->DeviceType = isCd ? 0x05 : 0x00;
    inq->RemovableMedia = isCd ? 1 : ((id[0] & (1 << 7)) ? 1 : 0);
    inq->ResponseDataFormat = 2;
    inq->AdditionalLength = 31;
    inq->CommandQueue = (id[76] & (1 << 8)) ? 1 : 0;

    if (HwInit->Ports[port].IdentifyValid) {
        AhciExtractAtaString(id, 27, 4, inq->VendorId, 8);
        AhciExtractAtaString(id, 31, 8, inq->ProductId, 16);
        AhciExtractAtaString(id, 23, 2, inq->ProductRevisionLevel, 4);
    } else {
        // No valid IDENTIFY data cached — report generic strings instead
        AHCI_DBG_LOG("AhciHandleInquiry: port %d has no valid IDENTIFY data, using generic strings", port);
        v = isCd ? "ATAPI   " : "ATA     ";
        p = isCd ? "SATA CD-ROM     " : "SATA HARDDISK   ";
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

// Return a minimal "no sense" REQUEST SENSE response, used after every
// command that already reported its status directly in the SRB.
static VOID
AhciHandleRequestSense(
    IN PSCSI_REQUEST_BLOCK Srb
)
{
    PSENSE_DATA sense;

    sense = (PSENSE_DATA)Srb->DataBuffer;
    ZeroMemoryBytes(sense, Srb->DataTransferLength);

    sense->ErrorCode = 0x70;
    sense->Valid = 1;
    sense->SenseKey = SCSI_SENSE_NO_SENSE;
    sense->AdditionalSenseLength = 10;
    sense->AdditionalSenseCode = 0x00;
    sense->AdditionalSenseCodeQualifier = 0x00;

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
    ULONGLONG totalSectors;
    ULONG cyls, heads, spt;
    PUCHAR p4;

    buf = (PUCHAR)Srb->DataBuffer;
    cdb = (PCDB)Srb->Cdb;
    pageControl = cdb->MODE_SENSE.PageCode & 0x3F;
    port = Srb->TargetId;
    id = (PUSHORT)HwInit->Ports[port].IdentifyData;
    totalSectors = AhciGetTotalSectors64(HwInit, port);
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
        cyls = (totalSectors > 0) ? (ULONG)(totalSectors / (heads * spt)) : 1;
        if (cyls == 0) cyls = 1;
    }

    buf[0] = 0; buf[1] = 0; buf[2] = 0; buf[3] = 8;

    if (Srb->DataTransferLength >= 12) {
        ULONG sectors32 = (totalSectors > 0xFFFFFFFF) ? 0xFFFFFFFF : (ULONG)totalSectors;
        buf[4] = 0;
        buf[5] = (UCHAR)((sectors32 >> 16) & 0xFF);
        buf[6] = (UCHAR)((sectors32 >> 8) & 0xFF);
        buf[7] = (UCHAR)(sectors32 & 0xFF);
        buf[8] = 0; buf[9] = 0; buf[10] = 2; buf[11] = 0;
    }

    if ((pageControl == 0x04 || pageControl == 0x3F) && Srb->DataTransferLength >= 36) {
        p4 = &buf[12];
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
    ULONG bytesLeft;
    PUCHAR virtAddr;
    PAHCI_PRDT_ENTRY prdt;
    ULONG entryIndex;
    ULONG segLen;
    SCSI_PHYSICAL_ADDRESS physAddr;
    ULONG bytesToPageBoundary;

    bytesLeft = Req->DataBufferLen;
    virtAddr = (PUCHAR)Req->DataBuffer;
    prdt = (PAHCI_PRDT_ENTRY)(HwInit->Ports[PortNumber].CommandTable + 0x80);
    entryIndex = 0;

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
        AHCI_DBG_LOG("AhciBuildPrdt: ran out of PRDT entries (%u bytes left)", bytesLeft);
        return FALSE;
    }

    *PrdtEntriesCount = entryIndex;
    return TRUE;
}

/* Non-polling, asynchronous command execution engine */
// Build and issue a single H2D FIS (ATA/ATAPI, DMA or PIO opcode family
// depending on Req->ForcePio and LBA range) on the port's command slot 0,
// then return immediately — completion is handled later by AhciInterrupt.
static BOOLEAN
AhciExecuteTransferEngineAsync(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN PATA_REQUEST Req
)
{
    ULONG portNumber;
    PUCHAR portBase;
    PAHCI_COMMAND_HEADER cmdHeader;
    PFIS_REG_H2D fis;
    PUCHAR acmd;
    ULONG prdtEntries;

    portNumber = HwInit->ActiveSrb ? HwInit->ActiveSrb->TargetId : 0;
    portBase = AHCI_PORT_BASE(HwInit->AbarMapped, portNumber);
    cmdHeader = &HwInit->Ports[portNumber].CommandList[0];
    fis = (PFIS_REG_H2D)HwInit->Ports[portNumber].CommandTable;
    acmd = (PUCHAR)(HwInit->Ports[portNumber].CommandTable + 0x40);
    prdtEntries = 0;

    if (Req->DataBufferLen > 0) {
        if (!AhciBuildPrdt(HwInit, portNumber, Req, &prdtEntries)) {
            AHCI_DBG_LOG("AhciExecuteTransferEngineAsync: port %d PRDT build failed", portNumber);
            if (HwInit->ActiveSrb) HwInit->ActiveSrb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
            return FALSE;
        }
    }

    if (Req->IsAtapi) {
        cmdHeader->Flags = 5 | (1 << 5);
        if (Req->DataBufferLen > 0 && Req->IsWrite) {
            cmdHeader->Flags |= (1 << 6);
        }
        cmdHeader->PrdtLength = (USHORT)prdtEntries;
        cmdHeader->PrdByteCount = 0;
        cmdHeader->CommandTableBase = HwInit->Ports[portNumber].CommandTablePhysical;
        cmdHeader->CommandTableBaseUpper = HwInit->Ports[portNumber].CommandTablePhysicalUpper;

        ZeroMemoryBytes(acmd, 16);
        if (HwInit->ActiveSrb) {
            ULONG i;
            PUCHAR cdb = (PUCHAR)HwInit->ActiveSrb->Cdb;
            ULONG cdbLen = HwInit->ActiveSrb->CdbLength;
            if (cdbLen > 16) cdbLen = 16;
            for (i = 0; i < cdbLen; i++) {
                acmd[i] = cdb[i];
            }
        }

        ZeroMemoryBytes(fis, sizeof(FIS_REG_H2D));
        fis->FisType = 0x27;
        fis->PmPortControl = 0x80;
        fis->Command = IDE_COMMAND_PACKET;
        
        if (Req->DataBufferLen > 0 && !Req->ForcePio) {
            fis->FeaturesLow = 0x01;
            fis->Lba1 = 0;
            fis->Lba2 = 0;
        } else {
            fis->FeaturesLow = 0x00;
            fis->Lba1 = (UCHAR)(Req->DataBufferLen & 0xFF);
            fis->Lba2 = (UCHAR)((Req->DataBufferLen >> 8) & 0xFF);
        }
    } else {
        cmdHeader->Flags = (USHORT)(5 | (Req->IsWrite ? (1 << 6) : 0));
        cmdHeader->PrdtLength = (USHORT)prdtEntries;
        cmdHeader->PrdByteCount = 0;
        cmdHeader->CommandTableBase = HwInit->Ports[portNumber].CommandTablePhysical;
        cmdHeader->CommandTableBaseUpper = HwInit->Ports[portNumber].CommandTablePhysicalUpper;

        ZeroMemoryBytes(fis, sizeof(FIS_REG_H2D));
        fis->FisType = 0x27;
        fis->PmPortControl = 0x80;

        if (Req->ForcePio) {
            if (Req->Lba < 0x10000000 && Req->SectorCount <= 256) {
                fis->Command = Req->IsWrite ? IDE_COMMAND_WRITE_SECTORS : IDE_COMMAND_READ_SECTORS;
                fis->Device = 0x40 | (UCHAR)((Req->Lba >> 24) & 0x0F);
                fis->Lba0 = (UCHAR)(Req->Lba & 0xFF);
                fis->Lba1 = (UCHAR)((Req->Lba >> 8) & 0xFF);
                fis->Lba2 = (UCHAR)((Req->Lba >> 16) & 0xFF);
                fis->SectorCountLow = (UCHAR)(Req->SectorCount == 256 ? 0 : Req->SectorCount);
            } else {
                fis->Command = Req->IsWrite ? IDE_COMMAND_WRITE_SECTORS_EXT : IDE_COMMAND_READ_SECTORS_EXT;
                fis->Device = 0x40;
                fis->Lba0 = (UCHAR)(Req->Lba & 0xFF);
                fis->Lba1 = (UCHAR)((Req->Lba >> 8) & 0xFF);
                fis->Lba2 = (UCHAR)((Req->Lba >> 16) & 0xFF);
                fis->Lba3 = (UCHAR)((Req->Lba >> 24) & 0xFF);
                fis->Lba4 = (UCHAR)((Req->Lba >> 32) & 0xFF);
                fis->Lba5 = (UCHAR)((Req->Lba >> 40) & 0xFF);
                fis->SectorCountLow = (UCHAR)(Req->SectorCount & 0xFF);
                fis->SectorCountHigh = (UCHAR)((Req->SectorCount >> 8) & 0xFF);
            }
        } else {
            if (Req->Lba < 0x10000000 && Req->SectorCount <= 256) {
                fis->Command = Req->IsWrite ? IDE_COMMAND_WRITE_DMA : IDE_COMMAND_READ_DMA;
                fis->Device = 0x40 | (UCHAR)((Req->Lba >> 24) & 0x0F);
                fis->Lba0 = (UCHAR)(Req->Lba & 0xFF);
                fis->Lba1 = (UCHAR)((Req->Lba >> 8) & 0xFF);
                fis->Lba2 = (UCHAR)((Req->Lba >> 16) & 0xFF);
                fis->SectorCountLow = (UCHAR)(Req->SectorCount == 256 ? 0 : Req->SectorCount);
            } else {
                fis->Command = Req->IsWrite ? IDE_COMMAND_WRITE_DMA_EXT : IDE_COMMAND_READ_DMA_EXT;
                fis->Device = 0x40;
                fis->Lba0 = (UCHAR)(Req->Lba & 0xFF);
                fis->Lba1 = (UCHAR)((Req->Lba >> 8) & 0xFF);
                fis->Lba2 = (UCHAR)((Req->Lba >> 16) & 0xFF);
                fis->Lba3 = (UCHAR)((Req->Lba >> 24) & 0xFF);
                fis->Lba4 = (UCHAR)((Req->Lba >> 32) & 0xFF);
                fis->Lba5 = (UCHAR)((Req->Lba >> 40) & 0xFF);
                fis->SectorCountLow = (UCHAR)(Req->SectorCount & 0xFF);
                fis->SectorCountHigh = (UCHAR)((Req->SectorCount >> 8) & 0xFF);
            }
        }
    }

    AHCI_WRITE_REG(portBase, AHCI_PORT_IS, 0xFFFFFFFF);
    AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
    AHCI_WRITE_REG(portBase, AHCI_PORT_IE, AHCI_PORT_IE_DEFAULT);
    AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_IS, (1UL << portNumber));

    /* Kick off the command slot */
    AHCI_WRITE_REG(portBase, AHCI_PORT_CI, 1);

    /* Return immediately without waiting; completion arrives via interrupt */
    return TRUE;
}

// SCSI-to-ATA translation layer (SATL) entry point: validates the target,
// then dispatches the SRB's CDB opcode to the matching handler. ATAPI
// devices get their own dispatch branch (pass-through PACKET for anything
// not handled directly); ATA devices are dispatched by SCSI opcode,
// including READ16/WRITE16 (0x88/0x8A) for full 64-bit LBA support.
BOOLEAN
AhciSatlProcessSrb(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN PSCSI_REQUEST_BLOCK Srb
)
{
    PCDB cdb;
    ATA_REQUEST ataReq;
    ULONG port;
    PREAD_CAPACITY_DATA cap;
    ULONGLONG tot;
    ULONG tot32;
    ULONG sectorSize;

    cdb = (PCDB)Srb->Cdb;
    port = Srb->TargetId;

    if (port >= MAX_SUPPORTED_PORTS || !HwInit->Ports[port].Present) {
        AHCI_DBG_LOG("AhciSatlProcessSrb: target %d not present", port);
        Srb->SrbStatus = SRB_STATUS_NO_DEVICE;
        return FALSE;
    }

    if (Srb->Lun != 0) {
        Srb->SrbStatus = SRB_STATUS_NO_DEVICE;
        return FALSE;
    }

    if (HwInit->Ports[port].IsAtapi) {
        if (cdb->CDB6GENERIC.OperationCode == SCSIOP_INQUIRY) {
            AhciHandleInquiry(HwInit, Srb);
            return FALSE;
        }

        if (cdb->CDB6GENERIC.OperationCode == SCSIOP_REQUEST_SENSE) {
            AhciHandleRequestSense(Srb);
            return FALSE;
        }

        if (cdb->CDB6GENERIC.OperationCode == SCSIOP_TEST_UNIT_READY ||
            cdb->CDB6GENERIC.OperationCode == SCSIOP_MEDIUM_REMOVAL ||
            cdb->CDB6GENERIC.OperationCode == 0x1B ||
            cdb->CDB6GENERIC.OperationCode == 0x46 ||
            cdb->CDB6GENERIC.OperationCode == 0x4A ||
            cdb->CDB6GENERIC.OperationCode == 0xA4 ||
            cdb->CDB6GENERIC.OperationCode == SCSIOP_SYNCHRONIZE_CACHE) 
        {
            Srb->SrbStatus = SRB_STATUS_SUCCESS;
            Srb->ScsiStatus = SCSISTAT_GOOD;
            return FALSE;
        }

        if (cdb->CDB6GENERIC.OperationCode == SCSIOP_MODE_SENSE ||
            cdb->CDB6GENERIC.OperationCode == 0x5A) 
        {
            AhciHandleModeSense(HwInit, Srb);
            return FALSE;
        }

        ataReq.DataBuffer = Srb->DataBuffer;
        ataReq.DataBufferLen = Srb->DataTransferLength;
        ataReq.IsAtapi = TRUE;
        ataReq.IsWrite = (Srb->SrbFlags & SRB_FLAGS_DATA_OUT) ? TRUE : FALSE;
        ataReq.ForcePio = FALSE;
        ataReq.Lba = 0;
        ataReq.SectorCount = 0;
        return AhciExecuteTransferEngineAsync(HwInit, &ataReq);
    }

    switch (cdb->CDB6GENERIC.OperationCode) {
    case SCSIOP_INQUIRY:
        AhciHandleInquiry(HwInit, Srb);
        return FALSE;

    case SCSIOP_REQUEST_SENSE:
        AhciHandleRequestSense(Srb);
        return FALSE;

    case SCSIOP_TEST_UNIT_READY:
    case SCSIOP_SYNCHRONIZE_CACHE:
    case SCSIOP_VERIFY:
    case SCSIOP_MEDIUM_REMOVAL:
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        Srb->ScsiStatus = SCSISTAT_GOOD;
        return FALSE;

    case SCSIOP_READ_CAPACITY:
        cap = (PREAD_CAPACITY_DATA)Srb->DataBuffer;
        tot = AhciGetTotalSectors64(HwInit, port);
        sectorSize = 512;

        if (tot > 0) tot--;
        tot32 = (tot > 0xFFFFFFFF) ? 0xFFFFFFFF : (ULONG)tot;

        cap->LogicalBlockAddress = 
            ((tot32 & 0xFF000000) >> 24) |
            ((tot32 & 0x00FF0000) >> 8)  |
            ((tot32 & 0x0000FF00) << 8)  |
            ((tot32 & 0x000000FF) << 24);

        cap->BytesPerBlock = 
            ((sectorSize & 0xFF000000) >> 24) |
            ((sectorSize & 0x00FF0000) >> 8)  |
            ((sectorSize & 0x0000FF00) << 8)  |
            ((sectorSize & 0x000000FF) << 24);

        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        Srb->ScsiStatus = SCSISTAT_GOOD;
        return FALSE;

    case SCSIOP_MODE_SENSE:
    case 0x5A:
        AhciHandleModeSense(HwInit, Srb);
        return FALSE;

    case SCSIOP_READ6:
    case SCSIOP_WRITE6:
        ataReq.DataBuffer = Srb->DataBuffer;
        ataReq.DataBufferLen = Srb->DataTransferLength;
        ataReq.IsAtapi = FALSE;
        ataReq.ForcePio = FALSE;
        ataReq.IsWrite = (cdb->CDB6GENERIC.OperationCode == SCSIOP_WRITE6);
        ataReq.Lba = ((ULONG)cdb->CDB6READWRITE.LogicalBlockMsb0 << 16) |
                     ((ULONG)cdb->CDB6READWRITE.LogicalBlockMsb1 << 8)  |
                     ((ULONG)cdb->CDB6READWRITE.LogicalBlockLsb);
        ataReq.SectorCount = (USHORT)cdb->CDB6READWRITE.TransferBlocks;
        if (ataReq.SectorCount == 0) ataReq.SectorCount = 256;
        return AhciExecuteTransferEngineAsync(HwInit, &ataReq);

    case SCSIOP_READ:
    case SCSIOP_WRITE:
        ataReq.DataBuffer = Srb->DataBuffer;
        ataReq.DataBufferLen = Srb->DataTransferLength;
        ataReq.IsAtapi = FALSE;
        ataReq.ForcePio = FALSE;
        ataReq.IsWrite = (cdb->CDB10.OperationCode == SCSIOP_WRITE);
        ataReq.Lba = ((ULONG)cdb->CDB10.LogicalBlockByte0 << 24) |
                     ((ULONG)cdb->CDB10.LogicalBlockByte1 << 16) |
                     ((ULONG)cdb->CDB10.LogicalBlockByte2 << 8)  |
                     ((ULONG)cdb->CDB10.LogicalBlockByte3);
        ataReq.SectorCount = ((USHORT)cdb->CDB10.TransferBlocksMsb << 8) |
                              ((USHORT)cdb->CDB10.TransferBlocksLsb);
        return AhciExecuteTransferEngineAsync(HwInit, &ataReq);

    case 0x88: /* SCSIOP_READ16 */
    case 0x8A: /* SCSIOP_WRITE16 */
        ataReq.DataBuffer = Srb->DataBuffer;
        ataReq.DataBufferLen = Srb->DataTransferLength;
        ataReq.IsAtapi = FALSE;
        ataReq.ForcePio = FALSE;
        ataReq.IsWrite = (cdb->CDB6GENERIC.OperationCode == 0x8A);
        ataReq.Lba = ((ULONGLONG)cdb->AsByte[2] << 56) |
                     ((ULONGLONG)cdb->AsByte[3] << 48) |
                     ((ULONGLONG)cdb->AsByte[4] << 40) |
                     ((ULONGLONG)cdb->AsByte[5] << 32) |
                     ((ULONGLONG)cdb->AsByte[6] << 24) |
                     ((ULONGLONG)cdb->AsByte[7] << 16) |
                     ((ULONGLONG)cdb->AsByte[8] << 8)  |
                     ((ULONGLONG)cdb->AsByte[9]);
        ataReq.SectorCount = ((USHORT)cdb->AsByte[12] << 8) | (USHORT)cdb->AsByte[13];
        return AhciExecuteTransferEngineAsync(HwInit, &ataReq);

    default:
        AHCI_DBG_LOG("AhciSatlProcessSrb: unsupported opcode 0x%02X on port %d", cdb->CDB6GENERIC.OperationCode, port);
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        return FALSE;
    }
}