

#ifdef __cplusplus
extern "C" {
#endif


#include "ahcint.h"

// Convert an ATA IDENTIFY string field (byte-swapped 16-bit words) into a
// left-justified, space-padded, NUL-terminated ASCII buffer of
// OutBufferMax bytes.
static
VOID
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

    if (OutBuffer == NULL || OutBufferMax == 0) return;

    outIndex = 0;

    // Extract characters safely within strict bounds
    for (i = 0; i < NumWords && (outIndex + 2) <= OutBufferMax; i++) {
        w = IdentifyWords[StartWord + i];
        OutBuffer[outIndex++] = (UCHAR)((w >> 8) & 0xFF);
        OutBuffer[outIndex++] = (UCHAR)(w & 0xFF);
    }

    // Trim trailing spaces and null characters
    while (outIndex > 0 && (OutBuffer[outIndex - 1] == ' ' || OutBuffer[outIndex - 1] == '\0')) {
        outIndex--;
    }
    
    // Fill remaining space with padding safely, leaving room for null termination
    while (outIndex < (OutBufferMax - 1)) {
        OutBuffer[outIndex++] = ' ';
    }

    // Guarantee null termination within the allocated buffer limit
    OutBuffer[OutBufferMax - 1] = '\0';
}


// Return total addressable sector count from IDENTIFY data, preferring
// 48-bit LBA, then 28-bit LBA, then legacy CHS as a fallback. Reads words
// by byte index rather than through a PUSHORT to avoid unaligned-access
// faults on some HALs.
static
ULONGLONG
AhciGetTotalSectors64(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN ULONG Port
)
{
    PUCHAR idData;
    ULONGLONG lba48;
    ULONG lba28;
    ULONG chs;
    USHORT word83, word86, word1, word3, word6;
    USHORT w100, w101, w102, w103, w60, w61;

    if (!HwInit->Ports[Port].IdentifyValid) {
        return 0;
    }

    idData = HwInit->Ports[Port].IdentifyData;

    // Safely extract words using byte indexing to prevent unaligned pointer faults on ACPI HALs
    word83 = (USHORT)idData[83 * 2] | ((USHORT)idData[83 * 2 + 1] << 8);
    word86 = (USHORT)idData[86 * 2] | ((USHORT)idData[86 * 2 + 1] << 8);

    if ((word83 & 0xC400) == 0x4400 || (word83 & (1 << 10)) || (word86 & (1 << 10))) {
        w100 = (USHORT)idData[100 * 2] | ((USHORT)idData[100 * 2 + 1] << 8);
        w101 = (USHORT)idData[101 * 2] | ((USHORT)idData[101 * 2 + 1] << 8);
        w102 = (USHORT)idData[102 * 2] | ((USHORT)idData[102 * 2 + 1] << 8);
        w103 = (USHORT)idData[103 * 2] | ((USHORT)idData[103 * 2 + 1] << 8);

        lba48 = (ULONGLONG)w100 |
                ((ULONGLONG)w101 << 16) |
                ((ULONGLONG)w102 << 32) |
                ((ULONGLONG)w103 << 48);
        if (lba48 > 0) return lba48;
    }

    w60 = (USHORT)idData[60 * 2] | ((USHORT)idData[60 * 2 + 1] << 8);
    w61 = (USHORT)idData[61 * 2] | ((USHORT)idData[61 * 2 + 1] << 8);
    lba28 = (ULONG)w60 | ((ULONG)w61 << 16);
    if (lba28 > 0) return (ULONGLONG)lba28;

    word1 = (USHORT)idData[1 * 2] | ((USHORT)idData[1 * 2 + 1] << 8);
    word3 = (USHORT)idData[3 * 2] | ((USHORT)idData[3 * 2 + 1] << 8);
    word6 = (USHORT)idData[6 * 2] | ((USHORT)idData[6 * 2 + 1] << 8);

    chs = (ULONG)word1 * (ULONG)word3 * (ULONG)word6;
    if (chs > 0) return (ULONGLONG)chs;

    return 0;
}


// Build SCSI INQUIRY response data from cached IDENTIFY data, or a generic
// fallback string set if IDENTIFY never completed successfully. Also
// opportunistically re-checks PxSSTS so a device that showed up after
// enumeration (hot-plug/late-detect) is reported as present.
static
VOID
AhciHandleInquiry(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN PSCSI_REQUEST_BLOCK Srb
)
{
    PINQUIRYDATA inq;
    ULONG port;
    PUCHAR portBase;
    ULONG ssts;
    PUSHORT id;
    BOOLEAN isCd;
    PUCHAR v;
    PUCHAR p;
    ULONG i;

    if (Srb == NULL || Srb->DataBuffer == NULL) {
        if (Srb != NULL) {
            Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
            Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
        }
        return;
    }

    if (Srb->DataTransferLength < sizeof(INQUIRYDATA)) {
        Srb->SrbStatus = SRB_STATUS_DATA_OVERRUN;
        Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
        return;
    }

    inq = (PINQUIRYDATA)Srb->DataBuffer;
    port = Srb->TargetId;

    if (port >= MAX_SUPPORTED_PORTS) {
        Srb->SrbStatus = SRB_STATUS_NO_DEVICE;
        Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
        return;
    }

    portBase = AHCI_PORT_BASE(HwInit->AbarMapped, port);

    // ACPI Safety: Force a synchronized hardware register read barrier for PxSSTS
    ssts = AHCI_READ_REG(portBase, AHCI_PORT_SSTS);
    
    // If the hardware detects a device now (DET == 3, IPM == 1), ensure port is marked present
    if ((ssts & 0x0F) == 0x03) {
        HwInit->Ports[port].Present = TRUE;
    }

    if (!HwInit->Ports[port].Present) {
        Srb->SrbStatus = SRB_STATUS_NO_DEVICE;
        Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
        return;
    }

    id = (PUSHORT)HwInit->Ports[port].IdentifyData;
    isCd = HwInit->Ports[port].IsAtapi;

    ZeroMemoryBytes(inq, Srb->DataTransferLength);

    inq->DeviceType = isCd ? 0x05 : 0x00;
    inq->RemovableMedia = isCd ? 1 : ((id && (id[0] & (1 << 7))) ? 1 : 0);
    inq->ResponseDataFormat = 2;
    inq->AdditionalLength = 31;
    inq->CommandQueue = (id && (id[76] & (1 << 8))) ? 1 : 0;

    if (HwInit->Ports[port].IdentifyValid && id != NULL) {
        AhciExtractAtaString(id, 27, 4, inq->VendorId, 8);
        AhciExtractAtaString(id, 31, 8, inq->ProductId, 16);
        AhciExtractAtaString(id, 23, 2, inq->ProductRevisionLevel, 4);
    } else {
        v = (PUCHAR)(isCd ? "ATAPI   " : "ATA     ");
        p = (PUCHAR)(isCd ? "SATA CD-ROM     " : "SATA HARDDISK   ");
        for (i = 0; i < 8 && i < sizeof(inq->VendorId); i++) inq->VendorId[i] = v[i];
        for (i = 0; i < 16 && i < sizeof(inq->ProductId); i++) inq->ProductId[i] = p[i];
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
static
VOID
AhciHandleRequestSense(
    IN PSCSI_REQUEST_BLOCK Srb
)
{
    PSENSE_DATA sense;

    // Validate buffer pointers and transfer length to prevent ACPI access violations
    if (Srb->DataBuffer == NULL || Srb->DataTransferLength == 0) {
        Srb->SrbStatus = SRB_STATUS_ERROR;
        Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
        return;
    }

    sense = (PSENSE_DATA)Srb->DataBuffer;
    
    // Safely zero memory up to the available transfer length or sense data size
    ZeroMemoryBytes(sense, Srb->DataTransferLength < sizeof(SENSE_DATA) ? Srb->DataTransferLength : sizeof(SENSE_DATA));

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
static
VOID
AhciHandleModeSense(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN PSCSI_REQUEST_BLOCK Srb
)
{
    PUCHAR buf;
    PCDB cdb;
    UCHAR pageControl;
    ULONG port;
    PUCHAR idData;
    ULONGLONG totalSectors;
    ULONG cyls, heads, spt;
    PUCHAR p4;
    USHORT word1, word3, word6;

    if (Srb->DataBuffer == NULL || Srb->DataTransferLength == 0) {
        Srb->SrbStatus = SRB_STATUS_ERROR;
        Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
        return;
    }

    buf = (PUCHAR)Srb->DataBuffer;
    cdb = (PCDB)Srb->Cdb;
    
    if (cdb == NULL) {
        Srb->SrbStatus = SRB_STATUS_ERROR;
        Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
        return;
    }

    pageControl = cdb->MODE_SENSE.PageCode & 0x3F;
    port = Srb->TargetId;
    idData = HwInit->Ports[port].IdentifyData;
    totalSectors = AhciGetTotalSectors64(HwInit, port);
    cyls = 0;
    heads = 0;
    spt = 0;

    ZeroMemoryBytes(buf, Srb->DataTransferLength);

    if (HwInit->Ports[port].IdentifyValid) {
        // Safely extract words using byte indexing to prevent unaligned pointer faults on ACPI HALs
        word1 = (USHORT)idData[1 * 2] | ((USHORT)idData[1 * 2 + 1] << 8);
        word3 = (USHORT)idData[3 * 2] | ((USHORT)idData[3 * 2 + 1] << 8);
        word6 = (USHORT)idData[6 * 2] | ((USHORT)idData[6 * 2 + 1] << 8);

        cyls = (ULONG)word1;
        heads = (ULONG)word3;
        spt = (ULONG)word6;
    }

    if (cyls == 0 || heads == 0 || spt == 0) {
        heads = 255;
        spt = 63;
        cyls = (totalSectors > 0) ? (ULONG)(totalSectors / (heads * spt)) : 1;
        if (cyls == 0) cyls = 1;
    }

    if (Srb->DataTransferLength >= 4) {
        buf[0] = 0; buf[1] = 0; buf[2] = 0; buf[3] = 8;
    }

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
        if (Srb->DataTransferLength >= 1) {
            buf[0] = 11;
        }
    }

    Srb->SrbStatus = SRB_STATUS_SUCCESS;
    Srb->ScsiStatus = SCSISTAT_GOOD;
}


// Walk the SRB's data buffer and fill the port's PRDT, splitting each
// scatter-gather segment at 4 KB physical page boundaries and forcing
// intermediate segment lengths to be even, per the AHCI spec. Fails if
// more than MAX_PRDT_ENTRIES entries would be needed.
static
BOOLEAN
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

    if (Req == NULL || Req->DataBuffer == NULL || PrdtEntriesCount == NULL) {
        return FALSE;
    }

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

        // AHCI specification mandates that PRDT byte counts must be even numbers
        if (bytesLeft > segLen && (segLen & 1) != 0) {
            segLen &= ~1; // Force even length for intermediate segments
        }

        if (segLen == 0) {
            return FALSE;
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


/* Non-polling, asynchronous command execution engine */
// Build and issue a single H2D FIS (ATA/ATAPI, DMA or PIO opcode family
// depending on Req->ForcePio and LBA range) on the port's command slot 0,
// then return immediately — completion is handled later by AhciInterrupt.
static
BOOLEAN
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

    // Flush status, error, and interrupt configuration registers with mandatory ACPI read-backs
    AHCI_WRITE_REG(portBase, AHCI_PORT_IS, 0xFFFFFFFF);
    AHCI_READ_REG(portBase, AHCI_PORT_IS);
    AHCI_WRITE_REG(portBase, AHCI_PORT_SERR, 0xFFFFFFFF);
    AHCI_READ_REG(portBase, AHCI_PORT_SERR);
    AHCI_WRITE_REG(portBase, AHCI_PORT_IE, AHCI_PORT_IE_DEFAULT);
    AHCI_READ_REG(portBase, AHCI_PORT_IE);
    AHCI_WRITE_REG(HwInit->AbarMapped, AHCI_GEN_IS, (1UL << portNumber));
    AHCI_READ_REG(HwInit->AbarMapped, AHCI_GEN_IS);

    /* Kick off the command slot, with write-flushing and memory barrier fencing */
    AHCI_WRITE_REG(portBase, AHCI_PORT_CI, 1);
    AHCI_READ_REG(portBase, AHCI_PORT_CI); // Forces write buffer flush and acts as a hardware barrier

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

    if (Srb == NULL) {
        return FALSE;
    }

    cdb = (PCDB)Srb->Cdb;
    if (cdb == NULL) {
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
        return FALSE;
    }

    port = Srb->TargetId;

    if (port >= MAX_SUPPORTED_PORTS || !HwInit->Ports[port].Present) {
        Srb->SrbStatus = SRB_STATUS_NO_DEVICE;
        Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
        return FALSE;
    }

    if (Srb->Lun != 0) {
        Srb->SrbStatus = SRB_STATUS_NO_DEVICE;
        Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
        return FALSE;
    }

    // Zero-initialize ATA request structure to prevent stack garbage on ACPI
    ZeroMemoryBytes(&ataReq, sizeof(ATA_REQUEST));

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
        if (Srb->DataBuffer == NULL || Srb->DataTransferLength < sizeof(READ_CAPACITY_DATA)) {
            Srb->SrbStatus = SRB_STATUS_DATA_OVERRUN;
            Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
            return FALSE;
        }
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
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
        return FALSE;
    }
}


#ifdef __cplusplus
}
#endif
